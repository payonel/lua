/*
** Loads that a freeze can stop
** See Copyright Notice in lua.h
*/

#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "lauxlib.h"
#include "ldo.h"
#include "lgc.h"
#include "lload.h"
#include "lmem.h"
#include "lparser.h"
#include "lstate.h"
#include "lua_internal.h"
#include "lzio.h"

#include "lhack.h"
#include "literals.h"

namespace lua::bytes
{

using ::lua::compiler::Dyndata;
using ::lua::compiler::Labeldesc;
using ::lua::compiler::Vardesc;

/* where the stream's piece was when the load stopped */
constexpr lu_byte ZS_NONE = 0;   /* none, or all read */
constexpr lu_byte ZS_STRING = 1; /* the chunk, a string (at 1) */
constexpr lu_byte ZS_PIECE = 2;  /* the piece the reader function gave (at 'pieceidx') */
constexpr lu_byte ZS_INPUT = 3;  /* the input read before a restore, read again */

/* the bytes of frames a stopped load may have for each level of C calls it made */
constexpr size_t FRAMES_PER_LEVEL = 1024;
constexpr size_t FRAMES_BASE = 2048;

constexpr size_t LOADBOX_SIZE = 192; /* (its own size, its frames and input aside) */

/*
** The box of a load that a freeze can stop, in a stack slot of the C
** function that loads (made at its first stop point). What it keeps is
** counted by the host, not in the Lua heap (a program does not see it):
** one block, given back when the collector frees the box.
** - its frames, recorded when a freeze passes (charged to it), how it
**   stopped and where the stream was; while stopped, the parser's arrays
**   and buffer (charged to it as room for them, at a stop point; they are
**   counted in it while it holds them) and its anchor table (a user value);
** - what is kept outside the process (see 'luaZ_loadk'): the input read
**   (the pieces of the reader function) and the stop points passed, from
**   which a restored load goes again to where it stopped.
*/
struct LoadBox : lua_UserObject
{
  global_State* g = nullptr;
  size_t hosted = 0; /* (the bytes the host counts for it) */
  std::vector<char> frames;
  size_t charged = 0;   /* (the room for its frames) */
  size_t room = 0;      /* (the room for the parser's arrays and buffer) */
  lu_byte stop = 0;
  lu_byte zsrc = ZS_NONE;
  size_t zpos = 0;
  bool given = false;    /* (a string chunk: was it given?) */
  bool holds = false;    /* (the arrays and buffer below, and the anchor) */
  bool complete = true;  /* is 'input' all the input read? */
  bool replay = false;   /* restored from outside the process: frames gone */
  size_t points = 0;     /* the stop points passed when it stopped */
  size_t ends = 0;       /* the reads that gave the end (nil or an empty piece) */
  int anchoridx = 0;     /* the stack slot of the anchor table */
  Dyndata dyd{};
  byte_owner buff;
  size_t bufflen = 0;
  std::vector<char> input; /* (charged to it) */
  ~LoadBox() override;
  const char* persistkind() const override { return "load"; }
  int persistsave(lua_Integer* out, byte_view* b) const override;
  bool persistuservalues() const override { return false; }
};

LoadBox::~LoadBox()
{
  if (holds)
  { /* (counted in its room) */
    delete[] dyd.actvar.arr;
    delete[] dyd.gt.arr;
    delete[] dyd.label.arr;
  }
  luaM_shrinkhost(g, hosted, 0);
}

/*
** 'n' more bytes kept by the box, counted by the host. (Called outside
** the parse, which is unlocked: a memory error leaves the state locked, as
** an error raised through the API does.)
*/
static void charge(lua_State* L, LoadBox* box, size_t n)
{
  lua_lock(L);
  luaM_growhost(L, box->hosted, box->hosted + n);
  box->hosted += n;
  lua_unlock(L);
}

/* a new box, at slot 'boxidx' (outside the parse) */
static LoadBox* newbox(lua_State* L, int boxidx)
{
  lua_lock(L);
  luaD_checkstack(L, 1);
  lua_unlock(L);
  auto o = std::make_unique<LoadBox>();
  LoadBox* box = o.get();
  box->g = L->l_G;
  lua_newuserobject(L, std::move(o), 0, 1);
  lua_replace(L, boxidx);
  charge(L, box, LOADBOX_SIZE);
  return box;
}

/* 'b' kept at the end of the box's input (charged as it grows) */
static void keep(lua_State* L, LoadBox* box, byte_view b)
{
  std::vector<char>& in = box->input;
  if (b.size() > in.capacity() - in.size())
  {
    size_t cap = std::max(in.size() + b.size(), 2 * in.capacity());
    charge(L, box, cap - in.capacity());
    in.reserve(cap);
  }
  in.insert(in.end(), b.data(), b.data() + b.size());
}

/* the blocks of the parser's arrays and buffer (bytes, as counted when made) */
enum
{
  NHELD = 4
};
static void heldblocks(const Dyndata* dyd, const MBuffer* buff, size_t* b)
{
  b[0] = static_cast<size_t>(dyd->actvar.size) * cost::Vardesc;
  b[1] = static_cast<size_t>(dyd->gt.size) * cost::Labeldesc;
  b[2] = static_cast<size_t>(dyd->label.size) * cost::Labeldesc;
  b[3] = buff->capacity() * cost::Byte;
}

static size_t heldsize(const Dyndata* dyd, const MBuffer* buff)
{
  size_t b[NHELD];
  heldblocks(dyd, buff, b);
  return b[0] + b[1] + b[2] + b[3];
}

/* kept outside the process: how it stopped, the points passed, the input */
enum
{
  LB_STOP,
  LB_POINTS,
  LB_GIVEN,
  LB_ANCHOR, /* (0: it never stopped, a box left from a load that ended) */
  LB_ENDS,
  LB_COMPLETE,
  LB_NINTS
};

int LoadBox::persistsave(lua_Integer* out, byte_view* b) const
{
  out[LB_STOP] = stop;
  out[LB_POINTS] = static_cast<lua_Integer>(points);
  out[LB_GIVEN] = given;
  out[LB_ANCHOR] = anchoridx;
  out[LB_ENDS] = static_cast<lua_Integer>(ends);
  out[LB_COMPLETE] = complete;
  *b = byte_view(input.data(), input.size());
  return LB_NINTS;
}

static std::unique_ptr<lua_UserObject> makeloadbox(lua_State* L, const lua_Integer* ints, int nints, byte_view bytes)
{
  if (nints != LB_NINTS || (ints[LB_STOP] != LS_READ && ints[LB_STOP] != LS_CHECK) || ints[LB_POINTS] < 0 ||
      (ints[LB_GIVEN] != 0 && ints[LB_GIVEN] != 1) || ints[LB_ANCHOR] < 0 || ints[LB_ANCHOR] > INT_MAX ||
      ints[LB_ENDS] < 0 || (ints[LB_COMPLETE] != 0 && ints[LB_COMPLETE] != 1))
    return nullptr;
  auto box = std::make_unique<LoadBox>();
  box->g = L->l_G;
  box->stop = static_cast<lu_byte>(ints[LB_STOP]);
  box->points = static_cast<size_t>(ints[LB_POINTS]);
  box->given = ints[LB_GIVEN] != 0;
  box->anchoridx = static_cast<int>(ints[LB_ANCHOR]);
  box->ends = static_cast<size_t>(ints[LB_ENDS]);
  box->complete = ints[LB_COMPLETE] != 0;
  /* (a load that stopped goes again over its input, if it kept all of it;
     another is not stopped, and one that did not keep its input cannot go
     on: an error if it is made to) */
  box->replay = (box->anchoridx != 0 && box->complete);
  luaM_growhost(L, 0, LOADBOX_SIZE + bytes.size()); /* (counted by the host, as in the original) */
  box->hosted = LOADBOX_SIZE + bytes.size();
  box->input.reserve(bytes.size());
  box->input.assign(bytes.data(), bytes.data() + bytes.size());
  return box;
}

struct LoadBoxKind
{
  LoadBoxKind() { lua_registerobjectkind("load", makeloadbox); }
};
static LoadBoxKind loadboxkind;

/* (in the parse, which runs locked) */
[[noreturn]] void luaZ_badstate(LoadResume* rz)
{
  rz->z->resuming = false;
  luaO_pushfstring(rz->L, lt::invalid_state_of_a_stopped_load, FmtArgs());
  luaD_throw(rz->L, LUA_ERRSYNTAX);
}

void luaZ_record(LoadResume* rz, int fn, int site, const void* l, size_t n)
{
  if (rz == nullptr || !rz->recording || rz->box == nullptr)
    return; /* (not a freeze of a stop point) */
  assert(n < 65536);
  std::vector<char>& f = rz->box->frames;
  size_t at = f.size();
  assert(at + n + 4 <= f.capacity()); /* (the bound of 'prepstop') */
  f.resize(at + n + 4);
  if (n != 0)
    memcpy(&f[at], l, n);
  f[at + n] = static_cast<char>(fn);
  f[at + n + 1] = static_cast<char>(site);
  f[at + n + 2] = static_cast<char>(n & 0xff);
  f[at + n + 3] = static_cast<char>(n >> 8);
}

int luaZ_restore(LoadResume* rz, int fn, void* l, size_t n)
{
  const std::vector<char>& f = rz->box->frames;
  size_t p = rz->rpos;
  if (p < 4)
    luaZ_badstate(rz);
  size_t hn = static_cast<unsigned char>(f[p - 2]) | (static_cast<size_t>(static_cast<unsigned char>(f[p - 1])) << 8);
  if (static_cast<unsigned char>(f[p - 4]) != fn || hn != n || p < 4 + n)
    luaZ_badstate(rz);
  if (n != 0)
    memcpy(l, &f[p - 4 - n], n);
  rz->rpos = p - 4 - n;
  return static_cast<unsigned char>(f[p - 3]);
}

void luaZ_resumed(LoadResume* rz)
{
  if (rz->rpos != 0)
    luaZ_badstate(rz);
  rz->z->resuming = false;
}

bool luaZ_recorded(const LoadResume* rz)
{
  return !rz->box->frames.empty();
}

/*
** A stop point passed again by a load restored from outside the process
** (see 'luaZ_loadk'): the one it stopped at is behind it.
*/
void luaZ_replaypoint(LoadResume* rz)
{
  if (rz->points == rz->box->points && rz->box->stop == LS_CHECK)
    rz->replaying = false; /* (the load goes on from here) */
}

/*
** A stop point of the load: a freeze may pass. The box is made (at its
** slot) the first time; it gets room for the frames, charged to it, and
** keeps how the load stops and where the stream is.
*/
static void prepstop(LoadResume* rz, lu_byte kind)
{
  lua_State* L = rz->L;
  LoadBox* box = rz->box;
  if (box == nullptr)
  {
    box = newbox(L, rz->boxidx);
    box->complete = (rz->pieces == 0);
    box->ends = rz->ends;
    rz->box = box;
  }
  size_t levels = static_cast<size_t>(getCcalls(L)) - rz->baseccalls;
  size_t bound = FRAMES_BASE + (levels + 1) * FRAMES_PER_LEVEL;
  if (bound > box->charged)
  {
    charge(L, box, bound - box->charged);
    box->charged = bound;
  }
  size_t held = heldsize(rz->dyd, rz->buff);
  if (held > box->room)
  { /* (a freeze here cannot be refused the memory the box holds then) */
    charge(L, box, held - box->room);
    box->room = held;
  }
  box->frames.clear();
  box->frames.reserve(bound);
  box->stop = kind;
  box->points = rz->points;
  ZIO* z = rz->z;
  const char* d = z->chunk.data();
  std::optional<byte_view> s;
  box->zpos = z->pos;
  box->given = (rz->given != nullptr && *rz->given);
  if (z->pos == z->chunk.size())
  { /* (all read: what the piece was does not matter; an empty piece, which
       ends the stream, leaves the one before it, no longer kept) */
    box->zsrc = ZS_NONE;
    box->zpos = 0;
  }
  else if (rz->readeridx == 0 && (s = lua_tolstring(L, 1)) && d == s->data())
    box->zsrc = ZS_STRING;
  else if (rz->readeridx > 0 && (s = lua_tolstring(L, rz->pieceidx)) && d == s->data())
    box->zsrc = ZS_PIECE;
  else if (d == box->input.data())
    box->zsrc = ZS_INPUT;
  else
  { /* (cannot be: the stream reads from one of those) */
    lua_pushistringref(L, lt::invalid_state_of_a_stopped_load);
    lua_error(L);
  }
  rz->recording = true;
}

/*
** The value the reader function gave, on the top: the next piece (kept in
** the box, if it keeps the input), or the end.
*/
static std::optional<byte_view> readerresult(lua_State* L, LoadResume* rz)
{
  LoadBox* box = rz->box;
  if (lua_isnil(L, -1))
  {
    lua_pop(L, 1); /* pop result */
    if (box == nullptr)
      rz->ends++;
    else
      box->ends++;
    return std::nullopt;
  }
  else if (l_unlikely(!lua_isstring(L, -1)))
    luaL_errori(L, lt::reader_function_must_return_a_string);
  lua_replace(L, rz->pieceidx); /* save string in its slot */
  byte_view piece = *lua_tolstring(L, rz->pieceidx);
  if (piece.empty())
  { /* (an empty piece ends the stream too) */
    if (box == nullptr)
      rz->ends++;
    else
      box->ends++;
  }
  else if (box == nullptr)
    rz->pieces++;
  else if (box->complete)
    keep(L, box, piece);
  return piece;
}

std::optional<byte_view> luaZ_resumeread(LoadResume* rz)
{
  return readerresult(rz->L, rz);
}

/*
** A check point of the load (a token read, when the budget is spent): the
** callback runs, and may freeze the load here. (The parse runs locked; the
** box and the check point are made through the API.)
*/
void luaZ_checkpoint(LoadResume* rz)
{
  lua_unlock(rz->L);
  prepstop(rz, LS_CHECK);
  lua_checkpointk(rz->L, rz->ctx, rz->k);
  rz->recording = false;
  lua_lock(rz->L);
}

/*
** The reader function of 'load', called as stock calls it, inside the
** parse (its errors call the message handler in effect, where they
** happen). With a budget, a freeze can pass through the call.
*/
struct FunctionReader : lua_Reader
{
  LoadResume* rz;
  std::optional<byte_view> read(lua_State* L) override
  {
    luaL_checkstacki(L, 2, lt::too_many_nested_functions);
    lua_pushvalue(L, rz->readeridx); /* get function */
    if (L->hookmask & LUA_MASKBUDGET)
    {
      prepstop(rz, LS_READ);
      int status = lua_pcallkeep(L, 0, 1, rz->ctx, rz->k);
      rz->recording = false;
      if (l_unlikely(status != LUA_OK))
      { /* (as it was: the handler has seen it) */
        lua_lock(L);
        luaD_throw(L, static_cast<TStatus>(status));
      }
    }
    else
      lua_call(L, 0, 1); /* call it */
    return readerresult(L, rz);
  }
};

/* a chunk that is a string (at 1), or that is in the box ('boxed'), read at once */
struct StringReader : lua_Reader
{
  bool given = false;
  const LoadResume* boxed = nullptr;
  std::optional<byte_view> read(lua_State* L) override
  {
    if (given)
      return std::nullopt;
    given = true;
    if (boxed != nullptr)
      return byte_view(boxed->box->input.data(), boxed->box->input.size());
    return lua_tolstring(L, 1);
  }
};

/*
** A load restored from outside the process goes again over the input it
** had read, then (stopped in a call of the reader function) gets the
** piece that call gave, and then reads as before.
*/
struct ReplayReader : lua_Reader
{
  LoadResume* rz;
  lua_Reader* next;
  int phase = 0;
  size_t ends = 0;
  std::optional<byte_view> read(lua_State* L) override
  {
    LoadBox* box = rz->box;
    if (phase == 0)
    {
      phase = 1;
      if (!box->input.empty())
        return byte_view(box->input.data(), box->input.size());
    }
    if (phase == 1)
    {
      if (ends < box->ends)
      { /* (the reads that gave the end) */
        ends++;
        return std::nullopt;
      }
      phase = 2;
      if (rz->replaying && box->stop == LS_READ)
      { /* the piece of the read it stopped in */
        rz->replaying = false;
        lua_getiuservalue(L, rz->boxidx, 1);
        lua_pushnil(L);
        lua_setiuservalue(L, rz->boxidx, 1);
        return readerresult(L, rz);
      }
    }
    return next->read(L);
  }
};

/*
** The chunk of a load from a reader that calls no code (a file), read
** whole into a new box at 'ls.boxidx' (with a budget: see 'lua_loadkread';
** outside the parse). It is kept as it is, perhaps long: its room is trimmed.
*/
void luaZ_loadread(lua_State* L, const lua_LoadSpec& ls, lua_Reader* source)
{
  LoadBox* box = newbox(L, ls.boxidx);
  for (;;)
  {
    std::optional<byte_view> b = source->read(L);
    if (!b.has_value() || b->empty())
      break;
    keep(L, box, *b);
  }
  std::vector<char>& in = box->input;
  size_t cap = in.capacity();
  if (cap > in.size())
  {
    charge(L, box, in.size()); /* (the trimmed copy, before it is made) */
    std::vector<char>(in.begin(), in.end()).swap(in);
    if (luaM_shrinkhost(L->l_G, box->hosted, box->hosted - cap))
      box->hosted -= cap;
  }
}

/* the load ended, with status 'st': what its box keeps is given back */
static int ended(lua_State* L, LoadResume* rz, int st)
{
  LoadBox* box = rz->box;
  if (box != nullptr && !box->holds)
  {
    std::vector<char>().swap(box->frames);
    std::vector<char>().swap(box->input);
    box->anchoridx = 0; /* (not stopped: inert, if kept outside the process) */
    if (luaM_shrinkhost(L->l_G, box->hosted, LOADBOX_SIZE))
    {
      box->hosted = LOADBOX_SIZE;
      box->charged = box->room = 0;
    }
  }
  return st;
}

/*
** A load with spec 'ls' starts, or goes on after a thaw ('resume': its box
** at 'ls.boxidx', the result of the reader function on the top if it
** stopped in a call of it):
** - a load stopped in this process makes its frames again;
** - a load restored from outside the process (its box keeps its input and
**   the stop points it passed) goes again over that input from the start,
**   taking no step and making no check point up to where it stopped.
** Returns its status: the function on the top, or the error.
*/
int luaZ_loadk(lua_State* L, const lua_LoadSpec& ls, bool resume)
{
  LoadResume rz;
  rz.L = L;
  rz.k = ls.k;
  rz.ctx = ls.ctx;
  rz.boxidx = ls.boxidx;
  rz.pieceidx = ls.pieceidx;
  rz.readeridx = ls.readeridx;
  rz.baseccalls = static_cast<l_uint32>(getCcalls(L));
  FunctionReader freader;
  StringReader sreader;
  ReplayReader rreader;
  freader.rz = &rz;
  rz.given = &sreader.given;
  if (ls.readeridx < 0)
    sreader.boxed = &rz;
  lua_Reader* reader = (ls.readeridx > 0) ? static_cast<lua_Reader*>(&freader) : &sreader;
  ZIO z;
  luaZ_init(L, &z, reader);
  z.rz = &rz;
  rz.z = &z;
  if (!resume && ls.readeridx < 0)
  { /* (its box, made by 'luaZ_loadread') */
    rz.box = dynamic_cast<LoadBox*>(lua_touserobject(L, ls.boxidx));
    if (rz.box == nullptr)
    {
      lua_pushistringref(L, lt::invalid_state_of_a_stopped_load);
      return LUA_ERRSYNTAX;
    }
  }
  else if (resume)
  {
    LoadBox* box = dynamic_cast<LoadBox*>(lua_touserobject(L, ls.boxidx));
    if (box == nullptr || (!box->holds && !box->replay))
    {
      lua_pushistringref(L, lt::invalid_state_of_a_stopped_load);
      return LUA_ERRSYNTAX;
    }
    rz.box = box;
    if (box->replay)
    { /* restored from outside the process: from the start again */
      if (box->anchoridx > lua_gettop(L) || (box->stop == LS_READ) != (lua_gettop(L) > box->anchoridx) ||
          lua_gettop(L) > box->anchoridx + 1)
      {
        lua_pushistringref(L, lt::invalid_state_of_a_stopped_load);
        return LUA_ERRSYNTAX;
      }
      if (box->stop == LS_READ)
        lua_setiuservalue(L, ls.boxidx, 1); /* (the piece, until the read gets it) */
      lua_settop(L, box->anchoridx - 1);
      box->replay = false;
      rz.replaying = true;
      sreader.given = (ls.readeridx < 0); /* (a chunk in the box: the replay gives it first) */
      rreader.rz = &rz;
      rreader.next = reader;
      z.reader = &rreader;
      lua_lock(L);
      int st = luaD_protectedparser(L, &z, ls.name, ls.mode, &rz);
      lua_unlock(L);
      return ended(L, &rz, st);
    }
    z.resuming = true;
    rz.rpos = box->frames.size();
    std::optional<byte_view> s;
    sreader.given = box->given;
    if (box->zsrc == ZS_STRING && ls.readeridx == 0 && (s = lua_tolstring(L, 1)))
      z.chunk = *s;
    else if (box->zsrc == ZS_PIECE && ls.readeridx != 0 && (s = lua_tolstring(L, ls.pieceidx)))
      z.chunk = *s;
    else if (box->zsrc == ZS_INPUT)
      z.chunk = byte_view(box->input.data(), box->input.size());
    else if (box->zsrc != ZS_NONE)
    {
      lua_pushistringref(L, lt::invalid_state_of_a_stopped_load);
      return LUA_ERRSYNTAX;
    }
    if (box->zpos > z.chunk.size())
    {
      lua_pushistringref(L, lt::invalid_state_of_a_stopped_load);
      return LUA_ERRSYNTAX;
    }
    z.pos = box->zpos;
    rz.points = box->points;
  }
  lua_lock(L);
  int st = luaD_protectedparser(L, &z, ls.name, ls.mode, &rz);
  lua_unlock(L);
  return ended(L, &rz, st);
}

/*
** The parser's arrays and buffer, and its anchor table (at stack index
** 'anchoridx', the first time), go to the box of a load a freeze stopped:
** the box keeps them, and what is half made is not left in the stack (it is
** not kept outside the process).
*/
void luaZ_hold(LoadResume* rz, CallInfo* ci, int anchoridx)
{
  lua_State* L = rz->L;
  LoadBox* box = rz->box;
  Dyndata* dyd = rz->dyd;
  MBuffer* buff = rz->buff;
  size_t b[NHELD];
  heldblocks(dyd, buff, b);
  assert(b[0] + b[1] + b[2] + b[3] <= box->room); /* (as at the stop point) */
  for (size_t n : b)
    luaM_freeblock(L, n); /* (counted in the box's room now) */
  box->dyd = *dyd;
  *dyd = Dyndata{};
  box->buff = buff->release(&box->bufflen);
  if (box->anchoridx == 0)
    box->anchoridx = anchoridx;
  TValue bv = getslot(L, ci->func + rz->boxidx);
  Udata* u = uvalue(&bv);
  TValue a = getslot(L, ci->func + box->anchoridx);
  copyobj(&u->uv[0], &a);
  luaC_barrier(L, u, &u->uv[0]);
  setslotnil(L, ci->func + box->anchoridx);
  box->holds = true;
}

/*
** ... and come back from it when the load goes on (a memory error if they
** cannot be counted on their own again: the box still holds them)
*/
void luaZ_unhold(LoadResume* rz, CallInfo* ci)
{
  lua_State* L = rz->L;
  LoadBox* box = rz->box;
  Dyndata* dyd = rz->dyd;
  MBuffer* buff = rz->buff;
  size_t b[NHELD];
  b[0] = static_cast<size_t>(box->dyd.actvar.size) * cost::Vardesc;
  b[1] = static_cast<size_t>(box->dyd.gt.size) * cost::Labeldesc;
  b[2] = static_cast<size_t>(box->dyd.label.size) * cost::Labeldesc;
  b[3] = box->buff.size() * cost::Byte;
  int i = 0;
  try
  {
    for (; i < NHELD; i++)
      luaM_takeblock(L, b[i]);
  }
  catch (...)
  {
    while (i-- > 0)
      luaM_freeblock(L, b[i]);
    throw;
  }
  *dyd = box->dyd;
  box->dyd = Dyndata{};
  buff->adopt(std::move(box->buff), box->bufflen);
  box->bufflen = 0;
  if (box->room != 0 && luaM_shrinkhost(box->g, box->hosted, box->hosted - box->room))
  { /* (else the box keeps its room) */
    box->hosted -= box->room;
    box->room = 0;
  }
  TValue bv = getslot(L, ci->func + rz->boxidx);
  Udata* u = uvalue(&bv);
  TValue a = getobj(&u->uv[0]);
  setslot(L, ci->func + box->anchoridx, &a);
  setnilvalue(&u->uv[0]);
  box->holds = false;
}

}
