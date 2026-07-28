/*
** $Id: lobject.h $
** Type definitions for Lua objects
** See Copyright Notice in lua.h
*/

#pragma once

#include <string>

#include "llimits.h"
#include "lua.h"

/*
** Extra types for collectable non-values
*/
constexpr int LUA_TUPVAL = LUA_NUMTYPES; /* upvalues */
constexpr int LUA_TPROTO = (LUA_NUMTYPES + 1); /* function prototypes */
constexpr int LUA_TDEADKEY = (LUA_NUMTYPES + 2); /* removed keys in tables */

/*
** number of all possible types (including LUA_TNONE but excluding DEADKEY)
*/
constexpr int LUA_TOTALTYPES = (LUA_TPROTO + 2);

/*
** tags for Tagged Values have the following use of bits:
** bits 0-3: actual tag (a LUA_T* constant)
** bits 4-5: variant bits
** bit 6: whether value is collectable
*/

/* add variant bits to a type */
/* add variant bits to a basic type tag (usable in constant expressions
** and case labels) */
constexpr lu_byte makevariant(int t, int v)
{
  return ((lu_byte)(t | (v << 4)));
}

/*
** The two storage columns of a Lua value: a 64-bit primitive payload
** and a GC reference. Which one is meaningful — and how the payload's
** bits are interpreted — is dictated entirely by the enclosing tag
** ('tt_' / 'key_tt' / array-part tag byte); the other column is
** ignored (a numeric store leaves 'ref' stale — never read, because
** the tag says non-collectable). This is the row form of the managed
** representation (parallel primitive/reference/tag arrays — the
** Truffle frame design): 'prim' transposes to the long[] column
** (doubles as raw bits via bit_cast — Java doubleToRawLongBits),
** 'ref' to the Object[] column. Port notes: managed setters null the
** ref column on primitive stores (GC hygiene the C++ GC doesn't need,
** since it consults the tag before tracing); light userdata and
** C functions ride 'prim' here as pointer bits, but map to the ref
** column (an Object) and the registry identifier respectively in the
** managed port.
*/
typedef struct Value
{
  lua_Integer prim;     /* integers, float bits, light udata, C functions */
  struct GCObject* ref; /* collectable objects */
} Value;

/*
** Tagged Values. This is the basic representation of values in Lua:
** an actual value plus a tag with its type.
*/

typedef struct TValue
{
  Value value_;
  lu_byte tt_;
} TValue;

/*
** Layout of a type tag byte (tt_ / tt):
**   bits 0-3: basic type (LUA_TNIL, LUA_TNUMBER, ...)
**   bits 4-5: variant bits (e.g. LUA_VNUMINT vs LUA_VNUMFLT)
**   bit 6:    BIT_ISCOLLECTABLE
*/

/* selects the basic type (bits 0-3) of a tag — was the novariant macro */
constexpr int NOVARIANT_MASK = 0x0F;

/* selects type + variant bits (bits 0-5) of a tag — was the withvariant macro */
constexpr int WITHVARIANT_MASK = 0x3F;

/* Macros for internal tests */

/* collectable object has the same tag as the original value */

/*
** Entries in a Lua stack: plain values. The list of to-be-closed
** variables lives in a separate array in 'lua_State' ('tbc'), not
** threaded through the stack.
*/
typedef TValue StackValue;

/* index to stack elements (integer offset from stack base) */
typedef size_t StkId;

/*
** {==================================================================
** Nil
** ===================================================================
*/

/* Standard nil */
constexpr lu_byte LUA_VNIL = makevariant(LUA_TNIL, 0);

/* Empty slot (which might be different from a slot containing nil) */
constexpr lu_byte LUA_VEMPTY = makevariant(LUA_TNIL, 1);

/* Value returned for a key not found in a table (absent key) */
constexpr lu_byte LUA_VABSTKEY = makevariant(LUA_TNIL, 2);

/* Special variant to signal that a fast get is accessing a non-table */
constexpr lu_byte LUA_VNOTABLE = makevariant(LUA_TNIL, 3);

/*
** Macro to test the result of a table access. Formally, it should
** distinguish between LUA_VEMPTY/LUA_VABSTKEY/LUA_VNOTABLE and
** other tags. As currently nil is equivalent to LUA_VEMPTY, it is
** simpler to just test whether the value is nil.
*/

/*
** macro to detect non-standard nils (used only in assertions)
*/

/*
** By default, entries with any kind of nil are considered empty.
** (In any definition, values associated with absent keys must also
** be accepted as empty.)
*/

/* initializer for a value corresponding to an absent key */

/* }================================================================== */

/*
** {==================================================================
** Booleans
** ===================================================================
*/

constexpr lu_byte LUA_VFALSE = makevariant(LUA_TBOOLEAN, 0);
constexpr lu_byte LUA_VTRUE = makevariant(LUA_TBOOLEAN, 1);

/* }================================================================== */

/*
** {==================================================================
** Threads
** ===================================================================
*/

constexpr lu_byte LUA_VTHREAD = makevariant(LUA_TTHREAD, 0);

/* }================================================================== */

/*
** {==================================================================
** Collectable Objects
** ===================================================================
*/

/*
** Common Header for all collectable objects (in macro form, to be
** included in other objects)
*/
/* Common type for all collectable objects */
typedef struct GCObject
{
  struct GCObject* next;
  lu_byte tt;
  lu_byte marked;
} GCObject;

/* Bit mark for collectable types */
constexpr int BIT_ISCOLLECTABLE = (1 << 6);

/* mark a tag as collectable (usable in constant expressions and case
** labels) */
constexpr lu_byte ctb(lu_byte t)
{
  return ((lu_byte)(t | BIT_ISCOLLECTABLE));
}

/* }================================================================== */

/*
** {==================================================================
** Numbers
** ===================================================================
*/

/* Variant tags for numbers */
constexpr lu_byte LUA_VNUMINT = makevariant(LUA_TNUMBER, 0); /* integer numbers */
constexpr lu_byte LUA_VNUMFLT = makevariant(LUA_TNUMBER, 1); /* float numbers */

/* }================================================================== */

/*
** {==================================================================
** Strings
** ===================================================================
*/

/* Variant tags for strings */
constexpr lu_byte LUA_VSTRING = makevariant(LUA_TSTRING, 0);
/* Keep old names for now during transition */
constexpr lu_byte LUA_VSHRSTR = LUA_VSTRING;
constexpr lu_byte LUA_VLNGSTR = LUA_VSTRING;

/*
** Header for a string value.
*/
typedef struct TString : GCObject
{
  lu_byte extra;       /* reserved words for short strings; "has hash" for longs */
  unsigned int hash;   /* cached hash value */
  std::string content; /* string data */
} TString;

static l_forceinline const char* getstr(const TString* ts)
{
  return ts->content.c_str();
}
static l_forceinline size_t tsslen(const TString* ts)
{
  return ts->content.size();
}

/* }================================================================== */

/*
** {==================================================================
** Userdata
** ===================================================================
*/

/*
** Light userdata should be a variant of userdata, but for compatibility
** reasons they are also different types.
*/
constexpr lu_byte LUA_VLIGHTUSERDATA = makevariant(LUA_TLIGHTUSERDATA, 0);

constexpr lu_byte LUA_VUSERDATA = makevariant(LUA_TUSERDATA, 0);

/*
** Header for userdata; the user values and the binary data area are
** separately allocated luaM arrays (managed shape: an object holding
** an array reference and a byte-array reference).
** Userdata with no user values do not need to be gray during GC, and
** therefore do not need a 'gclist'; the code never accesses 'gclist'
** on userdata with no user values.
*/
typedef struct Udata : GCObject
{
  unsigned short nuvalue; /* number of user values */
  size_t len;             /* number of bytes in 'data' */
  struct Table* metatable;
  GCObject* gclist;
  TValue* uv; /* user values (luaM array of 'nuvalue' elements) */
  char* data; /* user memory (luaM block of 'len' bytes) */
} Udata;

/* get the address of the memory block inside 'Udata' */
static l_forceinline char* getudatamem(Udata* u)
{
  return u->data;
}

/*
** Allocated size of the data block for a userdata of 'len' bytes.
** A zero-size userdata still allocates one byte: the C API promises a
** non-null data pointer (lua_touserdata distinguishes userdata by it).
** Used by allocation, freeing, and GC size accounting — the three must
** agree. (Managed targets drop this: a zero-length array is a valid
** non-null object there.)
*/
static l_forceinline size_t udatadatasize(size_t len)
{
  return len == 0 ? 1 : len;
}

/* }================================================================== */

/*
** {==================================================================
** Prototypes
** ===================================================================
*/

constexpr lu_byte LUA_VPROTO = makevariant(LUA_TPROTO, 0);

typedef l_uint32 Instruction;

/*
** Description of an upvalue for function prototypes
*/
typedef struct Upvaldesc
{
  TString* name;   /* upvalue name (for debug information) */
  lu_byte instack; /* whether it is in stack (register) */
  lu_byte idx;     /* index of upvalue (in stack or in outer function's list) */
  lu_byte kind;    /* kind of corresponding variable */
} Upvaldesc;

/*
** Description of a local variable for function prototypes
** (used for debug information)
*/
typedef struct LocVar
{
  TString* varname;
  int startpc; /* first point where variable is active */
  int endpc;   /* first point where variable is dead */
} LocVar;

/*
** Associates the absolute line source for a given instruction ('pc').
** The array 'lineinfo' gives, for each instruction, the difference in
** lines from the previous instruction. When that difference does not
** fit into a byte, Lua saves the absolute line for that instruction.
** (Lua also saves the absolute line periodically, to speed up the
** computation of a line number: we can use binary search in the
** absolute-line array, but we must traverse the 'lineinfo' array
** linearly to compute a line.)
*/
typedef struct AbsLineInfo
{
  int pc;
  int line;
} AbsLineInfo;

/*
** Flags in Prototypes
*/
constexpr int PF_VAHID = 1; /* function has hidden vararg arguments */
constexpr int PF_VATAB = 2; /* function has vararg table */
constexpr int PF_FIXED = 4; /* prototype has parts in fixed memory */

/* a vararg function either has hidden args. or a vararg table */

/*
** mark that a function needs a vararg table. (The flag PF_VAHID will
** be cleared later.)
*/

/*
** Function Prototypes
*/
typedef struct Proto : GCObject
{
  lu_byte numparams; /* number of fixed (named) parameters */
  lu_byte flag;
  lu_byte maxstacksize; /* number of registers needed by this function */
  int sizeupvalues;     /* size of 'upvalues' */
  int sizek;            /* size of 'k' */
  int sizecode;
  int sizelineinfo;
  int sizep; /* size of 'p' */
  int sizelocvars;
  int sizeabslineinfo;      /* size of 'abslineinfo' */
  int linedefined;          /* debug information  */
  int lastlinedefined;      /* debug information  */
  TValue* k;                /* constants used by the function */
  Instruction* code;        /* opcodes */
  struct Proto** p;         /* functions defined inside the function */
  Upvaldesc* upvalues;      /* upvalue information */
  ls_byte* lineinfo;        /* information about source lines (debug information) */
  AbsLineInfo* abslineinfo; /* idem */
  LocVar* locvars;          /* information about local variables (debug information) */
  TString* source;          /* used for debug information */
  GCObject* gclist;
} Proto;

static l_forceinline int isvararg(const Proto* p)
{
  return p->flag & (PF_VAHID | PF_VATAB);
}

static l_forceinline void needvatab(Proto* p)
{
  p->flag = (lu_byte)(p->flag | PF_VATAB);
}

/* }================================================================== */

/*
** {==================================================================
** Functions
** ===================================================================
*/

constexpr lu_byte LUA_VUPVAL = makevariant(LUA_TUPVAL, 0);

/* Variant tags for functions */
constexpr lu_byte LUA_VLCL = makevariant(LUA_TFUNCTION, 0); /* Lua closure */
constexpr lu_byte LUA_VLCF = makevariant(LUA_TFUNCTION, 1); /* light C function */
constexpr lu_byte LUA_VCCL = makevariant(LUA_TFUNCTION, 2); /* C closure */

/*
** Upvalues for Lua closures
*/
typedef struct UpVal : GCObject
{
  struct UpVal* opennext;    /* next in open list (nullptr when closed) */
  struct UpVal** previous;   /* back-pointer in open list (nullptr when closed) */
  struct lua_State* owner;   /* thread whose stack holds the value (when open) */
  StkId stkid;              /* stack index in 'owner' when open */
  TValue value;             /* embedded storage for closed-over value */
} UpVal;

typedef struct Closure : GCObject
{
  lu_byte nupvalues;
  GCObject* gclist;
} Closure;

typedef struct CClosure : Closure
{
  lua_CFunction f;
  TValue* upvalue; /* upvalues (luaM array of 'nupvalues' elements) */
} CClosure;

/* total bytes of a C closure (header + upvalue array), for GC accounting */
constexpr size_t sizeCclosure(int n)
{
  return sizeof(CClosure) + sizeof(TValue) * (unsigned int)n;
}

typedef struct LClosure : Closure
{
  struct Proto* p;
  UpVal** upvals; /* upvalues (luaM array of 'nupvalues' elements) */
} LClosure;

/* total bytes of a Lua closure (header + upvals array), for GC accounting */
constexpr size_t sizeLclosure(int n)
{
  return sizeof(LClosure) + sizeof(UpVal*) * (unsigned int)n;
}

/* }================================================================== */

/*
** {==================================================================
** Tables
** ===================================================================
*/

constexpr lu_byte LUA_VTABLE = makevariant(LUA_TTABLE, 0);

/*
** Nodes for Hash tables: A pack of two TValue's (key-value pairs)
** plus a 'next' field to link colliding entries.
*/
typedef struct Node
{
  TValue val;       /* node value */
  lu_byte key_tt;   /* key type */
  int next;         /* for chaining */
  Value key_val;    /* key value */
} Node;

typedef struct Table : GCObject
{
  lu_byte flags;      /* 1<<p means tagmethod(p) is not present */
  lu_byte lsizenode;  /* log2 of number of slots of 'node' array */
  unsigned int asize; /* number of slots in 'array' array */
  Value* array;       /* array part */
  Node* node;
  int lastfree;       /* last free position in hash (node index), scans backward */
  struct Table* metatable;
  GCObject* gclist;
} Table;

/*
** Dead keys in tables have the tag DEADKEY but keep their original
** gcvalue. This distinguishes them from regular keys but allows them to
** be found when searched in a special way. ('next' needs that to find
** keys removed from a table during a traversal.)
*/

/* }================================================================== */

/*
** 'module' operation for hashing (size is always a power of 2)
*/
static l_forceinline unsigned int lmod(lua_Unsigned s, unsigned int size)
{
  assert((size & (size - 1)) == 0); /* size must be a power of 2 */
  return (unsigned int)s & (size - 1);
}

constexpr unsigned int twoto(int x)
{
  return 1u << x;
}
static l_forceinline unsigned int sizenode(const Table* t)
{
  return twoto(t->lsizenode);
}

/*
** {==================================================================
** Get functions — typed reads out of TValues
**
** The single seam over the value representation: every read of a
** value's payload goes through one of these. The managed port swaps
** the representation (parallel primitive/reference/tag arrays — the
** Truffle frame design) behind these exact signatures; nothing
** outside this section and the Set functions below may touch the
** representation directly. Each accessor asserts the tag it reads
** under — the same check the managed slot API performs — and the
** GC-object accessors also cross-check the referent's own 'tt'
** (in Java, that second check is the checked cast itself).
** ===================================================================
*/

static l_forceinline lua_Integer ivalue(const TValue* o)
{
  assert(o->tt_ == LUA_VNUMINT);
  return o->value_.prim;
}

static l_forceinline lua_Number fltvalue(const TValue* o)
{
  assert(o->tt_ == LUA_VNUMFLT);
  return std::bit_cast<lua_Number>(o->value_.prim);
}

/* number (integer or float) coerced to a float */
static l_forceinline lua_Number nvalue(const TValue* o)
{
  assert(o->tt_ == LUA_VNUMINT || o->tt_ == LUA_VNUMFLT);
  return (o->tt_ == LUA_VNUMINT) ? ((lua_Number)(o->value_.prim))
                                 : std::bit_cast<lua_Number>(o->value_.prim);
}

static l_forceinline void* pvalue(const TValue* o)
{
  assert(o->tt_ == LUA_VLIGHTUSERDATA);
  return reinterpret_cast<void*>(o->value_.prim);
}

static l_forceinline lua_CFunction fvalue(const TValue* o)
{
  assert(o->tt_ == LUA_VLCF);
  return reinterpret_cast<lua_CFunction>(o->value_.prim);
}

static l_forceinline GCObject* gcvalue(const TValue* o)
{
  assert(o->tt_ & BIT_ISCOLLECTABLE);
  return o->value_.ref;
}

static l_forceinline TString* tsvalue(const TValue* o)
{
  assert(o->tt_ == ctb(LUA_VSHRSTR) || o->tt_ == ctb(LUA_VLNGSTR));
  assert((o->value_.ref->tt & NOVARIANT_MASK) == LUA_TSTRING);
  return static_cast<TString*>(o->value_.ref);
}

static l_forceinline Table* hvalue(const TValue* o)
{
  assert(o->tt_ == ctb(LUA_VTABLE));
  assert(o->value_.ref->tt == LUA_VTABLE);
  return static_cast<Table*>(o->value_.ref);
}

static l_forceinline Udata* uvalue(const TValue* o)
{
  assert(o->tt_ == ctb(LUA_VUSERDATA));
  assert(o->value_.ref->tt == LUA_VUSERDATA);
  return static_cast<Udata*>(o->value_.ref);
}

/* thvalue is defined in lstate.h (needs the complete lua_State) */

static l_forceinline Closure* clvalue(const TValue* o)
{
  assert(o->tt_ == ctb(LUA_VLCL) || o->tt_ == ctb(LUA_VCCL));
  assert(o->value_.ref->tt == LUA_VLCL || o->value_.ref->tt == LUA_VCCL);
  return static_cast<Closure*>(o->value_.ref);
}

static l_forceinline LClosure* clLvalue(const TValue* o)
{
  assert(o->tt_ == ctb(LUA_VLCL));
  assert(o->value_.ref->tt == LUA_VLCL);
  return static_cast<LClosure*>(o->value_.ref);
}

static l_forceinline CClosure* clCvalue(const TValue* o)
{
  assert(o->tt_ == ctb(LUA_VCCL));
  assert(o->value_.ref->tt == LUA_VCCL);
  return static_cast<CClosure*>(o->value_.ref);
}

static l_forceinline Proto* getproto(const TValue* o)
{
  return clLvalue(o)->p;
}

/* node keys: the same seam over a Node's key representation */

static l_forceinline GCObject* gckey(const Node* n)
{
  /* dead keys keep their reference for identity comparison in 'equalkey' */
  assert((n->key_tt & BIT_ISCOLLECTABLE) || n->key_tt == LUA_TDEADKEY);
  return n->key_val.ref;
}

static l_forceinline TString* keystrval(const Node* n)
{
  assert(n->key_tt == ctb(LUA_VSHRSTR));
  assert((n->key_val.ref->tt & NOVARIANT_MASK) == LUA_TSTRING);
  return static_cast<TString*>(n->key_val.ref);
}

static l_forceinline lua_Integer keyival(const Node* n)
{
  assert(n->key_tt == LUA_VNUMINT);
  return n->key_val.prim;
}

static l_forceinline lua_Number keyfltval(const Node* n)
{
  assert(n->key_tt == LUA_VNUMFLT);
  return std::bit_cast<lua_Number>(n->key_val.prim);
}

static l_forceinline void* keypval(const Node* n)
{
  assert(n->key_tt == LUA_VLIGHTUSERDATA);
  return reinterpret_cast<void*>(n->key_val.prim);
}

static l_forceinline lua_CFunction keyfval(const Node* n)
{
  assert(n->key_tt == LUA_VLCF);
  return reinterpret_cast<lua_CFunction>(n->key_val.prim);
}

/* }================================================================== */

/*
** {==================================================================
** Set functions — assign values into TValues
** ===================================================================
*/

/*
** Any value being manipulated by the program either is non
** collectable, or the collectable object has the right tag
** and it is not dead. The option 'L == nullptr' allows other
** callers to use it where L is not available.
** 'luaO_checkliveness' performs its checks via asserts and always
** returns 1, so it can be called as 'assert(luaO_checkliveness(...))'.
*/
LUAI_FUNC int luaO_checkliveness(struct lua_State* L, const TValue* obj);

static l_forceinline void checkliveness(struct lua_State* L, const TValue* obj)
{
  assert(luaO_checkliveness(L, obj));
}

/* main value copy (from 'obj2' to 'obj1') */
static l_forceinline void setobj(struct lua_State* L, TValue* obj1, const TValue* obj2)
{
  obj1->value_ = obj2->value_;
  obj1->tt_ = obj2->tt_;
  checkliveness(L, obj1);
  assert(!((((obj1)->tt_ & NOVARIANT_MASK) == LUA_TNIL) && !((obj1)->tt_ == LUA_VNIL)));
}

static l_forceinline void setfltvalue(TValue* obj, lua_Number x)
{
  obj->value_.prim = std::bit_cast<lua_Integer>(x);
  obj->tt_ = LUA_VNUMFLT;
}

/* change value of a float, keeping its tag */
static l_forceinline void chgfltvalue(TValue* obj, lua_Number x)
{
  assert(obj->tt_ == LUA_VNUMFLT);
  obj->value_.prim = std::bit_cast<lua_Integer>(x);
}

static l_forceinline void setivalue(TValue* obj, lua_Integer x)
{
  obj->value_.prim = x;
  obj->tt_ = LUA_VNUMINT;
}

/* change value of an integer, keeping its tag */
static l_forceinline void chgivalue(TValue* obj, lua_Integer x)
{
  assert(obj->tt_ == LUA_VNUMINT);
  obj->value_.prim = x;
}

static l_forceinline void setpvalue(TValue* obj, void* x)
{
  obj->value_.prim = reinterpret_cast<lua_Integer>(x);
  obj->tt_ = LUA_VLIGHTUSERDATA;
}

static l_forceinline void setfvalue(TValue* obj, lua_CFunction x)
{
  obj->value_.prim = reinterpret_cast<lua_Integer>(x);
  obj->tt_ = LUA_VLCF;
}

static l_forceinline void setgcovalue(struct lua_State* L, TValue* obj, GCObject* x)
{
  (void) L;
  obj->value_.ref = x;
  obj->tt_ = ctb(x->tt);
}

static l_forceinline void setsvalue(struct lua_State* L, TValue* obj, TString* x)
{
  assert((x->tt & NOVARIANT_MASK) == LUA_TSTRING);
  obj->value_.ref = x;
  obj->tt_ = ctb(x->tt);
  checkliveness(L, obj);
}

static l_forceinline void setuvalue(struct lua_State* L, TValue* obj, Udata* x)
{
  assert(x->tt == LUA_VUSERDATA);
  obj->value_.ref = x;
  obj->tt_ = ctb(LUA_VUSERDATA);
  checkliveness(L, obj);
}

static l_forceinline void sethvalue(struct lua_State* L, TValue* obj, Table* x)
{
  assert(x->tt == LUA_VTABLE);
  obj->value_.ref = x;
  obj->tt_ = ctb(LUA_VTABLE);
  checkliveness(L, obj);
}

static l_forceinline void setclLvalue(struct lua_State* L, TValue* obj, LClosure* x)
{
  assert(x->tt == LUA_VLCL);
  obj->value_.ref = x;
  obj->tt_ = ctb(LUA_VLCL);
  checkliveness(L, obj);
}

static l_forceinline void setclCvalue(struct lua_State* L, TValue* obj, CClosure* x)
{
  assert(x->tt == LUA_VCCL);
  obj->value_.ref = x;
  obj->tt_ = ctb(LUA_VCCL);
  checkliveness(L, obj);
}

/* setthvalue lives in lstate.h — it needs the complete lua_State type */

/* copy a value into a node key */
static l_forceinline void setnodekey(Node* node, const TValue* obj)
{
  node->key_val = obj->value_;
  node->key_tt = obj->tt_;
}

/* copy a value from a node key */
static l_forceinline void getnodekey(struct lua_State* L, TValue* obj, const Node* node)
{
  obj->value_ = node->key_val;
  obj->tt_ = node->key_tt;
  checkliveness(L, obj);
}

/* }================================================================== */

/* size of buffer for 'luaO_utf8esc' function */
constexpr int UTF8BUFFSZ = 8;

/*
** {==================================================================
** Arguments for 'luaO_pushvfstring': the replacement for C 'va_list'.
** A fixed array of typed values, filled by explicit typed appends at
** the call site, e.g.:
**   luaG_runerror(L, "too many %s (limit is %d)",
**                 FmtArgs().str(what).num(limit));
** Each append records which format directive it answers ('s', 'c',
** 'd', 'I', 'f', 'p', 'U'); the formatting loop asserts that
** directives and appended arguments match in type and count.
** ===================================================================
*/

/* maximum number of arguments to 'luaO_pushvfstring' */
constexpr int MAXFMTARGS = 8;

/*
** One argument: a directive tag plus one field per possible type
** (plain struct, no union; only the field selected by 'tag' is read)
*/
struct FmtArg
{
  char tag;      /* directive this argument answers */
  const char* s; /* '%s' */
  lua_Integer i; /* '%c', '%d', '%I', '%U' */
  lua_Number n;  /* '%f' */
  const void* p; /* '%p' */
};

struct FmtArgs
{
  int n = 0;              /* number of arguments appended */
  FmtArg arg[MAXFMTARGS]; /* only 'arg[0..n-1]' are meaningful */
  FmtArgs& str(const char* s);   /* '%s': a zero-terminated string */
  FmtArgs& chr(int c);           /* '%c': an 'int' as a character */
  FmtArgs& num(int d);           /* '%d': an 'int' */
  FmtArgs& lint(lua_Integer i);  /* '%I': a 'lua_Integer' */
  FmtArgs& flt(lua_Number f);    /* '%f': a 'lua_Number' */
  FmtArgs& ptr(const void* p);   /* '%p': a pointer */
  FmtArgs& utf8(unsigned long u); /* '%U': a codepoint as UTF-8 */
};

/* }================================================================== */

LUAI_FUNC int luaO_utf8esc(char* buff, l_uint32 x);
LUAI_FUNC lu_byte luaO_ceillog2(unsigned int x);
LUAI_FUNC lu_byte luaO_codeparam(unsigned int p);
LUAI_FUNC l_mem luaO_applyparam(lu_byte p, l_mem x);

LUAI_FUNC int luaO_rawarith(lua_State* L, int op, const TValue* p1,
                            const TValue* p2, TValue* res);
LUAI_FUNC void luaO_arith(lua_State* L, int op, const TValue* p1,
                          const TValue* p2, StkId res);
LUAI_FUNC size_t luaO_str2num(const char* s, TValue* o);
LUAI_FUNC unsigned luaO_tostringbuff(const TValue* obj, char* buff);
LUAI_FUNC lu_byte luaO_hexavalue(int c);
LUAI_FUNC void luaO_tostring(lua_State* L, TValue* obj);
LUAI_FUNC const char* luaO_pushvfstring(lua_State* L, const char* fmt,
                                        const FmtArgs& args);
LUAI_FUNC const char* luaO_pushfstring(lua_State* L, const char* fmt,
                                       const FmtArgs& args);
LUAI_FUNC const char* luaO_pushfstring(lua_State* L, const char* fmt);
LUAI_FUNC void luaO_chunkid(char* out, const char* source, size_t srclen);
