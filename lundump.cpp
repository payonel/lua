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
  lua_State* L;
  ZIO* Z;
  byte_view name;
  Table* h;          /* list for string reuse */
  size_t offset;     /* current position relative to beginning of dump */
  lua_Unsigned nstr; /* number of strings in the list */
  bool fixed;        /* dump is fixed in memory */
  MBuffer* buff;     /* for long strings (kept with a load a freeze stopped) */
};

/*
** A load that a freeze can stop (see lload.h): a call at site 'n' of a
** function's frame records the frame (its locals, 'l') if a freeze passes
** through it; the function made again goes to the site of its frame. A
** check point at site 'n' is a leaf: made again, the load goes on after it.
*/
#define LZ_SITE(n, ...)                            \
  site_##n:                                        \
  try                                              \
  {                                                \
    __VA_ARGS__;                                   \
  }                                                \
  catch (...)                                      \
  {                                                \
    luaZ_record(S->Z->rz, FN, n, &l, sizeof(l));   \
    throw;                                         \
  }

#define LZ_CHECK(n)                                                                  \
  if (l_unlikely(S->Z->rz != nullptr) && luaZ_checkdue(S->Z->rz, S->L))              \
  {                                                                                  \
    try                                                                              \
    {                                                                                \
      luaZ_checkpoint(S->Z->rz);                                                     \
    }                                                                                \
    catch (...)                                                                      \
    {                                                                                \
      luaZ_record(S->Z->rz, FN, n, &l, sizeof(l));                                   \
      throw;                                                                         \
    }                                                                                \
  }                                                                                  \
  site_##n:

#define LZ_RESUMING() l_unlikely(S->Z->resuming)
#define LZ_SITENO() luaZ_restore(S->Z->rz, FN, &l, sizeof(l))
#define LZ_BADSITE() luaZ_badstate(S->Z->rz)
#define LZ_LEAF(n)                \
  case n:                         \
    luaZ_resumed(S->Z->rz);       \
    goto site_##n

struct NoLocals
{
  char none = 0;
};

[[noreturn]] static void error(LoadState* S, byte_view why)
{
  luaO_pushfstring(S->L, lt::s_bad_binary_format_s, FmtArgs().str(S->name).str(why));
  luaD_throw(S->L, LUA_ERRSYNTAX);
}

/*
** All high-level loads go through loadVector; you can change it to
** adapt to the endianness of the input
*/

static void loadBlock(LoadState* S, std::span<std::byte> b)
{
  constexpr int FN = LF_LOADBLOCK;
  struct
  {
    size_t r;
  } l;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  LZ_SITE(1, l.r = luaZ_read(S->Z, b));
  if (l.r != 0)
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

static void loadAlign(LoadState* S, size_t align)
{
  constexpr int FN = LF_LOADALIGN;
  struct
  {
    size_t padding;
    std::byte paddingContent[sizeof(lua_Integer)];
  } l;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  l.padding = align - S->offset % align;
  if (l.padding < align)
  { /* (padding == align) means no padding */
    LZ_SITE(1, loadBlock(S, std::span<std::byte>(l.paddingContent).first(l.padding)));
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

static lu_byte loadByte(LoadState* S)
{
  constexpr int FN = LF_LOADBYTE;
  struct
  {
    int b;
  } l;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  LZ_SITE(1, l.b = (S->Z->pos < S->Z->chunk.size())
                       ? (unsigned char)S->Z->chunk[S->Z->pos++]
                       : luaZ_fill(S->Z));
  if (l.b == EOZ)
    error(S, lt::truncated_chunk);
  S->offset++;
  return (lu_byte)l.b;
}

static lua_Unsigned loadVarint(LoadState* S, lua_Unsigned limit)
{
  constexpr int FN = LF_LOADVARINT;
  struct
  {
    lua_Unsigned x;
    lua_Unsigned limit;
    int b;
  } l;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  l.x = 0;
  l.limit = limit >> 7;
  do
  {
    LZ_SITE(1, l.b = loadByte(S));
    if (l.x > l.limit)
      error(S, lt::integer_overflow);
    l.x = (l.x << 7) | (l.b & 0x7f);
  } while ((l.b & 0x80) != 0);
  return l.x;
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
  constexpr int FN = LF_LOADNUMBER;
  struct
  {
    lua_Number x;
  } l;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  LZ_SITE(1, loadVar(S, l.x));
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
  constexpr int FN = LF_LOADSTRING;
  struct
  {
    size_t size;
    lua_Unsigned idx;
    char buff[41];
  } l;
  lua_State* L = S->L;
  TString* ts;
  TValue sv;
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
  LZ_SITE(1, l.size = loadSize(S));
  if (l.size == 0)
  {                                                     /* previously saved string? */
    LZ_SITE(2, l.idx = loadVarint(S, LUA_MAXUNSIGNED)); /* get its index */
    TValue stv;
    if (l.idx == 0)
    { /* no string? */
      setslot(nullptr);
      return;
    }
    if (novariant(S->h->luaH_getint(l.idx, &stv)) != LUA_TSTRING)
      error(S, lt::invalid_string_index);
    assert(ttisstring(&stv));
    ts = tsvalue(&stv); /* get its value */
    setslot(ts);
    luaC_objbarrier(L, p, ts);
    return; /* do not save it again */
  }
  else if ((l.size -= 1) <= 40)
  {
    LZ_SITE(3, loadVector(S, std::span<char>(l.buff, l.size + 1))); /* load string into buffer */
    byte_view buffstr{l.buff, l.size};
    ts = TString::luaS_createstr(L, buffstr); /* create string */
    setslot(ts);
    luaC_objbarrier(L, p, ts);
  }
  else
  {
    S->buff->growTo(l.size + 1);
    LZ_SITE(4, loadVector(S, std::span<char>(S->buff->area().data(), l.size + 1)));
    byte_view tmpsv{S->buff->area().data(), l.size};
    ts = TString::luaS_createstr(L, tmpsv);
    S->buff->growTo(0);
    setslot(ts);
    luaC_objbarrier(L, p, ts);
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
  constexpr int FN = LF_LOADCODE;
  struct
  {
    int n;
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
  LZ_SITE(1, l.n = loadInt(S));
  LZ_SITE(2, loadAlign(S, sizeof(f->code[0])));
  if (S->fixed)
  {
    f->code = getaddr<Instruction>(S, l.n);
  }
  else
  {
    f->code = luaM_alloc_Instruction_span(S->L->l_G, S->L, {}, l.n, true);
    LZ_SITE(3, loadVector(S, f->code));
  }
}

static void loadFunction(LoadState* S, Proto* f);

static void loadConstants(LoadState* S, Proto* f)
{
  constexpr int FN = LF_LOADCONSTANTS;
  struct
  {
    int n;
    int i;
    int t;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    LZ_LEAF(2);
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    case 6: goto site_6;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, l.n = loadInt(S));
  f->k = luaM_alloc_TValue_span(S->L->l_G, S->L, {}, l.n, true);
  for (l.i = 0; l.i < l.n; l.i++)
    setnilvalue(&f->k[l.i]);
  for (l.i = 0; l.i < l.n; l.i++)
  {
    LZ_CHECK(2);
    LZ_SITE(3, l.t = loadByte(S));
    switch (l.t)
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
    {
      lua_Number x;
      LZ_SITE(4, x = loadNumber(S));
      setfltvalue(&f->k[l.i], x);
      break;
    }
    case LUA_VNUMINT:
    {
      lua_Integer x;
      LZ_SITE(5, x = loadInteger(S));
      setivalue(&f->k[l.i], x);
      break;
    }
    case LUA_VSHRSTR:
    {
      assert(f->source == nullptr);
      LZ_SITE(6, loadString(S, f, [f](TString* ts) { f->source = ts; })); /* use 'source' to anchor string */
      if (f->source == nullptr)
        error(S, lt::bad_format_for_constant_string);
      setsvalue(S->L, &f->k[l.i], f->source); /* save it in the right place */
      f->source = nullptr;
      break;
    }
    default:
      error(S, lt::invalid_constant);
    }
  }
}

static void loadProtos(LoadState* S, Proto* f)
{
  constexpr int FN = LF_LOADPROTOS;
  struct
  {
    int n;
    int i;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, l.n = loadInt(S));
  f->p = luaM_alloc_Proto_ptr_span(S->L->l_G, S->L, {}, l.n, true);
  for (l.i = 0; l.i < l.n; l.i++)
    f->p[l.i] = nullptr;
  for (l.i = 0; l.i < l.n; l.i++)
  {
    f->p[l.i] = Proto::luaF_newproto(S->L);
    luaC_objbarrier(S->L, f, f->p[l.i]);
    LZ_SITE(2, loadFunction(S, f->p[l.i]));
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
  constexpr int FN = LF_LOADUPVALUES;
  struct
  {
    int n;
    int i;
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
  LZ_SITE(1, l.n = loadInt(S));
  f->upvalues = luaM_alloc_Upvaldesc_span(S->L->l_G, S->L, {}, l.n, true);
  for (l.i = 0; l.i < l.n; l.i++) /* make array valid for GC */
    f->upvalues[l.i].name = nullptr;
  for (l.i = 0; l.i < l.n; l.i++)
  { /* following calls can raise errors */
    LZ_SITE(2, f->upvalues[l.i].instack = loadByte(S));
    LZ_SITE(3, f->upvalues[l.i].idx = loadByte(S));
    LZ_SITE(4, f->upvalues[l.i].kind = loadByte(S));
  }
}

static void loadDebug(LoadState* S, Proto* f)
{
  constexpr int FN = LF_LOADDEBUG;
  struct
  {
    int n;
    int i;
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
    case 6: goto site_6;
    LZ_LEAF(7);
    case 8: goto site_8;
    case 9: goto site_9;
    case 10: goto site_10;
    case 11: goto site_11;
    case 12: goto site_12;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, l.n = loadInt(S));
  if (S->fixed)
  {
    f->lineinfo = getaddr<ls_byte>(S, l.n);
  }
  else
  {
    f->lineinfo = luaM_alloc_ls_byte_span(S->L->l_G, S->L, {}, l.n, true);
    LZ_SITE(2, loadVector(S, f->lineinfo));
  }
  LZ_SITE(3, l.n = loadInt(S));
  if (l.n > 0)
  {
    LZ_SITE(4, loadAlign(S, sizeof(int)));
    if (S->fixed)
    {
      f->abslineinfo = getaddr<AbsLineInfo>(S, l.n);
    }
    else
    {
      f->abslineinfo = luaM_alloc_AbsLineInfo_span(S->L->l_G, S->L, {}, l.n, true);
      LZ_SITE(5, loadVector(S, f->abslineinfo));
    }
  }
  LZ_SITE(6, l.n = loadInt(S));
  f->locvars = luaM_alloc_LocVar_span(S->L->l_G, S->L, {}, l.n, true);
  for (l.i = 0; l.i < l.n; l.i++)
    f->locvars[l.i].varname = nullptr;
  for (l.i = 0; l.i < l.n; l.i++)
  {
    LZ_CHECK(7);
    LZ_SITE(8, loadString(S, f, [f, &l](TString* ts) { f->locvars[l.i].varname = ts; }));
    LZ_SITE(9, f->locvars[l.i].startpc = loadInt(S));
    LZ_SITE(10, f->locvars[l.i].endpc = loadInt(S));
  }
  LZ_SITE(11, l.n = loadInt(S));
  if (l.n != 0)                    /* does it have debug information? */
    l.n = (int)f->upvalues.size(); /* must be this many */
  for (l.i = 0; l.i < l.n; l.i++)
  {
    LZ_SITE(12, loadString(S, f, [f, &l](TString* ts) { f->upvalues[l.i].name = ts; }));
  }
}

static void loadFunction(LoadState* S, Proto* f)
{
  constexpr int FN = LF_LOADFUNCTION;
  NoLocals l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    LZ_LEAF(1);
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
    default: LZ_BADSITE();
    }
  }
  LZ_CHECK(1);
  LZ_SITE(2, f->linedefined = loadInt(S));
  LZ_SITE(3, f->lastlinedefined = loadInt(S));
  LZ_SITE(4, f->numparams = loadByte(S));
  /* get only the meaningful flags */
  LZ_SITE(5, f->flag = loadByte(S) & ~PF_FIXED);
  if (S->fixed)
    f->flag |= PF_FIXED; /* signal that code is fixed */
  LZ_SITE(6, f->maxstacksize = loadByte(S));
  LZ_SITE(7, loadCode(S, f));
  LZ_SITE(8, loadConstants(S, f));
  LZ_SITE(9, loadUpvalues(S, f));
  LZ_SITE(10, loadProtos(S, f));
  LZ_SITE(11, loadString(S, f, [f](TString* ts) { f->source = ts; }));
  LZ_SITE(12, loadDebug(S, f));
}

static void checkliteral(LoadState* S, byte_view s, byte_view msg)
{
  constexpr int FN = LF_CHECKLITERAL;
  struct
  {
    char buff[LUA_SIGNATURE.size() + lt::LUAC_DATA.size()]; /* larger than both */
  } l;
  size_t len = s.size();
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  LZ_SITE(1, loadVector(S, std::span<char>(l.buff, len)));
  if (!s.equals(byte_view(l.buff, len)))
    error(S, msg);
}

[[noreturn]] static void numerror(LoadState* S, byte_view what, byte_view tname)
{
  byte_view msg = luaO_pushfstring(S->L, lt::s_s_mismatch, FmtArgs().str(tname).str(what))->view();
  error(S, msg);
}

static void checknumsize(LoadState* S, int size, byte_view tname)
{
  constexpr int FN = LF_CHECKNUMSIZE;
  struct
  {
    int b;
  } l;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  LZ_SITE(1, l.b = loadByte(S));
  if (size != l.b)
    numerror(S, lt::size, tname);
}

static void checknumformat(LoadState* S, int eq, byte_view tname)
{
  if (!eq)
    numerror(S, lt::format, tname);
}

template <typename T>
static void checknum(LoadState* S, T v, byte_view tname)
{
  constexpr int FN = LF_CHECKNUM;
  struct
  {
    T i;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, checknumsize(S, sizeof(l.i), tname));
  LZ_SITE(2, loadVar(S, l.i));
  checknumformat(S, l.i == v, tname);
}

static void checkHeader(LoadState* S)
{
  constexpr int FN = LF_CHECKHEADER;
  struct
  {
    int b;
  } l;
  /* skip 1st char (already read and checked) */
  byte_view sig_tail = byte_view(LUA_SIGNATURE).substr(1);
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
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, checkliteral(S, sig_tail, lt::not_a_binary_chunk));
  LZ_SITE(2, l.b = loadByte(S));
  if (l.b != LUAC_VERSION)
    error(S, lt::version_mismatch);
  LZ_SITE(3, l.b = loadByte(S));
  if (l.b != LUAC_FORMAT)
    error(S, lt::format_mismatch);
  LZ_SITE(4, checkliteral(S, lt::LUAC_DATA, lt::corrupted_chunk));
  LZ_SITE(5, checknum<int>(S, LUAC_INT, lt::int_kw));
  LZ_SITE(6, checknum<Instruction>(S, LUAC_INST, lt::instruction));
  LZ_SITE(7, checknum<lua_Integer>(S, LUAC_INT, lt::Lua_integer));
  LZ_SITE(8, checknum<lua_Number>(S, LUAC_NUM, lt::Lua_number));
}

/*
** Load precompiled chunk.
*/
LClosure* luaU_undump(lua_State* L, ZIO* Z, Table* anchor, MBuffer* buff, byte_view name, int fixed)
{
  constexpr int FN = LF_UNDUMP;
  struct
  {
    LoadState S;
    LClosure* cl;
    int nup;
  } l;
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
    int site = luaZ_restore(Z->rz, FN, &l, sizeof(l));
    l.S.L = L;
    l.S.Z = Z;
    l.S.h = anchor;
    l.S.name = name;
    l.S.buff = buff;
    switch (site)
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    default: luaZ_badstate(Z->rz);
    }
  }
  l.S.name = name;
  l.S.L = L;
  l.S.Z = Z;
  l.S.fixed = fixed != 0;
  l.S.buff = buff;
  l.S.offset = 1; /* fist byte was already read */
  LZ_SITE(1, checkHeader(S));
  l.S.h = anchor;
  l.S.nstr = 0;
  LZ_SITE(2, l.nup = loadByte(S));
  l.cl = LClosure::luaF_newLclosure(L, l.nup);
  assert(novariant(l.cl->tt) >= LUA_TSTRING);
  luaD_anchorobj(L, anchor, l.cl);
  l.cl->p = Proto::luaF_newproto(L);
  luaC_objbarrier(L, l.cl, l.cl->p);
  LZ_SITE(3, loadFunction(S, l.cl->p));
  if (l.cl->upvals.size() != l.cl->p->upvalues.size())
    error(S, lt::corrupted_chunk);
  return l.cl;
}

}
