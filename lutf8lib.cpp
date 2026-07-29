/*
** $Id: lutf8lib.c $
** Standard library for UTF-8 manipulation
** See Copyright Notice in lua.h
*/


#include "lprefix.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "internal_string.h"
#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"

constexpr unsigned MAXUNICODE = 0x10FFFFu;

constexpr unsigned MAXUTF = 0x7FFFFFFFu;

constexpr char MSGInvalid[] = "invalid UTF-8 code";

constexpr int iscont(int c)
{
  return (c & 0xC0) == 0x80;
}

/*
** Byte at offset 'i' of an engine string, as unsigned 0..255; reads
** '\0' at/past the end (the bound check replaces upstream's reliance
** on the C terminator when a cursor peeks past the end).
*/
static inline int ubyte(const InternalString* s, size_t i)
{
  return (i < s->size()) ? s->byteAt(i) : 0;
}

static inline int iscontat(const InternalString* s, size_t i)
{
  return iscont(ubyte(s, i));
}

/* from strlib */
/* translate a relative string position: negative means back from end */
static lua_Integer u_posrelat(lua_Integer pos, size_t len)
{
  if (pos >= 0)
    return pos;
  else if (0u - (size_t) pos > len)
    return 0;
  else
    return (lua_Integer) len + pos + 1;
}

/*
** Decode one UTF-8 sequence starting at offset 'pos' of 's', returning
** the offset one past the sequence, or 0 if the byte sequence is
** invalid (0 can never be a successful result: success is always
** >= pos + 1).  The array 'limits' stores the minimum value for each
** sequence length, to check for overlong representations. Its first
** entry forces an error for non-ASCII bytes with no continuation
** bytes (count == 0).
*/
static size_t utf8_decode(const InternalString* s, size_t pos, l_uint32* val, int strict)
{
  static const l_uint32 limits[] =
      {~(l_uint32) 0, 0x80, 0x800, 0x10000u, 0x200000u, 0x4000000u};
  unsigned int c = (unsigned int) ubyte(s, pos);
  l_uint32 res = 0; /* final result */
  if (c < 0x80)     /* ASCII? */
    res = c;
  else if (c >= 0xfe) /* c >= 1111 1110b ? */
    return 0;         /* would need six or more continuation bytes */
  else
  {
    int count = 0; /* to count number of continuation bytes */
    for (; c & 0x40; c <<= 1)
    {                                                            /* while it needs continuation bytes... */
      unsigned int cc = (unsigned int) ubyte(s, pos + (size_t) ++count); /* read next byte */
      if (!iscont((int) cc))                                     /* not a continuation byte? */
        return 0;                                                /* invalid byte sequence */
      res = (res << 6) | (cc & 0x3F);                            /* add lower 6 bits from cont. byte */
    }
    assert(count <= 5);
    res |= ((l_uint32) (c & 0x7F) << (count * 5)); /* add first byte */
    if (res > MAXUTF || res < limits[count])
      return 0;             /* invalid byte sequence */
    pos += (size_t) count;  /* skip continuation bytes read */
  }
  if (strict)
  {
    /* check for invalid code points; too large or surrogates */
    if (res > MAXUNICODE || (0xD800u <= res && res <= 0xDFFFu))
      return 0;
  }
  if (val)
    *val = res;
  return pos + 1; /* +1 to include first byte */
}

/*
** utf8len(s [, i [, j [, lax]]]) --> number of characters that
** start in the range [i,j], or nil + current position if 's' is not
** well formed in that interval
*/
static int utflen(lua_State* L)
{
  lua_Integer n = 0; /* counter for the number of characters */
  const InternalString* s = luaL_checkistring(L, 1);
  size_t len = s->size(); /* string length in bytes */
  lua_Integer posi = u_posrelat(luaL_optinteger(L, 2, 1), len);
  lua_Integer posj = u_posrelat(luaL_optinteger(L, 3, -1), len);
  int lax = lua_toboolean(L, 4);
  luaL_argcheck(L, 1 <= posi && --posi <= (lua_Integer) len, 2,
                "initial position out of bounds");
  luaL_argcheck(L, --posj < (lua_Integer) len, 3,
                "final position out of bounds");
  while (posi <= posj)
  {
    size_t s1 = utf8_decode(s, (size_t) posi, nullptr, !lax);
    if (s1 == 0)
    {                               /* conversion error? */
      luaL_pushfail(L);             /* return fail ... */
      lua_pushinteger(L, posi + 1); /* ... and current position */
      return 2;
    }
    posi = (lua_Integer) s1;
    n++;
  }
  lua_pushinteger(L, n);
  return 1;
}

/*
** codepoint(s, [i, [j [, lax]]]) -> returns codepoints for all
** characters that start in the range [i,j]
*/
static int codepoint(lua_State* L)
{
  const InternalString* s = luaL_checkistring(L, 1);
  size_t len = s->size();
  lua_Integer posi = u_posrelat(luaL_optinteger(L, 2, 1), len);
  lua_Integer pose = u_posrelat(luaL_optinteger(L, 3, posi), len);
  int lax = lua_toboolean(L, 4);
  int n;
  size_t se; /* string end (offset one past the slice) */
  luaL_argcheck(L, posi >= 1, 2, "out of bounds");
  luaL_argcheck(L, pose <= (lua_Integer) len, 3, "out of bounds");
  if (posi > pose)
    return 0;                 /* empty interval; return no values */
  if (pose - posi >= INT_MAX) /* (lua_Integer -> int) overflow? */
    return luaL_error(L, "string slice too long");
  n = (int) (pose - posi) + 1; /* upper bound for number of returns */
  luaL_checkstack(L, n, "string slice too long");
  n = 0;             /* count the number of returns */
  se = (size_t) pose; /* string end */
  for (size_t pos = (size_t) posi - 1; pos < se;)
  {
    l_uint32 code;
    pos = utf8_decode(s, pos, &code, !lax);
    if (pos == 0)
      return luaL_error(L, MSGInvalid);
    lua_pushinteger(L, ((lua_Integer)(code)));
    n++;
  }
  return n;
}

static void pushutfchar(lua_State* L, int arg)
{
  lua_Unsigned code = (lua_Unsigned) luaL_checkinteger(L, arg);
  luaL_argcheck(L, code <= MAXUTF, arg, "value out of range");
  lua_pushinteger(L, ((lua_Integer)(code)));
  lua_pushfstring(L, "%U");
}

/*
** utfchar(n1, n2, ...)  -> char(n1)..char(n2)...
*/
static int utfchar(lua_State* L)
{
  int n = lua_gettop(L); /* number of arguments */
  if (n == 1)            /* optimize common case of single char */
    pushutfchar(L, 1);
  else
  {
    int i;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (i = 1; i <= n; i++)
    {
      pushutfchar(L, i);
      luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
  }
  return 1;
}

/*
** offset(s, n, [i])  -> indices where n-th character counting from
**   position 'i' starts and ends; 0 means character at 'i'.
*/
static int byteoffset(lua_State* L)
{
  const InternalString* s = luaL_checkistring(L, 1);
  size_t len = s->size();
  lua_Integer n = luaL_checkinteger(L, 2);
  lua_Integer posi = (n >= 0) ? 1 : ((lua_Integer)(len)) + 1;
  posi = u_posrelat(luaL_optinteger(L, 3, posi), len);
  luaL_argcheck(L, 1 <= posi && --posi <= (lua_Integer) len, 3,
                "position out of bounds");
  if (n == 0)
  {
    /* find beginning of current byte sequence */
    while (posi > 0 && iscontat(s, (size_t) posi))
      posi--;
  }
  else
  {
    if (iscontat(s, (size_t) posi))
      return luaL_error(L, "initial position is a continuation byte");
    if (n < 0)
    {
      while (n < 0 && posi > 0)
      { /* move back */
        do
        { /* find beginning of previous character */
          posi--;
        } while (posi > 0 && iscontat(s, (size_t) posi));
        n++;
      }
    }
    else
    {
      n--; /* do not move for 1st character */
      while (n > 0 && posi < (lua_Integer) len)
      {
        do
        { /* find beginning of next character */
          posi++;
        } while (iscontat(s, (size_t) posi)); /* (cannot pass the end: ubyte reads '\0' there) */
        n--;
      }
    }
  }
  if (n != 0)
  { /* did not find given character? */
    luaL_pushfail(L);
    return 1;
  }
  lua_pushinteger(L, posi + 1); /* initial position */
  if ((ubyte(s, (size_t) posi) & 0x80) != 0)
  { /* multi-byte character? */
    if (iscont(ubyte(s, (size_t) posi)))
      return luaL_error(L, "initial position is a continuation byte");
    while (iscontat(s, (size_t) posi + 1))
      posi++; /* skip to last continuation byte */
  }
  /* else one-byte character: final position is the initial one */
  lua_pushinteger(L, posi + 1); /* 'posi' now is the final position */
  return 2;
}

static int iter_aux(lua_State* L, int strict)
{
  const InternalString* s = luaL_checkistring(L, 1);
  size_t len = s->size();
  lua_Unsigned n = (lua_Unsigned) lua_tointeger(L, 2);
  if (n < len)
  {
    while (iscontat(s, n))
      n++; /* go to next character */
  }
  if (n >= len) /* (also handles original 'n' being negative) */
    return 0;   /* no more codepoints */
  else
  {
    l_uint32 code;
    size_t next = utf8_decode(s, n, &code, strict);
    if (next == 0 || iscontat(s, next))
      return luaL_error(L, MSGInvalid);
    lua_pushinteger(L, ((lua_Integer)(n + 1)));
    lua_pushinteger(L, ((lua_Integer)(code)));
    return 2;
  }
}

static int iter_auxstrict(lua_State* L)
{
  return iter_aux(L, 1);
}

static int iter_auxlax(lua_State* L)
{
  return iter_aux(L, 0);
}

static int iter_codes(lua_State* L)
{
  int lax = lua_toboolean(L, 2);
  const InternalString* s = luaL_checkistring(L, 1);
  luaL_argcheck(L, !iscontat(s, 0), 1, MSGInvalid);
  lua_pushcfunction(L, lax ? iter_auxlax : iter_auxstrict);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, 0);
  return 3;
}

/* pattern to match a single UTF-8 character */
#define UTF8PATT "[\0-\x7F\xC2-\xFD][\x80-\xBF]*"

static const luaL_Reg funcs[] = {
    {"offset", byteoffset},
    {"codepoint", codepoint},
    {"char", utfchar},
    {"len", utflen},
    {"codes", iter_codes},
    /* placeholders */
    {"charpattern", nullptr},
    {nullptr, nullptr}};

LUAMOD_API int luaopen_utf8(lua_State* L)
{
  (luaL_checkversion(L), lua_createtable(L, 0, sizeof(funcs) / sizeof((funcs)[0]) - 1), luaL_setfuncs(L, funcs, 0));
  lua_pushlstring(L, UTF8PATT, sizeof(UTF8PATT) / sizeof(char) - 1);
  lua_setfield(L, -2, "charpattern");
  return 1;
}
