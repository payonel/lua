/*
** $Id: lundump.c $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/


#include <limits.h>
#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstring.h"
#include "ltable.h"
#include "lundump.h"
#include "lzio.h"
#include "lload.h"
#include "lstate.h"

#include "lauxlib.h"
#include "literals.h"

namespace lua::compiler
{

struct LoadState
{
  lua_State* L = nullptr;
  ZIO* Z = nullptr;
  byte_view name;
  Table* h = nullptr;     /* list for string reuse */
  size_t offset = 0;      /* current position relative to beginning of dump */
  lua_Unsigned nstr = 0;  /* number of strings in the list */
  bool fixed = false;     /* dump is fixed in memory */
  MBuffer* buff = nullptr; /* for long strings (kept with a load a freeze stopped) */
};

/*
** A load that a freeze can stop (see lload.h): a function with calls a
** freeze can pass through runs as a state machine, its state and its
** locals in its frame ('l'). Each such call is made at a state of its own,
** the one its function goes to again when it is made again after the
** thaw; a freeze passing records the frame. A check point is a leaf: made
** again, the function goes on after it. A function with no frame of its
** own (as 'loadByte') goes the same way when made again.
*/

/* the frames of the binary loader's functions (see lload.h) */
struct VarintFrame : LoadFrame
{
  lua_Unsigned x = 0;
  LoadFrame* copy() const override { return new (std::nothrow) VarintFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct LoadNumberFrame : LoadFrame
{
  lua_Number x = 0;
  LoadFrame* copy() const override { return new (std::nothrow) LoadNumberFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct LoadStringFrame : LoadFrame
{
  size_t len = 0; /* (the string's size) */
  char buff[41] = {};
  LoadFrame* copy() const override { return new (std::nothrow) LoadStringFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

/* (of the functions with a count and an index) */
struct CountFrame : LoadFrame
{
  int n = 0;
  int i = 0;
  LoadFrame* copy() const override { return new (std::nothrow) CountFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct LiteralFrame : LoadFrame
{
  char buff[LUA_SIGNATURE.size() + lt::LUAC_DATA.size()] = {}; /* larger than both */
  LoadFrame* copy() const override { return new (std::nothrow) LiteralFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct CheckNumFrame : LoadFrame
{
  std::byte raw[sizeof(lua_Integer) > sizeof(lua_Number) ? sizeof(lua_Integer) : sizeof(lua_Number)] = {}; /* (the bytes read) */
  LoadFrame* copy() const override { return new (std::nothrow) CheckNumFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct UndumpFrame : LoadFrame
{
  LoadState S;
  LClosure* cl = nullptr;
  LoadFrame* copy() const override { return new (std::nothrow) UndumpFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

/* a check point of the load: a freeze can stop it there (the leaf of the frames) */
static void checkpoint(LoadState* S, LoadFrame& l, int site)
{
  LoadResume* rz = S->Z->rz;
  if (l_unlikely(rz != nullptr) && luaZ_checkdue(rz, S->L))
  {
    l.site = site;
    luaZ_checkpoint(rz);
  }
}

[[noreturn]] static void error(LoadState* S, byte_view why)
{
  luaO_pushfstring(S->L, lt::s_bad_binary_format_s, FmtArgs().str(S->name).str(why));
  luaD_throw(S->L, LUA_ERRSYNTAX);
}

/*
** All high-level loads go through loadVector; you can change it to
** adapt to the endianness of the input
*/

/* (with no frame of its own: made again, it reads the rest of 'b') */
static void loadBlock(LoadState* S, std::span<std::byte> b)
{
  if (luaZ_read(S->Z, b) != 0)
    error(S, lt::truncated_chunk);
  S->offset += b.size();
}

template <typename T>
static void loadVector(LoadState* S, std::span<T> v)
{
  loadBlock(S, std::as_writable_bytes(v));
}

template <typename T>
static void loadVar(LoadState* S, T& x)
{
  loadVector(S, std::span<T>(&x, 1));
}

/* (with no frame of its own: made again, it goes the same way; the padding's bytes are not kept) */
static void loadAlign(LoadState* S, size_t align)
{
  size_t padding = align - S->offset % align;
  if (padding < align)
  { /* (padding == align) means no padding */
    std::byte paddingContent[sizeof(lua_Integer)];
    loadBlock(S, std::span<std::byte>(paddingContent).first(padding));
    assert(S->offset % align == 0);
  }
}

template <typename T>
static std::span<T> getaddr(LoadState* S, int n)
{
  size_t size = n * sizeof(T);
  std::optional<byte_view> block = luaZ_getaddr(S->Z, size);
  S->offset += size;
  if (!block.has_value())
    error(S, lt::truncated_fixed_buffer);
  return luaZ_overlay<T>(*block);
}

/* (with no frame of its own: made again, it goes the same way) */
static lu_byte loadByte(LoadState* S)
{
  int b = (S->Z->pos < S->Z->chunk.size())
              ? (unsigned char)S->Z->chunk[S->Z->pos++]
              : luaZ_fill(S->Z);
  if (b == EOZ)
    error(S, lt::truncated_chunk);
  S->offset++;
  return (lu_byte)b;
}

static lua_Unsigned loadVarint(LoadState* S, lua_Unsigned limit)
{
  VarintFrame l;
  if (l_unlikely(S->Z->resuming))
    l = static_cast<const VarintFrame&>(luaZ_restore(S->Z->rz, LF_LOADVARINT));
  limit >>= 7;
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0: /* the loop */
      {
        int b = loadByte(S);
        if (l.x > limit)
          error(S, lt::integer_overflow);
        l.x = (l.x << 7) | (b & 0x7f);
        if ((b & 0x80) == 0)
          return l.x;
        continue;
      }
      default:
        luaZ_badstate(S->Z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_LOADVARINT, l);
    throw;
  }
}

static size_t loadSize(LoadState* S)
{
  return loadVarint(S, MAX_SIZE);
}

static int loadInt(LoadState* S)
{
  return (int)loadVarint(S, INT_MAX);
}

static lua_Number loadNumber(LoadState* S)
{
  LoadNumberFrame l;
  if (l_unlikely(S->Z->resuming))
    l = static_cast<const LoadNumberFrame&>(luaZ_restore(S->Z->rz, LF_LOADNUMBER));
  try
  {
    switch (l.site)
    {
    case 0:
      loadVar(S, l.x);
      break;
    default:
      luaZ_badstate(S->Z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_LOADNUMBER, l);
    throw;
  }
  return l.x;
}

static lua_Integer loadInteger(LoadState* S)
{
  lua_Unsigned cx = loadVarint(S, LUA_MAXUNSIGNED);
  /* decode unsigned to signed */
  if ((cx & 1) != 0)
    return ~(cx >> 1);
  else
    return cx >> 1;
}

/*
** Load a nullable string into slot 'sl' from prototype 'p'. The
** assignment to the slot and the barrier must be performed before any
** possible GC activity, to anchor the string. (Both 'loadVector' and
** 'luaH_setint' can call the GC.)
*/
template <typename SetSlot>
static void loadString(LoadState* S, Proto* p, SetSlot setslot)
{
  LoadStringFrame l;
  lua_State* L = S->L;
  TString* ts = nullptr;
  TValue sv;
  if (l_unlikely(S->Z->resuming))
    l = static_cast<const LoadStringFrame&>(luaZ_restore(S->Z->rz, LF_LOADSTRING));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.len = loadSize(S);
        if (l.len == 0) /* previously saved string? */
          l.site = 1;
        else if ((l.len -= 1) <= 40)
          l.site = 2;
        else
        {
          S->buff->growTo(l.len + 1);
          l.site = 3;
        }
        continue;
      case 1:
      {
        lua_Unsigned idx = loadVarint(S, LUA_MAXUNSIGNED); /* get its index */
        TValue stv;
        if (idx == 0)
        { /* no string? */
          setslot(nullptr);
          return;
        }
        if (novariant(S->h->luaH_getint(idx, &stv)) != LUA_TSTRING)
          error(S, lt::invalid_string_index);
        assert(ttisstring(&stv));
        ts = tsvalue(&stv); /* get its value */
        setslot(ts);
        luaC_objbarrier(L, p, ts);
        return; /* do not save it again */
      }
      case 2:
      {
        loadVector(S, std::span<char>(l.buff, l.len + 1)); /* load string into buffer */
        byte_view buffstr{l.buff, l.len};
        ts = TString::luaS_createstr(L, buffstr); /* create string */
        setslot(ts);
        luaC_objbarrier(L, p, ts);
        break;
      }
      case 3:
        loadVector(S, std::span<char>(S->buff->area().data(), l.len + 1));
        ts = TString::luaS_takeblock(L, S->buff->handover(), l.len); /* the buffer's bytes are the string's */
        setslot(ts);
        luaC_objbarrier(L, p, ts);
        break;
      default:
        luaZ_badstate(S->Z->rz);
      }
      break; /* (the string is made) */
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_LOADSTRING, l);
    throw;
  }
  /* add string to list of saved strings */
  S->nstr++;
  setsvalue(L, &sv, ts);
  S->h->luaH_setint(L, S->nstr, &sv);
  assert(novariant(S->h->tt) >= LUA_TSTRING);
  luaC_objbarrierback(L, S->h, ts);
}

static void loadCode(LoadState* S, Proto* f)
{
  CountFrame l;
  if (l_unlikely(S->Z->resuming))
    l = static_cast<const CountFrame&>(luaZ_restore(S->Z->rz, LF_LOADCODE));
  try
  {
    switch (l.site)
    {
    case 0:
      l.n = loadInt(S);
      l.site = 1;
      [[fallthrough]];
    case 1:
      loadAlign(S, sizeof(f->code[0]));
      if (S->fixed)
      {
        f->code = getaddr<Instruction>(S, l.n);
        break;
      }
      f->code = luaM_alloc_Instruction_span(S->L->l_G, S->L, {}, l.n, true);
      l.site = 2;
      [[fallthrough]];
    case 2:
      loadVector(S, f->code);
      break;
    default:
      luaZ_badstate(S->Z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_LOADCODE, l);
    throw;
  }
}

static void loadFunction(LoadState* S, Proto* f);

static void loadConstants(LoadState* S, Proto* f)
{
  CountFrame l;
  if (l_unlikely(S->Z->resuming))
  {
    l = static_cast<const CountFrame&>(luaZ_restore(S->Z->rz, LF_LOADCONSTANTS));
    if (l.site == 2)
    { /* (the check point is behind it) */
      luaZ_resumed(S->Z->rz);
      l.site = 3;
    }
  }
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.n = loadInt(S);
        f->k = luaM_alloc_TValue_span(S->L->l_G, S->L, {}, l.n, true);
        for (l.i = 0; l.i < l.n; l.i++)
          setnilvalue(&f->k[l.i]);
        l.i = 0;
        l.site = 1;
        [[fallthrough]];
      case 1: /* the loop */
        if (l.i >= l.n)
          return;
        checkpoint(S, l, 2);
        l.site = 3;
        [[fallthrough]];
      case 3:
        switch (loadByte(S))
        {
        case LUA_VNIL:
          setnilvalue(&f->k[l.i]);
          break;
        case LUA_VFALSE:
          (f->k[l.i].tt_ = LUA_VFALSE);
          break;
        case LUA_VTRUE:
          (f->k[l.i].tt_ = LUA_VTRUE);
          break;
        case LUA_VNUMFLT:
          l.site = 4;
          continue;
        case LUA_VNUMINT:
          l.site = 5;
          continue;
        case LUA_VSHRSTR:
          assert(f->source == nullptr);
          l.site = 6;
          continue;
        default:
          error(S, lt::invalid_constant);
        }
        l.i++;
        l.site = 1;
        continue;
      case 4:
      {
        lua_Number x = loadNumber(S);
        setfltvalue(&f->k[l.i], x);
        l.i++;
        l.site = 1;
        continue;
      }
      case 5:
      {
        lua_Integer x = loadInteger(S);
        setivalue(&f->k[l.i], x);
        l.i++;
        l.site = 1;
        continue;
      }
      case 6:
        loadString(S, f, [f](TString* ts) { f->source = ts; }); /* use 'source' to anchor string */
        if (f->source == nullptr)
          error(S, lt::bad_format_for_constant_string);
        setsvalue(S->L, &f->k[l.i], f->source); /* save it in the right place */
        f->source = nullptr;
        l.i++;
        l.site = 1;
        continue;
      default:
        luaZ_badstate(S->Z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_LOADCONSTANTS, l);
    throw;
  }
}

static void loadProtos(LoadState* S, Proto* f)
{
  CountFrame l;
  if (l_unlikely(S->Z->resuming))
    l = static_cast<const CountFrame&>(luaZ_restore(S->Z->rz, LF_LOADPROTOS));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.n = loadInt(S);
        f->p = luaM_alloc_Proto_ptr_span(S->L->l_G, S->L, {}, l.n, true);
        for (l.i = 0; l.i < l.n; l.i++)
          f->p[l.i] = nullptr;
        l.i = 0;
        l.site = 1;
        [[fallthrough]];
      case 1: /* the loop */
        if (l.i >= l.n)
          return;
        f->p[l.i] = Proto::luaF_newproto(S->L);
        luaC_objbarrier(S->L, f, f->p[l.i]);
        l.site = 2;
        [[fallthrough]];
      case 2:
        loadFunction(S, f->p[l.i]);
        l.i++;
        l.site = 1;
        continue;
      default:
        luaZ_badstate(S->Z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_LOADPROTOS, l);
    throw;
  }
}

/*
** Load the upvalues for a function. The names must be filled first,
** because the filling of the other fields can raise read errors and
** the creation of the error message can call an emergency collection;
** in that case all prototypes must be consistent for the GC.
*/
static void loadUpvalues(LoadState* S, Proto* f)
{
  CountFrame l;
  if (l_unlikely(S->Z->resuming))
    l = static_cast<const CountFrame&>(luaZ_restore(S->Z->rz, LF_LOADUPVALUES));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.n = loadInt(S);
        f->upvalues = luaM_alloc_Upvaldesc_span(S->L->l_G, S->L, {}, l.n, true);
        for (l.i = 0; l.i < l.n; l.i++) /* make array valid for GC */
          f->upvalues[l.i].name = nullptr;
        l.i = 0;
        l.site = 1;
        [[fallthrough]];
      case 1: /* the loop (following calls can raise errors) */
        if (l.i >= l.n)
          return;
        l.site = 2;
        [[fallthrough]];
      case 2:
        f->upvalues[l.i].instack = loadByte(S) != 0;
        l.site = 3;
        [[fallthrough]];
      case 3:
        f->upvalues[l.i].idx = loadByte(S);
        l.site = 4;
        [[fallthrough]];
      case 4:
        f->upvalues[l.i].kind = loadByte(S);
        l.i++;
        l.site = 1;
        continue;
      default:
        luaZ_badstate(S->Z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_LOADUPVALUES, l);
    throw;
  }
}

static void loadDebug(LoadState* S, Proto* f)
{
  CountFrame l;
  if (l_unlikely(S->Z->resuming))
  {
    l = static_cast<const CountFrame&>(luaZ_restore(S->Z->rz, LF_LOADDEBUG));
    if (l.site == 7)
    { /* (the check point is behind it) */
      luaZ_resumed(S->Z->rz);
      l.site = 8;
    }
  }
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.n = loadInt(S);
        if (S->fixed)
        {
          f->lineinfo = getaddr<ls_byte>(S, l.n);
          l.site = 2;
          continue;
        }
        f->lineinfo = luaM_alloc_ls_byte_span(S->L->l_G, S->L, {}, l.n, true);
        l.site = 1;
        [[fallthrough]];
      case 1:
        loadVector(S, f->lineinfo);
        l.site = 2;
        [[fallthrough]];
      case 2:
        l.n = loadInt(S);
        if (l.n <= 0)
        {
          l.site = 5;
          continue;
        }
        l.site = 3;
        [[fallthrough]];
      case 3:
        loadAlign(S, sizeof(int));
        if (S->fixed)
        {
          f->abslineinfo = getaddr<AbsLineInfo>(S, l.n);
          l.site = 5;
          continue;
        }
        f->abslineinfo = luaM_alloc_AbsLineInfo_span(S->L->l_G, S->L, {}, l.n, true);
        l.site = 4;
        [[fallthrough]];
      case 4:
        loadVector(S, f->abslineinfo);
        l.site = 5;
        [[fallthrough]];
      case 5:
        l.n = loadInt(S);
        f->locvars = luaM_alloc_LocVar_span(S->L->l_G, S->L, {}, l.n, true);
        for (l.i = 0; l.i < l.n; l.i++)
          f->locvars[l.i].varname = nullptr;
        l.i = 0;
        l.site = 6;
        [[fallthrough]];
      case 6: /* the loop */
        if (l.i >= l.n)
        {
          l.site = 11;
          continue;
        }
        checkpoint(S, l, 7);
        l.site = 8;
        [[fallthrough]];
      case 8:
        loadString(S, f, [f, i = l.i](TString* ts) { f->locvars[i].varname = ts; });
        l.site = 9;
        [[fallthrough]];
      case 9:
        f->locvars[l.i].startpc = loadInt(S);
        l.site = 10;
        [[fallthrough]];
      case 10:
        f->locvars[l.i].endpc = loadInt(S);
        l.i++;
        l.site = 6;
        continue;
      case 11:
        l.n = loadInt(S);
        if (l.n != 0)                    /* does it have debug information? */
          l.n = (int)f->upvalues.size(); /* must be this many */
        l.i = 0;
        l.site = 12;
        [[fallthrough]];
      case 12: /* the loop */
        if (l.i >= l.n)
          return;
        l.site = 13;
        [[fallthrough]];
      case 13:
        loadString(S, f, [f, i = l.i](TString* ts) { f->upvalues[i].name = ts; });
        l.i++;
        l.site = 12;
        continue;
      default:
        luaZ_badstate(S->Z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_LOADDEBUG, l);
    throw;
  }
}

static void loadFunction(LoadState* S, Proto* f)
{
  LoadFrame l;
  if (l_unlikely(S->Z->resuming))
  {
    l = luaZ_restore(S->Z->rz, LF_LOADFUNCTION);
    if (l.site == 1)
    { /* (the check point is behind it) */
      luaZ_resumed(S->Z->rz);
      l.site = 2;
    }
  }
  try
  {
    switch (l.site)
    {
    case 0:
      checkpoint(S, l, 1);
      l.site = 2;
      [[fallthrough]];
    case 2:
      f->linedefined = loadInt(S);
      l.site = 3;
      [[fallthrough]];
    case 3:
      f->lastlinedefined = loadInt(S);
      l.site = 4;
      [[fallthrough]];
    case 4:
      f->numparams = loadByte(S);
      l.site = 5;
      [[fallthrough]];
    case 5:
      /* get only the meaningful flags */
      f->flag = loadByte(S) & ~PF_FIXED;
      if (S->fixed)
        f->flag |= PF_FIXED; /* signal that code is fixed */
      l.site = 6;
      [[fallthrough]];
    case 6:
      f->maxstacksize = loadByte(S);
      l.site = 7;
      [[fallthrough]];
    case 7:
      loadCode(S, f);
      l.site = 8;
      [[fallthrough]];
    case 8:
      loadConstants(S, f);
      l.site = 9;
      [[fallthrough]];
    case 9:
      loadUpvalues(S, f);
      l.site = 10;
      [[fallthrough]];
    case 10:
      loadProtos(S, f);
      l.site = 11;
      [[fallthrough]];
    case 11:
      loadString(S, f, [f](TString* ts) { f->source = ts; });
      l.site = 12;
      [[fallthrough]];
    case 12:
      loadDebug(S, f);
      break;
    default:
      luaZ_badstate(S->Z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_LOADFUNCTION, l);
    throw;
  }
}

static void checkliteral(LoadState* S, byte_view s, byte_view msg)
{
  LiteralFrame l;
  size_t len = s.size();
  if (l_unlikely(S->Z->resuming))
    l = static_cast<const LiteralFrame&>(luaZ_restore(S->Z->rz, LF_CHECKLITERAL));
  try
  {
    switch (l.site)
    {
    case 0:
      loadVector(S, std::span<char>(l.buff, len));
      break;
    default:
      luaZ_badstate(S->Z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_CHECKLITERAL, l);
    throw;
  }
  if (!s.equals(byte_view(l.buff, len)))
    error(S, msg);
}

[[noreturn]] static void numerror(LoadState* S, byte_view what, byte_view tname)
{
  byte_view msg = luaO_pushfstring(S->L, lt::s_s_mismatch, FmtArgs().str(tname).str(what))->view();
  error(S, msg);
}

/* (with no frame of its own: made again, it goes the same way) */
static void checknumsize(LoadState* S, int size, byte_view tname)
{
  if (size != loadByte(S))
    numerror(S, lt::size, tname);
}

static void checknumformat(LoadState* S, bool eq, byte_view tname)
{
  if (!eq)
    numerror(S, lt::format, tname);
}

/* (its frame keeps the bytes read, as they come: the number is made of them at the end) */
template <typename T>
static void checknum(LoadState* S, T v, byte_view tname)
{
  CheckNumFrame l;
  if (l_unlikely(S->Z->resuming))
    l = static_cast<const CheckNumFrame&>(luaZ_restore(S->Z->rz, LF_CHECKNUM));
  try
  {
    switch (l.site)
    {
    case 0:
      checknumsize(S, sizeof(T), tname);
      l.site = 1;
      [[fallthrough]];
    case 1:
      loadVector(S, std::span<std::byte>(l.raw, sizeof(T)));
      break;
    default:
      luaZ_badstate(S->Z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_CHECKNUM, l);
    throw;
  }
  T i;
  memcpy(&i, l.raw, sizeof(T));
  checknumformat(S, i == v, tname);
}

static void checkHeader(LoadState* S)
{
  LoadFrame l;
  /* skip 1st char (already read and checked) */
  byte_view sig_tail = byte_view(LUA_SIGNATURE).substr(1);
  if (l_unlikely(S->Z->resuming))
    l = luaZ_restore(S->Z->rz, LF_CHECKHEADER);
  try
  {
    switch (l.site)
    {
    case 0:
      checkliteral(S, sig_tail, lt::not_a_binary_chunk);
      l.site = 1;
      [[fallthrough]];
    case 1:
      if (loadByte(S) != LUAC_VERSION)
        error(S, lt::version_mismatch);
      l.site = 2;
      [[fallthrough]];
    case 2:
      if (loadByte(S) != LUAC_FORMAT)
        error(S, lt::format_mismatch);
      l.site = 3;
      [[fallthrough]];
    case 3:
      checkliteral(S, lt::LUAC_DATA, lt::corrupted_chunk);
      l.site = 4;
      [[fallthrough]];
    case 4:
      checknum<int>(S, LUAC_INT, lt::int_kw);
      l.site = 5;
      [[fallthrough]];
    case 5:
      checknum<Instruction>(S, LUAC_INST, lt::instruction);
      l.site = 6;
      [[fallthrough]];
    case 6:
      checknum<lua_Integer>(S, LUAC_INT, lt::Lua_integer);
      l.site = 7;
      [[fallthrough]];
    case 7:
      checknum<lua_Number>(S, LUAC_NUM, lt::Lua_number);
      break;
    default:
      luaZ_badstate(S->Z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(S->Z->rz, LF_CHECKHEADER, l);
    throw;
  }
}

/*
** Load precompiled chunk.
*/
LClosure* luaU_undump(lua_State* L, ZIO* Z, Table* anchor, MBuffer* buff, byte_view name, bool fixed)
{
  UndumpFrame l;
  int c0 = sv_byteat(name, 0);
  if (c0 == '@' || c0 == '=')
  {
    name = byte_view(name).substr(1);
  }
  else if (c0 == LUA_SIGNATURE[0])
  {
    name = lt::binary_string;
  }
  LoadState* S = &l.S;
  if (l_unlikely(Z->resuming))
  { /* (the state again with this load's stream and buffer) */
    l = static_cast<const UndumpFrame&>(luaZ_restore(Z->rz, LF_UNDUMP));
    l.S.L = L;
    l.S.Z = Z;
    l.S.h = anchor;
    l.S.name = name;
    l.S.buff = buff;
  }
  else
  {
    l.S.name = name;
    l.S.L = L;
    l.S.Z = Z;
    l.S.fixed = fixed;
    l.S.buff = buff;
    l.S.offset = 1; /* fist byte was already read */
  }
  try
  {
    switch (l.site)
    {
    case 0:
      checkHeader(S);
      l.S.h = anchor;
      l.S.nstr = 0;
      l.site = 1;
      [[fallthrough]];
    case 1:
    {
      int nup = loadByte(S);
      l.cl = LClosure::luaF_newLclosure(L, nup);
      luaZ_madefunction(Z->rz, l.cl);
      assert(novariant(l.cl->tt) >= LUA_TSTRING);
      luaD_anchorobj(L, anchor, l.cl);
      l.cl->p = Proto::luaF_newproto(L);
      luaC_objbarrier(L, l.cl, l.cl->p);
      l.site = 2;
      [[fallthrough]];
    }
    case 2:
      loadFunction(S, l.cl->p);
      break;
    default:
      luaZ_badstate(Z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(Z->rz, LF_UNDUMP, l);
    throw;
  }
  if (l.cl->upvals.size() != l.cl->p->upvalues.size())
    error(S, lt::corrupted_chunk);
  return l.cl;
}

}
