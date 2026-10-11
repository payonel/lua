/*
** $Id: ltable.c $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/


/*
** Implementation of tables (aka arrays, objects, or hash tables).
** Tables keep its elements in two parts: an array part and a hash part.
** Non-negative integer keys are all candidates to be kept in the array
** part. The actual size of the array is the largest 'n' such that
** more than half the slots between 1 and n are in use.
** Hash uses a mix of chained scatter table with Brent's variation.
** A main invariant of these tables is that, if an element is not
** in its main position (i.e. the 'original' position that its hash gives
** to it), then the colliding element is in its own main position.
** Hence even when the load factor reaches 100%, performance remains good.
*/

#include <limits.h>
#include <math.h>
#include <string.h>

#include <limits>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"
#include "literals.h"

namespace lua::object
{

/*
** Only hash parts with at least 2^LIMFORLAST have a 'lastfree' field
** that optimizes finding a free slot. That field is stored just before
** the array of nodes, in the same block. Smaller tables do a complete
** search when looking for a free slot.
*/
constexpr int LIMFORLAST = 3; /* log2 of real limit (8) */

inline int Table::haslastfree() const
{
  return sizenode(this) >= twoto(LIMFORLAST);
}

int Table::asize() const
{
  return (int)array.size();
}

/*
** MAXABITS is the largest integer such that 2^MAXABITS fits in an
** unsigned int.
*/
constexpr int MAXABITS = 30;

/*
** MAXASIZEB is the maximum number of elements in the array part such
** that the size of the array fits in 'size_t'.
*/
constexpr size_t MAXASIZEB = MAX_SIZET / (cost::Value + cost::Byte);

/*
** MAXASIZE is the maximum size of the array part. It is the minimum
** between 2^MAXABITS and MAXASIZEB.
*/
constexpr int MAXASIZE = 1 << MAXABITS;
static_assert(MAXASIZE <= MAXASIZEB);

/*
** MAXHBITS is the largest integer such that 2^MAXHBITS fits in a
** signed int.
*/
constexpr int MAXHBITS = 30;

/*
** MAXHSIZE is the maximum size of the hash part. It is the minimum
** between 2^MAXHBITS and the maximum size such that, measured in bytes,
** it fits in a 'size_t'.
*/
constexpr int MAXHSIZE = (((1 << MAXHBITS) <= MAX_SIZET / cost::Node) ? (1 << MAXHBITS) : (MAX_SIZET / cost::Node));

/*
** When the original hash value is good, hashing by a power of 2
** avoids the cost of '%'.
*/
inline int Table::hashpow2(lua_Unsigned n) const
{
  return lmod(n, sizenode(this));
}

/*
** for other types, it is better to avoid modulo by power of 2, as
** they can have many 2 factors.
*/
inline int Table::hashmod(lua_Unsigned n) const
{
  return (int)l_umod(n, (sizenode(this) - 1) | 1);
}

inline int Table::hashstr(TString* str) const
{
  return hashpow2(luaS_gethash(str));
}
inline int Table::hashboolean(int p) const
{
  return hashpow2(p);
}

inline int Table::hashpointer(const void* p) const
{
  return hashmod(point2uint(p));
}

inline int Table::hashpointer(lua_CFunction p) const
{
  return hashmod(point2uint(p));
}


static const Node dummynode_ = {
    {{0, nullptr}, LUA_VEMPTY},
    LUA_TDEADKEY,
    0,
    {0, nullptr}
};

static const TValue absentkey = {{0, nullptr}, LUA_VABSTKEY};

/*
** Hash for integers. To allow a good hash, use the remainder operator
** ('%'). If integer fits as a non-negative int, compute an int
** remainder, which is faster. Otherwise, use an unsigned-integer
** remainder, which uses all bits and ensures a non-negative result.
*/
int Table::hashint(lua_Integer i) const
{
  const Table* t = this;
  lua_Unsigned ui = i;
  if (ui <= INT_MAX)
    return (int)ui % ((sizenode(t) - 1) | 1);
  else
    return t->hashmod(ui);
}

/*
** Hash for floating-point numbers.
** The main computation should be just
**     n = frexp(n, &i); return (n * INT_MAX) + i
** but there are some numerical subtleties.
** In a two-complement representation, INT_MAX may not have an exact
** representation as a float, but INT_MIN does; because the absolute
** value of 'frexp' is smaller than 1 (unless 'n' is inf/NaN), the
** absolute value of the product 'frexp * -INT_MIN' is smaller or equal
** to INT_MAX. Next, the use of 'unsigned int' avoids overflows when
** adding 'i'; the use of '~u' (instead of '-u') avoids problems with
** INT_MIN.
*/
static unsigned l_hashfloat(lua_Number n)
{
  int i;
  IntResult ni;
  n = frexp(n, &i) * -(lua_Number)INT_MIN;
  if (!lua_numbertointeger(n, &ni))
  { /* is 'n' inf/-inf/NaN? */
    assert((!(n == n)) || fabs(n) == (lua_Number)HUGE_VAL);
    return 0;
  }
  else
  { /* normal case */
    unsigned int u = (unsigned int)i + (unsigned int)ni.v;
    return (u <= INT_MAX ? u : ~u);
  }
}

/*
** returns the 'main' position of an element in a table (that is,
** the index of its hash value).
*/
int Table::mainpositionTV(const TValue* key) const
{
  const Table* t = this;
  switch (ttypetag(key))
  {
  case LUA_VNUMINT:
  {
    lua_Integer i = ivalue(key);
    return t->hashint(i);
  }
  case LUA_VNUMFLT:
  {
    lua_Number n = fltvalue(key);
    return t->hashmod(l_hashfloat(n));
  }
  case LUA_VSHRSTR:
  {
    assert(ttisstring(key));
    TString* ts = tsvalue(key);
    return t->hashstr(ts);
  }
  case LUA_VFALSE:
    return t->hashboolean(0);
  case LUA_VTRUE:
    return t->hashboolean(1);
  case LUA_VLIGHTUSERDATA:
  {
    void* p = pvalue(key);
    return t->hashpointer(p);
  }
  case LUA_VLCF:
  { /* an engine function by its name (the same in every process); another by address */
    const GCObject* o = gcvalue(key);
    return (o->hash != 0) ? t->hashmod(o->hash) : t->hashpointer(o);
  }
  default:
  {
    /* an object hashes by the hash it was given when created, not by its
       address: the same program with the same seed puts it in the same
       slot, and a copy of an object can take its hash */
    assert(iscollectable(key));
    GCObject* o = gcvalue(key);
    return t->hashmod(o->hash);
  }
  }
}

inline int Table::mainpositionfromnode(int nd) const
{
  TValue key;
  getnodekey(nullptr, &key, gnode(this, nd));
  return mainpositionTV(&key);
}

/*
** Check whether key 'k1' is equal to the key in node 'n2'. This
** equality is raw, so there are no metamethods. Floats with integer
** values have been normalized, so integers cannot be equal to
** floats. It is assumed that 'eqshrstr' is simply pointer equality,
** so that short strings are handled in the default case.  The flag
** 'deadok' means to accept dead keys as equal to their original values.
** (Only collectable objects can produce dead keys.) Note that dead
** long strings are also compared by identity.  Once a key is dead,
** its corresponding value may be collected, and then another value
** can be created with the same address. If this other value is given
** to 'next', 'equalkey' will signal a false positive. In a regular
** traversal, this situation should never happen, as all keys given to
** 'next' came from the table itself, and therefore could not have been
** collected. Outside a regular traversal, we have garbage in, garbage
** out. What is relevant is that this false positive does not break
** anything.  (In particular, 'next' will return some other valid item
** on the table or nil.)
*/
static int equalkey(const TValue* k1, const Node* n2, int deadok)
{
  if (k1->tt_ != n2->key_tt)
  { /* not the same variants? */
    if (keyisshrstr(n2) && ttisstring(k1))
    {
      /* an external string can be equal to a short-string key */
      return luaS_eqstr(tsvalue(k1),
                        keystrval(n2));
    }
    else if (deadok && keyisdead(n2) && iscollectable(k1))
    {
      /* a collectable value can be equal to a dead key */
      assert(iscollectable(k1));
      return gcvalue(k1) == gckey(n2);
    }
    else
      return 0; /* otherwise, different variants cannot be equal */
  }
  else
  { /* equal variants */
    switch (n2->key_tt)
    {
    case LUA_VNIL:
    case LUA_VFALSE:
    case LUA_VTRUE:
      return 1;
    case LUA_VNUMINT:
      assert(ttisinteger(k1));
      return (ivalue(k1) == keyival(n2));
    case LUA_VNUMFLT:
      assert(ttisfloat(k1));
      return (fltvalue(k1) == keyfltval(n2));
    case LUA_VLIGHTUSERDATA:
      assert(ttislightuserdata(k1));
      return pvalue(k1) == keypval(n2);
    case ctb(LUA_VLNGSTR):
    {
      return luaS_eqstr(tsvalue(k1),
                        keystrval(n2));
    }
    default:
      assert(iscollectable(k1));
      return gcvalue(k1) == gckey(n2);
    }
  }
}

int Table::nodegeneric(const TValue* key, int deadok)
{
  Table* t = this;
  int i = t->mainpositionTV(key);
  for (;;)
  { /* check whether 'key' is somewhere in the chain */
    Node* n = gnode(t, i);
    if (equalkey(key, n, deadok))
      return i; /* that's it */
    else
    {
      int nx = n->next;
      if (nx == 0)
        return -1; /* not found */
      i += nx;
    }
  }
}

const TValue* Table::getgeneric(const TValue* key, int deadok)
{
  Table* t = this;
  int idx = t->nodegeneric(key, deadok);
  return (idx < 0) ? &absentkey : &gnode(t, idx)->val;
}

/*
** Return the index 'k' (converted to an unsigned) if it is inside
** the range [1, limit].
*/
static int checkrange(lua_Integer k, int limit)
{
  return (k >= 1 && k <= limit) ? (int)k : 0;
}

/*
** Return the index 'k' if 'k' is an appropriate key to live in the
** array part of a table, 0 otherwise.
*/
static inline int arrayindex(lua_Integer k)
{
  return checkrange(k, MAXASIZE);
}

/*
** Check whether an integer key is in the array part of a table and
** return its index there, or zero.
*/
inline int Table::ikeyinarray(lua_Integer k) const
{
  return checkrange(k, asize());
}

/*
** Check whether a key is in the array part of a table and return its
** index there, or zero.
*/
int Table::keyinarray(const TValue* key)
{
  return ttisinteger(key) ? ikeyinarray(ivalue(key)) : 0;
}

/*
** returns the index of a 'key' for table traversals. First goes all
** elements in the array part, then elements in the hash part. The
** beginning of a traversal is signaled by 0.
*/
lua_Integer Table::findindex(lua_State* L, TValue* key, int asize)
{
  Table* t = this;
  int i;
  if (isempty(key))
    return 0; /* first iteration */
  i = t->keyinarray(key);
  if (i != 0) /* is 'key' inside array part? */
    return i; /* yes; that's the index */
  else
  {
    int idx = t->nodegeneric(key, 1);
    if (l_unlikely(idx < 0))
      luaG_runerror(L, lt::invalid_key_to_next); /* key not found */
    i = idx; /* key index in hash table */
    /* hash elements are numbered after array ones */
    return ((lua_Integer)i + 1) + asize;
  }
}

int Table::luaH_next(lua_State* L, StkId key)
{
  Table* t = this;
  int asize = t->asize();
  TValue keyv = getslot(L, key);
  lua_Integer i = t->findindex(L, &keyv, asize); /* find original key */
  for (; i < asize; i++)
  { /* try first array part */
    lu_byte tag = getArrTag(t, i);
    if (!tagisempty(tag))
    { /* a non-empty entry? */
      setsloti(L, key, i + 1);
      TValue res;
      farr2val(t, i, tag, &res);
      setslot(L, key + 1, &res);
      return 1;
    }
  }
  for (i -= asize; i < sizenode(t); i++)
  { /* hash part */
    if (!isempty(&gnode(t, i)->val))
    { /* a non-empty entry? */
      Node* n = gnode(t, i);
      TValue nk;
      getnodekey(L, &nk, n);
      setslot(L, key, &nk);
      setslot(L, key + 1, &n->val);
      return 1;
    }
  }
  return 0; /* no more elements */
}

/* 'node' size in bytes */
l_mem Table::sizehash()
{
  static_assert(std::numeric_limits<decltype(sizehash())>::max() / cost::Node >= MAXHSIZE);
  Table* t = this;
  return l_mem{sizenode(t)} * cost::Node;
}

void Table::freehash(lua_State* L)
{
  Table* t = this;
  if (!isdummy(t))
  {
    luaM_alloc_Node_span(L->l_G, L, t->node, 0, false);
  }
}

/*
** {=============================================================
** Rehash
** ==============================================================
*/

/*
** Structure to count the keys in a table.
** 'total' is the total number of keys in the table.
** 'na' is the number of *array indices* in the table (see 'arrayindex').
** 'deleted' is true if there are deleted nodes in the hash part.
** 'nums' is a "count array" where 'nums[i]' is the number of integer
** keys between 2^(i - 1) + 1 and 2^i. Note that 'na' is the summation
** of 'nums'.
*/
struct Counters
{
  int total;
  int na;
  int deleted;
  int nums[MAXABITS + 1];
};

/*
** Check whether it is worth to use 'na' array entries instead of 'nh'
** hash nodes. (A hash node uses ~3 times more memory than an array
** entry: Two values plus 'next' versus one value.) Evaluate with size_t
** to avoid overflows.
*/
static inline int arrayXhash(size_t na, size_t nh)
{
  return na <= nh * 3;
}

/*
** Compute the optimal size for the array part of table 't'.
** This size maximizes the number of elements going to the array part
** while satisfying the condition 'arrayXhash' with the use of memory if
** all those elements went to the hash part.
** 'ct->na' enters with the total number of array indices in the table
** and leaves with the number of keys that will go to the array part;
** return the optimal size for the array part.
*/
static int computesizes(Counters* ct)
{
  int i;
  int twotoi;      /* 2^i (candidate for optimal size) */
  int a = 0;       /* number of elements smaller than 2^i */
  int na = 0;      /* number of elements to go to array part */
  int optimal = 0; /* optimal size for array part */
  /* traverse slices while 'twotoi' does not overflow and total of array
     indices still can satisfy 'arrayXhash' against the array size */
  for (i = 0, twotoi = 1;
       i <= MAXABITS && arrayXhash(twotoi, ct->na);
       i++, twotoi = (i <= MAXABITS) ? twotoi * 2 : twotoi)
  {
    int nums = ct->nums[i];
    a += nums;
    if (nums > 0 && /* grows array only if it gets more elements... */
        arrayXhash(twotoi, a))
    {                   /* ...while using "less memory" */
      optimal = twotoi; /* optimal size (till now) */
      na = a;           /* all elements up to 'optimal' will go to array part */
    }
  }
  ct->na = na;
  return optimal;
}

static void countint(lua_Integer key, Counters* ct)
{
  int k = arrayindex(key);
  if (k != 0)
  {                               /* is 'key' an array index? */
    ct->nums[luaO_ceillog2(k)]++; /* count as such */
    ct->na++;
  }
}

inline int Table::arraykeyisempty(int key) const
{
  int tag = getArrTag(this, key - 1);
  return tagisempty(tag);
}

/*
** Count keys in array part of table 't'.
*/
void Table::numusearray(Counters* ct) const
{
  const Table* t = this;
  int lg;
  int ttlg;     /* 2^lg */
  int ause = 0; /* summation of 'nums' */
  int i = 1;    /* index to traverse all array keys */
  int asize = t->asize();
  /* traverse each slice */
  for (lg = 0, ttlg = 1; lg <= MAXABITS; lg++, ttlg = (lg <= MAXABITS) ? ttlg * 2 : ttlg)
  {
    int lc = 0; /* counter */
    int lim = ttlg;
    if (lim > asize)
    {
      lim = asize; /* adjust upper limit */
      if (i > lim)
        break; /* no more elements to count */
    }
    /* count elements in range (2^(lg - 1), 2^lg] */
    for (; i <= lim; i++)
    {
      if (!t->arraykeyisempty(i))
        lc++;
    }
    ct->nums[lg] += lc;
    ause += lc;
  }
  ct->total += ause;
  ct->na += ause;
}

/*
** Count keys in hash part of table 't'. As this only happens during
** a rehash, all nodes have been used. A node can have a nil value only
** if it was deleted after being created.
*/
void Table::numusehash(Counters* ct) const
{
  const Table* t = this;
  int i = sizenode(t);
  int total = 0;
  while (i--)
  {
    Node* n = gnode(t, i);
    if (isempty(&n->val))
    {
      assert(!keyisnil(n)); /* entry was deleted; key cannot be nil */
      ct->deleted = 1;
    }
    else
    {
      total++;
      if (keyisinteger(n))
        countint(keyival(n), ct);
    }
  }
  ct->total += total;
}

/*
** Convert an "abstract size" (number of slots in an array) to
** "concrete size" (number of bytes in the array).
*/
/*
** Resize the array part of a table. If new size is equal to the old,
** do nothing. Else, if new size is zero, free the old arrays. Otherwise,
** allocate the new value and tag arrays, move the common elements to
** their new position, and then free the old arrays.
*/
std::span<Value> Table::resizearray(lua_State* L,
                          int oldasize,
                          int newasize,
                          std::span<lu_byte>* newtags)
{
  Table* t = this;
  if (oldasize == newasize)
  {
    *newtags = t->tags; /* nothing to be done */
    return t->array;
  }
  else if (newasize == 0)
  { /* erasing array? */
    luaM_alloc_Value_span(L->l_G, L, t->array, 0, false);
    luaM_alloc_lu_byte_span(L->l_G, L, t->tags, 0, false);
    *newtags = {};
    return {};
  }
  else
  {
    std::span<Value> nv = luaM_alloc_Value_span(L->l_G, L, {}, newasize, false);
    if (nv.data() == nullptr) /* allocation error? */
    {
      *newtags = {};
      return {};
    }
    std::span<lu_byte> nt = luaM_alloc_lu_byte_span(L->l_G, L, {}, newasize, false);
    if (nt.data() == nullptr) /* allocation error? */
    {
      luaM_alloc_Value_span(L->l_G, L, nv, 0, false);
      *newtags = {};
      return {};
    }
    if (oldasize > 0)
    {
      /* move common elements to new arrays */
      int tomove = (oldasize < newasize) ? oldasize : newasize;
      for (int mi = 0; mi < tomove; mi++)
        nv[mi] = t->array[mi];
      for (int mi = 0; mi < tomove; mi++)
        nt[mi] = t->tags[mi];
      luaM_alloc_Value_span(L->l_G, L, t->array, 0, false);
      luaM_alloc_lu_byte_span(L->l_G, L, t->tags, 0, false);
    }
    *newtags = nt;
    return nv;
  }
}

/*
** Creates an array for the hash part of a table with the given
** size, or reuses the dummy node if size is zero.
** The computation for size overflow is in two steps: the first
** comparison ensures that the shift in the second one does not
** overflow.
*/
void Table::setnodevector(lua_State* L, int size)
{
  Table* t = this;
  if (size == 0)
  {                                   /* no elements to hash part? */
    t->node = std::span<Node>((Node*)(&dummynode_), 1); /* use common 'dummynode' */
    t->lastfree = 0;
    setdummy(t); /* signal that it is using dummy node */
  }
  else
  {
    int i;
    int lsize = luaO_ceillog2(size);
    if (lsize > MAXHBITS || (1 << lsize) > MAXHSIZE)
      luaG_runerror(L, lt::table_overflow);
    size = twoto(lsize);
    t->node = luaM_alloc_Node_span(L->l_G, L, {}, size, true);
    t->lastfree = 0;
    if (lsize >= LIMFORLAST)
    {
      t->lastfree = size; /* all positions are free */
    }
    setnodummy(t);
    for (i = 0; i < size; i++)
    {
      Node* n = gnode(t, i);
      n->next = 0;
      setnilkey(n);
      setempty(&n->val);
    }
  }
}

/*
** (Re)insert all elements from the hash part of 'ot' into table 't'.
*/
void reinserthash(lua_State* L, Table* ot, Table* t)
{
  int j;
  int size = sizenode(ot);
  for (j = 0; j < size; j++)
  {
    Node* old = gnode(ot, j);
    if (!isempty(&old->val))
    {
      /* doesn't need barrier/invalidate cache, as entry was
         already present in the table */
      TValue k;
      getnodekey(L, &k, old);
      t->newcheckedkey(&k, &old->val);
    }
  }
}

/*
** Exchange the hash part of 't1' and 't2'. (In 'flags', only the dummy
** bit must be exchanged:  The metamethod bits do not change during a
** resize, so the "real" table can keep their values.)
*/
static void exchangehashpart(Table* t1, Table* t2)
{
  std::span<Node> node = t1->node;
  int lastfree = t1->lastfree;
  int bitdummy1 = t1->flags & BITDUMMY;
  t1->node = t2->node;
  t1->lastfree = t2->lastfree;
  t1->flags = (t1->flags & NOTBITDUMMY) | (t2->flags & BITDUMMY);
  t2->node = node;
  t2->lastfree = lastfree;
  t2->flags = (lu_byte)((t2->flags & NOTBITDUMMY) | bitdummy1);
}

/*
** Re-insert into the new hash part of a table the elements from the
** vanishing slice of the array part.
*/
void Table::reinsertOldSlice(int oldasize,
                             int newasize)
{
  Table* t = this;
  int i;
  for (i = newasize; i < oldasize; i++)
  { /* traverse vanishing slice */
    lu_byte tag = getArrTag(t, i);
    if (!tagisempty(tag))
    { /* a non-empty entry? */
      TValue key, aux;
      setivalue(&key, (lua_Integer)i + 1); /* make the key */
      farr2val(t, i, tag, &aux);         /* copy value into 'aux' */
      t->insertkey(&key, &aux);          /* insert entry into the hash part */
    }
  }
}

/*
** Clear new slice of the array.
*/
void Table::clearNewSlice(int oldasize, int newasize)
{
  Table* t = this;
  for (; oldasize < newasize; oldasize++)
    setArrTag(t, oldasize, LUA_VEMPTY);
}

/*
** Resize table 't' for the new given sizes. Both allocations (for
** the hash part and for the array part) can fail, which creates some
** subtleties. If the first allocation, for the hash part, fails, an
** error is raised and that is it. Otherwise, it copies the elements from
** the shrinking part of the array (if it is shrinking) into the new
** hash. Then it reallocates the array part.  If that fails, the table
** is in its original state; the function frees the new hash part and then
** raises the allocation error. Otherwise, it sets the new hash part
** into the table, initializes the new part of the array (if any) with
** nils and reinserts the elements of the old hash back into the new
** parts of the table.
** Note that if the new size for the array part ('newasize') is equal to
** the old one ('oldasize'), this function will do nothing with that
** part.
*/
void Table::luaH_resize(lua_State* L, int newasize,
                 int nhsize)
{
  Table* t = this;
  Table newt; /* to keep the new hash part */
  int oldasize = t->asize();
  std::span<Value> newarray;
  std::span<lu_byte> newtags;
  if (newasize > MAXASIZE)
    luaG_runerror(L, lt::table_overflow);
  /* create new hash part with appropriate size into 'newt' */
  newt.flags = 0;
  newt.setnodevector(L, nhsize);
  if (newasize < oldasize)
  { /* will array shrink? */
    /* re-insert into the new hash the elements from vanishing slice */
    exchangehashpart(t, &newt); /* pretend table has new hash */
    t->reinsertOldSlice(oldasize, newasize);
    exchangehashpart(t, &newt); /* restore old hash (in case of errors) */
  }
  /* allocate new array */
  newarray = t->resizearray(L, oldasize, newasize, &newtags);
  if (l_unlikely(newarray.data() == nullptr && newasize > 0))
  {                     /* allocation failed? */
    newt.freehash(L); /* release new hash part */
    luaD_throw(L, LUA_ERRMEM);      /* raise error (with array unchanged) */
  }
  /* allocation ok; initialize new part of the array */
  exchangehashpart(t, &newt); /* 't' has the new hash ('newt' has the old) */
  t->array = newarray;        /* set new array part */
  t->tags = newtags;
  if (newarray.data() != nullptr)
    t->lenhint = newasize / 2; /* set an initial hint */
  t->clearNewSlice(oldasize, newasize);
  /* re-insert elements from old hash part into new parts */
  reinserthash(L, &newt, t); /* 'newt' now has the old hash */
  newt.freehash(L);        /* free old hash part */
}

void Table::luaH_resizearray(lua_State* L, int nasize)
{
  Table* t = this;
  int nsize = allocsizenode(t);
  t->luaH_resize(L, nasize, nsize);
}

/*
** Rehash a table. First, count its keys. If there are array indices
** outside the array part, compute the new best size for that part.
** Then, resize the table.
*/
void Table::rehash(lua_State* L, const TValue* ek)
{
  Table* t = this;
  int asize; /* optimal size for array part */
  Counters ct;
  int i;
  int nsize; /* size for the hash part */
  /* reset counts */
  for (i = 0; i <= MAXABITS; i++)
    ct.nums[i] = 0;
  ct.na = 0;
  ct.deleted = 0;
  ct.total = 1; /* count extra key */
  if (ttisinteger(ek))
    countint(ivalue(ek), &ct); /* extra key may go to array */
  t->numusehash(&ct);          /* count keys in hash part */
  if (ct.na == 0)
  {
    /* no new keys to enter array part; keep it with the same size */
    asize = t->asize();
  }
  else
  {                            /* compute best size for array part */
    t->numusearray(&ct);       /* count keys in array part */
    asize = computesizes(&ct); /* compute new size for array part */
  }
  /* all keys not in the array part go to the hash part */
  nsize = ct.total - ct.na;
  if (ct.deleted)
  { /* table has deleted entries? */
    /* insertion-deletion-insertion: give hash some extra size to
       avoid repeated resizings */
    nsize += nsize >> 2;
  }
  /* resize the table to new computed sizes */
  t->luaH_resize(L, asize, nsize);
}

/*
** }=============================================================
*/

Table* Table::luaH_new(lua_State* L)
{
  Table* t = luaM_malloc_Table(L);
  luaC_link(L, t, LUA_VTABLE);
  t->flags = maskflags; /* table has no metamethod fields */
  t->setnodevector(L, 0);
  return t;
}

l_mem Table::luaH_size()
{
  Table* t = this;
  l_mem sz = cost::Table
            + t->array.size() * (cost::Value + cost::Byte);
  if (!isdummy(t))
    sz += t->sizehash();
  return sz;
}

/*
** Frees a table.
*/
void Table::luaH_free(lua_State* L)
{
  Table* t = this;
  std::span<lu_byte> newtags;
  t->freehash(L);
  t->resizearray(L, t->asize(), 0, &newtags);
  luaM_free_Table(L, t);
}

/*
** Sets node 'i' of the hash part as a node of another table is: its key
** (none when 'key' is null), its value (empty when 'val' is null or nil)
** and its chain link 'link'. With 'dead', the key is a dead key (an entry
** removed before a collection): 'key' then gives only its identity, the
** object it was (a collectable value or a dead key read with
** 'getnodekey'), which 'next' compares by address and nothing
** dereferences; with a null 'key' it has none. With the same keys in the
** same slots and links, and the same 'lastfree', a table iterates in the
** same order, also from a deleted key. The caller keeps every key in a
** slot of the chain that starts at its main position.
*/
void Table::luaH_setnode(lua_State* L, int i, const TValue* key, bool dead,
                         const TValue* val, int link)
{
  Table* t = this;
  assert(!isdummy(t) && 0 <= i && i < sizenode(t));
  assert(0 <= i + link && i + link < sizenode(t));
  Node* n = gnode(t, i);
  n->next = link;
  if (dead)
  {
    assert(key == nullptr || iscollectable(key) || key->tt_ == LUA_TDEADKEY);
    setdeadkey(n);
    n->key_val = (key != nullptr) ? key->value_ : Value{0, nullptr};
  }
  else if (key == nullptr)
    setnilkey(n);
  else
  {
    setnodekey(n, key);
    luaC_barrierback(L, t, key);
  }
  if (val == nullptr || ttisnil(val))
    setempty(&n->val);
  else
  {
    setobj(nullptr, &n->val, val);
    luaC_barrierback(L, t, val);
  }
  invalidateTMcache(t);
}

/*
** Copies 'src' into this table, which must be new and empty (and anchored,
** as copying allocates): the same array and hash sizes, every node in its
** slot, the same 'lastfree' and length hint, and the same metatable, so the
** copy iterates in the same order as 'src', also from a deleted key.
*/
void Table::luaH_copyfrom(lua_State* L, Table* src)
{
  Table* t = this;
  t->luaH_resize(L, src->asize(), allocsizenode(src));
  for (int i = 0; i < src->asize(); i++)
  {
    TValue v;
    lu_byte tag = getArrTag(src, i);
    if (tagisempty(tag))
      continue;
    farr2val(src, i, tag, &v);
    obj2arr(t, i, &v);
    luaC_barrierback(L, t, &v);
  }
  if (!isdummy(src))
  {
    for (int i = 0; i < sizenode(src); i++)
    {
      const Node* n = gnode(src, i);
      TValue key; /* a live key, or a dead key's identity */
      bool hasKey = !keyisnil(n);
      if (hasKey)
        getnodekey(L, &key, n);
      t->luaH_setnode(L, i, hasKey ? &key : nullptr, keyisdead(n), &n->val, n->next);
    }
    t->lastfree = src->lastfree;
  }
  t->lenhint = src->lenhint;
  t->metatable = src->metatable;
  if (t->metatable != nullptr)
    luaC_objbarrier(L, t, t->metatable);
  invalidateTMcache(t);
}

int Table::getfreepos()
{
  Table* t = this;
  if (t->haslastfree())
  { /* does it have 'lastfree' information? */
    /* look for a spot before 'lastfree', updating 'lastfree' */
    while (t->lastfree > 0)
    {
      int free = --t->lastfree;
      if (keyisnil(gnode(t, free)))
        return free;
    }
  }
  else
  { /* no 'lastfree' information */
    int i = sizenode(t);
    while (i--)
    { /* do a linear search */
      if (keyisnil(gnode(t, i)))
        return i;
    }
  }
  return -1; /* could not find a free place */
}

/*
** Inserts a new key into a hash table; first, check whether key's main
** position is free. If not, check whether colliding node is in its main
** position or not: if it is not, move colliding node to an empty place
** and put new key in its main position; otherwise (colliding node is in
** its main position), new key goes to an empty position. Return 0 if
** could not insert key (could not find a free space).
*/
int Table::insertkey(const TValue* key, TValue* value)
{
  Table* t = this;
  int mp = t->mainpositionTV(key);
  /* table cannot already contain the key */
  assert(t->nodegeneric(key, 0) < 0);
  if (!isempty(&gnode(t, mp)->val) || isdummy(t))
  { /* main position is taken? */
    int f = t->getfreepos(); /* get a free place */
    if (f < 0)             /* cannot find a free place? */
      return 0;
    assert(!isdummy(t));
    int othern = t->mainpositionfromnode(mp);
    if (othern != mp)
    { /* is colliding node out of its main position? */
      /* yes; move colliding node into free position */
      while (othern + (gnode(t, othern)->next) != mp) /* find previous */
        othern += gnode(t, othern)->next;
      gnode(t, othern)->next = f - othern; /* rechain to point to 'f' */
      copynode(gnode(t, f), gnode(t, mp));          /* copy colliding node into free pos. (mp->next also goes) */
      if ((gnode(t, mp)->next) != 0)
      {
        gnode(t, f)->next += mp - f; /* correct 'next' */
        gnode(t, mp)->next = 0;      /* now 'mp' is free */
      }
      setempty(&gnode(t, mp)->val);
    }
    else
    { /* colliding node is in its own main position */
      /* new node will go into free position */
      if ((gnode(t, mp)->next) != 0)
        (gnode(t, f)->next) = (mp + gnode(t, mp)->next) - f; /* chain new position */
      else
        assert(gnode(t, f)->next == 0);
      gnode(t, mp)->next = f - mp;
      mp = f;
    }
  }
  setnodekey(gnode(t, mp), key);
  assert(isempty(&gnode(t, mp)->val));
  setobj(nullptr, &gnode(t, mp)->val, value);
  return 1;
}

/*
** Insert a key in a table where there is space for that key, the
** key is valid, and the value is not nil.
*/
void Table::newcheckedkey(const TValue* key, TValue* value)
{
  Table* t = this;
  int i = t->keyinarray(key);
  if (i > 0)                  /* is key in the array part? */
    obj2arr(t, i - 1, value); /* set value in the array */
  else
  {
    int done = t->insertkey(key, value); /* insert key in the hash part */
    assert(done);                    /* it cannot fail */
    (void)done;                    /* to avoid warnings */
  }
}

void Table::luaH_newkey(lua_State* L, const TValue* key,
                        TValue* value)
{
  Table* t = this;
  if (!isempty(value))
  { /* do not insert nil values */
    int done = t->insertkey(key, value);
    if (!done)
    {                               /* could not find a free place? */
      t->rehash(L, key);            /* grow table */
      t->newcheckedkey(key, value); /* insert key in grown table */
    }
    assert(novariant(t->tt) >= LUA_TSTRING);
    luaC_barrierback(L, t, key);
  }
}

int Table::nodeintfromhash(lua_Integer key)
{
  Table* t = this;
  int i = t->hashint(key);
  assert(!t->ikeyinarray(key));
  for (;;)
  { /* check whether 'key' is somewhere in the chain */
    Node* n = gnode(t, i);
    if (keyisinteger(n) && keyival(n) == key)
      return i; /* that's it */
    else
    {
      int nx = n->next;
      if (nx == 0)
        break;
      i += nx;
    }
  }
  return -1;
}

const TValue* Table::getintfromhash(lua_Integer key)
{
  Table* t = this;
  int idx = t->nodeintfromhash(key);
  return (idx < 0) ? &absentkey : &gnode(t, idx)->val;
}

int Table::hashkeyisempty(lua_Integer key)
{
  Table* t = this;
  const TValue* val = t->getintfromhash(key);
  return isempty(val);
}

static lu_byte finishnodeget(const TValue* val, TValue* res)
{
  if (!isempty(val))
  {
    setobj(nullptr, res, val);
  }
  return ttypetag(val);
}

lu_byte Table::luaH_getint(lua_Integer key, TValue* res)
{
  Table* t = this;
  int k = t->ikeyinarray(key);
  if (k > 0)
  {
    lu_byte tag = getArrTag(t, k - 1);
    if (!tagisempty(tag))
      farr2val(t, k - 1, tag, res);
    return tag;
  }
  else
    return finishnodeget(t->getintfromhash(key), res);
}

int Table::nodeshortstr(TString* key)
{
  Table* t = this;
  int i = t->hashstr(key);
  for (;;)
  { /* check whether 'key' is somewhere in the chain */
    Node* n = gnode(t, i);
    if (keyisshrstr(n))
    {
      if (luaS_eqstr(keystrval(n), key))
        return i; /* that's it */
    }
    int nx = n->next;
    if (nx == 0)
      return -1; /* not found */
    i += nx;
  }
}

const TValue* Table::luaH_Hgetshortstr(TString* key)
{
  Table* t = this;
  int idx = t->nodeshortstr(key);
  return (idx < 0) ? &absentkey : &gnode(t, idx)->val;
}

lu_byte Table::luaH_getshortstr(TString* key, TValue* res)
{
  Table* t = this;
  return finishnodeget(t->luaH_Hgetshortstr(key), res);
}

const TValue* Table::Hgetstr(TString* key)
{
  Table* t = this;
  return t->luaH_Hgetshortstr(key);
}

lu_byte Table::luaH_getstr(TString* key, TValue* res)
{
  Table* t = this;
  return finishnodeget(t->Hgetstr(key), res);
}

/*
** main search function
*/
lu_byte Table::luaH_get(const TValue* key, TValue* res)
{
  Table* t = this;
  const TValue* slot;
  switch (ttypetag(key))
  {
  case LUA_VSHRSTR:
    slot = t->luaH_Hgetshortstr(tsvalue(key));
    break;
  case LUA_VNUMINT:
    return t->luaH_getint(ivalue(key), res);
  case LUA_VNIL:
    slot = &absentkey;
    break;
  case LUA_VNUMFLT:
  {
    IntResult k;
    if (luaV_flttointeger(fltvalue(key), &k, F2Ieq)) /* integral index? */
      return t->luaH_getint(k.v, res);                 /* use specialized version */
    /* else... */
  } /* FALLTHROUGH */
  default:
    slot = t->getgeneric(key, 0);
    break;
  }
  return finishnodeget(slot, res);
}

/*
** When a 'pset' cannot be completed, this function returns an encoding
** of its result, to be used by 'luaH_finishset'.
*/
static int retpsetcode(int idx)
{
  if (idx < 0)
    return HNOTFOUND; /* no slot with that key */
  else                /* return node encoded */
    return idx + HFIRSTNODE;
}

int Table::finishnodeset(int idx, TValue* val)
{
  Table* t = this;
  if (idx >= 0 && !isempty(&gnode(t, idx)->val))
  {
    setobj(nullptr, &gnode(t, idx)->val, val);
    return HOK; /* success */
  }
  else
    return retpsetcode(idx);
}

int Table::rawfinishnodeset(int idx, TValue* val)
{
  if (idx < 0)
    return 0; /* no slot with that key */
  else
  {
    Table* t = this;
    setobj(nullptr, &gnode(t, idx)->val, val);
    return 1; /* success */
  }
}

int Table::luaH_psetint(lua_Integer key, TValue* val)
{
  Table* t = this;
  assert(!t->ikeyinarray(key));
  return t->finishnodeset(t->nodeintfromhash(key), val);
}

int Table::psetint(lua_Integer key, TValue* val)
{
  Table* t = this;
  int hres;
  lua_Integer u = l_intsub(key, 1);
  if (l_ult(u, t->asize()))
  {
    lu_byte tag = getArrTag(t, u);
    if (checknoTM(t->metatable, TM_NEWINDEX) || !tagisempty(tag))
    {
      fval2arr(t, u, val);
      hres = HOK;
    }
    else
      hres = ~(int)u;
  }
  else
    hres = t->luaH_psetint(key, val);
  return hres;
}

/*
** This function could be just this:
**    return finishnodeset(t, luaH_Hgetshortstr(t, key), val);
** However, it optimizes the common case created by constructors (e.g.,
** {x=1, y=2}), which creates a key in a table that has no metatable,
** it is not old/black, and it already has space for the key.
*/

int Table::luaH_psetshortstr(TString* key, TValue* val)
{
  Table* t = this;
  int idx = t->nodeshortstr(key);
  if (idx >= 0 && !isempty(&gnode(t, idx)->val))
  {                                                        /* key already has a value? (all too common) */
    setobj(nullptr, &gnode(t, idx)->val, val); /* update it */
    return HOK;                                            /* done */
  }
  else if (checknoTM(t->metatable, TM_NEWINDEX))
  {                        /* no metamethod? */
    if (isempty(val))      /* new value is nil? */
      return HOK;          /* done (value is already nil/absent) */
    if (idx < 0 && /* key is absent? */
        !(isblack(t) && iswhite(key)))
    {            /* and don't need barrier? */
      TValue tk; /* key as a TValue */
      setsvalue(nullptr, &tk, key);
      if (t->insertkey(&tk, val))
      { /* insert key, if there is space */
        invalidateTMcache(t);
        return HOK;
      }
    }
  }
  /* Else, either table has new-index metamethod, or it needs barrier,
     or it needs to rehash for the new key. In any of these cases, the
     operation cannot be completed here. Return a code for the caller. */
  return retpsetcode(idx);
}

int Table::luaH_psetstr(TString* key, TValue* val)
{
  Table* t = this;
  return t->luaH_psetshortstr(key, val);
}

int Table::luaH_pset(const TValue* key, TValue* val)
{
  Table* t = this;
  switch (ttypetag(key))
  {
  case LUA_VSHRSTR:
    return t->luaH_psetshortstr(tsvalue(key), val);
  case LUA_VNUMINT:
    return t->psetint(ivalue(key), val);
  case LUA_VNIL:
    return HNOTFOUND;
  case LUA_VNUMFLT:
  {
    IntResult k;
    if (luaV_flttointeger(fltvalue(key), &k, F2Ieq)) /* integral index? */
      return t->psetint(k.v, val);                     /* use specialized version */
    /* else... */
  } /* FALLTHROUGH */
  default:
    return t->finishnodeset(t->nodegeneric(key, 0), val);
  }
}

/*
** Finish a raw "set table" operation, where 'hres' encodes where the
** value should have been (the result of a previous 'pset' operation).
** Beware: when using this function the caller probably need to check a
** GC barrier and invalidate the TM cache.
*/
void Table::luaH_finishset(lua_State* L, const TValue* key,
                    TValue* value, int hres)
{
  Table* t = this;
  assert(hres != HOK);
  if (hres == HNOTFOUND)
  {
    TValue aux;
    const TValue* actk = key; /* actual key to insert */
    if (l_unlikely(isempty(key)))
      luaG_runerror(L, lt::table_index_is_nil);
    else if (ttisfloat(key))
    {
      lua_Number f = fltvalue(key);
      IntResult k;
      if (luaV_flttointeger(f, &k, F2Ieq))
      { /* is key equal to an integer? */
        setivalue(&aux, k.v);
        actk = &aux; /* use the integer as the key */
      }
      else if (l_unlikely(!(f == f)))
        luaG_runerror(L, lt::table_index_is_NaN);
    }
    t->luaH_newkey(L, actk, value);
  }
  else if (hres > 0)
  { /* regular Node? */
    setobj(L, &gnode(t, hres - HFIRSTNODE)->val, value);
  }
  else
  {               /* array entry */
    hres = ~hres; /* real index */
    obj2arr(t, hres, value);
  }
}

/*
** beware: when using this function you probably need to check a GC
** barrier and invalidate the TM cache.
*/
void Table::luaH_set(lua_State* L, const TValue* key, TValue* value)
{
  Table* t = this;
  int hres = t->luaH_pset(key, value);
  if (hres != HOK)
    t->luaH_finishset(L, key, value, hres);
}

/*
** Ditto for a GC barrier. (No need to invalidate the TM cache, as
** integers cannot be keys to metamethods.)
*/
void Table::luaH_setint(lua_State* L, lua_Integer key, TValue* value)
{
  Table* t = this;
  int ik = t->ikeyinarray(key);
  if (ik > 0)
    obj2arr(t, ik - 1, value);
  else
  {
    int ok = t->rawfinishnodeset(t->nodeintfromhash(key), value);
    if (!ok)
    {
      TValue k;
      setivalue(&k, key);
      t->luaH_newkey(L, &k, value);
    }
  }
}

/*
** Try to find a boundary in the hash part of table 't'. From the
** caller, we know that 'asize + 1' is present. We want to find a larger
** key that is absent from the table, so that we can do a binary search
** between the two keys to find a boundary. We keep doubling 'j' until
** we get an absent index.  If the doubling would overflow, we try
** LUA_MAXINTEGER. If it is absent, we are ready for the binary search.
** ('j', being max integer, is larger or equal to 'i', but it cannot be
** equal because it is absent while 'i' is present.) Otherwise, 'j' is a
** boundary. ('j + 1' cannot be a present integer key because it is not
** a valid integer in Lua.)
** About 'rnd': If we used a fixed algorithm, a bad actor could fill
** a table with only the keys that would be probed, in such a way that
** a small table could result in a huge length. To avoid that, we use
** the state's seed as a source of randomness. For the first probe,
** we "randomly double" 'i' by adding to it a random number roughly its
** width.
*/
lua_Integer Table::hash_search(lua_State* L, int asize)
{
  Table* t = this;
  lua_Integer i = (lua_Integer)asize + 1; /* caller ensures t[i] is present */
  unsigned rnd = L->l_G->seed;
  int n = (asize > 0) ? luaO_ceillog2(asize) : 0; /* width of 'asize' */
  lua_Integer mask = ((lua_Integer)1 << n) - 1;                  /* 11...111 with the width of 'asize' */
  lua_Integer incr = (rnd & mask) + 1;               /* first increment (at least 1) */
  lua_Integer j = (incr <= LUA_MAXINTEGER - i) ? i + incr : i + 1;
  rnd >>= n; /* used 'n' bits from 'rnd' */
  while (!t->hashkeyisempty(j))
  {        /* repeat until an absent t[j] */
    i = j; /* 'i' is a present index */
    if (j <= LUA_MAXINTEGER / 2 - 1)
    {
      j = j * 2 + (rnd & 1); /* try again with 2j or 2j+1 */
      rnd >>= 1;
    }
    else
    {
      j = LUA_MAXINTEGER;
      if (t->hashkeyisempty(j)) /* t[j] not present? */
        break;                  /* 'j' now is an absent index */
      else                      /* weird case */
        return j;               /* well, max integer is a boundary... */
    }
  }
  /* i < j  &&  t[i] present  &&  t[j] absent */
  while (j - i > 1)
  { /* do a binary search between them */
    lua_Integer m = i + (j - i) / 2;
    if (t->hashkeyisempty(m))
      j = m;
    else
      i = m;
  }
  return i;
}

int Table::binsearch(int i, int j)
{
  Table* t = this;
  assert(i <= j);
  while (j - i > 1)
  { /* binary search */
    int m = i + (j - i) / 2;
    if (t->arraykeyisempty(m))
      j = m;
    else
      i = m;
  }
  return i;
}

/* return a border, saving it as a hint for next call */
lua_Integer Table::newhint(int hint)
{
  Table* t = this;
  assert(hint <= t->asize());
  t->lenhint = hint;
  return hint;
}

/*
** Try to find a border in table 't'. (A 'border' is an integer index
** such that t[i] is present and t[i+1] is absent, or 0 if t[1] is absent,
** or 'maxinteger' if t[maxinteger] is present.)
** If there is an array part, try to find a border there. First try
** to find it in the vicinity of the previous result (hint), to handle
** cases like 't[#t + 1] = val' or 't[#t] = nil', that move the border
** by one entry. Otherwise, do a binary search to find the border.
** If there is no array part, or its last element is non empty, the
** border may be in the hash part.
*/
lua_Integer Table::luaH_getn(lua_State* L)
{
  Table* t = this;
  int asize = t->asize();
  if (asize > 0)
  { /* is there an array part? */
    const int maxvicinity = 4;
    int limit = t->lenhint; /* start with the hint */
    if (limit == 0)
      limit = 1; /* make limit a valid index in the array */
    if (t->arraykeyisempty(limit))
    { /* t[limit] empty? */
      /* there must be a border before 'limit' */
      int i;
      /* look for a border in the vicinity of the hint */
      for (i = 0; i < maxvicinity && limit > 1; i++)
      {
        limit--;
        if (!t->arraykeyisempty(limit))
          return t->newhint(limit); /* 'limit' is a border */
      }
      /* t[limit] still empty; search for a border in [0, limit) */
      return t->newhint(t->binsearch(0, limit));
    }
    else
    { /* 'limit' is present in table; look for a border after it */
      int i;
      /* look for a border in the vicinity of the hint */
      for (i = 0; i < maxvicinity && limit < asize; i++)
      {
        limit++;
        if (t->arraykeyisempty(limit))
          return t->newhint(limit - 1); /* 'limit - 1' is a border */
      }
      if (t->arraykeyisempty(asize))
      { /* last element empty? */
        /* t[limit] not empty; search for a border in [limit, asize) */
        return t->newhint(t->binsearch(limit, asize));
      }
    }
    /* last element non empty; set a hint to speed up finding that again */
    /* (keys in the hash part cannot be hints) */
    t->lenhint = asize;
  }
  /* no array part or t[asize] is not empty; check the hash part */
  assert(asize == 0 || !t->arraykeyisempty(asize));
  if (isdummy(t) || t->hashkeyisempty((lua_Integer)asize + 1))
    return asize; /* 'asize + 1' is empty */
  else            /* 'asize + 1' is also non empty */
    return t->hash_search(L, asize);
}

/*
** The main position of 'key': the node where a lookup for it starts. Every
** key of the hash part is in the chain that starts there (the test library
** and checks of a table's layout use it).
*/
int Table::luaH_mainposition(const TValue* key) const
{
  const Table* t = this;
  return t->mainpositionTV(key);
}

}
