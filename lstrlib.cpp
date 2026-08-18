/*
** $Id: lstrlib.c $
** Standard library for string operations and pattern-matching
** See Copyright Notice in lua.h
*/



#include <bit>

#include <charconv>
#include <cmath>
#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lua_internal.h"
#include "lobject.h"

#include "internal_string.h"
#include "lauxlib.h"
#include "llimits.h"
#include "locale_util.h"
#include "lualib.h"
#include "util_cast.h"
#include "literals.h"

/*
** maximum number of captures that a pattern can do during
** pattern-matching. This limit is arbitrary, but must fit in
** an unsigned char.
*/
constexpr int LUA_MAXCAPTURES = 32;

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
    lua_pushistringref(L, lt::empty_str);
  return 1;
}

static int str_reverse(lua_State* L)
{
  size_t i;
  luaL_Buffer b;
  const InternalString* s = luaL_checkistring(L, 1);
  size_t l = s->size();
  size_t off = luaL_buffinitsize(L, &b, l);
  std::span<char> p = luaL_buffaddr(&b);
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
  std::span<char> p = luaL_buffaddr(&b);
  for (i = 0; i < l; i++)
    p[off + i] = ((char)(locale_tolower(s->byteAt(i))));
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
  std::span<char> p = luaL_buffaddr(&b);
  for (i = 0; i < l; i++)
    p[off + i] = ((char)(locale_toupper(s->byteAt(i))));
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
  byte_view sv = s->view();
  lua_Integer n = luaL_checkinteger(L, 2);
  const InternalString* sep = luaL_optistring(L, 3, &lt::empty_str_i);
  byte_view sepsv = sep->view();
  if (n <= 0 || (sv.size() | sepsv.size()) == 0)
  {
    lua_pushistringref(L, lt::empty_str); /* no repetitions or both strings empty */
  }
  else if (l_unlikely(sv.size() > MAX_SIZE - sepsv.size() || ((lua_Integer)(sv.size() + sepsv.size())) > ((lua_Integer)(MAX_SIZE)) / n))
  {
    return luaL_errori(L, lt::resulting_string_too_large);
  }
  else
  {
    size_t totallen = (((size_t)(n)) * (sv.size() + sepsv.size())) - sepsv.size();
    luaL_Buffer b;
    size_t off = luaL_buffinitsize(L, &b, totallen);
    byte_span p = byte_span(luaL_buffaddr(&b));
    size_t pos = off;
    while (n-- > 1)
    { /* first n-1 copies (followed by separator) */
      p.copy_from(pos, sv);
      pos += sv.size();
      if (sepsv.size() > 0)
      { /* empty 'memcpy' is not that cheap */
        p.copy_from(pos, sepsv);
        pos += sepsv.size();
      }
    }
    p.copy_from(pos, sv); /* last copy without separator */
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
    return luaL_errori(L, lt::string_slice_too_long);
  n = (int) (pose - posi) + 1;
  luaL_checkstacki(L, n, lt::string_slice_too_long);
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
  std::span<char> p = luaL_buffaddr(&b);
  for (i = 1; i <= n; i++)
  {
    lua_Unsigned c = (lua_Unsigned) luaL_checkinteger(L, i);
    luaL_argchecki(L, c <= (lua_Unsigned) UCHAR_MAX, i, lt::value_out_of_range);
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

static int writer(lua_State* L, std::optional<byte_view> b, void* ud)
{
  struct str_Writer* state = (struct str_Writer*) ud;
  if (!state->init)
  {
    state->init = 1;
    luaL_buffinit(L, &state->B);
  }
  if (!b.has_value())
  {
    luaL_pushresult(&state->B); /* push result */
    lua_replace(L, 1);          /* move it to reserved slot */
  }
  else
    luaL_addlstring(&state->B, *b, 0, b->size());
  return 0;
}

static int str_dump(lua_State* L)
{
  struct str_Writer state;
  int strip = lua_toboolean(L, 2);
  luaL_argchecki(L, lua_type(L, 1) == LUA_TFUNCTION && !lua_iscfunction(L, 1), 1, lt::Lua_function_expected);
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
static void trymt(lua_State* L, byte_view mtkey, byte_view opname)
{
  lua_settop(L, 2); /* back to the original arguments */
  if (l_unlikely(lua_type(L, 2) == LUA_TSTRING ||
                 !luaL_getmetafield(L, 2, HostString{mtkey})))
  {
    byte_view t1 = lua_typenamei(L, lua_type(L, -2));
    byte_view t2 = lua_typenamei(L, lua_type(L, -1));
    lua_pushlstring(L, opname, 0, opname.size());
    lua_pushlstring(L, t1, 0, t1.size());
    lua_pushlstring(L, t2, 0, t2.size());
    luaL_errori(L, lt::attempt_to_s_a_s_with_a_s);
  }
  lua_insert(L, -3); /* put metamethod before arguments */
  lua_call(L, 2, 1); /* call metamethod */
}

static int arith(lua_State* L, int op, byte_view mtname)
{
  if (tonum(L, 1) && tonum(L, 2))
    lua_arith(L, op); /* result will be on the top */
  else
  {
    byte_view opname = byte_view(mtname).substr(2);
    trymt(L, mtname, opname);
  }
  return 1;
}

static int arith_add(lua_State* L)
{
  return arith(L, LUA_OPADD, lt::mm_add);
}

static int arith_sub(lua_State* L)
{
  return arith(L, LUA_OPSUB, lt::mm_sub);
}

static int arith_mul(lua_State* L)
{
  return arith(L, LUA_OPMUL, lt::mm_mul);
}

static int arith_mod(lua_State* L)
{
  return arith(L, LUA_OPMOD, lt::mm_mod);
}

static int arith_pow(lua_State* L)
{
  return arith(L, LUA_OPPOW, lt::mm_pow);
}

static int arith_div(lua_State* L)
{
  return arith(L, LUA_OPDIV, lt::mm_div);
}

static int arith_idiv(lua_State* L)
{
  return arith(L, LUA_OPIDIV, lt::mm_idiv);
}

static int arith_unm(lua_State* L)
{
  return arith(L, LUA_OPUNM, lt::mm_unm);
}

static CFuncInfo cfi_arith_add("mm_add"_hs, arith_add);
static CFuncInfo cfi_arith_sub("mm_sub"_hs, arith_sub);
static CFuncInfo cfi_arith_mul("mm_mul"_hs, arith_mul);
static CFuncInfo cfi_arith_mod("mm_mod"_hs, arith_mod);
static CFuncInfo cfi_arith_pow("mm_pow"_hs, arith_pow);
static CFuncInfo cfi_arith_div("mm_div"_hs, arith_div);
static CFuncInfo cfi_arith_idiv("mm_idiv"_hs, arith_idiv);
static CFuncInfo cfi_arith_unm("mm_unm"_hs, arith_unm);

static const luaL_RegCFI stringmetamethods[] = {
  {lt::mm_add, &cfi_arith_add},
  {lt::mm_sub, &cfi_arith_sub},
  {lt::mm_mul, &cfi_arith_mul},
  {lt::mm_mod, &cfi_arith_mod},
  {lt::mm_pow, &cfi_arith_pow},
  {lt::mm_div, &cfi_arith_div},
  {lt::mm_idiv, &cfi_arith_idiv},
  {lt::mm_unm, &cfi_arith_unm},
  {lt::mm_index, nullptr},
}; /* placeholder */

/* }====================================================== */

/*
** {======================================================
** PATTERN MATCHING
** =======================================================
*/

constexpr int CAP_UNFINISHED = (-1);
constexpr int CAP_POSITION = (-2);

constexpr ptrdiff_t NOMATCH = -1;

struct MatchState
{
  const InternalString* src;
  const InternalString* p;
  ptrdiff_t src_end;
  ptrdiff_t p_end;
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

static inline int srcbyte(const MatchState* ms, ptrdiff_t i)
{
  return (i < ms->src_end) ? ms->src->byteAt(((size_t)(i))) : 0;
}

static inline int patbyte(const MatchState* ms, ptrdiff_t i)
{
  return (i < ms->p_end) ? ms->p->byteAt(((size_t)(i))) : 0;
}

/* maximum recursion depth for 'match' */
constexpr int MAXCCALLS = 200;

constexpr char L_ESC = '%';

static int check_capture(MatchState* ms, int l)
{
  l -= '1';
  if (l_unlikely(l < 0 || l >= ms->level ||
                 ms->capture[l].len == CAP_UNFINISHED))
  {
    lua_pushinteger(ms->L, l + 1);
    return luaL_errori(ms->L, lt::invalid_capture_index_d);
  }
  return l;
}

static int capture_to_close(MatchState* ms)
{
  int level = ms->level;
  for (level--; level >= 0; level--)
    if (ms->capture[level].len == CAP_UNFINISHED)
      return level;
  return luaL_errori(ms->L, lt::invalid_pattern_capture);
}

static ptrdiff_t classend(MatchState* ms, ptrdiff_t p)
{
  switch (patbyte(ms, p++))
  {
  case L_ESC:
  {
    if (l_unlikely(p == ms->p_end))
      luaL_errori(ms->L, lt::malformed_pattern_ends_with);
    return p + 1;
  }
  case '[':
  {
    if (patbyte(ms, p) == '^')
      p++;
    do
    { /* look for a ']' */
      if (l_unlikely(p == ms->p_end))
        luaL_errori(ms->L, lt::malformed_pattern_missing);
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
  int lower = locale_tolower(cl);
  if (lower == 'z')
    res = (c == 0); /* deprecated option */
  else
    res = locale_matchclass(c, lower);
  if (res == 0 && lower == cl)
    return (cl == c); /* not a known class and lowercase — literal match */
  return (locale_islower(cl) ? res : !res);
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
    luaL_errori(ms->L, lt::malformed_pattern_missing_arguments_to_b);
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
    luaL_errori(ms->L, lt::too_many_captures);
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
  l = check_capture(ms, l);
  ptrdiff_t len = ms->capture[l].len;
  byte_view msv = ms->src->view();
  if ((ms->src_end - s) >= len &&
      msv.substr(ms->capture[l].init, len).equals(msv.substr(s, len)))
    return s + len;
  else
    return NOMATCH;
}

static ptrdiff_t match(MatchState* ms, ptrdiff_t s, ptrdiff_t p)
{
  if (l_unlikely(ms->matchdepth-- == 0))
    luaL_errori(ms->L, lt::pattern_too_complex);
  for (;;)
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
    {
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
        luaL_errori(ms->L, lt::missing_after_f_in_pattern);
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

static size_t lmemfind(byte_view s1, size_t off1, size_t l1,
                       byte_view s2, size_t l2)
{
  if (l2 == 0)
    return off1; /* empty strings are everywhere */
  else if (l2 > l1)
    return byte_view::npos; /* avoids a negative 'l1' */
  else
  {
    size_t p = off1;
    l2--;
    l1 = l1 - l2;         /* 's2' cannot be found after that */
    while (l1 > 0)
    {
      size_t init = s1.find(s2[0], p);
      if (init == byte_view::npos || init >= p + l1)
        break;
      init++; /* 1st char is already checked */
      if (s1.substr(init, l2).equals(s2.substr(1, l2)))
        return init - 1;
      else
      {
        l1 -= init - p;
        p = init;
      }
    }
    return byte_view::npos; /* not found */
  }
}

static ptrdiff_t get_onecapture(MatchState* ms, int i, ptrdiff_t s,
                                ptrdiff_t e, ptrdiff_t* cap)
{
  if (i >= ms->level)
  {
    if (l_unlikely(i != 0))
    {
      lua_pushinteger(ms->L, i + 1);
      luaL_errori(ms->L, lt::invalid_capture_index_d);
    }
    *cap = s;
    return (e - s);
  }
  else
  {
    ptrdiff_t capl = ms->capture[i].len;
    *cap = ms->capture[i].init;
    if (l_unlikely(capl == CAP_UNFINISHED))
      luaL_errori(ms->L, lt::unfinished_capture);
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
  luaL_checkstacki(ms->L, nlevels, lt::too_many_captures);
  for (i = 0; i < nlevels; i++)
    push_onecapture(ms, i, s, e);
  return nlevels; /* number of strings pushed */
}

/* check whether pattern has no special characters */
static int nospecials(byte_view p)
{
  return p.find_first_of(lt::pattern_specials) == byte_view::npos;
}

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
    if (pos != byte_view::npos)
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
  ptrdiff_t p;
  ptrdiff_t lastmatch;
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
    init = ls + 1;
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
  byte_view nv = news->view();
  FmtScan sc{nv};
  while (sc.next())
  {
    luaL_addlstring(b, nv, sc.litStart, sc.litEnd - sc.litStart);
    if (sc.spec == L_ESC) /* '%%' */
      luaL_addchar(b, ((char)(sc.spec)));
    else if (sc.spec == '0') /* '%0' */
      luaL_addlstring(b, ms->src->view(), ((size_t)(s)), ((size_t)(e - s)));
    else if ('1' <= sc.spec && sc.spec <= '9')
    { /* '%n' */
      ptrdiff_t cap;
      ptrdiff_t resl = get_onecapture(ms, sc.spec - '1', s, e, &cap);
      if (resl == CAP_POSITION)
        luaL_addvalue(b); /* add position to accumulated result */
      else
        luaL_addlstring(b, ms->src->view(), ((size_t)(cap)), ((size_t)(resl)));
    }
    else
    {
      lua_pushinteger(L, L_ESC);
      luaL_errori(L, lt::invalid_use_of_c_in_replacement_string);
    }
  }
  luaL_addlstring(b, nv, sc.litStart, sc.litEnd - sc.litStart);
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
    lua_pushistringref(L, lua_typenamei(L, lua_type(L, -1)));
    return luaL_errori(L, lt::invalid_replacement_value_a_s);
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
  luaL_argexpectedi(L, tr == LUA_TNUMBER || tr == LUA_TSTRING || tr == LUA_TFUNCTION || tr == LUA_TTABLE, 3, lt::string_function_table);
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
static lua_Number adddigit(byte_span buff, size_t n, lua_Number x)
{
  lua_Number dd = floor(x); /* get integer part from 'x' */
  int d = (int) dd;
  buff[n] = ((char)(d < 10 ? d + '0' : d - 10 + 'a')); /* add to buffer */
  return x - dd;                                        /* return what is left */
}

static size_t num2straux(std::span<char> area, size_t off, size_t sz, lua_Number x)
{
  byte_span item = byte_span(area).subspan(off, sz);
  /* if 'inf' or 'NaN', format it like '%g' */
  if (x != x)
  {
    byte_view r = std::signbit(x) ? lt::minus_nan : lt::nan;
    assert(sz > r.size());
    item.copy_from(0, r);
    return r.size();
  }
  else if (x == (lua_Number) HUGE_VAL || x == -(lua_Number) HUGE_VAL)
  {
    byte_view r = (x < 0) ? lt::minus_inf : lt::inf;
    item.copy_from(0, r);
    return r.size();
  }
  else if (x == 0)
  {
    byte_view r = std::signbit(x) ? lt::neg_hex_zero : lt::hex_zero;
    item.copy_from(0, r);
    return r.size();
  }
  else
  {
    int e;
    lua_Number m = frexp(x, &e); /* 'x' fraction and exponent */
    size_t n = 0;                        /* character count */
    if (m < 0)
    {                        /* is number negative? */
      item[n++] = '-'; /* add sign */
      m = -m;                /* make it positive */
    }
    item[n++] = '0';
    item[n++] = 'x';                            /* add "0x" */
    m = adddigit(item, n++, m * (1 << L_NBFD)); /* add first digit */
    e -= L_NBFD;                                      /* this digit goes before the radix point */
    if (m > 0)
    {                                      /* more digits? */
      item[n++] = locale_radixpoint(); /* add radix point */
      do
      { /* add as many digits as needed */
        m = adddigit(item, n++, m * 16);
      } while (m > 0);
    }
    item[n++] = 'p';
    item[n++] = (e >= 0) ? '+' : '-';
    n += item.subspan(n).write_number(e >= 0 ? e : -e);
    assert(n < sz);
    return n;
  }
}

static size_t lua_number2strx(lua_State* L, std::span<char> area, size_t off, size_t sz, byte_view fmt, lua_Number x)
{
  size_t n = num2straux(area, off, sz, x);
  if (fmt[1] == 'A')
  {
    for (size_t i = 0; i < n; i++)
      area[off + i] = ((char)(locale_toupper(((unsigned char)(area[off + i])))));
  }
  else if (l_unlikely(fmt[1] != 'a'))
  {
    return luaL_errori(L, lt::modifiers_for_format_a_A_not_implemented);
  }
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
/* valid flags for o, x, and X conversions */
/* valid flags for d and i conversions */
/* valid flags for u conversions */
/* valid flags for c, p, and s conversions */

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
    else if (locale_matchclass(c, 'c'))
    {
      char dbuf[4];
      byte_span dbufp(dbuf, sizeof(dbuf));
      byte_view d = dbufp.view().substr(0, dbufp.write_number(c));
      luaL_addchar(b, '\\');
      if (isdigit(sv_byteat(s->view(), i + 1)))
      {
        for (size_t j = d.size(); j < 3; j++)
          luaL_addchar(b, '0');
      }
      luaL_addlstring(b, d, 0, d.size());
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
static size_t quotefloat(lua_State* L, std::span<char> area, size_t off, lua_Number n)
{
  byte_view s;        /* for the fixed representations */
  if (n == (lua_Number) HUGE_VAL) /* inf? */
    s = lt::huge_exp;
  else if (n == -(lua_Number) HUGE_VAL) /* -inf? */
    s = lt::neg_huge_exp;
  else if (n != n) /* NaN? */
    s = lt::n0_0;
  else
  { /* format number as hexadecimal */
    size_t nb = lua_number2strx(L, area, off, MAX_ITEM, lt::fmt_a, n);
    byte_span region = byte_span(area).subspan(off, nb);
    if (region.view().find('.') == byte_view::npos)
    {                                                 /* no dot? */
      char point = locale_radixpoint(); /* try locale point */
      size_t ppos = region.view().find(point);
      if (ppos != byte_view::npos)
        region[ppos] = '.'; /* change it to a dot */
    }
    return nb;
  }
  /* for the fixed representations */
  size_t l = s.size();
  assert(l < MAX_ITEM);
  byte_span(area).copy_from(off, s);
  area[off + l] = '\0';
  return l;
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
    std::span<char> area = luaL_buffaddr(b);
    size_t nb;
    if (!lua_isinteger(L, arg)) /* float? */
      nb = quotefloat(L, area, off, lua_tonumber(L, arg));
    else
    { /* integers */
      lua_Integer n = lua_tointeger(L, arg);
      byte_span item = byte_span(area).subspan(off, MAX_ITEM);
      if (n == LUA_MININTEGER)
      {
        item[0] = '0';
        item[1] = 'x';
        nb = 2 + item.subspan(2).write_number(((lua_Unsigned)(n)), 16);
      }
      else
      {
        nb = item.write_number(n);
      }
    }
    luaL_addsize(b, nb);
    break;
  }
  case LUA_TNIL:
  case LUA_TBOOLEAN:
  {
    luaL_toistring(L, arg);
    luaL_addvalue(b);
    break;
  }
  default:
  {
    luaL_argerrori(L, arg, lt::value_has_no_literal_form);
  }
  }
}

static size_t get2digits(byte_view s, size_t p)
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
static void checkformat(lua_State* L, byte_view form,
                        byte_view flags, int precision)
{
  size_t p = 1;                              /* skip '%' */
  while (p < form.size() && flags.find(form[p]) != byte_view::npos)
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
    luaL_errori(L, lt::invalid_conversion_specification_s);
  }
}

static size_t getformat(lua_State* L, const InternalString* sf, size_t i,
                        std::span<char> form, size_t* formlen)
{
  /* spans flags, width, and precision ('0' is included as a flag) */
  byte_view sv = sf->view();
  byte_view fset = lt::fmtflags_f_digits;
  size_t len = 0;
  while (i + len < sv.size() && fset.find(sv[i + len]) != byte_view::npos)
    len++;
  len++; /* adds following character (should be the specifier) */
  /* still needs space for '%', '\0', plus a length modifier */
  if (len >= MAX_FORMAT - 10)
    luaL_errori(L, lt::invalid_format_too_long);
  form[0] = '%';
  byte_span(form).copy_from(1, sv.substr(i, len));
  form[1 + len] = '\0';
  *formlen = 1 + len;
  return i + len - 1;
}

struct FormatSpec
{
  bool left;
  bool plus;
  bool space;
  bool alt;
  bool zero;
  int width;
  int precision;
  char conv;
};

static void scanformat(byte_view form, FormatSpec* spec)
{
  size_t p = 1; /* skip '%' */
  *spec = FormatSpec{};
  spec->precision = -1;
  for (;; p++)
  {
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
  while (isdigit(sv_byteat(form, p)))
    spec->width = spec->width * 10 + (form[p++] - '0');
  if (sv_byteat(form, p) == '.')
  {
    p++;
    spec->precision = 0;
    while (isdigit(sv_byteat(form, p)))
      spec->precision = spec->precision * 10 + (form[p++] - '0');
  }
  spec->conv = form[form.size() - 1];
}

static size_t packformat(std::span<char> area, size_t off, size_t maxitem,
                      const FormatSpec* spec,
                      byte_view prefix, byte_view body,
                      bool zeropadok)
{
  size_t prefixlen = prefix.size();
  size_t bodylen = body.size();
  size_t content = prefixlen + bodylen;
  size_t width = ((size_t)(spec->width));
  size_t total = (content > width) ? content : width;
  size_t pad = total - content;
  size_t pos = off;
  assert(total < maxitem);
  (void) maxitem;
  if (spec->left)
  {
    byte_span(area).copy_from(pos, prefix);
    pos += prefixlen;
    byte_span(area).copy_from(pos, body);
    pos += bodylen;
    byte_span(area).fill(pos, pad, std::byte{' '});
  }
  else if (spec->zero && zeropadok)
  {
    byte_span(area).copy_from(pos, prefix);
    pos += prefixlen;
    byte_span(area).fill(pos, pad, std::byte{'0'});
    pos += pad;
    byte_span(area).copy_from(pos, body);
  }
  else
  {
    byte_span(area).fill(pos, pad, std::byte{' '});
    pos += pad;
    byte_span(area).copy_from(pos, prefix);
    pos += prefixlen;
    byte_span(area).copy_from(pos, body);
  }
  return total;
}

static size_t formatint(lua_State* L, int arg, byte_view form,
                     std::span<char> area, size_t off,
                     size_t maxitem, byte_view flags)
{
  lua_Integer n = luaL_checkinteger(L, arg);
  checkformat(L, form, flags, 1);
  FormatSpec spec;
  scanformat(form, &spec);
  bool issigned = (spec.conv == 'd' || spec.conv == 'i');
  int base = (spec.conv == 'o') ? 8
             : (spec.conv == 'x' || spec.conv == 'X') ? 16
                                                      : 10;
  unsigned long long u = (issigned && n < 0)
                             ? 0ULL - ((unsigned long long)(n))
                             : ((unsigned long long)(n));
  char digits[24];
  byte_span digitsp(digits, sizeof(digits));
  size_t ndigits = digitsp.write_number(u, base);
  if (spec.conv == 'X')
  {
    for (size_t i = 0; i < ndigits; i++)
      digitsp[i] = ((char)(locale_toupper(((unsigned char)(digitsp[i])))));
  }
  char body[MAX_ITEM];
  byte_span bodyp(body, sizeof(body));
  size_t blen = 0;
  if (u == 0 && spec.precision == 0)
    ;
  else
  {
    size_t mindigits = (spec.precision >= 0) ? ((size_t)(spec.precision)) : 1;
    while (blen + ndigits < mindigits)
      bodyp[blen++] = '0';
    bodyp.copy_from(blen, digitsp.view().substr(0, ndigits));
    blen += ndigits;
  }
  if (spec.alt && spec.conv == 'o' && (blen == 0 || bodyp[0] != '0'))
  {
    bodyp.move_from(1, bodyp.view().substr(0, blen));
    bodyp[0] = '0';
    blen++;
  }
  char prefix[2];
  size_t prefixlen = 0;
  if (issigned)
  {
    if (n < 0)
      prefix[prefixlen++] = '-';
    else if (spec.plus)
      prefix[prefixlen++] = '+';
    else if (spec.space)
      prefix[prefixlen++] = ' ';
  }
  else if (spec.alt && (spec.conv == 'x' || spec.conv == 'X') && u != 0)
  {
    prefix[prefixlen++] = '0';
    prefix[prefixlen++] = spec.conv;
  }
  return packformat(area, off, maxitem, &spec,
                    byte_view(prefix, sizeof(prefix)).substr(0, prefixlen),
                    bodyp.view().substr(0, blen), spec.precision < 0);
}

static size_t formatfloat(lua_State* L, int arg, byte_view form, std::span<char> area, size_t off, size_t maxitem)
{
  lua_Number n = luaL_checknumber(L, arg);
  checkformat(L, form, lt::fmtflags_f, 1);
  FormatSpec spec;
  scanformat(form, &spec);
  char lower = ((char)(locale_tolower(((unsigned char)(spec.conv)))));
  int precision = (spec.precision >= 0) ? spec.precision : 6;
  char prefix[1];
  size_t prefixlen = 0;
  if (std::signbit(n))
    prefix[prefixlen++] = '-';
  else if (spec.plus)
    prefix[prefixlen++] = '+';
  else if (spec.space)
    prefix[prefixlen++] = ' ';
  char body[MAX_ITEMF];
  byte_span bodyp(body, sizeof(body));
  size_t blen;
  bool zeropadok = true;
  if (std::isinf(n) || std::isnan(n))
  {
    byte_view r = std::isinf(n) ? lt::inf : lt::nan;
    blen = r.size();
    bodyp.copy_from(0, r);
    zeropadok = false;
  }
  else
  {
    std::chars_format cf = (lower == 'f')   ? std::chars_format::fixed
                           : (lower == 'e') ? std::chars_format::scientific
                                            : std::chars_format::general;
    if (lower == 'g' && precision == 0)
      precision = 1;
    blen = bodyp.write_number(std::fabs(n), cf, precision);
    if (spec.alt)
    {
      size_t epos = 0;
      bool haspoint = false;
      while (epos < blen && bodyp[epos] != 'e')
      {
        if (bodyp[epos] == '.')
          haspoint = true;
        epos++;
      }
      if (!haspoint)
      {
        bodyp.move_from(epos + 1, bodyp.view().substr(epos, blen - epos));
        bodyp[epos++] = '.';
        blen++;
      }
      if (lower == 'g')
      {
        size_t sig = 0;
        bool seennonzero = false;
        for (size_t i = 0; i < epos; i++)
        {
          if (isdigit(((unsigned char)(bodyp[i]))))
          {
            if (bodyp[i] != '0')
              seennonzero = true;
            if (seennonzero)
              sig++;
          }
        }
        if (sig == 0)
          sig = 1;
        size_t zeros = (((size_t)(precision)) > sig) ? precision - sig : 0;
        bodyp.move_from(epos + zeros, bodyp.view().substr(epos, blen - epos));
        bodyp.fill(epos, zeros, std::byte{'0'});
        blen += zeros;
      }
    }
  }
  if (spec.conv == 'E' || spec.conv == 'G')
  {
    for (size_t i = 0; i < blen; i++)
      bodyp[i] = ((char)(locale_toupper(((unsigned char)(bodyp[i])))));
  }
  return packformat(area, off, maxitem, &spec,
                    byte_view(prefix, sizeof(prefix)).substr(0, prefixlen),
                    bodyp.view().substr(0, blen), zeropadok);
}

static int str_format(lua_State* L)
{
  int top = lua_gettop(L);
  int arg = 1;
  const InternalString* sf = luaL_checkistring(L, arg);
  size_t sfl = sf->size();
  size_t i = 0;
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  while (i < sfl)
  {
    if (sf->byteAt(i) != L_ESC)
      luaL_addchar(&b, ((char)(sf->byteAt(i++))));
    else if (sv_byteat(sf->view(), ++i) == L_ESC)
    {
      luaL_addchar(&b, ((char)(sf->byteAt(i)))); /* %% */
      i++;
    }
    else
    {                                              /* format item */
      char form[MAX_FORMAT];                        /* to store the format ('%...') */
      size_t formlen;
      size_t maxitem = MAX_ITEM;                  /* maximum length for the result */
      size_t boff = luaL_prepbuffsize(&b, maxitem); /* to put result */
      size_t nb = 0;                                   /* number of bytes in result */
      if (++arg > top)
        return luaL_argerrori(L, arg, lt::no_value);
      i = getformat(L, sf, i, form, &formlen);
      byte_view formv{form, formlen};
      switch (sv_byteat(sf->view(), i++))
      {
      case 'c':
      {
        checkformat(L, formv, lt::minus_ch, 0);
        FormatSpec spec;
        scanformat(formv, &spec);
        char buff[1] {};
        buff[0] = ((char)((unsigned char)(luaL_checkinteger(L, arg))));
        byte_view body(buff, 1);
        nb = packformat(luaL_buffaddr(&b), boff, maxitem, &spec, lt::empty_str, body, false);
        break;
      }
      case 'd':
      case 'i':
        nb = formatint(L, arg, formv, luaL_buffaddr(&b), boff, maxitem, lt::fmt_flags_all);
        break;
      case 'u':
        nb = formatint(L, arg, formv, luaL_buffaddr(&b), boff, maxitem, lt::fmt_flags_zero);
        break;
      case 'o':
      case 'x':
      case 'X':
        nb = formatint(L, arg, formv, luaL_buffaddr(&b), boff, maxitem, lt::fmt_flags_hash);
        break;
      case 'a':
      case 'A':
        checkformat(L, formv, lt::fmtflags_f, 1);
        nb = lua_number2strx(L, luaL_buffaddr(&b), boff, maxitem, formv, luaL_checknumber(L, arg));
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
        nb = formatfloat(L, arg, formv, luaL_buffaddr(&b), boff, maxitem);
        break;
      }
      case 'p':
      {
        checkformat(L, formv, lt::minus_ch, 0);
        FormatSpec spec;
        scanformat(formv, &spec);
        char body[MAX_ITEM];
        byte_span bodyp(body, sizeof(body));
        size_t blen;
        if (lua_islightcfunction(L, arg))
        {
          InternalString fa = lua_funcaddressi(L, arg);
          blen = fa.size();
          assert(blen < sizeof(body));
          bodyp.copy_from(0, fa.view());
        }
        else
        {
          const void* p = lua_topointer(L, arg);
          if (p == nullptr)
          {
            const byte_view nullrep = lt::null;
            blen = nullrep.size();
            bodyp.copy_from(0, nullrep);
          }
          else
          {
            bodyp[0] = '0';
            bodyp[1] = 'x';
            blen = 2 + bodyp.subspan(2).write_number(((uintptr_t)(p)), 16);
          }
        }
        byte_view bodybytes = bodyp.view().substr(0, blen);
        nb = packformat(luaL_buffaddr(&b), boff, maxitem, &spec, lt::empty_str, bodybytes, false);
        break;
      }
      case 'q':
      {
        if (formlen != 2) /* modifiers? */
          return luaL_errori(L, lt::specifier_q_cannot_have_modifiers);
        addliteral(L, &b, arg);
        break;
      }
      case 's':
      {
        const InternalString* s = luaL_toistring(L, arg);
        byte_view sv = s->view();
        if (formlen == 2) /* no modifiers? */
          luaL_addvalue(&b); /* keep entire string */
        else
        {
          luaL_argchecki(L, sv.find('\0') == byte_view::npos, arg, lt::string_contains_zeros);
          checkformat(L, formv, lt::minus_ch, 1);
          if (formv.find('.') == byte_view::npos && sv.size() >= 100)
          {
            /* no precision and string is too long to be formatted */
            luaL_addvalue(&b); /* keep entire string */
          }
          else
          { /* format the string into 'buff' */
            FormatSpec spec;
            scanformat(formv, &spec);
            size_t blen = (spec.precision >= 0 && ((size_t)(spec.precision)) < sv.size())
                              ? ((size_t)(spec.precision)) /* truncate */
                              : sv.size();
            nb = packformat(luaL_buffaddr(&b), boff, maxitem, &spec, lt::empty_str, sv.substr(0, blen), false);
            lua_pop(L, 1);
          }
        }
        break;
      }
      default:
      { /* also treat cases 'pnLlh' */
        lua_pushlstring(L, formv, 0, formv.size());
        return luaL_errori(L, lt::invalid_conversion_s_to_format);
      }
      }
      assert(nb < maxitem);
      luaL_addsize(&b, nb);
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
constexpr char LUAL_PACKPADBYTE = 0x00;

/* maximum size for the binary representation of an integer */
constexpr int MAXINTSIZE = 16;

/* number of bits in a character */
constexpr int NB = CHAR_BIT;

/* mask for one character (NB 1's) */
constexpr int MC = ((1 << NB) - 1);

/* size of a lua_Integer */
constexpr int SZINT = ((int) sizeof(lua_Integer));

constexpr bool nativeendian_little = (std::endian::native == std::endian::little);

struct Header
{
  lua_State* L;
  const InternalString* fmt; /* format string */
  size_t pos;
  int islittle;
  size_t maxalign;
};

static inline int fmtbyte(const Header* h)
{
  return sv_byteat(h->fmt->view(), h->pos);
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
static size_t getnumlimit(Header* h, size_t df)
{
  size_t sz = getnum(h, df);
  if (l_unlikely((sz - 1u) >= MAXINTSIZE))
  {
    lua_pushinteger(h->L, ((lua_Integer)(sz)));
    lua_pushinteger(h->L, MAXINTSIZE);
    return ((size_t)(luaL_errori(h->L, lt::integral_size_d_out_of_limits_1_d)));
  }
  return sz;
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
      luaL_errori(h->L, lt::missing_size_for_format_option_c);
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
    luaL_errori(h->L, lt::invalid_format_option_c);
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
                          size_t* psize, size_t* ntoalign)
{
  KOption opt = getoption(h, psize);
  size_t align = *psize; /* usually, alignment follows size */
  if (opt == Kpaddalign)
  { /* 'X' gets alignment from following option */
    if (fmtbyte(h) <= 0 || getoption(h, &align) == Kchar || align == 0)
      luaL_argerrori(h->L, 1, lt::invalid_next_option_for_option_X);
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
      luaL_argerrori(h->L, 1, lt::format_asks_for_alignment_not_power_of_2);
    }
    else
    {
      /* 'szmoda' = totalsize % align */
      size_t szmoda = totalsize & (align - 1);
      *ntoalign = (align - szmoda) & (align - 1);
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
                    int islittle, size_t size, int neg)
{
  size_t off = luaL_prepbuffsize(b, size);
  std::span<char> buff = luaL_buffaddr(b);
  size_t i;
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
static void copywithendian(byte_span dest, byte_view src, int islittle)
{
  size_t size = src.size();
  if (islittle == nativeendian_little)
    dest.copy_from(0, src);
  else
  {
    for (size_t i = 0; i < size; i++)
      dest[i] = src[size - 1 - i];
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
  while (fmtbyte(&h) > 0)
  {
    size_t ntoalign;
    size_t size;
    KOption opt = getdetails(&h, totalsize, &size, &ntoalign);
    luaL_argchecki(L, size + ntoalign <= MAX_SIZE - totalsize, arg, lt::result_too_long);
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
        luaL_argchecki(L, -lim <= n && n < lim, arg, lt::integer_overflow);
      }
      packint(&b, (lua_Unsigned) n, h.islittle, size, (n < 0));
      break;
    }
    case Kuint:
    { /* unsigned integers */
      lua_Integer n = luaL_checkinteger(L, arg);
      if (size < SZINT) /* need overflow check? */
        luaL_argchecki(L, (lua_Unsigned) n < ((lua_Unsigned) 1 << (size * NB)), arg, lt::unsigned_overflow);
      packint(&b, (lua_Unsigned) n, h.islittle, size, 0);
      break;
    }
    case Kfloat:
    {                                             /* C float */
      float f = (float) luaL_checknumber(L, arg); /* get argument */
      size_t off = luaL_prepbuffsize(&b, sizeof(f));
      copywithendian(byte_span(luaL_buffaddr(&b)).subspan(off, sizeof(f)), byte_view((const char*) &f, sizeof(f)), h.islittle);
      luaL_addsize(&b, size);
      break;
    }
    case Knumber:
    {                                          /* Lua float */
      lua_Number f = luaL_checknumber(L, arg); /* get argument */
      size_t off = luaL_prepbuffsize(&b, sizeof(f));
      copywithendian(byte_span(luaL_buffaddr(&b)).subspan(off, sizeof(f)), byte_view((const char*) &f, sizeof(f)), h.islittle);
      luaL_addsize(&b, size);
      break;
    }
    case Kdouble:
    {                                               /* C double */
      double f = (double) luaL_checknumber(L, arg); /* get argument */
      size_t off = luaL_prepbuffsize(&b, sizeof(f));
      copywithendian(byte_span(luaL_buffaddr(&b)).subspan(off, sizeof(f)), byte_view((const char*) &f, sizeof(f)), h.islittle);
      luaL_addsize(&b, size);
      break;
    }
    case Kchar:
    { /* fixed-size string */
      byte_view s = luaL_checklstring(L, arg);
      size_t len = s.size();
      luaL_argchecki(L, len <= size, arg, lt::string_longer_than_given_size);
      luaL_addlstring(&b, s, 0, len); /* add string */
      if (len < size)
      {                            /* does it need padding? */
        size_t psize = size - len; /* pad size */
        size_t off = luaL_prepbuffsize(&b, psize);
        byte_span(luaL_buffaddr(&b)).fill(off, psize, std::byte{LUAL_PACKPADBYTE});
        luaL_addsize(&b, psize);
      }
      break;
    }
    case Kstring:
    { /* strings with length count */
      byte_view s = luaL_checklstring(L, arg);
      size_t len = s.size();
      luaL_argchecki(L, size >= sizeof(lua_Unsigned) || len < ((lua_Unsigned) 1 << (size * NB)), arg, lt::string_length_does_not_fit_in_given_size);
      /* pack length */
      packint(&b, (lua_Unsigned) len, h.islittle, size, 0);
      luaL_addlstring(&b, s, 0, len);
      totalsize += len;
      break;
    }
    case Kzstr:
    { /* zero-terminated string */
      byte_view s = luaL_checklstring(L, arg);
      size_t len = s.size();
      luaL_argchecki(L, s.find('\0') == byte_view::npos, arg, lt::string_contains_zeros);
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
  while (fmtbyte(&h) > 0)
  {
    size_t ntoalign;
    size_t size;
    KOption opt = getdetails(&h, totalsize, &size, &ntoalign);
    luaL_argchecki(L, opt != Kstring && opt != Kzstr, 1, lt::variable_length_format);
    size += ntoalign; /* total space used by option */
    luaL_argchecki(L, totalsize <= MAX_SIZE - size, 1, lt::format_result_too_large);
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
static lua_Integer unpackint(lua_State* L, byte_view data, size_t off,
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
        luaL_errori(L, lt::d_byte_integer_does_not_fit_into_Lua_Integer);
      }
    }
  }
  return (lua_Integer) res;
}

static int str_unpack(lua_State* L)
{
  Header h;
  const InternalString* fmt = luaL_checkistring(L, 1);
  byte_view data = luaL_checklstring(L, 2);
  size_t ld = data.size();
  size_t pos = posrelatI(luaL_optinteger(L, 3, 1), ld) - 1;
  int n = 0; /* number of results */
  luaL_argchecki(L, pos <= ld, 3, lt::initial_position_out_of_string);
  initheader(L, &h, fmt);
  while (fmtbyte(&h) > 0)
  {
    size_t ntoalign;
    size_t size;
    KOption opt = getdetails(&h, pos, &size, &ntoalign);
    luaL_argchecki(L, ntoalign + size <= ld - pos, 2, lt::data_string_too_short);
    pos += ntoalign; /* skip alignment */
    /* stack space for item + next position */
    luaL_checkstacki(L, 2, lt::too_many_results);
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
      copywithendian(byte_span((char*) &f, sizeof(f)), data.substr(pos, sizeof(f)), h.islittle);
      lua_pushnumber(L, (lua_Number) f);
      break;
    }
    case Knumber:
    {
      lua_Number f;
      copywithendian(byte_span((char*) &f, sizeof(f)), data.substr(pos, sizeof(f)), h.islittle);
      lua_pushnumber(L, f);
      break;
    }
    case Kdouble:
    {
      double f;
      copywithendian(byte_span((char*) &f, sizeof(f)), data.substr(pos, sizeof(f)), h.islittle);
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
      luaL_argchecki(L, len <= ld - pos - size, 2, lt::data_string_too_short);
      lua_pushlstring(L, data, pos + size, ((size_t)(len)));
      pos += ((size_t)(len)); /* skip string */
      break;
    }
    case Kzstr:
    {
      size_t zend = data.find('\0', pos);
      luaL_argchecki(L, zend != byte_view::npos, 2, lt::unfinished_string_for_format_z);
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

static CFuncInfo cfi_str_byte("byte"_hs, str_byte);
static CFuncInfo cfi_str_char("char"_hs, str_char);
static CFuncInfo cfi_str_dump("dump"_hs, str_dump);
static CFuncInfo cfi_str_find("find"_hs, str_find);
static CFuncInfo cfi_str_format("format"_hs, str_format);
static CFuncInfo cfi_gmatch("gmatch"_hs, gmatch);
static CFuncInfo cfi_str_gsub("gsub"_hs, str_gsub);
static CFuncInfo cfi_str_len("len"_hs, str_len);
static CFuncInfo cfi_str_lower("lower"_hs, str_lower);
static CFuncInfo cfi_str_match("match"_hs, str_match);
static CFuncInfo cfi_str_rep("rep"_hs, str_rep);
static CFuncInfo cfi_str_reverse("reverse"_hs, str_reverse);
static CFuncInfo cfi_str_sub("sub"_hs, str_sub);
static CFuncInfo cfi_str_upper("upper"_hs, str_upper);
static CFuncInfo cfi_str_pack("pack"_hs, str_pack);
static CFuncInfo cfi_str_packsize("packsize"_hs, str_packsize);
static CFuncInfo cfi_str_unpack("unpack"_hs, str_unpack);

static const luaL_RegCFI strlib[] = {
  {lt::byte, &cfi_str_byte},
  {lt::char_kw, &cfi_str_char},
  {lt::dump, &cfi_str_dump},
  {lt::find, &cfi_str_find},
  {lt::format, &cfi_str_format},
  {lt::gmatch, &cfi_gmatch},
  {lt::gsub, &cfi_str_gsub},
  {lt::len, &cfi_str_len},
  {lt::lower, &cfi_str_lower},
  {lt::match, &cfi_str_match},
  {lt::rep, &cfi_str_rep},
  {lt::reverse, &cfi_str_reverse},
  {lt::sub, &cfi_str_sub},
  {lt::upper, &cfi_str_upper},
  {lt::pack, &cfi_str_pack},
  {lt::packsize, &cfi_str_packsize},
  {lt::unpack, &cfi_str_unpack},
};

static void createmetatable(lua_State* L)
{
  /* table to be metatable for strings */
  lua_createtable(L, 0, sizeof(stringmetamethods) / sizeof((stringmetamethods)[0]) - 1);
  luaL_setfuncscfi(L, stringmetamethods);
  lua_pushistringref(L, lt::empty_str);         /* dummy string */
  lua_pushvalue(L, -2);           /* copy table */
  lua_setmetatable(L, -2);        /* set table as metatable for strings */
  lua_pop(L, 1);                  /* pop dummy string */
  lua_pushvalue(L, -2);           /* get string library */
  lua_setfieldi(L, -2, lt::mm_index); /* metatable.__index = string */
  lua_pop(L, 1);                  /* pop metatable */
}

/*
** Open string library
*/
LUAMOD_API int luaopen_string(lua_State* L)
{
  luaL_newlibcfi(L, strlib);
  createmetatable(L);
  return 1;
}
