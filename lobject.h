/*
** $Id: lobject.h $
** Type definitions for Lua objects
** See Copyright Notice in lua.h
*/

#pragma once

#include <optional>
#include <span>

#include "llimits.h"
#include "lmem.h"
#include "internal_string.h"
#include "lua.h"
#include "bytes.h"

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
constexpr lu_byte makevariant(int t, int v)
{
  return ((lu_byte)(t | (v << 4)));
}

struct Value
{
  lua_Integer prim;
  struct GCObject* ref; /* collectable objects; also light userdata (in Java, ref becomes Object) */
};

/*
** Tagged Values. This is the basic representation of values in Lua:
** an actual value plus a tag with its type.
*/

struct TValue;
static l_forceinline TValue getobj(const TValue* o);
static l_forceinline void copyobj(TValue* dst, const TValue* src);

struct TValue
{
  Value value_;
  lu_byte tt_;
  TValue() = default;
  constexpr TValue(Value v, lu_byte t) : value_(v), tt_(t) {}
  TValue(TValue&&) = default;
  TValue& operator=(TValue&&) = default;

private:
  TValue(const TValue&) = default;
  TValue& operator=(const TValue&) = default;
  friend TValue getobj(const TValue* o);
  friend void copyobj(TValue* dst, const TValue* src);
};

static l_forceinline TValue getobj(const TValue* o)
{
  return *o;
}

static l_forceinline void copyobj(TValue* dst, const TValue* src)
{
  *dst = *src;
}


constexpr int NOVARIANT_MASK = 0x0F;

constexpr int WITHVARIANT_MASK = 0x3F;

/* Macros for internal tests */

/* collectable object has the same tag as the original value */


typedef ptrdiff_t StkId;

constexpr StkId NOSLOT = -1;

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
struct GCObject
{
  struct GCObject* next;
  lu_byte tt;
  lu_byte marked;
};

struct GCListed : GCObject
{
  GCObject* gclist;
};

/* Bit mark for collectable types */
constexpr int BIT_ISCOLLECTABLE = (1 << 6);

constexpr lu_byte ctb(lu_byte t)
{
  return (t | BIT_ISCOLLECTABLE);
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
constexpr lu_byte LUA_VSHRSTR = LUA_VSTRING;
constexpr lu_byte LUA_VLNGSTR = LUA_VSTRING;

/*
** Header for a string value.
*/
struct TString : GCObject
{
  lu_byte extra;
  lu_byte hashed;
  unsigned int hash;
  TString* hnext;
  InternalString data;

  static TString* luaS_createstr(lua_State* L, byte_view str);
  static TString* luaS_take(lua_State* L, InternalString&& str);
  static TString* luaS_takeexternal(lua_State* L, InternalString&& str);

  void luaS_remove(lua_State* L);
  void luaS_forcehash();

private:
  static TString* createstrobj(lua_State* L, InternalString&& str, size_t h,
                                int hashed, int external);
};

constexpr lu_byte STREXTRA_EXTERNAL = 0xFF;

static l_forceinline int strisexternal(const TString* ts)
{
  return ts->extra == STREXTRA_EXTERNAL;
}

static l_forceinline size_t tsslen(const TString* ts)
{
  return ts->data.size();
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

struct Table;
struct Udata : GCListed
{
  Table* metatable;
  std::span<TValue> uv;
  byte_span data;
  bool zb;

  static Udata* luaS_newudata(lua_State* L, size_t s, int nuvalue);
};

/* get the address of the memory block inside 'Udata' */
static l_forceinline char* getudatamem(Udata* u)
{
  return u->data.data();
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
struct Upvaldesc
{
  TString* name;   /* upvalue name (for debug information) */
  lu_byte instack; /* whether it is in stack (register) */
  int idx;     /* index of upvalue (in stack or in outer function's list) */
  lu_byte kind;    /* kind of corresponding variable */
};

/*
** Description of a local variable for function prototypes
** (used for debug information)
*/
struct LocVar
{
  TString* varname;
  int startpc; /* first point where variable is active */
  int endpc;   /* first point where variable is dead */
};

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
struct AbsLineInfo
{
  int pc;
  int line;
};

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
struct Proto : GCListed
{
  int numparams; /* number of fixed (named) parameters */
  lu_byte flag;
  int maxstacksize; /* number of registers needed by this function */
  int linedefined;          /* debug information  */
  int lastlinedefined;      /* debug information  */
  std::span<TValue> k;                /* constants used by the function */
  std::span<Instruction> code;        /* opcodes */
  std::span<Proto*> p;         /* functions defined inside the function */
  std::span<Upvaldesc> upvalues;      /* upvalue information */
  std::span<ls_byte> lineinfo;        /* information about source lines (debug information) */
  std::span<AbsLineInfo> abslineinfo; /* idem */
  std::span<LocVar> locvars;          /* information about local variables (debug information) */
  TString* source;          /* used for debug information */

  static Proto* luaF_newproto(lua_State* L);

  l_mem luaF_protosize();
  void luaF_freeproto(lua_State* L);
  const InternalString* luaF_getlocalname(int local_number, int pc) const;
};

static l_forceinline int isvararg(const Proto* p)
{
  return p->flag & (PF_VAHID | PF_VATAB);
}

static l_forceinline void needvatab(Proto* p)
{
  p->flag = (p->flag | PF_VATAB);
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
struct UpVal : GCObject
{
  struct UpVal* opennext;
  struct UpVal* prevopen;
  struct lua_State* owner;
  StkId stkid;
  TValue value;

  static UpVal* luaF_findupval(lua_State* L, StkId level);
  void luaF_unlinkupval();

private:
  static UpVal* newupval(lua_State* L, StkId level, UpVal* prev);
};

struct Closure : GCListed
{
};

struct CClosure : Closure
{
  lua_CFunction f;
  std::span<TValue> upvalue;

  static CClosure* luaF_newCclosure(lua_State* L, int nupvals);
};

constexpr int sizeCclosure(int n)
{
  return cost::CClosure + cost::TValue * n;
}

struct LClosure : Closure
{
  struct Proto* p;
  std::span<UpVal*> upvals;

  static LClosure* luaF_newLclosure(lua_State* L, int nupvals);
  void luaF_initupvals(lua_State* L);
};

constexpr int sizeLclosure(int n)
{
  return cost::LClosure + cost::Ref * n;
}

struct CFuncInfo : GCObject
{
  lua_CFunction fptr;
  std::optional<InternalString> name;

  CFuncInfo()
    : GCObject{nullptr, LUA_VLCF, (1 << 5) | 4 /* BLACK | G_OLD */}
    , fptr(nullptr)
    , name(std::nullopt)
  {}

  CFuncInfo(HostString n, lua_CFunction f)
    : GCObject{nullptr, LUA_VLCF, (1 << 5) | 4 /* BLACK | G_OLD */}
    , fptr(f)
    , name(n.to_utf8())
  {}

  static CFuncInfo* luaF_newCFuncInfo(lua_State* L, lua_CFunction f,
                                       std::optional<byte_view> name = std::nullopt);
};

/* }================================================================== */

/*
** {==================================================================
** Tables
** ===================================================================
*/

constexpr lu_byte LUA_VTABLE = makevariant(LUA_TTABLE, 0);

struct Node
{
  TValue val;
  lu_byte key_tt;   /* key type */
  int next;         /* for chaining */
  Value key_val;    /* key value */
};

static l_forceinline void copynode(Node* dst, const Node* src)
{
  copyobj(&dst->val, &src->val);
  dst->key_tt = src->key_tt;
  dst->next = src->next;
  dst->key_val = src->key_val;
}

struct Counters;

struct Table : GCListed
{
  lu_byte flags;      /* 1<<p means tagmethod(p) is not present */
  std::span<Value> array; /* array part: values, forward-indexed */
  std::span<lu_byte> tags; /* array part: tags, forward-indexed */
  std::span<Node> node;
  int lastfree;
  int lenhint;   /* hint for #t; see luaH_getn */
  struct Table* metatable;

  static Table* luaH_new(lua_State* L);

  lu_byte luaH_get(const TValue* key, TValue* res);
  lu_byte luaH_getshortstr(TString* key, TValue* res);
  lu_byte luaH_getstr(TString* key, TValue* res);
  lu_byte luaH_getint(lua_Integer key, TValue* res);
  const TValue* luaH_Hgetshortstr(TString* key);

  int luaH_psetint(lua_Integer key, TValue* val);
  int luaH_psetshortstr(TString* key, TValue* val);
  int luaH_psetstr(TString* key, TValue* val);
  int luaH_pset(const TValue* key, TValue* val);

  void luaH_setint(lua_State* L, lua_Integer key, TValue* value);
  void luaH_set(lua_State* L, const TValue* key, TValue* value);

  void luaH_finishset(lua_State* L, const TValue* key, TValue* value, int hres);
  void luaH_resize(lua_State* L, int nasize, int nhsize);
  void luaH_resizearray(lua_State* L, int nasize);
  l_mem luaH_size();
  void luaH_free(lua_State* L);
  int luaH_next(lua_State* L, StkId key);
  lua_Integer luaH_getn(lua_State* L);
  int asize() const;

#if defined(LUA_DEBUG)
  int luaH_mainposition(const TValue* key) const;
#endif

  friend void reinserthash(lua_State* L, Table* ot, Table* t);

private:
  int haslastfree() const;
  int hashpow2(lua_Unsigned n) const;
  int hashmod(lua_Unsigned n) const;
  int hashstr(TString* str) const;
  int hashboolean(int p) const;
  int hashpointer(const void* p) const;
  int hashpointer(lua_CFunction p) const;
  int hashint(lua_Integer i) const;
  int mainpositionTV(const TValue* key) const;
  int mainpositionfromnode(int nd) const;
  int nodegeneric(const TValue* key, int deadok);
  const TValue* getgeneric(const TValue* key, int deadok);
  int ikeyinarray(lua_Integer k) const;
  int keyinarray(const TValue* key);
  lua_Integer findindex(lua_State* L, TValue* key, int asize);
  int sizehash();
  void freehash(lua_State* L);
  int insertkey(const TValue* key, TValue* value);
  void newcheckedkey(const TValue* key, TValue* value);
  void reinsertOldSlice(int oldasize, int newasize);
  void clearNewSlice(int oldasize, int newasize);
  void rehash(lua_State* L, const TValue* ek);
  int getfreepos();
  void luaH_newkey(lua_State* L, const TValue* key, TValue* value);
  int nodeintfromhash(lua_Integer key);
  const TValue* getintfromhash(lua_Integer key);
  int hashkeyisempty(lua_Integer key);
  int nodeshortstr(TString* key);
  const TValue* Hgetstr(TString* key);
  int finishnodeset(int idx, TValue* val);
  int rawfinishnodeset(int idx, TValue* val);
  int psetint(lua_Integer key, TValue* val);
  lua_Integer hash_search(lua_State* L, int asize);
  int binsearch(int i, int j);
  lua_Integer newhint(int hint);
  int arraykeyisempty(int key) const;
  void numusearray(Counters* ct) const;
  void numusehash(Counters* ct) const;
  std::span<Value> resizearray(lua_State* L, int oldasize, int newasize, std::span<lu_byte>* newtags);
  void setnodevector(lua_State* L, int size);
};

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
static l_forceinline int lmod(lua_Unsigned s, int size)
{
  assert((size & (size - 1)) == 0);
  return (int)(s & (lua_Unsigned)(size - 1));
}

constexpr int twoto(int x)
{
  return 1 << x;
}
static l_forceinline int sizenode(const Table* t)
{
  return (int)t->node.size();
}


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

static l_forceinline lua_Number nvalue(const TValue* o)
{
  assert(o->tt_ == LUA_VNUMINT || o->tt_ == LUA_VNUMFLT);
  return (o->tt_ == LUA_VNUMINT) ? ((lua_Number)(o->value_.prim))
                                 : std::bit_cast<lua_Number>(o->value_.prim);
}

static l_forceinline void* pvalue(const TValue* o)
{
  assert(o->tt_ == LUA_VLIGHTUSERDATA);
  return static_cast<void*>(o->value_.ref);
}

static l_forceinline lua_CFunction fvalue(const TValue* o)
{
  assert(o->tt_ == ctb(LUA_VLCF));
  return static_cast<CFuncInfo*>(o->value_.ref)->fptr;
}

static l_forceinline CFuncInfo* fcfuncinfo(const TValue* o)
{
  assert(o->tt_ == ctb(LUA_VLCF));
  return static_cast<CFuncInfo*>(o->value_.ref);
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


static l_forceinline GCObject* gckey(const Node* n)
{
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
  return static_cast<void*>(n->key_val.ref);
}

static l_forceinline lua_CFunction keyfval(const Node* n)
{
  assert(n->key_tt == ctb(LUA_VLCF));
  return static_cast<CFuncInfo*>(n->key_val.ref)->fptr;
}

static l_forceinline CFuncInfo* keycfuncinfo(const Node* n)
{
  assert(n->key_tt == ctb(LUA_VLCF));
  return static_cast<CFuncInfo*>(n->key_val.ref);
}

/* }================================================================== */

LUAI_FUNC int luaO_checkliveness(struct lua_State* L, const TValue* obj);

static l_forceinline void checkliveness(struct lua_State* L, const TValue* obj)
{
  assert(luaO_checkliveness(L, obj));
}

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

static l_forceinline void chgivalue(TValue* obj, lua_Integer x)
{
  assert(obj->tt_ == LUA_VNUMINT);
  obj->value_.prim = x;
}

static l_forceinline void setpvalue(TValue* obj, void* x)
{
  obj->value_.ref = static_cast<GCObject*>(x);
  obj->tt_ = LUA_VLIGHTUSERDATA;
}

static l_forceinline void setpvalue(TValue* obj, const void* x)
{
  setpvalue(obj, const_cast<void*>(x));
}

static l_forceinline void setfvalue(TValue* obj, CFuncInfo* cfi)
{
  obj->value_.ref = cfi;
  obj->tt_ = ctb(LUA_VLCF);
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


static l_forceinline void setnodekey(Node* node, const TValue* obj)
{
  node->key_val = obj->value_;
  node->key_tt = obj->tt_;
}

static l_forceinline void getnodekey(struct lua_State* L, TValue* obj, const Node* node)
{
  obj->value_ = node->key_val;
  obj->tt_ = node->key_tt;
  checkliveness(L, obj);
}

/* }================================================================== */

/* size of buffer for 'luaO_utf8esc' function */
constexpr int UTF8BUFFSZ = 8;


constexpr int MAXFMTARGS = 8;

struct FmtArg
{
  char tag;
  byte_view s;
  lua_Integer i;
  lua_Number n;
  const void* p;
};

struct FmtArgs
{
  int n = 0;
  FmtArg arg[MAXFMTARGS];
  FmtArgs& str(byte_view s);
  FmtArgs& str(std::string&&) = delete;
  FmtArgs& chr(int c);
  FmtArgs& num(int d);
  FmtArgs& lint(lua_Integer i);
  FmtArgs& flt(lua_Number f);
  FmtArgs& ptr(const void* p);
  FmtArgs& utf8(lua_Integer u);
};

/* }================================================================== */

LUAI_FUNC int luaO_utf8esc(byte_span buff, lua_Integer x);
LUAI_FUNC int luaO_ceillog2(int x);
LUAI_FUNC int luaO_codeparam(lua_Integer p);
LUAI_FUNC l_mem luaO_applyparam(int p, l_mem x);

LUAI_FUNC int luaO_rawarith(lua_State* L, int op, const TValue* p1, const TValue* p2, TValue* res);
LUAI_FUNC void luaO_arith(lua_State* L, int op, StkId p1, StkId p2, StkId res);
LUAI_FUNC size_t luaO_str2num(byte_view s, TValue* o);
LUAI_FUNC size_t luaO_tostringbuff(const TValue* obj, byte_span buff);
LUAI_FUNC lu_byte luaO_hexavalue(int c);
LUAI_FUNC void luaO_tostring(lua_State* L, TValue* obj);
LUAI_FUNC const InternalString* luaO_pushvfstring(lua_State* L, byte_view fmt, const FmtArgs& args);
LUAI_FUNC int luaO_fmtargcount(byte_view fmt);
LUAI_FUNC const InternalString* luaO_pushfstring(lua_State* L, byte_view fmt, const FmtArgs& args);
LUAI_FUNC void luaO_chunkid(byte_span out, byte_view source);
