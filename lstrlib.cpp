/*
** $Id: lstrlib.c $
** Standard library for string operations and pattern-matching
** See Copyright Notice in lua.h
*/


#include "lprefix.h"

#include <bit>

#include <charconv>
#include <cmath>
#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lua_internal.h"

#include "internal_string.h"
#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "util_cast.h"

/*
** maximum number of captures that a pattern can do during
** pattern-matching. This limit is arbitrary, but must fit in
** an unsigned char.
*/
#if !defined(LUA_MAXCAPTURES)
#define LUA_MAXCAPTURES 32
#endif

/*
** Byte at offset 'i' of an engine string, as unsigned 0..255; reads
** '\0' at/past the end (the bound check replaces upstream's reliance
** on the C terminator when a cursor peeks one past the end).
*/
static inline int istrbyte(const InternalString* s, size_t i)
{
  return (i < s->size()) ? s->byteAt(i) : 0;
}

static int str_len(lua_State* L)
{
  const InternalString* s = luaL_checkistring(L, 1);
  lua_pushinteger(L, (lua_Integer) s->size());
  return 1;
}

/*
** translate a relative initial string position
** (negative means back from end): clip result to [1, inf).
** The length of any string in Lua must fit in a lua_Integer,
** so there are no overflows in the casts.
** The inverted comparison avoids a possible overflow
** computing '-pos'.
*/
static size_t posrelatI(lua_Integer pos, size_t len)
{
  if (pos > 0)
    return (size_t) pos;
  else if (pos == 0)
    return 1;
  else if (pos < -(lua_Integer) len) /* inverted comparison */
    return 1;                        /* clip to 1 */
  else
    return len + (size_t) pos + 1;
}

/*
** Gets an optional ending string position from argument 'arg',
** with default value 'def'.
** Negative means back from end: clip result to [0, len]
*/
static size_t getendpos(lua_State* L, int arg, lua_Integer def,
                        size_t len)
{
  lua_Integer pos = luaL_optinteger(L, arg, def);
  if (pos > (lua_Integer) len)
    return len;
  else if (pos >= 0)
    return (size_t) pos;
  else if (pos < -(lua_Integer) len)
    return 0;
  else
    return len + (size_t) pos + 1;
}

static int str_sub(lua_State* L)
{
  const InternalString* s = luaL_checkistring(L, 1);
  size_t l = s->size();
  size_t start = posrelatI(luaL_checkinteger(L, 2), l);
  size_t end = getendpos(L, 3, -1, l);
  if (start <= end)
    lua_pushlstring(L, s->view(), start - 1, (end - start) + 1);
  else
    lua_pushliteral(L, HostString(lit<"">()->view()));
  return 1;
}

static int str_reverse(lua_State* L)
{
  size_t i;
  luaL_Buffer b;
  const InternalString* s = luaL_checkistring(L, 1);
  size_t l = s->size();
  size_t off = luaL_buffinitsize(L, &b, l);
  std::span<char> p = luaL_buffarea(&b);
  for (i = 0; i < l; i++)
    p[off + i] = ((char)(s->byteAt(l - i - 1)));
  luaL_pushresultsize(&b, l);
  return 1;
}

static int str_lower(lua_State* L)
{
  size_t i;
  luaL_Buffer b;
  const InternalString* s = luaL_checkistring(L, 1);
  size_t l = s->size();
  size_t off = luaL_buffinitsize(L, &b, l);
  std::span<char> p = luaL_buffarea(&b);
  for (i = 0; i < l; i++)
    p[off + i] = ((char)(tolower(s->byteAt(i))));
  luaL_pushresultsize(&b, l);
  return 1;
}

static int str_upper(lua_State* L)
{
  size_t i;
  luaL_Buffer b;
  const InternalString* s = luaL_checkistring(L, 1);
  size_t l = s->size();
  size_t off = luaL_buffinitsize(L, &b, l);
  std::span<char> p = luaL_buffarea(&b);
  for (i = 0; i < l; i++)
    p[off + i] = ((char)(toupper(s->byteAt(i))));
  luaL_pushresultsize(&b, l);
  return 1;
}

/*
** MAX_SIZE is limited both by size_t and lua_Integer.
** When x <= MAX_SIZE, x can be safely cast to size_t or lua_Integer.
*/
static int str_rep(lua_State* L)
{
  const InternalString* s = luaL_checkistring(L, 1);
  lua_Integer n = luaL_checkinteger(L, 2);
  const InternalString* sep = luaL_optistring(L, 3, lit<"">());
  size_t len = s->size();
  size_t lsep = sep->size();
  if (n <= 0 || (len | lsep) == 0)
    lua_pushliteral(L, HostString(lit<"">()->view())); /* no repetitions or both strings empty */
  else if (l_unlikely(len > MAX_SIZE - lsep ||
                      ((lua_Integer)(len + lsep)) > ((lua_Integer)(MAX_SIZE)) / n))
    return luaL_error(L, HostString(lit<"resulting string too large">()->view()));
  else
  {
    size_t totallen = (((size_t)(n)) * (len + lsep)) - lsep;
    luaL_Buffer b;
    size_t off = luaL_buffinitsize(L, &b, totallen);
    std::span<char> p = luaL_buffarea(&b);
    size_t pos = off;
    while (n-- > 1)
    { /* first n-1 copies (followed by separator) */
      memcpy(p.data() + pos, s->bytes(), len * sizeof(char));
      pos += len;
      if (lsep > 0)
      { /* empty 'memcpy' is not that cheap */
        memcpy(p.data() + pos, sep->bytes(), lsep * sizeof(char));
        pos += lsep;
      }
    }
    memcpy(p.data() + pos, s->bytes(), len * sizeof(char)); /* last copy without separator */
    luaL_pushresultsize(&b, totallen);
  }
  return 1;
}

static int str_byte(lua_State* L)
{
  const InternalString* s = luaL_checkistring(L, 1);
  size_t l = s->size();
  lua_Integer pi = luaL_optinteger(L, 2, 1);
  size_t posi = posrelatI(pi, l);
  size_t pose = getendpos(L, 3, pi, l);
  int n, i;
  if (posi > pose)
    return 0;                                      /* empty interval; return no values */
  if (l_unlikely(pose - posi >= (size_t) INT_MAX)) /* arithmetic overflow? */
    return luaL_error(L, HostString(lit<"string slice too long">()->view()));
  n = (int) (pose - posi) + 1;
  luaL_checkstack(L, n, HostString(lit<"string slice too long">()->view()));
  for (i = 0; i < n; i++)
    lua_pushinteger(L, s->byteAt(posi + ((unsigned int)(i)) - 1));
  return n;
}

static int str_char(lua_State* L)
{
  int n = lua_gettop(L); /* number of arguments */
  int i;
  luaL_Buffer b;
  size_t off = luaL_buffinitsize(L, &b, ((unsigned int)(n)));
  std::span<char> p = luaL_buffarea(&b);
  for (i = 1; i <= n; i++)
  {
    lua_Unsigned c = (lua_Unsigned) luaL_checkinteger(L, i);
    luaL_argcheck(L, c <= (lua_Unsigned) UCHAR_MAX, i, HostString(lit<"value out of range">()->view()));
    p[off + i - 1] = ((char)(((unsigned char)(c))));
  }
  luaL_pushresultsize(&b, ((unsigned int)(n)));
  return 1;
}

/*
** Buffer to store the result of 'string.dump'. It must be initialized
** after the call to 'lua_dump', to ensure that the function is on the
** top of the stack when 'lua_dump' is called. ('luaL_buffinit' might
** push stuff.)
*/
struct str_Writer
{
  int init; /* true iff buffer has been initialized */
  luaL_Buffer B;
};

static int writer(lua_State* L, std::string_view b, void* ud)
{
  struct str_Writer* state = (struct str_Writer*) ud;
  if (!state->init)
  {
    state->init = 1;
    luaL_buffinit(L, &state->B);
  }
  if (b.data() == nullptr)
  {                             /* finishing dump? (Java: null byte[]) */
    luaL_pushresult(&state->B); /* push result */
    lua_replace(L, 1);          /* move it to reserved slot */
  }
  else
    luaL_addlstring(&state->B, b, 0, b.size());
  return 0;
}

static int str_dump(lua_State* L)
{
  struct str_Writer state;
  int strip = lua_toboolean(L, 2);
  luaL_argcheck(L, lua_type(L, 1) == LUA_TFUNCTION && !lua_iscfunction(L, 1),
                1, HostString(lit<"Lua function expected">()->view()));
  /* ensure function is on the top of the stack and vacate slot 1 */
  lua_pushvalue(L, 1);
  state.init = 0;
  lua_dump(L, writer, &state, strip);
  lua_settop(L, 1); /* leave final result on top */
  return 1;
}

/*
** {======================================================
** METAMETHODS
** =======================================================
*/

static int tonum(lua_State* L, int arg)
{
  if (lua_type(L, arg) == LUA_TNUMBER)
  { /* already a number? */
    lua_pushvalue(L, arg);
    return 1;
  }
  else
  { /* check whether it is a numerical string */
    const InternalString* s = lua_toistring(L, arg);
    return (s != nullptr && lua_stringtonumber(L, s->view()) == s->size() + 1);
  }
}

/*
** To be here, either the first operand was a string or the first
** operand didn't have a corresponding metamethod. (Otherwise, that
** other metamethod would have been called.) So, if this metamethod
** doesn't work, the only other option would be for the second
** operand to have a different metamethod.
*/
static void trymt(lua_State* L, std::string_view mtkey, std::string_view opname)
{
  lua_settop(L, 2); /* back to the original arguments */
  if (l_unlikely(lua_type(L, 2) == LUA_TSTRING ||
                 !luaL_getmetafield(L, 2, HostString{mtkey})))
  {
    /* borrowed views of the static typenames (engine strings) */
    std::string_view t1 = lua_typenamei(L, lua_type(L, -2))->view();
    std::string_view t2 = lua_typenamei(L, lua_type(L, -1))->view();
    lua_pushlstring(L, opname, 0, opname.size());
    lua_pushlstring(L, t1, 0, t1.size());
    lua_pushlstring(L, t2, 0, t2.size());
    luaL_error(L, HostString(lit<"attempt to %s a '%s' with a '%s'">()->view()));
  }
  lua_insert(L, -3); /* put metamethod before arguments */
  lua_call(L, 2, 1); /* call metamethod */
}

static int arith(lua_State* L, int op, std::string_view mtname)
{
  if (tonum(L, 1) && tonum(L, 2))
    lua_arith(L, op); /* result will be on the top */
  else
    /* constant slice of a literal ("__add" -> "add"; Java: its own
       String constant) */
    trymt(L, mtname, mtname.substr(2));
  return 1;
}

static int arith_add(lua_State* L)
{
  return arith(L, LUA_OPADD, lit<"__add">()->view());
}

static int arith_sub(lua_State* L)
{
  return arith(L, LUA_OPSUB, lit<"__sub">()->view());
}

static int arith_mul(lua_State* L)
{
  return arith(L, LUA_OPMUL, lit<"__mul">()->view());
}

static int arith_mod(lua_State* L)
{
  return arith(L, LUA_OPMOD, lit<"__mod">()->view());
}

static int arith_pow(lua_State* L)
{
  return arith(L, LUA_OPPOW, lit<"__pow">()->view());
}

static int arith_div(lua_State* L)
{
  return arith(L, LUA_OPDIV, lit<"__div">()->view());
}

static int arith_idiv(lua_State* L)
{
  return arith(L, LUA_OPIDIV, lit<"__idiv">()->view());
}

static int arith_unm(lua_State* L)
{
  return arith(L, LUA_OPUNM, lit<"__unm">()->view());
}

static const luaL_Reg stringmetamethods[] = {
    {lit<"__add">()->view(), arith_add},
    {lit<"__sub">()->view(), arith_sub},
    {lit<"__mul">()->view(), arith_mul},
    {lit<"__mod">()->view(), arith_mod},
    {lit<"__pow">()->view(), arith_pow},
    {lit<"__div">()->view(), arith_div},
    {lit<"__idiv">()->view(), arith_idiv},
    {lit<"__unm">()->view(), arith_unm},
    {lit<"__index">()->view(), nullptr}, /* placeholder */
    {{}, nullptr}};

/* }====================================================== */

/*
** {======================================================
** PATTERN MATCHING
** =======================================================
*/

constexpr int CAP_UNFINISHED = (-1);
constexpr int CAP_POSITION = (-2);

/*
** Pattern-matching state. Cursor discipline (RULED 2026-07-29): the
** two strings are hoisted here as InternalString* once; every
** interior position travels as a plain integer offset — (string,
** offset) is the only interior-pointer shape that transcribes to
** Java. Offsets are ptrdiff_t (Java: int) and -1 is the "no match"
** sentinel (was nullptr in the pointer version).
*/
constexpr ptrdiff_t NOMATCH = -1;

struct MatchState
{
  const InternalString* src; /* source string (offsets 0..src_end) */
  const InternalString* p;   /* pattern string (offsets 0..p_end) */
  ptrdiff_t src_end;         /* end offset of source ('\0' position) */
  ptrdiff_t p_end;           /* end offset of pattern */
  lua_State* L;
  int matchdepth; /* control for recursive depth (to avoid C stack overflow) */
  int level;      /* total number of captures (finished or unfinished) */
  struct
  {
    ptrdiff_t init;
    ptrdiff_t len; /* length or special value (CAP_*) */
  } capture[LUA_MAXCAPTURES];
};

/* recursive function */
static ptrdiff_t match(MatchState* ms, ptrdiff_t s, ptrdiff_t p);

/*
** Byte at offset 'i' of the source/pattern, as unsigned 0..255. The
** end offset reads as '\0': upstream's cursors relied on the C
** terminator when peeking one past a validated position (suffix
** checks, '%f' at end); the bound check reproduces that exactly
** (InternalString has no terminator in the Java port).
*/
static inline int srcbyte(const MatchState* ms, ptrdiff_t i)
{
  return (i < ms->src_end) ? ms->src->byteAt(((size_t)(i))) : 0;
}

static inline int patbyte(const MatchState* ms, ptrdiff_t i)
{
  return (i < ms->p_end) ? ms->p->byteAt(((size_t)(i))) : 0;
}

/* maximum recursion depth for 'match' */
#if !defined(MAXCCALLS)
#define MAXCCALLS 200
#endif

constexpr char L_ESC = '%';
static const InternalString* const SPECIALS = lit<"^$*+?.([%-">();

static int check_capture(MatchState* ms, int l)
{
  l -= '1';
  if (l_unlikely(l < 0 || l >= ms->level ||
                 ms->capture[l].len == CAP_UNFINISHED))
  {
    lua_pushinteger(ms->L, l + 1);
    return luaL_error(ms->L, HostString(lit<"invalid capture index %%%d">()->view()));
  }
  return l;
}

static int capture_to_close(MatchState* ms)
{
  int level = ms->level;
  for (level--; level >= 0; level--)
    if (ms->capture[level].len == CAP_UNFINISHED)
      return level;
  return luaL_error(ms->L, HostString(lit<"invalid pattern capture">()->view()));
}

static ptrdiff_t classend(MatchState* ms, ptrdiff_t p)
{
  switch (patbyte(ms, p++))
  {
  case L_ESC:
  {
    if (l_unlikely(p == ms->p_end))
      luaL_error(ms->L, HostString(lit<"malformed pattern (ends with '%%')">()->view()));
    return p + 1;
  }
  case '[':
  {
    if (patbyte(ms, p) == '^')
      p++;
    do
    { /* look for a ']' */
      if (l_unlikely(p == ms->p_end))
        luaL_error(ms->L, HostString(lit<"malformed pattern (missing ']')">()->view()));
      if (patbyte(ms, p++) == L_ESC && p < ms->p_end)
        p++; /* skip escapes (e.g. '%]') */
    } while (patbyte(ms, p) != ']');
    return p + 1;
  }
  default:
  {
    return p;
  }
  }
}

static int match_class(int c, int cl)
{
  int res;
  switch (tolower(cl))
  {
  case 'a':
    res = isalpha(c);
    break;
  case 'c':
    res = iscntrl(c);
    break;
  case 'd':
    res = isdigit(c);
    break;
  case 'g':
    res = isgraph(c);
    break;
  case 'l':
    res = islower(c);
    break;
  case 'p':
    res = ispunct(c);
    break;
  case 's':
    res = isspace(c);
    break;
  case 'u':
    res = isupper(c);
    break;
  case 'w':
    res = isalnum(c);
    break;
  case 'x':
    res = isxdigit(c);
    break;
  case 'z':
    res = (c == 0);
    break; /* deprecated option */
  default:
    return (cl == c);
  }
  return (islower(cl) ? res : !res);
}

static int matchbracketclass(MatchState* ms, int c, ptrdiff_t p,
                             ptrdiff_t ec)
{
  int sig = 1;
  if (patbyte(ms, p + 1) == '^')
  {
    sig = 0;
    p++; /* skip the '^' */
  }
  while (++p < ec)
  {
    if (patbyte(ms, p) == L_ESC)
    {
      p++;
      if (match_class(c, patbyte(ms, p)))
        return sig;
    }
    else if ((patbyte(ms, p + 1) == '-') && (p + 2 < ec))
    {
      p += 2;
      if (patbyte(ms, p - 2) <= c && c <= patbyte(ms, p))
        return sig;
    }
    else if (patbyte(ms, p) == c)
      return sig;
  }
  return !sig;
}

static int singlematch(MatchState* ms, ptrdiff_t s, ptrdiff_t p,
                       ptrdiff_t ep)
{
  if (s >= ms->src_end)
    return 0;
  else
  {
    int c = srcbyte(ms, s);
    switch (patbyte(ms, p))
    {
    case '.':
      return 1; /* matches any char */
    case L_ESC:
      return match_class(c, patbyte(ms, p + 1));
    case '[':
      return matchbracketclass(ms, c, p, ep - 1);
    default:
      return (patbyte(ms, p) == c);
    }
  }
}

static ptrdiff_t matchbalance(MatchState* ms, ptrdiff_t s,
                              ptrdiff_t p)
{
  if (l_unlikely(p >= ms->p_end - 1))
    luaL_error(ms->L, HostString(lit<"malformed pattern (missing arguments to '%%b')">()->view()));
  if (srcbyte(ms, s) != patbyte(ms, p))
    return NOMATCH;
  else
  {
    int b = patbyte(ms, p);
    int e = patbyte(ms, p + 1);
    int cont = 1;
    while (++s < ms->src_end)
    {
      if (srcbyte(ms, s) == e)
      {
        if (--cont == 0)
          return s + 1;
      }
      else if (srcbyte(ms, s) == b)
        cont++;
    }
  }
  return NOMATCH; /* string ends out of balance */
}

static ptrdiff_t max_expand(MatchState* ms, ptrdiff_t s,
                            ptrdiff_t p, ptrdiff_t ep)
{
  ptrdiff_t i = 0; /* counts maximum expand for item */
  while (singlematch(ms, s + i, p, ep))
    i++;
  /* keeps trying to match with the maximum repetitions */
  while (i >= 0)
  {
    ptrdiff_t res = match(ms, (s + i), ep + 1);
    if (res != NOMATCH)
      return res;
    i--; /* else didn't match; reduce 1 repetition to try again */
  }
  return NOMATCH;
}

static ptrdiff_t min_expand(MatchState* ms, ptrdiff_t s,
                            ptrdiff_t p, ptrdiff_t ep)
{
  for (;;)
  {
    ptrdiff_t res = match(ms, s, ep + 1);
    if (res != NOMATCH)
      return res;
    else if (singlematch(ms, s, p, ep))
      s++; /* try with one more repetition */
    else
      return NOMATCH;
  }
}

static ptrdiff_t start_capture(MatchState* ms, ptrdiff_t s,
                               ptrdiff_t p, int what)
{
  ptrdiff_t res;
  int level = ms->level;
  if (level >= LUA_MAXCAPTURES)
    luaL_error(ms->L, HostString(lit<"too many captures">()->view()));
  ms->capture[level].init = s;
  ms->capture[level].len = what;
  ms->level = level + 1;
  if ((res = match(ms, s, p)) == NOMATCH) /* match failed? */
    ms->level--;                          /* undo capture */
  return res;
}

static ptrdiff_t end_capture(MatchState* ms, ptrdiff_t s,
                             ptrdiff_t p)
{
  int l = capture_to_close(ms);
  ptrdiff_t res;
  ms->capture[l].len = s - ms->capture[l].init; /* close capture */
  if ((res = match(ms, s, p)) == NOMATCH)       /* match failed? */
    ms->capture[l].len = CAP_UNFINISHED;        /* undo capture */
  return res;
}

static ptrdiff_t match_capture(MatchState* ms, ptrdiff_t s, int l)
{
  size_t len;
  l = check_capture(ms, l);
  len = ((size_t)(ms->capture[l].len));
  if ((size_t) (ms->src_end - s) >= len &&
      memcmp(ms->src->bytes() + ms->capture[l].init,
             ms->src->bytes() + s, len) == 0)
    return s + ((ptrdiff_t)(len));
  else
    return NOMATCH;
}

static ptrdiff_t match(MatchState* ms, ptrdiff_t s, ptrdiff_t p)
{
  if (l_unlikely(ms->matchdepth-- == 0))
    luaL_error(ms->L, HostString(lit<"pattern too complex">()->view()));
  for (;;) /* loop to optimize tail recursion: 'continue' == return match(ms, s, p) */
  {
    if (p == ms->p_end) /* end of pattern? */
      break;
    if (patbyte(ms, p) == '(')
    {                                /* start capture */
      if (patbyte(ms, p + 1) == ')') /* position capture? */
        s = start_capture(ms, s, p + 2, CAP_POSITION);
      else
        s = start_capture(ms, s, p + 1, CAP_UNFINISHED);
      break;
    }
    else if (patbyte(ms, p) == ')')
    { /* end capture */
      s = end_capture(ms, s, p + 1);
      break;
    }
    else if (patbyte(ms, p) == '$' && (p + 1) == ms->p_end)
    { /* '$' as the last char in pattern (otherwise it is a plain class below) */
      s = (s == ms->src_end) ? s : NOMATCH; /* check end of string */
      break;
    }
    else if (patbyte(ms, p) == L_ESC && patbyte(ms, p + 1) == 'b')
    { /* balanced string? */
      s = matchbalance(ms, s, p + 2);
      if (s == NOMATCH) /* fail */
        break;
      p += 4;
      continue; /* return match(ms, s, p + 4); */
    }
    else if (patbyte(ms, p) == L_ESC && patbyte(ms, p + 1) == 'f')
    { /* frontier? */
      ptrdiff_t ep;
      int previous;
      p += 2;
      if (l_unlikely(patbyte(ms, p) != '['))
        luaL_error(ms->L, HostString(lit<"missing '[' after '%%f' in pattern">()->view()));
      ep = classend(ms, p); /* points to what is next */
      previous = (s == 0) ? '\0' : srcbyte(ms, s - 1);
      if (!matchbracketclass(ms, previous, p, ep - 1) &&
          matchbracketclass(ms, srcbyte(ms, s), p, ep - 1))
      {
        p = ep;
        continue; /* return match(ms, s, ep); */
      }
      s = NOMATCH; /* match failed */
      break;
    }
    else if (patbyte(ms, p) == L_ESC && '0' <= patbyte(ms, p + 1) &&
             patbyte(ms, p + 1) <= '9')
    { /* capture results (%0-%9)? */
      s = match_capture(ms, s, patbyte(ms, p + 1));
      if (s == NOMATCH)
        break;
      p += 2;
      continue; /* return match(ms, s, p + 2) */
    }
    else
    {                                    /* pattern class plus optional suffix */
      ptrdiff_t ep = classend(ms, p);    /* points to optional suffix */
      /* does not match at least once? */
      if (!singlematch(ms, s, p, ep))
      {
        int sfx = patbyte(ms, ep);
        if (sfx == '*' || sfx == '?' || sfx == '-')
        { /* accept empty? */
          p = ep + 1;
          continue; /* return match(ms, s, ep + 1); */
        }
        else          /* '+' or no suffix */
          s = NOMATCH; /* fail */
      }
      else
      { /* matched once */
        switch (patbyte(ms, ep))
        { /* handle optional suffix */
        case '?':
        { /* optional */
          ptrdiff_t res;
          if ((res = match(ms, s + 1, ep + 1)) != NOMATCH)
            s = res;
          else
          {
            p = ep + 1;
            continue; /* else return match(ms, s, ep + 1); */
          }
          break;
        }
        case '+': /* 1 or more repetitions */
          s++;    /* 1 match already done */
          /* FALLTHROUGH */
        case '*': /* 0 or more repetitions */
          s = max_expand(ms, s, p, ep);
          break;
        case '-': /* 0 or more repetitions (minimum) */
          s = min_expand(ms, s, p, ep);
          break;
        default: /* no suffix */
          s++;
          p = ep;
          continue; /* return match(ms, s + 1, ep); */
        }
      }
      break;
    }
  }
  ms->matchdepth++;
  return s;
}

/*
** Find the pattern bytes s2[0..l2) inside s1[off1..off1+l1). Returns
** the match offset into 's1', or npos when absent. (Java: indexOf
** over (byte[], off, len) pairs.)
*/
static size_t lmemfind(std::string_view s1, size_t off1, size_t l1,
                       std::string_view s2, size_t l2)
{
  if (l2 == 0)
    return off1; /* empty strings are everywhere */
  else if (l2 > l1)
    return std::string_view::npos; /* avoids a negative 'l1' */
  else
  {
    size_t p = off1;      /* scan offset into 's1' */
    l2--;                 /* 1st char is checked by the find below */
    l1 = l1 - l2;         /* 's2' cannot be found after that */
    while (l1 > 0)
    {
      size_t init = s1.find(s2[0], p); /* search for the 1st char */
      if (init == std::string_view::npos || init >= p + l1)
        break;                         /* not found in the window */
      init++; /* 1st char is already checked */
      /* compare the rest (Java: bounded byte loop / mismatch) */
      if (memcmp(s1.data() + init, s2.data() + 1, l2) == 0)
        return init - 1;
      else
      { /* correct 'l1' and 'p' to try again */
        l1 -= init - p;
        p = init;
      }
    }
    return std::string_view::npos; /* not found */
  }
}

/*
** get information about the i-th capture. If there are no captures
** and 'i==0', return information about the whole match, which
** is the range 's'..'e'. If the capture is a string, return
** its length and put its start offset in '*cap'. If it is an integer
** (a position), push it on the stack and return CAP_POSITION.
*/
static ptrdiff_t get_onecapture(MatchState* ms, int i, ptrdiff_t s,
                                ptrdiff_t e, ptrdiff_t* cap)
{
  if (i >= ms->level)
  {
    if (l_unlikely(i != 0))
    {
      lua_pushinteger(ms->L, i + 1);
      luaL_error(ms->L, HostString(lit<"invalid capture index %%%d">()->view()));
    }
    *cap = s;
    return (e - s);
  }
  else
  {
    ptrdiff_t capl = ms->capture[i].len;
    *cap = ms->capture[i].init;
    if (l_unlikely(capl == CAP_UNFINISHED))
      luaL_error(ms->L, HostString(lit<"unfinished capture">()->view()));
    else if (capl == CAP_POSITION)
      lua_pushinteger(ms->L, ((lua_Integer)(ms->capture[i].init)) + 1);
    return capl;
  }
}

/*
** Push the i-th capture on the stack.
*/
static void push_onecapture(MatchState* ms, int i, ptrdiff_t s,
                            ptrdiff_t e)
{
  ptrdiff_t cap;
  ptrdiff_t l = get_onecapture(ms, i, s, e, &cap);
  if (l != CAP_POSITION)
    lua_pushlstring(ms->L, ms->src->view(), ((size_t)(cap)), ((size_t)(l)));
  /* else position was already pushed */
}

static int push_captures(MatchState* ms, ptrdiff_t s, ptrdiff_t e)
{
  int i;
  int nlevels = (ms->level == 0 && s != NOMATCH) ? 1 : ms->level;
  luaL_checkstack(ms->L, nlevels, HostString(lit<"too many captures">()->view()));
  for (i = 0; i < nlevels; i++)
    push_onecapture(ms, i, s, e);
  return nlevels; /* number of strings pushed */
}

/* check whether pattern has no special characters */
static int nospecials(std::string_view p)
{
  /* bounded scan over the whole view (upstream hopped strpbrk over
     embedded NULs; NUL is not a special, so one find covers all) */
  return p.find_first_of(SPECIALS->view()) == std::string_view::npos;
}

/*
** Prepare state for matches. These fields are not affected by each match.
** An anchored pattern skips its '^' via the caller's start offset;
** the pattern string itself is never "advanced".
*/
static void prepstate(MatchState* ms, lua_State* L,
                      const InternalString* s, const InternalString* p)
{
  ms->L = L;
  ms->src = s;
  ms->src_end = ((ptrdiff_t)(s->size()));
  ms->p = p;
  ms->p_end = ((ptrdiff_t)(p->size()));
}

/*
** (Re)prepare state for a match, setting fields that change during
** each match.
*/
static void reprepstate(MatchState* ms)
{
  ms->matchdepth = MAXCCALLS;
  ms->level = 0;
}

static int str_find_aux(lua_State* L, int find)
{
  const InternalString* s = luaL_checkistring(L, 1);
  const InternalString* p = luaL_checkistring(L, 2);
  size_t ls = s->size();
  size_t lp = p->size();
  size_t init = posrelatI(luaL_optinteger(L, 3, 1), ls) - 1;
  if (init > ls)
  {                   /* start after string's end? */
    luaL_pushfail(L); /* cannot find anything */
    return 1;
  }
  /* explicit request or no special characters? */
  if (find && (lua_toboolean(L, 4) || nospecials(p->view())))
  {
    /* do a plain search */
    size_t pos = lmemfind(s->view(), init, ls - init, p->view(), lp);
    if (pos != std::string_view::npos)
    {
      lua_pushinteger(L, ((lua_Integer)(pos)) + 1);
      lua_pushinteger(L, ((lua_Integer)(pos + lp)));
      return 2;
    }
  }
  else
  {
    MatchState ms;
    ptrdiff_t s1 = ((ptrdiff_t)(init));
    int anchor = (lp > 0 && p->byteAt(0) == '^');
    ptrdiff_t pinit = anchor ? 1 : 0; /* skip anchor character */
    prepstate(&ms, L, s, p);
    do
    {
      ptrdiff_t res;
      reprepstate(&ms);
      if ((res = match(&ms, s1, pinit)) != NOMATCH)
      {
        if (find)
        {
          lua_pushinteger(L, ((lua_Integer)(s1)) + 1); /* start */
          lua_pushinteger(L, ((lua_Integer)(res)));    /* end */
          return push_captures(&ms, NOMATCH, 0) + 2;
        }
        else
          return push_captures(&ms, s1, res);
      }
    } while (s1++ < ms.src_end && !anchor);
  }
  luaL_pushfail(L); /* not found */
  return 1;
}

static int str_find(lua_State* L)
{
  return str_find_aux(L, 1);
}

static int str_match(lua_State* L)
{
  return str_find_aux(L, 0);
}

/* state for 'gmatch' */
struct GMatchState
{
  ptrdiff_t src;       /* current position */
  ptrdiff_t p;         /* pattern start offset */
  ptrdiff_t lastmatch; /* end of last match (NOMATCH if none) */
  MatchState ms;       /* match state */
};

static int gmatch_aux(lua_State* L)
{
  GMatchState* gm = (GMatchState*) lua_touserdata(L, lua_upvalueindex(3));
  ptrdiff_t src;
  gm->ms.L = L;
  for (src = gm->src; src <= gm->ms.src_end; src++)
  {
    ptrdiff_t e;
    reprepstate(&gm->ms);
    if ((e = match(&gm->ms, src, gm->p)) != NOMATCH && e != gm->lastmatch)
    {
      gm->src = gm->lastmatch = e;
      return push_captures(&gm->ms, src, e);
    }
  }
  return 0; /* not found */
}

static int gmatch(lua_State* L)
{
  const InternalString* s = luaL_checkistring(L, 1);
  const InternalString* p = luaL_checkistring(L, 2);
  size_t ls = s->size();
  size_t init = posrelatI(luaL_optinteger(L, 3, 1), ls) - 1;
  GMatchState* gm;
  lua_settop(L, 2); /* keep strings on closure to avoid being collected */
  gm = (GMatchState*) lua_newuserdatauv(L, sizeof(GMatchState), 0);
  if (init > ls)   /* start after string's end? */
    init = ls + 1; /* avoid overflows in 'src + init' */
  prepstate(&gm->ms, L, s, p);
  gm->src = ((ptrdiff_t)(init));
  gm->p = 0;
  gm->lastmatch = NOMATCH;
  lua_pushcclosure(L, gmatch_aux, 3);
  return 1;
}

static void add_s(MatchState* ms, luaL_Buffer* b, ptrdiff_t s,
                  ptrdiff_t e)
{
  lua_State* L = ms->L;
  const InternalString* news = lua_toistring(L, 3);
  std::string_view nv = news->view();
  size_t nb = 0; /* scan offset into 'news' */
  size_t pc;
  while ((pc = nv.find(L_ESC, nb)) != std::string_view::npos)
  {
    luaL_addlstring(b, nv, nb, pc - nb);
    pc++;                     /* skip ESC */
    if (sv_byteat(nv, pc) == L_ESC) /* '%%' */
      luaL_addchar(b, nv[pc]);
    else if (sv_byteat(nv, pc) == '0') /* '%0' */
      luaL_addlstring(b, ms->src->view(), ((size_t)(s)), ((size_t)(e - s)));
    else if (isdigit(sv_byteat(nv, pc)))
    { /* '%n' */
      ptrdiff_t cap;
      ptrdiff_t resl = get_onecapture(ms, nv[pc] - '1', s, e, &cap);
      if (resl == CAP_POSITION)
        luaL_addvalue(b); /* add position to accumulated result */
      else
        luaL_addlstring(b, ms->src->view(), ((size_t)(cap)), ((size_t)(resl)));
    }
    else
    {
      lua_pushinteger(L, L_ESC);
      luaL_error(L, HostString(lit<"invalid use of '%c' in replacement string">()->view()));
    }
    nb = pc + 1;
  }
  luaL_addlstring(b, nv, nb, nv.size() - nb);
}

/*
** Add the replacement value to the string buffer 'b'.
** Return true if the original string was changed. (Function calls and
** table indexing resulting in nil or false do not change the subject.)
*/
static int add_value(MatchState* ms, luaL_Buffer* b, ptrdiff_t s,
                     ptrdiff_t e, int tr)
{
  lua_State* L = ms->L;
  switch (tr)
  {
  case LUA_TFUNCTION:
  { /* call the function */
    int n;
    lua_pushvalue(L, 3);         /* push the function */
    n = push_captures(ms, s, e); /* all captures as arguments */
    lua_call(L, n, 1);           /* call it */
    break;
  }
  case LUA_TTABLE:
  {                               /* index the table */
    push_onecapture(ms, 0, s, e); /* first capture is the index */
    lua_gettable(L, 3);
    break;
  }
  default:
  {                     /* LUA_TNUMBER or LUA_TSTRING */
    add_s(ms, b, s, e); /* add value to the buffer */
    return 1;           /* something changed */
  }
  }
  if (!lua_toboolean(L, -1))
  {                /* nil or false? */
    lua_pop(L, 1); /* remove value */
    luaL_addlstring(b, ms->src->view(), ((size_t)(s)),
                    ((size_t)(e - s))); /* keep original text */
    return 0;                           /* no changes */
  }
  else if (l_unlikely(!lua_isstring(L, -1)))
  {
    lua_pushstring(L, luaL_typename(L, -1));
    return luaL_error(L, HostString(lit<"invalid replacement value (a %s)">()->view()));
  }
  else
  {
    luaL_addvalue(b); /* add result to accumulator */
    return 1;         /* something changed */
  }
}

static int str_gsub(lua_State* L)
{
  const InternalString* srcs = luaL_checkistring(L, 1); /* subject */
  const InternalString* pat = luaL_checkistring(L, 2);  /* pattern */
  size_t srcl = srcs->size();
  size_t lp = pat->size();
  ptrdiff_t src = 0;             /* current position */
  ptrdiff_t lastmatch = NOMATCH; /* end of last match */
  int tr = lua_type(L, 3);       /* replacement type */
  /* max replacements */
  lua_Integer max_s = luaL_optinteger(L, 4, ((lua_Integer)(srcl)) + 1);
  int anchor = (lp > 0 && pat->byteAt(0) == '^');
  ptrdiff_t pinit = anchor ? 1 : 0; /* skip anchor character */
  lua_Integer n = 0;                /* replacement count */
  int changed = 0;                  /* change flag */
  MatchState ms;
  luaL_Buffer b;
  luaL_argexpected(L, tr == LUA_TNUMBER || tr == LUA_TSTRING || tr == LUA_TFUNCTION || tr == LUA_TTABLE, 3,
                   HostString(lit<"string/function/table">()->view()));
  luaL_buffinit(L, &b);
  prepstate(&ms, L, srcs, pat);
  while (n < max_s)
  {
    ptrdiff_t e;
    reprepstate(&ms); /* (re)prepare state for new match */
    if ((e = match(&ms, src, pinit)) != NOMATCH && e != lastmatch)
    { /* match? */
      n++;
      changed = add_value(&ms, &b, src, e, tr) || changed;
      src = lastmatch = e;
    }
    else if (src < ms.src_end) /* otherwise, skip one character */
      luaL_addchar(&b, ((char)(srcs->byteAt(((size_t)(src++))))));
    else
      break; /* end of subject */
    if (anchor)
      break;
  }
  if (!changed)          /* no changes? */
    lua_pushvalue(L, 1); /* return original string */
  else
  { /* something changed */
    luaL_addlstring(&b, srcs->view(), ((size_t)(src)), ((size_t)(ms.src_end - src)));
    luaL_pushresult(&b); /* create and return new string */
  }
  lua_pushinteger(L, n); /* number of substitutions */
  return 2;
}

/* }====================================================== */

/*
** {======================================================
** STRING FORMAT
** =======================================================
*/


/*
** Hexadecimal floating-point formatter
*/



/*
** Number of bits that goes into the first digit. It can be any value
** between 1 and 4; the following definition tries to align the number
** to nibble boundaries by making what is left after that first digit a
** multiple of 4.
*/
constexpr int L_NBFD = ((DBL_MANT_DIG - 1) % 4 + 1);

/*
** Add integer part of 'x' to buffer and return new 'x'
*/
static lua_Number adddigit(std::span<char> buff, size_t n, lua_Number x)
{
  lua_Number dd = floor(x); /* get integer part from 'x' */
  int d = (int) dd;
  buff[n] = ((char)(d < 10 ? d + '0' : d - 10 + 'a')); /* add to buffer */
  return x - dd;                                        /* return what is left */
}

static int num2straux(std::span<char> area, size_t off, unsigned sz, lua_Number x)
{
  /* if 'inf' or 'NaN', format it like '%g' */
  if (x != x)
  {
    std::string_view r = std::signbit(x) ? lit<"-nan">()->view() : lit<"nan">()->view();
    assert(sz > r.size());
    memcpy(area.data() + off, r.data(), r.size());
    return ((int)(r.size()));
  }
  else if (x == (lua_Number) HUGE_VAL || x == -(lua_Number) HUGE_VAL)
  {
    std::string_view r = (x < 0) ? lit<"-inf">()->view() : lit<"inf">()->view();
    memcpy(area.data() + off, r.data(), r.size());
    return ((int)(r.size()));
  }
  else if (x == 0)
  { /* can be -0... : "0" or "-0" followed by a zero hex exponent */
    std::string_view r = std::signbit(x) ? lit<"-0x0p+0">()->view() : lit<"0x0p+0">()->view();
    memcpy(area.data() + off, r.data(), r.size());
    return ((int)(r.size()));
  }
  else
  {
    int e;
    lua_Number m = frexp(x, &e); /* 'x' fraction and exponent */
    unsigned n = 0;                        /* character count */
    if (m < 0)
    {                        /* is number negative? */
      area[off + n++] = '-'; /* add sign */
      m = -m;                /* make it positive */
    }
    area[off + n++] = '0';
    area[off + n++] = 'x';                            /* add "0x" */
    m = adddigit(area, off + n++, m * (1 << L_NBFD)); /* add first digit */
    e -= L_NBFD;                                      /* this digit goes before the radix point */
    if (m > 0)
    {                                      /* more digits? */
      area[off + n++] = ((char)(lua_getlocaledecpoint())); /* add radix point */
      do
      { /* add as many digits as needed */
        m = adddigit(area, off + n++, m * 16);
      } while (m > 0);
    }
    area[off + n++] = 'p'; /* add exponent, always signed */
    area[off + n++] = (e >= 0) ? '+' : '-';
    std::string es = std::to_string(e >= 0 ? e : -e);
    assert(n + es.size() < sz);
    memcpy(area.data() + off + n, es.c_str(), es.size());
    n += ((unsigned int)(es.size()));
    assert(n < sz);
    return ((int)(n));
  }
}

static int lua_number2strx(lua_State* L, std::span<char> area, size_t off,
                           unsigned sz, std::string_view fmt, lua_Number x)
{
  int n = num2straux(area, off, sz, x);
  if (fmt[1] == 'A') /* fmt is "%a" or "%A" (no length modifier for double) */
  {
    int i;
    for (i = 0; i < n; i++)
      area[off + i] = ((char)(toupper(((unsigned char)(area[off + i])))));
  }
  else if (l_unlikely(fmt[1] != 'a'))
    return luaL_error(L, HostString(lit<"modifiers for format '%%a'/'%%A' not implemented">()->view()));
  return n;
}


/*
** Maximum size for items formatted with '%f'. This size is produced
** by format('%.99f', -maxfloat), and is equal to 99 + 3 ('-', '.',
** and '\0') + number of decimal digits to represent maxfloat (which
** is maximum exponent + 1). (99+3+1, adding some extra, 110)
*/
constexpr int MAX_ITEMF = (110 + DBL_MAX_10_EXP);

/*
** All formats except '%f' do not need that large limit.  The other
** float formats use exponents, so that they fit in the 99 limit for
** significant digits; 's' for large strings and 'q' add items directly
** to the buffer; all integer formats also fit in the 99 limit.  The
** worst case are floats: they may need 99 significant digits, plus
** '0x', '-', '.', 'e+XXXX', and '\0'. Adding some extra, 120.
*/
constexpr int MAX_ITEM = 120;

/* valid flags in a format specification */
#if !defined(L_FMTFLAGSF)

/* valid flags for a, A, e, E, f, F, g, and G conversions */
#define L_FMTFLAGSF "-+#0 "

/* valid flags for o, x, and X conversions */
static const InternalString* const L_FMTFLAGSX = lit<"-#0">();

/* valid flags for d and i conversions */
static const InternalString* const L_FMTFLAGSI = lit<"-+0 ">();

/* valid flags for u conversions */
static const InternalString* const L_FMTFLAGSU = lit<"-0">();

/* valid flags for c, p, and s conversions */
static const InternalString* const L_FMTFLAGSC = lit<"-">();

#endif

/*
** Maximum size of each format specification (such as "%-099.99d"):
** Initial '%', flags (up to 5), width (2), period, precision (2),
** length modifier (8), conversion specifier, and final '\0', plus some
** extra.
*/
constexpr int MAX_FORMAT = 32;

static void addquoted(luaL_Buffer* b, const InternalString* s)
{
  size_t len = s->size();
  size_t i = 0;
  luaL_addchar(b, '"');
  for (; i < len; i++)
  {
    int c = s->byteAt(i);
    if (c == '"' || c == '\\' || c == '\n')
    {
      luaL_addchar(b, '\\');
      luaL_addchar(b, ((char)(c)));
    }
    else if (iscntrl(c))
    { /* '\ddd' escape; 3 digits when followed by another digit */
      std::string d = std::to_string(c);
      luaL_addchar(b, '\\');
      if (isdigit(istrbyte(s, i + 1)))
      {
        for (size_t j = d.size(); j < 3; j++)
          luaL_addchar(b, '0');
      }
      luaL_addstring(b, d.c_str());
    }
    else
      luaL_addchar(b, ((char)(c)));
  }
  luaL_addchar(b, '"');
}

/*
** Serialize a floating-point number in such a way that it can be
** scanned back by Lua. Use hexadecimal format for "common" numbers
** (to preserve precision); inf, -inf, and NaN are handled separately.
** (NaN cannot be expressed as a numeral, so we write '(0/0)' for it.)
*/
static int quotefloat(lua_State* L, std::span<char> area, size_t off, lua_Number n)
{
  const InternalString* s;        /* for the fixed representations */
  if (n == (lua_Number) HUGE_VAL) /* inf? */
    s = lit<"1e9999">();
  else if (n == -(lua_Number) HUGE_VAL) /* -inf? */
    s = lit<"-1e9999">();
  else if (n != n) /* NaN? */
    s = lit<"(0/0)">();
  else
  { /* format number as hexadecimal */
    int nb = lua_number2strx(L, area, off, MAX_ITEM,
                             lit<"%a">()->view(), n);
    /* ensures that the numeral uses a dot as the radix character */
    if (memchr(area.data() + off, '.', ((unsigned int)(nb))) == nullptr)
    {                                                 /* no dot? */
      char point = ((char)(lua_getlocaledecpoint())); /* try locale point */
      char* ppoint = (char*) memchr(area.data() + off, point, ((unsigned int)(nb)));
      if (ppoint)
        *ppoint = '.'; /* change it to a dot */
    }
    return nb;
  }
  /* for the fixed representations */
  size_t l = s->view().size();
  assert(l < MAX_ITEM);
  memcpy(area.data() + off, s->view().data(), l);
  area[off + l] = '\0';
  return ((int)(l));
}

static void addliteral(lua_State* L, luaL_Buffer* b, int arg)
{
  switch (lua_type(L, arg))
  {
  case LUA_TSTRING:
  {
    addquoted(b, lua_toistring(L, arg));
    break;
  }
  case LUA_TNUMBER:
  {
    size_t off = luaL_prepbuffsize(b, MAX_ITEM);
    std::span<char> area = luaL_buffarea(b);
    int nb;
    if (!lua_isinteger(L, arg)) /* float? */
      nb = quotefloat(L, area, off, lua_tonumber(L, arg));
    else
    { /* integers */
      lua_Integer n = lua_tointeger(L, arg);
      if (n == LUA_MININTEGER)
      { /* corner case: use hex, which reads back as the same value */
        area[off] = '0';
        area[off + 1] = 'x';
        std::to_chars_result res = std::to_chars(
            area.data() + off + 2, area.data() + off + MAX_ITEM, ((lua_Unsigned)(n)), 16);
        assert(res.ec == std::errc());
        nb = ((int)(res.ptr - (area.data() + off)));
      }
      else
      {
        std::string d = std::to_string(n);
        assert(d.size() < MAX_ITEM);
        memcpy(area.data() + off, d.c_str(), d.size());
        nb = ((int)(d.size()));
      }
    }
    luaL_addsize(b, ((unsigned int)(nb)));
    break;
  }
  case LUA_TNIL:
  case LUA_TBOOLEAN:
  {
    luaL_toistring(L, arg); /* internal twin: push only */
    luaL_addvalue(b);
    break;
  }
  default:
  {
    luaL_argerror(L, arg, HostString(lit<"value has no literal form">()->view()));
  }
  }
}

static size_t get2digits(std::string_view s, size_t p)
{
  if (isdigit(sv_byteat(s, p)))
  {
    p++;
    if (isdigit(sv_byteat(s, p)))
      p++; /* (2 digits at most) */
  }
  return p;
}

/*
** Check whether a conversion specification is valid. When called,
** first character in 'form' must be '%' and last character must
** be a valid conversion specifier. 'flags' are the accepted flags;
** 'precision' signals whether to accept a precision.
*/
static void checkformat(lua_State* L, std::string_view form,
                        std::string_view flags, int precision)
{
  size_t p = 1;                              /* skip '%' */
  while (p < form.size() && flags.find(form[p]) != std::string_view::npos)
    p++;                                     /* skip flags */
  if (sv_byteat(form, p) != '0')
  {                            /* a width cannot start with '0' */
    p = get2digits(form, p);   /* skip width */
    if (sv_byteat(form, p) == '.' && precision)
    {
      p++;
      p = get2digits(form, p); /* skip precision */
    }
  }
  if (!(p + 1 == form.size() && isalpha(sv_byteat(form, p))))
  { /* did not go to the end? */
    lua_pushlstring(L, form, 0, form.size());
    luaL_error(L, HostString(lit<"invalid conversion specification: '%s'">()->view()));
  }
}

/*
** Get a conversion specification (starting just after its '%' at
** offset 'i' of 'sf') and copy it to 'form'.
** Return the offset of its last character.
*/
static size_t getformat(lua_State* L, const InternalString* sf, size_t i,
                        std::span<char> form, size_t* formlen)
{
  /* spans flags, width, and precision ('0' is included as a flag) */
  std::string_view sv = sf->view();
  std::string_view fset = lit<L_FMTFLAGSF "123456789.">()->view();
  size_t len = 0;
  while (i + len < sv.size() && fset.find(sv[i + len]) != std::string_view::npos)
    len++;
  len++; /* adds following character (should be the specifier) */
  /* still needs space for '%', '\0', plus a length modifier */
  if (len >= MAX_FORMAT - 10)
    luaL_error(L, HostString(lit<"invalid format (too long)">()->view()));
  form[0] = '%';
  /* Java: System.arraycopy(sf, i, form, 1, len) */
  memcpy(form.data() + 1, sv.data() + i, len * sizeof(char));
  form[1 + len] = '\0';
  *formlen = 1 + len;
  return i + len - 1;
}

/*
** A parsed conversion specification. Filled by 'scanformat' from a
** 'form' already validated by 'checkformat'.
*/
struct FormatSpec
{
  bool left;     /* '-' flag */
  bool plus;     /* '+' flag */
  bool space;    /* ' ' flag */
  bool alt;      /* '#' flag */
  bool zero;     /* '0' flag */
  int width;     /* minimum field width (0 if absent) */
  int precision; /* precision (-1 if absent) */
  char conv;     /* conversion specifier (last character of 'form') */
};

/*
** Parse a validated conversion specification into a FormatSpec.
** 'form' starts with '%' and ends with the conversion specifier;
** flags may repeat, width and precision have at most 2 digits each
** (both guaranteed by 'checkformat').
*/
static void scanformat(std::string_view form, FormatSpec* spec)
{
  size_t p = 1; /* skip '%' */
  *spec = FormatSpec{};
  spec->precision = -1;
  for (;; p++)
  { /* flags (any order, repeats allowed) */
    switch (sv_byteat(form, p))
    {
      case '-': spec->left = true; continue;
      case '+': spec->plus = true; continue;
      case ' ': spec->space = true; continue;
      case '#': spec->alt = true; continue;
      case '0': spec->zero = true; continue;
    }
    break;
  }
  while (isdigit(sv_byteat(form, p))) /* width */
    spec->width = spec->width * 10 + (form[p++] - '0');
  if (sv_byteat(form, p) == '.')
  { /* precision ('.' alone means 0) */
    p++;
    spec->precision = 0;
    while (isdigit(sv_byteat(form, p)))
      spec->precision = spec->precision * 10 + (form[p++] - '0');
  }
  spec->conv = form[form.size() - 1];
}

/*
** Assemble a formatted item into 'buff' from its sign/prefix and body,
** applying width padding: left-justified pads spaces on the right;
** zero padding (when permitted for the item) goes between the prefix
** and the body; otherwise spaces pad on the left. Returns the number
** of bytes written.
*/
static int packformat(std::span<char> area, size_t off, unsigned maxitem,
                      const FormatSpec* spec,
                      std::string_view prefix, std::string_view body,
                      bool zeropadok)
{
  size_t prefixlen = prefix.size();
  size_t bodylen = body.size();
  size_t content = prefixlen + bodylen;
  size_t width = ((size_t)(spec->width));
  size_t total = (content > width) ? content : width;
  size_t pad = total - content;
  size_t pos = off; /* write position (Java: arraycopy/fill offsets) */
  assert(total < maxitem);
  (void) maxitem;
  if (spec->left)
  { /* '[prefix][body][spaces]' */
    memcpy(area.data() + pos, prefix.data(), prefixlen);
    pos += prefixlen;
    memcpy(area.data() + pos, body.data(), bodylen);
    pos += bodylen;
    memset(area.data() + pos, ' ', pad);
  }
  else if (spec->zero && zeropadok)
  { /* '[prefix][zeros][body]' */
    memcpy(area.data() + pos, prefix.data(), prefixlen);
    pos += prefixlen;
    memset(area.data() + pos, '0', pad);
    pos += pad;
    memcpy(area.data() + pos, body.data(), bodylen);
  }
  else
  { /* '[spaces][prefix][body]' */
    memset(area.data() + pos, ' ', pad);
    pos += pad;
    memcpy(area.data() + pos, prefix.data(), prefixlen);
    pos += prefixlen;
    memcpy(area.data() + pos, body.data(), bodylen);
  }
  return ((int)(total));
}

/*
** Format one integer item; shared body for the 'd'/'i', 'u', and
** 'o'/'x'/'X' conversions, which differ only in their accepted flags.
** Digits come from 'std::to_chars' on the magnitude; sign, base
** prefix, precision zero-extension, and padding are applied
** explicitly. Returns the number of bytes written to 'buff'.
*/
static int formatint(lua_State* L, int arg, std::string_view form,
                     std::span<char> area, size_t off,
                     unsigned maxitem, std::string_view flags)
{
  lua_Integer n = luaL_checkinteger(L, arg);
  checkformat(L, form, flags, 1);
  FormatSpec spec;
  scanformat(form, &spec);
  bool issigned = (spec.conv == 'd' || spec.conv == 'i');
  int base = (spec.conv == 'o') ? 8
             : (spec.conv == 'x' || spec.conv == 'X') ? 16
                                                      : 10;
  /* magnitude for signed conversions; two's-complement bits otherwise */
  unsigned long long u = (issigned && n < 0)
                             ? 0ULL - ((unsigned long long)(n))
                             : ((unsigned long long)(n));
  char digits[24];
  std::to_chars_result res = std::to_chars(digits, digits + sizeof(digits), u, base);
  assert(res.ec == std::errc());
  size_t ndigits = ((size_t)(res.ptr - digits));
  if (spec.conv == 'X')
  {
    for (size_t i = 0; i < ndigits; i++)
      digits[i] = ((char)(toupper(((unsigned char)(digits[i])))));
  }
  char body[MAX_ITEM];
  size_t blen = 0;
  if (u == 0 && spec.precision == 0)
    ; /* explicit zero precision with value 0: no digits */
  else
  {
    size_t mindigits = (spec.precision >= 0) ? ((size_t)(spec.precision)) : 1;
    while (blen + ndigits < mindigits)
      body[blen++] = '0'; /* zero-extend to the precision */
    memcpy(body + blen, digits, ndigits);
    blen += ndigits;
  }
  if (spec.alt && spec.conv == 'o' && (blen == 0 || body[0] != '0'))
  { /* '#o': force a leading zero digit */
    memmove(body + 1, body, blen);
    body[0] = '0';
    blen++;
  }
  char prefix[2];
  size_t prefixlen = 0;
  if (issigned)
  { /* sign character, if any */
    if (n < 0)
      prefix[prefixlen++] = '-';
    else if (spec.plus)
      prefix[prefixlen++] = '+';
    else if (spec.space)
      prefix[prefixlen++] = ' ';
  }
  else if (spec.alt && (spec.conv == 'x' || spec.conv == 'X') && u != 0)
  { /* '#x': base prefix (nonzero values only) */
    prefix[prefixlen++] = '0';
    prefix[prefixlen++] = spec.conv;
  }
  /* an explicit precision disables zero padding for integers */
  return packformat(area, off, maxitem, &spec,
                    std::string_view{prefix, prefixlen},
                    std::string_view{body, blen}, spec.precision < 0);
}

/*
** Format one float item for the 'f', 'e'/'E', and 'g'/'G' conversions.
** The digits come from 'std::to_chars' with explicit precision (which
** matches printf output byte for byte); the '#' flag, sign flags, and
** padding are applied explicitly. Returns the number of bytes written
** to 'buff'.
*/
static int formatfloat(lua_State* L, int arg, std::string_view form,
                       std::span<char> area, size_t off, unsigned maxitem)
{
  lua_Number n = luaL_checknumber(L, arg);
  checkformat(L, form, lit<L_FMTFLAGSF>()->view(), 1);
  FormatSpec spec;
  scanformat(form, &spec);
  char lower = ((char)(tolower(((unsigned char)(spec.conv)))));
  int precision = (spec.precision >= 0) ? spec.precision : 6;
  char prefix[1];
  size_t prefixlen = 0;
  if (std::signbit(n)) /* sign applies to NaN and infinities too */
    prefix[prefixlen++] = '-';
  else if (spec.plus)
    prefix[prefixlen++] = '+';
  else if (spec.space)
    prefix[prefixlen++] = ' ';
  char body[MAX_ITEMF];
  size_t blen;
  bool zeropadok = true;
  if (std::isinf(n) || std::isnan(n))
  { /* fixed representations; zero padding does not apply */
    std::string_view r = std::isinf(n) ? "inf" : "nan";
    blen = r.size();
    memcpy(body, r.data(), blen);
    zeropadok = false;
  }
  else
  {
    std::chars_format cf = (lower == 'f')   ? std::chars_format::fixed
                           : (lower == 'e') ? std::chars_format::scientific
                                            : std::chars_format::general;
    if (lower == 'g' && precision == 0)
      precision = 1; /* '%g' treats zero precision as 1 */
    std::to_chars_result res = std::to_chars(body, body + sizeof(body),
                                             std::fabs(n), cf, precision);
    assert(res.ec == std::errc());
    blen = ((size_t)(res.ptr - body));
    if (spec.alt)
    { /* '#': always show the radix point; '%g' also keeps zeros */
      size_t epos = 0; /* position of the exponent (or end of body) */
      bool haspoint = false;
      while (epos < blen && body[epos] != 'e')
      {
        if (body[epos] == '.')
          haspoint = true;
        epos++;
      }
      if (!haspoint)
      { /* insert the radix point before the exponent */
        memmove(body + epos + 1, body + epos, blen - epos);
        body[epos++] = '.';
        blen++;
      }
      if (lower == 'g')
      { /* restore trailing zeros up to 'precision' significant digits */
        size_t sig = 0;
        bool seennonzero = false;
        for (size_t i = 0; i < epos; i++)
        {
          if (isdigit(((unsigned char)(body[i]))))
          {
            if (body[i] != '0')
              seennonzero = true;
            if (seennonzero)
              sig++;
          }
        }
        if (sig == 0)
          sig = 1; /* the value 0 has one significant digit */
        size_t zeros = (((size_t)(precision)) > sig) ? precision - sig : 0;
        memmove(body + epos + zeros, body + epos, blen - epos);
        memset(body + epos, '0', zeros);
        blen += zeros;
      }
    }
  }
  if (spec.conv == 'E' || spec.conv == 'G')
  { /* uppercase the body ('e', and 'inf'/'nan') */
    for (size_t i = 0; i < blen; i++)
      body[i] = ((char)(toupper(((unsigned char)(body[i])))));
  }
  return packformat(area, off, maxitem, &spec,
                    std::string_view{prefix, prefixlen},
                    std::string_view{body, blen}, zeropadok);
}

static int str_format(lua_State* L)
{
  int top = lua_gettop(L);
  int arg = 1;
  const InternalString* sf = luaL_checkistring(L, arg);
  size_t sfl = sf->size();
  size_t i = 0; /* cursor into 'sf' */
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  while (i < sfl)
  {
    if (sf->byteAt(i) != L_ESC)
      luaL_addchar(&b, ((char)(sf->byteAt(i++))));
    else if (istrbyte(sf, ++i) == L_ESC)
    {
      luaL_addchar(&b, ((char)(sf->byteAt(i)))); /* %% */
      i++;
    }
    else
    {                                              /* format item */
      char form[MAX_FORMAT];                        /* to store the format ('%...') */
      size_t formlen;                               /* its length */
      unsigned maxitem = MAX_ITEM;                  /* maximum length for the result */
      size_t boff = luaL_prepbuffsize(&b, maxitem); /* to put result */
      int nb = 0;                                   /* number of bytes in result */
      if (++arg > top)
        return luaL_argerror(L, arg, HostString(lit<"no value">()->view()));
      i = getformat(L, sf, i, form, &formlen);
      std::string_view formv{form, formlen};
      switch (istrbyte(sf, i++))
      {
      case 'c':
      {
        checkformat(L, formv, L_FMTFLAGSC->view(), 0);
        FormatSpec spec;
        scanformat(formv, &spec);
        char c = ((char)((unsigned char)(luaL_checkinteger(L, arg))));
        nb = packformat(luaL_buffarea(&b), boff, maxitem, &spec, "",
                        std::string_view{&c, 1}, false);
        break;
      }
      case 'd':
      case 'i':
        nb = formatint(L, arg, formv, luaL_buffarea(&b), boff, maxitem, L_FMTFLAGSI->view());
        break;
      case 'u':
        nb = formatint(L, arg, formv, luaL_buffarea(&b), boff, maxitem, L_FMTFLAGSU->view());
        break;
      case 'o':
      case 'x':
      case 'X':
        nb = formatint(L, arg, formv, luaL_buffarea(&b), boff, maxitem, L_FMTFLAGSX->view());
        break;
      case 'a':
      case 'A':
        checkformat(L, formv, lit<L_FMTFLAGSF>()->view(), 1);
        nb = lua_number2strx(L, luaL_buffarea(&b), boff, maxitem, formv,
                             luaL_checknumber(L, arg));
        break;
      case 'f':
        maxitem = MAX_ITEMF; /* extra space for '%f' */
        boff = luaL_prepbuffsize(&b, maxitem);
        /* FALLTHROUGH */
      case 'e':
      case 'E':
      case 'g':
      case 'G':
      {
        nb = formatfloat(L, arg, formv, luaL_buffarea(&b), boff, maxitem);
        break;
      }
      case 'p':
      {
        checkformat(L, formv, L_FMTFLAGSC->view(), 0);
        FormatSpec spec;
        scanformat(formv, &spec);
        char body[MAX_ITEM];
        size_t blen;
        if (lua_islightcfunction(L, arg))
        { /* light C function — body is its FuncAddress string */
          std::string fa = lua_funcaddressi(L, arg); /* engine bytes */
          blen = fa.size();
          assert(blen < sizeof(body));
          memcpy(body, fa.data(), blen);
        }
        else
        {
          const void* p = lua_topointer(L, arg);
          if (p == nullptr)
          { /* null pointer has a fixed representation */
            const std::string_view nullrep = lit<"(null)">()->view();
            blen = nullrep.size();
            memcpy(body, nullrep.data(), blen);
          }
          else
          { /* "0x" + lowercase hex digits */
            body[0] = '0';
            body[1] = 'x';
            std::to_chars_result res = std::to_chars(
                body + 2, body + sizeof(body), ((uintptr_t)(p)), 16);
            assert(res.ec == std::errc());
            blen = ((size_t)(res.ptr - body));
          }
        }
        nb = packformat(luaL_buffarea(&b), boff, maxitem, &spec, "",
                        std::string_view{body, blen}, false);
        break;
      }
      case 'q':
      {
        if (formlen != 2) /* modifiers? */
          return luaL_error(L, HostString(lit<"specifier '%%q' cannot have modifiers">()->view()));
        addliteral(L, &b, arg);
        break;
      }
      case 's':
      {
        const InternalString* s = luaL_toistring(L, arg);
        size_t l = s->size();
        if (formlen == 2) /* no modifiers? */
          luaL_addvalue(&b); /* keep entire string */
        else
        {
          luaL_argcheck(L, memchr(s->bytes(), '\0', l) == nullptr, arg,
                        HostString(lit<"string contains zeros">()->view()));
          checkformat(L, formv, L_FMTFLAGSC->view(), 1);
          if (formv.find('.') == std::string_view::npos && l >= 100)
          {
            /* no precision and string is too long to be formatted */
            luaL_addvalue(&b); /* keep entire string */
          }
          else
          { /* format the string into 'buff' */
            FormatSpec spec;
            scanformat(formv, &spec);
            size_t blen = (spec.precision >= 0 && ((size_t)(spec.precision)) < l)
                              ? ((size_t)(spec.precision)) /* truncate */
                              : l;
            nb = packformat(luaL_buffarea(&b), boff, maxitem, &spec, "",
                            std::string_view{s->bytes(), blen}, false);
            lua_pop(L, 1); /* remove result from 'luaL_toistring' */
          }
        }
        break;
      }
      default:
      { /* also treat cases 'pnLlh' */
        lua_pushlstring(L, formv, 0, formv.size());
        return luaL_error(L, HostString(lit<"invalid conversion '%s' to 'format'">()->view()));
      }
      }
      assert(((unsigned int)(nb)) < maxitem);
      luaL_addsize(&b, ((unsigned int)(nb)));
    }
  }
  luaL_pushresult(&b);
  return 1;
}

/* }====================================================== */

/*
** {======================================================
** PACK/UNPACK
** =======================================================
*/

/* value used for padding */
#if !defined(LUAL_PACKPADBYTE)
#define LUAL_PACKPADBYTE 0x00
#endif

/* maximum size for the binary representation of an integer */
constexpr int MAXINTSIZE = 16;

/* number of bits in a character */
constexpr int NB = CHAR_BIT;

/* mask for one character (NB 1's) */
constexpr int MC = ((1 << NB) - 1);

/* size of a lua_Integer */
constexpr int SZINT = ((int) sizeof(lua_Integer));

/* Native byte order for pack/unpack's '=' mode. The host is guaranteed
   little-endian (static_assert in llimits.h), so native-mode blobs are
   always little-endian; on a big-endian host their layout would have
   differed (that build fails the assert instead). */
constexpr bool nativeendian_little = (std::endian::native == std::endian::little);

/*
** information to pack/unpack stuff. The format string is hoisted here
** as an InternalString* and the cursor is a plain offset ('pos') —
** the (string, offset) interior-cursor ruling. A read at/past the end
** (or at an embedded NUL) yields '\0', preserving upstream's
** terminator-based stops.
*/
struct Header
{
  lua_State* L;
  const InternalString* fmt; /* format string */
  size_t pos;                /* cursor into 'fmt' */
  int islittle;
  unsigned maxalign;
};

/* current format byte (0 at end / embedded NUL — see above) */
static inline int fmtbyte(const Header* h)
{
  return istrbyte(h->fmt, h->pos);
}

/*
** options for pack/unpack
*/
typedef enum KOption
{
  Kint,       /* signed integers */
  Kuint,      /* unsigned integers */
  Kfloat,     /* single-precision floating-point numbers */
  Knumber,    /* Lua "native" floating-point numbers */
  Kdouble,    /* double-precision floating-point numbers */
  Kchar,      /* fixed-length strings */
  Kstring,    /* strings with prefixed length */
  Kzstr,      /* zero-terminated strings */
  Kpadding,   /* padding */
  Kpaddalign, /* padding for alignment */
  Knop        /* no-op (configuration or spaces) */
} KOption;

/*
** Read an integer numeral from string 'fmt' or return 'df' if
** there is no numeral
*/
static int digit(int c)
{
  return '0' <= c && c <= '9';
}

static size_t getnum(Header* h, size_t df)
{
  if (!digit(fmtbyte(h))) /* no number? */
    return df;            /* return default value */
  else
  {
    size_t a = 0;
    do
    {
      a = a * 10 + ((unsigned int)(fmtbyte(h) - '0'));
      h->pos++;
    } while (digit(fmtbyte(h)) && a <= (MAX_SIZE - 9) / 10);
    return a;
  }
}

/*
** Read an integer numeral and raises an error if it is larger
** than the maximum size of integers.
*/
static unsigned getnumlimit(Header* h, size_t df)
{
  size_t sz = getnum(h, df);
  if (l_unlikely((sz - 1u) >= MAXINTSIZE))
  {
    lua_pushinteger(h->L, ((lua_Integer)(sz)));
    lua_pushinteger(h->L, MAXINTSIZE);
    return ((unsigned int)(luaL_error(h->L, HostString(lit<"integral size (%d) out of limits [1,%d]">()->view()))));
  }
  return ((unsigned int)(sz));
}

/*
** Initialize Header
*/
static void initheader(lua_State* L, Header* h, const InternalString* fmt)
{
  h->L = L;
  h->fmt = fmt;
  h->pos = 0;
  h->islittle = nativeendian_little;
  h->maxalign = 1;
}

/*
** Read and classify next option. 'size' is filled with option's size.
*/
static KOption getoption(Header* h, size_t* size)
{
  /* native maximum alignment requirement */
  constexpr size_t maxalign = alignof(std::max_align_t);
  int opt = fmtbyte(h);
  h->pos++;
  *size = 0; /* default */
  switch (opt)
  {
  case 'b':
    *size = sizeof(char);
    return Kint;
  case 'B':
    *size = sizeof(char);
    return Kuint;
  case 'h':
    *size = sizeof(short);
    return Kint;
  case 'H':
    *size = sizeof(short);
    return Kuint;
  case 'l':
    *size = sizeof(long);
    return Kint;
  case 'L':
    *size = sizeof(long);
    return Kuint;
  case 'j':
    *size = sizeof(lua_Integer);
    return Kint;
  case 'J':
    *size = sizeof(lua_Integer);
    return Kuint;
  case 'T':
    *size = sizeof(size_t);
    return Kuint;
  case 'f':
    *size = sizeof(float);
    return Kfloat;
  case 'n':
    *size = sizeof(lua_Number);
    return Knumber;
  case 'd':
    *size = sizeof(double);
    return Kdouble;
  case 'i':
    *size = getnumlimit(h, sizeof(int));
    return Kint;
  case 'I':
    *size = getnumlimit(h, sizeof(int));
    return Kuint;
  case 's':
    *size = getnumlimit(h, sizeof(size_t));
    return Kstring;
  case 'c':
    *size = getnum(h, ((size_t)(-1)));
    if (l_unlikely(*size == ((size_t)(-1))))
      luaL_error(h->L, HostString(lit<"missing size for format option 'c'">()->view()));
    return Kchar;
  case 'z':
    return Kzstr;
  case 'x':
    *size = 1;
    return Kpadding;
  case 'X':
    return Kpaddalign;
  case ' ':
    break;
  case '<':
    h->islittle = 1;
    break;
  case '>':
    h->islittle = 0;
    break;
  case '=':
    h->islittle = nativeendian_little;
    break;
  case '!':
  {
    h->maxalign = getnumlimit(h, maxalign);
    break;
  }
  default:
    lua_pushinteger(h->L, opt);
    luaL_error(h->L, HostString(lit<"invalid format option '%c'">()->view()));
  }
  return Knop;
}

/*
** Read, classify, and fill other details about the next option.
** 'psize' is filled with option's size, 'notoalign' with its
** alignment requirements.
** Local variable 'size' gets the size to be aligned. (Kpadal option
** always gets its full alignment, other options are limited by
** the maximum alignment ('maxalign'). Kchar option needs no alignment
** despite its size.
*/
static KOption getdetails(Header* h, size_t totalsize,
                          size_t* psize, unsigned* ntoalign)
{
  KOption opt = getoption(h, psize);
  size_t align = *psize; /* usually, alignment follows size */
  if (opt == Kpaddalign)
  { /* 'X' gets alignment from following option */
    if (fmtbyte(h) == '\0' || getoption(h, &align) == Kchar || align == 0)
      luaL_argerror(h->L, 1, HostString(lit<"invalid next option for option 'X'">()->view()));
  }
  if (align <= 1 || opt == Kchar) /* need no alignment? */
    *ntoalign = 0;
  else
  {
    if (align > h->maxalign) /* enforce maximum alignment */
      align = h->maxalign;
    if (l_unlikely(!(((align) & ((align) - 1)) == 0)))
    {                /* not a power of 2? */
      *ntoalign = 0; /* to avoid warnings */
      luaL_argerror(h->L, 1, HostString(lit<"format asks for alignment not power of 2">()->view()));
    }
    else
    {
      /* 'szmoda' = totalsize % align */
      unsigned szmoda = ((unsigned int)(totalsize & (align - 1)));
      *ntoalign = ((unsigned int)((align - szmoda) & (align - 1)));
    }
  }
  return opt;
}

/*
** Pack integer 'n' with 'size' bytes and 'islittle' endianness.
** The final 'if' handles the case when 'size' is larger than
** the size of a Lua integer, correcting the extra sign-extension
** bytes if necessary (by default they would be zeros).
*/
static void packint(luaL_Buffer* b, lua_Unsigned n,
                    int islittle, unsigned size, int neg)
{
  size_t off = luaL_prepbuffsize(b, size);
  std::span<char> buff = luaL_buffarea(b);
  unsigned i;
  buff[off + (islittle ? 0 : size - 1)] = (char) (n & MC); /* first byte */
  for (i = 1; i < size; i++)
  {
    n >>= NB;
    buff[off + (islittle ? i : size - 1 - i)] = (char) (n & MC);
  }
  if (neg && size > SZINT)
  {                                /* negative number need sign extension? */
    for (i = SZINT; i < size; i++) /* correct extra bytes */
      buff[off + (islittle ? i : size - 1 - i)] = (char) MC;
  }
  luaL_addsize(b, size); /* add result to buffer */
}

/*
** Copy 'size' bytes from 'src' to 'dest', correcting endianness if
** given 'islittle' is different from native endianness.
*/
static void copywithendian(char* dest, const char* src,
                           unsigned size, int islittle)
{
  if (islittle == nativeendian_little)
    memcpy(dest, src, size);
  else
  {
    dest += size - 1;
    while (size-- != 0)
      *(dest--) = *(src++);
  }
}

static int str_pack(lua_State* L)
{
  luaL_Buffer b;
  Header h;
  const InternalString* fmt = luaL_checkistring(L, 1); /* format string */
  int arg = 1;          /* current argument to pack */
  size_t totalsize = 0; /* accumulate total size of result */
  initheader(L, &h, fmt);
  lua_pushnil(L); /* mark to separate arguments from string buffer */
  luaL_buffinit(L, &b);
  while (fmtbyte(&h) != '\0')
  {
    unsigned ntoalign;
    size_t size;
    KOption opt = getdetails(&h, totalsize, &size, &ntoalign);
    luaL_argcheck(L, size + ntoalign <= MAX_SIZE - totalsize, arg,
                  HostString(lit<"result too long">()->view()));
    totalsize += ntoalign + size;
    while (ntoalign-- > 0)
      luaL_addchar(&b, LUAL_PACKPADBYTE); /* fill alignment */
    arg++;
    switch (opt)
    {
    case Kint:
    { /* signed integers */
      lua_Integer n = luaL_checkinteger(L, arg);
      if (size < SZINT)
      { /* need overflow check? */
        lua_Integer lim = (lua_Integer) 1 << ((size * NB) - 1);
        luaL_argcheck(L, -lim <= n && n < lim, arg, HostString(lit<"integer overflow">()->view()));
      }
      packint(&b, (lua_Unsigned) n, h.islittle, ((unsigned int)(size)), (n < 0));
      break;
    }
    case Kuint:
    { /* unsigned integers */
      lua_Integer n = luaL_checkinteger(L, arg);
      if (size < SZINT) /* need overflow check? */
        luaL_argcheck(L, (lua_Unsigned) n < ((lua_Unsigned) 1 << (size * NB)),
                      arg, HostString(lit<"unsigned overflow">()->view()));
      packint(&b, (lua_Unsigned) n, h.islittle, ((unsigned int)(size)), 0);
      break;
    }
    case Kfloat:
    {                                             /* C float */
      float f = (float) luaL_checknumber(L, arg); /* get argument */
      size_t off = luaL_prepbuffsize(&b, sizeof(f));
      /* move 'f' to final result, correcting endianness if needed
         (raw-memory boundary: 'copywithendian' is category-6 byte moving) */
      copywithendian(luaL_buffarea(&b).data() + off, (char*) &f, sizeof(f), h.islittle);
      luaL_addsize(&b, size);
      break;
    }
    case Knumber:
    {                                          /* Lua float */
      lua_Number f = luaL_checknumber(L, arg); /* get argument */
      size_t off = luaL_prepbuffsize(&b, sizeof(f));
      /* move 'f' to final result, correcting endianness if needed
         (raw-memory boundary: 'copywithendian' is category-6 byte moving) */
      copywithendian(luaL_buffarea(&b).data() + off, (char*) &f, sizeof(f), h.islittle);
      luaL_addsize(&b, size);
      break;
    }
    case Kdouble:
    {                                               /* C double */
      double f = (double) luaL_checknumber(L, arg); /* get argument */
      size_t off = luaL_prepbuffsize(&b, sizeof(f));
      /* move 'f' to final result, correcting endianness if needed
         (raw-memory boundary: 'copywithendian' is category-6 byte moving) */
      copywithendian(luaL_buffarea(&b).data() + off, (char*) &f, sizeof(f), h.islittle);
      luaL_addsize(&b, size);
      break;
    }
    case Kchar:
    { /* fixed-size string */
      std::string_view s = luaL_checklstring(L, arg);
      size_t len = s.size();
      luaL_argcheck(L, len <= size, arg, HostString(lit<"string longer than given size">()->view()));
      luaL_addlstring(&b, s, 0, len); /* add string */
      if (len < size)
      {                            /* does it need padding? */
        size_t psize = size - len; /* pad size */
        size_t off = luaL_prepbuffsize(&b, psize);
        /* Java: Arrays.fill(buf, off, off + psize, PACKPADBYTE) */
        memset(luaL_buffarea(&b).data() + off, LUAL_PACKPADBYTE, psize);
        luaL_addsize(&b, psize);
      }
      break;
    }
    case Kstring:
    { /* strings with length count */
      std::string_view s = luaL_checklstring(L, arg);
      size_t len = s.size();
      luaL_argcheck(L, size >= sizeof(lua_Unsigned) || len < ((lua_Unsigned) 1 << (size * NB)),
                    arg, HostString(lit<"string length does not fit in given size">()->view()));
      /* pack length */
      packint(&b, (lua_Unsigned) len, h.islittle, ((unsigned int)(size)), 0);
      luaL_addlstring(&b, s, 0, len);
      totalsize += len;
      break;
    }
    case Kzstr:
    { /* zero-terminated string */
      std::string_view s = luaL_checklstring(L, arg);
      size_t len = s.size();
      luaL_argcheck(L, s.find('\0') == std::string_view::npos, arg,
                    HostString(lit<"string contains zeros">()->view()));
      luaL_addlstring(&b, s, 0, len);
      luaL_addchar(&b, '\0'); /* add zero at the end */
      totalsize += len + 1;
      break;
    }
    case Kpadding:
      luaL_addchar(&b, LUAL_PACKPADBYTE); /* FALLTHROUGH */
    case Kpaddalign:
    case Knop:
      arg--; /* undo increment */
      break;
    }
  }
  luaL_pushresult(&b);
  return 1;
}

static int str_packsize(lua_State* L)
{
  Header h;
  const InternalString* fmt = luaL_checkistring(L, 1); /* format string */
  size_t totalsize = 0; /* accumulate total size of result */
  initheader(L, &h, fmt);
  while (fmtbyte(&h) != '\0')
  {
    unsigned ntoalign;
    size_t size;
    KOption opt = getdetails(&h, totalsize, &size, &ntoalign);
    luaL_argcheck(L, opt != Kstring && opt != Kzstr, 1,
                  HostString(lit<"variable-length format">()->view()));
    size += ntoalign; /* total space used by option */
    luaL_argcheck(L, totalsize <= MAX_SIZE - size,
                  1, HostString(lit<"format result too large">()->view()));
    totalsize += size;
  }
  lua_pushinteger(L, ((lua_Integer)(totalsize)));
  return 1;
}

/*
** Unpack an integer with 'size' bytes and 'islittle' endianness.
** If size is smaller than the size of a Lua integer and integer
** is signed, must do sign extension (propagating the sign to the
** higher bits); if size is larger than the size of a Lua integer,
** it must check the unread bytes to see whether they do not cause an
** overflow.
*/
static lua_Integer unpackint(lua_State* L, std::string_view data, size_t off,
                             int islittle, int size, int issigned)
{
  lua_Unsigned res = 0;
  int i;
  int limit = (size <= SZINT) ? size : SZINT;
  for (i = limit - 1; i >= 0; i--)
  {
    res <<= NB;
    res |= (lua_Unsigned) (unsigned char) data[off + ((size_t)(islittle ? i : size - 1 - i))];
  }
  if (size < SZINT)
  { /* real size smaller than lua_Integer? */
    if (issigned)
    { /* needs sign extension? */
      lua_Unsigned mask = (lua_Unsigned) 1 << (size * NB - 1);
      res = ((res ^ mask) - mask); /* do sign extension */
    }
  }
  else if (size > SZINT)
  { /* must check unread bytes */
    int mask = (!issigned || (lua_Integer) res >= 0) ? 0 : MC;
    for (i = limit; i < size; i++)
    {
      if (l_unlikely((unsigned char) data[off + ((size_t)(islittle ? i : size - 1 - i))] != mask))
      {
        lua_pushinteger(L, size);
        luaL_error(L, HostString(lit<"%d-byte integer does not fit into Lua Integer">()->view()));
      }
    }
  }
  return (lua_Integer) res;
}

static int str_unpack(lua_State* L)
{
  Header h;
  const InternalString* fmt = luaL_checkistring(L, 1);
  std::string_view data = luaL_checklstring(L, 2);
  size_t ld = data.size();
  size_t pos = posrelatI(luaL_optinteger(L, 3, 1), ld) - 1;
  int n = 0; /* number of results */
  luaL_argcheck(L, pos <= ld, 3, HostString(lit<"initial position out of string">()->view()));
  initheader(L, &h, fmt);
  while (fmtbyte(&h) != '\0')
  {
    unsigned ntoalign;
    size_t size;
    KOption opt = getdetails(&h, pos, &size, &ntoalign);
    luaL_argcheck(L, ntoalign + size <= ld - pos, 2,
                  HostString(lit<"data string too short">()->view()));
    pos += ntoalign; /* skip alignment */
    /* stack space for item + next position */
    luaL_checkstack(L, 2, HostString(lit<"too many results">()->view()));
    n++;
    switch (opt)
    {
    case Kint:
    case Kuint:
    {
      lua_Integer res = unpackint(L, data, pos, h.islittle,
                                  ((int)(size)), (opt == Kint));
      lua_pushinteger(L, res);
      break;
    }
    case Kfloat:
    {
      float f;
      /* raw-memory boundary: category-6 byte moving */
      copywithendian((char*) &f, data.data() + pos, sizeof(f), h.islittle);
      lua_pushnumber(L, (lua_Number) f);
      break;
    }
    case Knumber:
    {
      lua_Number f;
      /* raw-memory boundary: category-6 byte moving */
      copywithendian((char*) &f, data.data() + pos, sizeof(f), h.islittle);
      lua_pushnumber(L, f);
      break;
    }
    case Kdouble:
    {
      double f;
      /* raw-memory boundary: category-6 byte moving */
      copywithendian((char*) &f, data.data() + pos, sizeof(f), h.islittle);
      lua_pushnumber(L, (lua_Number) f);
      break;
    }
    case Kchar:
    {
      lua_pushlstring(L, data, pos, size);
      break;
    }
    case Kstring:
    {
      lua_Unsigned len = (lua_Unsigned) unpackint(L, data, pos,
                                                  h.islittle, ((int)(size)), 0);
      luaL_argcheck(L, len <= ld - pos - size, 2, HostString(lit<"data string too short">()->view()));
      lua_pushlstring(L, data, pos + size, ((size_t)(len)));
      pos += ((size_t)(len)); /* skip string */
      break;
    }
    case Kzstr:
    {
      size_t zend = data.find('\0', pos); /* bounded scan (no NUL reliance) */
      luaL_argcheck(L, zend != std::string_view::npos, 2,
                    HostString(lit<"unfinished string for format 'z'">()->view()));
      size_t len = zend - pos;
      lua_pushlstring(L, data, pos, len);
      pos += len + 1; /* skip string plus final '\0' */
      break;
    }
    case Kpaddalign:
    case Kpadding:
    case Knop:
      n--; /* undo increment */
      break;
    }
    pos += size;
  }
  lua_pushinteger(L, ((lua_Integer)(pos)) + 1); /* next position */
  return n + 1;
}

/* }====================================================== */

static const luaL_Reg strlib[] = {
    {lit<"byte">()->view(), str_byte},
    {lit<"char">()->view(), str_char},
    {lit<"dump">()->view(), str_dump},
    {lit<"find">()->view(), str_find},
    {lit<"format">()->view(), str_format},
    {lit<"gmatch">()->view(), gmatch},
    {lit<"gsub">()->view(), str_gsub},
    {lit<"len">()->view(), str_len},
    {lit<"lower">()->view(), str_lower},
    {lit<"match">()->view(), str_match},
    {lit<"rep">()->view(), str_rep},
    {lit<"reverse">()->view(), str_reverse},
    {lit<"sub">()->view(), str_sub},
    {lit<"upper">()->view(), str_upper},
    {lit<"pack">()->view(), str_pack},
    {lit<"packsize">()->view(), str_packsize},
    {lit<"unpack">()->view(), str_unpack},
    {{}, nullptr}};

static void createmetatable(lua_State* L)
{
  /* table to be metatable for strings */
  lua_createtable(L, 0, sizeof(stringmetamethods) / sizeof((stringmetamethods)[0]) - 1);
  luaL_setfuncs(L, stringmetamethods, 0);
  lua_pushliteral(L, HostString(lit<"">()->view()));         /* dummy string */
  lua_pushvalue(L, -2);           /* copy table */
  lua_setmetatable(L, -2);        /* set table as metatable for strings */
  lua_pop(L, 1);                  /* pop dummy string */
  lua_pushvalue(L, -2);           /* get string library */
  lua_setfield(L, -2, HostString(lit<"__index">()->view())); /* metatable.__index = string */
  lua_pop(L, 1);                  /* pop metatable */
}

/*
** Open string library
*/
LUAMOD_API int luaopen_string(lua_State* L)
{
  (luaL_checkversion(L), lua_createtable(L, 0, sizeof(strlib) / sizeof((strlib)[0]) - 1), luaL_setfuncs(L, strlib, 0));
  createmetatable(L);
  return 1;
}
