/*
** $Id: ltable.c $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

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

/*
** Only hash parts with at least 2^LIMFORLAST have a 'lastfree' field
** that optimizes finding a free slot. That field is stored just before
** the array of nodes, in the same block. Smaller tables do a complete
** search when looking for a free slot.
*/
#define LIMFORLAST 3 /* log2 of real limit (8) */

static inline int haslastfree(const Table* t)
{
  return t->lsizenode >= LIMFORLAST;
}
static inline Node*& getlastfree(Table* t)
{
  return t->lastfree;
}

/*
** MAXABITS is the largest integer such that 2^MAXABITS fits in an
** unsigned int.
*/
#define MAXABITS (((int)(sizeof(int) * CHAR_BIT)) - 1)

/*
** MAXASIZEB is the maximum number of elements in the array part such
** that the size of the array fits in 'size_t'.
*/
#define MAXASIZEB (MAX_SIZET / (sizeof(Value) + 1))

/*
** MAXASIZE is the maximum size of the array part. It is the minimum
** between 2^MAXABITS and MAXASIZEB.
*/
#define MAXASIZE (((1u << MAXABITS) < MAXASIZEB) ? (1u << MAXABITS) : ((unsigned int)(MAXASIZEB)))

/*
** MAXHBITS is the largest integer such that 2^MAXHBITS fits in a
** signed int.
*/
#define MAXHBITS (MAXABITS - 1)

/*
** MAXHSIZE is the maximum size of the hash part. It is the minimum
** between 2^MAXHBITS and the maximum size such that, measured in bytes,
** it fits in a 'size_t'.
*/
#define MAXHSIZE ((((size_t)(1 << MAXHBITS)) <= MAX_SIZET / sizeof(Node)) ? (1 << MAXHBITS) : ((int)((MAX_SIZET / sizeof(Node)))))

/*
** When the original hash value is good, hashing by a power of 2
** avoids the cost of '%'.
*/
static inline Node* hashpow2(const Table* t, lua_Unsigned n)
{
  return gnode(t, lmod(n, sizenode(t)));
}

/*
** for other types, it is better to avoid modulo by power of 2, as
** they can have many 2 factors.
*/
static inline Node* hashmod(const Table* t, lua_Unsigned n)
{
  return gnode(t, n % ((sizenode(t) - 1u) | 1u));
}

static inline Node* hashstr(const Table* t, const TString* str)
{
  return hashpow2(t, str->hash);
}
static inline Node* hashboolean(const Table* t, int p)
{
  return hashpow2(t, (lua_Unsigned)p);
}

static inline Node* hashpointer(const Table* t, const void* p)
{
  return hashmod(t, point2uint(p));
}

static inline Node* hashpointer(const Table* t, lua_CFunction p)
{
  return hashmod(t, point2uint(p));
}

#define dummynode (&dummynode_)

/*
** Common hash part for tables with empty hash parts. That allows all
** tables to have a hash part, avoiding an extra check ("is there a hash
** part?") when indexing. Its sole node has an empty value and a key
** (DEADKEY, NULL) that is different from any valid TValue.
*/
static const Node dummynode_ = {
    {{NULL}, LUA_VEMPTY}, /* val (value and type) */
    LUA_TDEADKEY,         /* key_tt */
    0,                    /* next */
    {NULL}                /* key_val */
};

static const TValue absentkey = {ABSTKEYCONSTANT};

/*
** Hash for integers. To allow a good hash, use the remainder operator
** ('%'). If integer fits as a non-negative int, compute an int
** remainder, which is faster. Otherwise, use an unsigned-integer
** remainder, which uses all bits and ensures a non-negative result.
*/
static Node* hashint(const Table* t, lua_Integer i)
{
  lua_Unsigned ui = ((lua_Unsigned)(i));
  if (ui <= ((unsigned int)(INT_MAX)))
    return gnode(t, ((int)(ui)) % ((int)((sizenode(t) - 1) | 1)));
  else
    return hashmod(t, ui);
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
#if !defined(l_hashfloat)
static unsigned l_hashfloat(lua_Number n)
{
  int i;
  lua_Integer ni;
  n = l_mathop(frexp)(n, &i) * -((lua_Number)(INT_MIN));
  if (!lua_numbertointeger(n, &ni))
  { /* is 'n' inf/-inf/NaN? */
    lua_assert((!((n) == (n))) || l_mathop(fabs)(n) == ((lua_Number)(HUGE_VAL)));
    return 0;
  }
  else
  { /* normal case */
    unsigned int u = ((unsigned int)(i)) + ((unsigned int)(ni));
    return (u <= ((unsigned int)(INT_MAX)) ? u : ~u);
  }
}
#endif

/*
** returns the 'main' position of an element in a table (that is,
** the index of its hash value).
*/
static Node* mainpositionTV(const Table* t, const TValue* key)
{
  switch (((key)->tt_ & WITHVARIANT_MASK))
  {
  case LUA_VNUMINT:
  {
    lua_Integer i = (lua_assert((key)->tt_ == LUA_VNUMINT), ((key)->value_).i);
    return hashint(t, i);
  }
  case LUA_VNUMFLT:
  {
    lua_Number n = (lua_assert((key)->tt_ == LUA_VNUMFLT), ((key)->value_).n);
    return hashmod(t, l_hashfloat(n));
  }
  case LUA_VSHRSTR:
  {
    TString* ts = (lua_assert(((key)->tt_ & NOVARIANT_MASK) == LUA_TSTRING), (lua_assert((((((key)->value_).gc)->tt) & NOVARIANT_MASK) == LUA_TSTRING), static_cast<TString*>(((key)->value_).gc)));
    return hashstr(t, ts);
  }
  case LUA_VFALSE:
    return hashboolean(t, 0);
  case LUA_VTRUE:
    return hashboolean(t, 1);
  case LUA_VLIGHTUSERDATA:
  {
    void* p = (lua_assert((key)->tt_ == LUA_VLIGHTUSERDATA), ((key)->value_).p);
    return hashpointer(t, p);
  }
  case LUA_VLCF:
  {
    lua_CFunction f = (lua_assert((key)->tt_ == LUA_VLCF), ((key)->value_).f);
    return hashpointer(t, f);
  }
  default:
  {
    GCObject* o = (lua_assert((key)->tt_ & BIT_ISCOLLECTABLE), ((key)->value_).gc);
    return hashpointer(t, o);
  }
  }
}

static inline Node* mainpositionfromnode(const Table* t, Node* nd)
{
  TValue key;
  getnodekey(((lua_State*)(NULL)), &key, nd);
  return mainpositionTV(t, &key);
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
  if ((k1)->tt_ != ((n2)->key_tt))
  { /* not the same variants? */
    if (((n2)->key_tt == ctb(LUA_VSHRSTR)) && (((k1)->tt_ & NOVARIANT_MASK) == LUA_TSTRING))
    {
      /* an external string can be equal to a short-string key */
      return luaS_eqstr((lua_assert(((k1)->tt_ & NOVARIANT_MASK) == LUA_TSTRING), (lua_assert((((((k1)->value_).gc)->tt) & NOVARIANT_MASK) == LUA_TSTRING), static_cast<TString*>(((k1)->value_).gc))), ((lua_assert(((((n2)->key_val).gc)->tt & NOVARIANT_MASK) == LUA_TSTRING), static_cast<TString*>(((n2)->key_val).gc))));
    }
    else if (deadok && ((n2)->key_tt == LUA_TDEADKEY) && ((k1)->tt_ & BIT_ISCOLLECTABLE))
    {
      /* a collectable value can be equal to a dead key */
      return (lua_assert((k1)->tt_ & BIT_ISCOLLECTABLE), ((k1)->value_).gc) == ((((n2)->key_val)).gc);
    }
    else
      return 0; /* otherwise, different variants cannot be equal */
  }
  else
  { /* equal variants */
    switch (((n2)->key_tt))
    {
    case LUA_VNIL:
    case LUA_VFALSE:
    case LUA_VTRUE:
      return 1;
    case LUA_VNUMINT:
      return ((lua_assert((k1)->tt_ == LUA_VNUMINT), ((k1)->value_).i) == (((n2)->key_val).i));
    case LUA_VNUMFLT:
      return (((lua_assert((k1)->tt_ == LUA_VNUMFLT), ((k1)->value_).n)) == (((((n2)->key_val)).n)));
    case LUA_VLIGHTUSERDATA:
      return (lua_assert((k1)->tt_ == LUA_VLIGHTUSERDATA), ((k1)->value_).p) == ((((n2)->key_val)).p);
    case LUA_VLCF:
      return (lua_assert((k1)->tt_ == LUA_VLCF), ((k1)->value_).f) == ((((n2)->key_val)).f);
    case ctb(LUA_VLNGSTR):
      return luaS_eqstr((lua_assert(((k1)->tt_ & NOVARIANT_MASK) == LUA_TSTRING), (lua_assert((((((k1)->value_).gc)->tt) & NOVARIANT_MASK) == LUA_TSTRING), static_cast<TString*>(((k1)->value_).gc))), ((lua_assert(((((n2)->key_val).gc)->tt & NOVARIANT_MASK) == LUA_TSTRING), static_cast<TString*>(((n2)->key_val).gc))));
    default:
      return (lua_assert((k1)->tt_ & BIT_ISCOLLECTABLE), ((k1)->value_).gc) == ((((n2)->key_val)).gc);
    }
  }
}

/*
** "Generic" get version. (Not that generic: not valid for integers,
** which may be in array part, nor for floats with integral values.)
** See explanation about 'deadok' in function 'equalkey'.
*/
static const TValue* getgeneric(Table* t, const TValue* key, int deadok)
{
  Node* n = mainpositionTV(t, key);
  for (;;)
  { /* check whether 'key' is somewhere in the chain */
    if (equalkey(key, n, deadok))
      return (&(n)->val); /* that's it */
    else
    {
      int nx = ((n)->next);
      if (nx == 0)
        return &absentkey; /* not found */
      n += nx;
    }
  }
}

/*
** Return the index 'k' (converted to an unsigned) if it is inside
** the range [1, limit].
*/
static unsigned checkrange(lua_Integer k, unsigned limit)
{
  return (((lua_Unsigned)(k)) - 1u < limit) ? ((unsigned int)(k)) : 0;
}

/*
** Return the index 'k' if 'k' is an appropriate key to live in the
** array part of a table, 0 otherwise.
*/
static inline unsigned arrayindex(lua_Integer k)
{
  return checkrange(k, MAXASIZE);
}

/*
** Check whether an integer key is in the array part of a table and
** return its index there, or zero.
*/
static inline unsigned ikeyinarray(const Table* t, lua_Integer k)
{
  return checkrange(k, t->asize);
}

/*
** Check whether a key is in the array part of a table and return its
** index there, or zero.
*/
static unsigned keyinarray(Table* t, const TValue* key)
{
  return (((key)->tt_ == LUA_VNUMINT)) ? ikeyinarray(t, (lua_assert((key)->tt_ == LUA_VNUMINT), ((key)->value_).i)) : 0;
}

/*
** returns the index of a 'key' for table traversals. First goes all
** elements in the array part, then elements in the hash part. The
** beginning of a traversal is signaled by 0.
*/
static unsigned findindex(lua_State* L, Table* t, TValue* key,
                          unsigned asize)
{
  unsigned int i;
  if ((((key)->tt_ & NOVARIANT_MASK) == LUA_TNIL))
    return 0; /* first iteration */
  i = keyinarray(t, key);
  if (i != 0) /* is 'key' inside array part? */
    return i; /* yes; that's the index */
  else
  {
    const TValue* n = getgeneric(t, key, 1);
    if (l_unlikely(((n)->tt_ == LUA_VABSTKEY)))
      luaG_runerror(L, "invalid key to 'next'"); /* key not found */
    i = ((unsigned int)(((Node*)((n))) - gnode(t, 0))); /* key index in hash table */
    /* hash elements are numbered after array ones */
    return (i + 1) + asize;
  }
}

int luaH_next(lua_State* L, Table* t, StkId key)
{
  unsigned int asize = t->asize;
  unsigned int i = findindex(L, t, (&(L)->stack.p[key]), asize); /* find original key */
  for (; i < asize; i++)
  { /* try first array part */
    lu_byte tag = *getArrTag(t, i);
    if (!(((tag) & NOVARIANT_MASK) == LUA_TNIL))
    { /* a non-empty entry? */
      setivalue((&(L)->stack.p[key]), ((int)(i)) + 1);
      farr2val(t, i, tag, (&(L)->stack.p[key + 1]));
      return 1;
    }
  }
  for (i -= asize; i < sizenode(t); i++)
  { /* hash part */
    if (!((((&(gnode(t, i))->val))->tt_ & NOVARIANT_MASK) == LUA_TNIL))
    { /* a non-empty entry? */
      Node* n = gnode(t, i);
      getnodekey(L, (&(L)->stack.p[key]), n);
      setobj(L, (&(L)->stack.p[key + 1]), (&(n)->val));
      return 1;
    }
  }
  return 0; /* no more elements */
}

/* 'node' size in bytes */
static size_t sizehash(Table* t)
{
  return ((size_t)(sizenode(t))) * sizeof(Node);
}

static void freehash(lua_State* L, Table* t)
{
  if (!isdummy(t))
  {
    luaM_free_(L, (t->node), (sizehash(t)));
  }
}

/*
** {=============================================================
** Rehash
** ==============================================================
*/

static int insertkey(Table* t, const TValue* key, TValue* value);
static void newcheckedkey(Table* t, const TValue* key, TValue* value);

/*
** Structure to count the keys in a table.
** 'total' is the total number of keys in the table.
** 'na' is the number of *array indices* in the table (see 'arrayindex').
** 'deleted' is true if there are deleted nodes in the hash part.
** 'nums' is a "count array" where 'nums[i]' is the number of integer
** keys between 2^(i - 1) + 1 and 2^i. Note that 'na' is the summation
** of 'nums'.
*/
typedef struct
{
  unsigned total;
  unsigned na;
  int deleted;
  unsigned nums[MAXABITS + 1];
} Counters;

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
static unsigned computesizes(Counters* ct)
{
  int i;
  unsigned int twotoi;      /* 2^i (candidate for optimal size) */
  unsigned int a = 0;       /* number of elements smaller than 2^i */
  unsigned int na = 0;      /* number of elements to go to array part */
  unsigned int optimal = 0; /* optimal size for array part */
  /* traverse slices while 'twotoi' does not overflow and total of array
     indices still can satisfy 'arrayXhash' against the array size */
  for (i = 0, twotoi = 1;
       twotoi > 0 && arrayXhash(twotoi, ct->na);
       i++, twotoi *= 2)
  {
    unsigned nums = ct->nums[i];
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
  unsigned int k = arrayindex(key);
  if (k != 0)
  {                               /* is 'key' an array index? */
    ct->nums[luaO_ceillog2(k)]++; /* count as such */
    ct->na++;
  }
}

static inline int arraykeyisempty(const Table* t, unsigned key)
{
  int tag = *getArrTag(t, key - 1);
  return (((tag) & NOVARIANT_MASK) == LUA_TNIL);
}

/*
** Count keys in array part of table 't'.
*/
static void numusearray(const Table* t, Counters* ct)
{
  int lg;
  unsigned int ttlg;     /* 2^lg */
  unsigned int ause = 0; /* summation of 'nums' */
  unsigned int i = 1;    /* index to traverse all array keys */
  unsigned int asize = t->asize;
  /* traverse each slice */
  for (lg = 0, ttlg = 1; lg <= MAXABITS; lg++, ttlg *= 2)
  {
    unsigned int lc = 0; /* counter */
    unsigned int lim = ttlg;
    if (lim > asize)
    {
      lim = asize; /* adjust upper limit */
      if (i > lim)
        break; /* no more elements to count */
    }
    /* count elements in range (2^(lg - 1), 2^lg] */
    for (; i <= lim; i++)
    {
      if (!arraykeyisempty(t, i))
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
static void numusehash(const Table* t, Counters* ct)
{
  unsigned i = sizenode(t);
  unsigned total = 0;
  while (i--)
  {
    Node* n = &t->node[i];
    if (((((&(n)->val))->tt_ & NOVARIANT_MASK) == LUA_TNIL))
    {
      lua_assert(!((n)->key_tt == LUA_TNIL)); /* entry was deleted; key cannot be nil */
      ct->deleted = 1;
    }
    else
    {
      total++;
      if (((n)->key_tt == LUA_VNUMINT))
        countint((((n)->key_val).i), ct);
    }
  }
  ct->total += total;
}

/*
** Convert an "abstract size" (number of slots in an array) to
** "concrete size" (number of bytes in the array).
*/
static size_t concretesize(unsigned int size)
{
  if (size == 0)
    return 0;
  else /* space for the two arrays plus an unsigned in between */
    return size * (sizeof(Value) + 1) + sizeof(unsigned);
}

/*
** Resize the array part of a table. If new size is equal to the old,
** do nothing. Else, if new size is zero, free the old array. (It must
** be present, as the sizes are different.) Otherwise, allocate a new
** array, move the common elements to new proper position, and then
** frees the old array.
** We could reallocate the array, but we still would need to move the
** elements to their new position, so the copy implicit in realloc is a
** waste. Moreover, most allocators will move the array anyway when the
** new size is double the old one (the most common case).
*/
static Value* resizearray(lua_State* L, Table* t,
                          unsigned oldasize,
                          unsigned newasize)
{
  if (oldasize == newasize)
    return t->array; /* nothing to be done */
  else if (newasize == 0)
  {                                              /* erasing array? */
    Value* op = t->array - oldasize;             /* original array's real address */
    luaM_free_(L, (op), (concretesize(oldasize))); /* free it */
    return NULL;
  }
  else
  {
    size_t newasizeb = concretesize(newasize);
    Value* np = ((Value*)((((lu_byte*)(luaM_realloc_(L, NULL, ((size_t)(0)) * sizeof(lu_byte), ((size_t)(newasizeb)) * sizeof(lu_byte)))))));
    if (np == NULL) /* allocation error? */
      return NULL;
    np += newasize; /* shift pointer to the end of value segment */
    if (oldasize > 0)
    {
      /* move common elements to new position */
      size_t oldasizeb = concretesize(oldasize);
      Value* op = t->array; /* original array */
      unsigned tomove = (oldasize < newasize) ? oldasize : newasize;
      size_t tomoveb = (oldasize < newasize) ? oldasizeb : newasizeb;
      lua_assert(tomoveb > 0);
      memcpy(np - tomove, op - tomove, tomoveb);
      luaM_free_(L, (op - oldasize), (oldasizeb)); /* free old block */
    }
    return np;
  }
}

/*
** Creates an array for the hash part of a table with the given
** size, or reuses the dummy node if size is zero.
** The computation for size overflow is in two steps: the first
** comparison ensures that the shift in the second one does not
** overflow.
*/
static void setnodevector(lua_State* L, Table* t, unsigned size)
{
  if (size == 0)
  {                                   /* no elements to hash part? */
    t->node = ((Node*)(dummynode)); /* use common 'dummynode' */
    t->lastfree = NULL;
    t->lsizenode = 0;
    setdummy(t); /* signal that it is using dummy node */
  }
  else
  {
    int i;
    int lsize = luaO_ceillog2(size);
    if (lsize > MAXHBITS || (1 << lsize) > MAXHSIZE)
      luaG_runerror(L, "table overflow");
    size = twoto(lsize);
    t->node = ((Node*)(luaM_malloc_(L, ((size_t)(size)) * sizeof(Node), 0)));
    t->lastfree = NULL;
    t->lsizenode = ((lu_byte)(lsize));
    if (lsize >= LIMFORLAST)
    {
      getlastfree(t) = gnode(t, size); /* all positions are free */
    }
    setnodummy(t);
    for (i = 0; i < ((int)(size)); i++)
    {
      Node* n = gnode(t, i);
      ((n)->next) = 0;
      ((n)->key_tt = LUA_TNIL);
      (((&(n)->val))->tt_ = LUA_VEMPTY);
    }
  }
}

/*
** (Re)insert all elements from the hash part of 'ot' into table 't'.
*/
static void reinserthash(lua_State* L, Table* ot, Table* t)
{
  unsigned j;
  unsigned size = sizenode(ot);
  for (j = 0; j < size; j++)
  {
    Node* old = gnode(ot, j);
    if (!((((&(old)->val))->tt_ & NOVARIANT_MASK) == LUA_TNIL))
    {
      /* doesn't need barrier/invalidate cache, as entry was
         already present in the table */
      TValue k;
      getnodekey(L, &k, old);
      newcheckedkey(t, &k, (&(old)->val));
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
  lu_byte lsizenode = t1->lsizenode;
  Node* node = t1->node;
  Node* lastfree = t1->lastfree;
  int bitdummy1 = t1->flags & BITDUMMY;
  t1->lsizenode = t2->lsizenode;
  t1->node = t2->node;
  t1->lastfree = t2->lastfree;
  t1->flags = ((lu_byte)((t1->flags & NOTBITDUMMY) | (t2->flags & BITDUMMY)));
  t2->lsizenode = lsizenode;
  t2->node = node;
  t2->lastfree = lastfree;
  t2->flags = ((lu_byte)((t2->flags & NOTBITDUMMY) | bitdummy1));
}

/*
** Re-insert into the new hash part of a table the elements from the
** vanishing slice of the array part.
*/
static void reinsertOldSlice(Table* t, unsigned oldasize,
                             unsigned newasize)
{
  unsigned i;
  for (i = newasize; i < oldasize; i++)
  { /* traverse vanishing slice */
    lu_byte tag = *getArrTag(t, i);
    if (!(((tag) & NOVARIANT_MASK) == LUA_TNIL))
    { /* a non-empty entry? */
      TValue key, aux;
      setivalue(&key, ((lua_Integer)(i)) + 1); /* make the key */
      farr2val(t, i, tag, &aux);         /* copy value into 'aux' */
      insertkey(t, &key, &aux);          /* insert entry into the hash part */
    }
  }
}

/*
** Clear new slice of the array.
*/
static void clearNewSlice(Table* t, unsigned oldasize, unsigned newasize)
{
  for (; oldasize < newasize; oldasize++)
    *getArrTag(t, oldasize) = LUA_VEMPTY;
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
void luaH_resize(lua_State* L, Table* t, unsigned newasize,
                 unsigned nhsize)
{
  Table newt; /* to keep the new hash part */
  unsigned oldasize = t->asize;
  Value* newarray;
  if (newasize > MAXASIZE)
    luaG_runerror(L, "table overflow");
  /* create new hash part with appropriate size into 'newt' */
  newt.flags = 0;
  setnodevector(L, &newt, nhsize);
  if (newasize < oldasize)
  { /* will array shrink? */
    /* re-insert into the new hash the elements from vanishing slice */
    exchangehashpart(t, &newt); /* pretend table has new hash */
    reinsertOldSlice(t, oldasize, newasize);
    exchangehashpart(t, &newt); /* restore old hash (in case of errors) */
  }
  /* allocate new array */
  newarray = resizearray(L, t, oldasize, newasize);
  if (l_unlikely(newarray == NULL && newasize > 0))
  {                     /* allocation failed? */
    freehash(L, &newt); /* release new hash part */
    luaD_throw(L, LUA_ERRMEM);      /* raise error (with array unchanged) */
  }
  /* allocation ok; initialize new part of the array */
  exchangehashpart(t, &newt); /* 't' has the new hash ('newt' has the old) */
  t->array = newarray;        /* set new array part */
  t->asize = newasize;
  if (newarray != NULL)
    *lenhint(t) = newasize / 2u; /* set an initial hint */
  clearNewSlice(t, oldasize, newasize);
  /* re-insert elements from old hash part into new parts */
  reinserthash(L, &newt, t); /* 'newt' now has the old hash */
  freehash(L, &newt);        /* free old hash part */
}

void luaH_resizearray(lua_State* L, Table* t, unsigned int nasize)
{
  unsigned nsize = allocsizenode(t);
  luaH_resize(L, t, nasize, nsize);
}

/*
** Rehash a table. First, count its keys. If there are array indices
** outside the array part, compute the new best size for that part.
** Then, resize the table.
*/
static void rehash(lua_State* L, Table* t, const TValue* ek)
{
  unsigned asize; /* optimal size for array part */
  Counters ct;
  unsigned i;
  unsigned nsize; /* size for the hash part */
  /* reset counts */
  for (i = 0; i <= MAXABITS; i++)
    ct.nums[i] = 0;
  ct.na = 0;
  ct.deleted = 0;
  ct.total = 1; /* count extra key */
  if (((ek)->tt_ == LUA_VNUMINT))
    countint((lua_assert((ek)->tt_ == LUA_VNUMINT), ((ek)->value_).i), &ct); /* extra key may go to array */
  numusehash(t, &ct);          /* count keys in hash part */
  if (ct.na == 0)
  {
    /* no new keys to enter array part; keep it with the same size */
    asize = t->asize;
  }
  else
  {                            /* compute best size for array part */
    numusearray(t, &ct);       /* count keys in array part */
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
  luaH_resize(L, t, asize, nsize);
}

/*
** }=============================================================
*/

Table* luaH_new(lua_State* L)
{
  GCObject* o = luaC_newobj(L, LUA_VTABLE, sizeof(Table));
  Table* t = (lua_assert((o)->tt == LUA_VTABLE), static_cast<Table*>(o));
  t->metatable = NULL;
  t->flags = maskflags; /* table has no metamethod fields */
  t->array = NULL;
  t->asize = 0;
  setnodevector(L, t, 0);
  return t;
}

lu_mem luaH_size(Table* t)
{
  lu_mem sz = ((lu_mem)(sizeof(Table))) + concretesize(t->asize);
  if (!isdummy(t))
    sz += sizehash(t);
  return sz;
}

/*
** Frees a table.
*/
void luaH_free(lua_State* L, Table* t)
{
  freehash(L, t);
  resizearray(L, t, t->asize, 0);
  luaM_free_(L, (t), sizeof(*(t)));
}

static Node* getfreepos(Table* t)
{
  if (haslastfree(t))
  { /* does it have 'lastfree' information? */
    /* look for a spot before 'lastfree', updating 'lastfree' */
    while (getlastfree(t) > t->node)
    {
      Node* free = --getlastfree(t);
      if (((free)->key_tt == LUA_TNIL))
        return free;
    }
  }
  else
  { /* no 'lastfree' information */
    unsigned i = sizenode(t);
    while (i--)
    { /* do a linear search */
      Node* free = gnode(t, i);
      if (((free)->key_tt == LUA_TNIL))
        return free;
    }
  }
  return NULL; /* could not find a free place */
}

/*
** Inserts a new key into a hash table; first, check whether key's main
** position is free. If not, check whether colliding node is in its main
** position or not: if it is not, move colliding node to an empty place
** and put new key in its main position; otherwise (colliding node is in
** its main position), new key goes to an empty position. Return 0 if
** could not insert key (could not find a free space).
*/
static int insertkey(Table* t, const TValue* key, TValue* value)
{
  Node* mp = mainpositionTV(t, key);
  /* table cannot already contain the key */
  lua_assert(((getgeneric(t, key, 0))->tt_ == LUA_VABSTKEY));
  if (!((((&(mp)->val))->tt_ & NOVARIANT_MASK) == LUA_TNIL) || isdummy(t))
  { /* main position is taken? */
    Node* othern;
    Node* f = getfreepos(t); /* get a free place */
    if (f == NULL)           /* cannot find a free place? */
      return 0;
    lua_assert(!isdummy(t));
    othern = mainpositionfromnode(t, mp);
    if (othern != mp)
    { /* is colliding node out of its main position? */
      /* yes; move colliding node into free position */
      while (othern + ((othern)->next) != mp) /* find previous */
        othern += ((othern)->next);
      ((othern)->next) = ((int)(f - othern)); /* rechain to point to 'f' */
      *f = *mp;                             /* copy colliding node into free pos. (mp->next also goes) */
      if (((mp)->next) != 0)
      {
        ((f)->next) += ((int)(mp - f)); /* correct 'next' */
        ((mp)->next) = 0;                /* now 'mp' is free */
      }
      (((&(mp)->val))->tt_ = LUA_VEMPTY);
    }
    else
    { /* colliding node is in its own main position */
      /* new node will go into free position */
      if (((mp)->next) != 0)
        ((f)->next) = ((int)((mp + ((mp)->next)) - f)); /* chain new position */
      else
        lua_assert(((f)->next) == 0);
      ((mp)->next) = ((int)(f - mp));
      mp = f;
    }
  }
  setnodekey(mp, key);
  lua_assert(((((&(mp)->val))->tt_ & NOVARIANT_MASK) == LUA_TNIL));
  setobj(((lua_State*)(0)), (&(mp)->val), value);
  return 1;
}

/*
** Insert a key in a table where there is space for that key, the
** key is valid, and the value is not nil.
*/
static void newcheckedkey(Table* t, const TValue* key, TValue* value)
{
  unsigned i = keyinarray(t, key);
  if (i > 0)                  /* is key in the array part? */
    obj2arr(t, i - 1, value); /* set value in the array */
  else
  {
    int done = insertkey(t, key, value); /* insert key in the hash part */
    lua_assert(done);                    /* it cannot fail */
    ((void)(done));                    /* to avoid warnings */
  }
}

static void luaH_newkey(lua_State* L, Table* t, const TValue* key,
                        TValue* value)
{
  if (!(((value)->tt_ & NOVARIANT_MASK) == LUA_TNIL))
  { /* do not insert nil values */
    int done = insertkey(t, key, value);
    if (!done)
    {                               /* could not find a free place? */
      rehash(L, t, key);            /* grow table */
      newcheckedkey(t, key, value); /* insert key in grown table */
    }
    luaC_barrierback(L, (lua_assert((((t)->tt) & NOVARIANT_MASK) >= LUA_TSTRING), const_cast<GCObject*>(static_cast<const GCObject*>(t))), key);
    /* for debugging only: any new key may force an emergency collection */
    condchangemem(L, (void) 0, (void) 0, 1);
  }
}

static const TValue* getintfromhash(Table* t, lua_Integer key)
{
  Node* n = hashint(t, key);
  lua_assert(!ikeyinarray(t, key));
  for (;;)
  { /* check whether 'key' is somewhere in the chain */
    if (((n)->key_tt == LUA_VNUMINT) && (((n)->key_val).i) == key)
      return (&(n)->val); /* that's it */
    else
    {
      int nx = ((n)->next);
      if (nx == 0)
        break;
      n += nx;
    }
  }
  return &absentkey;
}

static int hashkeyisempty(Table* t, lua_Unsigned key)
{
  const TValue* val = getintfromhash(t, ((lua_Integer)(key)));
  return (((val)->tt_ & NOVARIANT_MASK) == LUA_TNIL);
}

static lu_byte finishnodeget(const TValue* val, TValue* res)
{
  if (!(((val)->tt_ & NOVARIANT_MASK) == LUA_TNIL))
  {
    setobj(((lua_State*) NULL), res, val);
  }
  return ((val)->tt_ & WITHVARIANT_MASK);
}

lu_byte luaH_getint(Table* t, lua_Integer key, TValue* res)
{
  unsigned k = ikeyinarray(t, key);
  if (k > 0)
  {
    lu_byte tag = *getArrTag(t, k - 1);
    if (!(((tag) & NOVARIANT_MASK) == LUA_TNIL))
      farr2val(t, k - 1, tag, res);
    return tag;
  }
  else
    return finishnodeget(getintfromhash(t, key), res);
}

/*
** search function for short strings
*/
const TValue* luaH_Hgetshortstr(Table* t, TString* key)
{
  Node* n = hashstr(t, key);
  for (;;)
  { /* check whether 'key' is somewhere in the chain */
    if (((n)->key_tt == ctb(LUA_VSHRSTR)) && luaS_eqstr(((lua_assert(((((n)->key_val).gc)->tt & NOVARIANT_MASK) == LUA_TSTRING), static_cast<TString*>(((n)->key_val).gc))), key))
      return (&(n)->val); /* that's it */
    else
    {
      int nx = ((n)->next);
      if (nx == 0)
        return &absentkey; /* not found */
      n += nx;
    }
  }
}

lu_byte luaH_getshortstr(Table* t, TString* key, TValue* res)
{
  return finishnodeget(luaH_Hgetshortstr(t, key), res);
}

static const TValue* Hgetstr(Table* t, TString* key)
{
  return luaH_Hgetshortstr(t, key);
}

lu_byte luaH_getstr(Table* t, TString* key, TValue* res)
{
  return finishnodeget(Hgetstr(t, key), res);
}

/*
** main search function
*/
lu_byte luaH_get(Table* t, const TValue* key, TValue* res)
{
  const TValue* slot;
  switch (((key)->tt_ & WITHVARIANT_MASK))
  {
  case LUA_VSHRSTR:
    slot = luaH_Hgetshortstr(t, (lua_assert(((key)->tt_ & NOVARIANT_MASK) == LUA_TSTRING), (lua_assert((((((key)->value_).gc)->tt) & NOVARIANT_MASK) == LUA_TSTRING), static_cast<TString*>(((key)->value_).gc))));
    break;
  case LUA_VNUMINT:
    return luaH_getint(t, (lua_assert((key)->tt_ == LUA_VNUMINT), ((key)->value_).i), res);
  case LUA_VNIL:
    slot = &absentkey;
    break;
  case LUA_VNUMFLT:
  {
    lua_Integer k;
    if (luaV_flttointeger((lua_assert((key)->tt_ == LUA_VNUMFLT), ((key)->value_).n), &k, F2Ieq)) /* integral index? */
      return luaH_getint(t, k, res);                 /* use specialized version */
    /* else... */
  } /* FALLTHROUGH */
  default:
    slot = getgeneric(t, key, 0);
    break;
  }
  return finishnodeget(slot, res);
}

/*
** When a 'pset' cannot be completed, this function returns an encoding
** of its result, to be used by 'luaH_finishset'.
*/
static int retpsetcode(Table* t, const TValue* slot)
{
  if (((slot)->tt_ == LUA_VABSTKEY))
    return HNOTFOUND; /* no slot with that key */
  else                /* return node encoded */
    return ((int)((((Node*)(slot)) - t->node))) + HFIRSTNODE;
}

static int finishnodeset(Table* t, const TValue* slot, TValue* val)
{
  if (!(((slot)->tt_ & NOVARIANT_MASK) == LUA_TNIL))
  {
    setobj(((lua_State*) NULL), ((TValue*)(slot)), val);
    return HOK; /* success */
  }
  else
    return retpsetcode(t, slot);
}

static int rawfinishnodeset(const TValue* slot, TValue* val)
{
  if (((slot)->tt_ == LUA_VABSTKEY))
    return 0; /* no slot with that key */
  else
  {
    setobj(((lua_State*) NULL), ((TValue*)(slot)), val);
    return 1; /* success */
  }
}

int luaH_psetint(Table* t, lua_Integer key, TValue* val)
{
  lua_assert(!ikeyinarray(t, key));
  return finishnodeset(t, getintfromhash(t, key), val);
}

static int psetint(Table* t, lua_Integer key, TValue* val)
{
  int hres;
  { Table* h_ = (t); lua_Unsigned u_ = ((lua_Unsigned)(key)) - 1u; if (u_ < h_->asize) { lu_byte* tag_ = getArrTag(h_, u_); if (checknoTM(h_->metatable, TM_NEWINDEX) || !(((*tag_) & NOVARIANT_MASK) == LUA_TNIL)) { fval2arr(h_, u_, tag_, val); (hres) = HOK; } else (hres) = ~((int)(u_)); } else { (hres) = luaH_psetint(h_, key, val); } };
  return hres;
}

/*
** This function could be just this:
**    return finishnodeset(t, luaH_Hgetshortstr(t, key), val);
** However, it optimizes the common case created by constructors (e.g.,
** {x=1, y=2}), which creates a key in a table that has no metatable,
** it is not old/black, and it already has space for the key.
*/

int luaH_psetshortstr(Table* t, TString* key, TValue* val)
{
  const TValue* slot = luaH_Hgetshortstr(t, key);
  if (!(((slot)->tt_ & NOVARIANT_MASK) == LUA_TNIL))
  {                                                        /* key already has a value? (all too common) */
    setobj(((lua_State*) NULL), ((TValue*)(slot)), val); /* update it */
    return HOK;                                            /* done */
  }
  else if (checknoTM(t->metatable, TM_NEWINDEX))
  {                        /* no metamethod? */
    if ((((val)->tt_ & NOVARIANT_MASK) == LUA_TNIL))      /* new value is nil? */
      return HOK;          /* done (value is already nil/absent) */
    if (((slot)->tt_ == LUA_VABSTKEY) && /* key is absent? */
        !(isblack(t) && iswhite(key)))
    {            /* and don't need barrier? */
      TValue tk; /* key as a TValue */
      setsvalue(((lua_State*)(NULL)), &tk, key);
      if (insertkey(t, &tk, val))
      { /* insert key, if there is space */
        invalidateTMcache(t);
        return HOK;
      }
    }
  }
  /* Else, either table has new-index metamethod, or it needs barrier,
     or it needs to rehash for the new key. In any of these cases, the
     operation cannot be completed here. Return a code for the caller. */
  return retpsetcode(t, slot);
}

int luaH_psetstr(Table* t, TString* key, TValue* val)
{
  return luaH_psetshortstr(t, key, val);
}

int luaH_pset(Table* t, const TValue* key, TValue* val)
{
  switch (((key)->tt_ & WITHVARIANT_MASK))
  {
  case LUA_VSHRSTR:
    return luaH_psetshortstr(t, (lua_assert(((key)->tt_ & NOVARIANT_MASK) == LUA_TSTRING), (lua_assert((((((key)->value_).gc)->tt) & NOVARIANT_MASK) == LUA_TSTRING), static_cast<TString*>(((key)->value_).gc))), val);
  case LUA_VNUMINT:
    return psetint(t, (lua_assert((key)->tt_ == LUA_VNUMINT), ((key)->value_).i), val);
  case LUA_VNIL:
    return HNOTFOUND;
  case LUA_VNUMFLT:
  {
    lua_Integer k;
    if (luaV_flttointeger((lua_assert((key)->tt_ == LUA_VNUMFLT), ((key)->value_).n), &k, F2Ieq)) /* integral index? */
      return psetint(t, k, val);                     /* use specialized version */
    /* else... */
  } /* FALLTHROUGH */
  default:
    return finishnodeset(t, getgeneric(t, key, 0), val);
  }
}

/*
** Finish a raw "set table" operation, where 'hres' encodes where the
** value should have been (the result of a previous 'pset' operation).
** Beware: when using this function the caller probably need to check a
** GC barrier and invalidate the TM cache.
*/
void luaH_finishset(lua_State* L, Table* t, const TValue* key,
                    TValue* value, int hres)
{
  lua_assert(hres != HOK);
  if (hres == HNOTFOUND)
  {
    TValue aux;
    const TValue* actk = key; /* actual key to insert */
    if (l_unlikely((((key)->tt_ & NOVARIANT_MASK) == LUA_TNIL)))
      luaG_runerror(L, "table index is nil");
    else if (((key)->tt_ == LUA_VNUMFLT))
    {
      lua_Number f = (lua_assert((key)->tt_ == LUA_VNUMFLT), ((key)->value_).n);
      lua_Integer k;
      if (luaV_flttointeger(f, &k, F2Ieq))
      { /* is key equal to an integer? */
        setivalue(&aux, k);
        actk = &aux; /* use the integer as the key */
      }
      else if (l_unlikely((!((f) == (f)))))
        luaG_runerror(L, "table index is NaN");
    }
    luaH_newkey(L, t, actk, value);
  }
  else if (hres > 0)
  { /* regular Node? */
    setobj(L, (&(gnode(t, hres - HFIRSTNODE))->val), value);
  }
  else
  {               /* array entry */
    hres = ~hres; /* real index */
    obj2arr(t, ((unsigned int)(hres)), value);
  }
}

/*
** beware: when using this function you probably need to check a GC
** barrier and invalidate the TM cache.
*/
void luaH_set(lua_State* L, Table* t, const TValue* key, TValue* value)
{
  int hres = luaH_pset(t, key, value);
  if (hres != HOK)
    luaH_finishset(L, t, key, value, hres);
}

/*
** Ditto for a GC barrier. (No need to invalidate the TM cache, as
** integers cannot be keys to metamethods.)
*/
void luaH_setint(lua_State* L, Table* t, lua_Integer key, TValue* value)
{
  unsigned ik = ikeyinarray(t, key);
  if (ik > 0)
    obj2arr(t, ik - 1, value);
  else
  {
    int ok = rawfinishnodeset(getintfromhash(t, key), value);
    if (!ok)
    {
      TValue k;
      setivalue(&k, key);
      luaH_newkey(L, t, &k, value);
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
static lua_Unsigned hash_search(lua_State* L, Table* t, unsigned asize)
{
  lua_Unsigned i = asize + 1; /* caller ensures t[i] is present */
  unsigned rnd = G(L)->seed;
  int n = (asize > 0) ? luaO_ceillog2(asize) : 0; /* width of 'asize' */
  unsigned mask = (1u << n) - 1;                  /* 11...111 with the width of 'asize' */
  unsigned incr = (rnd & mask) + 1;               /* first increment (at least 1) */
  lua_Unsigned j = (incr <= ((lua_Unsigned)(LUA_MAXINTEGER)) - i) ? i + incr : i + 1;
  rnd >>= n; /* used 'n' bits from 'rnd' */
  while (!hashkeyisempty(t, j))
  {        /* repeat until an absent t[j] */
    i = j; /* 'i' is a present index */
    if (j <= ((lua_Unsigned)(LUA_MAXINTEGER)) / 2 - 1)
    {
      j = j * 2 + (rnd & 1); /* try again with 2j or 2j+1 */
      rnd >>= 1;
    }
    else
    {
      j = LUA_MAXINTEGER;
      if (hashkeyisempty(t, j)) /* t[j] not present? */
        break;                  /* 'j' now is an absent index */
      else                      /* weird case */
        return j;               /* well, max integer is a boundary... */
    }
  }
  /* i < j  &&  t[i] present  &&  t[j] absent */
  while (j - i > 1u)
  { /* do a binary search between them */
    lua_Unsigned m = (i + j) / 2;
    if (hashkeyisempty(t, m))
      j = m;
    else
      i = m;
  }
  return i;
}

static unsigned int binsearch(Table* array, unsigned int i, unsigned int j)
{
  lua_assert(i <= j);
  while (j - i > 1u)
  { /* binary search */
    unsigned int m = (i + j) / 2;
    if (arraykeyisempty(array, m))
      j = m;
    else
      i = m;
  }
  return i;
}

/* return a border, saving it as a hint for next call */
static lua_Unsigned newhint(Table* t, unsigned hint)
{
  lua_assert(hint <= t->asize);
  *lenhint(t) = hint;
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
lua_Unsigned luaH_getn(lua_State* L, Table* t)
{
  unsigned asize = t->asize;
  if (asize > 0)
  { /* is there an array part? */
    const unsigned maxvicinity = 4;
    unsigned limit = *lenhint(t); /* start with the hint */
    if (limit == 0)
      limit = 1; /* make limit a valid index in the array */
    if (arraykeyisempty(t, limit))
    { /* t[limit] empty? */
      /* there must be a border before 'limit' */
      unsigned i;
      /* look for a border in the vicinity of the hint */
      for (i = 0; i < maxvicinity && limit > 1; i++)
      {
        limit--;
        if (!arraykeyisempty(t, limit))
          return newhint(t, limit); /* 'limit' is a border */
      }
      /* t[limit] still empty; search for a border in [0, limit) */
      return newhint(t, binsearch(t, 0, limit));
    }
    else
    { /* 'limit' is present in table; look for a border after it */
      unsigned i;
      /* look for a border in the vicinity of the hint */
      for (i = 0; i < maxvicinity && limit < asize; i++)
      {
        limit++;
        if (arraykeyisempty(t, limit))
          return newhint(t, limit - 1); /* 'limit - 1' is a border */
      }
      if (arraykeyisempty(t, asize))
      { /* last element empty? */
        /* t[limit] not empty; search for a border in [limit, asize) */
        return newhint(t, binsearch(t, limit, asize));
      }
    }
    /* last element non empty; set a hint to speed up finding that again */
    /* (keys in the hash part cannot be hints) */
    *lenhint(t) = asize;
  }
  /* no array part or t[asize] is not empty; check the hash part */
  lua_assert(asize == 0 || !arraykeyisempty(t, asize));
  if (isdummy(t) || hashkeyisempty(t, asize + 1))
    return asize; /* 'asize + 1' is empty */
  else            /* 'asize + 1' is also non empty */
    return hash_search(L, t, asize);
}

#if defined(LUA_DEBUG)

/* export this function for the test library */

Node* luaH_mainposition(const Table* t, const TValue* key)
{
  return mainpositionTV(t, key);
}

#endif
