/*
** $Id: lparser.c $
** Lua Parser
** See Copyright Notice in lua.h
*/


#include <limits.h>
#include <string.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"

#include "lhack.h"
#include "literals.h"
#include "lload.h"

namespace lua::compiler
{

/*
** A load that a freeze can stop (see lload.h): a call at site 'n' of a
** function's frame records the frame (its locals, 'l') if a freeze passes
** through it; the function made again goes to the site of its frame.
*/
#define LZ_SITE(n, ...)                            \
  site_##n:                                        \
  try                                              \
  {                                                \
    __VA_ARGS__;                                   \
  }                                                \
  catch (...)                                      \
  {                                                \
    luaZ_record(ls->z->rz, FN, n, &l, sizeof(l));  \
    throw;                                         \
  }

#define LZ_RESUMING() l_unlikely(ls->z->resuming)
#define LZ_SITENO() luaZ_restore(ls->z->rz, FN, &l, sizeof(l))
#define LZ_BADSITE() luaZ_badstate(ls->z->rz)

struct NoLocals
{
  char none = 0;
};

/* maximum number of variable declarations per function (must be
   smaller than 250, due to the bytecode format) */
constexpr int MAXVARS = 200;

constexpr int hasmultret(expkind k)
{
  return k == VCALL || k == VVARARG;
}

/* because all strings are unified by the scanner, the parser
   can use pointer equality for string equality */
static inline int eqstr(const TString* a, const TString* b)
{
  return a == b;
}

/*
** nodes for block list (list of active blocks)
*/
struct BlockCnt
{
  struct BlockCnt* previous; /* chain */
  int firstlabel;            /* index of first label in this block */
  int firstgoto;             /* index of first pending goto in this block */
  short nactvar;             /* number of active declarations at block entry */
  lu_byte upval;             /* true if some variable in the block is an upvalue */
  lu_byte isloop;            /* 1 if 'block' is a loop; 2 if it has pending breaks */
  lu_byte insidetbc;         /* true if inside the scope of a to-be-closed var. */
};

/*
** prototypes for recursive non-terminal functions
*/
static void statement(LexState* ls);
static void expr(LexState* ls, expdesc* v);

[[noreturn]] static void error_expected(LexState* ls, int token)
{
  luaX_syntaxerror(ls, luaO_pushfstring(ls->L, lt::s_expected, FmtArgs().str(luaX_token2str(ls, token)))->view());
}

[[noreturn]] static void errorlimit(FuncState* fs, int limit, byte_view what)
{
  lua_State* L = fs->ls->L;
  const InternalString* msg;
  int line = fs->f->linedefined;
  byte_view where = (line == 0)
                          ? lt::main_function
                          : luaO_pushfstring(L, lt::function_at_line_d, FmtArgs().num(line))->view();
  msg = luaO_pushfstring(L, lt::too_many_s_limit_is_d_in_s, FmtArgs().str(what).num(limit).str(where));
  luaX_syntaxerror(fs->ls, msg->view());
}

void luaY_checklimit(FuncState* fs, int v, int l, byte_view what)
{
  if (l_unlikely(v > l))
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
static int testnext(LexState* ls, int c)
{
  if (ls->t.token == c)
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
static void check(LexState* ls, int c)
{
  if (ls->t.token != c)
    error_expected(ls, c);
}

/*
** Check that next token is 'c' and skip it.
*/
static void checknext(LexState* ls, int c)
{
  check(ls, c);
  luaX_next(ls);
}

static inline void check_condition(LexState* ls, int c, byte_view msg)
{
  if (!c)
    luaX_syntaxerror(ls, msg);
}

/*
** Check that next token is 'what' and skip it. In case of error,
** raise an error that the expected 'what' should match a 'who'
** in line 'where' (if that is not the current line).
*/
static void check_match(LexState* ls, int what, int who, int where)
{
  if (l_unlikely(!testnext(ls, what)))
  {
    if (where == ls->linenumber) /* all in the same line? */
      error_expected(ls, what);  /* do not need a complex message */
    else
    {
      luaX_syntaxerror(ls, luaO_pushfstring(ls->L,
                                            lt::s_expected_to_close_s_at_line_d,
                                            FmtArgs()
                                                .str(luaX_token2str(ls, what))
                                                .str(luaX_token2str(ls, who))
                                                .num(where))->view());
    }
  }
}

static TString* str_checkname(LexState* ls)
{
  TString* ts;
  check(ls, TK_NAME);
  ts = ls->t.ts;
  luaX_next(ls);
  return ts;
}

static void init_exp(expdesc* e, expkind k, int i)
{
  e->f = e->t = NO_JUMP;
  e->k = k;
  e->info = i;
}

static void codestring(expdesc* e, TString* s)
{
  e->f = e->t = NO_JUMP;
  e->k = VKSTR;
  e->strval = s;
}

static void codename(LexState* ls, expdesc* e)
{
  codestring(e, str_checkname(ls));
}

/*
** Register a new local variable in the active 'Proto' (for debug
** information).
*/
static short registerlocalvar(LexState* ls, FuncState* fs,
                              TString* varname)
{
  Proto* f = fs->f;
  int oldsize = (int)f->locvars.size();
  f->locvars = luaM_safegrow_LocVar_span(ls->L, f->locvars, fs->ndebugvars);
  while (oldsize < (int)f->locvars.size())
    f->locvars[oldsize++].varname = nullptr;
  f->locvars[fs->ndebugvars].varname = varname;
  f->locvars[fs->ndebugvars].startpc = fs->pc;
  luaC_objbarrier(ls->L, f, varname);
  return fs->ndebugvars++;
}

/*
** Create a new variable with the given 'name' and given 'kind'.
** Return its index in the function.
*/
static int new_varkind(LexState* ls, TString* name, lu_byte kind)
{
  lua_State* L = ls->L;
  FuncState* fs = ls->fs;
  Dyndata* dyd = ls->dyd;
  Vardesc* var;
  {
    auto s = luaM_safegrow_Vardesc_span(L, std::span<Vardesc>(dyd->actvar.arr, dyd->actvar.size), dyd->actvar.n + 1);
    dyd->actvar.arr = s.data();
    dyd->actvar.size = (int)s.size();
  }
  var = &dyd->actvar.arr[dyd->actvar.n++];
  var->kind = kind; /* default */
  var->name = name;
  return dyd->actvar.n - 1 - fs->firstlocal;
}

/*
** Create a new local variable with the given 'name' and regular kind.
*/
static int new_localvar(LexState* ls, TString* name)
{
  return new_varkind(ls, name, VDKREG);
}

/*
** Return the "variable description" (Vardesc) of a given variable.
** (Unless noted otherwise, all variables are referred to by their
** compiler indices.)
*/
static Vardesc* getlocalvardesc(FuncState* fs, int vidx)
{
  return &fs->ls->dyd->actvar.arr[fs->firstlocal + vidx];
}

/*
** Convert 'nvar', a compiler index level, to its corresponding
** register. For that, search for the highest variable below that level
** that is in a register and uses its register index ('ridx') plus one.
*/
static int reglevel(FuncState* fs, int nvar)
{
  while (nvar-- > 0)
  {
    Vardesc* vd = getlocalvardesc(fs, nvar); /* get previous variable */
    if (varinreg(vd))                        /* is in a register? */
      return vd->ridx + 1;
  }
  return 0; /* no variables in registers */
}

/*
** Return the number of variables in the register stack for the given
** function.
*/
int luaY_nvarstack(FuncState* fs)
{
  return reglevel(fs, fs->nactvar);
}

/*
** Get the debug-information entry for current variable 'vidx'.
*/
static LocVar* localdebuginfo(FuncState* fs, int vidx)
{
  Vardesc* vd = getlocalvardesc(fs, vidx);
  if (!varinreg(vd))
    return nullptr; /* no debug info. for constants */
  else
  {
    int idx = vd->pidx;
    assert(idx < fs->ndebugvars);
    return &fs->f->locvars[idx];
  }
}

/*
** Create an expression representing variable 'vidx'
*/
static void init_var(FuncState* fs, expdesc* e, int vidx)
{
  e->f = e->t = NO_JUMP;
  e->k = VLOCAL;
  e->var.vidx = (short)vidx;
  e->var.ridx = getlocalvardesc(fs, vidx)->ridx;
}

/*
** Raises an error if variable described by 'e' is read only; moreover,
** if 'e' is t[exp] where t is the vararg parameter, change it to index
** a real table. (Virtual vararg tables cannot be changed.)
*/
static void check_readonly(LexState* ls, expdesc* e)
{
  FuncState* fs = ls->fs;
  TString* varname = nullptr; /* to be set if variable is const */
  switch (e->k)
  {
  case VCONST:
  {
    varname = ls->dyd->actvar.arr[e->info].name;
    break;
  }
  case VLOCAL:
  case VVARGVAR:
  {
    Vardesc* vardesc = getlocalvardesc(fs, e->var.vidx);
    if (vardesc->kind != VDKREG) /* not a regular variable? */
      varname = vardesc->name;
    break;
  }
  case VUPVAL:
  {
    Upvaldesc* up = &fs->f->upvalues[e->info];
    if (up->kind != VDKREG)
      varname = up->name;
    break;
  }
  case VVARGIND:
  {
    needvatab(fs->f); /* function will need a vararg table */
    e->k = VINDEXED;
  } /* FALLTHROUGH */
  case VINDEXUP:
  case VINDEXSTR:
  case VINDEXED:
  {                  /* global variable */
    if (e->ind.ro) /* read-only? */
    {
      TValue* kv = &fs->f->k[e->ind.keystr];
      assert(ttisstring(kv));
      varname = tsvalue(kv);
    }
    break;
  }
  default:
    assert(e->k == VINDEXI); /* this one doesn't need any check */
    return;                      /* integer index cannot be read-only */
  }
  if (varname)
    luaK_semerror(ls, lt::attempt_to_assign_to_const_variable_s,
                  FmtArgs().str(varname->data.view()));
}

/*
** Start the scope for the last 'nvars' created variables.
*/
static void adjustlocalvars(LexState* ls, int nvars)
{
  FuncState* fs = ls->fs;
  int reglevel = luaY_nvarstack(fs);
  int i;
  for (i = 0; i < nvars; i++)
  {
    int vidx = fs->nactvar++;
    Vardesc* var = getlocalvardesc(fs, vidx);
    var->ridx = reglevel++;
    var->pidx = registerlocalvar(ls, fs, var->name);
    luaY_checklimit(fs, reglevel, MAXVARS, lt::local_variables);
  }
}

/*
** Close the scope for all variables up to level 'tolevel'.
** (debug info.)
*/
static void removevars(FuncState* fs, int tolevel)
{
  fs->ls->dyd->actvar.n -= fs->nactvar - tolevel;
  while (fs->nactvar > tolevel)
  {
    LocVar* var = localdebuginfo(fs, --fs->nactvar);
    if (var) /* does it have debug information? */
      var->endpc = fs->pc;
  }
}

/*
** Search the upvalues of the function 'fs' for one
** with the given 'name'.
*/
static int searchupvalue(FuncState* fs, TString* name)
{
  int i;
  Upvaldesc* up = fs->f->upvalues.data();
  for (i = 0; i < fs->nups; i++)
  {
    if (eqstr(up[i].name, name))
      return i;
  }
  return -1; /* not found */
}

static Upvaldesc* allocupvalue(FuncState* fs)
{
  Proto* f = fs->f;
  int oldsize = (int)f->upvalues.size();
  luaY_checklimit(fs, fs->nups + 1, MAXUPVAL, lt::upvalues);
  f->upvalues = luaM_safegrow_Upvaldesc_span(fs->ls->L, f->upvalues, fs->nups);
  while (oldsize < (int)f->upvalues.size())
    f->upvalues[oldsize++].name = nullptr;
  return &f->upvalues[fs->nups++];
}

static int newupvalue(FuncState* fs, TString* name, expdesc* v)
{
  Upvaldesc* up = allocupvalue(fs);
  FuncState* prev = fs->prev;
  if (v->k == VLOCAL)
  {
    up->instack = 1;
    up->idx = v->var.ridx;
    up->kind = getlocalvardesc(prev, v->var.vidx)->kind;
    assert(eqstr(name, getlocalvardesc(prev, v->var.vidx)->name));
  }
  else
  {
    up->instack = 0;
    up->idx = v->info;
    up->kind = prev->f->upvalues[v->info].kind;
    assert(eqstr(name, prev->f->upvalues[v->info].name));
  }
  up->name = name;
  luaC_objbarrier(fs->ls->L, fs->f, name);
  return fs->nups - 1;
}

static int searchvar(FuncState* fs, TString* n, expdesc* var)
{
  int i;
  for (i = (int)fs->nactvar - 1; i >= 0; i--)
  {
    Vardesc* vd = getlocalvardesc(fs, i);
    if (varglobal(vd))
    { /* global declaration? */
      if (vd->name == nullptr)
      {                                     /* collective declaration? */
        if (var->info < 0)                /* no previous collective declaration? */
          var->info = fs->firstlocal + i; /* this is the first one */
      }
      else
      { /* global name */
        if (eqstr(n, vd->name))
        { /* found? */
          init_exp(var, VGLOBAL, fs->firstlocal + i);
          return VGLOBAL;
        }
        else if (var->info == -1) /* active preambular declaration? */
          var->info = -2;         /* invalidate preambular declaration */
      }
    }
    else if (eqstr(n, vd->name))
    {                            /* found? */
      if (vd->kind == RDKCTC) /* compile-time constant? */
        init_exp(var, VCONST, fs->firstlocal + i);
      else
      { /* local variable */
        init_var(fs, var, i);
        if (vd->kind == RDKVAVAR) /* vararg parameter? */
          var->k = VVARGVAR;
      }
      return var->k;
    }
  }
  return -1; /* not found */
}

/*
** Mark block where variable at given level was defined
** (to emit close instructions later).
*/
static void markupval(FuncState* fs, int level)
{
  BlockCnt* bl = fs->bl;
  while (bl->nactvar > level)
    bl = bl->previous;
  bl->upval = 1;
  fs->needclose = 1;
}

/*
** Mark that current block has a to-be-closed variable.
*/
static void marktobeclosed(FuncState* fs)
{
  BlockCnt* bl = fs->bl;
  bl->upval = 1;
  bl->insidetbc = 1;
  fs->needclose = 1;
}

/*
** Find a variable with the given name 'n'. If it is an upvalue, add
** this upvalue into all intermediate functions. If it is a global, set
** 'var' as 'void' as a flag.
*/
static void singlevaraux(FuncState* fs, TString* n, expdesc* var, int base)
{
  int v = searchvar(fs, n, var); /* look up variables at current level */
  if (v >= 0)
  { /* found? */
    if (!base)
    {
      if (var->k == VVARGVAR)      /* vararg parameter? */
        luaK_vapar2local(fs, var); /* change it to a regular local */
      if (var->k == VLOCAL)
        markupval(fs, var->var.vidx); /* will be used as an upvalue */
    }
    /* else nothing else to be done */
  }
  else
  {                                 /* not found at current level; try upvalues */
    int idx = searchupvalue(fs, n); /* try existing upvalues */
    if (idx < 0)
    {                                           /* not found? */
      if (fs->prev != nullptr)                     /* more levels? */
        singlevaraux(fs->prev, n, var, 0);      /* try upper levels */
      if (var->k == VLOCAL || var->k == VUPVAL) /* local or upvalue? */
        idx = newupvalue(fs, n, var);           /* will be a new upvalue */
      else                                      /* it is a global or a constant */
        return;                                 /* don't need to do anything at this level */
    }
    init_exp(var, VUPVAL, idx); /* new or old upvalue */
  }
}

static void buildglobal(LexState* ls, TString* varname, expdesc* var)
{
  FuncState* fs = ls->fs;
  expdesc key;
  init_exp(var, VGLOBAL, -1);         /* global by default */
  singlevaraux(fs, ls->envn, var, 1); /* get environment variable */
  if (var->k == VGLOBAL)
    luaK_semerror(ls, lt::s_is_global_when_accessing_variable_s,
                  FmtArgs().str(lt::_ENV).str(varname->data.view()));
  luaK_exp2anyregup(fs, var);  /* _ENV could be a constant */
  codestring(&key, varname);   /* key is variable name */
  luaK_indexed(fs, var, &key); /* 'var' represents _ENV[varname] */
}

/*
** Find a variable with the given name 'n', handling global variables
** too.
*/
static void buildvar(LexState* ls, TString* varname, expdesc* var)
{
  FuncState* fs = ls->fs;
  init_exp(var, VGLOBAL, -1); /* global by default */
  singlevaraux(fs, varname, var, 1);
  if (var->k == VGLOBAL)
  { /* global name? */
    int info = var->info;
    /* global by default in the scope of a global declaration? */
    if (info == -2)
      luaK_semerror(ls, lt::variable_s_not_declared,
                    FmtArgs().str(varname->data.view()));
    buildglobal(ls, varname, var);
    if (info != -1 && ls->dyd->actvar.arr[info].kind == GDKCONST)
      var->ind.ro = 1; /* mark variable as read-only */
    else                 /* anyway must be a global */
      assert(info == -1 || ls->dyd->actvar.arr[info].kind == GDKREG);
  }
}

static void singlevar(LexState* ls, expdesc* var)
{
  buildvar(ls, str_checkname(ls), var);
}

/*
** Adjust the number of results from an expression list 'e' with 'nexps'
** expressions to 'nvars' values.
*/
static void adjust_assign(LexState* ls, int nvars, int nexps, expdesc* e)
{
  FuncState* fs = ls->fs;
  int needed = nvars - nexps; /* extra values needed */
  luaK_checkstack(fs, needed);
  if (hasmultret(e->k))
  {                         /* last expression has multiple returns? */
    int extra = needed + 1; /* discount last expression itself */
    if (extra < 0)
      extra = 0;
    luaK_setreturns(fs, e, extra); /* last exp. provides the difference */
  }
  else
  {
    if (e->k != VVOID)                   /* at least one expression? */
      luaK_exp2nextreg(fs, e);           /* close last expression */
    if (needed > 0)                      /* missing values? */
      luaK_nil(fs, fs->freereg, needed); /* complete with nils */
  }
  if (needed > 0)
    luaK_reserveregs(fs, needed);                  /* registers for extra values */
  else                                             /* adding 'needed' is actually a subtraction */
    fs->freereg = fs->freereg + needed; /* remove extra values */
}

static inline void enterlevel(LexState* ls)
{
  luaE_incCstack(ls->L);
}

static inline void leavelevel(LexState* ls)
{
  ls->L->nCcalls--;
}

/*
** Generates an error that a goto jumps into the scope of some
** variable declaration.
*/
[[noreturn]] static void jumpscopeerror(LexState* ls, Labeldesc* gt)
{
  TString* tsname = getlocalvardesc(ls->fs, gt->nactvar)->name;
  byte_view varname = (tsname != nullptr) ? tsname->data.view() : lt::star;
  luaK_semerror(ls, lt::goto_s_at_line_d_jumps_into_the_scope_of_s, FmtArgs().str(gt->name->data.view()).num(gt->line).str(varname)); /* raise the error */
}

/*
** Closes the goto at index 'g' to given 'label' and removes it
** from the list of pending gotos.
** If it jumps into the scope of some variable, raises an error.
** The goto needs a CLOSE if it jumps out of a block with upvalues,
** or out of the scope of some variable and the block has upvalues
** (signaled by parameter 'bup').
*/
static void closegoto(LexState* ls, int g, Labeldesc* label, int bup)
{
  int i;
  FuncState* fs = ls->fs;
  Labellist* gl = &ls->dyd->gt; /* list of gotos */
  Labeldesc* gt = &gl->arr[g];  /* goto to be resolved */
  assert(eqstr(gt->name, label->name));
  if (l_unlikely(gt->nactvar < label->nactvar)) /* enter some scope? */
    jumpscopeerror(ls, gt);
  if (gt->close ||
      (label->nactvar < gt->nactvar && bup))
  { /* needs close? */
    int stklevel = reglevel(fs, label->nactvar);
    /* move jump to CLOSE position */
    fs->f->code[gt->pc + 1] = fs->f->code[gt->pc];
    /* put CLOSE instruction at original position */
    fs->f->code[gt->pc] = CREATE_ABCk(OP_CLOSE, stklevel, 0, 0, 0);
    gt->pc++; /* must point to jump instruction */
  }
  luaK_patchlist(ls->fs, gt->pc, label->pc); /* goto jumps to label */
  for (i = g; i < gl->n - 1; i++)            /* remove goto from pending list */
    gl->arr[i] = gl->arr[i + 1];
  gl->n--;
}

/*
** Search for an active label with the given name, starting at
** index 'ilb' (so that it can search for all labels in current block
** or all labels in current function).
*/
static Labeldesc* findlabel(LexState* ls, TString* name, int ilb)
{
  Dyndata* dyd = ls->dyd;
  for (; ilb < dyd->label.n; ilb++)
  {
    Labeldesc* lb = &dyd->label.arr[ilb];
    if (eqstr(lb->name, name)) /* correct label? */
      return lb;
  }
  return nullptr; /* label not found */
}

/*
** Adds a new label/goto in the corresponding list.
*/
static int newlabelentry(LexState* ls, Labellist* l, TString* name,
                         int line, int pc)
{
  int n = l->n;
  {
    auto s = luaM_safegrow_Labeldesc_span(ls->L, std::span<Labeldesc>(l->arr, l->size), n);
    l->arr = s.data();
    l->size = (int)s.size();
  }
  l->arr[n].name = name;
  l->arr[n].line = line;
  l->arr[n].nactvar = ls->fs->nactvar;
  l->arr[n].close = 0;
  l->arr[n].pc = pc;
  l->n = n + 1;
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
static int newgotoentry(LexState* ls, TString* name, int line)
{
  FuncState* fs = ls->fs;
  int pc = luaK_jump(fs);              /* create jump */
  luaK_codeABC(fs, OP_CLOSE, 0, 1, 0); /* spaceholder, marked as dead */
  return newlabelentry(ls, &ls->dyd->gt, name, line, pc);
}

/*
** Create a new label with the given 'name' at the given 'line'.
** 'last' tells whether label is the last non-op statement in its
** block. Solves all pending gotos to this new label and adds
** a close instruction if necessary.
** Returns true iff it added a close instruction.
*/
static void createlabel(LexState* ls, TString* name, int line, int last)
{
  FuncState* fs = ls->fs;
  Labellist* ll = &ls->dyd->label;
  int l = newlabelentry(ls, ll, name, line, luaK_getlabel(fs));
  if (last)
  { /* label is last no-op statement in the block? */
    /* assume that locals are already out of scope */
    ll->arr[l].nactvar = fs->bl->nactvar;
  }
}

/*
** Traverse the pending gotos of the finishing block checking whether
** each match some label of that block. Those that do not match are
** "exported" to the outer block, to be solved there. In particular,
** its 'nactvar' is updated with the level of the inner block,
** as the variables of the inner block are now out of scope.
*/
static void solvegotos(FuncState* fs, BlockCnt* bl)
{
  LexState* ls = fs->ls;
  Labellist* gl = &ls->dyd->gt;
  int outlevel = reglevel(fs, bl->nactvar); /* level outside the block */
  int igt = bl->firstgoto;                  /* first goto in the finishing block */
  while (igt < gl->n)
  { /* for each pending goto */
    Labeldesc* gt = &gl->arr[igt];
    /* search for a matching label in the current block */
    Labeldesc* lb = findlabel(ls, gt->name, bl->firstlabel);
    if (lb != nullptr)                      /* found a match? */
      closegoto(ls, igt, lb, bl->upval); /* close and remove goto */
    else
    { /* adjust 'goto' for outer block */
      /* block has variables to be closed and goto escapes the scope of
         some variable? */
      if (bl->upval && reglevel(fs, gt->nactvar) > outlevel)
        gt->close = 1;           /* jump may need a close */
      gt->nactvar = bl->nactvar; /* correct level for outer block */
      igt++;                     /* go to next goto */
    }
  }
  ls->dyd->label.n = bl->firstlabel; /* remove local labels */
}

static void enterblock(FuncState* fs, BlockCnt* bl, lu_byte isloop)
{
  bl->isloop = isloop;
  bl->nactvar = fs->nactvar;
  bl->firstlabel = fs->ls->dyd->label.n;
  bl->firstgoto = fs->ls->dyd->gt.n;
  bl->upval = 0;
  /* inherit 'insidetbc' from enclosing block */
  bl->insidetbc = (fs->bl != nullptr && fs->bl->insidetbc);
  bl->previous = fs->bl; /* link block in function's block list */
  fs->bl = bl;
  assert(fs->freereg == luaY_nvarstack(fs));
}

/*
** generates an error for an undefined 'goto'.
*/
[[noreturn]] static void undefgoto(LexState* ls, Labeldesc* gt)
{
  /* breaks are checked when created, cannot be undefined */
  assert(!eqstr(gt->name, ls->brkn));
  luaK_semerror(ls, lt::no_visible_label_s_for_goto_at_line_d,
                FmtArgs().str(gt->name->data.view()).num(gt->line));
}

static void leaveblock(FuncState* fs)
{
  BlockCnt* bl = fs->bl;
  LexState* ls = fs->ls;
  int stklevel = reglevel(fs, bl->nactvar); /* level outside block */
  if (bl->previous && bl->upval)                /* need a 'close'? */
    luaK_codeABC(fs, OP_CLOSE, stklevel, 0, 0);
  fs->freereg = stklevel;                 /* free registers */
  removevars(fs, bl->nactvar);            /* remove block locals */
  assert(bl->nactvar == fs->nactvar); /* back to level on entry */
  if (bl->isloop == 2)                    /* has to fix pending breaks? */
    createlabel(ls, ls->brkn, 0, 0);
  solvegotos(fs, bl);
  if (bl->previous == nullptr)
  {                                                   /* was it the last block? */
    if (bl->firstgoto < ls->dyd->gt.n)                /* still pending gotos? */
      undefgoto(ls, &ls->dyd->gt.arr[bl->firstgoto]); /* error */
  }
  fs->bl = bl->previous; /* current block now is previous one */
}

/*
** adds a new prototype into list of prototypes
*/
static Proto* addprototype(LexState* ls)
{
  Proto* clp;
  lua_State* L = ls->L;
  FuncState* fs = ls->fs;
  Proto* f = fs->f; /* prototype of current function */
  if (fs->np >= (int)f->p.size())
  {
    int oldsize = (int)f->p.size();
    f->p = luaM_safegrow_Proto_ptr_span(L, f->p, fs->np);
    while (oldsize < (int)f->p.size())
      f->p[oldsize++] = nullptr;
  }
  f->p[fs->np++] = clp = Proto::luaF_newproto(L);
  luaC_objbarrier(L, f, clp);
  return clp;
}

/*
** codes instruction to create new closure in parent function.
** The OP_CLOSURE instruction uses the last available register,
** so that, if it invokes the GC, the GC knows which registers
** are in use at that time.

*/
static void codeclosure(LexState* ls, expdesc* v)
{
  FuncState* fs = ls->fs->prev;
  init_exp(v, VRELOC, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np - 1));
  luaK_exp2nextreg(fs, v); /* fix it at the last register */
}

static void open_func(LexState* ls, FuncState* fs, BlockCnt* bl)
{
  lua_State* L = ls->L;
  Proto* f = fs->f;
  fs->prev = ls->fs; /* linked list of funcstates */
  fs->ls = ls;
  ls->fs = fs;
  fs->pc = 0;
  fs->previousline = f->linedefined;
  fs->iwthabs = 0;
  fs->lasttarget = 0;
  fs->freereg = 0;
  fs->nk = 0;
  fs->nabslineinfo = 0;
  fs->np = 0;
  fs->nups = 0;
  fs->ndebugvars = 0;
  fs->nactvar = 0;
  fs->needclose = 0;
  fs->firstlocal = ls->dyd->actvar.n;
  fs->firstlabel = ls->dyd->label.n;
  fs->bl = nullptr;
  f->source = ls->source;
  luaC_objbarrier(L, f, f->source);
  f->maxstacksize = 2;                           /* registers 0/1 are always valid */
  fs->kcache = Table::luaH_new(L);                      /* create table for function */
  luaC_givebackhash(L->l_G, fs->kcache);                /* (the parser's own) */
  assert(novariant(fs->kcache->tt) >= LUA_TSTRING);
  luaD_anchorobj(L, ls->h, fs->kcache); /* anchor it */
  enterblock(fs, bl, 0);
}

static void close_func(LexState* ls)
{
  lua_State* L = ls->L;
  FuncState* fs = ls->fs;
  Proto* f = fs->f;
  TValue temp;
  luaK_ret(fs, luaY_nvarstack(fs), 0); /* final return */
  leaveblock(fs);
  assert(fs->bl == nullptr);
  luaK_finish(fs);
  f->code = luaM_alloc_Instruction_span(L->l_G, L, f->code, fs->pc, true);
  f->lineinfo = luaM_alloc_ls_byte_span(L->l_G, L, f->lineinfo, fs->pc, true);
  f->abslineinfo = luaM_alloc_AbsLineInfo_span(L->l_G, L, f->abslineinfo, fs->nabslineinfo, true);
  f->k = luaM_alloc_TValue_span(L->l_G, L, f->k, fs->nk, true);
  f->p = luaM_alloc_Proto_ptr_span(L->l_G, L, f->p, fs->np, true);
  f->locvars = luaM_alloc_LocVar_span(L->l_G, L, f->locvars, fs->ndebugvars, true);
  f->upvalues = luaM_alloc_Upvaldesc_span(L->l_G, L, f->upvalues, fs->nups, true);
  /* remove kcache table from scanner table ("weigh" its anchor) */
  sethvalue(L, &temp, fs->kcache); /* key to be set to nil */
  ls->h->luaH_set(L, &temp, &L->l_G->nilvalue);
  ls->fs = fs->prev;
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
static int block_follow(LexState* ls, int withuntil)
{
  switch (ls->t.token)
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

static void statlist(LexState* ls)
{
  /* statlist -> { stat [';'] } */
  constexpr int FN = LF_STATLIST;
  NoLocals l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  while (!block_follow(ls, 1))
  {
    if (ls->t.token == TK_RETURN)
    {
      LZ_SITE(1, statement(ls));
      return; /* 'return' must be last statement */
    }
    LZ_SITE(2, statement(ls));
  }
}

static void fieldsel(LexState* ls, expdesc* v)
{
  /* fieldsel -> ['.' | ':'] NAME */
  constexpr int FN = LF_FIELDSEL;
  struct
  {
    expdesc key;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  luaK_exp2anyregup(fs, v);
  LZ_SITE(1, luaX_next(ls)); /* skip the dot or colon */
  LZ_SITE(2, codename(ls, &l.key));
  luaK_indexed(fs, v, &l.key);
}

static void yindex(LexState* ls, expdesc* v)
{
  /* index -> '[' expr ']' */
  constexpr int FN = LF_YINDEX;
  NoLocals l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, luaX_next(ls)); /* skip the '[' */
  LZ_SITE(2, expr(ls, v));
  luaK_exp2val(ls->fs, v);
  LZ_SITE(3, checknext(ls, ']'));
}

/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/

struct ConsControl
{
  expdesc v;      /* last list item read */
  expdesc* t;     /* table descriptor */
  int nh;         /* total number of 'record' elements */
  int na;         /* number of array elements already stored */
  int tostore;    /* number of array elements pending to be stored */
  int maxtostore; /* maximum number of pending elements */
};

/*
** Maximum number of elements in a constructor, to control the following:
** * counter overflows;
** * overflows in 'extra' for OP_NEWTABLE and OP_SETLIST;
** * overflows when adding multiple returns in OP_SETLIST.
*/
constexpr int MAX_CNST = ((INT_MAX / 2) / (MAXARG_vC + 1) > MAXARG_Ax)
                             ? MAXARG_Ax * (MAXARG_vC + 1)
                             : INT_MAX / 2;

static void recfield(LexState* ls, ConsControl* cc)
{
  /* recfield -> (NAME | '['exp']') = exp */
  constexpr int FN = LF_RECFIELD;
  struct
  {
    int reg;
    expdesc tab, key, val;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    default: LZ_BADSITE();
    }
  }
  l.reg = ls->fs->freereg;
  if (ls->t.token == TK_NAME)
  {
    LZ_SITE(1, codename(ls, &l.key));
  }
  else /* ls->t.token == '[' */
  {
    LZ_SITE(2, yindex(ls, &l.key));
  }
  cc->nh++;
  LZ_SITE(3, checknext(ls, '='));
  l.tab = *cc->t;
  luaK_indexed(fs, &l.tab, &l.key);
  LZ_SITE(4, expr(ls, &l.val));
  luaK_storevar(fs, &l.tab, &l.val);
  fs->freereg = l.reg; /* free registers */
}

static void closelistfield(FuncState* fs, ConsControl* cc)
{
  assert(cc->tostore > 0);
  luaK_exp2nextreg(fs, &cc->v);
  cc->v.k = VVOID;
  if (cc->tostore >= cc->maxtostore)
  {
    luaK_setlist(fs, cc->t->info, cc->na, cc->tostore); /* flush */
    cc->na += cc->tostore;
    cc->tostore = 0; /* no more items pending */
  }
}

static void lastlistfield(FuncState* fs, ConsControl* cc)
{
  if (cc->tostore == 0)
    return;
  if (hasmultret(cc->v.k))
  {
    luaK_setmultret(fs, &cc->v);
    luaK_setlist(fs, cc->t->info, cc->na, LUA_MULTRET);
    cc->na--; /* do not count last expression (unknown number of elements) */
  }
  else
  {
    if (cc->v.k != VVOID)
      luaK_exp2nextreg(fs, &cc->v);
    luaK_setlist(fs, cc->t->info, cc->na, cc->tostore);
  }
  cc->na += cc->tostore;
}

static void listfield(LexState* ls, ConsControl* cc)
{
  /* listfield -> exp */
  expr(ls, &cc->v);
  cc->tostore++;
}

static void field(LexState* ls, ConsControl* cc)
{
  /* field -> listfield | recfield */
  constexpr int FN = LF_FIELD;
  struct
  {
    int r;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  switch (ls->t.token)
  {
  case TK_NAME:
  { /* may be 'listfield' or 'recfield' */
    LZ_SITE(1, l.r = luaX_lookahead(ls));
    if (l.r != '=') /* expression? */
    {
      LZ_SITE(2, listfield(ls, cc));
    }
    else
    {
      LZ_SITE(3, recfield(ls, cc));
    }
    break;
  }
  case '[':
  {
    LZ_SITE(4, recfield(ls, cc));
    break;
  }
  default:
  {
    LZ_SITE(5, listfield(ls, cc));
    break;
  }
  }
}

/*
** Compute a limit for how many registers a constructor can use before
** emitting a 'SETLIST' instruction, based on how many registers are
** available.
*/
static int maxtostore(FuncState* fs)
{
  int numfreeregs = MAX_FSTACK - fs->freereg;
  if (numfreeregs >= 160)     /* "lots" of registers? */
    return numfreeregs / 5;   /* use up to 1/5 of them */
  else if (numfreeregs >= 80) /* still "enough" registers? */
    return 10;                /* one 'SETLIST' instruction for each 10 values */
  else                        /* save registers for potential more nesting */
    return 1;
}

static void constructor(LexState* ls, expdesc* t)
{
  /* constructor -> '{' [ field { sep field } [sep] ] '}'
     sep -> ',' | ';' */
  constexpr int FN = LF_CONSTRUCTOR;
  struct
  {
    int line;
    int pc;
    int r;
    ConsControl cc;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    int site = LZ_SITENO();
    l.cc.t = t;
    switch (site)
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  l.line = ls->linenumber;
  l.pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
  luaK_code(fs, 0); /* space for extra arg. */
  l.cc.na = l.cc.nh = l.cc.tostore = 0;
  l.cc.t = t;
  init_exp(t, VNONRELOC, fs->freereg); /* table will be at stack top */
  luaK_reserveregs(fs, 1);
  init_exp(&l.cc.v, VVOID, 0); /* no value (yet) */
  LZ_SITE(1, checknext(ls, '{' /*}*/));
  l.cc.maxtostore = maxtostore(fs);
  for (;;)
  {
    if (ls->t.token == /*{*/ '}')
      break;
    if (l.cc.v.k != VVOID)       /* is there a previous list item? */
      closelistfield(fs, &l.cc); /* close it */
    LZ_SITE(2, field(ls, &l.cc));
    luaY_checklimit(fs, l.cc.tostore + l.cc.na + l.cc.nh, MAX_CNST, lt::items_in_a_constructor);
    LZ_SITE(3, l.r = testnext(ls, ','));
    if (!l.r)
    {
      LZ_SITE(4, l.r = testnext(ls, ';'));
      if (!l.r)
        break;
    }
  }
  LZ_SITE(5, check_match(ls, /*{*/ '}', '{' /*}*/, l.line));
  lastlistfield(fs, &l.cc);
  luaK_settablesize(fs, l.pc, t->info, l.cc.na, l.cc.nh);
}

/* }====================================================================== */

static void setvararg(FuncState* fs)
{
  fs->f->flag |= PF_VAHID; /* by default, use hidden vararg arguments */
  luaK_codeABC(fs, OP_VARARGPREP, 0, 0, 0);
}

static void parlist(LexState* ls)
{
  /* parlist -> [ {NAME ','} (NAME | '...') ] */
  constexpr int FN = LF_PARLIST;
  struct
  {
    int nparams;
    int varargk;
    int r;
    TString* name;
  } l;
  FuncState* fs = ls->fs;
  Proto* f = fs->f;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    default: LZ_BADSITE();
    }
  }
  l.nparams = 0;
  l.varargk = 0;
  if (ls->t.token != ')')
  { /* is 'parlist' not empty? */
    for (;;)
    {
      switch (ls->t.token)
      {
      case TK_NAME:
      {
        LZ_SITE(1, l.name = str_checkname(ls));
        new_localvar(ls, l.name);
        l.nparams++;
        break;
      }
      case TK_DOTS:
      {
        l.varargk = 1;
        LZ_SITE(2, luaX_next(ls)); /* skip '...' */
        if (ls->t.token == TK_NAME)
        {
          LZ_SITE(3, l.name = str_checkname(ls));
          new_varkind(ls, l.name, RDKVAVAR);
        }
        else
        {
          byte_view name = lt::vararg_table;
          new_localvar(ls, luaX_newstring(ls, name, 0, name.size()));
        }
        break;
      }
      default:
        luaX_syntaxerror(ls, lt::name_or_expected);
      }
      if (l.varargk)
        break;
      LZ_SITE(4, l.r = testnext(ls, ','));
      if (!l.r)
        break;
    }
  }
  adjustlocalvars(ls, l.nparams);
  f->numparams = fs->nactvar;
  if (l.varargk)
  {
    setvararg(fs);          /* declared vararg */
    adjustlocalvars(ls, 1); /* vararg parameter */
  }
  /* reserve registers for parameters (plus vararg parameter, if present) */
  luaK_reserveregs(fs, fs->nactvar);
}

static void body(LexState* ls, expdesc* e, int ismethod, int line)
{
  /* body ->  '(' parlist ')' block END */
  constexpr int FN = LF_BODY;
  struct
  {
    FuncState new_fs;
    BlockCnt bl;
  } l;
  if (LZ_RESUMING())
  { /* (its function and block again in the chains) */
    int site = LZ_SITENO();
    l.new_fs.prev = ls->fs;
    l.new_fs.ls = ls;
    ls->fs = &l.new_fs;
    l.bl.previous = nullptr;
    l.new_fs.bl = &l.bl;
    switch (site)
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  l.new_fs.f = addprototype(ls);
  l.new_fs.f->linedefined = line;
  open_func(ls, &l.new_fs, &l.bl);
  LZ_SITE(1, checknext(ls, '('));
  if (ismethod)
  {
    byte_view self = lt::self;
    new_localvar(ls, luaX_newstring(ls, self, 0, self.size())); /* create 'self' parameter */
    adjustlocalvars(ls, 1);
  }
  LZ_SITE(2, parlist(ls));
  LZ_SITE(3, checknext(ls, ')'));
  LZ_SITE(4, statlist(ls));
  l.new_fs.f->lastlinedefined = ls->linenumber;
  LZ_SITE(5, check_match(ls, TK_END, TK_FUNCTION, line));
  codeclosure(ls, e);
  close_func(ls);
}

static int explist(LexState* ls, expdesc* v)
{
  /* explist -> expr { ',' expr } */
  constexpr int FN = LF_EXPLIST;
  struct
  {
    int n;
    int r;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    default: LZ_BADSITE();
    }
  }
  l.n = 1; /* at least one expression */
  LZ_SITE(1, expr(ls, v));
  for (;;)
  {
    LZ_SITE(2, l.r = testnext(ls, ','));
    if (!l.r)
      break;
    luaK_exp2nextreg(ls->fs, v);
    LZ_SITE(3, expr(ls, v));
    l.n++;
  }
  return l.n;
}

static void funcargs(LexState* ls, expdesc* f)
{
  constexpr int FN = LF_FUNCARGS;
  struct
  {
    expdesc args;
    int line;
  } l;
  FuncState* fs = ls->fs;
  int base, nparams;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  l.line = ls->linenumber;
  switch (ls->t.token)
  {
  case '(':
  { /* funcargs -> '(' [ explist ] ')' */
    LZ_SITE(1, luaX_next(ls));
    if (ls->t.token == ')') /* arg list is empty? */
      l.args.k = VVOID;
    else
    {
      LZ_SITE(2, explist(ls, &l.args));
      if (hasmultret(l.args.k))
        luaK_setmultret(fs, &l.args);
    }
    LZ_SITE(3, check_match(ls, ')', '(', l.line));
    break;
  }
  case '{' /*}*/:
  { /* funcargs -> constructor */
    LZ_SITE(4, constructor(ls, &l.args));
    break;
  }
  case TK_STRING:
  { /* funcargs -> STRING */
    codestring(&l.args, ls->t.ts);
    LZ_SITE(5, luaX_next(ls)); /* must use 'seminfo' before 'next' */
    break;
  }
  default:
  {
    luaX_syntaxerror(ls, lt::function_arguments_expected);
    return; /* to avoid warnings */
  }
  }
  assert(f->k == VNONRELOC);
  base = f->info; /* base register for call */
  if (hasmultret(l.args.k))
    nparams = LUA_MULTRET; /* open call */
  else
  {
    if (l.args.k != VVOID)
      luaK_exp2nextreg(fs, &l.args); /* close last argument */
    nparams = fs->freereg - (base + 1);
  }
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams + 1, 2));
  luaK_fixline(fs, l.line);
  /* call removes function and arguments and leaves one result (unless
     changed later) */
  fs->freereg = base + 1;
}

/*
** {======================================================================
** Expression parsing
** =======================================================================
*/

static void primaryexp(LexState* ls, expdesc* v)
{
  /* primaryexp -> NAME | '(' expr ')' */
  constexpr int FN = LF_PRIMARYEXP;
  struct
  {
    int line;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    default: LZ_BADSITE();
    }
  }
  switch (ls->t.token)
  {
  case '(':
  {
    l.line = ls->linenumber;
    LZ_SITE(1, luaX_next(ls));
    LZ_SITE(2, expr(ls, v));
    LZ_SITE(3, check_match(ls, ')', '(', l.line));
    luaK_dischargevars(ls->fs, v);
    return;
  }
  case TK_NAME:
  {
    LZ_SITE(4, singlevar(ls, v));
    return;
  }
  default:
  {
    luaX_syntaxerror(ls, lt::unexpected_symbol);
  }
  }
}

static void suffixedexp(LexState* ls, expdesc* v)
{
  /* suffixedexp ->
       primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs } */
  constexpr int FN = LF_SUFFIXEDEXP;
  struct
  {
    expdesc key;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    case 6: goto site_6;
    case 7: goto site_7;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, primaryexp(ls, v));
  for (;;)
  {
    switch (ls->t.token)
    {
    case '.':
    { /* fieldsel */
      LZ_SITE(2, fieldsel(ls, v));
      break;
    }
    case '[':
    { /* '[' exp ']' */
      luaK_exp2anyregup(fs, v);
      LZ_SITE(3, yindex(ls, &l.key));
      luaK_indexed(fs, v, &l.key);
      break;
    }
    case ':':
    { /* ':' NAME funcargs */
      LZ_SITE(4, luaX_next(ls));
      LZ_SITE(5, codename(ls, &l.key));
      luaK_self(fs, v, &l.key);
      LZ_SITE(6, funcargs(ls, v));
      break;
    }
    case '(':
    case TK_STRING:
    case '{' /*}*/:
    { /* funcargs */
      luaK_exp2nextreg(fs, v);
      LZ_SITE(7, funcargs(ls, v));
      break;
    }
    default:
      return;
    }
  }
}

static void simpleexp(LexState* ls, expdesc* v)
{
  /* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
                  constructor | FUNCTION body | suffixedexp */
  constexpr int FN = LF_SIMPLEEXP;
  struct
  {
    int line;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  switch (ls->t.token)
  {
  case TK_FLT:
  {
    init_exp(v, VKFLT, 0);
    v->nval = ls->t.r;
    break;
  }
  case TK_INT:
  {
    init_exp(v, VKINT, 0);
    v->ival = ls->t.i;
    break;
  }
  case TK_STRING:
  {
    codestring(v, ls->t.ts);
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
    FuncState* fs = ls->fs;
    check_condition(ls, isvararg(fs->f), lt::cannot_use_outside_a_vararg_function);
    init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, fs->f->numparams, 1));
    break;
  }
  case '{' /*}*/:
  { /* constructor */
    LZ_SITE(1, constructor(ls, v));
    return;
  }
  case TK_FUNCTION:
  {
    LZ_SITE(2, luaX_next(ls));
    l.line = ls->linenumber;
    LZ_SITE(3, body(ls, v, 0, l.line));
    return;
  }
  default:
  {
    LZ_SITE(4, suffixedexp(ls, v));
    return;
  }
  }
  LZ_SITE(5, luaX_next(ls));
}

static UnOpr getunopr(int op)
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

static BinOpr getbinopr(int op)
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
static const struct
{
  lu_byte left;  /* left priority for each binary operator */
  lu_byte right; /* right priority */
} priority[] = {
    /* ORDER OPR */
    {10, 10},
    {10, 10}, /* '+' '-' */
    {11, 11},
    {11, 11}, /* '*' '%' */
    {14, 13}, /* '^' (right associative) */
    {11, 11},
    {11, 11}, /* '/' '//' */
    {6, 6},
    {4, 4},
    {5, 5}, /* '&' '|' '~' */
    {7, 7},
    {7, 7}, /* '<<' '>>' */
    {9, 8}, /* '..' (right associative) */
    {3, 3},
    {3, 3},
    {3, 3}, /* ==, <, <= */
    {3, 3},
    {3, 3},
    {3, 3}, /* ~=, >, >= */
    {2, 2},
    {1, 1} /* and, or */
};

constexpr int UNARY_PRIORITY = 12; /* priority for unary operators */

/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where 'binop' is any binary operator with a priority higher than 'limit'
*/
static BinOpr subexpr(LexState* ls, expdesc* v, int limit)
{
  constexpr int FN = LF_SUBEXPR;
  struct
  {
    BinOpr op;
    UnOpr uop;
    int line;
    BinOpr nextop;
    expdesc v2;
  } l;
  enterlevel(ls);
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  l.uop = getunopr(ls->t.token);
  if (l.uop != OPR_NOUNOPR)
  { /* prefix (unary) operator? */
    l.line = ls->linenumber;
    LZ_SITE(1, luaX_next(ls)); /* skip operator */
    LZ_SITE(2, subexpr(ls, v, UNARY_PRIORITY));
    luaK_prefix(ls->fs, l.uop, v, l.line);
  }
  else
  {
    LZ_SITE(3, simpleexp(ls, v));
  }
  /* expand while operators have priorities higher than 'limit' */
  l.op = getbinopr(ls->t.token);
  while (l.op != OPR_NOBINOPR && priority[l.op].left > limit)
  {
    l.line = ls->linenumber;
    LZ_SITE(4, luaX_next(ls)); /* skip operator */
    luaK_infix(ls->fs, l.op, v);
    /* read sub-expression with higher priority */
    LZ_SITE(5, l.nextop = subexpr(ls, &l.v2, priority[l.op].right));
    luaK_posfix(ls->fs, l.op, v, &l.v2, l.line);
    l.op = l.nextop;
  }
  leavelevel(ls);
  return l.op; /* return first untreated operator */
}

static void expr(LexState* ls, expdesc* v)
{
  subexpr(ls, v, 0); /* (no frame of its own: see 'testnext') */
}

/* }==================================================================== */

/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/

static void block(LexState* ls)
{
  /* block -> statlist */
  constexpr int FN = LF_BLOCK;
  struct
  {
    BlockCnt bl;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    l.bl.previous = fs->bl; /* (again in the chain) */
    fs->bl = &l.bl;
    goto site_1;
  }
  enterblock(fs, &l.bl, 0);
  LZ_SITE(1, statlist(ls));
  leaveblock(fs);
}

/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
struct LHS_assign
{
  struct LHS_assign* prev;
  expdesc v; /* variable (global, local, upvalue, or indexed) */
};

/*
** check whether, in an assignment to an upvalue/local variable, the
** upvalue/local variable is begin used in a previous assignment to a
** table. If so, save original upvalue/local value in a safe place and
** use this safe copy in the previous assignment.
*/
static void check_conflict(LexState* ls, struct LHS_assign* lh, expdesc* v)
{
  FuncState* fs = ls->fs;
  int extra = fs->freereg; /* eventual position to save local variable */
  int conflict = 0;
  for (; lh; lh = lh->prev)
  { /* check all previous assignments */
    if (vkisindexed(lh->v.k))
    { /* assignment to table field? */
      if (lh->v.k == VINDEXUP)
      { /* is table an upvalue? */
        if (v->k == VUPVAL && lh->v.ind.t == v->info)
        {
          conflict = 1; /* table is the upvalue being assigned now */
          lh->v.k = VINDEXSTR;
          lh->v.ind.t = extra; /* assignment will use safe copy */
        }
      }
      else
      { /* table is a register */
        if (v->k == VLOCAL && lh->v.ind.t == v->var.ridx)
        {
          conflict = 1;          /* table is the local being assigned now */
          lh->v.ind.t = extra; /* assignment will use safe copy */
        }
        /* is index the local being assigned? */
        if (lh->v.k == VINDEXED && v->k == VLOCAL &&
            lh->v.ind.idx == v->var.ridx)
        {
          conflict = 1;
          lh->v.ind.idx = extra; /* previous assignment will use safe copy */
        }
      }
    }
  }
  if (conflict)
  {
    /* copy upvalue/local value to a temporary (in position 'extra') */
    if (v->k == VLOCAL)
      luaK_codeABC(fs, OP_MOVE, extra, v->var.ridx, 0);
    else
      luaK_codeABC(fs, OP_GETUPVAL, extra, v->info, 0);
    luaK_reserveregs(fs, 1);
  }
}

/* Create code to store the "top" register in 'var' */
static void storevartop(FuncState* fs, expdesc* var)
{
  expdesc e;
  init_exp(&e, VNONRELOC, fs->freereg - 1);
  luaK_storevar(fs, var, &e); /* will also free the top register */
}

/*
** Parse and compile a multiple assignment. The first "variable"
** (a 'suffixedexp') was already read by the caller.
**
** assignment -> suffixedexp restassign
** restassign -> ',' suffixedexp restassign | '=' explist
*/
static void restassign(LexState* ls, struct LHS_assign* lh, int nvars)
{
  constexpr int FN = LF_RESTASSIGN;
  struct
  {
    int nexps;
    int r;
    expdesc e;
    struct LHS_assign nv;
  } l;
  if (LZ_RESUMING())
  {
    int site = LZ_SITENO();
    l.nv.prev = lh; /* (again in the chain) */
    switch (site)
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  check_condition(ls, vkisvar(lh->v.k), lt::syntax_error);
  check_readonly(ls, &lh->v);
  LZ_SITE(1, l.r = testnext(ls, ','));
  if (l.r)
  { /* restassign -> ',' suffixedexp restassign */
    l.nv.prev = lh;
    LZ_SITE(2, suffixedexp(ls, &l.nv.v));
    if (!vkisindexed(l.nv.v.k))
      check_conflict(ls, lh, &l.nv.v);
    LZ_SITE(3, enterlevel(ls); /* control recursion depth */
               restassign(ls, &l.nv, nvars + 1));
    leavelevel(ls);
  }
  else
  { /* restassign -> '=' explist */
    LZ_SITE(4, checknext(ls, '='));
    LZ_SITE(5, l.nexps = explist(ls, &l.e));
    if (l.nexps != nvars)
      adjust_assign(ls, nvars, l.nexps, &l.e);
    else
    {
      luaK_setoneret(ls->fs, &l.e); /* close last expression */
      luaK_storevar(ls->fs, &lh->v, &l.e);
      return; /* avoid default */
    }
  }
  storevartop(ls->fs, &lh->v); /* default assignment */
}

static int cond(LexState* ls)
{
  /* cond -> exp */
  constexpr int FN = LF_COND;
  struct
  {
    expdesc v;
  } l;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  LZ_SITE(1, expr(ls, &l.v)); /* read condition */
  if (l.v.k == VNIL)
    l.v.k = VFALSE; /* 'falses' are all equal here */
  luaK_goiftrue(ls->fs, &l.v);
  return l.v.f;
}

static void gotostat(LexState* ls, int line)
{
  TString* name = str_checkname(ls); /* label's name */
  newgotoentry(ls, name, line);
}

/*
** Break statement. Semantically equivalent to "goto break".
*/
static void breakstat(LexState* ls, int line)
{
  constexpr int FN = LF_BREAKSTAT;
  NoLocals l;
  BlockCnt* bl; /* to look for an enclosing loop */
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  for (bl = ls->fs->bl; bl != nullptr; bl = bl->previous)
  {
    if (bl->isloop) /* found one? */
      break;
  }
  if (bl == nullptr)
    luaX_syntaxerror(ls, lt::break_outside_loop);
  bl->isloop = 2; /* signal that block has pending breaks */
  LZ_SITE(1, luaX_next(ls)); /* skip break */
  newgotoentry(ls, ls->brkn, line);
}

/*
** Check whether there is already a label with the given 'name' at
** current function.
*/
static void checkrepeated(LexState* ls, TString* name)
{
  Labeldesc* lb = findlabel(ls, name, ls->fs->firstlabel);
  if (l_unlikely(lb != nullptr)) /* already defined? */
    luaK_semerror(ls, lt::label_s_already_defined_on_line_d,
                  FmtArgs().str(name->data.view()).num(lb->line)); /* error */
}

static void labelstat(LexState* ls, TString* name, int line)
{
  /* label -> '::' NAME '::' */
  constexpr int FN = LF_LABELSTAT;
  NoLocals l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, checknext(ls, TK_DBCOLON)); /* skip double colon */
  while (ls->t.token == ';' || ls->t.token == TK_DBCOLON)
  {
    LZ_SITE(2, statement(ls)); /* skip other no-op statements */
  }
  checkrepeated(ls, name); /* check for repeated labels */
  createlabel(ls, name, line, block_follow(ls, 0));
}

static void whilestat(LexState* ls, int line)
{
  /* whilestat -> WHILE cond DO block END */
  constexpr int FN = LF_WHILESTAT;
  struct
  {
    int whileinit;
    int condexit;
    BlockCnt bl;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    int site = LZ_SITENO();
    if (site >= 3)
    { /* (its block again in the chain) */
      l.bl.previous = fs->bl;
      fs->bl = &l.bl;
    }
    switch (site)
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, luaX_next(ls)); /* skip WHILE */
  l.whileinit = luaK_getlabel(fs);
  LZ_SITE(2, l.condexit = cond(ls));
  enterblock(fs, &l.bl, 1);
  LZ_SITE(3, checknext(ls, TK_DO));
  LZ_SITE(4, block(ls));
  luaK_jumpto(fs, l.whileinit);
  LZ_SITE(5, check_match(ls, TK_END, TK_WHILE, line));
  leaveblock(fs);
  luaK_patchtohere(fs, l.condexit); /* false conditions finish the loop */
}

static void repeatstat(LexState* ls, int line)
{
  /* repeatstat -> REPEAT block UNTIL cond */
  constexpr int FN = LF_REPEATSTAT;
  struct
  {
    int condexit;
    int repeat_init;
    BlockCnt bl1, bl2;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    int site = LZ_SITENO();
    l.bl1.previous = fs->bl; /* (its blocks again in the chain) */
    l.bl2.previous = &l.bl1;
    fs->bl = &l.bl2;
    switch (site)
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    default: LZ_BADSITE();
    }
  }
  l.repeat_init = luaK_getlabel(fs);
  enterblock(fs, &l.bl1, 1); /* loop block */
  enterblock(fs, &l.bl2, 0); /* scope block */
  LZ_SITE(1, luaX_next(ls)); /* skip REPEAT */
  LZ_SITE(2, statlist(ls));
  LZ_SITE(3, check_match(ls, TK_UNTIL, TK_REPEAT, line));
  LZ_SITE(4, l.condexit = cond(ls)); /* read condition (inside scope block) */
  leaveblock(fs);                    /* finish scope */
  if (l.bl2.upval)
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
static void exp1(LexState* ls)
{
  constexpr int FN = LF_EXP1;
  struct
  {
    expdesc e;
  } l;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  LZ_SITE(1, expr(ls, &l.e));
  luaK_exp2nextreg(ls->fs, &l.e);
  assert(l.e.k == VNONRELOC);
}

/*
** Fix for instruction at position 'pc' to jump to 'dest'.
** (Jump addresses are relative in Lua). 'back' true means
** a back jump.
*/
static void fixforjump(FuncState* fs, int pc, int dest, int back)
{
  std::span<Instruction> code = fs->f->code;
  int offset = dest - (pc + 1);
  if (back)
    offset = -offset;
  if (l_unlikely(offset > MAXARG_Bx))
    luaX_syntaxerror(fs->ls, lt::control_structure_too_long);
  code[pc] = SETARG_Bx(code[pc], offset);
}

/*
** Generate code for a 'for' loop.
*/
static void forbody(LexState* ls, int base, int line, int nvars, int isgen)
{
  /* forbody -> DO block */
  static const OpCode forprep[2] = {OP_FORPREP, OP_TFORPREP};
  static const OpCode forloop[2] = {OP_FORLOOP, OP_TFORLOOP};
  constexpr int FN = LF_FORBODY;
  struct
  {
    int prep;
    BlockCnt bl;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    int site = LZ_SITENO();
    if (site == 2)
    { /* (its block again in the chain) */
      l.bl.previous = fs->bl;
      fs->bl = &l.bl;
    }
    switch (site)
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, checknext(ls, TK_DO));
  l.prep = luaK_codeABx(fs, forprep[isgen], base, 0);
  fs->freereg--;            /* both 'forprep' remove one register from the stack */
  enterblock(fs, &l.bl, 0); /* scope for declared variables */
  adjustlocalvars(ls, nvars);
  luaK_reserveregs(fs, nvars);
  LZ_SITE(2, block(ls));
  leaveblock(fs); /* end of scope for declared variables */
  fixforjump(fs, l.prep, luaK_getlabel(fs), 0);
  if (isgen)
  { /* generic for? */
    luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
    luaK_fixline(fs, line);
  }
  int endfor = luaK_codeABx(fs, forloop[isgen], base, 0);
  fixforjump(fs, endfor, l.prep + 1, 1);
  luaK_fixline(fs, line);
}

static void fornum(LexState* ls, TString* varname, int line)
{
  /* fornum -> NAME = exp,exp[,exp] forbody */
  constexpr int FN = LF_FORNUM;
  struct
  {
    int base;
    int r;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    case 6: goto site_6;
    case 7: goto site_7;
    default: LZ_BADSITE();
    }
  }
  l.base = fs->freereg;
  new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));
  new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));
  new_varkind(ls, varname, RDKCONST);
  LZ_SITE(1, checknext(ls, '='));
  LZ_SITE(2, exp1(ls)); /* initial value */
  LZ_SITE(3, checknext(ls, ','));
  LZ_SITE(4, exp1(ls)); /* limit */
  LZ_SITE(5, l.r = testnext(ls, ','));
  if (l.r)
  {
    LZ_SITE(6, exp1(ls)); /* optional step */
  }
  else
  { /* default step = 1 */
    luaK_int(fs, fs->freereg, 1);
    luaK_reserveregs(fs, 1);
  }
  adjustlocalvars(ls, 2); /* start scope for internal variables */
  LZ_SITE(7, forbody(ls, l.base, line, 1, 0));
}

static void forlist(LexState* ls, TString* indexname)
{
  /* forlist -> NAME {,NAME} IN explist forbody */
  constexpr int FN = LF_FORLIST;
  struct
  {
    int nvars; /* function, state, closing, control */
    int line;
    int base;
    int r;
    int nexps;
    TString* name;
    expdesc e;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  l.nvars = 4;
  l.base = fs->freereg;
  /* create internal variables */
  new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));  /* iterator function */
  new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));  /* state */
  new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));  /* closing var. (after swap) */
  new_varkind(ls, indexname, RDKCONST);
  /* other declared variables */
  for (;;)
  {
    LZ_SITE(1, l.r = testnext(ls, ','));
    if (!l.r)
      break;
    LZ_SITE(2, l.name = str_checkname(ls));
    new_localvar(ls, l.name);
    l.nvars++;
  }
  LZ_SITE(3, checknext(ls, TK_IN));
  l.line = ls->linenumber;
  LZ_SITE(4, l.nexps = explist(ls, &l.e));
  adjust_assign(ls, 4, l.nexps, &l.e);
  adjustlocalvars(ls, 3); /* start scope for internal variables */
  marktobeclosed(fs);     /* last internal var. must be closed */
  luaK_checkstack(fs, 2); /* extra space to call iterator */
  LZ_SITE(5, forbody(ls, l.base, l.line, l.nvars - 3, 1));
}

static void forstat(LexState* ls, int line)
{
  /* forstat -> FOR (fornum | forlist) END */
  constexpr int FN = LF_FORSTAT;
  struct
  {
    TString* varname;
    BlockCnt bl;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    int site = LZ_SITENO();
    l.bl.previous = fs->bl; /* (its block again in the chain) */
    fs->bl = &l.bl;
    switch (site)
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  enterblock(fs, &l.bl, 1);                /* scope for loop and control variables */
  LZ_SITE(1, luaX_next(ls));               /* skip 'for' */
  LZ_SITE(2, l.varname = str_checkname(ls)); /* first variable name */
  switch (ls->t.token)
  {
  case '=':
    LZ_SITE(3, fornum(ls, l.varname, line));
    break;
  case ',':
  case TK_IN:
    LZ_SITE(4, forlist(ls, l.varname));
    break;
  default:
    luaX_syntaxerror(ls, lt::or_in_expected);
  }
  LZ_SITE(5, check_match(ls, TK_END, TK_FOR, line));
  leaveblock(fs); /* loop scope ('break' jumps to this point) */
}

static int test_then_block(LexState* ls, int escapelist)
{
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  constexpr int FN = LF_TESTTHEN;
  struct
  {
    int condtrue;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, luaX_next(ls));          /* skip IF or ELSEIF */
  LZ_SITE(2, l.condtrue = cond(ls)); /* read condition */
  LZ_SITE(3, checknext(ls, TK_THEN));
  LZ_SITE(4, block(ls)); /* 'then' part */
  if (ls->t.token == TK_ELSE ||
      ls->t.token == TK_ELSEIF)                 /* followed by 'else'/'elseif'? */
    escapelist = luaK_concat(fs, escapelist, luaK_jump(fs)); /* must jump over it */
  luaK_patchtohere(fs, l.condtrue);
  return escapelist;
}

static void ifstat(LexState* ls, int line)
{
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  constexpr int FN = LF_IFSTAT;
  struct
  {
    int escapelist; /* exit list for finished parts */
    int r;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  l.escapelist = NO_JUMP;
  LZ_SITE(1, l.escapelist = test_then_block(ls, l.escapelist)); /* IF cond THEN block */
  while (ls->t.token == TK_ELSEIF)
  {
    LZ_SITE(2, l.escapelist = test_then_block(ls, l.escapelist)); /* ELSEIF cond THEN block */
  }
  LZ_SITE(3, l.r = testnext(ls, TK_ELSE));
  if (l.r)
  {
    LZ_SITE(4, block(ls)); /* 'else' part */
  }
  LZ_SITE(5, check_match(ls, TK_END, TK_IF, line));
  luaK_patchtohere(fs, l.escapelist); /* patch escape list to 'if' end */
}

static void localfunc(LexState* ls)
{
  constexpr int FN = LF_LOCALFUNC;
  struct
  {
    int fvar; /* function's variable index */
    int line;
    TString* name;
    expdesc b;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  l.fvar = fs->nactvar;
  LZ_SITE(1, l.name = str_checkname(ls));
  new_localvar(ls, l.name); /* new local variable */
  adjustlocalvars(ls, 1);   /* enter its scope */
  l.line = ls->linenumber;
  LZ_SITE(2, body(ls, &l.b, 0, l.line)); /* function created in next register */
  /* debug information will only see the variable after this point! */
  localdebuginfo(fs, l.fvar)->startpc = fs->pc;
}

static lu_byte getvarattribute(LexState* ls, lu_byte df)
{
  /* attrib -> ['<' NAME '>'] */
  constexpr int FN = LF_VARATTRIB;
  struct
  {
    int r;
    TString* ts;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, l.r = testnext(ls, '<'));
  if (l.r)
  {
    LZ_SITE(2, l.ts = str_checkname(ls));
    LZ_SITE(3, checknext(ls, '>'));
    byte_view attr = l.ts->data.view();
    if (attr == lt::const_kw)
      return RDKCONST; /* read-only variable */
    else if (attr == lt::close_kw)
      return RDKTOCLOSE; /* to-be-closed variable */
    else
      luaK_semerror(ls, lt::unknown_attribute_s, FmtArgs().str(l.ts->data.view()));
  }
  return df; /* return default value */
}

static void checktoclose(FuncState* fs, int level)
{
  if (level != -1)
  { /* is there a to-be-closed variable? */
    marktobeclosed(fs);
    luaK_codeABC(fs, OP_TBC, reglevel(fs, level), 0, 0);
  }
}

static void localstat(LexState* ls)
{
  /* stat -> LOCAL NAME attrib { ',' NAME attrib } ['=' explist] */
  constexpr int FN = LF_LOCALSTAT;
  struct
  {
    int toclose; /* index of to-be-closed variable (if any) */
    int vidx;    /* index of last variable */
    int nvars;
    int nexps;
    int r;
    lu_byte defkind;
    lu_byte kind;
    TString* vname;
    expdesc e;
  } l;
  FuncState* fs = ls->fs;
  Vardesc* var; /* last variable */
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    case 6: goto site_6;
    default: LZ_BADSITE();
    }
  }
  l.toclose = -1;
  l.nvars = 0;
  /* get prefixed attribute (if any); default is regular local variable */
  LZ_SITE(1, l.defkind = getvarattribute(ls, VDKREG));
  for (;;)
  { /* for each variable */
    LZ_SITE(2, l.vname = str_checkname(ls));            /* get its name */
    LZ_SITE(3, l.kind = getvarattribute(ls, l.defkind)); /* postfixed attribute */
    l.vidx = new_varkind(ls, l.vname, l.kind);           /* predeclare it */
    if (l.kind == RDKTOCLOSE)
    {                      /* to-be-closed? */
      if (l.toclose != -1) /* one already present? */
        luaK_semerror(ls, lt::multiple_to_be_closed_variables_in_local_list, FmtArgs());
      l.toclose = fs->nactvar + l.nvars;
    }
    l.nvars++;
    LZ_SITE(4, l.r = testnext(ls, ','));
    if (!l.r)
      break;
  }
  LZ_SITE(5, l.r = testnext(ls, '='));
  if (l.r) /* initialization? */
  {
    LZ_SITE(6, l.nexps = explist(ls, &l.e));
  }
  else
  {
    l.e.k = VVOID;
    l.nexps = 0;
  }
  var = getlocalvardesc(fs, l.vidx); /* retrieve last variable */
  if (l.nvars == l.nexps &&          /* no adjustments? */
      var->kind == RDKCONST &&       /* last variable is const? */
      luaK_exp2const(fs, &l.e, &var->k))
  {                                   /* compile-time constant? */
    var->kind = RDKCTC;               /* variable is a compile-time constant */
    adjustlocalvars(ls, l.nvars - 1); /* exclude last variable */
    fs->nactvar++;                    /* but count it */
  }
  else
  {
    adjust_assign(ls, l.nvars, l.nexps, &l.e);
    adjustlocalvars(ls, l.nvars);
  }
  checktoclose(fs, l.toclose);
}

static lu_byte getglobalattribute(LexState* ls, lu_byte df)
{
  lu_byte kind = getvarattribute(ls, df);
  switch (kind)
  {
  case RDKTOCLOSE:
    luaK_semerror(ls, lt::global_variables_cannot_be_to_be_closed, FmtArgs());
    return kind; /* to avoid warnings */
  case RDKCONST:
    return GDKCONST; /* adjust kind for global variable */
  default:
    return kind;
  }
}

static void checkglobal(LexState* ls, TString* varname, int line)
{
  FuncState* fs = ls->fs;
  expdesc var;
  int k;
  buildglobal(ls, varname, &var); /* create global variable in 'var' */
  k = var.ind.keystr;           /* index of global name in 'k' */
  luaK_codecheckglobal(fs, &var, k, line);
}

/*
** Recursively traverse list of globals to be initalized. When
** going, generate table description for the global. In the end,
** after all indices have been generated, read list of initializing
** expressions. When returning, generate the assignment of the value on
** the stack to the corresponding table description. 'n' is the variable
** being handled, range [0, nvars - 1].
*/
static void initglobal(LexState* ls, int nvars, int firstidx, int n,
                       int line)
{
  constexpr int FN = LF_INITGLOBAL;
  struct
  {
    int nexps;
    TString* varname;
    expdesc e;
    expdesc var;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  if (n == nvars)
  { /* traversed all variables? */
    LZ_SITE(1, l.nexps = explist(ls, &l.e)); /* read list of expressions */
    adjust_assign(ls, nvars, l.nexps, &l.e);
  }
  else
  { /* handle variable 'n' */
    l.varname = getlocalvardesc(fs, firstidx + n)->name;
    buildglobal(ls, l.varname, &l.var); /* create global variable in 'var' */
    LZ_SITE(2, enterlevel(ls); /* control recursion depth */
               initglobal(ls, nvars, firstidx, n + 1, line));
    leavelevel(ls);
    checkglobal(ls, l.varname, line);
    storevartop(fs, &l.var);
  }
}

static void globalnames(LexState* ls, lu_byte defkind)
{
  constexpr int FN = LF_GLOBALNAMES;
  struct
  {
    int nvars;
    int lastidx; /* index of last registered variable */
    int r;
    int line;
    lu_byte kind;
    TString* vname;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    default: LZ_BADSITE();
    }
  }
  l.nvars = 0;
  for (;;)
  { /* for each name */
    LZ_SITE(1, l.vname = str_checkname(ls));
    LZ_SITE(2, l.kind = getglobalattribute(ls, defkind));
    l.lastidx = new_varkind(ls, l.vname, l.kind);
    l.nvars++;
    LZ_SITE(3, l.r = testnext(ls, ','));
    if (!l.r)
      break;
  }
  LZ_SITE(4, l.r = testnext(ls, '='));
  if (l.r) /* initialization? */
  {
    l.line = ls->linenumber;
    LZ_SITE(5, initglobal(ls, l.nvars, l.lastidx - l.nvars + 1, 0, l.line));
  }
  fs->nactvar = (short)(fs->nactvar + l.nvars); /* activate declaration */
}

static void globalstat(LexState* ls)
{
  /* globalstat -> (GLOBAL) attrib '*'
     globalstat -> (GLOBAL) attrib NAME attrib {',' NAME attrib} */
  constexpr int FN = LF_GLOBALSTAT;
  struct
  {
    lu_byte defkind;
    int r;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    default: LZ_BADSITE();
    }
  }
  /* get prefixed attribute (if any); default is regular global variable */
  LZ_SITE(1, l.defkind = getglobalattribute(ls, GDKREG));
  LZ_SITE(2, l.r = testnext(ls, '*'));
  if (!l.r)
  {
    LZ_SITE(3, globalnames(ls, l.defkind));
  }
  else
  {
    new_varkind(ls, nullptr, l.defkind);
    fs->nactvar++; /* activate declaration */
  }
}

static void globalfunc(LexState* ls, int line)
{
  /* globalfunc -> (GLOBAL FUNCTION) NAME body */
  constexpr int FN = LF_GLOBALFUNC;
  struct
  {
    int bline;
    TString* fname;
    expdesc var, b;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, l.fname = str_checkname(ls));
  new_varkind(ls, l.fname, GDKREG); /* declare global variable */
  fs->nactvar++;                    /* enter its scope */
  buildglobal(ls, l.fname, &l.var);
  l.bline = ls->linenumber;
  LZ_SITE(2, body(ls, &l.b, 0, l.bline)); /* compile and return closure in 'b' */
  checkglobal(ls, l.fname, line);
  luaK_storevar(fs, &l.var, &l.b);
  luaK_fixline(fs, line); /* definition "happens" in the first line */
}

static void globalstatfunc(LexState* ls, int line)
{
  /* stat -> GLOBAL globalfunc | GLOBAL globalstat */
  constexpr int FN = LF_GLOBALSTATFUNC;
  struct
  {
    int r;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, luaX_next(ls)); /* skip 'global' */
  LZ_SITE(2, l.r = testnext(ls, TK_FUNCTION));
  if (l.r)
  {
    LZ_SITE(3, globalfunc(ls, line));
  }
  else
  {
    LZ_SITE(4, globalstat(ls));
  }
}

static int funcname(LexState* ls, expdesc* v)
{
  /* funcname -> NAME {fieldsel} [':' NAME] */
  constexpr int FN = LF_FUNCNAME;
  struct
  {
    int ismethod;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    default: LZ_BADSITE();
    }
  }
  l.ismethod = 0;
  LZ_SITE(1, singlevar(ls, v));
  while (ls->t.token == '.')
  {
    LZ_SITE(2, fieldsel(ls, v));
  }
  if (ls->t.token == ':')
  {
    l.ismethod = 1;
    LZ_SITE(3, fieldsel(ls, v));
  }
  return l.ismethod;
}

static void funcstat(LexState* ls, int line)
{
  /* funcstat -> FUNCTION funcname body */
  constexpr int FN = LF_FUNCSTAT;
  struct
  {
    int ismethod;
    expdesc v, b;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, luaX_next(ls)); /* skip FUNCTION */
  LZ_SITE(2, l.ismethod = funcname(ls, &l.v));
  check_readonly(ls, &l.v);
  LZ_SITE(3, body(ls, &l.b, l.ismethod, line));
  luaK_storevar(ls->fs, &l.v, &l.b);
  luaK_fixline(ls->fs, line); /* definition "happens" in the first line */
}

static void exprstat(LexState* ls)
{
  /* stat -> func | assignment */
  constexpr int FN = LF_EXPRSTAT;
  struct
  {
    struct LHS_assign v;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, suffixedexp(ls, &l.v.v));
  if (ls->t.token == '=' || ls->t.token == ',')
  { /* stat -> assignment ? */
    l.v.prev = nullptr;
    LZ_SITE(2, restassign(ls, &l.v, 1));
  }
  else
  { /* stat -> func */
    std::span<Instruction> code = fs->f->code;
    check_condition(ls, l.v.v.k == VCALL, lt::syntax_error);
    int pc = l.v.v.info;
    code[pc] = SETARG_C(code[pc], 1); /* call statement uses no results */
  }
}

static void retstat(LexState* ls)
{
  /* stat -> RETURN [explist] [';'] */
  constexpr int FN = LF_RETSTAT;
  struct
  {
    int nret;  /* number of values being returned */
    int first; /* first slot to be returned */
    expdesc e;
  } l;
  FuncState* fs = ls->fs;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  l.first = luaY_nvarstack(fs);
  if (block_follow(ls, 1) || ls->t.token == ';')
    l.nret = 0; /* return no values */
  else
  {
    LZ_SITE(1, l.nret = explist(ls, &l.e)); /* optional return values */
    if (hasmultret(l.e.k))
    {
      luaK_setmultret(fs, &l.e);
      if (l.e.k == VCALL && l.nret == 1 && !fs->bl->insidetbc)
      { /* tail call? */
        fs->f->code[l.e.info] = SET_OPCODE(fs->f->code[l.e.info], OP_TAILCALL);
        assert(GETARG_A(fs->f->code[l.e.info]) == luaY_nvarstack(fs));
      }
      l.nret = LUA_MULTRET; /* return all values */
    }
    else
    {
      if (l.nret == 1)                       /* only one single value? */
        l.first = luaK_exp2anyreg(fs, &l.e); /* can use original slot */
      else
      { /* values must go to the top of the stack */
        luaK_exp2nextreg(fs, &l.e);
        assert(l.nret == fs->freereg - l.first);
      }
    }
  }
  luaK_ret(fs, l.first, l.nret);
  LZ_SITE(2, testnext(ls, ';')); /* skip optional semicolon */
}

static void statement(LexState* ls)
{
  constexpr int FN = LF_STATEMENT;
  struct
  {
    int line; /* may be needed for error messages */
    int r;
    TString* name;
  } l;
  enterlevel(ls);
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    case 6: goto site_6;
    case 7: goto site_7;
    case 8: goto site_8;
    case 9: goto site_9;
    case 10: goto site_10;
    case 11: goto site_11;
    case 12: goto site_12;
    case 13: goto site_13;
    case 14: goto site_14;
    case 15: goto site_15;
    case 16: goto site_16;
    case 17: goto site_17;
    case 18: goto site_18;
    case 19: goto site_19;
    case 20: goto site_20;
    case 21: goto site_21;
    case 22: goto site_22;
    case 23: goto site_23;
    default: LZ_BADSITE();
    }
  }
  l.line = ls->linenumber;
  switch (ls->t.token)
  {
  case ';':
  {                            /* stat -> ';' (empty statement) */
    LZ_SITE(1, luaX_next(ls)); /* skip ';' */
    break;
  }
  case TK_IF:
  { /* stat -> ifstat */
    LZ_SITE(2, ifstat(ls, l.line));
    break;
  }
  case TK_WHILE:
  { /* stat -> whilestat */
    LZ_SITE(3, whilestat(ls, l.line));
    break;
  }
  case TK_DO:
  {                            /* stat -> DO block END */
    LZ_SITE(4, luaX_next(ls)); /* skip DO */
    LZ_SITE(5, block(ls));
    LZ_SITE(6, check_match(ls, TK_END, TK_DO, l.line));
    break;
  }
  case TK_FOR:
  { /* stat -> forstat */
    LZ_SITE(7, forstat(ls, l.line));
    break;
  }
  case TK_REPEAT:
  { /* stat -> repeatstat */
    LZ_SITE(8, repeatstat(ls, l.line));
    break;
  }
  case TK_FUNCTION:
  { /* stat -> funcstat */
    LZ_SITE(9, funcstat(ls, l.line));
    break;
  }
  case TK_LOCAL:
  {                                         /* stat -> localstat */
    LZ_SITE(10, luaX_next(ls));             /* skip LOCAL */
    LZ_SITE(11, l.r = testnext(ls, TK_FUNCTION)); /* local function? */
    if (l.r)
    {
      LZ_SITE(12, localfunc(ls));
    }
    else
    {
      LZ_SITE(13, localstat(ls));
    }
    break;
  }
  case TK_GLOBAL:
  { /* stat -> globalstatfunc */
    LZ_SITE(14, globalstatfunc(ls, l.line));
    break;
  }
  case TK_DBCOLON:
  {                             /* stat -> label */
    LZ_SITE(15, luaX_next(ls)); /* skip double colon */
    LZ_SITE(16, l.name = str_checkname(ls));
    LZ_SITE(17, labelstat(ls, l.name, l.line));
    break;
  }
  case TK_RETURN:
  {                             /* stat -> retstat */
    LZ_SITE(18, luaX_next(ls)); /* skip RETURN */
    LZ_SITE(19, retstat(ls));
    break;
  }
  case TK_BREAK:
  { /* stat -> breakstat */
    LZ_SITE(20, breakstat(ls, l.line));
    break;
  }
  case TK_GOTO:
  {                             /* stat -> 'goto' NAME */
    LZ_SITE(21, luaX_next(ls)); /* skip 'goto' */
    LZ_SITE(22, gotostat(ls, l.line));
    break;
  }
  /* FALLTHROUGH */
  default:
  { /* stat -> func | assignment */
    LZ_SITE(23, exprstat(ls));
    break;
  }
  }
  assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
             ls->fs->freereg >= luaY_nvarstack(ls->fs));
  ls->fs->freereg = luaY_nvarstack(ls->fs); /* free registers */
  leavelevel(ls);
}

/* }====================================================================== */

/* }====================================================================== */

/*
** compiles the main function, which is a regular vararg function with an
** upvalue named LUA_ENV
*/
static void mainfunc(LexState* ls, FuncState* fs)
{
  constexpr int FN = LF_MAINFUNC;
  struct
  {
    BlockCnt bl;
  } l;
  Upvaldesc* env;
  if (LZ_RESUMING())
  { /* (its function and block again in the chains) */
    int site = LZ_SITENO();
    fs->prev = nullptr;
    fs->ls = ls;
    ls->fs = fs;
    l.bl.previous = nullptr;
    fs->bl = &l.bl;
    switch (site)
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  open_func(ls, fs, &l.bl);
  setvararg(fs);          /* main function is always vararg */
  env = allocupvalue(fs); /* ...set environment upvalue */
  env->instack = 1;
  env->idx = 0;
  env->kind = VDKREG;
  env->name = ls->envn;
  luaC_objbarrier(ls->L, fs->f, env->name);
  LZ_SITE(1, luaX_next(ls)); /* read first token */
  LZ_SITE(2, statlist(ls));  /* parse main body */
  check(ls, TK_EOS);
  close_func(ls);
}

LClosure* luaY_parser(lua_State* L, ZIO* z, Table* anchor, MBuffer* buff, Dyndata* dyd, byte_view name, int firstchar)
{
  constexpr int FN = LF_PARSER;
  struct
  {
    LexState lexstate;
    FuncState funcstate;
    LClosure* cl;
  } l;
  if (l_unlikely(z->resuming))
  { /* (the scanner again with this load's stream, buffer and lists) */
    if (luaZ_restore(z->rz, FN, &l, sizeof(l)) != 1)
      luaZ_badstate(z->rz);
    l.lexstate.L = L;
    l.lexstate.z = z;
    l.lexstate.buff = buff;
    l.lexstate.dyd = dyd;
    l.lexstate.h = anchor;
    l.lexstate.fs = nullptr;
    goto site_1;
  }
  l.lexstate.h = anchor;                    /* table for scanner */
  l.cl = LClosure::luaF_newLclosure(L, 1); /* create main closure */
  luaZ_madefunction(z->rz, l.cl);
  assert(novariant(l.cl->tt) >= LUA_TSTRING);
  luaD_anchorobj(L, anchor, l.cl); /* anchor it in scanner table */
  l.funcstate.f = l.cl->p = Proto::luaF_newproto(L);
  luaC_objbarrier(L, l.cl, l.cl->p);
  l.funcstate.f->source = TString::luaS_createstr(L, name); /* create and anchor TString */
  luaC_objbarrier(L, l.funcstate.f, l.funcstate.f->source);
  l.lexstate.buff = buff;
  l.lexstate.dyd = dyd;
  dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
  luaX_setinput(L, &l.lexstate, z, l.funcstate.f->source, firstchar);
site_1:
  try
  {
    mainfunc(&l.lexstate, &l.funcstate);
  }
  catch (...)
  {
    luaZ_record(z->rz, FN, 1, &l, sizeof(l));
    throw;
  }
  assert(!l.funcstate.prev && l.funcstate.nups == 1 && !l.lexstate.fs);
  /* all scopes should be correctly finished */
  assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  return l.cl;
}

}
