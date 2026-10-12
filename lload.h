/*
** Loads that a freeze can stop
** See Copyright Notice in lua.h
*/

#pragma once

#include <new>
#include <vector>

#include "lua.h"

#include "llimits.h"
#include "lstate.h"
#include "lua_internal.h"

namespace lua::compiler
{
struct Dyndata;
}
namespace lua::object
{
struct GCObject;
}
namespace lua::runtime
{
struct CallInfo;
}

namespace lua::bytes
{

class MBuffer;
struct ZIO;

/*
** A load (a parse or a binary load) that a freeze can stop: in a call of
** its reader function, or at a check point (each token read is a step of
** the budget). The functions of the load on the C stack keep their shape;
** when a freeze passes, each records its frame (where it was and its
** locals) as the freeze leaves it, innermost first. After the thaw, the
** continuation of the C function that loads makes the calls again from
** the frames, outermost first, and the load goes on where it stopped.
*/

/*
** The frame of a function of a load, as a freeze leaves it: where it was
** (its state, the call it was in, which it makes again after the thaw)
** and its locals. Each function with locals to keep has its own frame
** type, with them as its fields; this one has none.
*/
struct LoadFrame
{
  int fn = 0;                 /* the function ('LF_*', set when recorded) */
  int site = 0;               /* its state (0: its start) */
  LoadFrame* below = nullptr; /* (in the box: the frame recorded before it) */
  LoadFrame() = default;
  LoadFrame(const LoadFrame&) = default;
  LoadFrame& operator=(const LoadFrame&) = default;
  virtual ~LoadFrame() = default;
  /* a copy, to record (nullptr: no memory for it) */
  virtual LoadFrame* copy() const { return new (std::nothrow) LoadFrame(*this); }
  /* its size, counted in the box's room for frames */
  virtual size_t size() const { return sizeof(*this); }
};

/* where a load stopped */
constexpr lu_byte LS_READ = 1;  /* in a call of its reader function */
constexpr lu_byte LS_CHECK = 2; /* at a check point */

/* the functions with frames ('luaZ_record'): a frame made again is checked to be its function's */
enum : lu_byte
{
  LF_FILL = 1,
  LF_ZREAD,
  LF_FPARSER,
  /* the lexer */
  LF_LLEX,
  LF_NEXT,
  LF_INCLINE,
  LF_NUMERAL,
  LF_SKIPSEP,
  LF_LONGSTRING,
  LF_ESCCHECK,
  LF_GETHEXA,
  LF_HEXAESC,
  LF_UTF8ESC,
  LF_UTF8,
  LF_DECESC,
  LF_STRING,
  /* the parser */
  LF_PARSER,
  LF_MAINFUNC,
  LF_INITGLOBAL,
  LF_STATLIST,
  LF_FIELDSEL,
  LF_YINDEX,
  LF_RECFIELD,
  LF_FIELD,
  LF_CONSTRUCTOR,
  LF_PARLIST,
  LF_BODY,
  LF_EXPLIST,
  LF_FUNCARGS,
  LF_PRIMARYEXP,
  LF_SUFFIXEDEXP,
  LF_SIMPLEEXP,
  LF_SUBEXPR,
  LF_BLOCK,
  LF_RESTASSIGN,
  LF_COND,
  LF_BREAKSTAT,
  LF_LABELSTAT,
  LF_WHILESTAT,
  LF_REPEATSTAT,
  LF_EXP1,
  LF_FORBODY,
  LF_FORNUM,
  LF_FORLIST,
  LF_FORSTAT,
  LF_TESTTHEN,
  LF_IFSTAT,
  LF_LOCALFUNC,
  LF_VARATTRIB,
  LF_LOCALSTAT,
  LF_GLOBALNAMES,
  LF_GLOBALSTAT,
  LF_GLOBALFUNC,
  LF_GLOBALSTATFUNC,
  LF_FUNCNAME,
  LF_FUNCSTAT,
  LF_EXPRSTAT,
  LF_RETSTAT,
  LF_STATEMENT,
  /* the binary loader */
  LF_UNDUMP,
  LF_LOADVARINT,
  LF_LOADNUMBER,
  LF_LOADSTRING,
  LF_LOADCODE,
  LF_LOADCONSTANTS,
  LF_LOADUPVALUES,
  LF_LOADPROTOS,
  LF_LOADDEBUG,
  LF_LOADFUNCTION,
  LF_CHECKLITERAL,
  LF_CHECKNUM,
  LF_CHECKHEADER
};

struct LoadBox;

struct LoadResume
{
  lua_State* L = nullptr;
  lua_KFunction k = nullptr; /* the continuation of the C function that loads */
  lua_KContext ctx = 0;
  int boxidx = 0;            /* the stack slot of its box (made at the first stop) */
  int pieceidx = 0;          /* the stack slot that keeps the piece being read */
  int readeridx = 0;         /* the reader function's slot (0: a string chunk at 1) */
  l_uint32 baseccalls = 0;   /* the C calls when the load began (a bound of its frames) */
  ZIO* z = nullptr;
  ::lua::compiler::Dyndata* dyd = nullptr; /* the parser's arrays and buffer (held by the box while stopped) */
  MBuffer* buff = nullptr;
  bool given = false; /* (a string chunk: did its reader give it?) */
  LoadBox* box = nullptr;
  bool recording = false; /* a freeze passes: the frames record themselves */
  const LoadFrame* rframe = nullptr; /* (the next frame to make again) */
  size_t points = 0;      /* the stop points it passed (tokens read, checks of the binary loader) */
  size_t pieces = 0;      /* the pieces read before it had a box */
  size_t ends = 0;        /* the reads that gave the end before it had a box */
  bool replaying = false; /* restored from outside the process: going again to where it stopped */
  unsigned int clhash = 0; /* the hash its function was given (0: not made yet) */
};

/* the frame 'l' of function 'fn', as a freeze leaves it (a copy, kept by the box) */
void luaZ_record(LoadResume* rz, int fn, const LoadFrame& l);
/* the frame of 'fn' made again (its type is the one 'fn' records) */
const LoadFrame& luaZ_restore(LoadResume* rz, int fn);

/* the piece the reader function gave to the read a freeze stopped (see 'luaZ_fill') */
std::optional<byte_view> luaZ_resumeread(LoadResume* rz);
/* the last frame made again: the load goes on from its stop */
void luaZ_resumed(LoadResume* rz);
/* a frame not as expected: an error */
[[noreturn]] void luaZ_badstate(LoadResume* rz);
/* did the frames of a freeze record themselves? */
bool luaZ_recorded(const LoadResume* rz);
/* a check point of the load */
void luaZ_checkpoint(LoadResume* rz);
/* a stop point passed again by a restored load ('replaying') */
void luaZ_replaypoint(LoadResume* rz);
/* the load with spec 'ls' starts, or goes on after a thaw */
int luaZ_loadk(lua_State* L, const lua_LoadSpec& ls, bool resume);
/* the function of the load is made ('cl', by the parser or the binary loader) */
void luaZ_madefunction(LoadResume* rz, ::lua::object::GCObject* cl);
/* the chunk of a load from a reader that calls no code, read whole into its box */
void luaZ_loadread(lua_State* L, const lua_LoadSpec& ls, lua_Reader* source);
/* the parser's arrays and buffer, held by the box while the load is stopped */
void luaZ_hold(LoadResume* rz, ::lua::runtime::CallInfo* ci, int anchoridx);
void luaZ_unhold(LoadResume* rz, ::lua::runtime::CallInfo* ci);

/* a stop point of a load that a freeze can stop: is a check point due there? */
static l_forceinline bool luaZ_checkdue(LoadResume* rz, lua_State* L)
{
  rz->points++;
  if (l_unlikely(rz->replaying))
  {
    luaZ_replaypoint(rz);
    return false; /* (its steps were taken before) */
  }
  return (L->hookmask & LUA_MASKBUDGET) && ::lua::runtime::luaE_spent(L, 1);
}

/* is 'rz' a stopped load whose frames are being made again (outermost first)? */
static l_forceinline bool luaZ_resuming(const LoadResume* rz)
{
  return rz != nullptr && rz->z->resuming;
}

}
