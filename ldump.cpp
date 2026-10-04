/*
** $Id: ldump.c $
** save precompiled Lua chunks
** See Copyright Notice in lua.h
*/


#include <limits.h>
#include <stddef.h>

#include "lua.h"

#include "lapi.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
#include "lundump.h"

#include "literals.h"

struct DumpState
{
  lua_State* L;
  lua_Writer* writer;
  size_t offset; /* current position relative to beginning of dump */
  int strip;
  int status;
  Table* h;          /* table to track saved strings */
  lua_Unsigned nstr; /* counter for counting saved strings */
};

/*
** All high-level dumps go through dumpVector; you can change it to
** change the endianness of the result
*/

static void dumpBlock(DumpState* D, const void* b, size_t size)
{
  if (D->status == 0)
  { /* do not write anything after an error */
    std::optional<byte_view> block = (b != nullptr)
        ? std::optional<byte_view>(byte_view{(const char*)(b), size})
        : std::nullopt;
    lua_unlock(D->L);
    D->status = D->writer->write(D->L, block);
    lua_lock(D->L);
    D->offset += size;
  }
}

/*
** Dump enough zeros to ensure that current position is a multiple of
** 'align'.
*/
static void dumpAlign(DumpState* D, unsigned align)
{
  unsigned padding = align - ((unsigned int)(D->offset % align));
  if (padding < align)
  { /* padding == align means no padding */
    static lua_Integer paddingContent = 0;
    assert(align <= sizeof(lua_Integer));
    dumpBlock(D, &paddingContent, padding);
  }
  assert(D->offset % align == 0);
}

static void dumpByte(DumpState* D, int y)
{
  lu_byte x = (lu_byte) y;
  dumpBlock(D, &(x), (1) * sizeof((&(x))[0]));
}

/*
** size for 'dumpVarint' buffer: each byte can store up to 7 bits.
** (The "+6" rounds up the division.)
*/
constexpr int DIBS = ((((int)(sizeof(lua_Unsigned) * CHAR_BIT)) + 6) / 7);

/*
** Dumps an unsigned integer using the MSB Varint encoding
*/
static void dumpVarint(DumpState* D, lua_Unsigned x)
{
  lu_byte buff[DIBS];
  unsigned n = 1;
  buff[DIBS - 1] = x & 0x7f; /* fill least-significant byte */
  while ((x >>= 7) != 0)     /* fill other bytes in reverse order */
    buff[DIBS - (++n)] = ((lu_byte)((x & 0x7f) | 0x80));
  dumpBlock(D, buff + DIBS - n, (n) * sizeof((buff + DIBS - n)[0]));
}

static void dumpSize(DumpState* D, size_t sz)
{
  dumpVarint(D, ((lua_Unsigned)(sz)));
}

static void dumpInt(DumpState* D, int x)
{
  assert(x >= 0);
  dumpVarint(D, ((unsigned int)(x)));
}

static void dumpNumber(DumpState* D, lua_Number x)
{
  dumpBlock(D, &(x), (1) * sizeof((&(x))[0]));
}

/*
** Signed integers are coded to keep small values small. (Coding -1 as
** 0xfff...fff would use too many bytes to save a quite common value.)
** A non-negative x is coded as 2x; a negative x is coded as -2x - 1.
** (0 => 0; -1 => 1; 1 => 2; -2 => 3; 2 => 4; ...)
*/
static void dumpInteger(DumpState* D, lua_Integer x)
{
  lua_Unsigned cx = (x >= 0) ? 2u * ((lua_Unsigned)(x))
                             : (2u * ~((lua_Unsigned)(x))) + 1;
  dumpVarint(D, cx);
}

static void dumpString(DumpState* D, TString* ts)
{
  if (ts == nullptr)
  {
    dumpVarint(D, 0);
    dumpVarint(D, 0);
  }
  else
  {
    TValue idx;
    int tag = D->h->luaH_getstr(ts, &idx);
    if (!(((tag) & NOVARIANT_MASK) == LUA_TNIL))
    {                                         /* string already saved? */
      dumpVarint(D, 0);                       /* reuse a saved string */
      dumpVarint(D, ((lua_Unsigned)(ivalue(&idx)))); /* index of saved string */
    }
    else
    {                    /* must write and save the string */
      TValue key, value; /* to save the string in the hash */
      size_t size;
      byte_view sv = ts->data.view();
      size = sv.size();
      const char* s = sv.data();
      dumpSize(D, size + 1);
      dumpBlock(D, s, (size + 1) * sizeof((s)[0]));            /* include ending '\0' */
      D->nstr++;                             /* one more saved string */
      setsvalue(D->L, &key, ts);             /* the string is the key */
      setivalue(&value, ((lua_Integer)(D->nstr))); /* its index is the value */
      D->h->luaH_set(D->L, &key, &value);    /* h[ts] = nstr */
      /* integer value does not need barrier */
    }
  }
}

static void dumpCode(DumpState* D, const Proto* f)
{
  dumpInt(D, (int)f->code.size());
  dumpAlign(D, sizeof(f->code[0]));
  assert(f->code.data() != nullptr);
  dumpBlock(D, f->code.data(), (((unsigned int)(f->code.size()))) * sizeof((f->code)[0]));
}

static void dumpFunction(DumpState* D, const Proto* f);

static void dumpConstants(DumpState* D, const Proto* f)
{
  int i;
  int n = (int)f->k.size();
  dumpInt(D, n);
  for (i = 0; i < n; i++)
  {
    const TValue* o = &f->k[i];
    int tt = ((o)->tt_ & WITHVARIANT_MASK);
    dumpByte(D, tt);
    switch (tt)
    {
    case LUA_VNUMFLT:
      dumpNumber(D, fltvalue(o));
      break;
    case LUA_VNUMINT:
      dumpInteger(D, ivalue(o));
      break;
    case LUA_VSHRSTR:
      assert((o->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
      dumpString(D, tsvalue(o));
      break;
    default:
      assert(tt == LUA_VNIL || tt == LUA_VFALSE || tt == LUA_VTRUE);
    }
  }
}

static void dumpProtos(DumpState* D, const Proto* f)
{
  int i;
  int n = (int)f->p.size();
  dumpInt(D, n);
  for (i = 0; i < n; i++)
    dumpFunction(D, f->p[i]);
}

static void dumpUpvalues(DumpState* D, const Proto* f)
{
  int i, n = (int)f->upvalues.size();
  dumpInt(D, n);
  for (i = 0; i < n; i++)
  {
    dumpByte(D, f->upvalues[i].instack);
    dumpByte(D, f->upvalues[i].idx);
    dumpByte(D, f->upvalues[i].kind);
  }
}

static void dumpDebug(DumpState* D, const Proto* f)
{
  int i, n;
  n = (D->strip) ? 0 : (int)f->lineinfo.size();
  dumpInt(D, n);
  if (f->lineinfo.data() != nullptr)
    dumpBlock(D, f->lineinfo.data(), (((unsigned int)(n))) * sizeof((f->lineinfo)[0]));
  n = (D->strip) ? 0 : (int)f->abslineinfo.size();
  dumpInt(D, n);
  if (n > 0)
  {
    /* 'abslineinfo' is an array of structures of int's */
    dumpAlign(D, sizeof(int));
    dumpBlock(D, f->abslineinfo.data(), (((unsigned int)(n))) * sizeof((f->abslineinfo)[0]));
  }
  n = (D->strip) ? 0 : (int)f->locvars.size();
  dumpInt(D, n);
  for (i = 0; i < n; i++)
  {
    dumpString(D, f->locvars[i].varname);
    dumpInt(D, f->locvars[i].startpc);
    dumpInt(D, f->locvars[i].endpc);
  }
  n = (D->strip) ? 0 : (int)f->upvalues.size();
  dumpInt(D, n);
  for (i = 0; i < n; i++)
    dumpString(D, f->upvalues[i].name);
}

static void dumpFunction(DumpState* D, const Proto* f)
{
  dumpInt(D, f->linedefined);
  dumpInt(D, f->lastlinedefined);
  dumpByte(D, f->numparams);
  dumpByte(D, f->flag);
  dumpByte(D, f->maxstacksize);
  dumpCode(D, f);
  dumpConstants(D, f);
  dumpUpvalues(D, f);
  dumpProtos(D, f);
  dumpString(D, D->strip ? nullptr : f->source);
  dumpDebug(D, f);
}

static void dumpHeader(DumpState* D)
{
  dumpBlock(D, LUA_SIGNATURE.data(), LUA_SIGNATURE.size());
  dumpByte(D, LUAC_VERSION);
  dumpByte(D, LUAC_FORMAT);
  dumpBlock(D, lt::LUAC_DATA.data(), lt::LUAC_DATA.size());
  { int i = LUAC_INT; dumpByte(D, sizeof(int)); dumpBlock(D, &(i), (1) * sizeof((&(i))[0])); };
  { Instruction i = LUAC_INST; dumpByte(D, sizeof(Instruction)); dumpBlock(D, &(i), (1) * sizeof((&(i))[0])); };
  { lua_Integer i = LUAC_INT; dumpByte(D, sizeof(lua_Integer)); dumpBlock(D, &(i), (1) * sizeof((&(i))[0])); };
  { lua_Number i = LUAC_NUM; dumpByte(D, sizeof(lua_Number)); dumpBlock(D, &(i), (1) * sizeof((&(i))[0])); };
}

/*
** dump Lua function as precompiled chunk
*/
int luaU_dump(lua_State* L, const Proto* f, lua_Writer* w,
              int strip)
{
  DumpState D;
  D.h = Table::luaH_new(L);             /* aux. table to keep strings already dumped */
  setsloth(L, L->top, D.h); /* anchor it */
  L->top++;
  D.L = L;
  D.writer = w;
  D.offset = 0;
  D.strip = strip;
  D.status = 0;
  D.nstr = 0;
  dumpHeader(&D);
  dumpByte(&D, (int)f->upvalues.size());
  dumpFunction(&D, f);
  dumpBlock(&D, nullptr, 0); /* signal end of dump */
  return D.status;
}
