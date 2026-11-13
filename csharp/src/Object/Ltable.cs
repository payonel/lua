using System;
using Payonel.LungoLua.Compat;
using Frexp = Payonel.LungoLua.Platform.Libm.Frexp;
using Cost = Payonel.LungoLua.Runtime.Cost;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Object.Llimits;
using static Payonel.LungoLua.Object.Lobject;
using static Payonel.LungoLua.Object.Lstring;
using static Payonel.LungoLua.Platform.Libm;
using static Payonel.LungoLua.Platform.Libm;
using static Payonel.LungoLua.Runtime.Ltm;

namespace Payonel.LungoLua.Object;

public sealed class Ltable
{
  private Ltable()
  {
  }

  public static Node gnode(Table t, int i)
  {
    return t.node[i];
  }

  /*
  ** Clear all bits of fast-access metamethods, which means that the table
  ** may have any of these metamethods. (First access that fails after the
  ** clearing will set the bit again.)
  */
  public static void invalidateTMcache(Table t)
  {
    t.flags = (sbyte) (t.flags & ~maskflags);
  }

  /*
  ** Bit BITDUMMY set in 'flags' means the table is using the dummy node
  ** for its hash part.
  */

  public const int BITDUMMY = 1 << 6;
  public const sbyte NOTBITDUMMY = (sbyte) ~BITDUMMY;

  public static bool isdummy(Table t)
  {
    return (t.flags & BITDUMMY) != 0;
  }

  public static void setnodummy(Table t)
  {
    t.flags = (sbyte) (t.flags & NOTBITDUMMY);
  }

  public static void setdummy(Table t)
  {
    t.flags = (sbyte) (t.flags | BITDUMMY);
  }

  /* allocated size for hash nodes */
  public static int allocsizenode(Table t)
  {
    return isdummy(t) ? 0 : sizenode(t);
  }

  /* results from pset */
  public const int HOK = 0;
  public const int HNOTFOUND = 1;
  public const int HNOTATABLE = 2;
  public const int HFIRSTNODE = 3;

  /*
  ** 'luaH_get*' operations set 'res', unless the value is absent, and
  ** return the tag of the result.
  ** The 'luaH_pset*' (pre-set) operations set the given value and return
  ** HOK, unless the original value is absent. In that case, if the key
  ** is really absent, they return HNOTFOUND. Otherwise, if there is a
  ** slot with that key but with no value, 'luaH_pset*' return an encoding
  ** of where the key is (usually called 'hres'). (pset cannot set that
  ** value because there might be a metamethod.) If the slot is in the
  ** hash part, the encoding is (HFIRSTNODE + hash index); if the slot is
  ** in the array part, the encoding is (~array index), a negative value.
  ** The value HNOTATABLE is used by the fast macros to signal that the
  ** value being indexed is not a table.
  ** (The size for the array part is limited by the maximum power of two
  ** that fits in an unsigned integer; that is INT_MAX+1. So, the C-index
  ** ranges from 0, which encodes to -1, to INT_MAX, which encodes to
  ** INT_MIN. The size of the hash part is limited by the maximum power of
  ** two that fits in a signed integer; that is (INT_MAX+1)/2. So, it is
  ** safe to add HFIRSTNODE to any index there.)
  */

  /*
  ** The array part of a table is two separate, forward-indexed arrays: a
  ** 'Value' array ('t->array') and a parallel tag array ('t->tags'). The
  ** length hint for #t lives in the 'lenhint' field of the Table.
  ** All accesses to the arrays should be through the macros 'getArrTag'
  ** and 'getArrVal'.
  */

  /* Computes the address of the tag for the abstract C-index 'k' */
  public static sbyte getArrTag(Table t, long k)
  {
    return t.tags[(int) k];
  }

  public static void setArrTag(Table t, long k, sbyte tag)
  {
    t.tags[(int) k] = tag;
  }

  /* Computes the address of the value for the abstract C-index 'k' */
  public static TValue getArrVal(Table t, long k)
  {
    return new TValue(t.array_prim[(int) k], t.array_ref[(int) k], (sbyte) 0);
  }

  /*
  ** Move TValues to/from arrays, using C indices
  */
  public static void arr2obj(Table h, long k, TValue val)
  {
    val.tt_ = h.tags[(int) k];
    val.prim = h.array_prim[(int) k];
    val.@ref = h.array_ref[(int) k];
  }

  public static void obj2arr(Table h, long k, TValue val)
  {
    h.tags[(int) k] = val.tt_;
    h.array_prim[(int) k] = val.prim;
    h.array_ref[(int) k] = val.@ref;
  }

  /*
  ** Often, we need to check the tag of a value before moving it. The
  ** following macros also move TValues to/from arrays, but receive the
  ** precomputed tag value or address as an extra argument.
  */
  public static void farr2val(Table h, long k, sbyte tag, TValue res)
  {
    res.tt_ = tag;
    res.prim = h.array_prim[(int) k];
    res.@ref = h.array_ref[(int) k];
  }

  public static void fval2arr(Table h, long k, TValue val)
  {
    h.tags[(int) k] = val.tt_;
    h.array_prim[(int) k] = val.prim;
    h.array_ref[(int) k] = val.@ref;
  }

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

  /*
  ** Only hash parts with at least 2^LIMFORLAST have a 'lastfree' field
  ** that optimizes finding a free slot. That field is stored just before
  ** the array of nodes, in the same block. Smaller tables do a complete
  ** search when looking for a free slot.
  */
  internal const int LIMFORLAST = 3; /* log2 of real limit (8) */

  /*
  ** MAXABITS is the largest integer such that 2^MAXABITS fits in an
  ** unsigned int.
  */
  internal const int MAXABITS = 30;

  /*
  ** MAXASIZEB is the maximum number of elements in the array part such
  ** that the size of the array fits in 'size_t'.
  */
  private static readonly long MAXASIZEB = JLong.divideUnsigned(MAX_SIZET, Cost.Value + Cost.Byte);

  /*
  ** MAXASIZE is the maximum size of the array part. It is the minimum
  ** between 2^MAXABITS and MAXASIZEB.
  */
  internal const int MAXASIZE = 1 << MAXABITS;

  private static readonly bool static_init_1 = static_init_1_run();
  private static bool static_init_1_run()
  {
    { if (JAssert.on && !( JLong.compareUnsigned(MAXASIZE, MAXASIZEB) <= 0)) throw new AssertionError(); }
  return true; }

  /*
  ** MAXHBITS is the largest integer such that 2^MAXHBITS fits in a
  ** signed int.
  */
  internal const int MAXHBITS = 30;

  /*
  ** MAXHSIZE is the maximum size of the hash part. It is the minimum
  ** between 2^MAXHBITS and the maximum size such that, measured in bytes,
  ** it fits in a 'size_t'.
  */
  internal static readonly int MAXHSIZE = ((JLong.compareUnsigned(1 << MAXHBITS, JLong.divideUnsigned(MAX_SIZET, Cost.Node)) <= 0) ? (1 << MAXHBITS) : (int) JLong.divideUnsigned(MAX_SIZET, Cost.Node));

  internal static readonly Node dummynode_ = new Node();

  private static readonly bool static_init_2 = static_init_2_run();
  private static bool static_init_2_run()
  {
    dummynode_.prim = 0;
    dummynode_.@ref = null;
    dummynode_.tt_ = LUA_VEMPTY;
    dummynode_.key_tt = LUA_TDEADKEY;
    dummynode_.next = 0;
    dummynode_.key_prim = 0;
    dummynode_.key_ref = null;
  return true; }

  internal static readonly TValue absentkey = new TValue(0, null, LUA_VABSTKEY);

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
  internal static int l_hashfloat(double n)
  {
    Frexp i = new Frexp();
    IntResult ni = new IntResult();
    n = frexp(n, i) * -(double) JInteger.MIN_VALUE;
    if (!lua_numbertointeger(n, ni))
    { /* is 'n' inf/-inf/NaN? */
      { if (JAssert.on && !( (!(n == n)) || fabs(n) == JDouble.POSITIVE_INFINITY)) throw new AssertionError(); }
      return 0;
    }
    else
    { /* normal case */
      int u = i.e + (int) ni.v;
      return (JInteger.compareUnsigned(u, JInteger.MAX_VALUE) <= 0 ? u : ~u);
    }
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
  internal static bool equalkey(TValue k1, Node n2, bool deadok)
  {
    if (k1.tt_ != n2.key_tt)
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
        { if (JAssert.on && !( iscollectable(k1))) throw new AssertionError(); }
        return gcvalue(k1) == gckey(n2);
      }
      else
        return false; /* otherwise, different variants cannot be equal */
    }
    else
    { /* equal variants */
      switch (n2.key_tt)
      {
      case LUA_VNIL:
      case LUA_VFALSE:
      case LUA_VTRUE:
        return true;
      case LUA_VNUMINT:
        { if (JAssert.on && !( ttisinteger(k1))) throw new AssertionError(); }
        return (ivalue(k1) == keyival(n2));
      case LUA_VNUMFLT:
        { if (JAssert.on && !( ttisfloat(k1))) throw new AssertionError(); }
        return (fltvalue(k1) == keyfltval(n2));
      case LUA_VLIGHTUSERDATA:
        { if (JAssert.on && !( ttislightuserdata(k1))) throw new AssertionError(); }
        return pvalue(k1) == keypval(n2);
      case LUA_VLNGSTR | BIT_ISCOLLECTABLE:
      {
        return luaS_eqstr(tsvalue(k1),
                          keystrval(n2));
      }
      default:
        { if (JAssert.on && !( iscollectable(k1))) throw new AssertionError(); }
        return gcvalue(k1) == gckey(n2);
      }
    }
  }

  /*
  ** Return the index 'k' (converted to an unsigned) if it is inside
  ** the range [1, limit].
  */
  internal static int checkrange(long k, int limit)
  {
    return (k >= 1 && k <= limit) ? (int) k : 0;
  }

  /*
  ** Return the index 'k' if 'k' is an appropriate key to live in the
  ** array part of a table, 0 otherwise.
  */
  private static int arrayindex(long k)
  {
    return checkrange(k, MAXASIZE);
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
  internal sealed class Counters
  {
    internal int total;
    internal int na;
    internal bool deleted;
    internal int[] nums = new int[MAXABITS + 1];
  }

  /*
  ** Check whether it is worth to use 'na' array entries instead of 'nh'
  ** hash nodes. (A hash node uses ~3 times more memory than an array
  ** entry: Two values plus 'next' versus one value.) Evaluate with size_t
  ** to avoid overflows.
  */
  private static bool arrayXhash(long na, long nh)
  {
    return JLong.compareUnsigned(na, nh * 3) <= 0;
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
  internal static int computesizes(Counters ct)
  {
    int i;
    int twotoi;      /* 2^i (candidate for optimal size) */
    int a = 0;       /* number of elements smaller than 2^i */
    int na = 0;      /* number of elements to go to array part */
    int optimal = 0; /* optimal size for array part */
    /* traverse slices while 'twotoi' does not overflow and total of array
       indices still can satisfy 'arrayXhash' against the array size */
    for (i = 0, twotoi = 1;
         i <= MAXABITS && arrayXhash(twotoi, ct.na);
         i++, twotoi = (i <= MAXABITS) ? twotoi * 2 : twotoi)
    {
      int nums = ct.nums[i];
      a += nums;
      if (nums > 0 && /* grows array only if it gets more elements... */
          arrayXhash(twotoi, a))
      {                   /* ...while using "less memory" */
        optimal = twotoi; /* optimal size (till now) */
        na = a;           /* all elements up to 'optimal' will go to array part */
      }
    }
    ct.na = na;
    return optimal;
  }

  internal static void countint(long key, Counters ct)
  {
    int k = arrayindex(key);
    if (k != 0)
    {                               /* is 'key' an array index? */
      ct.nums[luaO_ceillog2(k)]++; /* count as such */
      ct.na++;
    }
  }

  /*
  ** (Re)insert all elements from the hash part of 'ot' into table 't'.
  */
  public static void reinserthash(lua_State L, Table ot, Table t)
  {
    int j;
    int size = sizenode(ot);
    for (j = 0; j < size; j++)
    {
      Node old = gnode(ot, j);
      if (!isempty(old))
      {
        /* doesn't need barrier/invalidate cache, as entry was
           already present in the table */
        TValue k = new TValue();
        getnodekey(L, k, old);
        t.newcheckedkey(k, old);
      }
    }
  }

  /*
  ** Exchange the hash part of 't1' and 't2'. (In 'flags', only the dummy
  ** bit must be exchanged:  The metamethod bits do not change during a
  ** resize, so the "real" table can keep their values.)
  */
  internal static void exchangehashpart(Table t1, Table t2)
  {
    Node[] node = t1.node;
    int lastfree = t1.lastfree;
    int bitdummy1 = t1.flags & BITDUMMY;
    t1.node = t2.node;
    t1.lastfree = t2.lastfree;
    t1.flags = (sbyte) ((t1.flags & NOTBITDUMMY) | (t2.flags & BITDUMMY));
    t2.node = node;
    t2.lastfree = lastfree;
    t2.flags = (sbyte) ((t2.flags & NOTBITDUMMY) | bitdummy1);
  }

  /*
  ** }=============================================================
  */

  internal static sbyte finishnodeget(TValue val, TValue res)
  {
    if (!isempty(val))
    {
      setobj(null, res, val);
    }
    return ttypetag(val);
  }

  /*
  ** When a 'pset' cannot be completed, this function returns an encoding
  ** of its result, to be used by 'luaH_finishset'.
  */
  internal static int retpsetcode(int idx)
  {
    if (idx < 0)
      return HNOTFOUND; /* no slot with that key */
    else                /* return node encoded */
      return idx + HFIRSTNODE;
  }
}
