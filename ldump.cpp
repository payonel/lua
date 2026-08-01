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

struct DumpState
{
  lua_State* L;
  lua_Writer writer;
  void* data;
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

/*
** Dump the block of memory pointed by 'b' with given 'size'.
** 'b' should not be nullptr, except for the last call signaling the end
** of the dump.
*/
/*
** Multi-byte values are dumped in host byte order — guaranteed
** little-endian (static_assert in llimits.h). A big-endian build would
** have produced incompatible chunks; it fails the assert instead.
*/
static void dumpBlock(DumpState* D, const void* b, size_t size)
{
  if (D->status == 0)
  { /* do not write anything after an error */
    /* writer boundary: engine bytes as a read-only view, valid only
       during the call; default view (null data) signals end of dump
       (Java: byte[] argument, null = end) */
    std::string_view block = (b != nullptr)
        ? std::string_view{(const char*)(b), size}
        : std::string_view{};
    lua_unlock(D->L);
    D->status = (*D->writer)(D->L, block, D->data);
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

/*
** Dump a String. First dump its "size":
** size==0 is followed by an index and means "reuse saved string with
** that index"; index==0 means nullptr.
** size>=1 is followed by the string contents with real size==size-1 and
** means that string, which will be saved with the next available index.
** The real size does not include the ending '\0' (which is not dumped),
** so adding 1 to it cannot overflow a size_t.
*/
static void dumpString(DumpState* D, TString* ts)
{
  if (ts == nullptr)
  {
    dumpVarint(D, 0); /* will "reuse" nullptr */
    dumpVarint(D, 0); /* special index for nullptr */
  }
  else
  {
    TValue idx;
    int tag = luaH_getstr(D->h, ts, &idx);
    if (!(((tag) & NOVARIANT_MASK) == LUA_TNIL))
    {                                         /* string already saved? */
      dumpVarint(D, 0);                       /* reuse a saved string */
      dumpVarint(D, ((lua_Unsigned)(ivalue(&idx)))); /* index of saved string */
    }
    else
    {                    /* must write and save the string */
      TValue key, value; /* to save the string in the hash */
      size_t size;
      std::string_view sv = ts->data.view();
      size = sv.size();
      const char* s = sv.data(); /* stream boundary: chunk writer (Java: write(byte[], off, len)) */
      dumpSize(D, size + 1);
      dumpBlock(D, s, (size + 1) * sizeof((s)[0]));            /* include ending '\0' */
      D->nstr++;                             /* one more saved string */
      setsvalue(D->L, &key, ts);             /* the string is the key */
      setivalue(&value, ((lua_Integer)(D->nstr))); /* its index is the value */
      luaH_set(D->L, D->h, &key, &value);    /* h[ts] = nstr */
      /* integer value does not need barrier */
    }
  }
}

static void dumpCode(DumpState* D, const Proto* f)
{
  dumpInt(D, f->sizecode);
  dumpAlign(D, sizeof(f->code[0]));
  assert(f->code != nullptr);
  dumpBlock(D, f->code, (((unsigned int)(f->sizecode))) * sizeof((f->code)[0]));
}

static void dumpFunction(DumpState* D, const Proto* f);

static void dumpConstants(DumpState* D, const Proto* f)
{
  int i;
  int n = f->sizek;
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
  int n = f->sizep;
  dumpInt(D, n);
  for (i = 0; i < n; i++)
    dumpFunction(D, f->p[i]);
}

static void dumpUpvalues(DumpState* D, const Proto* f)
{
  int i, n = f->sizeupvalues;
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
  n = (D->strip) ? 0 : f->sizelineinfo;
  dumpInt(D, n);
  if (f->lineinfo != nullptr)
    dumpBlock(D, f->lineinfo, (((unsigned int)(n))) * sizeof((f->lineinfo)[0]));
  n = (D->strip) ? 0 : f->sizeabslineinfo;
  dumpInt(D, n);
  if (n > 0)
  {
    /* 'abslineinfo' is an array of structures of int's */
    dumpAlign(D, sizeof(int));
    dumpBlock(D, f->abslineinfo, (((unsigned int)(n))) * sizeof((f->abslineinfo)[0]));
  }
  n = (D->strip) ? 0 : f->sizelocvars;
  dumpInt(D, n);
  for (i = 0; i < n; i++)
  {
    dumpString(D, f->locvars[i].varname);
    dumpInt(D, f->locvars[i].startpc);
    dumpInt(D, f->locvars[i].endpc);
  }
  n = (D->strip) ? 0 : f->sizeupvalues;
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
  dumpBlock(D, LUAC_DATA.data(), LUAC_DATA.size());
  { int i = LUAC_INT; dumpByte(D, sizeof(int)); dumpBlock(D, &(i), (1) * sizeof((&(i))[0])); };
  { Instruction i = LUAC_INST; dumpByte(D, sizeof(Instruction)); dumpBlock(D, &(i), (1) * sizeof((&(i))[0])); };
  { lua_Integer i = LUAC_INT; dumpByte(D, sizeof(lua_Integer)); dumpBlock(D, &(i), (1) * sizeof((&(i))[0])); };
  { lua_Number i = LUAC_NUM; dumpByte(D, sizeof(lua_Number)); dumpBlock(D, &(i), (1) * sizeof((&(i))[0])); };
}

/*
** dump Lua function as precompiled chunk
*/
int luaU_dump(lua_State* L, const Proto* f, lua_Writer w, void* data,
              int strip)
{
  DumpState D;
  D.h = luaH_new(L);             /* aux. table to keep strings already dumped */
  setsloth(L, L->top, D.h); /* anchor it */
  L->top++;
  D.L = L;
  D.writer = w;
  D.offset = 0;
  D.data = data;
  D.strip = strip;
  D.status = 0;
  D.nstr = 0;
  dumpHeader(&D);
  dumpByte(&D, f->sizeupvalues);
  dumpFunction(&D, f);
  dumpBlock(&D, nullptr, 0); /* signal end of dump */
  return D.status;
}
