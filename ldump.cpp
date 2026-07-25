/*
** $Id: ldump.c $
** save precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

#include <bit>
#include <limits.h>
#include <stddef.h>

#include "lua.h"

#include "lapi.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
#include "lundump.h"

typedef struct
{
  lua_State* L;
  lua_Writer writer;
  void* data;
  size_t offset; /* current position relative to beginning of dump */
  int strip;
  int status;
  Table* h;          /* table to track saved strings */
  lua_Unsigned nstr; /* counter for counting saved strings */
} DumpState;

/*
** Every multi-byte field in a chunk has a width and byte order defined
** by LUAC_FORMAT (little-endian, assembled byte-by-byte): chunks are
** portable across hosts, and a managed reader/writer produces
** identical bytes. Raw block I/O is used only for byte arrays
** (signature/LUAC_DATA literals, string bodies, lineinfo).
*/

/*
** Dump the block of memory pointed by 'b' with given 'size'.
** 'b' should not be nullptr, except for the last call signaling the end
** of the dump.
*/
static void dumpBlock(DumpState* D, const void* b, size_t size)
{
  if (D->status == 0)
  { /* do not write anything after an error */
    lua_unlock(D->L);
    D->status = (*D->writer)(D->L, b, size, D->data);
    lua_lock(D->L);
    D->offset += size;
  }
}

static void dumpByte(DumpState* D, int y)
{
  lu_byte x = (lu_byte) y;
  dumpBlock(D, &(x), (1) * sizeof((&(x))[0]));
}

/*
** Dump an exact 32-bit unsigned value, little-endian by definition of
** the format (LUAC_FORMAT 1) — independent of host memory layout.
** (Java: the four DataOutput-style byte writes of an int.)
*/
static void dumpUint32(DumpState* D, l_uint32 x)
{
  lu_byte b[4];
  b[0] = (lu_byte) (x & 0xFF);
  b[1] = (lu_byte) ((x >> 8) & 0xFF);
  b[2] = (lu_byte) ((x >> 16) & 0xFF);
  b[3] = (lu_byte) ((x >> 24) & 0xFF);
  dumpBlock(D, b, 4);
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

/*
** A Lua number is dumped as its IEEE-754 binary64 bit pattern, as two
** little-endian 32-bit halves (low half first) — a format-defined
** encoding, not a memory copy. The bit_cast statically checks that
** lua_Number has exactly 64 bits. (Java: Double.doubleToLongBits.)
*/
static void dumpNumber(DumpState* D, lua_Number x)
{
  lua_Unsigned bits = std::bit_cast<lua_Unsigned>(x);
  dumpUint32(D, (l_uint32) (bits & 0xFFFFFFFFu));
  dumpUint32(D, (l_uint32) (bits >> 32));
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
      assert((&idx)->tt_ == LUA_VNUMINT);
      dumpVarint(D, ((lua_Unsigned)(((&idx)->value_).i))); /* index of saved string */
    }
    else
    {                    /* must write and save the string */
      TValue key, value; /* to save the string in the hash */
      size_t size;
      const char* s = ((void)((size) = (ts)->content.size()), (ts)->content.c_str());
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
  int i;
  dumpInt(D, f->sizecode);
  assert(f->code != nullptr);
  for (i = 0; i < f->sizecode; i++) /* each instruction is a u32 field */
    dumpUint32(D, f->code[i]);
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
      dumpNumber(D, o->value_.n);
      break;
    case LUA_VNUMINT:
      dumpInteger(D, o->value_.i);
      break;
    case LUA_VSHRSTR:
      assert((o->tt_ & NOVARIANT_MASK) == LUA_TSTRING);
      assert((o->value_.gc->tt & NOVARIANT_MASK) == LUA_TSTRING);
      dumpString(D, static_cast<TString*>(o->value_.gc));
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
  for (i = 0; i < n; i++)
  { /* each entry is two varint fields, not a struct image */
    dumpInt(D, f->abslineinfo[i].pc);
    dumpInt(D, f->abslineinfo[i].line);
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

/*
** Header: signature, version, format, LUAC_DATA, then three check
** values written with the format's own field encodings (varint-signed
** integer, u32 instruction, binary64 number). There are no native
** size/layout probes: LUAC_FORMAT itself defines every field width.
*/
static void dumpHeader(DumpState* D)
{
  dumpBlock(D, LUA_SIGNATURE.data(), LUA_SIGNATURE.size());
  dumpByte(D, LUAC_VERSION);
  dumpByte(D, LUAC_FORMAT);
  dumpBlock(D, LUAC_DATA.data(), LUAC_DATA.size());
  dumpInteger(D, LUAC_INT);
  dumpUint32(D, LUAC_INST);
  dumpNumber(D, LUAC_NUM);
}

/*
** dump Lua function as precompiled chunk
*/
int luaU_dump(lua_State* L, const Proto* f, lua_Writer w, void* data,
              int strip)
{
  DumpState D;
  D.h = luaH_new(L);             /* aux. table to keep strings already dumped */
  sethvalue(L, (&(L)->stack.p[L->top]), D.h); /* anchor it */
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
