/*
** $Id: lstrlib.c $
** Standard library for string operations and pattern-matching
** See Copyright Notice in lua.h
*/



#include <array>
#include <bit>

#include <charconv>
#include <cmath>
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
#include "lstate.h"
#include "lgc.h"

#include "internal_string.h"
#include "lauxlib.h"
#include "llimits.h"
#include "locale_util.h"
#include "lualib.h"
#include "util_cast.h"
#include "literals.h"

namespace lua::lib
{

/*
** maximum number of captures that a pattern can do during
** pattern-matching. This limit is arbitrary, but must fit in
** an unsigned char.
*/
constexpr int LUA_MAXCAPTURES = 32;

static int str_len(lua_State* L)
{
  const InternalString* s = luaL_checkistring(L, 1);
  lua_pushinteger(L, s->size());
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
    return pos;
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
    return pos;
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
  byte_span p = luaL_buffaddr(&b);
  for (i = 0; i < l; i++)
    p[off + i] = (char)s->byteAt(l - i - 1);
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
  byte_span p = luaL_buffaddr(&b);
  for (i = 0; i < l; i++)
    p[off + i] = (char)locale_tolower(s->byteAt(i));
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
  byte_span p = luaL_buffaddr(&b);
  for (i = 0; i < l; i++)
    p[off + i] = (char)locale_toupper(s->byteAt(i));
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
  else if (l_unlikely(sv.size() > MAX_SIZE - sepsv.size() || (lua_Integer)(sv.size() + sepsv.size()) > ((lua_Integer)MAX_SIZE) / n))
  {
    return luaL_errori(L, lt::resulting_string_too_large);
  }
  else
  {
    size_t totallen = ((size_t)n * (sv.size() + sepsv.size())) - sepsv.size();
    luaL_Buffer b;
    size_t off = luaL_buffinitsize(L, &b, totallen);
    byte_span p = byte_span(luaL_buffaddr(&b));
    size_t pos = off;
    while (n-- > 1)
    { /* first n-1 copies (followed by separator) */
      p.copy_from(sv, pos);
      pos += sv.size();
      if (sepsv.size() > 0)
      { /* empty 'memcpy' is not that cheap */
        p.copy_from(sepsv, pos);
        pos += sepsv.size();
      }
    }
    p.copy_from(sv, pos); /* last copy without separator */
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
  if (l_unlikely(pose - posi >= INT_MAX)) /* arithmetic overflow? */
    return luaL_errori(L, lt::string_slice_too_long);
  n = (int) (pose - posi) + 1;
  luaL_checkstacki(L, n, lt::string_slice_too_long);
  for (i = 0; i < n; i++)
    lua_pushinteger(L, s->byteAt(posi + i - 1));
  return n;
}

static int str_char(lua_State* L)
{
  int n = lua_gettop(L); /* number of arguments */
  int i;
  luaL_Buffer b;
  size_t off = luaL_buffinitsize(L, &b, n);
  byte_span p = luaL_buffaddr(&b);
  for (i = 1; i <= n; i++)
  {
    lua_Unsigned c = luaL_checkinteger(L, i);
    luaL_argchecki(L, c <= UCHAR_MAX, i, lt::value_out_of_range);
    p[off + i - 1] = (char)c;
  }
  luaL_pushresultsize(&b, n);
  return 1;
}

/*
** Buffer to store the result of 'string.dump'. It must be initialized
** after the call to 'lua_dump', to ensure that the function is on the
** top of the stack when 'lua_dump' is called. ('luaL_buffinit' might
** push stuff.)
*/
struct str_Writer : lua_Writer
{
  int init; /* true iff buffer has been initialized */
  luaL_Buffer B;
  int write(lua_State* L, std::optional<byte_view> b) override;
};

static int writer(lua_State* L, std::optional<byte_view> b, str_Writer* state)
{
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

int str_Writer::write(lua_State* L, std::optional<byte_view> b)
{
  return writer(L, b, this);
}

static int str_dump(lua_State* L)
{
  struct str_Writer state;
  int strip = lua_toboolean(L, 2);
  luaL_argchecki(L, lua_type(L, 1) == LUA_TFUNCTION && !lua_iscfunction(L, 1), 1, lt::Lua_function_expected);
  /* ensure function is on the top of the stack and vacate slot 1 */
  lua_pushvalue(L, 1);
  state.init = 0;
  lua_dump(L, &state, strip);
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
/* (the other operand's metamethod returned) */
static int arithk(lua_State* L, int status, lua_KContext ctx)
{
  (void)L;
  (void)status;
  (void)ctx;
  return 1;
}

LUA_KNAME("string", arithk);

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
  luaE_freezeonly(L);
  lua_callk(L, 2, 1, 0, arithk); /* call metamethod */
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

static CFuncInfo cfi_arith_add(arith_add, "string.arith_add");
static CFuncInfo cfi_arith_sub(arith_sub, "string.arith_sub");
static CFuncInfo cfi_arith_mul(arith_mul, "string.arith_mul");
static CFuncInfo cfi_arith_mod(arith_mod, "string.arith_mod");
static CFuncInfo cfi_arith_pow(arith_pow, "string.arith_pow");
static CFuncInfo cfi_arith_div(arith_div, "string.arith_div");
static CFuncInfo cfi_arith_idiv(arith_idiv, "string.arith_idiv");
static CFuncInfo cfi_arith_unm(arith_unm, "string.arith_unm");

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

constexpr int CAP_UNFINISHED = -1;
constexpr int CAP_POSITION = -2;

constexpr ptrdiff_t NOMATCH = -1;

struct MatchBox;

struct MatchState
{
  const InternalString* src;
  const InternalString* p;
  ptrdiff_t src_end;
  ptrdiff_t p_end;
  lua_State* L;
  int matchdepth; /* control for recursive depth (to avoid C stack overflow) */
  int level;      /* total number of captures (finished or unfinished) */
  bool budgeted;  /* is there a budget (the calls of 'match' are its steps)? */
  bool goingon;   /* is the next call of 'match' one going on ('goon')? */
  struct
  {
    ptrdiff_t init;
    ptrdiff_t len; /* length or special value (CAP_*) */
  } capture[LUA_MAXCAPTURES];
  /* where a check point keeps the match ('match_checkpoint') */
  int boxidx;            /* the stack index of its match box */
  MatchBox* box;         /* the box, once made */
  MatchBox* rec;         /* the box a freeze records the frames in, or nullptr */
  int nrec;              /* the frames left to record */
  lua_KFunction k;       /* the caller's continuation */
  lua_KContext ctx;
  ptrdiff_t origin;      /* where the caller's match started */
  luaL_Buffer* b;        /* 'gsub': its buffer, kept in a box below the match box... */
  luaL_Box* bbox;        /* ...this one, once made */
  lua_Integer loop[3];   /* 'gsub': its last match, count and change flag */
};

/* recursive function */
static ptrdiff_t match(MatchState* ms, ptrdiff_t s, ptrdiff_t p);

static inline int srcbyte(const MatchState* ms, ptrdiff_t i)
{
  return (i < ms->src_end) ? ms->src->byteAt(i) : 0;
}

static inline int patbyte(const MatchState* ms, ptrdiff_t i)
{
  return (i < ms->p_end) ? ms->p->byteAt(i) : 0;
}

/* maximum recursion depth for 'match' */
constexpr int MAXCCALLS = 200;

/*
** A match goes through check points: each call of 'match' is a step of the
** budget, and where the budget is spent the callback runs and may freeze.
** The match then stops where it is, kept in a match box (a user object at
** 'boxidx', made at the first check point): the position the call of
** 'match' began with, the captures, and a frame for each call of 'match'
** waiting for the one it made (outermost first), recorded as the freeze
** leaves them. After the thaw, the caller's continuation makes those calls
** again from their frames ('match_resume') and the match goes on as if
** nothing had happened.
*/
enum : char
{
  MF_START = 1, /* 'start_capture': undoes its capture if the match fails */
  MF_END,       /* 'end_capture' of capture 'a': undoes its close */
  MF_OPT,       /* '?' at 'a' (subject), 'b' (its class end): goes on without it */
  MF_MAX,       /* 'max_expand' at 'a', 'b' with 'c' repetitions: tries fewer */
  MF_MIN        /* 'min_expand' at 'a', class 'b' to 'c': tries one more */
};
constexpr size_t MF_SIZE = 1 + 3 * sizeof(lua_Integer);   /* kind, a, b, c */
constexpr size_t MB_CAPSIZE = 2 * sizeof(lua_Integer); /* init, len */
constexpr size_t MB_BYTES = LUA_MAXCAPTURES * MB_CAPSIZE + MAXCCALLS * MF_SIZE;
enum
{
  MB_S,       /* the position the stopped call of 'match' began with... */
  MB_P,       /* ...in the subject and in the pattern */
  MB_LEVEL,   /* the captures (as many) */
  MB_NFRAMES, /* the calls waiting */
  MB_ORIGIN,  /* where the caller's match started */
  MB_NINTS
};
constexpr size_t MATCHBOX_SIZE = 6800; /* (its accounted size) */

struct MatchBox : lua_UserObject
{
  lua_Integer ints[MB_NINTS]{};
  std::array<char, MB_BYTES> data{}; /* the captures, then the frames */
  size_t used() const
  {
    return static_cast<size_t>(ints[MB_LEVEL]) * MB_CAPSIZE + static_cast<size_t>(ints[MB_NFRAMES]) * MF_SIZE;
  }
  const char* persistkind() const override { return "string.match"; }
  int persistsave(lua_Integer* out, byte_view* b) const override
  {
    for (int i = 0; i < MB_NINTS; i++)
      out[i] = ints[i];
    *b = byte_view(data.data(), used());
    return MB_NINTS;
  }
};
static_assert(sizeof(MatchBox) <= MATCHBOX_SIZE);

static void putint(char* d, lua_Integer v)
{
  memcpy(d, &v, sizeof(v));
}

static lua_Integer getint(const char* d)
{
  lua_Integer v;
  memcpy(&v, d, sizeof(v));
  return v;
}

/* a match box from its state, checked to be one (not yet against its subject) */
static std::unique_ptr<lua_UserObject> makematchbox(lua_State*, const lua_Integer* ints, int nints,
                                                    byte_view bytes)
{
  if (nints != MB_NINTS || ints[MB_LEVEL] < 0 || ints[MB_LEVEL] > LUA_MAXCAPTURES ||
      ints[MB_NFRAMES] < 0 || ints[MB_NFRAMES] >= MAXCCALLS)
    return nullptr;
  auto mb = std::make_unique<MatchBox>();
  for (int i = 0; i < MB_NINTS; i++)
    mb->ints[i] = ints[i];
  if (bytes.size() != mb->used())
    return nullptr;
  memcpy(mb->data.data(), bytes.data(), bytes.size());
  return mb;
}

struct MatchBoxKind
{
  MatchBoxKind() { lua_registerobjectkind("string.match", makematchbox); }
};
static MatchBoxKind matchboxkind;

/* records the frame of a call of 'match' a freeze leaves (see 'match_checkpoint') */
static void record(MatchState* ms, char kind, ptrdiff_t a, ptrdiff_t b, ptrdiff_t c)
{
  MatchBox* mb = ms->rec;
  if (mb == nullptr || ms->nrec == 0)
    return; /* (not a freeze of a check point) */
  ms->nrec--;
  char* d = mb->data.data() + static_cast<size_t>(mb->ints[MB_LEVEL]) * MB_CAPSIZE +
            static_cast<size_t>(ms->nrec) * MF_SIZE;
  d[0] = kind;
  putint(d + 1, a);
  putint(d + 1 + sizeof(lua_Integer), b);
  putint(d + 1 + 2 * sizeof(lua_Integer), c);
}

/*
** A check point at the start of a call of 'match' at 's', 'p': the budget is
** spent. The match box (and, for 'gsub', the box of its buffer below it)
** is made at its index the first time. The callback runs; if it freezes,
** the calls of 'match' record their frames as the freeze leaves them.
*/
static void match_checkpoint(MatchState* ms, ptrdiff_t s, ptrdiff_t p)
{
  lua_State* L = ms->L;
  if (ms->b != nullptr && ms->bbox == nullptr)
  { /* (the caller's arguments stay below the boxes) */
    lua_settop(L, ms->boxidx - 2);
    ms->bbox = luaL_newbox(L);
  }
  if (ms->box == nullptr)
  {
    lua_settop(L, ms->boxidx - 1);
    auto o = std::make_unique<MatchBox>();
    MatchBox* nb = o.get();
    lua_newuserobject(L, std::move(o), MATCHBOX_SIZE, 0);
    ms->box = nb;
  }
  MatchBox* mb = ms->box;
  luaL_Box* bb = (ms->b != nullptr) ? ms->bbox : nullptr;
  int nframes = MAXCCALLS - 1 - ms->matchdepth;
  mb->ints[MB_S] = s;
  mb->ints[MB_P] = p;
  mb->ints[MB_LEVEL] = ms->level;
  mb->ints[MB_NFRAMES] = nframes;
  mb->ints[MB_ORIGIN] = ms->origin;
  for (int i = 0; i < ms->level; i++)
  {
    putint(mb->data.data() + i * MB_CAPSIZE, ms->capture[i].init);
    putint(mb->data.data() + i * MB_CAPSIZE + sizeof(lua_Integer), ms->capture[i].len);
  }
  if (bb != nullptr)
  {
    bb->ints[0] = ms->origin;
    for (int i = 0; i < 3; i++)
      bb->ints[i + 1] = ms->loop[i];
    luaL_buffsave(ms->b, bb);
  }
  ms->rec = mb;
  ms->nrec = nframes;
  lua_checkpointk(L, ms->ctx, ms->k);
  ms->rec = nullptr; /* (no freeze) */
  if (bb != nullptr)
    luaL_buffload(ms->b, bb);
}

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

/* keeps trying to match with 'i' repetitions, then fewer */
static l_forceinline ptrdiff_t max_expand_from(MatchState* ms, ptrdiff_t s, ptrdiff_t ep, ptrdiff_t i)
{
  while (i >= 0)
  {
    ptrdiff_t res;
    try
    {
      res = match(ms, s + i, ep + 1);
    }
    catch (...)
    {
      record(ms, MF_MAX, s, ep, i);
      throw;
    }
    if (res != NOMATCH)
      return res;
    i--; /* else didn't match; reduce 1 repetition to try again */
  }
  return NOMATCH;
}

static ptrdiff_t max_expand(MatchState* ms, ptrdiff_t s,
                            ptrdiff_t p, ptrdiff_t ep)
{
  ptrdiff_t i = 0; /* counts maximum expand for item */
  while (singlematch(ms, s + i, p, ep))
    i++;
  /* keeps trying to match with the maximum repetitions */
  return max_expand_from(ms, s, ep, i);
}

static l_forceinline ptrdiff_t min_expand(MatchState* ms, ptrdiff_t s,
                            ptrdiff_t p, ptrdiff_t ep)
{
  for (;;)
  {
    ptrdiff_t res;
    try
    {
      res = match(ms, s, ep + 1);
    }
    catch (...)
    {
      record(ms, MF_MIN, s, p, ep);
      throw;
    }
    if (res != NOMATCH)
      return res;
    else if (singlematch(ms, s, p, ep))
      s++; /* try with one more repetition */
    else
      return NOMATCH;
  }
}

static l_forceinline ptrdiff_t start_capture(MatchState* ms, ptrdiff_t s,
                               ptrdiff_t p, int what)
{
  ptrdiff_t res;
  int level = ms->level;
  if (level >= LUA_MAXCAPTURES)
    luaL_errori(ms->L, lt::too_many_captures);
  ms->capture[level].init = s;
  ms->capture[level].len = what;
  ms->level = level + 1;
  try
  {
    res = match(ms, s, p);
  }
  catch (...)
  {
    record(ms, MF_START, 0, 0, 0);
    throw;
  }
  if (res == NOMATCH) /* match failed? */
    ms->level--;      /* undo capture */
  return res;
}

static l_forceinline ptrdiff_t end_capture(MatchState* ms, ptrdiff_t s,
                             ptrdiff_t p)
{
  int l = capture_to_close(ms);
  ptrdiff_t res;
  ms->capture[l].len = s - ms->capture[l].init; /* close capture */
  try
  {
    res = match(ms, s, p);
  }
  catch (...)
  {
    record(ms, MF_END, l, 0, 0);
    throw;
  }
  if (res == NOMATCH)                    /* match failed? */
    ms->capture[l].len = CAP_UNFINISHED; /* undo capture */
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
  if (l_unlikely(ms->budgeted))
  {
    if (ms->goingon)
      ms->goingon = false; /* (a call going on after a freeze: see 'goon') */
    else if (luaE_spent(ms->L, 1))
      match_checkpoint(ms, s, p);
  }
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
          try
          {
            res = match(ms, s + 1, ep + 1);
          }
          catch (...)
          {
            record(ms, MF_OPT, s, ep, 0);
            throw;
          }
          if (res != NOMATCH)
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
** A call of 'match' goes on from 's', 'p' after its start (its check point
** behind it): 'match' runs again, as that same call (at its depth, with no
** step taken).
*/
static ptrdiff_t goon(MatchState* ms, ptrdiff_t s, ptrdiff_t p)
{
  ms->matchdepth++; /* ('match' takes it again) */
  ms->goingon = true;
  s = match(ms, s, p);
  ms->matchdepth--;
  return s;
}

/*
** A match stopped at a check point goes on (see 'match_checkpoint'): the
** call of 'match' of frame 'k' gets the result of the one it made (frame
** 'k + 1', or the stopped one, which goes on from where it began) and goes
** on as it would have. Returns the result of that call.
*/
static ptrdiff_t resume_call(MatchState* ms, const char* frames, int k, int n,
                             ptrdiff_t s, ptrdiff_t p)
{
  if (k == n)
  { /* the stopped call: its check point is behind it */
    s = goon(ms, s, p);
    ms->matchdepth++;
    return s;
  }
  const char* d = frames + static_cast<size_t>(k) * MF_SIZE;
  char kind = d[0];
  ptrdiff_t a = (ptrdiff_t)getint(d + 1);
  ptrdiff_t b = (ptrdiff_t)getint(d + 1 + sizeof(lua_Integer));
  ptrdiff_t c = (ptrdiff_t)getint(d + 1 + 2 * sizeof(lua_Integer));
  ptrdiff_t res;
  try
  {
    res = resume_call(ms, frames, k + 1, n, s, p);
  }
  catch (...)
  {
    record(ms, kind, a, b, c);
    throw;
  }
  switch (kind)
  {
  case MF_START:
    if (res == NOMATCH)
      ms->level--; /* undo capture */
    break;
  case MF_END:
    if (res == NOMATCH)
      ms->capture[a].len = CAP_UNFINISHED; /* undo capture */
    break;
  case MF_OPT:
    if (res == NOMATCH)
      res = goon(ms, a, b + 1); /* without the optional item */
    break;
  case MF_MAX:
    if (res == NOMATCH)
      res = max_expand_from(ms, a, b, c - 1);
    break;
  default: /* MF_MIN */
    if (res == NOMATCH)
      res = singlematch(ms, a, b, c) ? min_expand(ms, a + 1, b, c) : NOMATCH;
    break;
  }
  ms->matchdepth++;
  return res;
}

/*
** Checks the stopped match in box 'mb' against the subject and pattern of
** 'ms' (a box made from outside the process is not trusted to fit them).
*/
static bool match_fits(const MatchState* ms, const MatchBox* mb)
{
  const lua_Integer* v = mb->ints;
  int level = static_cast<int>(v[MB_LEVEL]);
  int n = static_cast<int>(v[MB_NFRAMES]);
  if (v[MB_S] < 0 || v[MB_S] > ms->src_end || v[MB_P] < 0 || v[MB_P] > ms->p_end ||
      v[MB_ORIGIN] < 0 || v[MB_ORIGIN] > ms->src_end)
    return false;
  for (int i = 0; i < level; i++)
  {
    lua_Integer init = getint(mb->data.data() + i * MB_CAPSIZE);
    lua_Integer len = getint(mb->data.data() + i * MB_CAPSIZE + sizeof(lua_Integer));
    if (init < 0 || init > ms->src_end ||
        (len != CAP_UNFINISHED && len != CAP_POSITION && (len < 0 || len > ms->src_end - init)))
      return false;
  }
  int starts = 0;
  const char* frames = mb->data.data() + static_cast<size_t>(level) * MB_CAPSIZE;
  for (int k = 0; k < n; k++)
  {
    const char* d = frames + static_cast<size_t>(k) * MF_SIZE;
    lua_Integer a = getint(d + 1);
    lua_Integer b = getint(d + 1 + sizeof(lua_Integer));
    lua_Integer c = getint(d + 1 + 2 * sizeof(lua_Integer));
    switch (d[0])
    {
    case MF_START:
      starts++;
      break;
    case MF_END:
      if (a < 0 || a >= level)
        return false;
      break;
    case MF_OPT:
      if (a < 0 || a >= ms->src_end || b < 0 || b >= ms->p_end)
        return false;
      break;
    case MF_MAX:
      if (a < 0 || a > ms->src_end || b < 0 || b >= ms->p_end || c < 0 || c > ms->src_end - a)
        return false;
      break;
    case MF_MIN:
      if (a < 0 || a > ms->src_end || b < 0 || c <= b || c >= ms->p_end)
        return false;
      break;
    default:
      return false;
    }
  }
  return starts <= level;
}

/*
** The match in the box at 'ms->boxidx', stopped by a freeze, goes on:
** returns what the caller's call of 'match' returns.
*/
static ptrdiff_t match_resume(MatchState* ms)
{
  MatchBox* mb = dynamic_cast<MatchBox*>(lua_touserobject(ms->L, ms->boxidx));
  if (mb == nullptr || !match_fits(ms, mb))
    luaL_errori(ms->L, lt::invalid_match_state);
  ms->box = mb;
  ms->budgeted = true; /* (it was) */
  ms->goingon = false;
  ms->origin = (ptrdiff_t)mb->ints[MB_ORIGIN];
  ms->level = static_cast<int>(mb->ints[MB_LEVEL]);
  for (int i = 0; i < ms->level; i++)
  {
    ms->capture[i].init = (ptrdiff_t)getint(mb->data.data() + i * MB_CAPSIZE);
    ms->capture[i].len = (ptrdiff_t)getint(mb->data.data() + i * MB_CAPSIZE + sizeof(lua_Integer));
  }
  int n = static_cast<int>(mb->ints[MB_NFRAMES]);
  ms->matchdepth = MAXCCALLS - 1 - n;
  /* (each call reads its frame before the next is made, so all are read
     before a check point can write the box again) */
  const char* frames = mb->data.data() + static_cast<size_t>(ms->level) * MB_CAPSIZE;
  return resume_call(ms, frames, 0, n, (ptrdiff_t)mb->ints[MB_S], (ptrdiff_t)mb->ints[MB_P]);
}

static int str_findk(lua_State* L, int status, lua_KContext ctx);

/*
** The plain search of 'lmemfind' from 'p' on, with 'l1' places left for
** the first character of 's2' ('l2' the length of the rest). Each place it
** compares at is a step of the budget, and where the budget is spent a
** check point of 'find' (the place kept in a box at 5, made there then).
*/
static size_t lmemfind_from(lua_State* L, byte_view s1, size_t p, size_t l1,
                            byte_view s2, size_t l2, luaL_Box* box)
{
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
    if (l_unlikely(luaE_spent(L, 1)))
    {
      if (box == nullptr)
      {
        lua_settop(L, 4); /* (the arguments stay below it) */
        box = luaL_newbox(L);
      }
      box->ints[0] = static_cast<lua_Integer>(p);
      box->ints[1] = static_cast<lua_Integer>(l1);
      lua_checkpointk(L, 2, str_findk);
    }
  }
  return byte_view::npos; /* not found */
}

static size_t lmemfind(lua_State* L, byte_view s1, size_t off1, size_t l1,
                       byte_view s2, size_t l2)
{
  if (l2 == 0)
    return off1; /* empty strings are everywhere */
  else if (l2 > l1)
    return byte_view::npos; /* avoids a negative 'l1' */
  else
  {
    l2--;
    l1 = l1 - l2; /* 's2' cannot be found after that */
    return lmemfind_from(L, s1, off1, l1, s2, l2, nullptr);
  }
}

struct Capture
{
  ptrdiff_t init;
  ptrdiff_t len;
};

static Capture get_onecapture(MatchState* ms, int i, ptrdiff_t s,
                                ptrdiff_t e)
{
  if (i >= ms->level)
  {
    if (l_unlikely(i != 0))
    {
      lua_pushinteger(ms->L, i + 1);
      luaL_errori(ms->L, lt::invalid_capture_index_d);
    }
    return {s, (e - s)};
  }
  else
  {
    ptrdiff_t capl = ms->capture[i].len;
    if (l_unlikely(capl == CAP_UNFINISHED))
      luaL_errori(ms->L, lt::unfinished_capture);
    else if (capl == CAP_POSITION)
      lua_pushinteger(ms->L, (lua_Integer)ms->capture[i].init + 1);
    return {ms->capture[i].init, capl};
  }
}

/*
** Push the i-th capture on the stack.
*/
static void push_onecapture(MatchState* ms, int i, ptrdiff_t s,
                            ptrdiff_t e)
{
  Capture cap = get_onecapture(ms, i, s, e);
  if (cap.len != CAP_POSITION)
    lua_pushlstring(ms->L, ms->src->view(), cap.init, cap.len);
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
  ms->src_end = s->size();
  ms->p = p;
  ms->p_end = p->size();
  ms->box = nullptr;
  ms->rec = nullptr;
  ms->b = nullptr;
  ms->bbox = nullptr;
}

/* where the check points of a match keep it, and how its caller goes on */
static void prepcheck(MatchState* ms, int boxidx, lua_KFunction k, lua_KContext ctx)
{
  ms->boxidx = boxidx;
  ms->k = k;
  ms->ctx = ctx;
}

/*
** (Re)prepare state for a match, setting fields that change during
** each match.
*/
static void reprepstate(MatchState* ms)
{
  ms->matchdepth = MAXCCALLS;
  ms->level = 0;
  /* (no Lua code runs in a match, but at its check points: a match with
     no budget gets none) */
  ms->budgeted = (ms->L->hookmask & LUA_MASKBUDGET) != 0;
  ms->goingon = false;
}

static int find_loop(lua_State* L, MatchState* ms, int find, ptrdiff_t s1, ptrdiff_t res, bool first);

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
    size_t pos = lmemfind(L, s->view(), init, ls - init, p->view(), lp);
    if (pos != byte_view::npos)
    {
      lua_pushinteger(L, (lua_Integer)pos + 1);
      lua_pushinteger(L, pos + lp);
      return 2;
    }
  }
  else
  {
    MatchState ms;
    prepstate(&ms, L, s, p);
    prepcheck(&ms, 5, str_findk, find); /* (a match box at 5, after the arguments) */
    return find_loop(L, &ms, find, init, NOMATCH, false);
  }
  luaL_pushfail(L); /* not found */
  return 1;
}

/*
** The matches of 'find' and 'match' from 's1' on ('first' when the match
** from 's1' is already 'res').
*/
static int find_loop(lua_State* L, MatchState* ms, int find, ptrdiff_t s1, ptrdiff_t res, bool first)
{
  int anchor = (ms->p_end > 0 && ms->p->byteAt(0) == '^');
  ptrdiff_t pinit = anchor ? 1 : 0; /* skip anchor character */
  do
  {
    if (!first)
    {
      reprepstate(ms);
      ms->origin = s1;
      res = match(ms, s1, pinit);
    }
    first = false;
    if (res != NOMATCH)
    {
      if (find)
      {
        lua_pushinteger(L, (lua_Integer)s1 + 1); /* start */
        lua_pushinteger(L, res);                 /* end */
        return push_captures(ms, NOMATCH, 0) + 2;
      }
      else
        return push_captures(ms, s1, res);
    }
  } while (s1++ < ms->src_end && !anchor);
  luaL_pushfail(L); /* not found */
  return 1;
}

/*
** 'find' and 'match' go on after a freeze in a match (context: 'find'), or
** 'find' after a freeze in a plain search (context 2, its place in a box
** at 5)
*/
static int str_findk(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  if (ctx == 2)
  {
    const InternalString* s = luaL_checkistring(L, 1);
    const InternalString* p = luaL_checkistring(L, 2);
    luaL_Box* box = dynamic_cast<luaL_Box*>(lua_touserobject(L, 5));
    size_t ls = s->size();
    size_t l2 = p->size() - 1;
    if (box == nullptr || p->size() == 0 || box->ints[0] < 0 || box->ints[1] < 0 ||
        box->ints[0] + box->ints[1] != static_cast<lua_Integer>(ls - l2))
      luaL_errori(L, lt::invalid_match_state);
    size_t pos = lmemfind_from(L, s->view(), static_cast<size_t>(box->ints[0]),
                               static_cast<size_t>(box->ints[1]), p->view(), l2, box);
    if (pos == byte_view::npos)
    {
      luaL_pushfail(L); /* not found */
      return 1;
    }
    lua_pushinteger(L, (lua_Integer)pos + 1);
    lua_pushinteger(L, pos + p->size());
    return 2;
  }
  MatchState ms;
  prepstate(&ms, L, luaL_checkistring(L, 1), luaL_checkistring(L, 2));
  prepcheck(&ms, 5, str_findk, ctx);
  ptrdiff_t res = match_resume(&ms);
  return find_loop(L, &ms, (int)ctx, ms.origin, res, true);
}

LUA_KNAME("string", str_findk);

static int str_find(lua_State* L)
{
  return str_find_aux(L, 1);
}

static int str_match(lua_State* L)
{
  return str_find_aux(L, 0);
}

/* state for 'gmatch' */
constexpr size_t GMATCHSTATE_SIZE = 584;

struct GMatchState : lua_UserObject
{
  ptrdiff_t src;       /* current position */
  ptrdiff_t p;
  ptrdiff_t lastmatch;
  MatchState ms;       /* match state */
};

static int gmatchk(lua_State* L, int status, lua_KContext ctx);

/* the matches of a 'gmatch' iterator from 'src' on ('first': see 'find_loop') */
static int gmatch_loop(GMatchState* gm, ptrdiff_t src, ptrdiff_t e, bool first)
{
  for (; src <= gm->ms.src_end; src++)
  {
    if (!first)
    {
      reprepstate(&gm->ms);
      gm->ms.origin = src;
      e = match(&gm->ms, src, gm->p);
    }
    first = false;
    if (e != NOMATCH && e != gm->lastmatch)
    {
      gm->src = gm->lastmatch = e;
      return push_captures(&gm->ms, src, e);
    }
  }
  return 0; /* not found */
}

static int gmatch_aux(lua_State* L)
{
  GMatchState* gm = static_cast<GMatchState*>(lua_touserobject(L, lua_upvalueindex(3)));
  gm->ms.L = L;
  gm->ms.box = nullptr;
  gm->ms.rec = nullptr;
  prepcheck(&gm->ms, 1, gmatchk, 0); /* (a match box at 1: its arguments are not used) */
  return gmatch_loop(gm, gm->src, NOMATCH, false);
}

/* a 'gmatch' iterator goes on after a freeze in a match */
static int gmatchk(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  GMatchState* gm = static_cast<GMatchState*>(lua_touserobject(L, lua_upvalueindex(3)));
  gm->ms.L = L;
  gm->ms.rec = nullptr;
  prepcheck(&gm->ms, 1, gmatchk, 0);
  ptrdiff_t e = match_resume(&gm->ms);
  return gmatch_loop(gm, gm->ms.origin, e, true);
}

LUA_KNAME("string", gmatchk);

/* the state of a gmatch iterator, kept outside the process: where it is */
static int gmatch_save(const CClosure* cl, lua_Integer* ints)
{
  const Udata* u = uvalue(&cl->upvalue[2]);
  const GMatchState* gm = dynamic_cast<const GMatchState*>(u->obj.get());
  ints[0] = gm->src;
  ints[1] = gm->p;
  ints[2] = gm->lastmatch;
  return 3;
}

static bool gmatch_make(lua_State* L, CClosure* cl, const lua_Integer* ints, int nints)
{
  if (nints != 3 || !ttisstring(&cl->upvalue[0]) || !ttisstring(&cl->upvalue[1]))
    return false;
  const InternalString* s = &tsvalue(&cl->upvalue[0])->data;
  const InternalString* p = &tsvalue(&cl->upvalue[1])->data;
  if (ints[0] < 0 || ints[0] > static_cast<lua_Integer>(s->size()) + 1 || ints[1] < 0 ||
      ints[1] > static_cast<lua_Integer>(p->size()))
    return false;
  std::unique_ptr<GMatchState> o = std::make_unique<GMatchState>();
  GMatchState* gm = o.get();
  lua_newuserobject(L, std::move(o), GMATCHSTATE_SIZE, 0);
  prepstate(&gm->ms, L, s, p);
  gm->src = ints[0];
  gm->p = ints[1];
  gm->lastmatch = ints[2];
  TValue u = getslot(L, L->top - 1);
  copyobj(&cl->upvalue[2], &u);
  luaC_barrier(L, cl, &cl->upvalue[2]);
  lua_pop(L, 1);
  return true;
}

static CClosureNamer gmatch_kind(lua_CClosureKind{"string.gmatch_aux", gmatch_aux, 3, gmatch_save, gmatch_make});

static int gmatch(lua_State* L)
{
  const InternalString* s = luaL_checkistring(L, 1);
  const InternalString* p = luaL_checkistring(L, 2);
  size_t ls = s->size();
  size_t init = posrelatI(luaL_optinteger(L, 3, 1), ls) - 1;
  GMatchState* gm;
  lua_settop(L, 2); /* keep strings on closure to avoid being collected */
  std::unique_ptr<GMatchState> o = std::make_unique<GMatchState>();
  gm = o.get();
  lua_newuserobject(L, std::move(o), GMATCHSTATE_SIZE, 0);
  if (init > ls)   /* start after string's end? */
    init = ls + 1;
  prepstate(&gm->ms, L, s, p);
  gm->src = init;
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
      luaL_addchar(b, (char)sc.spec);
    else if (sc.spec == '0') /* '%0' */
      luaL_addlstring(b, ms->src->view(), s, e - s);
    else if ('1' <= sc.spec && sc.spec <= '9')
    { /* '%n' */
      Capture cap = get_onecapture(ms, sc.spec - '1', s, e);
      if (cap.len == CAP_POSITION)
        luaL_addvalue(b); /* add position to accumulated result */
      else
        luaL_addlstring(b, ms->src->view(), cap.init, cap.len);
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
** The arguments of 'gsub' and what follows from them (its continuation
** gets them again).
*/
struct GsubArgs
{
  const InternalString* srcs; /* subject */
  const InternalString* pat;  /* pattern */
  int tr;                     /* replacement type */
  lua_Integer max_s;          /* max replacements */
  int anchor;
  ptrdiff_t pinit; /* where the pattern starts (after an anchor) */
};

static GsubArgs gsubargs(lua_State* L)
{
  GsubArgs a;
  a.srcs = luaL_checkistring(L, 1);
  a.pat = luaL_checkistring(L, 2);
  a.tr = lua_type(L, 3);
  a.max_s = luaL_optinteger(L, 4, (lua_Integer)a.srcs->size() + 1);
  a.anchor = (a.pat->size() > 0 && a.pat->byteAt(0) == '^');
  a.pinit = a.anchor ? 1 : 0; /* skip anchor character */
  return a;
}

/* where a 'gsub' calling its replacement keeps its state (box at 5) */
enum
{
  GS_S,      /* the match: its start... */
  GS_E,      /* ...and its end */
  GS_N,      /* replacement count */
  GS_CHANGED /* change flag before the match */
};

static int gsubk(lua_State* L, int status, lua_KContext ctx);

/*
** The replacement function or table gave the value on the top for the
** match 's'..'e': add it to the string buffer 'b'. Return true if the
** original string was changed. (Function calls and table indexing
** resulting in nil or false do not change the subject.)
*/
static int add_result(lua_State* L, luaL_Buffer* b, const InternalString* src,
                      ptrdiff_t s, ptrdiff_t e)
{
  if (!lua_toboolean(L, -1))
  {                /* nil or false? */
    lua_pop(L, 1); /* remove value */
    luaL_addlstring(b, src->view(), s, e - s); /* keep original text */
    return 0;                                  /* no changes */
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

/*
** Add the replacement value to the string buffer 'b'. A replacement
** function or table is called with the state in 'box', for 'gsubk' to go
** on with after a freeze. Return true if the original string was changed.
*/
static int add_value(MatchState* ms, luaL_Buffer* b, luaL_Box* box, ptrdiff_t s,
                     ptrdiff_t e, int tr, lua_Integer n, int changed)
{
  lua_State* L = ms->L;
  switch (tr)
  {
  case LUA_TFUNCTION:
  { /* call the function */
    lua_pushvalue(L, 3);            /* push the function */
    int nargs = push_captures(ms, s, e); /* all captures as arguments */
    box->ints[GS_S] = s;
    box->ints[GS_E] = e;
    box->ints[GS_N] = n;
    box->ints[GS_CHANGED] = changed;
    luaL_buffsave(b, box);
    lua_callk(L, nargs, 1, 0, gsubk); /* call it */
    luaL_buffload(b, box);
    break;
  }
  case LUA_TTABLE:
  {                               /* index the table */
    push_onecapture(ms, 0, s, e); /* first capture is the index */
    if (box == nullptr)           /* (a table with no metatable: calls nothing) */
      lua_gettable(L, 3);
    else
    {
      box->ints[GS_S] = s;
      box->ints[GS_E] = e;
      box->ints[GS_N] = n;
      box->ints[GS_CHANGED] = changed;
      luaL_buffsave(b, box);
      lua_gettablek(L, 3, 0, gsubk);
      luaL_buffload(b, box);
    }
    break;
  }
  default:
  {                     /* LUA_TNUMBER or LUA_TSTRING */
    add_s(ms, b, s, e); /* add value to the buffer */
    return 1;           /* something changed */
  }
  }
  return add_result(L, b, ms->src, s, e);
}

static int gsub_end(lua_State* L, const GsubArgs& a, MatchState* ms, luaL_Buffer* b,
                    ptrdiff_t src, lua_Integer n, int changed)
{
  if (!changed)          /* no changes? */
    lua_pushvalue(L, 1); /* return original string */
  else
  { /* something changed */
    luaL_addlstring(b, a.srcs->view(), src, ms->src_end - src);
    luaL_pushresult(b); /* create and return new string */
  }
  lua_pushinteger(L, n); /* number of substitutions */
  return 2;
}

/* the matches of 'gsub' from 'src' on ('first': see 'find_loop') */
static int gsub_loop(lua_State* L, const GsubArgs& a, MatchState* ms, luaL_Buffer* b,
                     luaL_Box* box, ptrdiff_t src, ptrdiff_t lastmatch,
                     lua_Integer n, int changed, ptrdiff_t e, bool first)
{
  while (n < a.max_s)
  {
    if (!first)
    {
      reprepstate(ms); /* (re)prepare state for new match */
      ms->origin = src;
      ms->loop[0] = lastmatch;
      ms->loop[1] = n;
      ms->loop[2] = changed;
      e = match(ms, src, a.pinit);
    }
    first = false;
    if (e != NOMATCH && e != lastmatch)
    { /* match? */
      n++;
      changed = add_value(ms, b, box, src, e, a.tr, n, changed) || changed;
      src = lastmatch = e;
    }
    else if (src < ms->src_end) /* otherwise, skip one character */
      luaL_addchar(b, (char)a.srcs->byteAt(src++));
    else
      break; /* end of subject */
    if (a.anchor)
      break;
  }
  return gsub_end(L, a, ms, b, src, n, changed);
}

/*
** 'gsub' goes on after its replacement function or table (context 0), or
** after a freeze in a match (context 1, a match box at 6). The box at 5
** keeps its buffer and where it is (for a match: from where, its last
** match, count and change flag).
*/
static int gsubk(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  GsubArgs a = gsubargs(L);
  luaL_Box* box = luaL_tobox(L, 5);
  MatchState ms;
  prepstate(&ms, L, a.srcs, a.pat);
  prepcheck(&ms, 6, gsubk, 1);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_buffload(&b, box);
  ms.b = &b;
  ms.bbox = box;
  if (ctx == 1)
  { /* (a check point of the match going on keeps these again) */
    ptrdiff_t lastmatch = (ptrdiff_t)box->ints[GS_E];
    lua_Integer n = box->ints[GS_N];
    int changed = (box->ints[GS_CHANGED] != 0);
    ms.loop[0] = lastmatch;
    ms.loop[1] = n;
    ms.loop[2] = changed;
    ptrdiff_t e = match_resume(&ms);
    bool calls = (a.tr == LUA_TFUNCTION || (a.tr == LUA_TTABLE && !lua_isplaintable(L, 3)));
    return gsub_loop(L, a, &ms, &b, calls ? box : nullptr, ms.origin, lastmatch, n, changed, e, true);
  }
  ptrdiff_t s = (ptrdiff_t)box->ints[GS_S];
  ptrdiff_t e = (ptrdiff_t)box->ints[GS_E];
  if (s < 0 || s > e || e > ms.src_end)
    luaL_errori(L, lt::invalid_match_state);
  lua_Integer n = box->ints[GS_N];
  int changed = add_result(L, &b, a.srcs, s, e) || (int)box->ints[GS_CHANGED];
  if (lua_gettop(L) == 6)
    ms.box = dynamic_cast<MatchBox*>(lua_touserobject(L, 6)); /* (made by a check point before) */
  if (a.anchor)
    return gsub_end(L, a, &ms, &b, e, n, changed);
  return gsub_loop(L, a, &ms, &b, box, e, e, n, changed, NOMATCH, false);
}

LUA_KNAME("string", gsubk);

static int str_gsub(lua_State* L)
{
  GsubArgs a = gsubargs(L);
  int tr = a.tr;
  luaL_argexpectedi(L, tr == LUA_TNUMBER || tr == LUA_TSTRING || tr == LUA_TFUNCTION || tr == LUA_TTABLE, 3, lt::string_function_table);
  luaL_Box* box = nullptr;
  if (tr == LUA_TFUNCTION || (tr == LUA_TTABLE && !lua_isplaintable(L, 3)))
  { /* replacements call back: keep the state where a freeze finds it */
    lua_settop(L, 4);
    box = luaL_newbox(L);
    luaE_freezeonly(L);
  }
  MatchState ms;
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  prepstate(&ms, L, a.srcs, a.pat);
  prepcheck(&ms, 6, gsubk, 1); /* (boxes at 5 and 6, after the arguments) */
  ms.b = &b;
  ms.bbox = box;
  return gsub_loop(L, a, &ms, &b, box, 0, NOMATCH, 0, 0, NOMATCH, false);
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
constexpr int L_NBFD = (DBL_MANT_DIG - 1) % 4 + 1;

/*
** Add integer part of 'x' to buffer and return new 'x'
*/
static lua_Number adddigit(byte_span buff, size_t n, lua_Number x)
{
  lua_Number dd = floor(x); /* get integer part from 'x' */
  int d = (int) dd;
  buff[n] = (char)(d < 10 ? d + '0' : d - 10 + 'a'); /* add to buffer */
  return x - dd;                                        /* return what is left */
}

static size_t num2straux(byte_span area, size_t off, [[maybe_unused]] size_t sz, lua_Number x)
{
  /* if 'inf' or 'NaN', format it like '%g' */
  if (x != x)
  {
    byte_view r = std::signbit(x) ? lt::minus_nan : lt::nan;
    assert(sz > r.size());
    area.copy_from(r, off);
    return r.size();
  }
  else if (x == HUGE_VAL || x == -(lua_Number) HUGE_VAL)
  {
    byte_view r = (x < 0) ? lt::minus_inf : lt::inf;
    area.copy_from(r, off);
    return r.size();
  }
  else if (x == 0)
  {
    byte_view r = std::signbit(x) ? lt::neg_hex_zero : lt::hex_zero;
    area.copy_from(r,off);
    return r.size();
  }
  else
  {
    int e;
    lua_Number m = frexp(x, &e); /* 'x' fraction and exponent */
    size_t n = 0;                        /* character count */
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
      area[off + n++] = locale_radixpoint(); /* add radix point */
      do
      { /* add as many digits as needed */
        m = adddigit(area, off + n++, m * 16);
      } while (m > 0);
    }
    area[off + n++] = 'p';
    area[off + n++] = (e >= 0) ? '+' : '-';
    n += area.write_number(e >= 0 ? e : -e, 10, off + n);
    assert(n < sz);
    return n;
  }
}

static size_t lua_number2strx(lua_State* L, byte_span area, size_t off, size_t sz, byte_view fmt, lua_Number x)
{
  size_t n = num2straux(area, off, sz, x);
  if (fmt[1] == 'A')
  {
    for (size_t i = 0; i < n; i++)
      area[off + i] = (char)locale_toupper((unsigned char)area[off + i]);
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
constexpr int MAX_ITEMF = 110 + DBL_MAX_10_EXP;

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
      luaL_addchar(b, (char)c);
    }
    else if (locale_matchclass(c, 'c'))
    {
      byte_inline<4> dbuf;
      byte_view d = dbuf.view().substr(0, dbuf.write_number(c));
      luaL_addchar(b, '\\');
      if (locale_isdigit(sv_byteat(s->view(), i + 1)))
      {
        for (size_t j = d.size(); j < 3; j++)
          luaL_addchar(b, '0');
      }
      luaL_addlstring(b, d, 0, d.size());
    }
    else
      luaL_addchar(b, (char)c);
  }
  luaL_addchar(b, '"');
}

/*
** Serialize a floating-point number in such a way that it can be
** scanned back by Lua. Use hexadecimal format for "common" numbers
** (to preserve precision); inf, -inf, and NaN are handled separately.
** (NaN cannot be expressed as a numeral, so we write '(0/0)' for it.)
*/
static size_t quotefloat(lua_State* L, byte_span area, size_t off, lua_Number n)
{
  byte_view s;        /* for the fixed representations */
  if (n == HUGE_VAL) /* inf? */
    s = lt::huge_exp;
  else if (n == -(lua_Number) HUGE_VAL) /* -inf? */
    s = lt::neg_huge_exp;
  else if (n != n) /* NaN? */
    s = lt::n0_0;
  else
  { /* format number as hexadecimal */
    size_t nb = lua_number2strx(L, area, off, MAX_ITEM, lt::fmt_a, n);
    byte_view region = area.view().substr(off, nb);
    if (region.find('.') == byte_view::npos)
    {                                                 /* no dot? */
      char point = locale_radixpoint(); /* try locale point */
      size_t ppos = region.find(point);
      if (ppos != byte_view::npos)
      {
        area[off + ppos] = '.'; /* change it to a dot */
      }
    }
    return nb;
  }
  /* for the fixed representations */
  size_t l = s.size();
  assert(l < MAX_ITEM);
  area.copy_from(s, off);
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
    byte_span area = luaL_buffaddr(b);
    size_t nb;
    if (!lua_isinteger(L, arg)) /* float? */
      nb = quotefloat(L, area, off, lua_tonumber(L, arg));
    else
    { /* integers */
      lua_Integer n = lua_tointeger(L, arg);
      if (n == LUA_MININTEGER)
      {
        nb = area.write_number((lua_Unsigned)n, 16, off, true);
      }
      else
      {
        nb = area.write_number(n, 10, off);
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
  if (locale_isdigit(sv_byteat(s, p)))
  {
    p++;
    if (locale_isdigit(sv_byteat(s, p)))
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
  if (!(p + 1 == form.size() && locale_isalpha(sv_byteat(form, p))))
  { /* did not go to the end? */
    lua_pushlstring(L, form, 0, form.size());
    luaL_errori(L, lt::invalid_conversion_specification_s);
  }
}

static size_t getformat(lua_State* L, const InternalString* sf, size_t i,
                        byte_span form, size_t* formlen)
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
  form.copy_from(sv.substr(i, len), 1);
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
  while (locale_isdigit(sv_byteat(form, p)))
    spec->width = spec->width * 10 + (form[p++] - '0');
  if (sv_byteat(form, p) == '.')
  {
    p++;
    spec->precision = 0;
    while (locale_isdigit(sv_byteat(form, p)))
      spec->precision = spec->precision * 10 + (form[p++] - '0');
  }
  spec->conv = form[form.size() - 1];
}

static size_t packformat(byte_span area, size_t off, size_t maxitem,
                      const FormatSpec* spec,
                      byte_view prefix, byte_view body,
                      bool zeropadok)
{
  size_t prefixlen = prefix.size();
  size_t bodylen = body.size();
  size_t content = prefixlen + bodylen;
  size_t width = spec->width;
  size_t total = (content > width) ? content : width;
  size_t pad = total - content;
  size_t pos = off;
  assert(total < maxitem);
  (void) maxitem;
  if (spec->left)
  {
    area.copy_from(prefix, pos);
    pos += prefixlen;
    area.copy_from(body, pos);
    pos += bodylen;
    area.fill(std::byte{' '}, pos, pad);
  }
  else if (spec->zero && zeropadok)
  {
    area.copy_from(prefix, pos);
    pos += prefixlen;
    area.fill(std::byte{'0'}, pos, pad);
    pos += pad;
    area.copy_from(body, pos);
  }
  else
  {
    area.fill(std::byte{' '}, pos, pad);
    pos += pad;
    area.copy_from(prefix, pos);
    pos += prefixlen;
    area.copy_from(body, pos);
  }
  return total;
}

static size_t formatint(lua_State* L, int arg, byte_view form,
                     byte_span area, size_t off,
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
  lua_Unsigned u = (issigned && n < 0)
                       ? 0u - (lua_Unsigned)n
                       : n;
  byte_inline<24> digits;
  size_t ndigits = digits.write_number(u, base);
  if (spec.conv == 'X')
  {
    for (size_t i = 0; i < ndigits; i++)
      digits[i] = (char)locale_toupper((unsigned char)digits[i]);
  }
  byte_inline<MAX_ITEM> body;
  size_t blen = 0;
  if (u == 0 && spec.precision == 0)
    ;
  else
  {
    size_t mindigits = (spec.precision >= 0) ? spec.precision : 1;
    while (blen + ndigits < mindigits)
      body[blen++] = '0';
    body.copy_from(digits.view().substr(0, ndigits), blen);
    blen += ndigits;
  }
  if (spec.alt && spec.conv == 'o' && (blen == 0 || body[0] != '0'))
  {
    body.move_from(body.view().substr(0, blen), 1);
    body[0] = '0';
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
                    byte_view(prefix, std::size(prefix)).substr(0, prefixlen),
                    body.view().substr(0, blen), spec.precision < 0);
}

static size_t formatfloat(lua_State* L, int arg, byte_view form, byte_span area, size_t off, size_t maxitem)
{
  lua_Number n = luaL_checknumber(L, arg);
  checkformat(L, form, lt::fmtflags_f, 1);
  FormatSpec spec;
  scanformat(form, &spec);
  char lower = (char)locale_tolower((unsigned char)spec.conv);
  int precision = (spec.precision >= 0) ? spec.precision : 6;
  char prefix[1];
  size_t prefixlen = 0;
  if (std::signbit(n))
    prefix[prefixlen++] = '-';
  else if (spec.plus)
    prefix[prefixlen++] = '+';
  else if (spec.space)
    prefix[prefixlen++] = ' ';
  byte_inline<MAX_ITEMF> body;
  size_t blen;
  bool zeropadok = true;
  if (std::isinf(n) || std::isnan(n))
  {
    byte_view r = std::isinf(n) ? lt::inf : lt::nan;
    blen = r.size();
    body.copy_from(r);
    zeropadok = false;
  }
  else
  {
    std::chars_format cf = (lower == 'f')   ? std::chars_format::fixed
                           : (lower == 'e') ? std::chars_format::scientific
                                            : std::chars_format::general;
    if (lower == 'g' && precision == 0)
      precision = 1;
    blen = body.write_number(std::fabs(n), cf, precision);
    if (spec.alt)
    {
      size_t epos = 0;
      bool haspoint = false;
      while (epos < blen && body[epos] != 'e')
      {
        if (body[epos] == '.')
          haspoint = true;
        epos++;
      }
      if (!haspoint)
      {
        body.move_from(body.view().substr(epos, blen - epos), epos + 1);
        body[epos++] = '.';
        blen++;
      }
      if (lower == 'g')
      {
        size_t sig = 0;
        bool seennonzero = false;
        for (size_t i = 0; i < epos; i++)
        {
          if (locale_isdigit((unsigned char)body[i]))
          {
            if (body[i] != '0')
              seennonzero = true;
            if (seennonzero)
              sig++;
          }
        }
        if (sig == 0)
          sig = 1;
        size_t zeros = ((size_t)precision > sig) ? precision - sig : 0;
        body.move_from(body.view().substr(epos, blen - epos), epos + zeros);
        body.fill(std::byte{'0'}, epos, zeros);
        blen += zeros;
      }
    }
  }
  if (spec.conv == 'E' || spec.conv == 'G')
  {
    for (size_t i = 0; i < blen; i++)
      body[i] = (char)locale_toupper((unsigned char)body[i]);
  }
  return packformat(area, off, maxitem, &spec,
                    byte_view(prefix, std::size(prefix)).substr(0, prefixlen),
                    body.view().substr(0, blen), zeropadok);
}

/* where a 'format' calling '__tostring' keeps its state (box after the
   arguments) */
enum
{
  FMT_ITEM, /* the item: where it starts (after its '%')... */
  FMT_ARG   /* ...and its argument */
};

static int formatk(lua_State* L, int status, lua_KContext ctx);

/*
** Whether the value at 'arg' has a '__tostring' to call for a format
** item: then the state goes into the box (made the first time) before
** the call, for 'formatk'.
*/
static luaL_Box* formatsave(lua_State* L, luaL_Buffer* b, luaL_Box* box, [[maybe_unused]] int top,
                            size_t item, int arg)
{
  if (luaL_getmetafieldi(L, arg, lt::mm_tostring) == LUA_TNIL)
    return nullptr;
  lua_pop(L, 1);
  if (box == nullptr)
  {
    assert(lua_gettop(L) == top);
    box = luaL_newbox(L);
    luaE_freezeonly(L);
  }
  box->ints[FMT_ITEM] = (lua_Integer)item;
  box->ints[FMT_ARG] = arg;
  luaL_buffsave(b, box);
  return box;
}

/* the string 's' of a '%s' item, on the top: add it as the format says */
static size_t addstringitem(lua_State* L, luaL_Buffer* b, byte_view formv, size_t formlen,
                            int arg, size_t boff, size_t maxitem, const InternalString* s)
{
  size_t nb = 0;
  byte_view sv = s->view();
  if (formlen == 2) /* no modifiers? */
    luaL_addvalue(b); /* keep entire string */
  else
  {
    luaL_argchecki(L, sv.find('\0') == byte_view::npos, arg, lt::string_contains_zeros);
    checkformat(L, formv, lt::minus_ch, 1);
    if (formv.find('.') == byte_view::npos && sv.size() >= 100)
    {
      /* no precision and string is too long to be formatted */
      luaL_addvalue(b); /* keep entire string */
    }
    else
    { /* format the string into 'buff' */
      FormatSpec spec;
      scanformat(formv, &spec);
      size_t blen = (spec.precision >= 0 && (size_t)spec.precision < sv.size())
                        ? spec.precision /* truncate */
                        : sv.size();
      nb = packformat(luaL_buffaddr(b), boff, maxitem, &spec, lt::empty_str, sv.substr(0, blen), false);
      lua_pop(L, 1);
    }
  }
  return nb;
}

/*
** Format from position 'i' of 'sf' on, the last argument used being 'arg'
** (of 'top'), into 'b'. With a box, it is at 'top + 1'.
*/
static int format_from(lua_State* L, const InternalString* sf, size_t i, int arg, int top,
                       luaL_Buffer& b, luaL_Box* box)
{
  size_t sfl = sf->size();
  while (i < sfl)
  {
    if (sf->byteAt(i) != L_ESC)
      luaL_addchar(&b, (char)sf->byteAt(i++));
    else if (sv_byteat(sf->view(), ++i) == L_ESC)
    {
      luaL_addchar(&b, (char)sf->byteAt(i)); /* %% */
      i++;
    }
    else
    {                                              /* format item */
      byte_inline<MAX_FORMAT> form;         /* to store the format ('%...') */
      size_t formlen;
      size_t maxitem = MAX_ITEM;                  /* maximum length for the result */
      size_t boff = luaL_prepbuffsize(&b, maxitem); /* to put result */
      size_t nb = 0;                                   /* number of bytes in result */
      size_t item = i;
      if (++arg > top)
        return luaL_argerrori(L, arg, lt::no_value);
      i = getformat(L, sf, i, form, &formlen);
      byte_view formv = form.view().substr(0, formlen);
      switch (sv_byteat(sf->view(), i++))
      {
      case 'c':
      {
        checkformat(L, formv, lt::minus_ch, 0);
        FormatSpec spec;
        scanformat(formv, &spec);
        char buff[1] {};
        buff[0] = (char)luaL_checkinteger(L, arg);
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
        byte_inline<MAX_ITEM> body;
        size_t blen;
        if (lua_islightcfunction(L, arg))
        {
          InternalString fa = lua_funcaddressi(L, arg);
          blen = fa.size();
          assert(blen < body.size());
          body.copy_from(fa.view());
        }
        else
        {
          const void* p = lua_topointer(L, arg);
          if (p == nullptr)
          {
            const byte_view nullrep = lt::null;
            blen = nullrep.size();
            body.copy_from(nullrep);
          }
          else
          {
            blen = util::pointertext(body, 0, p);
          }
        }
        byte_view bodybytes = body.view().substr(0, blen);
        nb = packformat(luaL_buffaddr(&b), boff, maxitem, &spec, lt::empty_str, bodybytes, false);
        break;
      }
      case 'q':
      {
        if (formlen != 2) /* modifiers? */
          return luaL_errori(L, lt::specifier_q_cannot_have_modifiers);
        int t = lua_type(L, arg);
        luaL_Box* saved;
        if ((t == LUA_TNIL || t == LUA_TBOOLEAN) &&
            (saved = formatsave(L, &b, box, top, item, arg)) != nullptr)
        { /* (a '__tostring' for nil or a boolean) */
          box = saved;
          luaL_toistringk(L, arg, 0, formatk);
          luaL_buffload(&b, box);
          luaL_tostringresult(L);
          luaL_addvalue(&b);
        }
        else
          addliteral(L, &b, arg);
        break;
      }
      case 's':
      {
        luaL_Box* saved = formatsave(L, &b, box, top, item, arg);
        const InternalString* str;
        if (saved != nullptr)
        {
          box = saved;
          luaL_toistringk(L, arg, 0, formatk);
          luaL_buffload(&b, box);
          str = luaL_tostringresult(L);
        }
        else
        {
          luaL_tostringdefault(L, arg);
          str = lua_toistring(L, -1);
        }
        nb = addstringitem(L, &b, formv, formlen, arg, boff, maxitem, str);
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

/* 'format' goes on after a '__tostring' */
static int formatk(lua_State* L, int status, lua_KContext ctx)
{
  (void)status;
  (void)ctx;
  int top = lua_gettop(L) - 2; /* (the arguments, the box, the string) */
  const InternalString* sf = lua_toistring(L, 1);
  luaL_Box* box = luaL_tobox(L, top + 1);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_buffload(&b, box);
  int arg = (int)box->ints[FMT_ARG];
  byte_inline<MAX_FORMAT> form;
  size_t formlen;
  size_t i = getformat(L, sf, (size_t)box->ints[FMT_ITEM], form, &formlen);
  if (sv_byteat(sf->view(), i++) == 'q')
  {
    luaL_tostringresult(L);
    luaL_addvalue(&b);
  }
  else
  { /* 's' */
    size_t maxitem = MAX_ITEM;
    size_t boff = luaL_prepbuffsize(&b, maxitem);
    size_t nb = addstringitem(L, &b, form.view().substr(0, formlen), formlen, arg, boff, maxitem,
                              luaL_tostringresult(L));
    assert(nb < maxitem);
    luaL_addsize(&b, nb);
  }
  return format_from(L, sf, i, arg, top, b, box);
}

LUA_KNAME("string", formatk);

static int str_format(lua_State* L)
{
  int top = lua_gettop(L);
  const InternalString* sf = luaL_checkistring(L, 1);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  return format_from(L, sf, 0, 1, top, b, nullptr);
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

constexpr size_t PACK_MAXALIGN = 16;

/* number of bits in a character */
constexpr int NB = CHAR_BIT;

/* mask for one character (NB 1's) */
constexpr int MC = (1 << NB) - 1;

/* size of a lua_Integer */
constexpr int SZINT = 8;

constexpr size_t SZ_CHAR = 1;
constexpr size_t SZ_SHORT = 2;
constexpr size_t SZ_INT = 4;
constexpr size_t SZ_LONG = 8;
constexpr size_t SZ_INTEGER = 8;
constexpr size_t SZ_SIZET = 8;
constexpr size_t SZ_FLOAT = 4;
constexpr size_t SZ_NUMBER = 8;
constexpr size_t SZ_DOUBLE = 8;
static_assert(SZINT == sizeof(lua_Integer) && SZ_INTEGER == sizeof(lua_Integer) &&
              SZ_CHAR == sizeof(char) && SZ_SHORT == sizeof(short) &&
              SZ_INT == sizeof(int) && SZ_LONG == sizeof(long) &&
              SZ_SIZET == sizeof(size_t) && SZ_FLOAT == sizeof(float) &&
              SZ_NUMBER == sizeof(lua_Number) && SZ_DOUBLE == sizeof(double));

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
      a = a * 10 + (fmtbyte(h) - '0');
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
    lua_pushinteger(h->L, sz);
    lua_pushinteger(h->L, MAXINTSIZE);
    return luaL_errori(h->L, lt::integral_size_d_out_of_limits_1_d);
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
struct PackOption
{
  KOption opt;
  size_t size;
};

static PackOption getoption(Header* h)
{
  int opt = fmtbyte(h);
  h->pos++;
  switch (opt)
  {
  case 'b':
    return {Kint, SZ_CHAR};
  case 'B':
    return {Kuint, SZ_CHAR};
  case 'h':
    return {Kint, SZ_SHORT};
  case 'H':
    return {Kuint, SZ_SHORT};
  case 'l':
    return {Kint, SZ_LONG};
  case 'L':
    return {Kuint, SZ_LONG};
  case 'j':
    return {Kint, SZ_INTEGER};
  case 'J':
    return {Kuint, SZ_INTEGER};
  case 'T':
    return {Kuint, SZ_SIZET};
  case 'f':
    return {Kfloat, SZ_FLOAT};
  case 'n':
    return {Knumber, SZ_NUMBER};
  case 'd':
    return {Kdouble, SZ_DOUBLE};
  case 'i':
    return {Kint, getnumlimit(h, SZ_INT)};
  case 'I':
    return {Kuint, getnumlimit(h, SZ_INT)};
  case 's':
    return {Kstring, getnumlimit(h, SZ_SIZET)};
  case 'c':
  {
    size_t size = getnum(h, -1);
    if (l_unlikely(size == (size_t)(-1)))
      luaL_errori(h->L, lt::missing_size_for_format_option_c);
    return {Kchar, size};
  }
  case 'z':
    return {Kzstr, 0};
  case 'x':
    return {Kpadding, 1};
  case 'X':
    return {Kpaddalign, 0};
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
    h->maxalign = getnumlimit(h, PACK_MAXALIGN);
    break;
  }
  default:
    lua_pushinteger(h->L, opt);
    luaL_errori(h->L, lt::invalid_format_option_c);
  }
  return {Knop, 0};
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
struct PackDetails
{
  KOption opt;
  size_t size;
  size_t ntoalign;
};

static PackDetails getdetails(Header* h, size_t totalsize)
{
  PackOption po = getoption(h);
  KOption opt = po.opt;
  size_t align = po.size; /* usually, alignment follows size */
  size_t ntoalign;
  if (opt == Kpaddalign)
  { /* 'X' gets alignment from following option */
    if (fmtbyte(h) <= 0)
      luaL_argerrori(h->L, 1, lt::invalid_next_option_for_option_X);
    PackOption next = getoption(h);
    align = next.size;
    if (next.opt == Kchar || align == 0)
      luaL_argerrori(h->L, 1, lt::invalid_next_option_for_option_X);
  }
  if (align <= 1 || opt == Kchar) /* need no alignment? */
    ntoalign = 0;
  else
  {
    if (align > h->maxalign) /* enforce maximum alignment */
      align = h->maxalign;
    if (l_unlikely(!ispow2(align)))
    {                /* not a power of 2? */
      ntoalign = 0; /* to avoid warnings */
      luaL_argerrori(h->L, 1, lt::format_asks_for_alignment_not_power_of_2);
    }
    else
    {
      /* 'szmoda' = totalsize % align */
      size_t szmoda = totalsize & (align - 1);
      ntoalign = (align - szmoda) & (align - 1);
    }
  }
  return {opt, po.size, ntoalign};
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
  byte_span buff = luaL_buffaddr(b);
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

static void packFloat(luaL_Buffer* b, float f, int islittle)
{
  size_t pos = luaL_prepbuffsize(b, SZ_FLOAT);
  byte_span buff = luaL_buffaddr(b);
  luaL_addsize(b, SZ_FLOAT);
  buff.write_float(f, pos);
  if (islittle != nativeendian_little)
  {
    buff.reverse(pos, SZ_FLOAT);
  }
}

static void packDouble(luaL_Buffer* b, double f, bool islittle)
{
  size_t pos = luaL_prepbuffsize(b, SZ_DOUBLE);
  byte_span buff = luaL_buffaddr(b);
  luaL_addsize(b, SZ_DOUBLE);
  buff.write_double(f, pos);
  if (islittle != nativeendian_little)
  {
    buff.reverse(pos, SZ_DOUBLE);
  }
}

static double unpackDouble(byte_view data, size_t pos, bool islittle)
{
  data = data.substr(pos, SZ_DOUBLE);
  return (islittle == nativeendian_little ? data : byte_owner(data).reverse().view()).toDouble();
}

static float unpackFloat(byte_view data, size_t pos, bool islittle)
{
  data = data.substr(pos, SZ_FLOAT);
  return (islittle == nativeendian_little ? data : byte_owner(data).reverse().view()).toFloat();
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
    PackDetails d = getdetails(&h, totalsize);
    KOption opt = d.opt;
    size_t size = d.size;
    size_t ntoalign = d.ntoalign;
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
      packint(&b, n, h.islittle, size, n < 0);
      break;
    }
    case Kuint:
    { /* unsigned integers */
      lua_Integer n = luaL_checkinteger(L, arg);
      if (size < SZINT) /* need overflow check? */
        luaL_argchecki(L, l_ult(n, lua_Integer{1} << (size * NB)), arg, lt::unsigned_overflow);
      packint(&b, n, h.islittle, size, 0);
      break;
    }
    case Kfloat:
    {                                             /* C float */
      float f = (float) luaL_checknumber(L, arg); /* get argument */
      packFloat(&b, f, h.islittle);
      break;
    }
    case Knumber:
    {                                          /* Lua float */
      lua_Number f = luaL_checknumber(L, arg); /* get argument */
      packDouble(&b, f, h.islittle);
      break;
    }
    case Kdouble:
    {                                               /* C double */
      double f = luaL_checknumber(L, arg); /* get argument */
      packDouble(&b, f, h.islittle);
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
        size_t pos = luaL_prepbuffsize(&b, psize);
        byte_span(luaL_buffaddr(&b)).fill(std::byte{LUAL_PACKPADBYTE}, pos, psize);
        luaL_addsize(&b, psize);
      }
      break;
    }
    case Kstring:
    { /* strings with length count */
      byte_view s = luaL_checklstring(L, arg);
      size_t len = s.size();
      luaL_argchecki(L, size >= SZ_INTEGER || len < ((lua_Unsigned) 1 << (size * NB)), arg, lt::string_length_does_not_fit_in_given_size);
      /* pack length */
      packint(&b, len, h.islittle, size, 0);
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
    PackDetails d = getdetails(&h, totalsize);
    KOption opt = d.opt;
    size_t size = d.size;
    size_t ntoalign = d.ntoalign;
    luaL_argchecki(L, opt != Kstring && opt != Kzstr, 1, lt::variable_length_format);
    size += ntoalign; /* total space used by option */
    luaL_argchecki(L, totalsize <= MAX_SIZE - size, 1, lt::format_result_too_large);
    totalsize += size;
  }
  lua_pushinteger(L, totalsize);
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
    res |= (lua_Unsigned) (unsigned char) data[off + (size_t)(islittle ? i : size - 1 - i)];
  }
  if (size < SZINT)
  { /* real size smaller than lua_Integer? */
    if (issigned)
    { /* needs sign extension? */
      lua_Unsigned mask = (lua_Unsigned) 1 << (size * NB - 1);
      res = (res ^ mask) - mask; /* do sign extension */
    }
  }
  else if (size > SZINT)
  { /* must check unread bytes */
    int mask = (!issigned || (lua_Integer) res >= 0) ? 0 : MC;
    for (i = limit; i < size; i++)
    {
      if (l_unlikely((unsigned char) data[off + (size_t)(islittle ? i : size - 1 - i)] != mask))
      {
        lua_pushinteger(L, size);
        luaL_errori(L, lt::d_byte_integer_does_not_fit_into_Lua_Integer);
      }
    }
  }
  return res;
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
    PackDetails d = getdetails(&h, pos);
    KOption opt = d.opt;
    size_t size = d.size;
    size_t ntoalign = d.ntoalign;
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
                                  (int)size, opt == Kint);
      lua_pushinteger(L, res);
      break;
    }
    case Kfloat:
    {
      float f = unpackFloat(data, pos, h.islittle);
      lua_pushnumber(L, f);
      break;
    }
    case Knumber:
    {
      lua_Number f = unpackDouble(data, pos, h.islittle);
      lua_pushnumber(L, f);
      break;
    }
    case Kdouble:
    {
      double f = unpackDouble(data, pos, h.islittle);
      lua_pushnumber(L, f);
      break;
    }
    case Kchar:
    {
      lua_pushlstring(L, data, pos, size);
      break;
    }
    case Kstring:
    {
      lua_Unsigned len = unpackint(L, data, pos,
                                                  h.islittle, (int)size, 0);
      luaL_argchecki(L, len <= ld - pos - size, 2, lt::data_string_too_short);
      lua_pushlstring(L, data, pos + size, len);
      pos += (size_t)len; /* skip string */
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
  lua_pushinteger(L, (lua_Integer)pos + 1); /* next position */
  return n + 1;
}

/* }====================================================== */

static CFuncInfo cfi_str_byte(str_byte, "string.str_byte");
static CFuncInfo cfi_str_char(str_char, "string.str_char");
static CFuncInfo cfi_str_dump(str_dump, "string.str_dump");
static CFuncInfo cfi_str_find(str_find, "string.str_find");
static CFuncInfo cfi_str_format(str_format, "string.str_format");
static CFuncInfo cfi_gmatch(gmatch, "string.gmatch");
static CFuncInfo cfi_str_gsub(str_gsub, "string.str_gsub");
static CFuncInfo cfi_str_len(str_len, "string.str_len");
static CFuncInfo cfi_str_lower(str_lower, "string.str_lower");
static CFuncInfo cfi_str_match(str_match, "string.str_match");
static CFuncInfo cfi_str_rep(str_rep, "string.str_rep");
static CFuncInfo cfi_str_reverse(str_reverse, "string.str_reverse");
static CFuncInfo cfi_str_sub(str_sub, "string.str_sub");
static CFuncInfo cfi_str_upper(str_upper, "string.str_upper");
static CFuncInfo cfi_str_pack(str_pack, "string.str_pack");
static CFuncInfo cfi_str_packsize(str_packsize, "string.str_packsize");
static CFuncInfo cfi_str_unpack(str_unpack, "string.str_unpack");

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
  lua_createtable(L, 0, std::size(stringmetamethods) - 1);
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
}

LUAMOD_API int luaopen_string(lua_State* L)
{
  luaL_newlibcfi(L, strlib);
  createmetatable(L);
  return 1;
}

