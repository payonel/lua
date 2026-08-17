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

static l_noret error_expected(LexState* ls, int token)
{
  luaX_syntaxerror(ls, luaO_pushfstring(ls->L, lt::s_expected, FmtArgs().str(luaX_token2str(ls, token)))->view());
}

static l_noret errorlimit(FuncState* fs, int limit, byte_view what)
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
static lu_byte reglevel(FuncState* fs, int nvar)
{
  while (nvar-- > 0)
  {
    Vardesc* vd = getlocalvardesc(fs, nvar); /* get previous variable */
    if (varinreg(vd))                        /* is in a register? */
      return ((lu_byte)(vd->ridx + 1));
  }
  return 0; /* no variables in registers */
}

/*
** Return the number of variables in the register stack for the given
** function.
*/
lu_byte luaY_nvarstack(FuncState* fs)
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
  e->var.vidx = ((short)(vidx));
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
      assert((kv->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
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
    var->ridx = ((lu_byte)(reglevel++));
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
  fs->ls->dyd->actvar.n -= (fs->nactvar - tolevel);
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
    up->idx = ((lu_byte)(v->info));
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
  for (i = ((int)(fs->nactvar)) - 1; i >= 0; i--)
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
      return ((int)(var->k));
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
    fs->freereg = ((lu_byte)(fs->freereg + needed)); /* remove extra values */
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
static l_noret jumpscopeerror(LexState* ls, Labeldesc* gt)
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
    lu_byte stklevel = reglevel(fs, label->nactvar);
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
static l_noret undefgoto(LexState* ls, Labeldesc* gt)
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
  lu_byte stklevel = reglevel(fs, bl->nactvar); /* level outside block */
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
  f->p[fs->np++] = clp = luaF_newproto(L);
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
  fs->kcache = luaH_new(L);                      /* create table for function */
  assert((fs->kcache->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaD_anchorobj(L, ls->h, const_cast<GCObject*>(static_cast<const GCObject*>(fs->kcache))); /* anchor it */
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
  f->code = luaM_alloc_Instruction_span(L->l_G, L, f->code, (size_t)fs->pc, true);
  f->lineinfo = luaM_alloc_ls_byte_span(L->l_G, L, f->lineinfo, (size_t)fs->pc, true);
  f->abslineinfo = luaM_alloc_AbsLineInfo_span(L->l_G, L, f->abslineinfo, (size_t)fs->nabslineinfo, true);
  f->k = luaM_alloc_TValue_span(L->l_G, L, f->k, (size_t)fs->nk, true);
  f->p = luaM_alloc_Proto_ptr_span(L->l_G, L, f->p, (size_t)fs->np, true);
  f->locvars = luaM_alloc_LocVar_span(L->l_G, L, f->locvars, (size_t)fs->ndebugvars, true);
  f->upvalues = luaM_alloc_Upvaldesc_span(L->l_G, L, f->upvalues, (size_t)fs->nups, true);
  /* remove kcache table from scanner table ("weigh" its anchor) */
  sethvalue(L, &temp, fs->kcache); /* key to be set to nil */
  luaH_set(L, ls->h, &temp, &L->l_G->nilvalue);
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
  while (!block_follow(ls, 1))
  {
    if (ls->t.token == TK_RETURN)
    {
      statement(ls);
      return; /* 'return' must be last statement */
    }
    statement(ls);
  }
}

static void fieldsel(LexState* ls, expdesc* v)
{
  /* fieldsel -> ['.' | ':'] NAME */
  FuncState* fs = ls->fs;
  expdesc key;
  luaK_exp2anyregup(fs, v);
  luaX_next(ls); /* skip the dot or colon */
  codename(ls, &key);
  luaK_indexed(fs, v, &key);
}

static void yindex(LexState* ls, expdesc* v)
{
  /* index -> '[' expr ']' */
  luaX_next(ls); /* skip the '[' */
  expr(ls, v);
  luaK_exp2val(ls->fs, v);
  checknext(ls, ']');
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
  FuncState* fs = ls->fs;
  lu_byte reg = ls->fs->freereg;
  expdesc tab, key, val;
  if (ls->t.token == TK_NAME)
    codename(ls, &key);
  else /* ls->t.token == '[' */
    yindex(ls, &key);
  cc->nh++;
  checknext(ls, '=');
  tab = *cc->t;
  luaK_indexed(fs, &tab, &key);
  expr(ls, &val);
  luaK_storevar(fs, &tab, &val);
  fs->freereg = reg; /* free registers */
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
  switch (ls->t.token)
  {
  case TK_NAME:
  {                                /* may be 'listfield' or 'recfield' */
    if (luaX_lookahead(ls) != '=') /* expression? */
      listfield(ls, cc);
    else
      recfield(ls, cc);
    break;
  }
  case '[':
  {
    recfield(ls, cc);
    break;
  }
  default:
  {
    listfield(ls, cc);
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
  FuncState* fs = ls->fs;
  int line = ls->linenumber;
  int pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
  ConsControl cc;
  luaK_code(fs, 0); /* space for extra arg. */
  cc.na = cc.nh = cc.tostore = 0;
  cc.t = t;
  init_exp(t, VNONRELOC, fs->freereg); /* table will be at stack top */
  luaK_reserveregs(fs, 1);
  init_exp(&cc.v, VVOID, 0); /* no value (yet) */
  checknext(ls, '{' /*}*/);
  cc.maxtostore = maxtostore(fs);
  do
  {
    if (ls->t.token == /*{*/ '}')
      break;
    if (cc.v.k != VVOID)       /* is there a previous list item? */
      closelistfield(fs, &cc); /* close it */
    field(ls, &cc);
    luaY_checklimit(fs, cc.tostore + cc.na + cc.nh, MAX_CNST, lt::items_in_a_constructor);
  } while (testnext(ls, ',') || testnext(ls, ';'));
  check_match(ls, /*{*/ '}', '{' /*}*/, line);
  lastlistfield(fs, &cc);
  luaK_settablesize(fs, pc, t->info, cc.na, cc.nh);
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
  FuncState* fs = ls->fs;
  Proto* f = fs->f;
  int nparams = 0;
  int varargk = 0;
  if (ls->t.token != ')')
  { /* is 'parlist' not empty? */
    do
    {
      switch (ls->t.token)
      {
      case TK_NAME:
      {
        new_localvar(ls, str_checkname(ls));
        nparams++;
        break;
      }
      case TK_DOTS:
      {
        varargk = 1;
        luaX_next(ls); /* skip '...' */
        if (ls->t.token == TK_NAME)
          new_varkind(ls, str_checkname(ls), RDKVAVAR);
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
    } while (!varargk && testnext(ls, ','));
  }
  adjustlocalvars(ls, nparams);
  f->numparams = ((lu_byte)(fs->nactvar));
  if (varargk)
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
  FuncState new_fs;
  BlockCnt bl;
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  open_func(ls, &new_fs, &bl);
  checknext(ls, '(');
  if (ismethod)
  {
    byte_view self = lt::self;
    new_localvar(ls, luaX_newstring(ls, self, 0, self.size())); /* create 'self' parameter */
    adjustlocalvars(ls, 1);
  }
  parlist(ls);
  checknext(ls, ')');
  statlist(ls);
  new_fs.f->lastlinedefined = ls->linenumber;
  check_match(ls, TK_END, TK_FUNCTION, line);
  codeclosure(ls, e);
  close_func(ls);
}

static int explist(LexState* ls, expdesc* v)
{
  /* explist -> expr { ',' expr } */
  int n = 1; /* at least one expression */
  expr(ls, v);
  while (testnext(ls, ','))
  {
    luaK_exp2nextreg(ls->fs, v);
    expr(ls, v);
    n++;
  }
  return n;
}

static void funcargs(LexState* ls, expdesc* f)
{
  FuncState* fs = ls->fs;
  expdesc args;
  int base, nparams;
  int line = ls->linenumber;
  switch (ls->t.token)
  {
  case '(':
  { /* funcargs -> '(' [ explist ] ')' */
    luaX_next(ls);
    if (ls->t.token == ')') /* arg list is empty? */
      args.k = VVOID;
    else
    {
      explist(ls, &args);
      if (hasmultret(args.k))
        luaK_setmultret(fs, &args);
    }
    check_match(ls, ')', '(', line);
    break;
  }
  case '{' /*}*/:
  { /* funcargs -> constructor */
    constructor(ls, &args);
    break;
  }
  case TK_STRING:
  { /* funcargs -> STRING */
    codestring(&args, ls->t.ts);
    luaX_next(ls); /* must use 'seminfo' before 'next' */
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
  if (hasmultret(args.k))
    nparams = LUA_MULTRET; /* open call */
  else
  {
    if (args.k != VVOID)
      luaK_exp2nextreg(fs, &args); /* close last argument */
    nparams = fs->freereg - (base + 1);
  }
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams + 1, 2));
  luaK_fixline(fs, line);
  /* call removes function and arguments and leaves one result (unless
     changed later) */
  fs->freereg = ((lu_byte)(base + 1));
}

/*
** {======================================================================
** Expression parsing
** =======================================================================
*/

static void primaryexp(LexState* ls, expdesc* v)
{
  /* primaryexp -> NAME | '(' expr ')' */
  switch (ls->t.token)
  {
  case '(':
  {
    int line = ls->linenumber;
    luaX_next(ls);
    expr(ls, v);
    check_match(ls, ')', '(', line);
    luaK_dischargevars(ls->fs, v);
    return;
  }
  case TK_NAME:
  {
    singlevar(ls, v);
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
  FuncState* fs = ls->fs;
  primaryexp(ls, v);
  for (;;)
  {
    switch (ls->t.token)
    {
    case '.':
    { /* fieldsel */
      fieldsel(ls, v);
      break;
    }
    case '[':
    { /* '[' exp ']' */
      expdesc key;
      luaK_exp2anyregup(fs, v);
      yindex(ls, &key);
      luaK_indexed(fs, v, &key);
      break;
    }
    case ':':
    { /* ':' NAME funcargs */
      expdesc key;
      luaX_next(ls);
      codename(ls, &key);
      luaK_self(fs, v, &key);
      funcargs(ls, v);
      break;
    }
    case '(':
    case TK_STRING:
    case '{' /*}*/:
    { /* funcargs */
      luaK_exp2nextreg(fs, v);
      funcargs(ls, v);
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
    constructor(ls, v);
    return;
  }
  case TK_FUNCTION:
  {
    luaX_next(ls);
    body(ls, v, 0, ls->linenumber);
    return;
  }
  default:
  {
    suffixedexp(ls, v);
    return;
  }
  }
  luaX_next(ls);
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
  BinOpr op;
  UnOpr uop;
  enterlevel(ls);
  uop = getunopr(ls->t.token);
  if (uop != OPR_NOUNOPR)
  { /* prefix (unary) operator? */
    int line = ls->linenumber;
    luaX_next(ls); /* skip operator */
    subexpr(ls, v, UNARY_PRIORITY);
    luaK_prefix(ls->fs, uop, v, line);
  }
  else
    simpleexp(ls, v);
  /* expand while operators have priorities higher than 'limit' */
  op = getbinopr(ls->t.token);
  while (op != OPR_NOBINOPR && priority[op].left > limit)
  {
    expdesc v2;
    BinOpr nextop;
    int line = ls->linenumber;
    luaX_next(ls); /* skip operator */
    luaK_infix(ls->fs, op, v);
    /* read sub-expression with higher priority */
    nextop = subexpr(ls, &v2, priority[op].right);
    luaK_posfix(ls->fs, op, v, &v2, line);
    op = nextop;
  }
  leavelevel(ls);
  return op; /* return first untreated operator */
}

static void expr(LexState* ls, expdesc* v)
{
  subexpr(ls, v, 0);
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
  FuncState* fs = ls->fs;
  BlockCnt bl;
  enterblock(fs, &bl, 0);
  statlist(ls);
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
  lu_byte extra = fs->freereg; /* eventual position to save local variable */
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
  expdesc e;
  check_condition(ls, vkisvar(lh->v.k), lt::syntax_error);
  check_readonly(ls, &lh->v);
  if (testnext(ls, ','))
  { /* restassign -> ',' suffixedexp restassign */
    struct LHS_assign nv;
    nv.prev = lh;
    suffixedexp(ls, &nv.v);
    if (!vkisindexed(nv.v.k))
      check_conflict(ls, lh, &nv.v);
    enterlevel(ls); /* control recursion depth */
    restassign(ls, &nv, nvars + 1);
    leavelevel(ls);
  }
  else
  { /* restassign -> '=' explist */
    int nexps;
    checknext(ls, '=');
    nexps = explist(ls, &e);
    if (nexps != nvars)
      adjust_assign(ls, nvars, nexps, &e);
    else
    {
      luaK_setoneret(ls->fs, &e); /* close last expression */
      luaK_storevar(ls->fs, &lh->v, &e);
      return; /* avoid default */
    }
  }
  storevartop(ls->fs, &lh->v); /* default assignment */
}

static int cond(LexState* ls)
{
  /* cond -> exp */
  expdesc v;
  expr(ls, &v); /* read condition */
  if (v.k == VNIL)
    v.k = VFALSE; /* 'falses' are all equal here */
  luaK_goiftrue(ls->fs, &v);
  return v.f;
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
  BlockCnt* bl; /* to look for an enclosing loop */
  for (bl = ls->fs->bl; bl != nullptr; bl = bl->previous)
  {
    if (bl->isloop) /* found one? */
      break;
  }
  if (bl == nullptr)
    luaX_syntaxerror(ls, lt::break_outside_loop);
  bl->isloop = 2; /* signal that block has pending breaks */
  luaX_next(ls);  /* skip break */
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
  checknext(ls, TK_DBCOLON); /* skip double colon */
  while (ls->t.token == ';' || ls->t.token == TK_DBCOLON)
    statement(ls);         /* skip other no-op statements */
  checkrepeated(ls, name); /* check for repeated labels */
  createlabel(ls, name, line, block_follow(ls, 0));
}

static void whilestat(LexState* ls, int line)
{
  /* whilestat -> WHILE cond DO block END */
  FuncState* fs = ls->fs;
  int whileinit;
  int condexit;
  BlockCnt bl;
  luaX_next(ls); /* skip WHILE */
  whileinit = luaK_getlabel(fs);
  condexit = cond(ls);
  enterblock(fs, &bl, 1);
  checknext(ls, TK_DO);
  block(ls);
  luaK_jumpto(fs, whileinit);
  check_match(ls, TK_END, TK_WHILE, line);
  leaveblock(fs);
  luaK_patchtohere(fs, condexit); /* false conditions finish the loop */
}

static void repeatstat(LexState* ls, int line)
{
  /* repeatstat -> REPEAT block UNTIL cond */
  int condexit;
  FuncState* fs = ls->fs;
  int repeat_init = luaK_getlabel(fs);
  BlockCnt bl1, bl2;
  enterblock(fs, &bl1, 1); /* loop block */
  enterblock(fs, &bl2, 0); /* scope block */
  luaX_next(ls);           /* skip REPEAT */
  statlist(ls);
  check_match(ls, TK_UNTIL, TK_REPEAT, line);
  condexit = cond(ls); /* read condition (inside scope block) */
  leaveblock(fs);      /* finish scope */
  if (bl2.upval)
  {                                 /* upvalues? */
    int exit = luaK_jump(fs);       /* normal exit must jump over fix */
    luaK_patchtohere(fs, condexit); /* repetition must close upvalues */
    luaK_codeABC(fs, OP_CLOSE, reglevel(fs, bl2.nactvar), 0, 0);
    condexit = luaK_jump(fs);   /* repeat after closing upvalues */
    luaK_patchtohere(fs, exit); /* normal exit comes to here */
  }
  luaK_patchlist(fs, condexit, repeat_init); /* close the loop */
  leaveblock(fs);                            /* finish loop */
}

/*
** Read an expression and generate code to put its results in next
** stack slot.
**
*/
static void exp1(LexState* ls)
{
  expdesc e;
  expr(ls, &e);
  luaK_exp2nextreg(ls->fs, &e);
  assert(e.k == VNONRELOC);
}

/*
** Fix for instruction at position 'pc' to jump to 'dest'.
** (Jump addresses are relative in Lua). 'back' true means
** a back jump.
*/
static void fixforjump(FuncState* fs, int pc, int dest, int back)
{
  Instruction* jmp = &fs->f->code[pc];
  int offset = dest - (pc + 1);
  if (back)
    offset = -offset;
  if (l_unlikely(offset > MAXARG_Bx))
    luaX_syntaxerror(fs->ls, lt::control_structure_too_long);
  ((*jmp) = (((*jmp) & MASK0(SIZE_Bx, POS_Bx)) | ((((Instruction)(offset)) << POS_Bx) & MASK1(SIZE_Bx, POS_Bx))));
}

/*
** Generate code for a 'for' loop.
*/
static void forbody(LexState* ls, int base, int line, int nvars, int isgen)
{
  /* forbody -> DO block */
  static const OpCode forprep[2] = {OP_FORPREP, OP_TFORPREP};
  static const OpCode forloop[2] = {OP_FORLOOP, OP_TFORLOOP};
  BlockCnt bl;
  FuncState* fs = ls->fs;
  int prep, endfor;
  checknext(ls, TK_DO);
  prep = luaK_codeABx(fs, forprep[isgen], base, 0);
  fs->freereg--;          /* both 'forprep' remove one register from the stack */
  enterblock(fs, &bl, 0); /* scope for declared variables */
  adjustlocalvars(ls, nvars);
  luaK_reserveregs(fs, nvars);
  block(ls);
  leaveblock(fs); /* end of scope for declared variables */
  fixforjump(fs, prep, luaK_getlabel(fs), 0);
  if (isgen)
  { /* generic for? */
    luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
    luaK_fixline(fs, line);
  }
  endfor = luaK_codeABx(fs, forloop[isgen], base, 0);
  fixforjump(fs, endfor, prep + 1, 1);
  luaK_fixline(fs, line);
}

static void fornum(LexState* ls, TString* varname, int line)
{
  /* fornum -> NAME = exp,exp[,exp] forbody */
  FuncState* fs = ls->fs;
  int base = fs->freereg;
  new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));
  new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));
  new_varkind(ls, varname, RDKCONST);
  checknext(ls, '=');
  exp1(ls); /* initial value */
  checknext(ls, ',');
  exp1(ls); /* limit */
  if (testnext(ls, ','))
    exp1(ls); /* optional step */
  else
  { /* default step = 1 */
    luaK_int(fs, fs->freereg, 1);
    luaK_reserveregs(fs, 1);
  }
  adjustlocalvars(ls, 2); /* start scope for internal variables */
  forbody(ls, base, line, 1, 0);
}

static void forlist(LexState* ls, TString* indexname)
{
  /* forlist -> NAME {,NAME} IN explist forbody */
  FuncState* fs = ls->fs;
  expdesc e;
  int nvars = 4; /* function, state, closing, control */
  int line;
  int base = fs->freereg;
  /* create internal variables */
  new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));  /* iterator function */
  new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));  /* state */
  new_localvar(ls, luaX_newstring(ls, lt::for_state, 0, lt::for_state.size()));  /* closing var. (after swap) */
  new_varkind(ls, indexname, RDKCONST);
  /* other declared variables */
  while (testnext(ls, ','))
  {
    new_localvar(ls, str_checkname(ls));
    nvars++;
  }
  checknext(ls, TK_IN);
  line = ls->linenumber;
  adjust_assign(ls, 4, explist(ls, &e), &e);
  adjustlocalvars(ls, 3); /* start scope for internal variables */
  marktobeclosed(fs);     /* last internal var. must be closed */
  luaK_checkstack(fs, 2); /* extra space to call iterator */
  forbody(ls, base, line, nvars - 3, 1);
}

static void forstat(LexState* ls, int line)
{
  /* forstat -> FOR (fornum | forlist) END */
  FuncState* fs = ls->fs;
  TString* varname;
  BlockCnt bl;
  enterblock(fs, &bl, 1);      /* scope for loop and control variables */
  luaX_next(ls);               /* skip 'for' */
  varname = str_checkname(ls); /* first variable name */
  switch (ls->t.token)
  {
  case '=':
    fornum(ls, varname, line);
    break;
  case ',':
  case TK_IN:
    forlist(ls, varname);
    break;
  default:
    luaX_syntaxerror(ls, lt::or_in_expected);
  }
  check_match(ls, TK_END, TK_FOR, line);
  leaveblock(fs); /* loop scope ('break' jumps to this point) */
}

static void test_then_block(LexState* ls, int* escapelist)
{
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  FuncState* fs = ls->fs;
  int condtrue;
  luaX_next(ls);       /* skip IF or ELSEIF */
  condtrue = cond(ls); /* read condition */
  checknext(ls, TK_THEN);
  block(ls); /* 'then' part */
  if (ls->t.token == TK_ELSE ||
      ls->t.token == TK_ELSEIF)                 /* followed by 'else'/'elseif'? */
    luaK_concat(fs, escapelist, luaK_jump(fs)); /* must jump over it */
  luaK_patchtohere(fs, condtrue);
}

static void ifstat(LexState* ls, int line)
{
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  FuncState* fs = ls->fs;
  int escapelist = NO_JUMP;         /* exit list for finished parts */
  test_then_block(ls, &escapelist); /* IF cond THEN block */
  while (ls->t.token == TK_ELSEIF)
    test_then_block(ls, &escapelist); /* ELSEIF cond THEN block */
  if (testnext(ls, TK_ELSE))
    block(ls); /* 'else' part */
  check_match(ls, TK_END, TK_IF, line);
  luaK_patchtohere(fs, escapelist); /* patch escape list to 'if' end */
}

static void localfunc(LexState* ls)
{
  expdesc b;
  FuncState* fs = ls->fs;
  int fvar = fs->nactvar;              /* function's variable index */
  new_localvar(ls, str_checkname(ls)); /* new local variable */
  adjustlocalvars(ls, 1);              /* enter its scope */
  body(ls, &b, 0, ls->linenumber);     /* function created in next register */
  /* debug information will only see the variable after this point! */
  localdebuginfo(fs, fvar)->startpc = fs->pc;
}

static lu_byte getvarattribute(LexState* ls, lu_byte df)
{
  /* attrib -> ['<' NAME '>'] */
  if (testnext(ls, '<'))
  {
    TString* ts = str_checkname(ls);
    byte_view attr = ts->data.view();
    checknext(ls, '>');
    if (attr == lt::const_kw)
      return RDKCONST; /* read-only variable */
    else if (attr == lt::close_kw)
      return RDKTOCLOSE; /* to-be-closed variable */
    else
      luaK_semerror(ls, lt::unknown_attribute_s, FmtArgs().str(ts->data.view()));
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
  FuncState* fs = ls->fs;
  int toclose = -1; /* index of to-be-closed variable (if any) */
  Vardesc* var;     /* last variable */
  int vidx;         /* index of last variable */
  int nvars = 0;
  int nexps;
  expdesc e;
  /* get prefixed attribute (if any); default is regular local variable */
  lu_byte defkind = getvarattribute(ls, VDKREG);
  do
  {                                              /* for each variable */
    TString* vname = str_checkname(ls);          /* get its name */
    lu_byte kind = getvarattribute(ls, defkind); /* postfixed attribute */
    vidx = new_varkind(ls, vname, kind);         /* predeclare it */
    if (kind == RDKTOCLOSE)
    {                    /* to-be-closed? */
      if (toclose != -1) /* one already present? */
        luaK_semerror(ls, lt::multiple_to_be_closed_variables_in_local_list, FmtArgs());
      toclose = fs->nactvar + nvars;
    }
    nvars++;
  } while (testnext(ls, ','));
  if (testnext(ls, '=')) /* initialization? */
    nexps = explist(ls, &e);
  else
  {
    e.k = VVOID;
    nexps = 0;
  }
  var = getlocalvardesc(fs, vidx); /* retrieve last variable */
  if (nvars == nexps &&            /* no adjustments? */
      var->kind == RDKCONST &&  /* last variable is const? */
      luaK_exp2const(fs, &e, &var->k))
  {                                 /* compile-time constant? */
    var->kind = RDKCTC;          /* variable is a compile-time constant */
    adjustlocalvars(ls, nvars - 1); /* exclude last variable */
    fs->nactvar++;                  /* but count it */
  }
  else
  {
    adjust_assign(ls, nvars, nexps, &e);
    adjustlocalvars(ls, nvars);
  }
  checktoclose(fs, toclose);
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
  if (n == nvars)
  { /* traversed all variables? */
    expdesc e;
    int nexps = explist(ls, &e); /* read list of expressions */
    adjust_assign(ls, nvars, nexps, &e);
  }
  else
  { /* handle variable 'n' */
    FuncState* fs = ls->fs;
    expdesc var;
    TString* varname = getlocalvardesc(fs, firstidx + n)->name;
    buildglobal(ls, varname, &var); /* create global variable in 'var' */
    enterlevel(ls);                 /* control recursion depth */
    initglobal(ls, nvars, firstidx, n + 1, line);
    leavelevel(ls);
    checkglobal(ls, varname, line);
    storevartop(fs, &var);
  }
}

static void globalnames(LexState* ls, lu_byte defkind)
{
  FuncState* fs = ls->fs;
  int nvars = 0;
  int lastidx; /* index of last registered variable */
  do
  { /* for each name */
    TString* vname = str_checkname(ls);
    lu_byte kind = getglobalattribute(ls, defkind);
    lastidx = new_varkind(ls, vname, kind);
    nvars++;
  } while (testnext(ls, ','));
  if (testnext(ls, '=')) /* initialization? */
    initglobal(ls, nvars, lastidx - nvars + 1, 0, ls->linenumber);
  fs->nactvar = ((short)(fs->nactvar + nvars)); /* activate declaration */
}

static void globalstat(LexState* ls)
{
  /* globalstat -> (GLOBAL) attrib '*'
     globalstat -> (GLOBAL) attrib NAME attrib {',' NAME attrib} */
  FuncState* fs = ls->fs;
  /* get prefixed attribute (if any); default is regular global variable */
  lu_byte defkind = getglobalattribute(ls, GDKREG);
  if (!testnext(ls, '*'))
    globalnames(ls, defkind);
  else
  {
    new_varkind(ls, nullptr, defkind);
    fs->nactvar++; /* activate declaration */
  }
}

static void globalfunc(LexState* ls, int line)
{
  /* globalfunc -> (GLOBAL FUNCTION) NAME body */
  expdesc var, b;
  FuncState* fs = ls->fs;
  TString* fname = str_checkname(ls);
  new_varkind(ls, fname, GDKREG); /* declare global variable */
  fs->nactvar++;                  /* enter its scope */
  buildglobal(ls, fname, &var);
  body(ls, &b, 0, ls->linenumber); /* compile and return closure in 'b' */
  checkglobal(ls, fname, line);
  luaK_storevar(fs, &var, &b);
  luaK_fixline(fs, line); /* definition "happens" in the first line */
}

static void globalstatfunc(LexState* ls, int line)
{
  /* stat -> GLOBAL globalfunc | GLOBAL globalstat */
  luaX_next(ls); /* skip 'global' */
  if (testnext(ls, TK_FUNCTION))
    globalfunc(ls, line);
  else
    globalstat(ls);
}

static int funcname(LexState* ls, expdesc* v)
{
  /* funcname -> NAME {fieldsel} [':' NAME] */
  int ismethod = 0;
  singlevar(ls, v);
  while (ls->t.token == '.')
    fieldsel(ls, v);
  if (ls->t.token == ':')
  {
    ismethod = 1;
    fieldsel(ls, v);
  }
  return ismethod;
}

static void funcstat(LexState* ls, int line)
{
  /* funcstat -> FUNCTION funcname body */
  int ismethod;
  expdesc v, b;
  luaX_next(ls); /* skip FUNCTION */
  ismethod = funcname(ls, &v);
  check_readonly(ls, &v);
  body(ls, &b, ismethod, line);
  luaK_storevar(ls->fs, &v, &b);
  luaK_fixline(ls->fs, line); /* definition "happens" in the first line */
}

static void exprstat(LexState* ls)
{
  /* stat -> func | assignment */
  FuncState* fs = ls->fs;
  struct LHS_assign v;
  suffixedexp(ls, &v.v);
  if (ls->t.token == '=' || ls->t.token == ',')
  { /* stat -> assignment ? */
    v.prev = nullptr;
    restassign(ls, &v, 1);
  }
  else
  { /* stat -> func */
    Instruction* inst;
    check_condition(ls, v.v.k == VCALL, lt::syntax_error);
    inst = &((fs)->f->code[(&v.v)->info]);
    ((*inst) = (((*inst) & MASK0(SIZE_C, POS_C)) | ((((Instruction)(1)) << POS_C) & MASK1(SIZE_C, POS_C)))); /* call statement uses no results */
  }
}

static void retstat(LexState* ls)
{
  /* stat -> RETURN [explist] [';'] */
  FuncState* fs = ls->fs;
  expdesc e;
  int nret;                       /* number of values being returned */
  int first = luaY_nvarstack(fs); /* first slot to be returned */
  if (block_follow(ls, 1) || ls->t.token == ';')
    nret = 0; /* return no values */
  else
  {
    nret = explist(ls, &e); /* optional return values */
    if (hasmultret(e.k))
    {
      luaK_setmultret(fs, &e);
      if (e.k == VCALL && nret == 1 && !fs->bl->insidetbc)
      { /* tail call? */
        ((((fs)->f->code[(&e)->info])) = (((((fs)->f->code[(&e)->info])) & MASK0(SIZE_OP, POS_OP)) | ((((Instruction)(OP_TAILCALL)) << POS_OP) & MASK1(SIZE_OP, POS_OP))));
        assert(GETARG_A(((fs)->f->code[(&e)->info])) == luaY_nvarstack(fs));
      }
      nret = LUA_MULTRET; /* return all values */
    }
    else
    {
      if (nret == 1)                     /* only one single value? */
        first = luaK_exp2anyreg(fs, &e); /* can use original slot */
      else
      { /* values must go to the top of the stack */
        luaK_exp2nextreg(fs, &e);
        assert(nret == fs->freereg - first);
      }
    }
  }
  luaK_ret(fs, first, nret);
  testnext(ls, ';'); /* skip optional semicolon */
}

static void statement(LexState* ls)
{
  int line = ls->linenumber; /* may be needed for error messages */
  enterlevel(ls);
  switch (ls->t.token)
  {
  case ';':
  {                /* stat -> ';' (empty statement) */
    luaX_next(ls); /* skip ';' */
    break;
  }
  case TK_IF:
  { /* stat -> ifstat */
    ifstat(ls, line);
    break;
  }
  case TK_WHILE:
  { /* stat -> whilestat */
    whilestat(ls, line);
    break;
  }
  case TK_DO:
  {                /* stat -> DO block END */
    luaX_next(ls); /* skip DO */
    block(ls);
    check_match(ls, TK_END, TK_DO, line);
    break;
  }
  case TK_FOR:
  { /* stat -> forstat */
    forstat(ls, line);
    break;
  }
  case TK_REPEAT:
  { /* stat -> repeatstat */
    repeatstat(ls, line);
    break;
  }
  case TK_FUNCTION:
  { /* stat -> funcstat */
    funcstat(ls, line);
    break;
  }
  case TK_LOCAL:
  {                                /* stat -> localstat */
    luaX_next(ls);                 /* skip LOCAL */
    if (testnext(ls, TK_FUNCTION)) /* local function? */
      localfunc(ls);
    else
      localstat(ls);
    break;
  }
  case TK_GLOBAL:
  { /* stat -> globalstatfunc */
    globalstatfunc(ls, line);
    break;
  }
  case TK_DBCOLON:
  {                /* stat -> label */
    luaX_next(ls); /* skip double colon */
    labelstat(ls, str_checkname(ls), line);
    break;
  }
  case TK_RETURN:
  {                /* stat -> retstat */
    luaX_next(ls); /* skip RETURN */
    retstat(ls);
    break;
  }
  case TK_BREAK:
  { /* stat -> breakstat */
    breakstat(ls, line);
    break;
  }
  case TK_GOTO:
  {                /* stat -> 'goto' NAME */
    luaX_next(ls); /* skip 'goto' */
    gotostat(ls, line);
    break;
  }
  /* FALLTHROUGH */
  default:
  { /* stat -> func | assignment */
    exprstat(ls);
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
  BlockCnt bl;
  Upvaldesc* env;
  open_func(ls, fs, &bl);
  setvararg(fs);          /* main function is always vararg */
  env = allocupvalue(fs); /* ...set environment upvalue */
  env->instack = 1;
  env->idx = 0;
  env->kind = VDKREG;
  env->name = ls->envn;
  luaC_objbarrier(ls->L, fs->f, env->name);
  luaX_next(ls); /* read first token */
  statlist(ls);  /* parse main body */
  check(ls, TK_EOS);
  close_func(ls);
}

LClosure* luaY_parser(lua_State* L, ZIO* z, Table* anchor, MBuffer* buff, Dyndata* dyd, byte_view name, int firstchar)
{
  LexState lexstate;
  FuncState funcstate;
  LClosure* cl;
  lexstate.h = anchor;                    /* table for scanner */
  cl = luaF_newLclosure(L, 1);            /* create main closure */
  assert((cl->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaD_anchorobj(L, anchor, const_cast<GCObject*>(static_cast<const GCObject*>(cl))); /* anchor it in scanner table */
  funcstate.f = cl->p = luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  funcstate.f->source = luaS_createstr(L, name, 0, name.size()); /* create and anchor TString */
  luaC_objbarrier(L, funcstate.f, funcstate.f->source);
  lexstate.buff = buff;
  lexstate.dyd = dyd;
  dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
  luaX_setinput(L, &lexstate, z, funcstate.f->source, firstchar);
  mainfunc(&lexstate, &funcstate);
  assert(!funcstate.prev && funcstate.nups == 1 && !lexstate.fs);
  /* all scopes should be correctly finished */
  assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  return cl;
}
