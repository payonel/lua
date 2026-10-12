package lua.runtime;

import lua.api.lua_KFunction;

public final class CallInfo
{
  public int func = 0;              /* function index in the stack */
  public int top = 0;               /* top for this function */
  public CallInfo previous = null;  /* dynamic call link */
  public CallInfo next = null;
  public int savedpc = 0;
  public volatile int trap = 0;     /* function is tracing lines/counts */
  public int nextraargs = 0;        /* # of extra arguments in vararg functions */
  public lua_KFunction k = null;    /* continuation in case of yields */
  public int old_errfunc = 0;
  public long ctx = 0;              /* context info. in case of yields */
  public int funcidx = 0;           /* called-function index */
  public int nyield = 0;            /* number of values yielded */
  public int nres = 0;              /* number of values returned */
  public int callstatus = 0;
  public int nccalls = 0;           /* CIST_CCOUNT: 'nCcalls' while it runs its own code */
  public byte apiop = 0;            /* CIST_APIK: how to finish the API call (APIK_*) */
  public int apifirst = 0;          /* CIST_APIK, a concatenation: its first value */
  /* CIST_HOOKED: what a hook function that freezes needs to be finished
     after a thaw (see 'luaD_finishhook') */
  public byte hookev = 0;           /* the event the hook runs for (HE_*) */
  public byte hookdone = 0;         /* HE_INSTR: the hooks already called for this instruction (HD_*) */
  public int hooktop = 0;           /* 'L->top' before the hook */
  public int hookcitop = 0;         /* 'top' of the frame before the hook */
  public int hookdelta = 0;         /* a return hook of a vararg function: 'func' was moved this much */
  public int hookres = 0;           /* a return hook after closing variables: where the results go */
}
