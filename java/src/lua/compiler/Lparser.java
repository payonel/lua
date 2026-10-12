package lua.compiler;

import static lua.api.LuaApi.*;
import static lua.bytes.Lload.*;
import static lua.compiler.Lcode.*;
import static lua.compiler.Llex.*;
import static lua.gc.Lgc.*;
import static lua.object.Lfunc.*;
import static lua.object.Lobject.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lmem.*;
import static lua.runtime.Lstate.*;
import static lua.vm.Lopcodes.*;

import lua.bytes.ByteView;
import lua.bytes.InternalString;
import lua.bytes.LoadFrame;
import lua.bytes.Lt;
import lua.bytes.MBuffer;
import lua.bytes.ZIO;
import lua.object.FmtArgs;
import lua.object.LClosure;
import lua.object.LocVar;
import lua.object.Proto;
import lua.object.TString;
import lua.object.TValue;
import lua.object.Table;
import lua.object.Upvaldesc;
import lua.runtime.lua_State;

public final class Lparser
{
  private Lparser()
  {
  }

  /*
  ** Expression and variable descriptor.
  ** Code generation for variables and expressions can be delayed to allow
  ** optimizations; An 'expdesc' structure describes a potentially-delayed
  ** variable/expression. It has a description of its "main" value plus a
  ** list of conditional jumps that can also produce its value (generated
  ** by short-circuit operators 'and'/'or').
  */

  /* kinds of variables/expressions */
  public static final int VVOID = 0;      /* when 'expdesc' describes the last expression of a list,
                                             this kind means an empty list (so, no expression) */
  public static final int VNIL = 1;       /* constant nil */
  public static final int VTRUE = 2;      /* constant true */
  public static final int VFALSE = 3;     /* constant false */
  public static final int VK = 4;         /* constant in 'k'; info = index of constant in 'k' */
  public static final int VKFLT = 5;      /* floating constant; nval = numerical float value */
  public static final int VKINT = 6;      /* integer constant; ival = numerical integer value */
  public static final int VKSTR = 7;      /* string constant; strval = TString address;
                                             (string is fixed by the scanner) */
  public static final int VNONRELOC = 8;  /* expression has its value in a fixed register;
                                             info = result register */
  public static final int VLOCAL = 9;     /* local variable; var.ridx = register index;
                                             var.vidx = relative index in 'actvar.arr'  */
  public static final int VVARGVAR = 10;  /* vararg parameter; var.ridx = register index;
                                             var.vidx = relative index in 'actvar.arr'  */
  public static final int VGLOBAL = 11;   /* global variable;
                                             info = relative index in 'actvar.arr' (or -1 for
                                                    implicit declaration) */
  public static final int VUPVAL = 12;    /* upvalue variable; info = index of upvalue in 'upvalues' */
  public static final int VCONST = 13;    /* compile-time <const> variable;
                                             info = absolute index in 'actvar.arr'  */
  public static final int VINDEXED = 14;  /* indexed variable;
                                             ind.t = table register;
                                             ind.idx = key's R index;
                                             ind.ro = true if it represents a read-only global;
                                             ind.keystr = if key is a string, index in 'k' of that string;
                                                          -1 if key is not a string */
  public static final int VVARGIND = 15;  /* indexed vararg parameter;
                                             ind.* as in VINDEXED */
  public static final int VINDEXUP = 16;  /* indexed upvalue;
                                             ind.idx = key's K index;
                                             ind.* as in VINDEXED */
  public static final int VINDEXI = 17;   /* indexed variable with constant integer;
                                               ind.t = table register;
                                               ind.idx = key's value */
  public static final int VINDEXSTR = 18; /* indexed variable with literal string;
                                             ind.idx = key's K index;
                                             ind.* as in VINDEXED */
  public static final int VJMP = 19;      /* expression is a test/comparison;
                                             info = pc of corresponding jump instruction */
  public static final int VRELOC = 20;    /* expression can put result in any register;
                                             info = instruction pc */
  public static final int VCALL = 21;     /* expression is a function call; info = instruction pc */
  public static final int VVARARG = 22;   /* vararg expression; info = instruction pc */

  public static int vkisvar(int k)
  {
    return (VLOCAL <= k && k <= VINDEXSTR) ? 1 : 0;
  }

  public static int vkisindexed(int k)
  {
    return (VINDEXED <= k && k <= VINDEXSTR) ? 1 : 0;
  }

  /* kinds of variables */
  public static final int VDKREG = 0;     /* regular local */
  public static final int RDKCONST = 1;   /* local constant */
  public static final int RDKVAVAR = 2;   /* vararg parameter */
  public static final int RDKTOCLOSE = 3; /* to-be-closed */
  public static final int RDKCTC = 4;     /* local compile-time constant */
  public static final int GDKREG = 5;     /* regular global */
  public static final int GDKCONST = 6;   /* global constant */

  /* variables that live in registers */
  public static int varinreg(Vardesc v)
  {
    return (v.kind <= RDKTOCLOSE) ? 1 : 0;
  }

  /* test for global variables */
  public static int varglobal(Vardesc v)
  {
    return (v.kind >= GDKREG) ? 1 : 0;
  }

  /*
  ** A load that a freeze can stop (see lload.h): a function with calls a
  ** freeze can pass through runs as a state machine, its state and its
  ** locals in its frame ('l'). Each such call is made at a state of its own,
  ** the one its function goes to again when it is made again after the
  ** thaw (the call made again, which goes on where it stopped); a freeze
  ** passing records the frame. A function's frame may hold pointers into
  ** its callers' frames (a chain of blocks or functions, the table of a
  ** constructor): made again, it links them again.
  */

  /* maximum number of variable declarations per function (must be
     smaller than 250, due to the bytecode format) */
  private static final int MAXVARS = 200;

  private static int hasmultret(int k)
  {
    return (k == VCALL || k == VVARARG) ? 1 : 0;
  }

  /* because all strings are unified by the scanner, the parser
     can use pointer equality for string equality */
  private static int eqstr(TString a, TString b)
  {
    return (a == b) ? 1 : 0;
  }

  private static RuntimeException error_expected(LexState ls, int token)
  {
    throw luaX_syntaxerror(ls, luaO_pushfstring(ls.L, Lt.s_expected, new FmtArgs().str(luaX_token2str(ls, token))).view());
  }

  private static RuntimeException errorlimit(FuncState fs, int limit, ByteView what)
  {
    lua_State L = fs.ls.L;
    InternalString msg;
    int line = fs.f.linedefined;
    ByteView where = (line == 0)
                         ? Lt.main_function
                         : luaO_pushfstring(L, Lt.function_at_line_d, new FmtArgs().num(line)).view();
    msg = luaO_pushfstring(L, Lt.too_many_s_limit_is_d_in_s, new FmtArgs().str(what).num(limit).str(where));
    throw luaX_syntaxerror(fs.ls, msg.view());
  }

  public static void luaY_checklimit(FuncState fs, int v, int l, ByteView what)
  {
    if (v > l)
      errorlimit(fs, l, what);
  }

  /*
  ** Test whether next token is 'c'; if so, skip it.
  */
  /*
  ** (The calls below with no frame of their own, see lload.h, do nothing
  ** before the call that can stop but read what a stop leaves as it was:
  ** made again, they go the same way.)
  */
  private static int testnext(LexState ls, int c)
  {
    if (ls.t.token == c)
    {
      luaX_next(ls);
      return 1;
    }
    else
      return 0;
  }

  /*
  ** Check that next token is 'c'.
  */
  private static void check(LexState ls, int c)
  {
    if (ls.t.token != c)
      error_expected(ls, c);
  }

  /*
  ** Check that next token is 'c' and skip it.
  */
  private static void checknext(LexState ls, int c)
  {
    check(ls, c);
    luaX_next(ls);
  }

  private static void check_condition(LexState ls, int c, ByteView msg)
  {
    if (c == 0)
      luaX_syntaxerror(ls, msg);
  }

  /*
  ** Check that next token is 'what' and skip it. In case of error,
  ** raise an error that the expected 'what' should match a 'who'
  ** in line 'where' (if that is not the current line).
  */
  private static void check_match(LexState ls, int what, int who, int where)
  {
    if (testnext(ls, what) == 0)
    {
      if (where == ls.linenumber) /* all in the same line? */
        error_expected(ls, what);  /* do not need a complex message */
      else
      {
        luaX_syntaxerror(ls, luaO_pushfstring(ls.L,
                                              Lt.s_expected_to_close_s_at_line_d,
                                              new FmtArgs()
                                                  .str(luaX_token2str(ls, what))
                                                  .str(luaX_token2str(ls, who))
                                                  .num(where)).view());
      }
    }
  }

  private static TString str_checkname(LexState ls)
  {
    TString ts;
    check(ls, TK_NAME);
    ts = ls.t.ts;
    luaX_next(ls);
    return ts;
  }

  private static void init_exp(expdesc e, int k, int i)
  {
    e.f = e.t = NO_JUMP;
    e.k = k;
    e.info = i;
  }

  private static void codestring(expdesc e, TString s)
  {
    e.f = e.t = NO_JUMP;
    e.k = VKSTR;
    e.strval = s;
  }

  private static void codename(LexState ls, expdesc e)
  {
    codestring(e, str_checkname(ls));
  }

  /*
  ** Register a new local variable in the active 'Proto' (for debug
  ** information).
  */
  private static short registerlocalvar(LexState ls, FuncState fs,
                                        TString varname)
  {
    Proto f = fs.f;
    int oldsize = f.locvars.length;
    f.locvars = luaM_safegrow_LocVar_span(ls.L, f.locvars, fs.ndebugvars);
    while (oldsize < f.locvars.length)
      f.locvars[oldsize++].varname = null;
    f.locvars[fs.ndebugvars].varname = varname;
    f.locvars[fs.ndebugvars].startpc = fs.pc;
    luaC_objbarrier(ls.L, f, varname);
    return fs.ndebugvars++;
  }

  /*
  ** Create a new variable with the given 'name' and given 'kind'.
  ** Return its index in the function.
  */
  private static int new_varkind(LexState ls, TString name, byte kind)
  {
    lua_State L = ls.L;
    FuncState fs = ls.fs;
    Dyndata dyd = ls.dyd;
    Vardesc var;
    {
      Vardesc[] s = luaM_safegrow_Vardesc_span(L, dyd.actvar.arr, dyd.actvar.n + 1);
      dyd.actvar.arr = s;
      dyd.actvar.size = s.length;
    }
    var = dyd.actvar.arr[dyd.actvar.n++];
    var.kind = kind; /* default */
    var.name = name;
    return dyd.actvar.n - 1 - fs.firstlocal;
  }

  /*
  ** Create a new local variable with the given 'name' and regular kind.
  */
  private static int new_localvar(LexState ls, TString name)
  {
    return new_varkind(ls, name, (byte) VDKREG);
  }

  /*
  ** Return the "variable description" (Vardesc) of a given variable.
  ** (Unless noted otherwise, all variables are referred to by their
  ** compiler indices.)
  */
  private static Vardesc getlocalvardesc(FuncState fs, int vidx)
  {
    return fs.ls.dyd.actvar.arr[fs.firstlocal + vidx];
  }

  /*
  ** Convert 'nvar', a compiler index level, to its corresponding
  ** register. For that, search for the highest variable below that level
  ** that is in a register and uses its register index ('ridx') plus one.
  */
  private static int reglevel(FuncState fs, int nvar)
  {
    while (nvar-- > 0)
    {
      Vardesc vd = getlocalvardesc(fs, nvar); /* get previous variable */
      if (varinreg(vd) != 0)                  /* is in a register? */
        return vd.ridx + 1;
    }
    return 0; /* no variables in registers */
  }

  /*
  ** Return the number of variables in the register stack for the given
  ** function.
  */
  public static int luaY_nvarstack(FuncState fs)
  {
    return reglevel(fs, fs.nactvar);
  }

  /*
  ** Get the debug-information entry for current variable 'vidx'.
  */
  private static LocVar localdebuginfo(FuncState fs, int vidx)
  {
    Vardesc vd = getlocalvardesc(fs, vidx);
    if (varinreg(vd) == 0)
      return null; /* no debug info. for constants */
    else
    {
      int idx = vd.pidx;
      assert idx < fs.ndebugvars;
      return fs.f.locvars[idx];
    }
  }

  /*
  ** Create an expression representing variable 'vidx'
  */
  private static void init_var(FuncState fs, expdesc e, int vidx)
  {
    e.f = e.t = NO_JUMP;
    e.k = VLOCAL;
    e.var.vidx = (short) vidx;
    e.var.ridx = getlocalvardesc(fs, vidx).ridx;
  }

  /*
  ** Raises an error if variable described by 'e' is read only; moreover,
  ** if 'e' is t[exp] where t is the vararg parameter, change it to index
  ** a real table. (Virtual vararg tables cannot be changed.)
  */
  private static void check_readonly(LexState ls, expdesc e)
  {
    FuncState fs = ls.fs;
    TString varname = null; /* to be set if variable is const */
    switch (e.k)
    {
    case VCONST:
    {
      varname = ls.dyd.actvar.arr[e.info].name;
      break;
    }
    case VLOCAL:
    case VVARGVAR:
    {
      Vardesc vardesc = getlocalvardesc(fs, e.var.vidx);
      if (vardesc.kind != VDKREG) /* not a regular variable? */
        varname = vardesc.name;
      break;
    }
    case VUPVAL:
    {
      Upvaldesc up = fs.f.upvalues[e.info];
      if (up.kind != VDKREG)
        varname = up.name;
      break;
    }
    case VVARGIND:
    {
      needvatab(fs.f); /* function will need a vararg table */
      e.k = VINDEXED;
    } /* FALLTHROUGH */
    case VINDEXUP:
    case VINDEXSTR:
    case VINDEXED:
    {                  /* global variable */
      if (e.ind.ro != 0) /* read-only? */
      {
        TValue kv = fs.f.k[e.ind.keystr];
        assert ttisstring(kv);
        varname = tsvalue(kv);
      }
      break;
    }
    default:
      assert e.k == VINDEXI; /* this one doesn't need any check */
      return;                /* integer index cannot be read-only */
    }
    if (varname != null)
      luaK_semerror(ls, Lt.attempt_to_assign_to_const_variable_s,
                    new FmtArgs().str(varname.data.view()));
  }

  /*
  ** Start the scope for the last 'nvars' created variables.
  */
  private static void adjustlocalvars(LexState ls, int nvars)
  {
    FuncState fs = ls.fs;
    int reglevel = luaY_nvarstack(fs);
    int i;
    for (i = 0; i < nvars; i++)
    {
      int vidx = fs.nactvar++;
      Vardesc var = getlocalvardesc(fs, vidx);
      var.ridx = reglevel++;
      var.pidx = registerlocalvar(ls, fs, var.name);
      luaY_checklimit(fs, reglevel, MAXVARS, Lt.local_variables);
    }
  }

  /*
  ** Close the scope for all variables up to level 'tolevel'.
  ** (debug info.)
  */
  private static void removevars(FuncState fs, int tolevel)
  {
    fs.ls.dyd.actvar.n -= fs.nactvar - tolevel;
    while (fs.nactvar > tolevel)
    {
      LocVar var = localdebuginfo(fs, --fs.nactvar);
      if (var != null) /* does it have debug information? */
        var.endpc = fs.pc;
    }
  }

  /*
  ** Search the upvalues of the function 'fs' for one
  ** with the given 'name'.
  */
  private static int searchupvalue(FuncState fs, TString name)
  {
    int i;
    Upvaldesc[] up = fs.f.upvalues;
    for (i = 0; i < fs.nups; i++)
    {
      if (eqstr(up[i].name, name) != 0)
        return i;
    }
    return -1; /* not found */
  }

  private static Upvaldesc allocupvalue(FuncState fs)
  {
    Proto f = fs.f;
    int oldsize = f.upvalues.length;
    luaY_checklimit(fs, fs.nups + 1, MAXUPVAL, Lt.upvalues);
    f.upvalues = luaM_safegrow_Upvaldesc_span(fs.ls.L, f.upvalues, fs.nups);
    while (oldsize < f.upvalues.length)
      f.upvalues[oldsize++].name = null;
    return f.upvalues[fs.nups++];
  }

  private static int newupvalue(FuncState fs, TString name, expdesc v)
  {
    Upvaldesc up = allocupvalue(fs);
    FuncState prev = fs.prev;
    if (v.k == VLOCAL)
    {
      up.instack = 1;
      up.idx = v.var.ridx;
      up.kind = getlocalvardesc(prev, v.var.vidx).kind;
      assert eqstr(name, getlocalvardesc(prev, v.var.vidx).name) != 0;
    }
    else
    {
      up.instack = 0;
      up.idx = v.info;
      up.kind = prev.f.upvalues[v.info].kind;
      assert eqstr(name, prev.f.upvalues[v.info].name) != 0;
    }
    up.name = name;
    luaC_objbarrier(fs.ls.L, fs.f, name);
    return fs.nups - 1;
  }

  private static int searchvar(FuncState fs, TString n, expdesc var)
  {
    int i;
    for (i = (int) fs.nactvar - 1; i >= 0; i--)
    {
      Vardesc vd = getlocalvardesc(fs, i);
      if (varglobal(vd) != 0)
      { /* global declaration? */
        if (vd.name == null)
        {                                    /* collective declaration? */
          if (var.info < 0)                /* no previous collective declaration? */
            var.info = fs.firstlocal + i; /* this is the first one */
        }
        else
        { /* global name */
          if (eqstr(n, vd.name) != 0)
          { /* found? */
            init_exp(var, VGLOBAL, fs.firstlocal + i);
            return VGLOBAL;
          }
          else if (var.info == -1) /* active preambular declaration? */
            var.info = -2;         /* invalidate preambular declaration */
        }
      }
      else if (eqstr(n, vd.name) != 0)
      {                           /* found? */
        if (vd.kind == RDKCTC) /* compile-time constant? */
          init_exp(var, VCONST, fs.firstlocal + i);
        else
        { /* local variable */
          init_var(fs, var, i);
          if (vd.kind == RDKVAVAR) /* vararg parameter? */
            var.k = VVARGVAR;
        }
        return var.k;
      }
    }
    return -1; /* not found */
  }

  /*
  ** Mark block where variable at given level was defined
  ** (to emit close instructions later).
  */
  private static void markupval(FuncState fs, int level)
  {
    BlockCnt bl = fs.bl;
    while (bl.nactvar > level)
      bl = bl.previous;
    bl.upval = 1;
    fs.needclose = 1;
  }

  /*
  ** Mark that current block has a to-be-closed variable.
  */
  private static void marktobeclosed(FuncState fs)
  {
    BlockCnt bl = fs.bl;
    bl.upval = 1;
    bl.insidetbc = 1;
    fs.needclose = 1;
  }

  /*
  ** Find a variable with the given name 'n'. If it is an upvalue, add
  ** this upvalue into all intermediate functions. If it is a global, set
  ** 'var' as 'void' as a flag.
  */
  private static void singlevaraux(FuncState fs, TString n, expdesc var, int base)
  {
    int v = searchvar(fs, n, var); /* look up variables at current level */
    if (v >= 0)
    { /* found? */
      if (base == 0)
      {
        if (var.k == VVARGVAR)      /* vararg parameter? */
          luaK_vapar2local(fs, var); /* change it to a regular local */
        if (var.k == VLOCAL)
          markupval(fs, var.var.vidx); /* will be used as an upvalue */
      }
      /* else nothing else to be done */
    }
    else
    {                                 /* not found at current level; try upvalues */
      int idx = searchupvalue(fs, n); /* try existing upvalues */
      if (idx < 0)
      {                                           /* not found? */
        if (fs.prev != null)                     /* more levels? */
          singlevaraux(fs.prev, n, var, 0);      /* try upper levels */
        if (var.k == VLOCAL || var.k == VUPVAL) /* local or upvalue? */
          idx = newupvalue(fs, n, var);           /* will be a new upvalue */
        else                                      /* it is a global or a constant */
          return;                                 /* don't need to do anything at this level */
      }
      init_exp(var, VUPVAL, idx); /* new or old upvalue */
    }
  }

  private static void buildglobal(LexState ls, TString varname, expdesc var)
  {
    FuncState fs = ls.fs;
    expdesc key = new expdesc();
    init_exp(var, VGLOBAL, -1);        /* global by default */
    singlevaraux(fs, ls.envn, var, 1); /* get environment variable */
    if (var.k == VGLOBAL)
      luaK_semerror(ls, Lt.s_is_global_when_accessing_variable_s,
                    new FmtArgs().str(Lt._ENV).str(varname.data.view()));
    luaK_exp2anyregup(fs, var); /* _ENV could be a constant */
    codestring(key, varname);   /* key is variable name */
    luaK_indexed(fs, var, key); /* 'var' represents _ENV[varname] */
  }

  /*
  ** Find a variable with the given name 'n', handling global variables
  ** too.
  */
  private static void buildvar(LexState ls, TString varname, expdesc var)
  {
    FuncState fs = ls.fs;
    init_exp(var, VGLOBAL, -1); /* global by default */
    singlevaraux(fs, varname, var, 1);
    if (var.k == VGLOBAL)
    { /* global name? */
      int info = var.info;
      /* global by default in the scope of a global declaration? */
      if (info == -2)
        luaK_semerror(ls, Lt.variable_s_not_declared,
                      new FmtArgs().str(varname.data.view()));
      buildglobal(ls, varname, var);
      if (info != -1 && ls.dyd.actvar.arr[info].kind == GDKCONST)
        var.ind.ro = 1; /* mark variable as read-only */
      else              /* anyway must be a global */
        assert info == -1 || ls.dyd.actvar.arr[info].kind == GDKREG;
    }
  }

  private static void singlevar(LexState ls, expdesc var)
  {
    buildvar(ls, str_checkname(ls), var);
  }

  /*
  ** Adjust the number of results from an expression list 'e' with 'nexps'
  ** expressions to 'nvars' values.
  */
  private static void adjust_assign(LexState ls, int nvars, int nexps, expdesc e)
  {
    FuncState fs = ls.fs;
    int needed = nvars - nexps; /* extra values needed */
    luaK_checkstack(fs, needed);
    if (hasmultret(e.k) != 0)
    {                         /* last expression has multiple returns? */
      int extra = needed + 1; /* discount last expression itself */
      if (extra < 0)
        extra = 0;
      luaK_setreturns(fs, e, extra); /* last exp. provides the difference */
    }
    else
    {
      if (e.k != VVOID)                   /* at least one expression? */
        luaK_exp2nextreg(fs, e);          /* close last expression */
      if (needed > 0)                     /* missing values? */
        luaK_nil(fs, fs.freereg, needed); /* complete with nils */
    }
    if (needed > 0)
      luaK_reserveregs(fs, needed);    /* registers for extra values */
    else                               /* adding 'needed' is actually a subtraction */
      fs.freereg = fs.freereg + needed; /* remove extra values */
  }

  private static void enterlevel(LexState ls)
  {
    luaE_incCstack(ls.L);
  }

  private static void leavelevel(LexState ls)
  {
    ls.L.nCcalls--;
  }

  /*
  ** Generates an error that a goto jumps into the scope of some
  ** variable declaration.
  */
  private static RuntimeException jumpscopeerror(LexState ls, Labeldesc gt)
  {
    TString tsname = getlocalvardesc(ls.fs, gt.nactvar).name;
    ByteView varname = (tsname != null) ? tsname.data.view() : Lt.star;
    throw luaK_semerror(ls, Lt.goto_s_at_line_d_jumps_into_the_scope_of_s, new FmtArgs().str(gt.name.data.view()).num(gt.line).str(varname)); /* raise the error */
  }

  /*
  ** Closes the goto at index 'g' to given 'label' and removes it
  ** from the list of pending gotos.
  ** If it jumps into the scope of some variable, raises an error.
  ** The goto needs a CLOSE if it jumps out of a block with upvalues,
  ** or out of the scope of some variable and the block has upvalues
  ** (signaled by parameter 'bup').
  */
  private static void closegoto(LexState ls, int g, Labeldesc label, int bup)
  {
    int i;
    FuncState fs = ls.fs;
    Labellist gl = ls.dyd.gt;   /* list of gotos */
    Labeldesc gt = gl.arr[g];   /* goto to be resolved */
    assert eqstr(gt.name, label.name) != 0;
    if (gt.nactvar < label.nactvar) /* enter some scope? */
      jumpscopeerror(ls, gt);
    if (gt.close != 0 ||
        (label.nactvar < gt.nactvar && bup != 0))
    { /* needs close? */
      int stklevel = reglevel(fs, label.nactvar);
      /* move jump to CLOSE position */
      fs.f.code[gt.pc + 1] = fs.f.code[gt.pc];
      /* put CLOSE instruction at original position */
      fs.f.code[gt.pc] = CREATE_ABCk(OP_CLOSE, stklevel, 0, 0, 0);
      gt.pc++; /* must point to jump instruction */
    }
    luaK_patchlist(ls.fs, gt.pc, label.pc); /* goto jumps to label */
    for (i = g; i < gl.n - 1; i++)          /* remove goto from pending list */
      gl.arr[i].copyfrom(gl.arr[i + 1]);
    gl.n--;
  }

  /*
  ** Search for an active label with the given name, starting at
  ** index 'ilb' (so that it can search for all labels in current block
  ** or all labels in current function).
  */
  private static Labeldesc findlabel(LexState ls, TString name, int ilb)
  {
    Dyndata dyd = ls.dyd;
    for (; ilb < dyd.label.n; ilb++)
    {
      Labeldesc lb = dyd.label.arr[ilb];
      if (eqstr(lb.name, name) != 0) /* correct label? */
        return lb;
    }
    return null; /* label not found */
  }

  /*
  ** Adds a new label/goto in the corresponding list.
  */
  private static int newlabelentry(LexState ls, Labellist l, TString name,
                                   int line, int pc)
  {
    int n = l.n;
    {
      Labeldesc[] s = luaM_safegrow_Labeldesc_span(ls.L, l.arr, n);
      l.arr = s;
      l.size = s.length;
    }
    l.arr[n].name = name;
    l.arr[n].line = line;
    l.arr[n].nactvar = ls.fs.nactvar;
    l.arr[n].close = 0;
    l.arr[n].pc = pc;
    l.n = n + 1;
    return n;
  }

  /*
  ** Create an entry for the goto and the code for it. As it is not known
  ** at this point whether the goto may need a CLOSE, the code has a jump
  ** followed by an CLOSE. (As the CLOSE comes after the jump, it is a
  ** dead instruction; it works as a placeholder.) When the goto is closed
  ** against a label, if it needs a CLOSE, the two instructions swap
  ** positions, so that the CLOSE comes before the jump.
  */
  private static int newgotoentry(LexState ls, TString name, int line)
  {
    FuncState fs = ls.fs;
    int pc = luaK_jump(fs);              /* create jump */
    luaK_codeABC(fs, OP_CLOSE, 0, 1, 0); /* spaceholder, marked as dead */
    return newlabelentry(ls, ls.dyd.gt, name, line, pc);
  }

  /*
  ** Create a new label with the given 'name' at the given 'line'.
  ** 'last' tells whether label is the last non-op statement in its
  ** block. Solves all pending gotos to this new label and adds
  ** a close instruction if necessary.
  ** Returns true iff it added a close instruction.
  */
  private static void createlabel(LexState ls, TString name, int line, int last)
  {
    FuncState fs = ls.fs;
    Labellist ll = ls.dyd.label;
    int l = newlabelentry(ls, ll, name, line, luaK_getlabel(fs));
    if (last != 0)
    { /* label is last no-op statement in the block? */
      /* assume that locals are already out of scope */
      ll.arr[l].nactvar = fs.bl.nactvar;
    }
  }

  /*
  ** Traverse the pending gotos of the finishing block checking whether
  ** each match some label of that block. Those that do not match are
  ** "exported" to the outer block, to be solved there. In particular,
  ** its 'nactvar' is updated with the level of the inner block,
  ** as the variables of the inner block are now out of scope.
  */
  private static void solvegotos(FuncState fs, BlockCnt bl)
  {
    LexState ls = fs.ls;
    Labellist gl = ls.dyd.gt;
    int outlevel = reglevel(fs, bl.nactvar); /* level outside the block */
    int igt = bl.firstgoto;                  /* first goto in the finishing block */
    while (igt < gl.n)
    { /* for each pending goto */
      Labeldesc gt = gl.arr[igt];
      /* search for a matching label in the current block */
      Labeldesc lb = findlabel(ls, gt.name, bl.firstlabel);
      if (lb != null)                      /* found a match? */
        closegoto(ls, igt, lb, bl.upval); /* close and remove goto */
      else
      { /* adjust 'goto' for outer block */
        /* block has variables to be closed and goto escapes the scope of
           some variable? */
        if (bl.upval != 0 && reglevel(fs, gt.nactvar) > outlevel)
          gt.close = 1;          /* jump may need a close */
        gt.nactvar = bl.nactvar; /* correct level for outer block */
        igt++;                   /* go to next goto */
      }
    }
    ls.dyd.label.n = bl.firstlabel; /* remove local labels */
  }

  private static void enterblock(FuncState fs, BlockCnt bl, byte isloop)
  {
    bl.isloop = isloop;
    bl.nactvar = fs.nactvar;
    bl.firstlabel = fs.ls.dyd.label.n;
    bl.firstgoto = fs.ls.dyd.gt.n;
    bl.upval = 0;
    /* inherit 'insidetbc' from enclosing block */
    bl.insidetbc = (byte) ((fs.bl != null && fs.bl.insidetbc != 0) ? 1 : 0);
    bl.previous = fs.bl; /* link block in function's block list */
    fs.bl = bl;
    assert fs.freereg == luaY_nvarstack(fs);
  }

  /*
  ** generates an error for an undefined 'goto'.
  */
  private static RuntimeException undefgoto(LexState ls, Labeldesc gt)
  {
    /* breaks are checked when created, cannot be undefined */
    assert eqstr(gt.name, ls.brkn) == 0;
    throw luaK_semerror(ls, Lt.no_visible_label_s_for_goto_at_line_d,
                        new FmtArgs().str(gt.name.data.view()).num(gt.line));
  }

  private static void leaveblock(FuncState fs)
  {
    BlockCnt bl = fs.bl;
    LexState ls = fs.ls;
    int stklevel = reglevel(fs, bl.nactvar); /* level outside block */
    if (bl.previous != null && bl.upval != 0) /* need a 'close'? */
      luaK_codeABC(fs, OP_CLOSE, stklevel, 0, 0);
    fs.freereg = stklevel;              /* free registers */
    removevars(fs, bl.nactvar);         /* remove block locals */
    assert bl.nactvar == fs.nactvar;    /* back to level on entry */
    if (bl.isloop == 2)                 /* has to fix pending breaks? */
      createlabel(ls, ls.brkn, 0, 0);
    solvegotos(fs, bl);
    if (bl.previous == null)
    {                                                 /* was it the last block? */
      if (bl.firstgoto < ls.dyd.gt.n)                 /* still pending gotos? */
        undefgoto(ls, ls.dyd.gt.arr[bl.firstgoto]); /* error */
    }
    fs.bl = bl.previous; /* current block now is previous one */
  }

  /*
  ** adds a new prototype into list of prototypes
  */
  private static Proto addprototype(LexState ls)
  {
    Proto clp;
    lua_State L = ls.L;
    FuncState fs = ls.fs;
    Proto f = fs.f; /* prototype of current function */
    if (fs.np >= f.p.length)
    {
      int oldsize = f.p.length;
      f.p = luaM_safegrow_Proto_ptr_span(L, f.p, fs.np);
      while (oldsize < f.p.length)
        f.p[oldsize++] = null;
    }
    f.p[fs.np++] = clp = Proto.luaF_newproto(L);
    luaC_objbarrier(L, f, clp);
    return clp;
  }

  /*
  ** codes instruction to create new closure in parent function.
  ** The OP_CLOSURE instruction uses the last available register,
  ** so that, if it invokes the GC, the GC knows which registers
  ** are in use at that time.

  */
  private static void codeclosure(LexState ls, expdesc v)
  {
    FuncState fs = ls.fs.prev;
    init_exp(v, VRELOC, luaK_codeABx(fs, OP_CLOSURE, 0, fs.np - 1));
    luaK_exp2nextreg(fs, v); /* fix it at the last register */
  }

  private static void open_func(LexState ls, FuncState fs, BlockCnt bl)
  {
    lua_State L = ls.L;
    Proto f = fs.f;
    fs.prev = ls.fs; /* linked list of funcstates */
    fs.ls = ls;
    ls.fs = fs;
    fs.pc = 0;
    fs.previousline = f.linedefined;
    fs.iwthabs = 0;
    fs.lasttarget = 0;
    fs.freereg = 0;
    fs.nk = 0;
    fs.nabslineinfo = 0;
    fs.np = 0;
    fs.nups = 0;
    fs.ndebugvars = 0;
    fs.nactvar = 0;
    fs.needclose = 0;
    fs.firstlocal = ls.dyd.actvar.n;
    fs.firstlabel = ls.dyd.label.n;
    fs.bl = null;
    f.source = ls.source;
    luaC_objbarrier(L, f, f.source);
    f.maxstacksize = 2;                    /* registers 0/1 are always valid */
    fs.kcache = Table.luaH_new(L);         /* create table for function */
    luaC_givebackhash(L.l_G, fs.kcache);   /* (the parser's own) */
    assert novariant(fs.kcache.tt) >= LUA_TSTRING;
    luaD_anchorobj(L, ls.h, fs.kcache); /* anchor it */
    enterblock(fs, bl, (byte) 0);
  }

  private static void close_func(LexState ls)
  {
    lua_State L = ls.L;
    FuncState fs = ls.fs;
    Proto f = fs.f;
    TValue temp = new TValue();
    luaK_ret(fs, luaY_nvarstack(fs), 0); /* final return */
    leaveblock(fs);
    assert fs.bl == null;
    luaK_finish(fs);
    f.code = luaM_alloc_Instruction_span(L.l_G, L, f.code, fs.pc, true);
    f.lineinfo = luaM_alloc_ls_byte_span(L.l_G, L, f.lineinfo, fs.pc, true);
    f.abslineinfo = luaM_alloc_AbsLineInfo_span(L.l_G, L, f.abslineinfo, fs.nabslineinfo, true);
    f.k = luaM_alloc_TValue_span(L.l_G, L, f.k, fs.nk, true);
    f.p = luaM_alloc_Proto_ptr_span(L.l_G, L, f.p, fs.np, true);
    f.locvars = luaM_alloc_LocVar_span(L.l_G, L, f.locvars, fs.ndebugvars, true);
    f.upvalues = luaM_alloc_Upvaldesc_span(L.l_G, L, f.upvalues, fs.nups, true);
    /* remove kcache table from scanner table ("weigh" its anchor) */
    sethvalue(L, temp, fs.kcache); /* key to be set to nil */
    ls.h.luaH_set(L, temp, L.l_G.nilvalue);
    ls.fs = fs.prev;
    luaC_checkGC(L);
  }

  /*
  ** {======================================================================
  ** GRAMMAR RULES
  ** =======================================================================
  */

  /*
  ** check whether current token is in the follow set of a block.
  ** 'until' closes syntactical blocks, but do not close scope,
  ** so it is handled in separate.
  */
  private static int block_follow(LexState ls, int withuntil)
  {
    switch (ls.t.token)
    {
    case TK_ELSE:
    case TK_ELSEIF:
    case TK_END:
    case TK_EOS:
      return 1;
    case TK_UNTIL:
      return withuntil;
    default:
      return 0;
    }
  }

  private static void statlist(LexState ls)
  {
    /* statlist -> { stat [';'] } */
    LoadFrame l = new LoadFrame();
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_STATLIST));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0: /* the loop */
          if (block_follow(ls, 1) != 0)
            return;
          l.site = (ls.t.token == TK_RETURN) ? 1 : 2;
          continue;
        case 1:
          statement(ls);
          return; /* 'return' must be last statement */
        case 2:
          statement(ls);
          l.site = 0;
          continue;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_STATLIST, l);
      throw ex;
    }
  }

  private static void fieldsel(LexState ls, expdesc v)
  {
    /* fieldsel -> ['.' | ':'] NAME */
    LoadFrame l = new LoadFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_FIELDSEL));
    try
    {
      switch (l.site)
      {
      case 0:
        luaK_exp2anyregup(fs, v);
        l.site = 1;
      case 1:
        luaX_next(ls); /* skip the dot or colon */
        l.site = 2;
      case 2:
      {
        expdesc key = new expdesc();
        codename(ls, key);
        luaK_indexed(fs, v, key);
        break;
      }
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_FIELDSEL, l);
      throw ex;
    }
  }

  private static void yindex(LexState ls, expdesc v)
  {
    /* index -> '[' expr ']' */
    LoadFrame l = new LoadFrame();
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_YINDEX));
    try
    {
      switch (l.site)
      {
      case 0:
        luaX_next(ls); /* skip the '[' */
        l.site = 1;
      case 1:
        expr(ls, v);
        luaK_exp2val(ls.fs, v);
        l.site = 2;
      case 2:
        checknext(ls, ']');
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_YINDEX, l);
      throw ex;
    }
  }

  /*
  ** {======================================================================
  ** Rules for Constructors
  ** =======================================================================
  */

  /*
  ** Maximum number of elements in a constructor, to control the following:
  ** * counter overflows;
  ** * overflows in 'extra' for OP_NEWTABLE and OP_SETLIST;
  ** * overflows when adding multiple returns in OP_SETLIST.
  */
  private static final int MAX_CNST = ((Integer.MAX_VALUE / 2) / (MAXARG_vC + 1) > MAXARG_Ax)
                                          ? MAXARG_Ax * (MAXARG_vC + 1)
                                          : Integer.MAX_VALUE / 2;

  /* the frame of 'recfield' (see lload.h) */
  private static final class RecfieldFrame extends LoadFrame
  {
    int reg = 0;
    expdesc tab = new expdesc(), key = new expdesc(), val = new expdesc();

    RecfieldFrame()
    {
    }

    RecfieldFrame(RecfieldFrame o)
    {
      super(o);
      reg = o.reg;
      tab = new expdesc(o.tab);
      key = new expdesc(o.key);
      val = new expdesc(o.val);
    }

    @Override
    public LoadFrame copy()
    {
      return new RecfieldFrame(this);
    }

    @Override
    public int size()
    {
      return 248;
    }
  }

  private static void recfield(LexState ls, ConsControl cc)
  {
    /* recfield -> (NAME | '['exp']') = exp */
    RecfieldFrame l = new RecfieldFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new RecfieldFrame((RecfieldFrame) luaZ_restore(ls.z.rz, LF_RECFIELD));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.reg = ls.fs.freereg;
          l.site = (ls.t.token == TK_NAME) ? 1 : 2; /* (else '[') */
          continue;
        case 1:
          codename(ls, l.key);
          l.site = 3;
          continue;
        case 2:
          yindex(ls, l.key);
          l.site = 3;
        case 3:
          cc.nh++;
          l.site = 4;
        case 4:
          checknext(ls, '=');
          l.tab.copyfrom(cc.t);
          luaK_indexed(fs, l.tab, l.key);
          l.site = 5;
        case 5:
          expr(ls, l.val);
          luaK_storevar(fs, l.tab, l.val);
          fs.freereg = l.reg; /* free registers */
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_RECFIELD, l);
      throw ex;
    }
  }

  private static void closelistfield(FuncState fs, ConsControl cc)
  {
    assert cc.tostore > 0;
    luaK_exp2nextreg(fs, cc.v);
    cc.v.k = VVOID;
    if (cc.tostore >= cc.maxtostore)
    {
      luaK_setlist(fs, cc.t.info, cc.na, cc.tostore); /* flush */
      cc.na += cc.tostore;
      cc.tostore = 0; /* no more items pending */
    }
  }

  private static void lastlistfield(FuncState fs, ConsControl cc)
  {
    if (cc.tostore == 0)
      return;
    if (hasmultret(cc.v.k) != 0)
    {
      luaK_setmultret(fs, cc.v);
      luaK_setlist(fs, cc.t.info, cc.na, LUA_MULTRET);
      cc.na--; /* do not count last expression (unknown number of elements) */
    }
    else
    {
      if (cc.v.k != VVOID)
        luaK_exp2nextreg(fs, cc.v);
      luaK_setlist(fs, cc.t.info, cc.na, cc.tostore);
    }
    cc.na += cc.tostore;
  }

  private static void listfield(LexState ls, ConsControl cc)
  {
    /* listfield -> exp */
    expr(ls, cc.v);
    cc.tostore++;
  }

  private static void field(LexState ls, ConsControl cc)
  {
    /* field -> listfield | recfield */
    LoadFrame l = new LoadFrame();
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_FIELD));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          switch (ls.t.token)
          {
          case TK_NAME: /* may be 'listfield' or 'recfield' */
            l.site = 1;
            break;
          case '[':
            l.site = 3;
            break;
          default:
            l.site = 2;
            break;
          }
          continue;
        case 1:
          l.site = (luaX_lookahead(ls) != '=') ? 2 : 3; /* expression? */
          continue;
        case 2:
          listfield(ls, cc);
          return;
        case 3:
          recfield(ls, cc);
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_FIELD, l);
      throw ex;
    }
  }

  /*
  ** Compute a limit for how many registers a constructor can use before
  ** emitting a 'SETLIST' instruction, based on how many registers are
  ** available.
  */
  private static int maxtostore(FuncState fs)
  {
    int numfreeregs = MAX_FSTACK - fs.freereg;
    if (numfreeregs >= 160)     /* "lots" of registers? */
      return numfreeregs / 5;   /* use up to 1/5 of them */
    else if (numfreeregs >= 80) /* still "enough" registers? */
      return 10;                /* one 'SETLIST' instruction for each 10 values */
    else                        /* save registers for potential more nesting */
      return 1;
  }

  /* the frame of 'constructor' (see lload.h) */
  private static final class ConstructorFrame extends LoadFrame
  {
    int line = 0;
    int pc = 0;
    ConsControl cc = new ConsControl();

    ConstructorFrame()
    {
    }

    ConstructorFrame(ConstructorFrame o)
    {
      super(o);
      line = o.line;
      pc = o.pc;
      cc = new ConsControl(o.cc);
    }

    @Override
    public LoadFrame copy()
    {
      return new ConstructorFrame(this);
    }

    @Override
    public int size()
    {
      return 128;
    }
  }

  private static void constructor(LexState ls, expdesc t)
  {
    /* constructor -> '{' [ field { sep field } [sep] ] '}'
       sep -> ',' | ';' */
    ConstructorFrame l = new ConstructorFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
    {
      l = new ConstructorFrame((ConstructorFrame) luaZ_restore(ls.z.rz, LF_CONSTRUCTOR));
      l.cc.t = t; /* (again its table) */
    }
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.line = ls.linenumber;
          l.pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
          luaK_code(fs, 0); /* space for extra arg. */
          l.cc.na = l.cc.nh = l.cc.tostore = 0;
          l.cc.t = t;
          init_exp(t, VNONRELOC, fs.freereg); /* table will be at stack top */
          luaK_reserveregs(fs, 1);
          init_exp(l.cc.v, VVOID, 0); /* no value (yet) */
          l.site = 1;
        case 1:
          checknext(ls, '{' /*}*/);
          l.cc.maxtostore = maxtostore(fs);
          l.site = 2;
        case 2: /* the loop */
          if (ls.t.token == /*{*/ '}')
          {
            l.site = 6;
            continue;
          }
          if (l.cc.v.k != VVOID)      /* is there a previous list item? */
            closelistfield(fs, l.cc); /* close it */
          l.site = 3;
        case 3:
          field(ls, l.cc);
          luaY_checklimit(fs, l.cc.tostore + l.cc.na + l.cc.nh, MAX_CNST, Lt.items_in_a_constructor);
          l.site = 4;
        case 4:
          if (testnext(ls, ',') != 0)
          {
            l.site = 2;
            continue;
          }
          l.site = 5;
        case 5:
          if (testnext(ls, ';') != 0)
          {
            l.site = 2;
            continue;
          }
          l.site = 6;
        case 6:
          check_match(ls, /*{*/ '}', '{' /*}*/, l.line);
          lastlistfield(fs, l.cc);
          luaK_settablesize(fs, l.pc, t.info, l.cc.na, l.cc.nh);
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_CONSTRUCTOR, l);
      throw ex;
    }
  }

  /* }====================================================================== */

  private static void setvararg(FuncState fs)
  {
    fs.f.flag |= PF_VAHID; /* by default, use hidden vararg arguments */
    luaK_codeABC(fs, OP_VARARGPREP, 0, 0, 0);
  }

  /* the frame of 'parlist' (see lload.h) */
  private static final class ParlistFrame extends LoadFrame
  {
    int nparams = 0;
    int varargk = 0;

    ParlistFrame()
    {
    }

    ParlistFrame(ParlistFrame o)
    {
      super(o);
      nparams = o.nparams;
      varargk = o.varargk;
    }

    @Override
    public LoadFrame copy()
    {
      return new ParlistFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static void parlist(LexState ls)
  {
    /* parlist -> [ {NAME ','} (NAME | '...') ] */
    ParlistFrame l = new ParlistFrame();
    FuncState fs = ls.fs;
    Proto f = fs.f;
    if (ls.z.resuming)
      l = new ParlistFrame((ParlistFrame) luaZ_restore(ls.z.rz, LF_PARLIST));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.nparams = 0;
          l.varargk = 0;
          if (ls.t.token == ')') /* is 'parlist' empty? */
            break;
          l.site = 1;
        case 1: /* the loop */
          switch (ls.t.token)
          {
          case TK_NAME:
            l.site = 2;
            break;
          case TK_DOTS:
            l.varargk = 1;
            l.site = 3;
            break;
          default:
            luaX_syntaxerror(ls, Lt.name_or_expected);
          }
          continue;
        case 2:
        {
          TString name = str_checkname(ls);
          new_localvar(ls, name);
          l.nparams++;
          l.site = 5;
          continue;
        }
        case 3:
          luaX_next(ls); /* skip '...' */
          if (ls.t.token != TK_NAME)
          {
            ByteView name = Lt.vararg_table;
            new_localvar(ls, luaX_newstring(ls, name, 0, name.size()));
            break;
          }
          l.site = 4;
        case 4:
        {
          TString name = str_checkname(ls);
          new_varkind(ls, name, (byte) RDKVAVAR);
          break;
        }
        case 5:
          if (testnext(ls, ',') != 0)
          {
            l.site = 1;
            continue;
          }
          break;
        default:
          luaZ_badstate(ls.z.rz);
        }
        break; /* (the list is read) */
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_PARLIST, l);
      throw ex;
    }
    adjustlocalvars(ls, l.nparams);
    f.numparams = fs.nactvar;
    if (l.varargk != 0)
    {
      setvararg(fs);          /* declared vararg */
      adjustlocalvars(ls, 1); /* vararg parameter */
    }
    /* reserve registers for parameters (plus vararg parameter, if present) */
    luaK_reserveregs(fs, fs.nactvar);
  }

  /* the frame of 'body' (see lload.h) */
  private static final class BodyFrame extends LoadFrame
  {
    FuncState new_fs = new FuncState();
    BlockCnt bl = new BlockCnt();

    BodyFrame()
    {
    }

    BodyFrame(BodyFrame o)
    {
      super(o);
      new_fs = new FuncState(o.new_fs);
      bl = new BlockCnt(o.bl);
    }

    @Override
    public LoadFrame copy()
    {
      return new BodyFrame(this);
    }

    @Override
    public int size()
    {
      return 144;
    }
  }

  private static void body(LexState ls, expdesc e, int ismethod, int line)
  {
    /* body ->  '(' parlist ')' block END */
    BodyFrame l = new BodyFrame();
    if (ls.z.resuming)
    { /* (its function and block again in the chains) */
      l = new BodyFrame((BodyFrame) luaZ_restore(ls.z.rz, LF_BODY));
      l.new_fs.prev = ls.fs;
      l.new_fs.ls = ls;
      ls.fs = l.new_fs;
      l.bl.previous = null;
      l.new_fs.bl = l.bl;
    }
    try
    {
      switch (l.site)
      {
      case 0:
        l.new_fs.f = addprototype(ls);
        l.new_fs.f.linedefined = line;
        open_func(ls, l.new_fs, l.bl);
        l.site = 1;
      case 1:
        checknext(ls, '(');
        if (ismethod != 0)
        {
          ByteView self = Lt.self;
          new_localvar(ls, luaX_newstring(ls, self, 0, self.size())); /* create 'self' parameter */
          adjustlocalvars(ls, 1);
        }
        l.site = 2;
      case 2:
        parlist(ls);
        l.site = 3;
      case 3:
        checknext(ls, ')');
        l.site = 4;
      case 4:
        statlist(ls);
        l.new_fs.f.lastlinedefined = ls.linenumber;
        l.site = 5;
      case 5:
        check_match(ls, TK_END, TK_FUNCTION, line);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_BODY, l);
      throw ex;
    }
    codeclosure(ls, e);
    close_func(ls);
  }

  /* the frame of 'explist' (see lload.h) */
  private static final class ExplistFrame extends LoadFrame
  {
    int n = 0;

    ExplistFrame()
    {
    }

    ExplistFrame(ExplistFrame o)
    {
      super(o);
      n = o.n;
    }

    @Override
    public LoadFrame copy()
    {
      return new ExplistFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static int explist(LexState ls, expdesc v)
  {
    /* explist -> expr { ',' expr } */
    ExplistFrame l = new ExplistFrame();
    if (ls.z.resuming)
      l = new ExplistFrame((ExplistFrame) luaZ_restore(ls.z.rz, LF_EXPLIST));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.n = 1; /* at least one expression */
          l.site = 1;
        case 1:
          expr(ls, v);
          l.site = 2;
        case 2: /* the loop */
          if (testnext(ls, ',') == 0)
            return l.n;
          luaK_exp2nextreg(ls.fs, v);
          l.site = 3;
        case 3:
          expr(ls, v);
          l.n++;
          l.site = 2;
          continue;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_EXPLIST, l);
      throw ex;
    }
  }

  /* the frame of 'funcargs' (see lload.h) */
  private static final class FuncargsFrame extends LoadFrame
  {
    expdesc args = new expdesc();
    int line = 0;

    FuncargsFrame()
    {
    }

    FuncargsFrame(FuncargsFrame o)
    {
      super(o);
      args = new expdesc(o.args);
      line = o.line;
    }

    @Override
    public LoadFrame copy()
    {
      return new FuncargsFrame(this);
    }

    @Override
    public int size()
    {
      return 104;
    }
  }

  private static void funcargs(LexState ls, expdesc f)
  {
    FuncargsFrame l = new FuncargsFrame();
    FuncState fs = ls.fs;
    int base, nparams;
    if (ls.z.resuming)
      l = new FuncargsFrame((FuncargsFrame) luaZ_restore(ls.z.rz, LF_FUNCARGS));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.line = ls.linenumber;
          switch (ls.t.token)
          {
          case '(': /* funcargs -> '(' [ explist ] ')' */
            l.site = 1;
            break;
          case '{' /*}*/: /* funcargs -> constructor */
            l.site = 4;
            break;
          case TK_STRING: /* funcargs -> STRING */
            codestring(l.args, ls.t.ts);
            l.site = 5;
            break;
          default:
            luaX_syntaxerror(ls, Lt.function_arguments_expected);
          }
          continue;
        case 1:
          luaX_next(ls);
          if (ls.t.token == ')') /* arg list is empty? */
          {
            l.args.k = VVOID;
            l.site = 3;
            continue;
          }
          l.site = 2;
        case 2:
          explist(ls, l.args);
          if (hasmultret(l.args.k) != 0)
            luaK_setmultret(fs, l.args);
          l.site = 3;
        case 3:
          check_match(ls, ')', '(', l.line);
          break;
        case 4:
          constructor(ls, l.args);
          break;
        case 5:
          luaX_next(ls); /* must use 'seminfo' before 'next' */
          break;
        default:
          luaZ_badstate(ls.z.rz);
        }
        break; /* (the arguments are read) */
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_FUNCARGS, l);
      throw ex;
    }
    assert f.k == VNONRELOC;
    base = f.info; /* base register for call */
    if (hasmultret(l.args.k) != 0)
      nparams = LUA_MULTRET; /* open call */
    else
    {
      if (l.args.k != VVOID)
        luaK_exp2nextreg(fs, l.args); /* close last argument */
      nparams = fs.freereg - (base + 1);
    }
    init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams + 1, 2));
    luaK_fixline(fs, l.line);
    /* call removes function and arguments and leaves one result (unless
       changed later) */
    fs.freereg = base + 1;
  }

  /*
  ** {======================================================================
  ** Expression parsing
  ** =======================================================================
  */

  /* the frame of a function with a line to keep (see lload.h) */
  private static final class LineFrame extends LoadFrame
  {
    int line = 0;

    LineFrame()
    {
    }

    LineFrame(LineFrame o)
    {
      super(o);
      line = o.line;
    }

    @Override
    public LoadFrame copy()
    {
      return new LineFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static void primaryexp(LexState ls, expdesc v)
  {
    /* primaryexp -> NAME | '(' expr ')' */
    LineFrame l = new LineFrame();
    if (ls.z.resuming)
      l = new LineFrame((LineFrame) luaZ_restore(ls.z.rz, LF_PRIMARYEXP));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          switch (ls.t.token)
          {
          case '(':
            l.line = ls.linenumber;
            l.site = 1;
            break;
          case TK_NAME:
            l.site = 4;
            break;
          default:
            luaX_syntaxerror(ls, Lt.unexpected_symbol);
          }
          continue;
        case 1:
          luaX_next(ls);
          l.site = 2;
        case 2:
          expr(ls, v);
          l.site = 3;
        case 3:
          check_match(ls, ')', '(', l.line);
          luaK_dischargevars(ls.fs, v);
          return;
        case 4:
          singlevar(ls, v);
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_PRIMARYEXP, l);
      throw ex;
    }
  }

  /* the frame of a function with a key to keep (see lload.h) */
  private static final class KeyFrame extends LoadFrame
  {
    expdesc key = new expdesc();

    KeyFrame()
    {
    }

    KeyFrame(KeyFrame o)
    {
      super(o);
      key = new expdesc(o.key);
    }

    @Override
    public LoadFrame copy()
    {
      return new KeyFrame(this);
    }

    @Override
    public int size()
    {
      return 96;
    }
  }

  private static void suffixedexp(LexState ls, expdesc v)
  {
    /* suffixedexp ->
         primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs } */
    KeyFrame l = new KeyFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new KeyFrame((KeyFrame) luaZ_restore(ls.z.rz, LF_SUFFIXEDEXP));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          primaryexp(ls, v);
          l.site = 1;
        case 1: /* the loop */
          switch (ls.t.token)
          {
          case '.': /* fieldsel */
            l.site = 2;
            break;
          case '[': /* '[' exp ']' */
            luaK_exp2anyregup(fs, v);
            l.site = 3;
            break;
          case ':': /* ':' NAME funcargs */
            l.site = 4;
            break;
          case '(':
          case TK_STRING:
          case '{' /*}*/: /* funcargs */
            luaK_exp2nextreg(fs, v);
            l.site = 6;
            break;
          default:
            return;
          }
          continue;
        case 2:
          fieldsel(ls, v);
          l.site = 1;
          continue;
        case 3:
          yindex(ls, l.key);
          luaK_indexed(fs, v, l.key);
          l.site = 1;
          continue;
        case 4:
          luaX_next(ls);
          l.site = 5;
        case 5:
          codename(ls, l.key);
          luaK_self(fs, v, l.key);
          l.site = 6;
        case 6:
          funcargs(ls, v);
          l.site = 1;
          continue;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_SUFFIXEDEXP, l);
      throw ex;
    }
  }

  private static void simpleexp(LexState ls, expdesc v)
  {
    /* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
                    constructor | FUNCTION body | suffixedexp */
    LineFrame l = new LineFrame();
    if (ls.z.resuming)
      l = new LineFrame((LineFrame) luaZ_restore(ls.z.rz, LF_SIMPLEEXP));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          switch (ls.t.token)
          {
          case TK_FLT:
          {
            init_exp(v, VKFLT, 0);
            v.nval = ls.t.r;
            break;
          }
          case TK_INT:
          {
            init_exp(v, VKINT, 0);
            v.ival = ls.t.i;
            break;
          }
          case TK_STRING:
          {
            codestring(v, ls.t.ts);
            break;
          }
          case TK_NIL:
          {
            init_exp(v, VNIL, 0);
            break;
          }
          case TK_TRUE:
          {
            init_exp(v, VTRUE, 0);
            break;
          }
          case TK_FALSE:
          {
            init_exp(v, VFALSE, 0);
            break;
          }
          case TK_DOTS:
          { /* vararg */
            FuncState fs = ls.fs;
            check_condition(ls, isvararg(fs.f), Lt.cannot_use_outside_a_vararg_function);
            init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, fs.f.numparams, 1));
            break;
          }
          case '{' /*}*/: /* constructor */
            l.site = 2;
            continue;
          case TK_FUNCTION:
            l.site = 3;
            continue;
          default:
            l.site = 5;
            continue;
          }
          l.site = 1;
        case 1:
          luaX_next(ls);
          return;
        case 2:
          constructor(ls, v);
          return;
        case 3:
          luaX_next(ls);
          l.line = ls.linenumber;
          l.site = 4;
        case 4:
          body(ls, v, 0, l.line);
          return;
        case 5:
          suffixedexp(ls, v);
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_SIMPLEEXP, l);
      throw ex;
    }
  }

  private static int getunopr(int op)
  {
    switch (op)
    {
    case TK_NOT:
      return OPR_NOT;
    case '-':
      return OPR_MINUS;
    case '~':
      return OPR_BNOT;
    case '#':
      return OPR_LEN;
    default:
      return OPR_NOUNOPR;
    }
  }

  private static int getbinopr(int op)
  {
    switch (op)
    {
    case '+':
      return OPR_ADD;
    case '-':
      return OPR_SUB;
    case '*':
      return OPR_MUL;
    case '%':
      return OPR_MOD;
    case '^':
      return OPR_POW;
    case '/':
      return OPR_DIV;
    case TK_IDIV:
      return OPR_IDIV;
    case '&':
      return OPR_BAND;
    case '|':
      return OPR_BOR;
    case '~':
      return OPR_BXOR;
    case TK_SHL:
      return OPR_SHL;
    case TK_SHR:
      return OPR_SHR;
    case TK_CONCAT:
      return OPR_CONCAT;
    case TK_NE:
      return OPR_NE;
    case TK_EQ:
      return OPR_EQ;
    case '<':
      return OPR_LT;
    case TK_LE:
      return OPR_LE;
    case '>':
      return OPR_GT;
    case TK_GE:
      return OPR_GE;
    case TK_AND:
      return OPR_AND;
    case TK_OR:
      return OPR_OR;
    default:
      return OPR_NOBINOPR;
    }
  }

  /*
  ** Priority table for binary operators.
  */
  private static final class Priority
  {
    final byte left;  /* left priority for each binary operator */
    final byte right; /* right priority */

    Priority(int left, int right)
    {
      this.left = (byte) left;
      this.right = (byte) right;
    }
  }

  private static final Priority[] priority = {
      /* ORDER OPR */
      new Priority(10, 10),
      new Priority(10, 10), /* '+' '-' */
      new Priority(11, 11),
      new Priority(11, 11), /* '*' '%' */
      new Priority(14, 13), /* '^' (right associative) */
      new Priority(11, 11),
      new Priority(11, 11), /* '/' '//' */
      new Priority(6, 6),
      new Priority(4, 4),
      new Priority(5, 5), /* '&' '|' '~' */
      new Priority(7, 7),
      new Priority(7, 7), /* '<<' '>>' */
      new Priority(9, 8), /* '..' (right associative) */
      new Priority(3, 3),
      new Priority(3, 3),
      new Priority(3, 3), /* ==, <, <= */
      new Priority(3, 3),
      new Priority(3, 3),
      new Priority(3, 3), /* ~=, >, >= */
      new Priority(2, 2),
      new Priority(1, 1) /* and, or */
  };

  private static final int UNARY_PRIORITY = 12; /* priority for unary operators */

  /* the frame of 'subexpr' (see lload.h) */
  private static final class SubexprFrame extends LoadFrame
  {
    int op = OPR_NOBINOPR;
    int uop = OPR_NOUNOPR;
    int line = 0;
    expdesc v2 = new expdesc();

    SubexprFrame()
    {
    }

    SubexprFrame(SubexprFrame o)
    {
      super(o);
      op = o.op;
      uop = o.uop;
      line = o.line;
      v2 = new expdesc(o.v2);
    }

    @Override
    public LoadFrame copy()
    {
      return new SubexprFrame(this);
    }

    @Override
    public int size()
    {
      return 112;
    }
  }

  /*
  ** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
  ** where 'binop' is any binary operator with a priority higher than 'limit'
  */
  private static int subexpr(LexState ls, expdesc v, int limit)
  {
    SubexprFrame l = new SubexprFrame();
    enterlevel(ls);
    if (ls.z.resuming)
      l = new SubexprFrame((SubexprFrame) luaZ_restore(ls.z.rz, LF_SUBEXPR));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.uop = getunopr(ls.t.token);
          if (l.uop == OPR_NOUNOPR)
          {
            l.site = 3;
            continue;
          }
          /* prefix (unary) operator */
          l.line = ls.linenumber;
          l.site = 1;
        case 1:
          luaX_next(ls); /* skip operator */
          l.site = 2;
        case 2:
          subexpr(ls, v, UNARY_PRIORITY);
          luaK_prefix(ls.fs, l.uop, v, l.line);
          l.site = 4;
          continue;
        case 3:
          simpleexp(ls, v);
          l.site = 4;
        case 4:
          /* expand while operators have priorities higher than 'limit' */
          l.op = getbinopr(ls.t.token);
          l.site = 5;
        case 5: /* the loop */
          if (l.op == OPR_NOBINOPR || priority[l.op].left <= limit)
          {
            leavelevel(ls);
            return l.op; /* return first untreated operator */
          }
          l.line = ls.linenumber;
          l.site = 6;
        case 6:
          luaX_next(ls); /* skip operator */
          luaK_infix(ls.fs, l.op, v);
          l.site = 7;
        case 7:
        {
          /* read sub-expression with higher priority */
          int nextop = subexpr(ls, l.v2, priority[l.op].right);
          luaK_posfix(ls.fs, l.op, v, l.v2, l.line);
          l.op = nextop;
          l.site = 5;
          continue;
        }
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_SUBEXPR, l);
      throw ex;
    }
  }

  private static void expr(LexState ls, expdesc v)
  {
    subexpr(ls, v, 0); /* (no frame of its own: see 'testnext') */
  }

  /* }==================================================================== */

  /*
  ** {======================================================================
  ** Rules for Statements
  ** =======================================================================
  */

  /* the frame of a function with a block (see lload.h) */
  private static final class BlockFrame extends LoadFrame
  {
    BlockCnt bl = new BlockCnt();

    BlockFrame()
    {
    }

    BlockFrame(BlockFrame o)
    {
      super(o);
      bl = new BlockCnt(o.bl);
    }

    @Override
    public LoadFrame copy()
    {
      return new BlockFrame(this);
    }

    @Override
    public int size()
    {
      return 48;
    }
  }

  private static void block(LexState ls)
  {
    /* block -> statlist */
    BlockFrame l = new BlockFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
    {
      l = new BlockFrame((BlockFrame) luaZ_restore(ls.z.rz, LF_BLOCK));
      l.bl.previous = fs.bl; /* (again in the chain) */
      fs.bl = l.bl;
    }
    try
    {
      switch (l.site)
      {
      case 0:
        enterblock(fs, l.bl, (byte) 0);
        l.site = 1;
      case 1:
        statlist(ls);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_BLOCK, l);
      throw ex;
    }
    leaveblock(fs);
  }

  /*
  ** check whether, in an assignment to an upvalue/local variable, the
  ** upvalue/local variable is begin used in a previous assignment to a
  ** table. If so, save original upvalue/local value in a safe place and
  ** use this safe copy in the previous assignment.
  */
  private static void check_conflict(LexState ls, LHS_assign lh, expdesc v)
  {
    FuncState fs = ls.fs;
    int extra = fs.freereg; /* eventual position to save local variable */
    int conflict = 0;
    for (; lh != null; lh = lh.prev)
    { /* check all previous assignments */
      if (vkisindexed(lh.v.k) != 0)
      { /* assignment to table field? */
        if (lh.v.k == VINDEXUP)
        { /* is table an upvalue? */
          if (v.k == VUPVAL && lh.v.ind.t == v.info)
          {
            conflict = 1; /* table is the upvalue being assigned now */
            lh.v.k = VINDEXSTR;
            lh.v.ind.t = extra; /* assignment will use safe copy */
          }
        }
        else
        { /* table is a register */
          if (v.k == VLOCAL && lh.v.ind.t == v.var.ridx)
          {
            conflict = 1;       /* table is the local being assigned now */
            lh.v.ind.t = extra; /* assignment will use safe copy */
          }
          /* is index the local being assigned? */
          if (lh.v.k == VINDEXED && v.k == VLOCAL &&
              lh.v.ind.idx == v.var.ridx)
          {
            conflict = 1;
            lh.v.ind.idx = extra; /* previous assignment will use safe copy */
          }
        }
      }
    }
    if (conflict != 0)
    {
      /* copy upvalue/local value to a temporary (in position 'extra') */
      if (v.k == VLOCAL)
        luaK_codeABC(fs, OP_MOVE, extra, v.var.ridx, 0);
      else
        luaK_codeABC(fs, OP_GETUPVAL, extra, v.info, 0);
      luaK_reserveregs(fs, 1);
    }
  }

  /* Create code to store the "top" register in 'var' */
  private static void storevartop(FuncState fs, expdesc var)
  {
    expdesc e = new expdesc();
    init_exp(e, VNONRELOC, fs.freereg - 1);
    luaK_storevar(fs, var, e); /* will also free the top register */
  }

  /* the frame of 'restassign' (see lload.h) */
  private static final class RestassignFrame extends LoadFrame
  {
    expdesc e = new expdesc();
    LHS_assign nv = new LHS_assign();

    RestassignFrame()
    {
    }

    RestassignFrame(RestassignFrame o)
    {
      super(o);
      e = new expdesc(o.e);
      nv = new LHS_assign(o.nv);
    }

    @Override
    public LoadFrame copy()
    {
      return new RestassignFrame(this);
    }

    @Override
    public int size()
    {
      return 176;
    }
  }

  /*
  ** Parse and compile a multiple assignment. The first "variable"
  ** (a 'suffixedexp') was already read by the caller.
  **
  ** assignment -> suffixedexp restassign
  ** restassign -> ',' suffixedexp restassign | '=' explist
  */
  private static void restassign(LexState ls, LHS_assign lh, int nvars)
  {
    RestassignFrame l = new RestassignFrame();
    if (ls.z.resuming)
    {
      l = new RestassignFrame((RestassignFrame) luaZ_restore(ls.z.rz, LF_RESTASSIGN));
      l.nv.prev = lh; /* (again in the chain) */
    }
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          check_condition(ls, vkisvar(lh.v.k), Lt.syntax_error);
          check_readonly(ls, lh.v);
          l.site = 1;
        case 1:
          if (testnext(ls, ',') == 0)
          { /* restassign -> '=' explist */
            l.site = 4;
            continue;
          }
          /* restassign -> ',' suffixedexp restassign */
          l.nv.prev = lh;
          l.site = 2;
        case 2:
          suffixedexp(ls, l.nv.v);
          if (vkisindexed(l.nv.v.k) == 0)
            check_conflict(ls, lh, l.nv.v);
          l.site = 3;
        case 3:
          enterlevel(ls); /* control recursion depth */
          restassign(ls, l.nv, nvars + 1);
          leavelevel(ls);
          break;
        case 4:
          checknext(ls, '=');
          l.site = 5;
        case 5:
        {
          int nexps = explist(ls, l.e);
          if (nexps != nvars)
            adjust_assign(ls, nvars, nexps, l.e);
          else
          {
            luaK_setoneret(ls.fs, l.e); /* close last expression */
            luaK_storevar(ls.fs, lh.v, l.e);
            return; /* avoid default */
          }
          break;
        }
        default:
          luaZ_badstate(ls.z.rz);
        }
        break; /* (the assignment is read) */
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_RESTASSIGN, l);
      throw ex;
    }
    storevartop(ls.fs, lh.v); /* default assignment */
  }

  /* the frame of a function with an expression to keep (see lload.h) */
  private static final class ExpFrame extends LoadFrame
  {
    expdesc v = new expdesc();

    ExpFrame()
    {
    }

    ExpFrame(ExpFrame o)
    {
      super(o);
      v = new expdesc(o.v);
    }

    @Override
    public LoadFrame copy()
    {
      return new ExpFrame(this);
    }

    @Override
    public int size()
    {
      return 96;
    }
  }

  private static int cond(LexState ls)
  {
    /* cond -> exp */
    ExpFrame l = new ExpFrame();
    if (ls.z.resuming)
      l = new ExpFrame((ExpFrame) luaZ_restore(ls.z.rz, LF_COND));
    try
    {
      switch (l.site)
      {
      case 0:
        expr(ls, l.v); /* read condition */
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_COND, l);
      throw ex;
    }
    if (l.v.k == VNIL)
      l.v.k = VFALSE; /* 'falses' are all equal here */
    luaK_goiftrue(ls.fs, l.v);
    return l.v.f;
  }

  private static void gotostat(LexState ls, int line)
  {
    TString name = str_checkname(ls); /* label's name */
    newgotoentry(ls, name, line);
  }

  /*
  ** Break statement. Semantically equivalent to "goto break".
  */
  private static void breakstat(LexState ls, int line)
  {
    LoadFrame l = new LoadFrame();
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_BREAKSTAT));
    try
    {
      switch (l.site)
      {
      case 0:
      {
        BlockCnt bl; /* to look for an enclosing loop */
        for (bl = ls.fs.bl; bl != null; bl = bl.previous)
        {
          if (bl.isloop != 0) /* found one? */
            break;
        }
        if (bl == null)
          luaX_syntaxerror(ls, Lt.break_outside_loop);
        bl.isloop = 2; /* signal that block has pending breaks */
        l.site = 1;
      }
      case 1:
        luaX_next(ls); /* skip break */
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_BREAKSTAT, l);
      throw ex;
    }
    newgotoentry(ls, ls.brkn, line);
  }

  /*
  ** Check whether there is already a label with the given 'name' at
  ** current function.
  */
  private static void checkrepeated(LexState ls, TString name)
  {
    Labeldesc lb = findlabel(ls, name, ls.fs.firstlabel);
    if (lb != null) /* already defined? */
      luaK_semerror(ls, Lt.label_s_already_defined_on_line_d,
                    new FmtArgs().str(name.data.view()).num(lb.line)); /* error */
  }

  private static void labelstat(LexState ls, TString name, int line)
  {
    /* label -> '::' NAME '::' */
    LoadFrame l = new LoadFrame();
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_LABELSTAT));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          checknext(ls, TK_DBCOLON); /* skip double colon */
          l.site = 1;
        case 1: /* the loop */
          if (ls.t.token != ';' && ls.t.token != TK_DBCOLON)
            break;
          l.site = 2;
        case 2:
          statement(ls); /* skip other no-op statements */
          l.site = 1;
          continue;
        default:
          luaZ_badstate(ls.z.rz);
        }
        break; /* (the no-op statements are skipped) */
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_LABELSTAT, l);
      throw ex;
    }
    checkrepeated(ls, name); /* check for repeated labels */
    createlabel(ls, name, line, block_follow(ls, 0));
  }

  /* the frame of 'whilestat' (see lload.h) */
  private static final class WhileFrame extends LoadFrame
  {
    int whileinit = 0;
    int condexit = 0;
    BlockCnt bl = new BlockCnt();

    WhileFrame()
    {
    }

    WhileFrame(WhileFrame o)
    {
      super(o);
      whileinit = o.whileinit;
      condexit = o.condexit;
      bl = new BlockCnt(o.bl);
    }

    @Override
    public LoadFrame copy()
    {
      return new WhileFrame(this);
    }

    @Override
    public int size()
    {
      return 56;
    }
  }

  private static void whilestat(LexState ls, int line)
  {
    /* whilestat -> WHILE cond DO block END */
    WhileFrame l = new WhileFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
    {
      l = new WhileFrame((WhileFrame) luaZ_restore(ls.z.rz, LF_WHILESTAT));
      if (l.site >= 2)
      { /* (its block again in the chain) */
        l.bl.previous = fs.bl;
        fs.bl = l.bl;
      }
    }
    try
    {
      switch (l.site)
      {
      case 0:
        luaX_next(ls); /* skip WHILE */
        l.whileinit = luaK_getlabel(fs);
        l.site = 1;
      case 1:
        l.condexit = cond(ls);
        enterblock(fs, l.bl, (byte) 1);
        l.site = 2;
      case 2:
        checknext(ls, TK_DO);
        l.site = 3;
      case 3:
        block(ls);
        luaK_jumpto(fs, l.whileinit);
        l.site = 4;
      case 4:
        check_match(ls, TK_END, TK_WHILE, line);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_WHILESTAT, l);
      throw ex;
    }
    leaveblock(fs);
    luaK_patchtohere(fs, l.condexit); /* false conditions finish the loop */
  }

  /* the frame of 'repeatstat' (see lload.h) */
  private static final class RepeatFrame extends LoadFrame
  {
    int condexit = 0;
    int repeat_init = 0;
    BlockCnt bl1 = new BlockCnt(), bl2 = new BlockCnt();

    RepeatFrame()
    {
    }

    RepeatFrame(RepeatFrame o)
    {
      super(o);
      condexit = o.condexit;
      repeat_init = o.repeat_init;
      bl1 = new BlockCnt(o.bl1);
      bl2 = new BlockCnt(o.bl2);
    }

    @Override
    public LoadFrame copy()
    {
      return new RepeatFrame(this);
    }

    @Override
    public int size()
    {
      return 80;
    }
  }

  private static void repeatstat(LexState ls, int line)
  {
    /* repeatstat -> REPEAT block UNTIL cond */
    RepeatFrame l = new RepeatFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
    {
      l = new RepeatFrame((RepeatFrame) luaZ_restore(ls.z.rz, LF_REPEATSTAT));
      l.bl1.previous = fs.bl; /* (its blocks again in the chain) */
      l.bl2.previous = l.bl1;
      fs.bl = l.bl2;
    }
    try
    {
      switch (l.site)
      {
      case 0:
        l.repeat_init = luaK_getlabel(fs);
        enterblock(fs, l.bl1, (byte) 1); /* loop block */
        enterblock(fs, l.bl2, (byte) 0); /* scope block */
        l.site = 1;
      case 1:
        luaX_next(ls); /* skip REPEAT */
        l.site = 2;
      case 2:
        statlist(ls);
        l.site = 3;
      case 3:
        check_match(ls, TK_UNTIL, TK_REPEAT, line);
        l.site = 4;
      case 4:
        l.condexit = cond(ls); /* read condition (inside scope block) */
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_REPEATSTAT, l);
      throw ex;
    }
    leaveblock(fs); /* finish scope */
    if (l.bl2.upval != 0)
    {                                   /* upvalues? */
      int exit = luaK_jump(fs);         /* normal exit must jump over fix */
      luaK_patchtohere(fs, l.condexit); /* repetition must close upvalues */
      luaK_codeABC(fs, OP_CLOSE, reglevel(fs, l.bl2.nactvar), 0, 0);
      l.condexit = luaK_jump(fs); /* repeat after closing upvalues */
      luaK_patchtohere(fs, exit); /* normal exit comes to here */
    }
    luaK_patchlist(fs, l.condexit, l.repeat_init); /* close the loop */
    leaveblock(fs);                                /* finish loop */
  }

  /*
  ** Read an expression and generate code to put its results in next
  ** stack slot.
  **
  */
  private static void exp1(LexState ls)
  {
    ExpFrame l = new ExpFrame();
    if (ls.z.resuming)
      l = new ExpFrame((ExpFrame) luaZ_restore(ls.z.rz, LF_EXP1));
    try
    {
      switch (l.site)
      {
      case 0:
        expr(ls, l.v);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_EXP1, l);
      throw ex;
    }
    luaK_exp2nextreg(ls.fs, l.v);
    assert l.v.k == VNONRELOC;
  }

  /*
  ** Fix for instruction at position 'pc' to jump to 'dest'.
  ** (Jump addresses are relative in Lua). 'back' true means
  ** a back jump.
  */
  private static void fixforjump(FuncState fs, int pc, int dest, int back)
  {
    int[] code = fs.f.code;
    int offset = dest - (pc + 1);
    if (back != 0)
      offset = -offset;
    if (offset > MAXARG_Bx)
      luaX_syntaxerror(fs.ls, Lt.control_structure_too_long);
    code[pc] = SETARG_Bx(code[pc], offset);
  }

  /* the frame of 'forbody' (see lload.h) */
  private static final class ForbodyFrame extends LoadFrame
  {
    int prep = 0;
    BlockCnt bl = new BlockCnt();

    ForbodyFrame()
    {
    }

    ForbodyFrame(ForbodyFrame o)
    {
      super(o);
      prep = o.prep;
      bl = new BlockCnt(o.bl);
    }

    @Override
    public LoadFrame copy()
    {
      return new ForbodyFrame(this);
    }

    @Override
    public int size()
    {
      return 56;
    }
  }

  private static final int[] forprep = {OP_FORPREP, OP_TFORPREP};
  private static final int[] forloop = {OP_FORLOOP, OP_TFORLOOP};

  /*
  ** Generate code for a 'for' loop.
  */
  private static void forbody(LexState ls, int base, int line, int nvars, int isgen)
  {
    /* forbody -> DO block */
    ForbodyFrame l = new ForbodyFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
    {
      l = new ForbodyFrame((ForbodyFrame) luaZ_restore(ls.z.rz, LF_FORBODY));
      if (l.site == 1)
      { /* (its block again in the chain) */
        l.bl.previous = fs.bl;
        fs.bl = l.bl;
      }
    }
    try
    {
      switch (l.site)
      {
      case 0:
        checknext(ls, TK_DO);
        l.prep = luaK_codeABx(fs, forprep[isgen], base, 0);
        fs.freereg--;                    /* both 'forprep' remove one register from the stack */
        enterblock(fs, l.bl, (byte) 0); /* scope for declared variables */
        adjustlocalvars(ls, nvars);
        luaK_reserveregs(fs, nvars);
        l.site = 1;
      case 1:
        block(ls);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_FORBODY, l);
      throw ex;
    }
    leaveblock(fs); /* end of scope for declared variables */
    fixforjump(fs, l.prep, luaK_getlabel(fs), 0);
    if (isgen != 0)
    { /* generic for? */
      luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
      luaK_fixline(fs, line);
    }
    int endfor = luaK_codeABx(fs, forloop[isgen], base, 0);
    fixforjump(fs, endfor, l.prep + 1, 1);
    luaK_fixline(fs, line);
  }

  /* the frame of 'fornum' (see lload.h) */
  private static final class FornumFrame extends LoadFrame
  {
    int base = 0;

    FornumFrame()
    {
    }

    FornumFrame(FornumFrame o)
    {
      super(o);
      base = o.base;
    }

    @Override
    public LoadFrame copy()
    {
      return new FornumFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static void fornum(LexState ls, TString varname, int line)
  {
    /* fornum -> NAME = exp,exp[,exp] forbody */
    FornumFrame l = new FornumFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new FornumFrame((FornumFrame) luaZ_restore(ls.z.rz, LF_FORNUM));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.base = fs.freereg;
          new_localvar(ls, luaX_newstring(ls, Lt.for_state, 0, Lt.for_state.size()));
          new_localvar(ls, luaX_newstring(ls, Lt.for_state, 0, Lt.for_state.size()));
          new_varkind(ls, varname, (byte) RDKCONST);
          l.site = 1;
        case 1:
          checknext(ls, '=');
          l.site = 2;
        case 2:
          exp1(ls); /* initial value */
          l.site = 3;
        case 3:
          checknext(ls, ',');
          l.site = 4;
        case 4:
          exp1(ls); /* limit */
          l.site = 5;
        case 5:
          if (testnext(ls, ',') == 0)
          { /* default step = 1 */
            luaK_int(fs, fs.freereg, 1);
            luaK_reserveregs(fs, 1);
            l.site = 7;
            continue;
          }
          l.site = 6;
        case 6:
          exp1(ls); /* optional step */
          l.site = 7;
        case 7:
          adjustlocalvars(ls, 2); /* start scope for internal variables */
          l.site = 8;
        case 8:
          forbody(ls, l.base, line, 1, 0);
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_FORNUM, l);
      throw ex;
    }
  }

  /* the frame of 'forlist' (see lload.h) */
  private static final class ForlistFrame extends LoadFrame
  {
    int nvars = 0; /* function, state, closing, control */
    int line = 0;
    int base = 0;
    expdesc e = new expdesc();

    ForlistFrame()
    {
    }

    ForlistFrame(ForlistFrame o)
    {
      super(o);
      nvars = o.nvars;
      line = o.line;
      base = o.base;
      e = new expdesc(o.e);
    }

    @Override
    public LoadFrame copy()
    {
      return new ForlistFrame(this);
    }

    @Override
    public int size()
    {
      return 112;
    }
  }

  private static void forlist(LexState ls, TString indexname)
  {
    /* forlist -> NAME {,NAME} IN explist forbody */
    ForlistFrame l = new ForlistFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new ForlistFrame((ForlistFrame) luaZ_restore(ls.z.rz, LF_FORLIST));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.nvars = 4;
          l.base = fs.freereg;
          /* create internal variables */
          new_localvar(ls, luaX_newstring(ls, Lt.for_state, 0, Lt.for_state.size()));  /* iterator function */
          new_localvar(ls, luaX_newstring(ls, Lt.for_state, 0, Lt.for_state.size()));  /* state */
          new_localvar(ls, luaX_newstring(ls, Lt.for_state, 0, Lt.for_state.size()));  /* closing var. (after swap) */
          new_varkind(ls, indexname, (byte) RDKCONST);
          l.site = 1;
        case 1: /* the loop: other declared variables */
          if (testnext(ls, ',') == 0)
          {
            l.site = 3;
            continue;
          }
          l.site = 2;
        case 2:
        {
          TString name = str_checkname(ls);
          new_localvar(ls, name);
          l.nvars++;
          l.site = 1;
          continue;
        }
        case 3:
          checknext(ls, TK_IN);
          l.line = ls.linenumber;
          l.site = 4;
        case 4:
        {
          int nexps = explist(ls, l.e);
          adjust_assign(ls, 4, nexps, l.e);
          adjustlocalvars(ls, 3); /* start scope for internal variables */
          marktobeclosed(fs);     /* last internal var. must be closed */
          luaK_checkstack(fs, 2); /* extra space to call iterator */
          l.site = 5;
        }
        case 5:
          forbody(ls, l.base, l.line, l.nvars - 3, 1);
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_FORLIST, l);
      throw ex;
    }
  }

  /* the frame of 'forstat' (see lload.h) */
  private static final class ForstatFrame extends LoadFrame
  {
    TString varname = null;
    BlockCnt bl = new BlockCnt();

    ForstatFrame()
    {
    }

    ForstatFrame(ForstatFrame o)
    {
      super(o);
      varname = o.varname;
      bl = new BlockCnt(o.bl);
    }

    @Override
    public LoadFrame copy()
    {
      return new ForstatFrame(this);
    }

    @Override
    public int size()
    {
      return 56;
    }
  }

  private static void forstat(LexState ls, int line)
  {
    /* forstat -> FOR (fornum | forlist) END */
    ForstatFrame l = new ForstatFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
    {
      l = new ForstatFrame((ForstatFrame) luaZ_restore(ls.z.rz, LF_FORSTAT));
      l.bl.previous = fs.bl; /* (its block again in the chain) */
      fs.bl = l.bl;
    }
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          enterblock(fs, l.bl, (byte) 1); /* scope for loop and control variables */
          l.site = 1;
        case 1:
          luaX_next(ls); /* skip 'for' */
          l.site = 2;
        case 2:
          l.varname = str_checkname(ls); /* first variable name */
          switch (ls.t.token)
          {
          case '=':
            l.site = 3;
            break;
          case ',':
          case TK_IN:
            l.site = 4;
            break;
          default:
            luaX_syntaxerror(ls, Lt.or_in_expected);
          }
          continue;
        case 3:
          fornum(ls, l.varname, line);
          l.site = 5;
          continue;
        case 4:
          forlist(ls, l.varname);
          l.site = 5;
        case 5:
          check_match(ls, TK_END, TK_FOR, line);
          break;
        default:
          luaZ_badstate(ls.z.rz);
        }
        break; /* (the loop is read) */
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_FORSTAT, l);
      throw ex;
    }
    leaveblock(fs); /* loop scope ('break' jumps to this point) */
  }

  /* the frame of 'test_then_block' (see lload.h) */
  private static final class TestThenFrame extends LoadFrame
  {
    int condtrue = 0;

    TestThenFrame()
    {
    }

    TestThenFrame(TestThenFrame o)
    {
      super(o);
      condtrue = o.condtrue;
    }

    @Override
    public LoadFrame copy()
    {
      return new TestThenFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static int test_then_block(LexState ls, int escapelist)
  {
    /* test_then_block -> [IF | ELSEIF] cond THEN block */
    TestThenFrame l = new TestThenFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new TestThenFrame((TestThenFrame) luaZ_restore(ls.z.rz, LF_TESTTHEN));
    try
    {
      switch (l.site)
      {
      case 0:
        luaX_next(ls); /* skip IF or ELSEIF */
        l.site = 1;
      case 1:
        l.condtrue = cond(ls); /* read condition */
        l.site = 2;
      case 2:
        checknext(ls, TK_THEN);
        l.site = 3;
      case 3:
        block(ls); /* 'then' part */
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_TESTTHEN, l);
      throw ex;
    }
    if (ls.t.token == TK_ELSE ||
        ls.t.token == TK_ELSEIF)                                /* followed by 'else'/'elseif'? */
      escapelist = luaK_concat(fs, escapelist, luaK_jump(fs)); /* must jump over it */
    luaK_patchtohere(fs, l.condtrue);
    return escapelist;
  }

  /* the frame of 'ifstat' (see lload.h) */
  private static final class IfFrame extends LoadFrame
  {
    int escapelist = 0; /* exit list for finished parts */

    IfFrame()
    {
    }

    IfFrame(IfFrame o)
    {
      super(o);
      escapelist = o.escapelist;
    }

    @Override
    public LoadFrame copy()
    {
      return new IfFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static void ifstat(LexState ls, int line)
  {
    /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
    IfFrame l = new IfFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new IfFrame((IfFrame) luaZ_restore(ls.z.rz, LF_IFSTAT));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.escapelist = NO_JUMP;
          l.site = 1;
        case 1:
          l.escapelist = test_then_block(ls, l.escapelist); /* IF cond THEN block, or ELSEIF cond THEN block */
          l.site = 2;
        case 2: /* the loop */
          if (ls.t.token == TK_ELSEIF)
          {
            l.site = 1;
            continue;
          }
          l.site = 3;
        case 3:
          if (testnext(ls, TK_ELSE) == 0)
          {
            l.site = 5;
            continue;
          }
          l.site = 4;
        case 4:
          block(ls); /* 'else' part */
          l.site = 5;
        case 5:
          check_match(ls, TK_END, TK_IF, line);
          luaK_patchtohere(fs, l.escapelist); /* patch escape list to 'if' end */
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_IFSTAT, l);
      throw ex;
    }
  }

  /* the frame of 'localfunc' (see lload.h) */
  private static final class LocalfuncFrame extends LoadFrame
  {
    int fvar = 0; /* function's variable index */
    int line = 0;
    expdesc b = new expdesc();

    LocalfuncFrame()
    {
    }

    LocalfuncFrame(LocalfuncFrame o)
    {
      super(o);
      fvar = o.fvar;
      line = o.line;
      b = new expdesc(o.b);
    }

    @Override
    public LoadFrame copy()
    {
      return new LocalfuncFrame(this);
    }

    @Override
    public int size()
    {
      return 104;
    }
  }

  private static void localfunc(LexState ls)
  {
    LocalfuncFrame l = new LocalfuncFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new LocalfuncFrame((LocalfuncFrame) luaZ_restore(ls.z.rz, LF_LOCALFUNC));
    try
    {
      switch (l.site)
      {
      case 0:
      {
        l.fvar = fs.nactvar;
        TString name = str_checkname(ls);
        new_localvar(ls, name); /* new local variable */
        adjustlocalvars(ls, 1); /* enter its scope */
        l.line = ls.linenumber;
        l.site = 1;
      }
      case 1:
        body(ls, l.b, 0, l.line); /* function created in next register */
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_LOCALFUNC, l);
      throw ex;
    }
    /* debug information will only see the variable after this point! */
    localdebuginfo(fs, l.fvar).startpc = fs.pc;
  }

  /* the frame of 'getvarattribute' (see lload.h) */
  private static final class VarAttribFrame extends LoadFrame
  {
    TString ts = null;

    VarAttribFrame()
    {
    }

    VarAttribFrame(VarAttribFrame o)
    {
      super(o);
      ts = o.ts;
    }

    @Override
    public LoadFrame copy()
    {
      return new VarAttribFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static byte getvarattribute(LexState ls, byte df)
  {
    /* attrib -> ['<' NAME '>'] */
    VarAttribFrame l = new VarAttribFrame();
    if (ls.z.resuming)
      l = new VarAttribFrame((VarAttribFrame) luaZ_restore(ls.z.rz, LF_VARATTRIB));
    try
    {
      switch (l.site)
      {
      case 0:
        if (testnext(ls, '<') == 0)
          return df; /* return default value */
        l.site = 1;
      case 1:
        l.ts = str_checkname(ls);
        l.site = 2;
      case 2:
        checknext(ls, '>');
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_VARATTRIB, l);
      throw ex;
    }
    ByteView attr = l.ts.data.view();
    if (attr.equals(Lt.const_kw))
      return RDKCONST; /* read-only variable */
    else if (attr.equals(Lt.close_kw))
      return RDKTOCLOSE; /* to-be-closed variable */
    else
      luaK_semerror(ls, Lt.unknown_attribute_s, new FmtArgs().str(l.ts.data.view()));
    return df; /* return default value */
  }

  private static void checktoclose(FuncState fs, int level)
  {
    if (level != -1)
    { /* is there a to-be-closed variable? */
      marktobeclosed(fs);
      luaK_codeABC(fs, OP_TBC, reglevel(fs, level), 0, 0);
    }
  }

  /* the frame of 'localstat' (see lload.h) */
  private static final class LocalstatFrame extends LoadFrame
  {
    int toclose = 0; /* index of to-be-closed variable (if any) */
    int vidx = 0;    /* index of last variable */
    int nvars = 0;
    int nexps = 0;
    byte defkind = 0;
    TString vname = null;
    expdesc e = new expdesc();

    LocalstatFrame()
    {
    }

    LocalstatFrame(LocalstatFrame o)
    {
      super(o);
      toclose = o.toclose;
      vidx = o.vidx;
      nvars = o.nvars;
      nexps = o.nexps;
      defkind = o.defkind;
      vname = o.vname;
      e = new expdesc(o.e);
    }

    @Override
    public LoadFrame copy()
    {
      return new LocalstatFrame(this);
    }

    @Override
    public int size()
    {
      return 128;
    }
  }

  private static void localstat(LexState ls)
  {
    /* stat -> LOCAL NAME attrib { ',' NAME attrib } ['=' explist] */
    LocalstatFrame l = new LocalstatFrame();
    FuncState fs = ls.fs;
    Vardesc var; /* last variable */
    if (ls.z.resuming)
      l = new LocalstatFrame((LocalstatFrame) luaZ_restore(ls.z.rz, LF_LOCALSTAT));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.toclose = -1;
          l.nvars = 0;
          l.site = 1;
        case 1:
          /* get prefixed attribute (if any); default is regular local variable */
          l.defkind = getvarattribute(ls, (byte) VDKREG);
          l.site = 2;
        case 2:                          /* the loop: for each variable */
          l.vname = str_checkname(ls); /* get its name */
          l.site = 3;
        case 3:
        {
          byte kind = getvarattribute(ls, l.defkind); /* postfixed attribute */
          l.vidx = new_varkind(ls, l.vname, kind);    /* predeclare it */
          if (kind == RDKTOCLOSE)
          {                      /* to-be-closed? */
            if (l.toclose != -1) /* one already present? */
              luaK_semerror(ls, Lt.multiple_to_be_closed_variables_in_local_list, new FmtArgs());
            l.toclose = fs.nactvar + l.nvars;
          }
          l.nvars++;
          l.site = 4;
        }
        case 4:
          if (testnext(ls, ',') != 0)
          {
            l.site = 2;
            continue;
          }
          l.site = 5;
        case 5:
          if (testnext(ls, '=') == 0)
          {
            l.e.k = VVOID;
            l.nexps = 0;
            break;
          }
          l.site = 6; /* initialization */
        case 6:
          l.nexps = explist(ls, l.e);
          break;
        default:
          luaZ_badstate(ls.z.rz);
        }
        break; /* (the declaration is read) */
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_LOCALSTAT, l);
      throw ex;
    }
    var = getlocalvardesc(fs, l.vidx); /* retrieve last variable */
    if (l.nvars == l.nexps &&          /* no adjustments? */
        var.kind == RDKCONST &&        /* last variable is const? */
        luaK_exp2const(fs, l.e, var.k) != 0)
    {                                   /* compile-time constant? */
      var.kind = RDKCTC;                /* variable is a compile-time constant */
      adjustlocalvars(ls, l.nvars - 1); /* exclude last variable */
      fs.nactvar++;                     /* but count it */
    }
    else
    {
      adjust_assign(ls, l.nvars, l.nexps, l.e);
      adjustlocalvars(ls, l.nvars);
    }
    checktoclose(fs, l.toclose);
  }

  private static byte getglobalattribute(LexState ls, byte df)
  {
    byte kind = getvarattribute(ls, df);
    switch (kind)
    {
    case RDKTOCLOSE:
      luaK_semerror(ls, Lt.global_variables_cannot_be_to_be_closed, new FmtArgs());
      return kind; /* to avoid warnings */
    case RDKCONST:
      return GDKCONST; /* adjust kind for global variable */
    default:
      return kind;
    }
  }

  private static void checkglobal(LexState ls, TString varname, int line)
  {
    FuncState fs = ls.fs;
    expdesc var = new expdesc();
    int k;
    buildglobal(ls, varname, var); /* create global variable in 'var' */
    k = var.ind.keystr;            /* index of global name in 'k' */
    luaK_codecheckglobal(fs, var, k, line);
  }

  /* the frame of 'initglobal' (see lload.h) */
  private static final class InitglobalFrame extends LoadFrame
  {
    TString varname = null;
    expdesc e = new expdesc();
    expdesc var = new expdesc();

    InitglobalFrame()
    {
    }

    InitglobalFrame(InitglobalFrame o)
    {
      super(o);
      varname = o.varname;
      e = new expdesc(o.e);
      var = new expdesc(o.var);
    }

    @Override
    public LoadFrame copy()
    {
      return new InitglobalFrame(this);
    }

    @Override
    public int size()
    {
      return 176;
    }
  }

  /*
  ** Recursively traverse list of globals to be initalized. When
  ** going, generate table description for the global. In the end,
  ** after all indices have been generated, read list of initializing
  ** expressions. When returning, generate the assignment of the value on
  ** the stack to the corresponding table description. 'n' is the variable
  ** being handled, range [0, nvars - 1].
  */
  private static void initglobal(LexState ls, int nvars, int firstidx, int n,
                                 int line)
  {
    InitglobalFrame l = new InitglobalFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new InitglobalFrame((InitglobalFrame) luaZ_restore(ls.z.rz, LF_INITGLOBAL));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          if (n != nvars)
          { /* handle variable 'n' */
            l.varname = getlocalvardesc(fs, firstidx + n).name;
            buildglobal(ls, l.varname, l.var); /* create global variable in 'var' */
            l.site = 2;
            continue;
          }
          /* traversed all variables */
          l.site = 1;
        case 1:
        {
          int nexps = explist(ls, l.e); /* read list of expressions */
          adjust_assign(ls, nvars, nexps, l.e);
          return;
        }
        case 2:
          enterlevel(ls); /* control recursion depth */
          initglobal(ls, nvars, firstidx, n + 1, line);
          leavelevel(ls);
          checkglobal(ls, l.varname, line);
          storevartop(fs, l.var);
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_INITGLOBAL, l);
      throw ex;
    }
  }

  /* the frame of 'globalnames' (see lload.h) */
  private static final class GlobalnamesFrame extends LoadFrame
  {
    int nvars = 0;
    int lastidx = 0; /* index of last registered variable */
    int line = 0;
    TString vname = null;

    GlobalnamesFrame()
    {
    }

    GlobalnamesFrame(GlobalnamesFrame o)
    {
      super(o);
      nvars = o.nvars;
      lastidx = o.lastidx;
      line = o.line;
      vname = o.vname;
    }

    @Override
    public LoadFrame copy()
    {
      return new GlobalnamesFrame(this);
    }

    @Override
    public int size()
    {
      return 48;
    }
  }

  private static void globalnames(LexState ls, byte defkind)
  {
    GlobalnamesFrame l = new GlobalnamesFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new GlobalnamesFrame((GlobalnamesFrame) luaZ_restore(ls.z.rz, LF_GLOBALNAMES));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.nvars = 0;
          l.site = 1;
        case 1: /* the loop: for each name */
          l.vname = str_checkname(ls);
          l.site = 2;
        case 2:
        {
          byte kind = getglobalattribute(ls, defkind);
          l.lastidx = new_varkind(ls, l.vname, kind);
          l.nvars++;
          l.site = 3;
        }
        case 3:
          if (testnext(ls, ',') != 0)
          {
            l.site = 1;
            continue;
          }
          l.site = 4;
        case 4:
          if (testnext(ls, '=') == 0)
            break;
          /* initialization */
          l.line = ls.linenumber;
          l.site = 5;
        case 5:
          initglobal(ls, l.nvars, l.lastidx - l.nvars + 1, 0, l.line);
          break;
        default:
          luaZ_badstate(ls.z.rz);
        }
        break; /* (the names are read) */
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_GLOBALNAMES, l);
      throw ex;
    }
    fs.nactvar = (short) (fs.nactvar + l.nvars); /* activate declaration */
  }

  /* the frame of 'globalstat' (see lload.h) */
  private static final class GlobalstatFrame extends LoadFrame
  {
    byte defkind = 0;

    GlobalstatFrame()
    {
    }

    GlobalstatFrame(GlobalstatFrame o)
    {
      super(o);
      defkind = o.defkind;
    }

    @Override
    public LoadFrame copy()
    {
      return new GlobalstatFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static void globalstat(LexState ls)
  {
    /* globalstat -> (GLOBAL) attrib '*'
       globalstat -> (GLOBAL) attrib NAME attrib {',' NAME attrib} */
    GlobalstatFrame l = new GlobalstatFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new GlobalstatFrame((GlobalstatFrame) luaZ_restore(ls.z.rz, LF_GLOBALSTAT));
    try
    {
      switch (l.site)
      {
      case 0:
        /* get prefixed attribute (if any); default is regular global variable */
        l.defkind = getglobalattribute(ls, (byte) GDKREG);
        l.site = 1;
      case 1:
        if (testnext(ls, '*') != 0)
        {
          new_varkind(ls, null, l.defkind);
          fs.nactvar++; /* activate declaration */
          break;
        }
        l.site = 2;
      case 2:
        globalnames(ls, l.defkind);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_GLOBALSTAT, l);
      throw ex;
    }
  }

  /* the frame of 'globalfunc' (see lload.h) */
  private static final class GlobalfuncFrame extends LoadFrame
  {
    int bline = 0;
    TString fname = null;
    expdesc var = new expdesc(), b = new expdesc();

    GlobalfuncFrame()
    {
    }

    GlobalfuncFrame(GlobalfuncFrame o)
    {
      super(o);
      bline = o.bline;
      fname = o.fname;
      var = new expdesc(o.var);
      b = new expdesc(o.b);
    }

    @Override
    public LoadFrame copy()
    {
      return new GlobalfuncFrame(this);
    }

    @Override
    public int size()
    {
      return 184;
    }
  }

  private static void globalfunc(LexState ls, int line)
  {
    /* globalfunc -> (GLOBAL FUNCTION) NAME body */
    GlobalfuncFrame l = new GlobalfuncFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new GlobalfuncFrame((GlobalfuncFrame) luaZ_restore(ls.z.rz, LF_GLOBALFUNC));
    try
    {
      switch (l.site)
      {
      case 0:
        l.fname = str_checkname(ls);
        new_varkind(ls, l.fname, (byte) GDKREG); /* declare global variable */
        fs.nactvar++;                            /* enter its scope */
        buildglobal(ls, l.fname, l.var);
        l.bline = ls.linenumber;
        l.site = 1;
      case 1:
        body(ls, l.b, 0, l.bline); /* compile and return closure in 'b' */
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_GLOBALFUNC, l);
      throw ex;
    }
    checkglobal(ls, l.fname, line);
    luaK_storevar(fs, l.var, l.b);
    luaK_fixline(fs, line); /* definition "happens" in the first line */
  }

  private static void globalstatfunc(LexState ls, int line)
  {
    /* stat -> GLOBAL globalfunc | GLOBAL globalstat */
    LoadFrame l = new LoadFrame();
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_GLOBALSTATFUNC));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          luaX_next(ls); /* skip 'global' */
          l.site = 1;
        case 1:
          l.site = (testnext(ls, TK_FUNCTION) != 0) ? 2 : 3;
          continue;
        case 2:
          globalfunc(ls, line);
          return;
        case 3:
          globalstat(ls);
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_GLOBALSTATFUNC, l);
      throw ex;
    }
  }

  /* the frame of 'funcname' (see lload.h) */
  private static final class FuncnameFrame extends LoadFrame
  {
    int ismethod = 0;

    FuncnameFrame()
    {
    }

    FuncnameFrame(FuncnameFrame o)
    {
      super(o);
      ismethod = o.ismethod;
    }

    @Override
    public LoadFrame copy()
    {
      return new FuncnameFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static int funcname(LexState ls, expdesc v)
  {
    /* funcname -> NAME {fieldsel} [':' NAME] */
    FuncnameFrame l = new FuncnameFrame();
    if (ls.z.resuming)
      l = new FuncnameFrame((FuncnameFrame) luaZ_restore(ls.z.rz, LF_FUNCNAME));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.ismethod = 0;
          singlevar(ls, v);
          l.site = 1;
        case 1: /* the loop */
          if (ls.t.token == '.')
            l.site = 2;
          else if (ls.t.token == ':')
          {
            l.ismethod = 1;
            l.site = 3;
          }
          else
            return l.ismethod;
          continue;
        case 2:
          fieldsel(ls, v);
          l.site = 1;
          continue;
        case 3:
          fieldsel(ls, v);
          return l.ismethod;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_FUNCNAME, l);
      throw ex;
    }
  }

  /* the frame of 'funcstat' (see lload.h) */
  private static final class FuncstatFrame extends LoadFrame
  {
    int ismethod = 0;
    expdesc v = new expdesc(), b = new expdesc();

    FuncstatFrame()
    {
    }

    FuncstatFrame(FuncstatFrame o)
    {
      super(o);
      ismethod = o.ismethod;
      v = new expdesc(o.v);
      b = new expdesc(o.b);
    }

    @Override
    public LoadFrame copy()
    {
      return new FuncstatFrame(this);
    }

    @Override
    public int size()
    {
      return 176;
    }
  }

  private static void funcstat(LexState ls, int line)
  {
    /* funcstat -> FUNCTION funcname body */
    FuncstatFrame l = new FuncstatFrame();
    if (ls.z.resuming)
      l = new FuncstatFrame((FuncstatFrame) luaZ_restore(ls.z.rz, LF_FUNCSTAT));
    try
    {
      switch (l.site)
      {
      case 0:
        luaX_next(ls); /* skip FUNCTION */
        l.site = 1;
      case 1:
        l.ismethod = funcname(ls, l.v);
        check_readonly(ls, l.v);
        l.site = 2;
      case 2:
        body(ls, l.b, l.ismethod, line);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_FUNCSTAT, l);
      throw ex;
    }
    luaK_storevar(ls.fs, l.v, l.b);
    luaK_fixline(ls.fs, line); /* definition "happens" in the first line */
  }

  /* the frame of 'exprstat' (see lload.h) */
  private static final class ExprstatFrame extends LoadFrame
  {
    LHS_assign v = new LHS_assign();

    ExprstatFrame()
    {
    }

    ExprstatFrame(ExprstatFrame o)
    {
      super(o);
      v = new LHS_assign(o.v);
    }

    @Override
    public LoadFrame copy()
    {
      return new ExprstatFrame(this);
    }

    @Override
    public int size()
    {
      return 104;
    }
  }

  private static void exprstat(LexState ls)
  {
    /* stat -> func | assignment */
    ExprstatFrame l = new ExprstatFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new ExprstatFrame((ExprstatFrame) luaZ_restore(ls.z.rz, LF_EXPRSTAT));
    try
    {
      switch (l.site)
      {
      case 0:
        suffixedexp(ls, l.v.v);
        if (ls.t.token != '=' && ls.t.token != ',')
        { /* stat -> func */
          int[] code = fs.f.code;
          check_condition(ls, (l.v.v.k == VCALL) ? 1 : 0, Lt.syntax_error);
          int pc = l.v.v.info;
          code[pc] = SETARG_C(code[pc], 1); /* call statement uses no results */
          break;
        }
        /* stat -> assignment */
        l.v.prev = null;
        l.site = 1;
      case 1:
        restassign(ls, l.v, 1);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_EXPRSTAT, l);
      throw ex;
    }
  }

  /* the frame of 'retstat' (see lload.h) */
  private static final class RetstatFrame extends LoadFrame
  {
    int nret = 0;  /* number of values being returned */
    int first = 0; /* first slot to be returned */
    expdesc e = new expdesc();

    RetstatFrame()
    {
    }

    RetstatFrame(RetstatFrame o)
    {
      super(o);
      nret = o.nret;
      first = o.first;
      e = new expdesc(o.e);
    }

    @Override
    public LoadFrame copy()
    {
      return new RetstatFrame(this);
    }

    @Override
    public int size()
    {
      return 104;
    }
  }

  private static void retstat(LexState ls)
  {
    /* stat -> RETURN [explist] [';'] */
    RetstatFrame l = new RetstatFrame();
    FuncState fs = ls.fs;
    if (ls.z.resuming)
      l = new RetstatFrame((RetstatFrame) luaZ_restore(ls.z.rz, LF_RETSTAT));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.first = luaY_nvarstack(fs);
          if (block_follow(ls, 1) != 0 || ls.t.token == ';')
          {
            l.nret = 0; /* return no values */
            l.site = 2;
            continue;
          }
          l.site = 1;
        case 1:
          l.nret = explist(ls, l.e); /* optional return values */
          if (hasmultret(l.e.k) != 0)
          {
            luaK_setmultret(fs, l.e);
            if (l.e.k == VCALL && l.nret == 1 && fs.bl.insidetbc == 0)
            { /* tail call? */
              fs.f.code[l.e.info] = SET_OPCODE(fs.f.code[l.e.info], OP_TAILCALL);
              assert GETARG_A(fs.f.code[l.e.info]) == luaY_nvarstack(fs);
            }
            l.nret = LUA_MULTRET; /* return all values */
          }
          else
          {
            if (l.nret == 1)                     /* only one single value? */
              l.first = luaK_exp2anyreg(fs, l.e); /* can use original slot */
            else
            { /* values must go to the top of the stack */
              luaK_exp2nextreg(fs, l.e);
              assert l.nret == fs.freereg - l.first;
            }
          }
          l.site = 2;
        case 2:
          luaK_ret(fs, l.first, l.nret);
          l.site = 3;
        case 3:
          testnext(ls, ';'); /* skip optional semicolon */
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_RETSTAT, l);
      throw ex;
    }
  }

  /* the frame of 'statement' (see lload.h) */
  private static final class StatementFrame extends LoadFrame
  {
    int line = 0; /* may be needed for error messages */
    TString name = null;

    StatementFrame()
    {
    }

    StatementFrame(StatementFrame o)
    {
      super(o);
      line = o.line;
      name = o.name;
    }

    @Override
    public LoadFrame copy()
    {
      return new StatementFrame(this);
    }

    @Override
    public int size()
    {
      return 40;
    }
  }

  private static void statement(LexState ls)
  {
    StatementFrame l = new StatementFrame();
    enterlevel(ls);
    if (ls.z.resuming)
      l = new StatementFrame((StatementFrame) luaZ_restore(ls.z.rz, LF_STATEMENT));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.line = ls.linenumber;
          switch (ls.t.token)
          {
          case ';': /* stat -> ';' (empty statement) */
            l.site = 1;
            break;
          case TK_IF: /* stat -> ifstat */
            l.site = 2;
            break;
          case TK_WHILE: /* stat -> whilestat */
            l.site = 3;
            break;
          case TK_DO: /* stat -> DO block END */
            l.site = 4;
            break;
          case TK_FOR: /* stat -> forstat */
            l.site = 7;
            break;
          case TK_REPEAT: /* stat -> repeatstat */
            l.site = 8;
            break;
          case TK_FUNCTION: /* stat -> funcstat */
            l.site = 9;
            break;
          case TK_LOCAL: /* stat -> localstat */
            l.site = 10;
            break;
          case TK_GLOBAL: /* stat -> globalstatfunc */
            l.site = 14;
            break;
          case TK_DBCOLON: /* stat -> label */
            l.site = 15;
            break;
          case TK_RETURN: /* stat -> retstat */
            l.site = 18;
            break;
          case TK_BREAK: /* stat -> breakstat */
            l.site = 20;
            break;
          case TK_GOTO: /* stat -> 'goto' NAME */
            l.site = 21;
            break;
          default: /* stat -> func | assignment */
            l.site = 23;
            break;
          }
          continue;
        case 1:
          luaX_next(ls); /* skip ';' */
          break;
        case 2:
          ifstat(ls, l.line);
          break;
        case 3:
          whilestat(ls, l.line);
          break;
        case 4:
          luaX_next(ls); /* skip DO */
          l.site = 5;
        case 5:
          block(ls);
          l.site = 6;
        case 6:
          check_match(ls, TK_END, TK_DO, l.line);
          break;
        case 7:
          forstat(ls, l.line);
          break;
        case 8:
          repeatstat(ls, l.line);
          break;
        case 9:
          funcstat(ls, l.line);
          break;
        case 10:
          luaX_next(ls); /* skip LOCAL */
          l.site = 11;
        case 11:
          l.site = (testnext(ls, TK_FUNCTION) != 0) ? 12 : 13; /* local function? */
          continue;
        case 12:
          localfunc(ls);
          break;
        case 13:
          localstat(ls);
          break;
        case 14:
          globalstatfunc(ls, l.line);
          break;
        case 15:
          luaX_next(ls); /* skip double colon */
          l.site = 16;
        case 16:
          l.name = str_checkname(ls);
          l.site = 17;
        case 17:
          labelstat(ls, l.name, l.line);
          break;
        case 18:
          luaX_next(ls); /* skip RETURN */
          l.site = 19;
        case 19:
          retstat(ls);
          break;
        case 20:
          breakstat(ls, l.line);
          break;
        case 21:
          luaX_next(ls); /* skip 'goto' */
          l.site = 22;
        case 22:
          gotostat(ls, l.line);
          break;
        case 23:
          exprstat(ls);
          break;
        default:
          luaZ_badstate(ls.z.rz);
        }
        break; /* (the statement is read) */
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_STATEMENT, l);
      throw ex;
    }
    assert ls.fs.f.maxstacksize >= ls.fs.freereg &&
           ls.fs.freereg >= luaY_nvarstack(ls.fs);
    ls.fs.freereg = luaY_nvarstack(ls.fs); /* free registers */
    leavelevel(ls);
  }

  /* }====================================================================== */

  /* }====================================================================== */

  /*
  ** compiles the main function, which is a regular vararg function with an
  ** upvalue named LUA_ENV
  */
  private static void mainfunc(LexState ls, FuncState fs)
  {
    BlockFrame l = new BlockFrame();
    if (ls.z.resuming)
    { /* (its function and block again in the chains) */
      l = new BlockFrame((BlockFrame) luaZ_restore(ls.z.rz, LF_MAINFUNC));
      fs.prev = null;
      fs.ls = ls;
      ls.fs = fs;
      l.bl.previous = null;
      fs.bl = l.bl;
    }
    try
    {
      switch (l.site)
      {
      case 0:
      {
        open_func(ls, fs, l.bl);
        setvararg(fs);                    /* main function is always vararg */
        Upvaldesc env = allocupvalue(fs); /* ...set environment upvalue */
        env.instack = 1;
        env.idx = 0;
        env.kind = VDKREG;
        env.name = ls.envn;
        luaC_objbarrier(ls.L, fs.f, env.name);
        l.site = 1;
      }
      case 1:
        luaX_next(ls); /* read first token */
        l.site = 2;
      case 2:
        statlist(ls); /* parse main body */
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(ls.z.rz, LF_MAINFUNC, l);
      throw ex;
    }
    check(ls, TK_EOS);
    close_func(ls);
  }

  /* the frame of 'luaY_parser' (see lload.h) */
  private static final class ParserFrame extends LoadFrame
  {
    LexState lexstate = new LexState();
    FuncState funcstate = new FuncState();
    LClosure cl = null;

    ParserFrame()
    {
    }

    ParserFrame(ParserFrame o)
    {
      super(o);
      lexstate = new LexState(o.lexstate);
      funcstate = new FuncState(o.funcstate);
      cl = o.cl;
    }

    @Override
    public LoadFrame copy()
    {
      return new ParserFrame(this);
    }

    @Override
    public int size()
    {
      return 288;
    }
  }

  public static LClosure luaY_parser(lua_State L, ZIO z, Table anchor, MBuffer buff, Dyndata dyd, ByteView name, int firstchar)
  {
    ParserFrame l = new ParserFrame();
    if (z.resuming)
    { /* (the scanner again with this load's stream, buffer and lists) */
      l = new ParserFrame((ParserFrame) luaZ_restore(z.rz, LF_PARSER));
      l.lexstate.L = L;
      l.lexstate.z = z;
      l.lexstate.buff = buff;
      l.lexstate.dyd = dyd;
      l.lexstate.h = anchor;
      l.lexstate.fs = null;
    }
    try
    {
      switch (l.site)
      {
      case 0:
        l.lexstate.h = anchor;                   /* table for scanner */
        l.cl = LClosure.luaF_newLclosure(L, 1); /* create main closure */
        luaZ_madefunction(z.rz, l.cl);
        assert novariant(l.cl.tt) >= LUA_TSTRING;
        luaD_anchorobj(L, anchor, l.cl); /* anchor it in scanner table */
        l.funcstate.f = l.cl.p = Proto.luaF_newproto(L);
        luaC_objbarrier(L, l.cl, l.cl.p);
        l.funcstate.f.source = TString.luaS_createstr(L, name); /* create and anchor TString */
        luaC_objbarrier(L, l.funcstate.f, l.funcstate.f.source);
        l.lexstate.buff = buff;
        l.lexstate.dyd = dyd;
        dyd.actvar.n = dyd.gt.n = dyd.label.n = 0;
        luaX_setinput(L, l.lexstate, z, l.funcstate.f.source, firstchar);
        l.site = 1;
      case 1:
        mainfunc(l.lexstate, l.funcstate);
        break;
      default:
        luaZ_badstate(z.rz);
      }
    }
    catch (Throwable ex)
    {
      luaZ_record(z.rz, LF_PARSER, l);
      throw ex;
    }
    assert l.funcstate.prev == null && l.funcstate.nups == 1 && l.lexstate.fs == null;
    /* all scopes should be correctly finished */
    assert dyd.actvar.n == 0 && dyd.gt.n == 0 && dyd.label.n == 0;
    return l.cl;
  }
}
