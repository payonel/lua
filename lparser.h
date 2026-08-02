/*
** $Id: lparser.h $
** Lua Parser
** See Copyright Notice in lua.h
*/

#pragma once

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"

/*
** Expression and variable descriptor.
** Code generation for variables and expressions can be delayed to allow
** optimizations; An 'expdesc' structure describes a potentially-delayed
** variable/expression. It has a description of its "main" value plus a
** list of conditional jumps that can also produce its value (generated
** by short-circuit operators 'and'/'or').
*/

/* kinds of variables/expressions */
typedef enum
{
  VVOID,     /* when 'expdesc' describes the last expression of a list,
                this kind means an empty list (so, no expression) */
  VNIL,      /* constant nil */
  VTRUE,     /* constant true */
  VFALSE,    /* constant false */
  VK,        /* constant in 'k'; info = index of constant in 'k' */
  VKFLT,     /* floating constant; nval = numerical float value */
  VKINT,     /* integer constant; ival = numerical integer value */
  VKSTR,     /* string constant; strval = TString address;
                (string is fixed by the scanner) */
  VNONRELOC, /* expression has its value in a fixed register;
                info = result register */
  VLOCAL,    /* local variable; var.ridx = register index;
                var.vidx = relative index in 'actvar.arr'  */
  VVARGVAR,  /* vararg parameter; var.ridx = register index;
              var.vidx = relative index in 'actvar.arr'  */
  VGLOBAL,   /* global variable;
                info = relative index in 'actvar.arr' (or -1 for
                       implicit declaration) */
  VUPVAL,    /* upvalue variable; info = index of upvalue in 'upvalues' */
  VCONST,    /* compile-time <const> variable;
                info = absolute index in 'actvar.arr'  */
  VINDEXED,  /* indexed variable;
                ind.t = table register;
                ind.idx = key's R index;
                ind.ro = true if it represents a read-only global;
                ind.keystr = if key is a string, index in 'k' of that string;
                             -1 if key is not a string */
  VVARGIND,  /* indexed vararg parameter;
                ind.* as in VINDEXED */
  VINDEXUP,  /* indexed upvalue;
                ind.idx = key's K index;
                ind.* as in VINDEXED */
  VINDEXI,   /* indexed variable with constant integer;
                  ind.t = table register;
                  ind.idx = key's value */
  VINDEXSTR, /* indexed variable with literal string;
                ind.idx = key's K index;
                ind.* as in VINDEXED */
  VJMP,      /* expression is a test/comparison;
                info = pc of corresponding jump instruction */
  VRELOC,    /* expression can put result in any register;
                info = instruction pc */
  VCALL,     /* expression is a function call; info = instruction pc */
  VVARARG    /* vararg expression; info = instruction pc */
} expkind;

constexpr int vkisvar(expkind k)
{
  return VLOCAL <= k && k <= VINDEXSTR;
}
constexpr int vkisindexed(expkind k)
{
  return VINDEXED <= k && k <= VINDEXSTR;
}

struct expdesc
{
  expkind k;
  lua_Integer ival; /* for VKINT */
  lua_Number nval;  /* for VKFLT */
  TString* strval;  /* for VKSTR */
  int info;         /* for generic use */
  struct
  {             /* for indexed variables */
    short idx;  /* index (R or "long" K) */
    lu_byte t;  /* table (register or upvalue) */
    lu_byte ro; /* true if variable is read-only */
    int keystr; /* index in 'k' of string key, or -1 if not a string */
  } ind;
  struct
  {               /* for local variables */
    lu_byte ridx; /* register holding the variable */
    short vidx;   /* index in 'actvar.arr' */
  } var;
  int t; /* patch list of 'exit when true' */
  int f; /* patch list of 'exit when false' */
};

/* kinds of variables */
constexpr int VDKREG = 0; /* regular local */
constexpr int RDKCONST = 1; /* local constant */
constexpr int RDKVAVAR = 2; /* vararg parameter */
constexpr int RDKTOCLOSE = 3; /* to-be-closed */
constexpr int RDKCTC = 4; /* local compile-time constant */
constexpr int GDKREG = 5; /* regular global */
constexpr int GDKCONST = 6; /* global constant */

/* description of an active variable */
struct Vardesc
{
  TValue k;      /* constant value (if it is a compile-time constant) */
  lu_byte kind;
  lu_byte ridx;  /* register holding the variable */
  short pidx;    /* index of the variable in the Proto's 'locvars' array */
  TString* name; /* variable name */
};

/* variables that live in registers */
static inline int varinreg(const Vardesc* v)
{
  return v->kind <= RDKTOCLOSE;
}

/* test for global variables */
static inline int varglobal(const Vardesc* v)
{
  return v->kind >= GDKREG;
}

/* description of pending goto statements and label statements */
struct Labeldesc
{
  TString* name; /* label identifier */
  int pc;        /* position in code */
  int line;      /* line where it appeared */
  short nactvar; /* number of active variables in that position */
  lu_byte close; /* true for goto that escapes upvalues */
};

/* list of labels or gotos */
struct Labellist
{
  Labeldesc* arr; /* array */
  int n;          /* number of entries in use */
  int size;       /* array size */
};

/* dynamic structures used by the parser */
struct Dyndata
{
  struct
  { /* list of all active local variables */
    Vardesc* arr;
    int n;
    int size;
  } actvar;
  Labellist gt;    /* list of pending gotos */
  Labellist label; /* list of active labels */
};

/* control of blocks */
struct BlockCnt; /* defined in lparser.c */

/* state needed to generate code for a given function */
struct FuncState
{
  Proto* f;               /* current function header */
  struct FuncState* prev; /* enclosing function */
  struct LexState* ls;    /* lexical state */
  struct BlockCnt* bl;    /* chain of current blocks */
  Table* kcache;          /* cache for reusing constants */
  int pc;                 /* next position to code (equivalent to 'ncode') */
  int lasttarget;         /* 'label' of last 'jump label' */
  int previousline;       /* last line that was saved in 'lineinfo' */
  int nk;                 /* number of elements in 'k' */
  int np;                 /* number of elements in 'p' */
  int nabslineinfo;       /* number of elements in 'abslineinfo' */
  int firstlocal;         /* index of first local var (in Dyndata array) */
  int firstlabel;         /* index of first label (in 'dyd->label->arr') */
  short ndebugvars;       /* number of elements in 'f->locvars' */
  short nactvar;          /* number of active variable declarations */
  lu_byte nups;           /* number of upvalues */
  lu_byte freereg;        /* first free register */
  lu_byte iwthabs;        /* instructions issued since last absolute line info */
  lu_byte needclose;      /* function needs to close upvalues when returning */
};

LUAI_FUNC lu_byte luaY_nvarstack(FuncState* fs);
LUAI_FUNC void luaY_checklimit(FuncState* fs, int v, int l, const InternalString* what);
LUAI_FUNC LClosure* luaY_parser(lua_State* L, ZIO* z, Table* anchor, ByteBuilder* buff, Dyndata* dyd, std::string_view name, int firstchar);
