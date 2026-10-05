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
  lua_Unsigned x = 0;
  int b;
  limit >>= 7;
  do
  {
    b = loadByte(S);
    if (x > limit)
      error(S, lt::integer_overflow);
    x = (x << 7) | (b & 0x7f);
  } while ((b & 0x80) != 0);
  return x;
}

static size_t loadSize(LoadState* S)
{
  return loadVarint(S, MAX_SIZE);
}

static int loadInt(LoadState* S)
{
  return (int)(loadVarint(S, INT_MAX));
}

static lua_Number loadNumber(LoadState* S)
{
  lua_Number x;
  loadVar(S, x);
  return x;
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
  lua_State* L = S->L;
  TString* ts;
  TValue sv;
  size_t size = loadSize(S);
  if (size == 0)
  {                                                    /* previously saved string? */
    lua_Unsigned idx = loadVarint(S, LUA_MAXUNSIGNED); /* get its index */
    TValue stv;
    if (idx == 0)
    {                          /* no string? */
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
  else if ((size -= 1) <= 40)
  {
    char buff[41];
    loadVector(S, std::span<char>(buff, size + 1)); /* load string into buffer */
    byte_view buffstr{buff, size};
    ts = TString::luaS_createstr(L, buffstr); /* create string */
    setslot(ts);
    luaC_objbarrier(L, p, ts);
  }
  else
  {
    MBuffer tmp{L};
    tmp.growTo(size + 1);
    loadVector(S, std::span<char>(tmp.area().data(), size + 1));
    byte_view tmpsv{tmp.area().data(), size};
    ts = TString::luaS_createstr(L, tmpsv);
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
  int n = loadInt(S);
  loadAlign(S, sizeof(f->code[0]));
  if (S->fixed)
  {
    f->code = getaddr<Instruction>(S, n);
  }
  else
  {
    f->code = luaM_alloc_Instruction_span(S->L->l_G, S->L, {}, n, true);
    loadVector(S, f->code);
  }
}

static void loadFunction(LoadState* S, Proto* f);

static void loadConstants(LoadState* S, Proto* f)
{
  int i;
  int n = loadInt(S);
  f->k = luaM_alloc_TValue_span(S->L->l_G, S->L, {}, n, true);
  for (i = 0; i < n; i++)
    setnilvalue(&f->k[i]);
  for (i = 0; i < n; i++)
  {
    TValue* o = &f->k[i];
    int t = loadByte(S);
    switch (t)
    {
    case LUA_VNIL:
      setnilvalue(o);
      break;
    case LUA_VFALSE:
      (o->tt_ = LUA_VFALSE);
      break;
    case LUA_VTRUE:
      (o->tt_ = LUA_VTRUE);
      break;
    case LUA_VNUMFLT:
      setfltvalue(o, loadNumber(S));
      break;
    case LUA_VNUMINT:
      setivalue(o, loadInteger(S));
      break;
    case LUA_VSHRSTR:
    {
      assert(f->source == nullptr);
      loadString(S, f, [f](TString* ts) { f->source = ts; }); /* use 'source' to anchor string */
      if (f->source == nullptr)
        error(S, lt::bad_format_for_constant_string);
      setsvalue(S->L, o, f->source); /* save it in the right place */
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
  int i;
  int n = loadInt(S);
  f->p = luaM_alloc_Proto_ptr_span(S->L->l_G, S->L, {}, n, true);
  for (i = 0; i < n; i++)
    f->p[i] = nullptr;
  for (i = 0; i < n; i++)
  {
    f->p[i] = Proto::luaF_newproto(S->L);
    luaC_objbarrier(S->L, f, f->p[i]);
    loadFunction(S, f->p[i]);
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
  int i;
  int n = loadInt(S);
  f->upvalues = luaM_alloc_Upvaldesc_span(S->L->l_G, S->L, {}, n, true);
  for (i = 0; i < n; i++) /* make array valid for GC */
    f->upvalues[i].name = nullptr;
  for (i = 0; i < n; i++)
  { /* following calls can raise errors */
    f->upvalues[i].instack = loadByte(S);
    f->upvalues[i].idx = loadByte(S);
    f->upvalues[i].kind = loadByte(S);
  }
}

static void loadDebug(LoadState* S, Proto* f)
{
  int i;
  int n = loadInt(S);
  if (S->fixed)
  {
    f->lineinfo = getaddr<ls_byte>(S, n);
  }
  else
  {
    f->lineinfo = luaM_alloc_ls_byte_span(S->L->l_G, S->L, {}, n, true);
    loadVector(S, f->lineinfo);
  }
  n = loadInt(S);
  if (n > 0)
  {
    loadAlign(S, sizeof(int));
    if (S->fixed)
    {
      f->abslineinfo = getaddr<AbsLineInfo>(S, n);
    }
    else
    {
      f->abslineinfo = luaM_alloc_AbsLineInfo_span(S->L->l_G, S->L, {}, n, true);
      loadVector(S, f->abslineinfo);
    }
  }
  n = loadInt(S);
  f->locvars = luaM_alloc_LocVar_span(S->L->l_G, S->L, {}, n, true);
  for (i = 0; i < n; i++)
    f->locvars[i].varname = nullptr;
  for (i = 0; i < n; i++)
  {
    loadString(S, f, [f, i](TString* ts) { f->locvars[i].varname = ts; });
    f->locvars[i].startpc = loadInt(S);
    f->locvars[i].endpc = loadInt(S);
  }
  n = loadInt(S);
  if (n != 0)            /* does it have debug information? */
    n = (int)f->upvalues.size(); /* must be this many */
  for (i = 0; i < n; i++)
    loadString(S, f, [f, i](TString* ts) { f->upvalues[i].name = ts; });
}

static void loadFunction(LoadState* S, Proto* f)
{
  f->linedefined = loadInt(S);
  f->lastlinedefined = loadInt(S);
  f->numparams = loadByte(S);
  /* get only the meaningful flags */
  f->flag = loadByte(S) & ~PF_FIXED;
  if (S->fixed)
    f->flag |= PF_FIXED; /* signal that code is fixed */
  f->maxstacksize = loadByte(S);
  loadCode(S, f);
  loadConstants(S, f);
  loadUpvalues(S, f);
  loadProtos(S, f);
  loadString(S, f, [f](TString* ts) { f->source = ts; });
  loadDebug(S, f);
}

static void checkliteral(LoadState* S, byte_view s, byte_view msg)
{
  char buff[LUA_SIGNATURE.size() + lt::LUAC_DATA.size()]; /* larger than both */
  size_t len = s.size();
  loadVector(S, std::span<char>(buff, len));
  if (!s.equals(byte_view(buff, len)))
    error(S, msg);
}

[[noreturn]] static void numerror(LoadState* S, byte_view what, byte_view tname)
{
  byte_view msg = luaO_pushfstring(S->L, lt::s_s_mismatch, FmtArgs().str(tname).str(what))->view();
  error(S, msg);
}

static void checknumsize(LoadState* S, int size, byte_view tname)
{
  if (size != loadByte(S))
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
  T i;
  checknumsize(S, sizeof(i), tname);
  loadVar(S, i);
  checknumformat(S, i == v, tname);
}

static void checkHeader(LoadState* S)
{
  /* skip 1st char (already read and checked) */
  byte_view sig_tail = byte_view(LUA_SIGNATURE).substr(1);
  checkliteral(S, sig_tail, lt::not_a_binary_chunk);
  if (loadByte(S) != LUAC_VERSION)
    error(S, lt::version_mismatch);
  if (loadByte(S) != LUAC_FORMAT)
    error(S, lt::format_mismatch);
  checkliteral(S, lt::LUAC_DATA, lt::corrupted_chunk);
  checknum<int>(S, LUAC_INT, lt::int_kw);
  checknum<Instruction>(S, LUAC_INST, lt::instruction);
  checknum<lua_Integer>(S, LUAC_INT, lt::Lua_integer);
  checknum<lua_Number>(S, LUAC_NUM, lt::Lua_number);
}

/*
** Load precompiled chunk.
*/
LClosure* luaU_undump(lua_State* L, ZIO* Z, Table* anchor, byte_view name, int fixed)
{
  LoadState S;
  LClosure* cl;
  int c0 = sv_byteat(name, 0);
  if (c0 == '@' || c0 == '=')
  {
    name = byte_view(name).substr(1);
  }
  else if (c0 == LUA_SIGNATURE[0])
  {
    name = lt::binary_string;
  }
  S.name = name;
  S.L = L;
  S.Z = Z;
  S.fixed = fixed != 0;
  S.offset = 1; /* fist byte was already read */
  checkHeader(&S);
  S.h = anchor;
  S.nstr = 0;
  cl = LClosure::luaF_newLclosure(L, loadByte(&S));
  assert(novariant(cl->tt) >= LUA_TSTRING);
  luaD_anchorobj(L, anchor, cl);
  cl->p = Proto::luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  loadFunction(&S, cl->p);
  if (cl->upvals.size() != cl->p->upvalues.size())
    error(&S, lt::corrupted_chunk);
  return cl;
}

}
