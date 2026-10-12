package lua.object;

import static lua.api.LuaApi.*;
import static lua.api.LuaConf.*;
import static lua.bytes.InternalString.sv_byteat;
import static lua.bytes.Lctype.*;
import static lua.bytes.UtilCast.*;
import static lua.gc.Lgc.*;
import static lua.object.Llimits.*;
import static lua.platform.Libm.pow;
import static lua.platform.LocaleUtil.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lmem.*;
import static lua.runtime.Lstate.*;
import static lua.runtime.Ltm.*;
import static lua.vm.Lvm.*;

import lua.api.lua_CFunction;
import lua.bytes.ByteOwner;
import lua.bytes.ByteSpan;
import lua.bytes.ByteView;
import lua.bytes.CharsFormat;
import lua.bytes.FmtScan;
import lua.bytes.InternalString;
import lua.bytes.Lt;
import lua.bytes.ToChars;
import lua.runtime.ProtectedCall;
import lua.runtime.global_State;
import lua.runtime.lua_State;

public final class Lobject
{
  private Lobject()
  {
  }

  /*
  ** Extra types for collectable non-values
  */
  public static final int LUA_TUPVAL = LUA_NUMTYPES;      /* upvalues */
  public static final int LUA_TPROTO = LUA_NUMTYPES + 1;  /* function prototypes */
  public static final int LUA_TBUDGET = LUA_NUMTYPES + 2; /* budget groups */
  public static final int LUA_TDEADKEY = LUA_NUMTYPES + 3; /* removed keys in tables */

  /*
  ** number of all possible types (including LUA_TNONE but excluding DEADKEY)
  */
  public static final int LUA_TOTALTYPES = LUA_TBUDGET + 2;

  /* add variant bits to a type */
  public static byte makevariant(int t, int v)
  {
    return (byte) (t | (v << 4));
  }

  public static TValue getobj(TValue o)
  {
    return new TValue(o.prim, o.ref, o.tt_);
  }

  public static void copyobj(TValue dst, TValue src)
  {
    /* field by field, like upstream's setobj */
    dst.prim = src.prim;
    dst.ref = src.ref;
    dst.tt_ = src.tt_;
  }

  public static final int NOVARIANT_MASK = 0x0F;

  public static final int WITHVARIANT_MASK = 0x3F;

  /* tag with no variants (bits 0-3) */
  public static byte novariant(int t)
  {
    return (byte) (t & NOVARIANT_MASK);
  }

  /* type tag of a TValue (bits 0-3 for tags + variant bits 4-5) */
  public static byte withvariant(int t)
  {
    return (byte) (t & WITHVARIANT_MASK);
  }

  public static byte ttypetag(TValue o)
  {
    return withvariant(o.tt_);
  }

  /* type of a TValue */
  public static byte ttype(TValue o)
  {
    return novariant(o.tt_);
  }

  /* Macros to test type */
  public static boolean checktag(TValue o, int t)
  {
    return o.tt_ == t;
  }

  public static boolean checktype(TValue o, int t)
  {
    return ttype(o) == t;
  }

  public static final int NOSLOT = -1;

  /*
  ** {==================================================================
  ** Nil
  ** ===================================================================
  */

  /* Standard nil */
  public static final byte LUA_VNIL = (byte) (LUA_TNIL | (0 << 4));

  /* Empty slot (which might be different from a slot containing nil) */
  public static final byte LUA_VEMPTY = (byte) (LUA_TNIL | (1 << 4));

  /* Value returned for a key not found in a table (absent key) */
  public static final byte LUA_VABSTKEY = (byte) (LUA_TNIL | (2 << 4));

  /* Special variant to signal that a fast get is accessing a non-table */
  public static final byte LUA_VNOTABLE = (byte) (LUA_TNIL | (3 << 4));

  /* macro to test for (any kind of) nil */
  public static boolean ttisnil(TValue v)
  {
    return checktype(v, LUA_TNIL);
  }

  /*
  ** Macro to test the result of a table access. Formally, it should
  ** distinguish between LUA_VEMPTY/LUA_VABSTKEY/LUA_VNOTABLE and
  ** other tags. As currently nil is equivalent to LUA_VEMPTY, it is
  ** simpler to just test whether the value is nil.
  */
  public static boolean tagisempty(int tag)
  {
    return novariant(tag) == LUA_TNIL;
  }

  /* macro to test for a standard nil */
  public static boolean ttisstrictnil(TValue o)
  {
    return checktag(o, LUA_VNIL);
  }

  public static void setnilvalue(TValue obj)
  {
    obj.tt_ = LUA_VNIL;
  }

  public static boolean isabstkey(TValue v)
  {
    return checktag(v, LUA_VABSTKEY);
  }

  /*
  ** macro to detect non-standard nils (used only in assertions)
  */
  public static boolean isnonstrictnil(TValue v)
  {
    return ttisnil(v) && !ttisstrictnil(v);
  }

  /*
  ** By default, entries with any kind of nil are considered empty.
  ** (In any definition, values associated with absent keys must also
  ** be accepted as empty.)
  */
  public static boolean isempty(TValue v)
  {
    return ttisnil(v);
  }

  /* mark an entry as empty */
  public static void setempty(TValue v)
  {
    v.tt_ = LUA_VEMPTY;
  }

  /*
  ** {==================================================================
  ** Booleans
  ** ===================================================================
  */

  public static final byte LUA_VFALSE = (byte) (LUA_TBOOLEAN | (0 << 4));
  public static final byte LUA_VTRUE = (byte) (LUA_TBOOLEAN | (1 << 4));

  public static boolean ttisboolean(TValue o)
  {
    return checktype(o, LUA_TBOOLEAN);
  }

  public static boolean ttisfalse(TValue o)
  {
    return checktag(o, LUA_VFALSE);
  }

  public static boolean ttistrue(TValue o)
  {
    return checktag(o, LUA_VTRUE);
  }

  public static boolean l_isfalse(TValue o)
  {
    return ttisfalse(o) || ttisnil(o);
  }

  public static boolean tagisfalse(int t)
  {
    return t == LUA_VFALSE || novariant(t) == LUA_TNIL;
  }

  /*
  ** {==================================================================
  ** Threads
  ** ===================================================================
  */

  public static final byte LUA_VTHREAD = (byte) (LUA_TTHREAD | (0 << 4));

  /*
  ** {==================================================================
  ** Collectable Objects
  ** ===================================================================
  */

  /* Bit mark for collectable types */
  public static final int BIT_ISCOLLECTABLE = 1 << 6;

  public static byte ctb(int t)
  {
    return (byte) (t | BIT_ISCOLLECTABLE);
  }

  public static boolean iscollectable(TValue o)
  {
    return (o.tt_ & BIT_ISCOLLECTABLE) != 0;
  }

  public static boolean ttisthread(TValue o)
  {
    return checktag(o, ctb(LUA_VTHREAD));
  }

  /*
  ** {==================================================================
  ** Numbers
  ** ===================================================================
  */

  /* Variant tags for numbers */
  public static final byte LUA_VNUMINT = (byte) (LUA_TNUMBER | (0 << 4)); /* integer numbers */
  public static final byte LUA_VNUMFLT = (byte) (LUA_TNUMBER | (1 << 4)); /* float numbers */

  public static boolean ttisnumber(TValue o)
  {
    return checktype(o, LUA_TNUMBER);
  }

  public static boolean ttisfloat(TValue o)
  {
    return checktag(o, LUA_VNUMFLT);
  }

  public static boolean ttisinteger(TValue o)
  {
    return checktag(o, LUA_VNUMINT);
  }

  /*
  ** {==================================================================
  ** Strings
  ** ===================================================================
  */

  /* Variant tags for strings */
  public static final byte LUA_VSTRING = (byte) (LUA_TSTRING | (0 << 4));
  public static final byte LUA_VSHRSTR = LUA_VSTRING;
  public static final byte LUA_VLNGSTR = LUA_VSTRING;

  /* test for a string */
  public static boolean ttisstring(TValue o)
  {
    return checktype(o, LUA_TSTRING);
  }

  public static boolean ttisshrstring(TValue o)
  {
    return checktag(o, ctb(LUA_VSHRSTR));
  }

  public static final byte STREXTRA_EXTERNAL = (byte) 0xFF;

  public static boolean strisexternal(TString ts)
  {
    return ts.extra == STREXTRA_EXTERNAL;
  }

  public static int tsslen(TString ts)
  {
    return ts.data.size();
  }

  /*
  ** {==================================================================
  ** Userdata
  ** ===================================================================
  */

  /*
  ** Light userdata should be a variant of userdata, but for compatibility
  ** reasons they are also different types.
  */
  public static final byte LUA_VLIGHTUSERDATA = (byte) (LUA_TLIGHTUSERDATA | (0 << 4));

  public static final byte LUA_VUSERDATA = (byte) (LUA_TUSERDATA | (0 << 4));

  public static boolean ttislightuserdata(TValue o)
  {
    return checktag(o, LUA_VLIGHTUSERDATA);
  }

  public static boolean ttisfulluserdata(TValue o)
  {
    return checktag(o, ctb(LUA_VUSERDATA));
  }

  /* get the address of the memory block inside 'Udata' */
  public static byte[] getudatamem(Udata u)
  {
    return u.data.array();
  }

  /*
  ** {==================================================================
  ** Prototypes
  ** ===================================================================
  */

  public static final byte LUA_VPROTO = (byte) (LUA_TPROTO | (0 << 4));

  /*
  ** Flags in Prototypes
  */
  public static final int PF_VAHID = 1; /* function has hidden vararg arguments */
  public static final int PF_VATAB = 2; /* function has vararg table */
  public static final int PF_FIXED = 4; /* prototype has parts in fixed memory */

  public static int isvararg(Proto p)
  {
    return p.flag & (PF_VAHID | PF_VATAB);
  }

  public static void needvatab(Proto p)
  {
    p.flag = (byte) (p.flag | PF_VATAB);
  }

  /*
  ** {==================================================================
  ** Functions
  ** ===================================================================
  */

  public static final byte LUA_VUPVAL = (byte) (LUA_TUPVAL | (0 << 4));

  public static final byte LUA_VBUDGET = (byte) (LUA_TBUDGET | (0 << 4));

  /* Variant tags for functions */
  public static final byte LUA_VLCL = (byte) (LUA_TFUNCTION | (0 << 4)); /* Lua closure */
  public static final byte LUA_VLCF = (byte) (LUA_TFUNCTION | (1 << 4)); /* light C function */
  public static final byte LUA_VCCL = (byte) (LUA_TFUNCTION | (2 << 4)); /* C closure */

  public static boolean ttisfunction(TValue o)
  {
    return checktype(o, LUA_TFUNCTION);
  }

  public static boolean ttisLclosure(TValue o)
  {
    return checktag(o, ctb(LUA_VLCL));
  }

  public static boolean ttislcf(TValue o)
  {
    return checktag(o, ctb(LUA_VLCF));
  }

  public static boolean ttisCclosure(TValue o)
  {
    return checktag(o, ctb(LUA_VCCL));
  }

  public static int sizeCclosure(int n)
  {
    return lua.runtime.Cost.CClosure + lua.runtime.Cost.TValue * n;
  }

  public static int sizeLclosure(int n)
  {
    return lua.runtime.Cost.LClosure + lua.runtime.Cost.Ref * n;
  }

  /*
  ** {==================================================================
  ** Tables
  ** ===================================================================
  */

  public static final byte LUA_VTABLE = (byte) (LUA_TTABLE | (0 << 4));

  public static boolean ttistable(TValue o)
  {
    return checktag(o, ctb(LUA_VTABLE));
  }

  public static void copynode(Node dst, Node src)
  {
    copyobj(dst, src);
    dst.key_tt = src.key_tt;
    dst.next = src.next;
    dst.key_prim = src.key_prim;
    dst.key_ref = src.key_ref;
  }

  /*
  ** 'module' operation for hashing (size is always a power of 2)
  */
  public static int lmod(long s, int size)
  {
    assert Llimits.ispow2(size);
    return (int) (s & (long) (size - 1));
  }

  public static int twoto(int x)
  {
    return 1 << x;
  }

  public static int sizenode(Table t)
  {
    return t.node.length;
  }

  public static long ivalue(TValue o)
  {
    assert ttisinteger(o);
    return o.prim;
  }

  public static double fltvalue(TValue o)
  {
    assert ttisfloat(o);
    return Double.longBitsToDouble(o.prim);
  }

  public static double nvalue(TValue o)
  {
    assert ttisinteger(o) || ttisfloat(o);
    return ttisinteger(o) ? (double) o.prim : Double.longBitsToDouble(o.prim);
  }

  public static Object pvalue(TValue o)
  {
    assert ttislightuserdata(o);
    return o.ref;
  }

  public static lua_CFunction fvalue(TValue o)
  {
    assert ttislcf(o);
    return ((CFuncInfo) o.ref).fptr;
  }

  public static CFuncInfo fcfuncinfo(TValue o)
  {
    assert ttislcf(o);
    return (CFuncInfo) o.ref;
  }

  public static GCObject gcvalue(TValue o)
  {
    assert iscollectable(o);
    return (GCObject) o.ref;
  }

  public static TString tsvalue(TValue o)
  {
    assert ttisshrstring(o);
    return (TString) o.ref;
  }

  public static Table hvalue(TValue o)
  {
    assert ttistable(o);
    return (Table) o.ref;
  }

  public static Udata uvalue(TValue o)
  {
    assert ttisfulluserdata(o);
    return (Udata) o.ref;
  }

  public static Closure clvalue(TValue o)
  {
    assert ttisLclosure(o) || ttisCclosure(o);
    return (Closure) o.ref;
  }

  public static LClosure clLvalue(TValue o)
  {
    assert ttisLclosure(o);
    return (LClosure) o.ref;
  }

  public static CClosure clCvalue(TValue o)
  {
    assert ttisCclosure(o);
    return (CClosure) o.ref;
  }

  public static Proto getproto(TValue o)
  {
    return clLvalue(o).p;
  }

  public static boolean keyisnil(Node node)
  {
    return node.key_tt == LUA_TNIL;
  }

  public static boolean keyisinteger(Node node)
  {
    return node.key_tt == LUA_VNUMINT;
  }

  public static boolean keyisshrstr(Node node)
  {
    return node.key_tt == ctb(LUA_VSHRSTR);
  }

  public static void setnilkey(Node node)
  {
    node.key_tt = LUA_TNIL;
  }

  public static boolean keyiscollectable(Node n)
  {
    return (n.key_tt & BIT_ISCOLLECTABLE) != 0;
  }

  public static void setdeadkey(Node node)
  {
    node.key_tt = LUA_TDEADKEY;
  }

  public static boolean keyisdead(Node node)
  {
    return node.key_tt == LUA_TDEADKEY;
  }

  public static GCObject gckey(Node n)
  {
    assert keyiscollectable(n) || keyisdead(n);
    return (GCObject) n.key_ref;
  }

  public static GCObject gckeyN(Node n)
  {
    return keyiscollectable(n) ? gckey(n) : null;
  }

  public static TString keystrval(Node n)
  {
    assert keyisshrstr(n);
    return (TString) n.key_ref;
  }

  public static long keyival(Node n)
  {
    assert keyisinteger(n);
    return n.key_prim;
  }

  public static double keyfltval(Node n)
  {
    assert n.key_tt == LUA_VNUMFLT;
    return Double.longBitsToDouble(n.key_prim);
  }

  public static Object keypval(Node n)
  {
    assert n.key_tt == LUA_VLIGHTUSERDATA;
    return n.key_ref;
  }

  public static lua_CFunction keyfval(Node n)
  {
    assert n.key_tt == ctb(LUA_VLCF);
    return ((CFuncInfo) n.key_ref).fptr;
  }

  public static CFuncInfo keycfuncinfo(Node n)
  {
    assert n.key_tt == ctb(LUA_VLCF);
    return (CFuncInfo) n.key_ref;
  }

  public static void checkliveness(Object L, TValue obj)
  {
    assert luaO_checkliveness((lua_State) L, obj) != 0;
  }

  public static void setobj(Object L, TValue obj1, TValue obj2)
  {
    obj1.prim = obj2.prim;
    obj1.ref = obj2.ref;
    obj1.tt_ = obj2.tt_;
    checkliveness(L, obj1);
    assert !isnonstrictnil(obj1);
  }

  public static void setfltvalue(TValue obj, double x)
  {
    obj.prim = Double.doubleToRawLongBits(x);
    obj.tt_ = LUA_VNUMFLT;
  }

  public static void chgfltvalue(TValue obj, double x)
  {
    assert ttisfloat(obj);
    obj.prim = Double.doubleToRawLongBits(x);
  }

  public static void setivalue(TValue obj, long x)
  {
    obj.prim = x;
    obj.tt_ = LUA_VNUMINT;
  }

  public static void chgivalue(TValue obj, long x)
  {
    assert ttisinteger(obj);
    obj.prim = x;
  }

  public static void setpvalue(TValue obj, Object x)
  {
    obj.ref = x;
    obj.tt_ = LUA_VLIGHTUSERDATA;
  }

  public static void setfvalue(TValue obj, CFuncInfo cfi)
  {
    obj.ref = cfi;
    obj.tt_ = ctb(LUA_VLCF);
  }

  public static void setgcovalue(Object L, TValue obj, GCObject x)
  {
    obj.ref = x;
    obj.tt_ = ctb(x.tt);
  }

  public static void setsvalue(Object L, TValue obj, TString x)
  {
    assert novariant(x.tt) == LUA_TSTRING;
    obj.ref = x;
    obj.tt_ = ctb(x.tt);
    checkliveness(L, obj);
  }

  public static void setuvalue(Object L, TValue obj, Udata x)
  {
    assert x.tt == LUA_VUSERDATA;
    obj.ref = x;
    obj.tt_ = ctb(LUA_VUSERDATA);
    checkliveness(L, obj);
  }

  public static void sethvalue(Object L, TValue obj, Table x)
  {
    assert x.tt == LUA_VTABLE;
    obj.ref = x;
    obj.tt_ = ctb(LUA_VTABLE);
    checkliveness(L, obj);
  }

  public static void setclLvalue(Object L, TValue obj, LClosure x)
  {
    assert x.tt == LUA_VLCL;
    obj.ref = x;
    obj.tt_ = ctb(LUA_VLCL);
    checkliveness(L, obj);
  }

  public static void setclCvalue(Object L, TValue obj, CClosure x)
  {
    assert x.tt == LUA_VCCL;
    obj.ref = x;
    obj.tt_ = ctb(LUA_VCCL);
    checkliveness(L, obj);
  }

  public static void setnodekey(Node node, TValue obj)
  {
    node.key_prim = obj.prim;
    node.key_ref = obj.ref;
    node.key_tt = obj.tt_;
  }

  public static void getnodekey(Object L, TValue obj, Node node)
  {
    obj.prim = node.key_prim;
    obj.ref = node.key_ref;
    obj.tt_ = node.key_tt;
    checkliveness(L, obj);
  }

  /* size of buffer for 'luaO_utf8esc' function */
  public static final int UTF8BUFFSZ = 8;

  public static final int MAXFMTARGS = 8;

  /*
  ** Computes ceil(log2(x)), which is the smallest integer n such that
  ** x <= (1 << n).
  */
  public static int luaO_checkliveness(lua_State L, TValue obj)
  {
    if (iscollectable(obj))
    {
      assert ttypetag(obj) == gcvalue(obj).tt;
      assert L == null || isdead(L.l_G, gcvalue(obj)) == 0;
    }
    return 1;
  }

  private static final byte[] log_2 = {/* log_2[i - 1] = ceil(log2(i)) */
                                       0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
                                       6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
                                       7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
                                       7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
                                       8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                       8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                       8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                       8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};

  public static int luaO_ceillog2(int x)
  {
    int l = 0;
    x--;
    while (x >= 256)
    {
      l += 8;
      x >>= 8;
    }
    return l + log_2[x];
  }

  /*
  ** Encodes 'p'% as a floating-point byte, represented as (eeeexxxx).
  ** The exponent is represented using excess-7. Mimicking IEEE 754, the
  ** representation normalizes the number when possible, assuming an extra
  ** 1 before the mantissa (xxxx) and adding one to the exponent (eeee)
  ** to signal that. So, the real value is (1xxxx) * 2^(eeee - 7 - 1) if
  ** eeee != 0, and (xxxx) * 2^-7 otherwise (subnormal numbers).
  */
  public static int luaO_codeparam(long p)
  {
    if (p < 0 || p >= ((long) 0x1F << (0xF - 7 - 1)) * 100) /* overflow? */
      return 0xFF;                                          /* return maximum value */
    else
    {
      int q = (int) ((p * 128 + 99) / 100); /* round up the division */
      if (q < 0x10)
      { /* subnormal number? */
        /* exponent bits are already zero; nothing else to do */
        return q;
      }
      else
      { /* p >= 0x10 implies ceil(log2(p + 1)) >= 5 */
        /* preserve 5 bits in 'p' */
        int log = luaO_ceillog2(q + 1) - 5;
        return ((q >> log) - 0x10) | ((log + 1) << 4);
      }
    }
  }

  /*
  ** Computes 'p' times 'x', where 'p' is a floating-point byte. Roughly,
  ** we have to multiply 'x' by the mantissa and then shift accordingly to
  ** the exponent.  If the exponent is positive, both the multiplication
  ** and the shift increase 'x', so we have to care only about overflows.
  ** For negative exponents, however, multiplying before the shift keeps
  ** more significant bits, as long as the multiplication does not
  ** overflow, so we check which order is best.
  */
  public static long luaO_applyparam(int p, long x)
  {
    int m = p & 0xF;  /* mantissa */
    int e = p >> 4; /* exponent */
    if (e > 0)
    {            /* normalized? */
      e--;       /* correct exponent */
      m += 0x10; /* correct mantissa; maximum value is 0x1F */
    }
    e -= 7; /* correct excess-7 */
    if (e >= 0)
    {
      if (x < (MAX_LMEM / 0x1F) >> e) /* no overflow? */
        return (x * m) << e;          /* order doesn't matter here */
      else                            /* real overflow */
        return MAX_LMEM;
    }
    else
    { /* negative exponent */
      e = -e;
      if (x < MAX_LMEM / 0x1F)             /* multiplication cannot overflow? */
        return (x * m) >> e;               /* multiplying first gives more precision */
      else if ((x >> e) < MAX_LMEM / 0x1F) /* cannot overflow after shift? */
        return (x >> e) * m;
      else /* real overflow */
        return MAX_LMEM;
    }
  }

  private static long intarith(lua_State L, int op, long v1,
                               long v2)
  {
    switch (op)
    {
    case LUA_OPADD:
      return l_intadd(v1, v2);
    case LUA_OPSUB:
      return l_intsub(v1, v2);
    case LUA_OPMUL:
      return l_intmul(v1, v2);
    case LUA_OPMOD:
      return luaV_mod(L, v1, v2);
    case LUA_OPIDIV:
      return luaV_idiv(L, v1, v2);
    case LUA_OPBAND:
      return l_intband(v1, v2);
    case LUA_OPBOR:
      return l_intbor(v1, v2);
    case LUA_OPBXOR:
      return l_intbxor(v1, v2);
    case LUA_OPSHL:
      return luaV_shiftl(v1, v2);
    case LUA_OPSHR:
      return luaV_shiftr(v1, v2);
    case LUA_OPUNM:
      return l_intunm(v1);
    case LUA_OPBNOT:
      return l_intbnot(v1);
    default:
      assert false;
      return 0;
    }
  }

  private static double numarith(lua_State L, int op, double v1,
                                 double v2)
  {
    switch (op)
    {
    case LUA_OPADD:
      return (v1) + v2;
    case LUA_OPSUB:
      return (v1) - v2;
    case LUA_OPMUL:
      return (v1) * v2;
    case LUA_OPDIV:
      return v1 / v2;
    case LUA_OPPOW:
      return (v2 == 2) ? v1 * v1 : pow(v1, v2);
    case LUA_OPIDIV:
      return Math.floor(v1 / v2);
    case LUA_OPUNM:
      return -v1;
    case LUA_OPMOD:
      return luaV_modf(L, v1, v2);
    default:
      assert false;
      return 0;
    }
  }

  public static int luaO_rawarith(lua_State L, int op, TValue p1, TValue p2,
                                  TValue res)
  {
    switch (op)
    {
    case LUA_OPBAND:
    case LUA_OPBOR:
    case LUA_OPBXOR:
    case LUA_OPSHL:
    case LUA_OPSHR:
    case LUA_OPBNOT:
    { /* operate only on integers */
      IntResult i1 = new IntResult();
      IntResult i2 = new IntResult();
      if (tointegerns(p1, i1) != 0 && tointegerns(p2, i2) != 0)
      {
        setivalue(res, intarith(L, op, i1.v, i2.v));
        return 1;
      }
      else
        return 0; /* fail */
    }
    case LUA_OPDIV:
    case LUA_OPPOW:
    { /* operate only on floats */
      NumResult n1 = new NumResult();
      NumResult n2 = new NumResult();
      if (tonumberns(p1, n1) != 0 && tonumberns(p2, n2) != 0)
      {
        setfltvalue(res, numarith(L, op, n1.v, n2.v));
        return 1;
      }
      else
        return 0; /* fail */
    }
    default:
    { /* other operations */
      NumResult n1 = new NumResult();
      NumResult n2 = new NumResult();
      if (ttisinteger(p1) && ttisinteger(p2))
      {
        setivalue(res, intarith(L, op, ivalue(p1), ivalue(p2)));
        return 1;
      }
      else if (tonumberns(p1, n1) != 0 && tonumberns(p2, n2) != 0)
      {
        setfltvalue(res, numarith(L, op, n1.v, n2.v));
        return 1;
      }
      else
        return 0; /* fail */
    }
    }
  }

  public static void luaO_arith(lua_State L, int op, int p1slot, int p2slot,
                                int res)
  {
    TValue p1v = getslot(L, p1slot);
    TValue p2v = getslot(L, p2slot);
    TValue p1 = p1v;
    TValue p2 = p2v;
    TValue r = new TValue();
    if (luaO_rawarith(L, op, p1, p2, r) == 0)
    {
      /* could not perform raw operation; try metamethod */
      luaT_trybinTM(L, p1, p2, res, (op - LUA_OPADD) + TM_ADD, NOSLOT, NOSLOT);
    }
    else
      setslot(L, res, r);
  }

  public static byte luaO_hexavalue(int c)
  {
    assert lisxdigit(c) != 0;
    if (lisdigit(c) != 0)
      return (byte) (c - '0');
    else
      return (byte) ((ltolower(c) - 'a') + 10);
  }

  private static final class Sign
  {
    int neg;
    int off;

    Sign(int neg, int off)
    {
      this.neg = neg;
      this.off = off;
    }
  }

  private static Sign isneg(ByteView s, int off)
  {
    if (sv_byteat(s, off) == '-')
      return new Sign(1, off + 1);
    else if (sv_byteat(s, off) == '+')
      return new Sign(0, off + 1);
    return new Sign(0, off);
  }

  private static final class NumParse
  {
    int end;
    double value;

    NumParse(int end, double value)
    {
      this.end = end;
      this.value = value;
    }
  }

  private static final class IntParse
  {
    int end;
    long value;

    IntParse(int end, long value)
    {
      this.end = end;
      this.value = value;
    }
  }

  /*
  ** {==================================================================
  ** Lua's implementation for 'lua_strx2number'
  ** ===================================================================
  */


  /* maximum number of significant digits to read (to avoid overflows
     even with single floats) */
  private static final int MAXSIGDIG = 30;

  /*
  ** convert a hexadecimal numeric string to a number, following
  ** C99 specification for 'strtod'
  */
  private static NumParse lua_strx2number(ByteView s, int off)
  {
    int dot = '.';
    double r = 0.0; /* result (accumulator) */
    int sigdig = 0;               /* number of significant digits */
    int nosigdig = 0;             /* number of non-significant digits */
    int e = 0;                    /* exponent correction */
    int neg;                      /* 1 if number is negative */
    int hasdot = 0;               /* true after seen a dot */
    int p = off;
    int endpos = off;                /* nothing is valid yet */
    while (lisspace(sv_byteat(s, p)) != 0)
      p++;                                                    /* skip initial spaces */
    Sign sign = isneg(s, p);                                       /* check sign */
    neg = sign.neg;
    p = sign.off;
    if (!(sv_byteat(s, p) == '0' && (sv_byteat(s, p + 1) == 'x' || sv_byteat(s, p + 1) == 'X'))) /* check '0x' */
      return new NumParse(endpos, 0.0);                       /* invalid format (no '0x') */
    for (p += 2;; p++)
    { /* skip '0x' and read numeral */
      if (sv_byteat(s, p) == dot)
      {
        if (hasdot != 0)
          break; /* second dot? stop loop */
        else
          hasdot = 1;
      }
      else if (lisxdigit(sv_byteat(s, p)) != 0)
      {
        if (sigdig == 0 && s.at(p) == '0') /* non-significant digit (zero)? */
          nosigdig++;
        else if (++sigdig <= MAXSIGDIG) /* can read it without overflow? */
          r = (r * 16.0) + luaO_hexavalue(s.at(p));
        else
          e++; /* too many digits; ignore, but still count for exponent */
        if (hasdot != 0)
          e--; /* decimal digit? correct exponent */
      }
      else
        break; /* neither a dot nor a digit */
    }
    if (nosigdig + sigdig == 0) /* no digits? */
      return new NumParse(endpos, 0.0); /* invalid format */
    endpos = p;                /* valid up to here */
    e *= 4;                     /* each digit multiplies/divides value by 2^4 */
    if (sv_byteat(s, p) == 'p' || sv_byteat(s, p) == 'P')
    {                     /* exponent part? */
      int exp1 = 0;       /* exponent value */
      int neg1;           /* exponent sign */
      p++;                /* skip 'p' */
      Sign sign1 = isneg(s, p); /* sign */
      neg1 = sign1.neg;
      p = sign1.off;
      if (lisdigit(sv_byteat(s, p)) == 0)
        return new NumParse(endpos, 0.0);  /* invalid; must have at least one digit */
      while (lisdigit(sv_byteat(s, p)) != 0) /* read exponent */
        exp1 = exp1 * 10 + s.at(p++) - '0';
      if (neg1 != 0)
        exp1 = -exp1;
      e += exp1;
      endpos = p; /* valid up to here */
    }
    if (neg != 0)
      r = -r;
    return new NumParse(endpos, Math.scalb(r, e));
  }

  /* }====================================================== */

  /* maximum length of a numeral to be converted to a number */
  private static final int L_MAXLENNUM = 200;

  private static NumParse str2dec(ByteView s, int off)
  {
    int p = off;
    while (lisspace(sv_byteat(s, p)) != 0)
      p++;
    int neg = 0;
    if (sv_byteat(s, p) == '+' || sv_byteat(s, p) == '-')
    {
      neg = (s.at(p) == '-') ? 1 : 0;
      p++;
    }
    if (sv_byteat(s, p) == '+' || sv_byteat(s, p) == '-')
      return new NumParse(off, 0.0);
    ToChars.ParseResult res = s.parse_number(p, CharsFormat.general);
    double v = res.result;
    int endp = res.next;
    if (res.ec == ToChars.Errc.result_out_of_range)
    {
      long e10 = 0;
      int frac = 0;
      int sig = 0;
      long intdigits = 0;
      long fraczeros = 0;
      int q;
      for (q = p; q < endp; q++)
      {
        if (s.at(q) == '.')
          frac = 1;
        else if (s.at(q) == 'e' || s.at(q) == 'E')
        {
          int x = q + 1;
          int eneg = 0;
          if (sv_byteat(s, x) == '+' || sv_byteat(s, x) == '-')
          {
            eneg = (s.at(x) == '-') ? 1 : 0;
            x++;
          }
          long acc = 0;
          while (lisdigit(sv_byteat(s, x)) != 0)
          {
            if (acc < 1000000000L)
              acc = acc * 10 + (s.at(x) - '0');
            x++;
          }
          e10 = (eneg != 0) ? -acc : acc;
          break;
        }
        else
        {
          if (sig == 0 && s.at(q) != '0')
            sig = 1;
          if (sig != 0 && frac == 0)
            intdigits++;
          else if (sig == 0 && frac != 0)
            fraczeros++;
        }
      }
      long magnitude = e10 + ((intdigits > 0) ? intdigits : -fraczeros);
      v = (magnitude > 0) ? Double.POSITIVE_INFINITY : 0.0;
    }
    else if (res.ec != ToChars.Errc.ok)
      return new NumParse(off, 0.0);
    return new NumParse(endp, (neg != 0) ? -v : v);
  }

  private static NumParse l_str2dloc(ByteView s, int off, int mode)
  {
    NumParse r = (mode == 'x') ? lua_strx2number(s, off) : str2dec(s, off);
    if (r.end == off)
      return new NumParse(ByteView.npos, r.value); /* nothing recognized? */
    while (lisspace(sv_byteat(s, r.end)) != 0)
      r.end++; /* skip trailing spaces */
    return new NumParse((r.end == s.size()) ? r.end : ByteView.npos, r.value);
  }

  /*
  ** Convert string 's' to a Lua number (put in 'result') handling the
  ** current locale.
  ** This function accepts both the current locale or a dot as the radix
  ** mark. If the conversion fails, it may mean number has a dot but
  ** locale accepts something else. In that case, the code copies 's'
  ** to a buffer (because 's' is read-only), changes the dot to the
  ** current locale radix mark, and tries to convert again.
  ** The variable 'mode' checks for special characters in the string:
  ** - 'n' means 'inf' or 'nan' (which should be rejected)
  ** - 'x' means a hexadecimal numeral
  ** - '.' just optimizes the search for the common case (no special chars)
  */
  private static NumParse l_str2d(ByteView s, int off)
  {
    int pmode = s.find_first_of(Lt.xXnN_chars, off); /* look for special chars */
    int mode = (pmode != ByteView.npos)
                   ? ltolower(s.at(pmode) & 0xFF)
                   : 0;
    if (mode == 'n') /* reject 'inf' and 'nan' */
      return new NumParse(ByteView.npos, 0.0);
    NumParse r = l_str2dloc(s, off, mode); /* try to convert */
    if (r.end == ByteView.npos)
    {
      ByteOwner buff = new ByteOwner(L_MAXLENNUM + 1);
      byte point = locale_radixpoint();
      int ppoint = (point != '.') ? s.find(point, off) : ByteView.npos;
      int len = s.size() - off;
      if (ppoint == ByteView.npos || len > L_MAXLENNUM)
        return new NumParse(ByteView.npos, r.value);
      buff.apply(s.substr(off, len));
      buff.set(ppoint - off, '.');
      r = l_str2dloc(buff.view().substr(0, len), 0, mode); /* try again */
      if (r.end != ByteView.npos)
        r.end = off + r.end; /* make relative to 's' */
    }
    return r;
  }

  private static final long MAXBY10 = LUA_MAXINTEGER / 10;
  private static final int MAXLASTD = (int) (LUA_MAXINTEGER % 10);

  private static IntParse l_str2int(ByteView s, int off)
  {
    long a = 0;
    int empty = 1;
    int neg;
    int p = off;
    while (lisspace(sv_byteat(s, p)) != 0)
      p++; /* skip initial spaces */
    Sign sign = isneg(s, p);
    neg = sign.neg;
    p = sign.off;
    if (sv_byteat(s, p) == '0' &&
        (sv_byteat(s, p + 1) == 'x' || sv_byteat(s, p + 1) == 'X'))
    {         /* hex? */
      p += 2; /* skip '0x' */
      for (; lisxdigit(sv_byteat(s, p)) != 0; p++)
      {
        a = a * 16 + luaO_hexavalue(s.at(p));
        empty = 0;
      }
    }
    else
    { /* decimal */
      for (; lisdigit(sv_byteat(s, p)) != 0; p++)
      {
        int d = s.at(p) - '0';
        if (Long.compareUnsigned(a, MAXBY10) >= 0 && (Long.compareUnsigned(a, MAXBY10) > 0 || d > MAXLASTD + neg)) /* overflow? */
          return new IntParse(ByteView.npos, 0);                                                                 /* do not accept it (as integer) */
        a = a * 10 + d;
        empty = 0;
      }
    }
    while (lisspace(sv_byteat(s, p)) != 0)
      p++; /* skip trailing spaces */
    if (empty != 0 || p != s.size())
      return new IntParse(ByteView.npos, 0); /* something wrong in the numeral */
    else
      return new IntParse(p, (neg != 0) ? 0 - a : a);
  }

  public static int luaO_str2num(ByteView s, TValue o)
  {
    IntParse i = l_str2int(s, 0);
    int e = i.end;
    if (e != ByteView.npos)
    { /* try as an integer */
      setivalue(o, i.value);
    }
    else
    { /* else try as a float */
      NumParse n = l_str2d(s, 0);
      e = n.end;
      if (e == ByteView.npos)
        return 0; /* conversion failed */
      setfltvalue(o, n.value);
    }
    assert e == s.size();
    return e + 1;
  }

  public static int luaO_utf8esc(ByteSpan buff, long x)
  {
    int n = 1; /* number of bytes put in buffer (backwards) */
    assert 0 <= x && x <= 0x7FFFFFFF;
    if (x < 0x80) /* ASCII? */
      buff.set(UTF8BUFFSZ - 1, (byte) x);
    else
    {                          /* need continuation bytes */
      int mfb = 0x3f; /* maximum that fits in first byte */
      do
      { /* add continuation bytes */
        buff.set(UTF8BUFFSZ - (n++), (byte) (0x80 | (x & 0x3f)));
        x >>= 6;   /* remove added bits */
        mfb >>= 1; /* now there is one less bit available in first byte */
      } while (x > mfb); /* still needs continuation byte? */
      buff.set(UTF8BUFFSZ - n, (byte) ((~mfb << 1) | x)); /* add first byte */
    }
    return n;
  }

  /*
  ** Convert a float to a string, adding it to a buffer. First try with
  ** a not too large number of digits, to avoid noise (for instance,
  ** 1.1 going to "1.1000000000000001"). If that lose precision, so
  ** that reading the result back gives a different number, then do the
  ** conversion again with extra precision. Moreover, if the numeral looks
  ** like an integer (without a decimal point or an exponent), add ".0" to
  ** its end.
  */
  private static int tostringbuffFloat(double n, ByteSpan buff)
  {
    int len = buff.write_number(n, CharsFormat.general, 15);
    ByteView view = buff.view().substr(0, len);
    boolean bneg = buff.view().starts_with('-');
    ToChars.ParseResult ret = view.parse_number(bneg ? 1 : 0, CharsFormat.general);
    double check = ret.result;
    if (bneg)
    {
      check = -check;
    }
    if (check != n)
    {
      len = buff.write_number(n, CharsFormat.general, 17);
      view = buff.view().substr(0, len); /* refresh: len just changed */
    }
    if (view.find_first_not_of(Lt.digits_and_dash) == ByteView.npos)
    {
      buff.set(len++, '.');
      buff.set(len++, '0'); /* adds '.0' to result */
    }
    byte point = locale_radixpoint();
    if (point != '.')
    {
      view = buff.view().substr(0, len); // update as len could move
      int dotpos = view.find('.');
      if (dotpos != ByteView.npos)
        buff.set(dotpos, point);
    }
    return len;
  }

  /*
  ** Convert a number object to a string, adding it to a buffer.
  */
  public static int luaO_tostringbuff(TValue obj, ByteSpan buff)
  {
    int len;
    assert ttisnumber(obj);
    assert buff.size() >= LUA_N2SBUFFSZ;
    if (ttisinteger(obj))
    {
      ByteSpan bs = buff;
      len = bs.write_number(ivalue(obj));
    }
    else
    {
      assert ttisfloat(obj);
      len = tostringbuffFloat(fltvalue(obj), buff);
    }
    assert len < LUA_N2SBUFFSZ;
    return len;
  }

  /*
  ** Convert a number object to a Lua string, replacing the value at 'obj'
  */
  public static void luaO_tostring(lua_State L, TValue obj)
  {
    ByteOwner buff = new ByteOwner(LUA_N2SBUFFSZ);
    int len = luaO_tostringbuff(obj, buff.span());
    ByteView bytes = buff.view().substr(0, len);
    setsvalue(L, obj, TString.luaS_createstr(L, bytes));
  }

  /*
  ** {==================================================================
  ** 'luaO_pushvfstring'
  ** ===================================================================
  */

  private static final class BuffFS
  {
    lua_State L;
    ByteOwner b;
    int blen;
    int err;
  }

  private static void initbuff(lua_State L, BuffFS buff)
  {
    buff.L = L;
    buff.b = new ByteOwner();
    buff.blen = 0;
    buff.err = 0;
  }

  private static void freebuff(BuffFS buff)
  {
    global_State g = buff.L.l_G;
    if (buff.b.held())
    {
      luaM_alloc_char_span(g, null, buff.b.span(), 0, false);
      buff.b = new ByteOwner();
    }
  }

  private static void pushbuff(lua_State L, BuffFS buff)
  {
    if (buff.err == 1)
      luaD_throw(L, (byte) LUA_ERRMEM);
    ByteView res = new ByteView(buff.b.array(), 0, buff.blen);
    TString ts;
    if (buff.err == 2)
    { /* (too long to be short) */
      ts = TString.luaS_createlngstrobj(L, res.size() + Lt.ellipsis.size());
      ts.data.area().copy_from(res, 0);
      ts.data.area().copy_from(Lt.ellipsis, res.size());
    }
    else
      ts = TString.luaS_createstr(L, res);
    setslots(L, L.top, ts);
    L.top++;
  }

  private static final class PushBuff implements ProtectedCall
  {
    BuffFS buff;

    @Override
    public void run(lua_State L)
    {
      pushbuff(L, buff);
    }
  }

  private static InternalString clearbuff(BuffFS buff)
  {
    lua_State L = buff.L;
    InternalString res;
    PushBuff pb = new PushBuff();
    pb.buff = buff;
    if (luaD_rawrunprotected(L, pb) != LUA_OK) /* errors? */
      res = null;                              /* error message is on the top of the stack */
    else
    {
      int s = L.top - 1;
      assert novariant(slottag(L, s)) == LUA_TSTRING;
      res = slottsval(L, s).data;
    }
    freebuff(buff);
    return res;
  }

  private static void addstr2buff(BuffFS buff, ByteView bv)
  {
    if (buff.err != 0) /* do nothing else after an error */
      return;
    int slen = bv.size();
    if (slen > (MAX_SIZE / 2) - buff.blen)
    {                /* overflow? */
      buff.err = 2;
      return;
    }
    if (buff.blen + slen > buff.b.size())
    {
      global_State g = buff.L.l_G;
      int newsize = Math.max(buff.blen + slen, 256);
      ByteOwner nb = luaM_alloc_char_span(g, null, buff.b.span(), newsize, false);
      if (!nb.held())
      {
        buff.err = 1; /* memory error */
        return;
      }
      buff.b = nb;
    }
    buff.b.span().copy_from(bv, buff.blen);
    buff.blen += slen;
  }

  /*
  ** Add a numeral to the buffer.
  */
  private static void addnum2buff(BuffFS buff, TValue num)
  {
    ByteOwner numbuff = new ByteOwner(LUA_N2SBUFFSZ);
    int len = luaO_tostringbuff(num, numbuff.span());
    addstr2buff(buff, numbuff.view().substr(0, len));
  }

  public static InternalString luaO_pushvfstring(lua_State L,
                                                 ByteView fmt,
                                                 FmtArgs args)
  {
    BuffFS buff = new BuffFS(); /* holds last part of the result */
    ByteView f = fmt;
    int argn = 0;
    initbuff(L, buff);
    FmtScan sc = new FmtScan(f);
    while (sc.next())
    {
      addstr2buff(buff, f.substr(sc.litStart, sc.litEnd - sc.litStart));
      switch (sc.spec)
      { /* conversion specifier */
      case 's':
      {
        assert argn < args.n && args.arg[argn].tag == 'S';
        addstr2buff(buff, args.arg[argn++].s);
        break;
      }
      case 'c':
      { /* an 'int' as a character */
        assert argn < args.n && args.arg[argn].tag == 'c';
        byte[] c = {(byte) args.arg[argn++].i};
        addstr2buff(buff, new ByteView(c, 0, c.length));
        break;
      }
      case 'd':
      { /* an 'int' */
        assert argn < args.n && args.arg[argn].tag == 'd';
        TValue num = new TValue();
        setivalue(num, args.arg[argn++].i);
        addnum2buff(buff, num);
        break;
      }
      case 'I':
      { /* a 'lua_Integer' */
        assert argn < args.n && args.arg[argn].tag == 'I';
        TValue num = new TValue();
        setivalue(num, args.arg[argn++].i);
        addnum2buff(buff, num);
        break;
      }
      case 'f':
      { /* a 'lua_Number' */
        assert argn < args.n && args.arg[argn].tag == 'f';
        TValue num = new TValue();
        setfltvalue(num, args.arg[argn++].n);
        addnum2buff(buff, num);
        break;
      }
      case 'p':
      {
        ByteOwner bf = new ByteOwner(LUA_N2SBUFFSZ);
        assert argn < args.n && args.arg[argn].tag == 'p';
        Object p = args.arg[argn++].p;
        int bfn = pointertext(bf.span(), 0, p);
        addstr2buff(buff, bf.view().substr(0, bfn));
        break;
      }
      case 'U':
      {
        assert argn < args.n && args.arg[argn].tag == 'U';
        ByteOwner bf = new ByteOwner(UTF8BUFFSZ);
        int len = luaO_utf8esc(bf.span(), args.arg[argn++].i);
        addstr2buff(buff, bf.view().substr(UTF8BUFFSZ - len));
        break;
      }
      case -1:
      case '%':
      {
        addstr2buff(buff, Lt.percent_ch);
        break;
      }
      default:
      {
        addstr2buff(buff, f.substr(sc.escPos, 2)); /* keep unknown format in the result */
        break;
      }
      }
    }
    addstr2buff(buff, f.substr(sc.litStart, sc.litEnd - sc.litStart)); /* rest of 'fmt' */
    assert argn == args.n;
    return clearbuff(buff); /* empty buffer into a new string */
  }

  public static int luaO_fmtargcount(ByteView fmt)
  {
    int n = 0;
    FmtScan sc = new FmtScan(fmt);
    while (sc.next())
    {
      switch (sc.spec)
      {
      case 's':
      case 'c':
      case 'd':
      case 'I':
      case 'f':
      case 'p':
      case 'U':
        n++;
        break;
      default:
        break;
      }
    }
    return n;
  }

  public static InternalString luaO_pushfstring(lua_State L, ByteView fmt, FmtArgs args)
  {
    InternalString msg = luaO_pushvfstring(L, fmt, args);
    if (msg == null) /* error? */
      luaD_throw(L, (byte) LUA_ERRMEM);
    return msg;
  }

  /* }================================================================== */

  public static void luaO_chunkid(ByteSpan out, ByteView source)
  {
    assert out.size() >= LUA_IDSIZE;
    int srclen = source.size();
    int bufflen = LUA_IDSIZE; /* free space in buffer */
    int pos = 0;
    if (sv_byteat(source, 0) == '=')
    {                        /* 'literal' source */
      if (srclen <= bufflen) /* small enough? */
      {
        out.copy_from(source.substr(1, srclen - 1));
        out.set(srclen - 1, '\0');
      }
      else
      { /* truncate it */
        out.copy_from(source.substr(1, bufflen - 1));
        out.set(bufflen - 1, '\0');
      }
    }
    else if (sv_byteat(source, 0) == '@')
    {                        /* file name */
      if (srclen <= bufflen) /* small enough? */
      {
        out.copy_from(source.substr(1, srclen - 1));
        out.set(srclen - 1, '\0');
      }
      else
      { /* add '...' before rest of name */
        ByteView bytes = Lt.ellipsis;
        out.copy_from(bytes);
        pos += Lt.ellipsis.size();
        bufflen -= Lt.ellipsis.size();
        out.copy_from(source.substr(1 + srclen - bufflen, bufflen - 1), pos);
        out.set(pos + bufflen - 1, '\0');
      }
    }
    else
    {                                                 /* string; format as [string "source"] */
      int nl = source.find('\n');
      ByteView bytes = Lt.string_2;
      out.copy_from(bytes); /* add prefix */
      pos += Lt.string_2.size();
      bufflen -= Lt.string_2.size() + Lt.ellipsis.size() + Lt.backquote.size() + 1; /* save space for prefix+suffix+'\0' */
      if (srclen < bufflen && nl == ByteView.npos)
      {
        out.copy_from(source.substr(0, srclen), pos);
        pos += srclen;
      }
      else
      {
        if (nl != ByteView.npos)
          srclen = nl; /* stop at first newline */
        if (srclen > bufflen)
          srclen = bufflen;
        out.copy_from(source.substr(0, srclen), pos);
        pos += srclen;
        bytes = Lt.ellipsis;
        out.copy_from(bytes, pos);
        pos += Lt.ellipsis.size();
      }
      bytes = Lt.backquote;
      out.copy_from(bytes, pos);
      out.set(pos + Lt.backquote.size(), '\0');
    }
  }
}
