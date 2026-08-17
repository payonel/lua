/*
** $Id: lundump.c $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/


#include <limits.h>
#include <string.h>
#include <string>

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

struct LoadState
{
  lua_State* L;
  ZIO* Z;
  byte_view name;
  Table* h;          /* list for string reuse */
  size_t offset;     /* current position relative to beginning of dump */
  lua_Unsigned nstr; /* number of strings in the list */
  lu_byte fixed;     /* dump is fixed in memory */
};

static l_noret error(LoadState* S, byte_view why)
{
  luaO_pushfstring(S->L, lt::s_bad_binary_format_s, FmtArgs().str(S->name).str(why));
  luaD_throw(S->L, LUA_ERRSYNTAX);
}

/*
** All high-level loads go through loadVector; you can change it to
** adapt to the endianness of the input
*/

static void loadBlock(LoadState* S, void* b, size_t size)
{
  if (luaZ_read(S->Z, b, size) != 0)
    error(S, lt::truncated_chunk);
  S->offset += size;
}

static void loadAlign(LoadState* S, unsigned align)
{
  unsigned padding = align - ((unsigned int)(S->offset % align));
  if (padding < align)
  { /* (padding == align) means no padding */
    lua_Integer paddingContent;
    loadBlock(S, &paddingContent, padding);
    assert(S->offset % align == 0);
  }
}

static const void* getaddr_(LoadState* S, size_t size)
{
  const void* block = luaZ_getaddr(S->Z, size);
  S->offset += size;
  if (block == nullptr)
    error(S, lt::truncated_fixed_buffer);
  return block;
}

static lu_byte loadByte(LoadState* S)
{
  int b = (S->Z->pos < S->Z->chunk.size())
              ? ((unsigned char)(S->Z->chunk[S->Z->pos++]))
              : luaZ_fill(S->Z);
  if (b == EOZ)
    error(S, lt::truncated_chunk);
  S->offset++;
  return ((lu_byte)(b));
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
  return ((size_t)(loadVarint(S, MAX_SIZE)));
}

static int loadInt(LoadState* S)
{
  return ((int)(loadVarint(S, ((size_t)(INT_MAX)))));
}

static lua_Number loadNumber(LoadState* S)
{
  lua_Number x;
  loadBlock(S, &(x), ((size_t)(1)) * sizeof((&(x))[0]));
  return x;
}

static lua_Integer loadInteger(LoadState* S)
{
  lua_Unsigned cx = loadVarint(S, LUA_MAXUNSIGNED);
  /* decode unsigned to signed */
  if ((cx & 1) != 0)
    return ((lua_Integer)(~(cx >> 1)));
  else
    return ((lua_Integer)(cx >> 1));
}

/*
** Load a nullable string into slot 'sl' from prototype 'p'. The
** assignment to the slot and the barrier must be performed before any
** possible GC activity, to anchor the string. (Both 'loadVector' and
** 'luaH_setint' can call the GC.)
*/
static void loadString(LoadState* S, Proto* p, TString** sl)
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
      assert(*sl == nullptr); /* must be prefilled */
      return;
    }
    if ((luaH_getint(S->h, ((lua_Integer)(idx)), &stv) & NOVARIANT_MASK) != LUA_TSTRING)
      error(S, lt::invalid_string_index);
    assert((stv.tt_ & NOVARIANT_MASK) == LUA_TSTRING);
    *sl = ts = tsvalue(&stv); /* get its value */
    luaC_objbarrier(L, p, ts);
    return; /* do not save it again */
  }
  else if ((size -= 1) <= 40)
  {
    char buff[41];
    loadBlock(S, buff, ((size_t)(size + 1)) * sizeof((buff)[0]));          /* load string into buffer */
    byte_view buffstr{buff, size};
    *sl = ts = luaS_createstr(L, buffstr, 0, buffstr.size()); /* create string */
    luaC_objbarrier(L, p, ts);
  }
  else
  {
    MBuffer tmp{L};
    tmp.growTo(size + 1);
    loadBlock(S, tmp.area().data(), ((size_t)(size + 1)) * sizeof(char));
    byte_view tmpsv{tmp.area().data(), size};
    *sl = ts = luaS_createstr(L, tmpsv, 0, tmpsv.size());
    luaC_objbarrier(L, p, ts);
  }
  /* add string to list of saved strings */
  S->nstr++;
  setsvalue(L, &sv, ts);
  luaH_setint(L, S->h, ((lua_Integer)(S->nstr)), &sv);
  assert((S->h->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaC_objbarrierback(L, const_cast<GCObject*>(static_cast<const GCObject*>(S->h)), ts);
}

static void loadCode(LoadState* S, Proto* f)
{
  int n = loadInt(S);
  loadAlign(S, sizeof(f->code[0]));
  if (S->fixed)
  {
    f->code = ((Instruction*)(getaddr_(S, ((size_t)(n)) * sizeof(Instruction))));
    f->sizecode = n;
  }
  else
  {
    f->code = luaM_malloc_Instruction_span(S->L, (size_t)n).data();
    f->sizecode = n;
    loadBlock(S, f->code, ((size_t)(n)) * sizeof((f->code)[0]));
  }
}

static void loadFunction(LoadState* S, Proto* f);

static void loadConstants(LoadState* S, Proto* f)
{
  int i;
  int n = loadInt(S);
  f->k = luaM_malloc_TValue_span(S->L, (size_t)n).data();
  f->sizek = n;
  for (i = 0; i < n; i++)
    ((&f->k[i])->tt_ = LUA_VNIL);
  for (i = 0; i < n; i++)
  {
    TValue* o = &f->k[i];
    int t = loadByte(S);
    switch (t)
    {
    case LUA_VNIL:
      ((o)->tt_ = LUA_VNIL);
      break;
    case LUA_VFALSE:
      ((o)->tt_ = LUA_VFALSE);
      break;
    case LUA_VTRUE:
      ((o)->tt_ = LUA_VTRUE);
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
      loadString(S, f, &f->source); /* use 'source' to anchor string */
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
  f->p = luaM_malloc_Proto_ptr_span(S->L, (size_t)n).data();
  f->sizep = n;
  for (i = 0; i < n; i++)
    f->p[i] = nullptr;
  for (i = 0; i < n; i++)
  {
    f->p[i] = luaF_newproto(S->L);
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
  f->upvalues = luaM_malloc_Upvaldesc_span(S->L, (size_t)n).data();
  f->sizeupvalues = n;
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
    f->lineinfo = ((ls_byte*)(getaddr_(S, ((size_t)(n)) * sizeof(ls_byte))));
    f->sizelineinfo = n;
  }
  else
  {
    f->lineinfo = luaM_malloc_ls_byte_span(S->L, (size_t)n).data();
    f->sizelineinfo = n;
    loadBlock(S, f->lineinfo, ((size_t)(n)) * sizeof((f->lineinfo)[0]));
  }
  n = loadInt(S);
  if (n > 0)
  {
    loadAlign(S, sizeof(int));
    if (S->fixed)
    {
      f->abslineinfo = ((AbsLineInfo*)(getaddr_(S, ((size_t)(n)) * sizeof(AbsLineInfo))));
      f->sizeabslineinfo = n;
    }
    else
    {
      f->abslineinfo = luaM_malloc_AbsLineInfo_span(S->L, (size_t)n).data();
      f->sizeabslineinfo = n;
      loadBlock(S, f->abslineinfo, ((size_t)(n)) * sizeof((f->abslineinfo)[0]));
    }
  }
  n = loadInt(S);
  f->locvars = luaM_malloc_LocVar_span(S->L, (size_t)n).data();
  f->sizelocvars = n;
  for (i = 0; i < n; i++)
    f->locvars[i].varname = nullptr;
  for (i = 0; i < n; i++)
  {
    loadString(S, f, &f->locvars[i].varname);
    f->locvars[i].startpc = loadInt(S);
    f->locvars[i].endpc = loadInt(S);
  }
  n = loadInt(S);
  if (n != 0)            /* does it have debug information? */
    n = f->sizeupvalues; /* must be this many */
  for (i = 0; i < n; i++)
    loadString(S, f, &f->upvalues[i].name);
}

static void loadFunction(LoadState* S, Proto* f)
{
  f->linedefined = loadInt(S);
  f->lastlinedefined = loadInt(S);
  f->numparams = loadByte(S);
  /* get only the meaningful flags */
  f->flag = ((lu_byte)(loadByte(S) & ~PF_FIXED));
  if (S->fixed)
    f->flag |= PF_FIXED; /* signal that code is fixed */
  f->maxstacksize = loadByte(S);
  loadCode(S, f);
  loadConstants(S, f);
  loadUpvalues(S, f);
  loadProtos(S, f);
  loadString(S, f, &f->source);
  loadDebug(S, f);
}

static void checkliteral(LoadState* S, byte_view s, byte_view msg)
{
  char buff[LUA_SIGNATURE.size() + lt::LUAC_DATA.size()]; /* larger than both */
  size_t len = s.size();
  loadBlock(S, buff, len * sizeof(buff[0]));
  if (memcmp(s.data(), buff, len) != 0)
    error(S, msg);
}

static l_noret numerror(LoadState* S, byte_view what, byte_view tname)
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
  { int i; checknumsize(S, sizeof(i), lt::int_kw); loadBlock(S, &(i), ((size_t)(1)) * sizeof((&(i))[0])); checknumformat(S, i == (LUAC_INT), lt::int_kw); };
  { Instruction i; checknumsize(S, sizeof(i), lt::instruction); loadBlock(S, &(i), ((size_t)(1)) * sizeof((&(i))[0])); checknumformat(S, i == (LUAC_INST), lt::instruction); };
  { lua_Integer i; checknumsize(S, sizeof(i), lt::Lua_integer); loadBlock(S, &(i), ((size_t)(1)) * sizeof((&(i))[0])); checknumformat(S, i == (LUAC_INT), lt::Lua_integer); };
  { lua_Number i; checknumsize(S, sizeof(i), lt::Lua_number); loadBlock(S, &(i), ((size_t)(1)) * sizeof((&(i))[0])); checknumformat(S, i == (LUAC_NUM), lt::Lua_number); };
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
  S.fixed = ((lu_byte)(fixed));
  S.offset = 1; /* fist byte was already read */
  checkHeader(&S);
  S.h = anchor;
  S.nstr = 0;
  cl = luaF_newLclosure(L, loadByte(&S));
  assert((cl->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaD_anchorobj(L, anchor, const_cast<GCObject*>(static_cast<const GCObject*>(cl)));
  cl->p = luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  loadFunction(&S, cl->p);
  if (cl->nupvalues != cl->p->sizeupvalues)
    error(&S, lt::corrupted_chunk);
  return cl;
}
