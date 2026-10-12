package lua.object;

import static lua.api.LuaApi.*;
import static lua.api.LuaConf.*;
import static lua.gc.Lgc.*;
import static lua.object.Llimits.*;
import static lua.object.Lobject.*;
import static lua.object.Lstring.*;
import static lua.object.Ltable.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lmem.*;
import static lua.runtime.Lstate.*;
import static lua.runtime.Ltm.*;
import static lua.vm.Ldebug.*;
import static lua.vm.Lvm.*;

import lua.api.lua_CFunction;
import lua.bytes.Lt;
import lua.runtime.Cost;
import lua.runtime.lua_State;

public final class Table extends GCListed
{
  public byte flags;                      /* 1<<p means tagmethod(p) is not present */
  public long[] array_prim = new long[0]; /* array part: values, forward-indexed */
  public Object[] array_ref = new Object[0];
  public byte[] tags = new byte[0];       /* array part: tags, forward-indexed */
  public Node[] node = new Node[0];
  public int lastfree = 0;
  public int lenhint = 0; /* hint for #t; see luaH_getn */
  public Table metatable = null;

  private static final class ArrayPart
  {
    final long[] prim;
    final Object[] ref;
    final byte[] tags;

    ArrayPart(long[] prim, Object[] ref, byte[] tags)
    {
      this.prim = prim;
      this.ref = ref;
      this.tags = tags;
    }
  }

  private int haslastfree()
  {
    return (sizenode(this) >= twoto(LIMFORLAST)) ? 1 : 0;
  }

  public int asize()
  {
    return array_prim.length;
  }

  /*
  ** When the original hash value is good, hashing by a power of 2
  ** avoids the cost of '%'.
  */
  private int hashpow2(long n)
  {
    return lmod(n, sizenode(this));
  }

  /*
  ** for other types, it is better to avoid modulo by power of 2, as
  ** they can have many 2 factors.
  */
  private int hashmod(long n)
  {
    return (int) l_umod(n, (sizenode(this) - 1) | 1);
  }

  private int hashstr(TString str)
  {
    return hashpow2(Integer.toUnsignedLong(luaS_gethash(str)));
  }

  private int hashboolean(int p)
  {
    return hashpow2(p);
  }

  private int hashpointer(Object p)
  {
    return hashmod(Integer.toUnsignedLong(point2uint(p)));
  }

  private int hashpointer(lua_CFunction p)
  {
    return hashmod(Integer.toUnsignedLong(point2uint(p)));
  }

  /*
  ** Hash for integers. To allow a good hash, use the remainder operator
  ** ('%'). If integer fits as a non-negative int, compute an int
  ** remainder, which is faster. Otherwise, use an unsigned-integer
  ** remainder, which uses all bits and ensures a non-negative result.
  */
  private int hashint(long i)
  {
    Table t = this;
    long ui = i;
    if (Long.compareUnsigned(ui, Integer.MAX_VALUE) <= 0)
      return (int) ui % ((sizenode(t) - 1) | 1);
    else
      return t.hashmod(ui);
  }

  /*
  ** returns the 'main' position of an element in a table (that is,
  ** the index of its hash value).
  */
  private int mainpositionTV(TValue key)
  {
    Table t = this;
    switch (ttypetag(key))
    {
    case LUA_VNUMINT:
    {
      long i = ivalue(key);
      return t.hashint(i);
    }
    case LUA_VNUMFLT:
    {
      double n = fltvalue(key);
      return t.hashmod(Integer.toUnsignedLong(l_hashfloat(n)));
    }
    case LUA_VSHRSTR:
    {
      assert ttisstring(key);
      TString ts = tsvalue(key);
      return t.hashstr(ts);
    }
    case LUA_VFALSE:
      return t.hashboolean(0);
    case LUA_VTRUE:
      return t.hashboolean(1);
    case LUA_VLIGHTUSERDATA:
    {
      Object p = pvalue(key);
      return t.hashpointer(p);
    }
    case LUA_VLCF:
    { /* an engine function by its name (the same in every process); another by address */
      GCObject o = gcvalue(key);
      return (o.hash != 0) ? t.hashmod(Integer.toUnsignedLong(o.hash)) : t.hashpointer(o);
    }
    default:
    {
      /* an object hashes by the hash it was given when created, not by its
         address: the same program with the same seed puts it in the same
         slot, and a copy of an object can take its hash */
      assert iscollectable(key);
      GCObject o = gcvalue(key);
      return t.hashmod(Integer.toUnsignedLong(o.hash));
    }
    }
  }

  private int mainpositionfromnode(int nd)
  {
    TValue key = new TValue();
    getnodekey(null, key, gnode(this, nd));
    return mainpositionTV(key);
  }

  private int nodegeneric(TValue key, int deadok)
  {
    Table t = this;
    int i = t.mainpositionTV(key);
    for (;;)
    { /* check whether 'key' is somewhere in the chain */
      Node n = gnode(t, i);
      if (equalkey(key, n, deadok) != 0)
        return i; /* that's it */
      else
      {
        int nx = n.next;
        if (nx == 0)
          return -1; /* not found */
        i += nx;
      }
    }
  }

  private TValue getgeneric(TValue key, int deadok)
  {
    Table t = this;
    int idx = t.nodegeneric(key, deadok);
    return (idx < 0) ? absentkey : gnode(t, idx);
  }

  /*
  ** Check whether an integer key is in the array part of a table and
  ** return its index there, or zero.
  */
  private int ikeyinarray(long k)
  {
    return checkrange(k, asize());
  }

  /*
  ** Check whether a key is in the array part of a table and return its
  ** index there, or zero.
  */
  private int keyinarray(TValue key)
  {
    return ttisinteger(key) ? ikeyinarray(ivalue(key)) : 0;
  }

  /*
  ** returns the index of a 'key' for table traversals. First goes all
  ** elements in the array part, then elements in the hash part. The
  ** beginning of a traversal is signaled by 0.
  */
  private long findindex(lua_State L, TValue key, int asize)
  {
    Table t = this;
    int i;
    if (isempty(key))
      return 0; /* first iteration */
    i = t.keyinarray(key);
    if (i != 0) /* is 'key' inside array part? */
      return i; /* yes; that's the index */
    else
    {
      int idx = t.nodegeneric(key, 1);
      if (idx < 0)
        luaG_runerror(L, Lt.invalid_key_to_next); /* key not found */
      i = idx; /* key index in hash table */
      /* hash elements are numbered after array ones */
      return ((long) i + 1) + asize;
    }
  }

  public int luaH_next(lua_State L, int key)
  {
    Table t = this;
    int asize = t.asize();
    TValue keyv = getslot(L, key);
    long i = t.findindex(L, keyv, asize); /* find original key */
    for (; i < asize; i++)
    { /* try first array part */
      byte tag = getArrTag(t, i);
      if (!tagisempty(tag))
      { /* a non-empty entry? */
        setsloti(L, key, i + 1);
        TValue res = new TValue();
        farr2val(t, i, tag, res);
        setslot(L, key + 1, res);
        return 1;
      }
    }
    for (i -= asize; i < sizenode(t); i++)
    { /* hash part */
      if (!isempty(gnode(t, (int) i)))
      { /* a non-empty entry? */
        Node n = gnode(t, (int) i);
        TValue nk = new TValue();
        getnodekey(L, nk, n);
        setslot(L, key, nk);
        setslot(L, key + 1, n);
        return 1;
      }
    }
    return 0; /* no more elements */
  }

  /* 'node' size in bytes */
  private long sizehash()
  {
    assert Long.MAX_VALUE / Cost.Node >= MAXHSIZE;
    Table t = this;
    return (long) sizenode(t) * Cost.Node;
  }

  private void freehash(lua_State L)
  {
    Table t = this;
    if (isdummy(t) == 0)
    {
      luaM_alloc_Node_span(L.l_G, L, t.node, 0, false);
    }
  }

  /*
  ** {=============================================================
  ** Rehash
  ** ==============================================================
  */

  private int arraykeyisempty(int key)
  {
    int tag = getArrTag(this, key - 1);
    return tagisempty(tag) ? 1 : 0;
  }

  /*
  ** Count keys in array part of table 't'.
  */
  private void numusearray(Counters ct)
  {
    Table t = this;
    int lg;
    int ttlg;     /* 2^lg */
    int ause = 0; /* summation of 'nums' */
    int i = 1;    /* index to traverse all array keys */
    int asize = t.asize();
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
        if (t.arraykeyisempty(i) == 0)
          lc++;
      }
      ct.nums[lg] += lc;
      ause += lc;
    }
    ct.total += ause;
    ct.na += ause;
  }

  /*
  ** Count keys in hash part of table 't'. As this only happens during
  ** a rehash, all nodes have been used. A node can have a nil value only
  ** if it was deleted after being created.
  */
  private void numusehash(Counters ct)
  {
    Table t = this;
    int i = sizenode(t);
    int total = 0;
    while (i-- != 0)
    {
      Node n = gnode(t, i);
      if (isempty(n))
      {
        assert !keyisnil(n); /* entry was deleted; key cannot be nil */
        ct.deleted = 1;
      }
      else
      {
        total++;
        if (keyisinteger(n))
          countint(keyival(n), ct);
      }
    }
    ct.total += total;
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
  private ArrayPart resizearray(lua_State L,
                                int oldasize,
                                int newasize)
  {
    Table t = this;
    if (oldasize == newasize)
    {
      return new ArrayPart(t.array_prim, t.array_ref, t.tags); /* nothing to be done */
    }
    else if (newasize == 0)
    { /* erasing array? */
      luaM_alloc_Value_span(L.l_G, L, t.array_prim, 0, false);
      luaM_alloc_lu_byte_span(L.l_G, L, t.tags, 0, false);
      return new ArrayPart(new long[0], new Object[0], new byte[0]);
    }
    else
    {
      long[] nv_prim = luaM_alloc_Value_span(L.l_G, L, new long[0], newasize, false);
      if (nv_prim == null) /* allocation error? */
      {
        return new ArrayPart(new long[0], new Object[0], new byte[0]);
      }
      Object[] nv_ref = luaM_alloc_Value_span(new Object[0], newasize);
      byte[] nt = luaM_alloc_lu_byte_span(L.l_G, L, new byte[0], newasize, false);
      if (nt == null) /* allocation error? */
      {
        luaM_alloc_Value_span(L.l_G, L, nv_prim, 0, false);
        return new ArrayPart(new long[0], new Object[0], new byte[0]);
      }
      if (oldasize > 0)
      {
        /* move common elements to new arrays */
        int tomove = (oldasize < newasize) ? oldasize : newasize;
        for (int mi = 0; mi < tomove; mi++)
        {
          nv_prim[mi] = t.array_prim[mi];
          nv_ref[mi] = t.array_ref[mi];
        }
        for (int mi = 0; mi < tomove; mi++)
          nt[mi] = t.tags[mi];
        luaM_alloc_Value_span(L.l_G, L, t.array_prim, 0, false);
        luaM_alloc_lu_byte_span(L.l_G, L, t.tags, 0, false);
      }
      return new ArrayPart(nv_prim, nv_ref, nt);
    }
  }

  /*
  ** Creates an array for the hash part of a table with the given
  ** size, or reuses the dummy node if size is zero.
  ** The computation for size overflow is in two steps: the first
  ** comparison ensures that the shift in the second one does not
  ** overflow.
  */
  private void setnodevector(lua_State L, int size)
  {
    Table t = this;
    if (size == 0)
    {                                   /* no elements to hash part? */
      t.node = new Node[] { dummynode_ }; /* use common 'dummynode' */
      t.lastfree = 0;
      setdummy(t); /* signal that it is using dummy node */
    }
    else
    {
      int i;
      int lsize = luaO_ceillog2(size);
      if (lsize > MAXHBITS || (1 << lsize) > MAXHSIZE)
        luaG_runerror(L, Lt.table_overflow);
      size = twoto(lsize);
      t.node = luaM_alloc_Node_span(L.l_G, L, new Node[0], size, true);
      t.lastfree = 0;
      if (lsize >= LIMFORLAST)
      {
        t.lastfree = size; /* all positions are free */
      }
      setnodummy(t);
      for (i = 0; i < size; i++)
      {
        Node n = gnode(t, i);
        n.next = 0;
        setnilkey(n);
        setempty(n);
      }
    }
  }

  /*
  ** Re-insert into the new hash part of a table the elements from the
  ** vanishing slice of the array part.
  */
  private void reinsertOldSlice(int oldasize,
                                int newasize)
  {
    Table t = this;
    int i;
    for (i = newasize; i < oldasize; i++)
    { /* traverse vanishing slice */
      byte tag = getArrTag(t, i);
      if (!tagisempty(tag))
      { /* a non-empty entry? */
        TValue key = new TValue(), aux = new TValue();
        setivalue(key, (long) i + 1); /* make the key */
        farr2val(t, i, tag, aux);         /* copy value into 'aux' */
        t.insertkey(key, aux);          /* insert entry into the hash part */
      }
    }
  }

  /*
  ** Clear new slice of the array.
  */
  private void clearNewSlice(int oldasize, int newasize)
  {
    Table t = this;
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
  public void luaH_resize(lua_State L, int newasize,
                          int nhsize)
  {
    Table t = this;
    Table newt = new Table(); /* to keep the new hash part */
    int oldasize = t.asize();
    ArrayPart newarray;
    if (newasize > MAXASIZE)
      luaG_runerror(L, Lt.table_overflow);
    /* create new hash part with appropriate size into 'newt' */
    newt.flags = 0;
    newt.setnodevector(L, nhsize);
    if (newasize < oldasize)
    { /* will array shrink? */
      /* re-insert into the new hash the elements from vanishing slice */
      exchangehashpart(t, newt); /* pretend table has new hash */
      t.reinsertOldSlice(oldasize, newasize);
      exchangehashpart(t, newt); /* restore old hash (in case of errors) */
    }
    /* allocate new array */
    newarray = t.resizearray(L, oldasize, newasize);
    if (newarray.prim.length == 0 && newasize > 0)
    {                     /* allocation failed? */
      newt.freehash(L); /* release new hash part */
      luaD_throw(L, (byte) LUA_ERRMEM);      /* raise error (with array unchanged) */
    }
    /* allocation ok; initialize new part of the array */
    exchangehashpart(t, newt); /* 't' has the new hash ('newt' has the old) */
    t.array_prim = newarray.prim;        /* set new array part */
    t.array_ref = newarray.ref;
    t.tags = newarray.tags;
    if (newarray.prim.length != 0)
      t.lenhint = newasize / 2; /* set an initial hint */
    t.clearNewSlice(oldasize, newasize);
    /* re-insert elements from old hash part into new parts */
    reinserthash(L, newt, t); /* 'newt' now has the old hash */
    newt.freehash(L);        /* free old hash part */
  }

  public void luaH_resizearray(lua_State L, int nasize)
  {
    Table t = this;
    int nsize = allocsizenode(t);
    t.luaH_resize(L, nasize, nsize);
  }

  /*
  ** Rehash a table. First, count its keys. If there are array indices
  ** outside the array part, compute the new best size for that part.
  ** Then, resize the table.
  */
  private void rehash(lua_State L, TValue ek)
  {
    Table t = this;
    int asize; /* optimal size for array part */
    Counters ct = new Counters();
    int i;
    int nsize; /* size for the hash part */
    /* reset counts */
    for (i = 0; i <= MAXABITS; i++)
      ct.nums[i] = 0;
    ct.na = 0;
    ct.deleted = 0;
    ct.total = 1; /* count extra key */
    if (ttisinteger(ek))
      countint(ivalue(ek), ct); /* extra key may go to array */
    t.numusehash(ct);          /* count keys in hash part */
    if (ct.na == 0)
    {
      /* no new keys to enter array part; keep it with the same size */
      asize = t.asize();
    }
    else
    {                            /* compute best size for array part */
      t.numusearray(ct);       /* count keys in array part */
      asize = computesizes(ct); /* compute new size for array part */
    }
    /* all keys not in the array part go to the hash part */
    nsize = ct.total - ct.na;
    if (ct.deleted != 0)
    { /* table has deleted entries? */
      /* insertion-deletion-insertion: give hash some extra size to
         avoid repeated resizings */
      nsize += nsize >> 2;
    }
    /* resize the table to new computed sizes */
    t.luaH_resize(L, asize, nsize);
  }

  /*
  ** }=============================================================
  */

  public static Table luaH_new(lua_State L)
  {
    Table t = luaM_malloc_Table(L);
    luaC_link(L, t, LUA_VTABLE);
    t.flags = maskflags; /* table has no metamethod fields */
    t.setnodevector(L, 0);
    return t;
  }

  public long luaH_size()
  {
    Table t = this;
    long sz = Cost.Table
              + (long) t.array_prim.length * (Cost.Value + Cost.Byte);
    if (isdummy(t) == 0)
      sz += t.sizehash();
    return sz;
  }

  /*
  ** Frees a table.
  */
  public void luaH_free(lua_State L)
  {
    Table t = this;
    t.freehash(L);
    t.resizearray(L, t.asize(), 0);
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
  public void luaH_setnode(lua_State L, int i, TValue key, boolean dead,
                           TValue val, int link)
  {
    Table t = this;
    assert isdummy(t) == 0 && 0 <= i && i < sizenode(t);
    assert 0 <= i + link && i + link < sizenode(t);
    Node n = gnode(t, i);
    n.next = link;
    if (dead)
    {
      assert key == null || iscollectable(key) || key.tt_ == LUA_TDEADKEY;
      setdeadkey(n);
      n.key_prim = (key != null) ? key.prim : 0;
      n.key_ref = (key != null) ? key.ref : null;
    }
    else if (key == null)
      setnilkey(n);
    else
    {
      setnodekey(n, key);
      luaC_barrierback(L, t, key);
    }
    if (val == null || ttisnil(val))
      setempty(n);
    else
    {
      setobj(null, n, val);
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
  public void luaH_copyfrom(lua_State L, Table src)
  {
    Table t = this;
    t.luaH_resize(L, src.asize(), allocsizenode(src));
    for (int i = 0; i < src.asize(); i++)
    {
      TValue v = new TValue();
      byte tag = getArrTag(src, i);
      if (tagisempty(tag))
        continue;
      farr2val(src, i, tag, v);
      obj2arr(t, i, v);
      luaC_barrierback(L, t, v);
    }
    if (isdummy(src) == 0)
    {
      for (int i = 0; i < sizenode(src); i++)
      {
        Node n = gnode(src, i);
        TValue key = new TValue(); /* a live key, or a dead key's identity */
        boolean hasKey = !keyisnil(n);
        if (hasKey)
          getnodekey(L, key, n);
        t.luaH_setnode(L, i, hasKey ? key : null, keyisdead(n), n, n.next);
      }
      t.lastfree = src.lastfree;
    }
    t.lenhint = src.lenhint;
    t.metatable = src.metatable;
    if (t.metatable != null)
      luaC_objbarrier(L, t, t.metatable);
    invalidateTMcache(t);
  }

  private int getfreepos()
  {
    Table t = this;
    if (t.haslastfree() != 0)
    { /* does it have 'lastfree' information? */
      /* look for a spot before 'lastfree', updating 'lastfree' */
      while (t.lastfree > 0)
      {
        int free = --t.lastfree;
        if (keyisnil(gnode(t, free)))
          return free;
      }
    }
    else
    { /* no 'lastfree' information */
      int i = sizenode(t);
      while (i-- != 0)
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
  private int insertkey(TValue key, TValue value)
  {
    Table t = this;
    int mp = t.mainpositionTV(key);
    /* table cannot already contain the key */
    assert t.nodegeneric(key, 0) < 0;
    if (!isempty(gnode(t, mp)) || isdummy(t) != 0)
    { /* main position is taken? */
      int f = t.getfreepos(); /* get a free place */
      if (f < 0)             /* cannot find a free place? */
        return 0;
      assert isdummy(t) == 0;
      int othern = t.mainpositionfromnode(mp);
      if (othern != mp)
      { /* is colliding node out of its main position? */
        /* yes; move colliding node into free position */
        while (othern + (gnode(t, othern).next) != mp) /* find previous */
          othern += gnode(t, othern).next;
        gnode(t, othern).next = f - othern; /* rechain to point to 'f' */
        copynode(gnode(t, f), gnode(t, mp));          /* copy colliding node into free pos. (mp->next also goes) */
        if ((gnode(t, mp).next) != 0)
        {
          gnode(t, f).next += mp - f; /* correct 'next' */
          gnode(t, mp).next = 0;      /* now 'mp' is free */
        }
        setempty(gnode(t, mp));
      }
      else
      { /* colliding node is in its own main position */
        /* new node will go into free position */
        if ((gnode(t, mp).next) != 0)
          (gnode(t, f).next) = (mp + gnode(t, mp).next) - f; /* chain new position */
        else
          assert gnode(t, f).next == 0;
        gnode(t, mp).next = f - mp;
        mp = f;
      }
    }
    setnodekey(gnode(t, mp), key);
    assert isempty(gnode(t, mp));
    setobj(null, gnode(t, mp), value);
    return 1;
  }

  /*
  ** Insert a key in a table where there is space for that key, the
  ** key is valid, and the value is not nil.
  */
  void newcheckedkey(TValue key, TValue value)
  {
    Table t = this;
    int i = t.keyinarray(key);
    if (i > 0)                  /* is key in the array part? */
      obj2arr(t, i - 1, value); /* set value in the array */
    else
    {
      int done = t.insertkey(key, value); /* insert key in the hash part */
      assert done != 0;                    /* it cannot fail */
    }
  }

  private void luaH_newkey(lua_State L, TValue key,
                           TValue value)
  {
    Table t = this;
    if (!isempty(value))
    { /* do not insert nil values */
      int done = t.insertkey(key, value);
      if (done == 0)
      {                               /* could not find a free place? */
        t.rehash(L, key);            /* grow table */
        t.newcheckedkey(key, value); /* insert key in grown table */
      }
      assert novariant(t.tt) >= LUA_TSTRING;
      luaC_barrierback(L, t, key);
    }
  }

  private int nodeintfromhash(long key)
  {
    Table t = this;
    int i = t.hashint(key);
    assert t.ikeyinarray(key) == 0;
    for (;;)
    { /* check whether 'key' is somewhere in the chain */
      Node n = gnode(t, i);
      if (keyisinteger(n) && keyival(n) == key)
        return i; /* that's it */
      else
      {
        int nx = n.next;
        if (nx == 0)
          break;
        i += nx;
      }
    }
    return -1;
  }

  private TValue getintfromhash(long key)
  {
    Table t = this;
    int idx = t.nodeintfromhash(key);
    return (idx < 0) ? absentkey : gnode(t, idx);
  }

  private int hashkeyisempty(long key)
  {
    Table t = this;
    TValue val = t.getintfromhash(key);
    return isempty(val) ? 1 : 0;
  }

  public byte luaH_getint(long key, TValue res)
  {
    Table t = this;
    int k = t.ikeyinarray(key);
    if (k > 0)
    {
      byte tag = getArrTag(t, k - 1);
      if (!tagisempty(tag))
        farr2val(t, k - 1, tag, res);
      return tag;
    }
    else
      return finishnodeget(t.getintfromhash(key), res);
  }

  private int nodeshortstr(TString key)
  {
    Table t = this;
    int i = t.hashstr(key);
    for (;;)
    { /* check whether 'key' is somewhere in the chain */
      Node n = gnode(t, i);
      if (keyisshrstr(n))
      {
        if (luaS_eqstr(keystrval(n), key) != 0)
          return i; /* that's it */
      }
      int nx = n.next;
      if (nx == 0)
        return -1; /* not found */
      i += nx;
    }
  }

  public TValue luaH_Hgetshortstr(TString key)
  {
    Table t = this;
    int idx = t.nodeshortstr(key);
    return (idx < 0) ? absentkey : gnode(t, idx);
  }

  public byte luaH_getshortstr(TString key, TValue res)
  {
    Table t = this;
    return finishnodeget(t.luaH_Hgetshortstr(key), res);
  }

  private TValue Hgetstr(TString key)
  {
    Table t = this;
    return t.luaH_Hgetshortstr(key);
  }

  public byte luaH_getstr(TString key, TValue res)
  {
    Table t = this;
    return finishnodeget(t.Hgetstr(key), res);
  }

  /*
  ** main search function
  */
  public byte luaH_get(TValue key, TValue res)
  {
    Table t = this;
    TValue slot;
    switch (ttypetag(key))
    {
    case LUA_VSHRSTR:
      slot = t.luaH_Hgetshortstr(tsvalue(key));
      break;
    case LUA_VNUMINT:
      return t.luaH_getint(ivalue(key), res);
    case LUA_VNIL:
      slot = absentkey;
      break;
    case LUA_VNUMFLT:
    {
      IntResult k = new IntResult();
      if (luaV_flttointeger(fltvalue(key), k, F2Ieq) != 0) /* integral index? */
        return t.luaH_getint(k.v, res);                 /* use specialized version */
      /* else... */
    } /* FALLTHROUGH */
    default:
      slot = t.getgeneric(key, 0);
      break;
    }
    return finishnodeget(slot, res);
  }

  private int finishnodeset(int idx, TValue val)
  {
    Table t = this;
    if (idx >= 0 && !isempty(gnode(t, idx)))
    {
      setobj(null, gnode(t, idx), val);
      return HOK; /* success */
    }
    else
      return retpsetcode(idx);
  }

  private int rawfinishnodeset(int idx, TValue val)
  {
    if (idx < 0)
      return 0; /* no slot with that key */
    else
    {
      Table t = this;
      setobj(null, gnode(t, idx), val);
      return 1; /* success */
    }
  }

  public int luaH_psetint(long key, TValue val)
  {
    Table t = this;
    assert t.ikeyinarray(key) == 0;
    return t.finishnodeset(t.nodeintfromhash(key), val);
  }

  private int psetint(long key, TValue val)
  {
    Table t = this;
    int hres;
    long u = l_intsub(key, 1);
    if (l_ult(u, t.asize()))
    {
      byte tag = getArrTag(t, u);
      if (checknoTM(t.metatable, TM_NEWINDEX) != 0 || !tagisempty(tag))
      {
        fval2arr(t, u, val);
        hres = HOK;
      }
      else
        hres = ~(int) u;
    }
    else
      hres = t.luaH_psetint(key, val);
    return hres;
  }

  /*
  ** This function could be just this:
  **    return finishnodeset(t, luaH_Hgetshortstr(t, key), val);
  ** However, it optimizes the common case created by constructors (e.g.,
  ** {x=1, y=2}), which creates a key in a table that has no metatable,
  ** it is not old/black, and it already has space for the key.
  */

  public int luaH_psetshortstr(TString key, TValue val)
  {
    Table t = this;
    int idx = t.nodeshortstr(key);
    if (idx >= 0 && !isempty(gnode(t, idx)))
    {                                                        /* key already has a value? (all too common) */
      setobj(null, gnode(t, idx), val); /* update it */
      return HOK;                                            /* done */
    }
    else if (checknoTM(t.metatable, TM_NEWINDEX) != 0)
    {                        /* no metamethod? */
      if (isempty(val))      /* new value is nil? */
        return HOK;          /* done (value is already nil/absent) */
      if (idx < 0 && /* key is absent? */
          !(isblack(t) != 0 && iswhite(key) != 0))
      {            /* and don't need barrier? */
        TValue tk = new TValue(); /* key as a TValue */
        setsvalue(null, tk, key);
        if (t.insertkey(tk, val) != 0)
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

  public int luaH_psetstr(TString key, TValue val)
  {
    Table t = this;
    return t.luaH_psetshortstr(key, val);
  }

  public int luaH_pset(TValue key, TValue val)
  {
    Table t = this;
    switch (ttypetag(key))
    {
    case LUA_VSHRSTR:
      return t.luaH_psetshortstr(tsvalue(key), val);
    case LUA_VNUMINT:
      return t.psetint(ivalue(key), val);
    case LUA_VNIL:
      return HNOTFOUND;
    case LUA_VNUMFLT:
    {
      IntResult k = new IntResult();
      if (luaV_flttointeger(fltvalue(key), k, F2Ieq) != 0) /* integral index? */
        return t.psetint(k.v, val);                     /* use specialized version */
      /* else... */
    } /* FALLTHROUGH */
    default:
      return t.finishnodeset(t.nodegeneric(key, 0), val);
    }
  }

  /*
  ** Finish a raw "set table" operation, where 'hres' encodes where the
  ** value should have been (the result of a previous 'pset' operation).
  ** Beware: when using this function the caller probably need to check a
  ** GC barrier and invalidate the TM cache.
  */
  public void luaH_finishset(lua_State L, TValue key,
                             TValue value, int hres)
  {
    Table t = this;
    assert hres != HOK;
    if (hres == HNOTFOUND)
    {
      TValue aux = new TValue();
      TValue actk = key; /* actual key to insert */
      if (isempty(key))
        luaG_runerror(L, Lt.table_index_is_nil);
      else if (ttisfloat(key))
      {
        double f = fltvalue(key);
        IntResult k = new IntResult();
        if (luaV_flttointeger(f, k, F2Ieq) != 0)
        { /* is key equal to an integer? */
          setivalue(aux, k.v);
          actk = aux; /* use the integer as the key */
        }
        else if (!(f == f))
          luaG_runerror(L, Lt.table_index_is_NaN);
      }
      t.luaH_newkey(L, actk, value);
    }
    else if (hres > 0)
    { /* regular Node? */
      setobj(L, gnode(t, hres - HFIRSTNODE), value);
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
  public void luaH_set(lua_State L, TValue key, TValue value)
  {
    Table t = this;
    int hres = t.luaH_pset(key, value);
    if (hres != HOK)
      t.luaH_finishset(L, key, value, hres);
  }

  /*
  ** Ditto for a GC barrier. (No need to invalidate the TM cache, as
  ** integers cannot be keys to metamethods.)
  */
  public void luaH_setint(lua_State L, long key, TValue value)
  {
    Table t = this;
    int ik = t.ikeyinarray(key);
    if (ik > 0)
      obj2arr(t, ik - 1, value);
    else
    {
      int ok = t.rawfinishnodeset(t.nodeintfromhash(key), value);
      if (ok == 0)
      {
        TValue k = new TValue();
        setivalue(k, key);
        t.luaH_newkey(L, k, value);
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
  private long hash_search(lua_State L, int asize)
  {
    Table t = this;
    long i = (long) asize + 1; /* caller ensures t[i] is present */
    int rnd = L.l_G.seed;
    int n = (asize > 0) ? luaO_ceillog2(asize) : 0; /* width of 'asize' */
    long mask = ((long) 1 << n) - 1;                  /* 11...111 with the width of 'asize' */
    long incr = (Integer.toUnsignedLong(rnd) & mask) + 1;               /* first increment (at least 1) */
    long j = (incr <= LUA_MAXINTEGER - i) ? i + incr : i + 1;
    rnd >>>= n; /* used 'n' bits from 'rnd' */
    while (t.hashkeyisempty(j) == 0)
    {        /* repeat until an absent t[j] */
      i = j; /* 'i' is a present index */
      if (j <= LUA_MAXINTEGER / 2 - 1)
      {
        j = j * 2 + (rnd & 1); /* try again with 2j or 2j+1 */
        rnd >>>= 1;
      }
      else
      {
        j = LUA_MAXINTEGER;
        if (t.hashkeyisempty(j) != 0) /* t[j] not present? */
          break;                  /* 'j' now is an absent index */
        else                      /* weird case */
          return j;               /* well, max integer is a boundary... */
      }
    }
    /* i < j  &&  t[i] present  &&  t[j] absent */
    while (j - i > 1)
    { /* do a binary search between them */
      long m = i + (j - i) / 2;
      if (t.hashkeyisempty(m) != 0)
        j = m;
      else
        i = m;
    }
    return i;
  }

  private int binsearch(int i, int j)
  {
    Table t = this;
    assert i <= j;
    while (j - i > 1)
    { /* binary search */
      int m = i + (j - i) / 2;
      if (t.arraykeyisempty(m) != 0)
        j = m;
      else
        i = m;
    }
    return i;
  }

  /* return a border, saving it as a hint for next call */
  private long newhint(int hint)
  {
    Table t = this;
    assert hint <= t.asize();
    t.lenhint = hint;
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
  public long luaH_getn(lua_State L)
  {
    Table t = this;
    int asize = t.asize();
    if (asize > 0)
    { /* is there an array part? */
      final int maxvicinity = 4;
      int limit = t.lenhint; /* start with the hint */
      if (limit == 0)
        limit = 1; /* make limit a valid index in the array */
      if (t.arraykeyisempty(limit) != 0)
      { /* t[limit] empty? */
        /* there must be a border before 'limit' */
        int i;
        /* look for a border in the vicinity of the hint */
        for (i = 0; i < maxvicinity && limit > 1; i++)
        {
          limit--;
          if (t.arraykeyisempty(limit) == 0)
            return t.newhint(limit); /* 'limit' is a border */
        }
        /* t[limit] still empty; search for a border in [0, limit) */
        return t.newhint(t.binsearch(0, limit));
      }
      else
      { /* 'limit' is present in table; look for a border after it */
        int i;
        /* look for a border in the vicinity of the hint */
        for (i = 0; i < maxvicinity && limit < asize; i++)
        {
          limit++;
          if (t.arraykeyisempty(limit) != 0)
            return t.newhint(limit - 1); /* 'limit - 1' is a border */
        }
        if (t.arraykeyisempty(asize) != 0)
        { /* last element empty? */
          /* t[limit] not empty; search for a border in [limit, asize) */
          return t.newhint(t.binsearch(limit, asize));
        }
      }
      /* last element non empty; set a hint to speed up finding that again */
      /* (keys in the hash part cannot be hints) */
      t.lenhint = asize;
    }
    /* no array part or t[asize] is not empty; check the hash part */
    assert asize == 0 || t.arraykeyisempty(asize) == 0;
    if (isdummy(t) != 0 || t.hashkeyisempty((long) asize + 1) != 0)
      return asize; /* 'asize + 1' is empty */
    else            /* 'asize + 1' is also non empty */
      return t.hash_search(L, asize);
  }

  /*
  ** The main position of 'key': the node where a lookup for it starts. Every
  ** key of the hash part is in the chain that starts there (the test library
  ** and checks of a table's layout use it).
  */
  public int luaH_mainposition(TValue key)
  {
    Table t = this;
    return t.mainpositionTV(key);
  }
}
