/*
** $Id: lobject.h $
** Type definitions for Lua objects
** See Copyright Notice in lua.h
*/

#pragma once

#include <stdarg.h>
#include <string>

#include "llimits.h"
#include "lua.h"

/*
** Extra types for collectable non-values
*/
#define LUA_TUPVAL   LUA_NUMTYPES       /* upvalues */
#define LUA_TPROTO   (LUA_NUMTYPES + 1) /* function prototypes */
#define LUA_TDEADKEY (LUA_NUMTYPES + 2) /* removed keys in tables */

/*
** number of all possible types (including LUA_TNONE but excluding DEADKEY)
*/
#define LUA_TOTALTYPES (LUA_TPROTO + 2)

/*
** tags for Tagged Values have the following use of bits:
** bits 0-3: actual tag (a LUA_T* constant)
** bits 4-5: variant bits
** bit 6: whether value is collectable
*/

/* add variant bits to a type */
#define makevariant(t, v) ((t) | ((v) << 4))

/*
** Union of all Lua values
*/
typedef union Value
{
  struct GCObject* gc; /* collectable objects */
  void* p;             /* light userdata */
  lua_CFunction f;     /* light C functions */
  lua_Integer i;       /* integer numbers */
  lua_Number n;        /* float numbers */
  /* not used, but may avoid warnings for uninitialized value */
  lu_byte ub;
} Value;

/*
** Tagged Values. This is the basic representation of values in Lua:
** an actual value plus a tag with its type.
*/

#define TValuefields \
  Value value_;      \
  lu_byte tt_

typedef struct TValue
{
  TValuefields;
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
#define righttt(obj) (((obj)->tt_ & WITHVARIANT_MASK) == (lua_assert((obj)->tt_ & BIT_ISCOLLECTABLE), ((obj)->value_).gc)->tt)

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
#define LUA_VNIL makevariant(LUA_TNIL, 0)

/* Empty slot (which might be different from a slot containing nil) */
#define LUA_VEMPTY makevariant(LUA_TNIL, 1)

/* Value returned for a key not found in a table (absent key) */
#define LUA_VABSTKEY makevariant(LUA_TNIL, 2)

/* Special variant to signal that a fast get is accessing a non-table */
#define LUA_VNOTABLE makevariant(LUA_TNIL, 3)

/*
** Macro to test the result of a table access. Formally, it should
** distinguish between LUA_VEMPTY/LUA_VABSTKEY/LUA_VNOTABLE and
** other tags. As currently nil is equivalent to LUA_VEMPTY, it is
** simpler to just test whether the value is nil.
*/
#define tagisempty(tag) (((tag) & NOVARIANT_MASK) == LUA_TNIL)

#define isabstkey(v) ((v)->tt_ == LUA_VABSTKEY)

/*
** macro to detect non-standard nils (used only in assertions)
*/
#define isnonstrictnil(v) ((((v)->tt_ & NOVARIANT_MASK) == LUA_TNIL) && !((v)->tt_ == LUA_VNIL))

/*
** By default, entries with any kind of nil are considered empty.
** (In any definition, values associated with absent keys must also
** be accepted as empty.)
*/
#define isempty(v) (((v)->tt_ & NOVARIANT_MASK) == LUA_TNIL)

/* macro defining a value corresponding to an absent key */
#define ABSTKEYCONSTANT {NULL}, LUA_VABSTKEY

/* }================================================================== */

/*
** {==================================================================
** Booleans
** ===================================================================
*/

#define LUA_VFALSE makevariant(LUA_TBOOLEAN, 0)
#define LUA_VTRUE  makevariant(LUA_TBOOLEAN, 1)

#define l_isfalse(o)  (((o)->tt_ == LUA_VFALSE) || (((o)->tt_ & NOVARIANT_MASK) == LUA_TNIL))
#define tagisfalse(t) ((t) == LUA_VFALSE || ((t) & NOVARIANT_MASK) == LUA_TNIL)

/* }================================================================== */

/*
** {==================================================================
** Threads
** ===================================================================
*/

#define LUA_VTHREAD makevariant(LUA_TTHREAD, 0)

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
#define CommonHeader     \
  struct GCObject* next; \
  lu_byte tt;            \
  lu_byte marked

/* Common type for all collectable objects */
typedef struct GCObject
{
  CommonHeader;
} GCObject;

/* Bit mark for collectable types */
#define BIT_ISCOLLECTABLE (1 << 6)

/* mark a tag as collectable */
#define ctb(t) ((t) | BIT_ISCOLLECTABLE)

/* }================================================================== */

/*
** {==================================================================
** Numbers
** ===================================================================
*/

/* Variant tags for numbers */
#define LUA_VNUMINT makevariant(LUA_TNUMBER, 0) /* integer numbers */
#define LUA_VNUMFLT makevariant(LUA_TNUMBER, 1) /* float numbers */

/* }================================================================== */

/*
** {==================================================================
** Strings
** ===================================================================
*/

/* Variant tags for strings */
#define LUA_VSTRING makevariant(LUA_TSTRING, 0)
/* Keep old names for now during transition */
#define LUA_VSHRSTR LUA_VSTRING
#define LUA_VLNGSTR LUA_VSTRING

/*
** Header for a string value.
*/
typedef struct TString : GCObject
{
  lu_byte extra;       /* reserved words for short strings; "has hash" for longs */
  unsigned int hash;   /* cached hash value */
  std::string content; /* string data */
} TString;

#define getstr(ts)  ((ts)->content.c_str())
#define tsslen(ts)  ((ts)->content.size())
#define getlstr(ts, len) (cast_void((len) = (ts)->content.size()), (ts)->content.c_str())

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
#define LUA_VLIGHTUSERDATA makevariant(LUA_TLIGHTUSERDATA, 0)

#define LUA_VUSERDATA makevariant(LUA_TUSERDATA, 0)

/* Ensures that addresses after this type are always fully aligned. */
typedef union UValue
{
  TValue uv;
  LUAI_MAXALIGN; /* ensures maximum alignment for udata bytes */
} UValue;

/*
** Header for userdata with user values;
** memory area follows the end of this structure.
*/
typedef struct Udata : GCObject
{
  unsigned short nuvalue; /* number of user values */
  size_t len;             /* number of bytes */
  struct Table* metatable;
  GCObject* gclist;
  UValue uv[1]; /* user values */
} Udata;

/*
** Header for userdata with no user values. These userdata do not need
** to be gray during GC, and therefore do not need a 'gclist' field.
** To simplify, the code always use 'Udata' for both kinds of userdata,
** making sure it never accesses 'gclist' on userdata with no user values.
** This structure here is used only to compute the correct size for
** this representation. (The 'bindata' field in its end ensures correct
** alignment for binary data following this header.)
*/
typedef struct Udata0 : GCObject
{
  unsigned short nuvalue; /* number of user values */
  size_t len;             /* number of bytes */
  struct Table* metatable;
  union
  {
    LUAI_MAXALIGN;
  } bindata;
} Udata0;

/* compute the offset of the memory area of a userdata */
#define udatamemoffset(nuv) ((nuv) == 0 ? sizeof(Udata0) - sizeof(((Udata0*)0)->bindata) : sizeof(Udata) - sizeof(UValue) + (sizeof(UValue) * (nuv)))

/* get the address of the memory block inside 'Udata' */
#define getudatamem(u) (cast_charp(u) + udatamemoffset((u)->nuvalue))

/* compute the size of a userdata */
#define sizeudata(nuv, nb) (udatamemoffset(nuv) + (nb))

/* }================================================================== */

/*
** {==================================================================
** Prototypes
** ===================================================================
*/

#define LUA_VPROTO makevariant(LUA_TPROTO, 0)

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
#define PF_VAHID 1 /* function has hidden vararg arguments */
#define PF_VATAB 2 /* function has vararg table */
#define PF_FIXED 4 /* prototype has parts in fixed memory */

/* a vararg function either has hidden args. or a vararg table */
#define isvararg(p) ((p)->flag & (PF_VAHID | PF_VATAB))

/*
** mark that a function needs a vararg table. (The flag PF_VAHID will
** be cleared later.)
*/
#define needvatab(p) ((p)->flag |= PF_VATAB)

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

/* }================================================================== */

/*
** {==================================================================
** Functions
** ===================================================================
*/

#define LUA_VUPVAL makevariant(LUA_TUPVAL, 0)

/* Variant tags for functions */
#define LUA_VLCL makevariant(LUA_TFUNCTION, 0) /* Lua closure */
#define LUA_VLCF makevariant(LUA_TFUNCTION, 1) /* light C function */
#define LUA_VCCL makevariant(LUA_TFUNCTION, 2) /* C closure */

/*
** Upvalues for Lua closures
*/
typedef struct UpVal : GCObject
{
  struct UpVal* opennext;    /* next in open list (NULL when closed) */
  struct UpVal** previous;   /* back-pointer in open list (NULL when closed) */
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
  TValue upvalue[1]; /* list of upvalues */
} CClosure;

#define sizeCclosure(n) (sizeof(CClosure) - sizeof(TValue) + sizeof(TValue) * cast_uint(n))

typedef struct LClosure : Closure
{
  struct Proto* p;
  UpVal* upvals[1]; /* list of upvalues */
} LClosure;

#define sizeLclosure(n) (sizeof(LClosure) - sizeof(UpVal*) + sizeof(UpVal*) * cast_uint(n))

#define getproto(o) ((lua_assert((o)->tt_ == ctb(LUA_VLCL)), (lua_assert((((o)->value_).gc)->tt == LUA_VLCL), static_cast<LClosure*>(((o)->value_).gc)))->p)

/* }================================================================== */

/*
** {==================================================================
** Tables
** ===================================================================
*/

#define LUA_VTABLE makevariant(LUA_TTABLE, 0)

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
  Node* lastfree;     /* last free position in hash, scans backward */
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
#define lmod(s, size) (check_exp((size & (size - 1)) == 0, (cast_uint(s) & cast_uint((size) - 1))))

#define twoto(x)    (1u << (x))
#define sizenode(t) (twoto((t)->lsizenode))

/*
** {==================================================================
** Set functions — assign values into TValues
** ===================================================================
*/

/*
** Any value being manipulated by the program either is non
** collectable, or the collectable object has the right tag
** and it is not dead. The option 'L == NULL' allows other
** callers to use it where L is not available.
** Debug builds check via luaO_checkliveness; release builds compile
** the check away (assert_code).
*/
LUAI_FUNC void luaO_checkliveness(struct lua_State* L, const TValue* obj);

static inline void checkliveness(struct lua_State* L, const TValue* obj)
{
  (void) L;
  (void) obj;
  assert_code(luaO_checkliveness(L, obj));
}

/* main value copy (from 'obj2' to 'obj1') */
static inline void setobj(struct lua_State* L, TValue* obj1, const TValue* obj2)
{
  obj1->value_ = obj2->value_;
  obj1->tt_ = obj2->tt_;
  checkliveness(L, obj1);
  lua_assert(!isnonstrictnil(obj1));
}

static inline void setfltvalue(TValue* obj, lua_Number x)
{
  obj->value_.n = x;
  obj->tt_ = LUA_VNUMFLT;
}

/* change value of a float, keeping its tag */
static inline void chgfltvalue(TValue* obj, lua_Number x)
{
  lua_assert(obj->tt_ == LUA_VNUMFLT);
  obj->value_.n = x;
}

static inline void setivalue(TValue* obj, lua_Integer x)
{
  obj->value_.i = x;
  obj->tt_ = LUA_VNUMINT;
}

/* change value of an integer, keeping its tag */
static inline void chgivalue(TValue* obj, lua_Integer x)
{
  lua_assert(obj->tt_ == LUA_VNUMINT);
  obj->value_.i = x;
}

static inline void setpvalue(TValue* obj, void* x)
{
  obj->value_.p = x;
  obj->tt_ = LUA_VLIGHTUSERDATA;
}

static inline void setfvalue(TValue* obj, lua_CFunction x)
{
  obj->value_.f = x;
  obj->tt_ = LUA_VLCF;
}

static inline void setgcovalue(struct lua_State* L, TValue* obj, GCObject* x)
{
  (void) L;
  obj->value_.gc = x;
  obj->tt_ = ctb(x->tt);
}

static inline void setsvalue(struct lua_State* L, TValue* obj, TString* x)
{
  lua_assert((x->tt & NOVARIANT_MASK) == LUA_TSTRING);
  obj->value_.gc = x;
  obj->tt_ = ctb(x->tt);
  checkliveness(L, obj);
}

static inline void setuvalue(struct lua_State* L, TValue* obj, Udata* x)
{
  lua_assert(x->tt == LUA_VUSERDATA);
  obj->value_.gc = x;
  obj->tt_ = ctb(LUA_VUSERDATA);
  checkliveness(L, obj);
}

static inline void sethvalue(struct lua_State* L, TValue* obj, Table* x)
{
  lua_assert(x->tt == LUA_VTABLE);
  obj->value_.gc = x;
  obj->tt_ = ctb(LUA_VTABLE);
  checkliveness(L, obj);
}

static inline void setclLvalue(struct lua_State* L, TValue* obj, LClosure* x)
{
  lua_assert(x->tt == LUA_VLCL);
  obj->value_.gc = x;
  obj->tt_ = ctb(LUA_VLCL);
  checkliveness(L, obj);
}

static inline void setclCvalue(struct lua_State* L, TValue* obj, CClosure* x)
{
  lua_assert(x->tt == LUA_VCCL);
  obj->value_.gc = x;
  obj->tt_ = ctb(LUA_VCCL);
  checkliveness(L, obj);
}

/* setthvalue lives in lstate.h — it needs the complete lua_State type */

/* copy a value into a node key */
static inline void setnodekey(Node* node, const TValue* obj)
{
  node->key_val = obj->value_;
  node->key_tt = obj->tt_;
}

/* copy a value from a node key */
static inline void getnodekey(struct lua_State* L, TValue* obj, const Node* node)
{
  obj->value_ = node->key_val;
  obj->tt_ = node->key_tt;
  checkliveness(L, obj);
}

/* }================================================================== */

/* size of buffer for 'luaO_utf8esc' function */
#define UTF8BUFFSZ 8

/* macro to call 'luaO_pushvfstring' correctly */
#define pushvfstring(L, argp, fmt, msg)                    \
  {                                                        \
    va_start(argp, fmt);                                   \
    msg = luaO_pushvfstring(L, fmt, argp);                 \
    va_end(argp);                                          \
    if (msg == NULL)                                       \
      luaD_throw(L, LUA_ERRMEM); /* only after 'va_end' */ \
  }

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
                                        va_list argp);
LUAI_FUNC const char* luaO_pushfstring(lua_State* L, const char* fmt, ...);
LUAI_FUNC void luaO_chunkid(char* out, const char* source, size_t srclen);
