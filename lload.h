/*
** Loads that a freeze can stop
** See Copyright Notice in lua.h
*/

#pragma once

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

/* where a load stopped */
constexpr lu_byte LS_READ = 1;  /* in a call of its reader function */
constexpr lu_byte LS_CHECK = 2; /* at a check point */

/* the functions with frames ('luaZ_record'): a frame is checked to be the one expected */
enum : lu_byte
{
  LF_FILL = 1,
  LF_CHECKBUFFER,
  LF_ZREAD,
  LF_GETADDR,
  LF_FPARSER,
  /* the lexer */
  LF_LLEX,
  LF_NEXT,
  LF_LOOKAHEAD,
  LF_INCLINE,
  LF_CHECKNEXT1,
  LF_CHECKNEXT2,
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
  LF_TESTNEXT,
  LF_CHECKNEXT,
  LF_CHECKMATCH,
  LF_CHECKNAME,
  LF_CODENAME,
  LF_SINGLEVAR,
  LF_INITGLOBAL,
  LF_STATLIST,
  LF_FIELDSEL,
  LF_YINDEX,
  LF_RECFIELD,
  LF_LISTFIELD,
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
  LF_EXPR,
  LF_BLOCK,
  LF_RESTASSIGN,
  LF_COND,
  LF_GOTOSTAT,
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
  LF_GLOBALATTRIB,
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
  LF_LOADBLOCK,
  LF_LOADALIGN,
  LF_LOADBYTE,
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
  LF_CHECKNUMSIZE,
  LF_CHECKNUM,
  LF_CHECKHEADER,
  LF_LAST
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
  size_t rpos = 0;        /* (the end of the next frame to make again) */
  size_t points = 0;      /* the stop points it passed (tokens read, checks of the binary loader) */
  size_t pieces = 0;      /* the pieces read before it had a box */
  size_t ends = 0;        /* the reads that gave the end before it had a box */
  bool replaying = false; /* restored from outside the process: going again to where it stopped */
  unsigned int clhash = 0; /* the hash its function was given (0: not made yet) */
};

/* a frame of function 'fn' at 'site', its locals at 'l' ('n' bytes), as a freeze leaves it */
void luaZ_record(LoadResume* rz, int fn, int site, const void* l, size_t n);
/* the frame of 'fn' made again: its locals into 'l'; returns its site */
int luaZ_restore(LoadResume* rz, int fn, void* l, size_t n);

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
