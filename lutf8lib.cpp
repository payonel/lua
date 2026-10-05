/*
** $Id: lutf8lib.c $
** Standard library for UTF-8 manipulation
** See Copyright Notice in lua.h
*/



#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lua_internal.h"
#include "lobject.h"

#include "internal_string.h"
#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "literals.h"

namespace lua::lib
{

constexpr int MAXUNICODE = 0x10FFFF;

constexpr int MAXUTF = 0x7FFFFFFF;

constexpr int iscont(int c)
{
  return (c & 0xC0) == 0x80;
}

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
  else if (pos < -(lua_Integer) len)
    return 0;
  else
    return (lua_Integer) len + pos + 1;
}

struct Utf8Code
{
  size_t next;
  int code;
};

static Utf8Code utf8_decode(const InternalString* s, size_t pos, int strict)
{
  static const int limits[] =
      {INT_MAX, 0x80, 0x800, 0x10000, 0x200000, 0x4000000};
  int c = ubyte(s, pos);
  int res = 0; /* final result */
  if (c < 0x80)     /* ASCII? */
    res = c;
  else if (c >= 0xfe) /* c >= 1111 1110b ? */
    return {0, 0};         /* would need six or more continuation bytes */
  else
  {
    int count = 0; /* to count number of continuation bytes */
    for (; c & 0x40; c <<= 1)
    {                                                            /* while it needs continuation bytes... */
      int cc = ubyte(s, pos + (size_t) ++count); /* read next byte */
      if (!iscont(cc))                                     /* not a continuation byte? */
        return {0, 0};                                                /* invalid byte sequence */
      res = (res << 6) | (cc & 0x3F);                            /* add lower 6 bits from cont. byte */
    }
    assert(count <= 5);
    res |= (c & 0x7F) << (count * 5); /* add first byte */
    if (res > MAXUTF || res < limits[count])
      return {0, 0};             /* invalid byte sequence */
    pos += (size_t) count;  /* skip continuation bytes read */
  }
  if (strict)
  {
    /* check for invalid code points; too large or surrogates */
    if (res > MAXUNICODE || (0xD800 <= res && res <= 0xDFFF))
      return {0, 0};
  }
  return {pos + 1, res}; /* +1 to include first byte */
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
  luaL_argchecki(L, 1 <= posi && --posi <= (lua_Integer) len, 2, lt::initial_position_out_of_bounds);
  luaL_argchecki(L, --posj < (lua_Integer) len, 3, lt::final_position_out_of_bounds);
  while (posi <= posj)
  {
    size_t s1 = utf8_decode(s, posi, !lax).next;
    if (s1 == 0)
    {                               /* conversion error? */
      luaL_pushfail(L);             /* return fail ... */
      lua_pushinteger(L, posi + 1); /* ... and current position */
      return 2;
    }
    posi = s1;
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
  size_t se;
  luaL_argchecki(L, posi >= 1, 2, lt::out_of_bounds);
  luaL_argchecki(L, pose <= (lua_Integer) len, 3, lt::out_of_bounds);
  if (posi > pose)
    return 0;                 /* empty interval; return no values */
  if (pose - posi >= INT_MAX) /* (lua_Integer -> int) overflow? */
    return luaL_errori(L, lt::string_slice_too_long);
  n = (int) (pose - posi) + 1; /* upper bound for number of returns */
  luaL_checkstacki(L, n, lt::string_slice_too_long);
  n = 0;             /* count the number of returns */
  se = pose; /* string end */
  for (size_t pos = (size_t) posi - 1; pos < se;)
  {
    Utf8Code dec = utf8_decode(s, pos, !lax);
    pos = dec.next;
    if (pos == 0)
      return luaL_errori(L, lt::invalid_UTF_8_code);
    lua_pushinteger(L, dec.code);
    n++;
  }
  return n;
}

static void pushutfchar(lua_State* L, int arg)
{
  lua_Integer code = luaL_checkinteger(L, arg);
  luaL_argchecki(L, 0 <= code && code <= MAXUTF, arg, lt::value_out_of_range);
  lua_pushinteger(L, code);
  lua_pushfstringi(L, lt::fmt_U);
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
  lua_Integer posi = (n >= 0) ? 1 : (lua_Integer)len + 1;
  posi = u_posrelat(luaL_optinteger(L, 3, posi), len);
  luaL_argchecki(L, 1 <= posi && --posi <= (lua_Integer) len, 3, lt::position_out_of_bounds);
  if (n == 0)
  {
    /* find beginning of current byte sequence */
    while (posi > 0 && iscontat(s, posi))
      posi--;
  }
  else
  {
    if (iscontat(s, posi))
      return luaL_errori(L, lt::initial_position_is_a_continuation_byte);
    if (n < 0)
    {
      while (n < 0 && posi > 0)
      { /* move back */
        do
        { /* find beginning of previous character */
          posi--;
        } while (posi > 0 && iscontat(s, posi));
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
        } while (iscontat(s, posi));
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
  if ((ubyte(s, posi) & 0x80) != 0)
  { /* multi-byte character? */
    if (iscont(ubyte(s, posi)))
      return luaL_errori(L, lt::initial_position_is_a_continuation_byte);
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
  lua_Unsigned n = lua_tointeger(L, 2);
  if (n < len)
  {
    while (iscontat(s, n))
      n++; /* go to next character */
  }
  if (n >= len) /* (also handles original 'n' being negative) */
    return 0;   /* no more codepoints */
  else
  {
    Utf8Code dec = utf8_decode(s, n, strict);
    if (dec.next == 0 || iscontat(s, dec.next))
      return luaL_errori(L, lt::invalid_UTF_8_code);
    lua_pushinteger(L, n + 1);
    lua_pushinteger(L, dec.code);
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
  static CFuncInfo cfi_iter_auxlax("utf8.codes_lax"_hs, iter_auxlax);
  static CFuncInfo cfi_iter_auxstrict("utf8.codes_strict"_hs, iter_auxstrict);
  const InternalString* s = luaL_checkistring(L, 1);
  luaL_argchecki(L, !iscontat(s, 0), 1, lt::invalid_UTF_8_code);
  lua_pushcfuncinfo(L, lax ? &cfi_iter_auxlax : &cfi_iter_auxstrict);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, 0);
  return 3;
}

static CFuncInfo cfi_byteoffset("offset"_hs, byteoffset);
static CFuncInfo cfi_codepoint("codepoint"_hs, codepoint);
static CFuncInfo cfi_utfchar("char"_hs, utfchar);
static CFuncInfo cfi_utflen("len"_hs, utflen);
static CFuncInfo cfi_iter_codes("codes"_hs, iter_codes);

static const luaL_RegCFI funcs[] = {
  {lt::offset, &cfi_byteoffset},
  {lt::codepoint, &cfi_codepoint},
  {lt::char_kw, &cfi_utfchar},
  {lt::len, &cfi_utflen},
  {lt::codes, &cfi_iter_codes},
  {lt::charpattern, nullptr},
};

}

LUAMOD_API int luaopen_utf8(lua_State* L)
{
  luaL_newlibcfi(L, funcs);
  const byte_view patt = lt::utf8patt;
  lua_pushlstring(L, patt, 0, patt.size());
  lua_setfieldi(L, -2, lt::charpattern);
  return 1;
}

