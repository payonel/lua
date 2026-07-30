/*
** $Id: lobject.c $
** Some generic functions over Lua objects
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

#include <charconv>
#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "lua.h"

#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lgc.h"
#include "lstate.h"
#include "util_cast.h"
#include "lstring.h"
#include "lvm.h"

/*
** Computes ceil(log2(x)), which is the smallest integer n such that
** x <= (1 << n).
*/
/*
** Debug check that a value being manipulated is alive: either it is
** not collectable, or the collectable object has the right tag and is
** not dead. 'L == nullptr' skips the deadness check (used where no
** lua_State is available). Called only from debug builds (the
** checkliveness inline wrapper compiles the call away in release).
*/
int luaO_checkliveness(lua_State* L, const TValue* obj)
{
  (void) L; /* unused when asserts are disabled */
  if (obj->tt_ & BIT_ISCOLLECTABLE)
  {
    assert((((obj)->tt_ & WITHVARIANT_MASK) == gcvalue(obj)->tt));
    assert(L == nullptr || !isdead(G(L), gcvalue(obj)));
  }
  return 1;
}

lu_byte luaO_ceillog2(unsigned int x)
{
  static const lu_byte log_2[256] = {/* log_2[i - 1] = ceil(log2(i)) */
                                     0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
                                     6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
                                     7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
                                     7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
                                     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
  int l = 0;
  x--;
  while (x >= 256)
  {
    l += 8;
    x >>= 8;
  }
  return ((lu_byte)(l + log_2[x]));
}

/*
** Encodes 'p'% as a floating-point byte, represented as (eeeexxxx).
** The exponent is represented using excess-7. Mimicking IEEE 754, the
** representation normalizes the number when possible, assuming an extra
** 1 before the mantissa (xxxx) and adding one to the exponent (eeee)
** to signal that. So, the real value is (1xxxx) * 2^(eeee - 7 - 1) if
** eeee != 0, and (xxxx) * 2^-7 otherwise (subnormal numbers).
*/
lu_byte luaO_codeparam(unsigned int p)
{
  if (p >= (((lu_mem)(0x1F)) << (0xF - 7 - 1)) * 100u) /* overflow? */
    return 0xFF;                                         /* return maximum value */
  else
  {
    p = (((l_uint32)(p)) * 128 + 99) / 100; /* round up the division */
    if (p < 0x10)
    { /* subnormal number? */
      /* exponent bits are already zero; nothing else to do */
      return ((lu_byte)(p));
    }
    else
    { /* p >= 0x10 implies ceil(log2(p + 1)) >= 5 */
      /* preserve 5 bits in 'p' */
      unsigned log = luaO_ceillog2(p + 1) - 5u;
      return ((lu_byte)(((p >> log) - 0x10) | ((log + 1) << 4)));
    }
  }
}

/*
** Computes 'p' times 'x', where 'p' is a floating-point byte. Roughly,
** we have to multiply 'x' by the mantissa and then shift accordingly to
** the exponent.  If the exponent is positive, both the multiplication
** and the shift increase 'x', so we have to care only about overflows.
** For negative exponents, however, multiplying before the shift keeps
** more significant bits, as long as the multiplication does not
** overflow, so we check which order is best.
*/
l_mem luaO_applyparam(lu_byte p, l_mem x)
{
  int m = p & 0xF;  /* mantissa */
  int e = (p >> 4); /* exponent */
  if (e > 0)
  {            /* normalized? */
    e--;       /* correct exponent */
    m += 0x10; /* correct mantissa; maximum value is 0x1F */
  }
  e -= 7; /* correct excess-7 */
  if (e >= 0)
  {
    if (x < (MAX_LMEM / 0x1F) >> e) /* no overflow? */
      return (x * m) << e;          /* order doesn't matter here */
    else                            /* real overflow */
      return MAX_LMEM;
  }
  else
  { /* negative exponent */
    e = -e;
    if (x < MAX_LMEM / 0x1F)             /* multiplication cannot overflow? */
      return (x * m) >> e;               /* multiplying first gives more precision */
    else if ((x >> e) < MAX_LMEM / 0x1F) /* cannot overflow after shift? */
      return (x >> e) * m;
    else /* real overflow */
      return MAX_LMEM;
  }
}

static lua_Integer intarith(lua_State* L, int op, lua_Integer v1,
                            lua_Integer v2)
{
  switch (op)
  {
  case LUA_OPADD:
    return ((lua_Integer)(((lua_Unsigned)(v1)) + ((lua_Unsigned)(v2))));
  case LUA_OPSUB:
    return ((lua_Integer)(((lua_Unsigned)(v1)) - ((lua_Unsigned)(v2))));
  case LUA_OPMUL:
    return ((lua_Integer)(((lua_Unsigned)(v1)) * ((lua_Unsigned)(v2))));
  case LUA_OPMOD:
    return luaV_mod(L, v1, v2);
  case LUA_OPIDIV:
    return luaV_idiv(L, v1, v2);
  case LUA_OPBAND:
    return ((lua_Integer)(((lua_Unsigned)(v1)) & ((lua_Unsigned)(v2))));
  case LUA_OPBOR:
    return ((lua_Integer)(((lua_Unsigned)(v1)) | ((lua_Unsigned)(v2))));
  case LUA_OPBXOR:
    return ((lua_Integer)(((lua_Unsigned)(v1)) ^ ((lua_Unsigned)(v2))));
  case LUA_OPSHL:
    return luaV_shiftl(v1, v2);
  case LUA_OPSHR:
    return luaV_shiftr(v1, v2);
  case LUA_OPUNM:
    return ((lua_Integer)(((lua_Unsigned)(0)) - ((lua_Unsigned)(v1))));
  case LUA_OPBNOT:
    return ((lua_Integer)(((lua_Unsigned)(~((lua_Unsigned)(0)))) ^ ((lua_Unsigned)(v1))));
  default:
    assert(0);
    return 0;
  }
}

static lua_Number numarith(lua_State* L, int op, lua_Number v1,
                           lua_Number v2)
{
  switch (op)
  {
  case LUA_OPADD:
    return ((void)(L), (v1) + (v2));
  case LUA_OPSUB:
    return ((void)(L), (v1) - (v2));
  case LUA_OPMUL:
    return ((void)(L), (v1) * (v2));
  case LUA_OPDIV:
    return ((void)(L), (v1) / (v2));
  case LUA_OPPOW:
    return ((void)(L), ((v2) == 2) ? (v1) * (v1) : pow(v1, v2));
  case LUA_OPIDIV:
    return ((void)(L), floor((v1) / (v2)));
  case LUA_OPUNM:
    return ((void)(L), -(v1));
  case LUA_OPMOD:
    return luaV_modf(L, v1, v2);
  default:
    assert(0);
    return 0;
  }
}

int luaO_rawarith(lua_State* L, int op, const TValue* p1, const TValue* p2,
                  TValue* res)
{
  switch (op)
  {
  case LUA_OPBAND:
  case LUA_OPBOR:
  case LUA_OPBXOR:
  case LUA_OPSHL:
  case LUA_OPSHR:
  case LUA_OPBNOT:
  { /* operate only on integers */
    lua_Integer i1;
    lua_Integer i2;
    if (tointegerns(p1, &i1) && tointegerns(p2, &i2))
    {
      setivalue(res, intarith(L, op, i1, i2));
      return 1;
    }
    else
      return 0; /* fail */
  }
  case LUA_OPDIV:
  case LUA_OPPOW:
  { /* operate only on floats */
    lua_Number n1;
    lua_Number n2;
    if (tonumberns(p1, &n1) && tonumberns(p2, &n2))
    {
      setfltvalue(res, numarith(L, op, n1, n2));
      return 1;
    }
    else
      return 0; /* fail */
  }
  default:
  { /* other operations */
    lua_Number n1;
    lua_Number n2;
    if (((p1)->tt_ == LUA_VNUMINT) && ((p2)->tt_ == LUA_VNUMINT))
    {
      setivalue(res, intarith(L, op, ivalue(p1), ivalue(p2)));
      return 1;
    }
    else if (tonumberns(p1, &n1) && tonumberns(p2, &n2))
    {
      setfltvalue(res, numarith(L, op, n1, n2));
      return 1;
    }
    else
      return 0; /* fail */
  }
  }
}

void luaO_arith(lua_State* L, int op, StkId p1slot, StkId p2slot,
                StkId res)
{
  TValue p1v = getslot(L, p1slot);
  TValue p2v = getslot(L, p2slot);
  const TValue* p1 = &p1v;
  const TValue* p2 = &p2v;
  TValue r;
  if (!luaO_rawarith(L, op, p1, p2, &r))
  {
    /* could not perform raw operation; try metamethod */
    luaT_trybinTM(L, p1, p2, res, ((TMS)((op - LUA_OPADD) + TM_ADD)), NOSLOT, NOSLOT);
  }
  else
    setslot(L, res, &r);
}

lu_byte luaO_hexavalue(int c)
{
  assert(lisxdigit(c));
  if (lisdigit(c))
    return ((lu_byte)(c - '0'));
  else
    return ((lu_byte)((ltolower(c) - 'a') + 10));
}

/* consume one optional sign at s[*off]; returns 1 if negative */
static int isneg(std::string_view s, size_t* off)
{
  if (sv_byteat(s, *off) == '-')
  {
    (*off)++;
    return 1;
  }
  else if (sv_byteat(s, *off) == '+')
    (*off)++;
  return 0;
}

/*
** {==================================================================
** Lua's implementation for 'lua_strx2number'
** ===================================================================
*/


/* maximum number of significant digits to read (to avoid overflows
   even with single floats) */
constexpr int MAXSIGDIG = 30;

/*
** convert a hexadecimal numeric string to a number, following
** C99 specification for 'strtod'
*/
static lua_Number lua_strx2number(std::string_view s, size_t off, size_t* endpos)
{
  int dot = '.'; /* radix is pinned to '.' (no locale dependence) */
  lua_Number r = 0.0; /* result (accumulator) */
  int sigdig = 0;               /* number of significant digits */
  int nosigdig = 0;             /* number of non-significant digits */
  int e = 0;                    /* exponent correction */
  int neg;                      /* 1 if number is negative */
  int hasdot = 0;               /* true after seen a dot */
  size_t p = off;               /* scan offset */
  *endpos = off;                /* nothing is valid yet */
  while (lisspace(sv_byteat(s, p)))
    p++;                                                    /* skip initial spaces */
  neg = isneg(s, &p);                                       /* check sign */
  if (!(sv_byteat(s, p) == '0' && (sv_byteat(s, p + 1) == 'x' || sv_byteat(s, p + 1) == 'X'))) /* check '0x' */
    return 0.0;                                   /* invalid format (no '0x') */
  for (p += 2;; p++)
  { /* skip '0x' and read numeral */
    if (sv_byteat(s, p) == dot)
    {
      if (hasdot)
        break; /* second dot? stop loop */
      else
        hasdot = 1;
    }
    else if (lisxdigit(sv_byteat(s, p)))
    {
      if (sigdig == 0 && s[p] == '0') /* non-significant digit (zero)? */
        nosigdig++;
      else if (++sigdig <= MAXSIGDIG) /* can read it without overflow? */
        r = (r * 16.0) + luaO_hexavalue(s[p]);
      else
        e++; /* too many digits; ignore, but still count for exponent */
      if (hasdot)
        e--; /* decimal digit? correct exponent */
    }
    else
      break; /* neither a dot nor a digit */
  }
  if (nosigdig + sigdig == 0) /* no digits? */
    return 0.0;     /* invalid format */
  *endpos = p;                /* valid up to here */
  e *= 4;                     /* each digit multiplies/divides value by 2^4 */
  if (sv_byteat(s, p) == 'p' || sv_byteat(s, p) == 'P')
  {                     /* exponent part? */
    int exp1 = 0;       /* exponent value */
    int neg1;           /* exponent sign */
    p++;                /* skip 'p' */
    neg1 = isneg(s, &p); /* sign */
    if (!lisdigit(sv_byteat(s, p)))
      return 0.0;          /* invalid; must have at least one digit */
    while (lisdigit(sv_byteat(s, p))) /* read exponent */
      exp1 = exp1 * 10 + s[p++] - '0';
    if (neg1)
      exp1 = -exp1;
    e += exp1;
    *endpos = p; /* valid up to here */
  }
  if (neg)
    r = -r;
  return ldexp(r, e);
}

/* }====================================================== */

/* maximum length of a numeral to be converted to a number */
#if !defined(L_MAXLENNUM)
#define L_MAXLENNUM 200
#endif

/*
** Convert a decimal numeral 's' to a lua_Number via 'std::from_chars'
** (locale-free, correctly rounded; Java: 'Double.parseDouble' on the
** trimmed token). The C 'strtod' behaviors that 'from_chars' lacks are
** explicit here: leading spaces and one optional sign are consumed by
** this wrapper, and an out-of-range numeral saturates to +-HUGE_VAL /
** +-0.0 instead of failing (the "1e9999" idiom; 'parseDouble'
** saturates the same way). Returns where parsing stopped, or 's'
** itself if nothing was recognized.
*/
static size_t str2dec(std::string_view s, size_t off, lua_Number* result)
{
  size_t p = off;
  while (lisspace(sv_byteat(s, p)))
    p++; /* skip leading spaces */
  int neg = 0;
  if (sv_byteat(s, p) == '+' || sv_byteat(s, p) == '-')
  { /* consume one optional sign; the sign is applied below */
    neg = (s[p] == '-');
    p++;
  }
  if (sv_byteat(s, p) == '+' || sv_byteat(s, p) == '-')
    return off; /* a second sign is not a numeral */
  lua_Number v;
  /* host-conversion seam (Java: correctly-rounded parse of the token) */
  std::from_chars_result res =
      std::from_chars(s.data() + p, s.data() + s.size(), v,
                      std::chars_format::general);
  size_t endp = ((size_t)(res.ptr - s.data()));
  if (res.ec == std::errc::result_out_of_range)
  { /* saturate like C 'strtod' and Java 'parseDouble': overflow goes
       to +-infinity, underflow to +-0. Classify by estimating the
       decimal magnitude: the explicit exponent, plus one per
       significant digit before the radix point, minus one per zero
       between the point and the first significant digit. Overflow
       (magnitude >= ~309) and underflow (<= ~-324) are hundreds of
       orders apart, so the sign of the estimate classifies exactly. */
    long e10 = 0;      /* explicit exponent */
    int frac = 0;      /* inside the fractional part? */
    int sig = 0;       /* seen a significant (nonzero) digit? */
    long intdigits = 0; /* significant digits before the point */
    long fraczeros = 0; /* zeros after the point before a significant digit */
    size_t q;
    for (q = p; q < endp; q++)
    {
      if (s[q] == '.')
        frac = 1;
      else if (s[q] == 'e' || s[q] == 'E')
      { /* read the explicit exponent, clamped: only the SIGN of the
           final magnitude matters and the clamp dwarfs the digit
           corrections (upstream used 'strtol', which saturates) */
        size_t x = q + 1;
        int eneg = 0;
        if (sv_byteat(s, x) == '+' || sv_byteat(s, x) == '-')
        {
          eneg = (s[x] == '-');
          x++;
        }
        long acc = 0;
        while (lisdigit(sv_byteat(s, x)))
        {
          if (acc < 1000000000L)
            acc = acc * 10 + (s[x] - '0');
          x++;
        }
        e10 = eneg ? -acc : acc;
        break;
      }
      else
      { /* a digit */
        if (!sig && s[q] != '0')
          sig = 1; /* first significant digit */
        if (sig && !frac)
          intdigits++;
        else if (!sig && frac)
          fraczeros++;
      }
    }
    long magnitude = e10 + ((intdigits > 0) ? intdigits : -fraczeros);
    v = (magnitude > 0) ? ((lua_Number)(HUGE_VAL)) : 0.0;
  }
  else if (res.ec != std::errc())
    return off; /* nothing recognized */
  *result = neg ? -v : v;
  return endp;
}

/*
** Convert the numeral at s[off..] to a Lua number (put in 'result').
** Return npos on fail, or the end offset (== s.size()) on success.
** ('mode' == 'x') means a hexadecimal numeral.
*/
static size_t l_str2dloc(std::string_view s, size_t off, lua_Number* result,
                         int mode)
{
  size_t endpos;
  if (mode == 'x')
  { /* hexadecimal numeral: Lua's own reader */
    *result = lua_strx2number(s, off, &endpos);
    if (endpos == off)
      return std::string_view::npos; /* nothing recognized? */
  }
  else
  {
    endpos = str2dec(s, off, result);
    if (endpos == off)
      return std::string_view::npos; /* nothing recognized? */
  }
  while (lisspace(sv_byteat(s, endpos)))
    endpos++; /* skip trailing spaces */
  /* OK iff no trailing chars (offset form of upstream's NUL check) */
  return (endpos == s.size()) ? endpos : std::string_view::npos;
}

/*
** Convert string 's' to a Lua number (put in 'result') handling the
** current locale.
** This function accepts both the current locale or a dot as the radix
** mark. If the conversion fails, it may mean number has a dot but
** locale accepts something else. In that case, the code copies 's'
** to a buffer (because 's' is read-only), changes the dot to the
** current locale radix mark, and tries to convert again.
** The variable 'mode' checks for special characters in the string:
** - 'n' means 'inf' or 'nan' (which should be rejected)
** - 'x' means a hexadecimal numeral
** - '.' just optimizes the search for the common case (no special chars)
*/
static size_t l_str2d(std::string_view s, size_t off, lua_Number* result)
{
  size_t pmode = s.find_first_of(".xXnN", off); /* look for special chars */
  int mode = (pmode != std::string_view::npos)
                 ? ltolower(((unsigned char)(s[pmode])))
                 : 0;
  if (mode == 'n') /* reject 'inf' and 'nan' */
    return std::string_view::npos;
  size_t endpos = l_str2dloc(s, off, result, mode); /* try to convert */
  if (endpos == std::string_view::npos)
  { /* failed? may use the locale radix character instead of '.'
       (the conversion core is '.'-canonical; this shim maps the
       locale point to '.' and retries -- the mirror of upstream,
       whose core spoke the locale and whose shim mapped '.') */
    char buff[L_MAXLENNUM + 1];
    char point = ((char)(lua_getlocaledecpoint()));
    size_t ppoint = (point != '.') ? s.find(point, off)
                                   : std::string_view::npos;
    size_t len = s.size() - off;
    if (ppoint == std::string_view::npos || len > L_MAXLENNUM)
      return std::string_view::npos; /* string too long or no locale point; fail */
    memcpy(buff, s.data() + off, len);     /* copy token (Java: arraycopy) */
    buff[ppoint - off] = '.';              /* canonicalize decimal point */
    endpos = l_str2dloc(std::string_view{buff, len}, 0, result, mode); /* try again */
    if (endpos != std::string_view::npos)
      endpos = off + endpos; /* make relative to 's' */
  }
  return endpos;
}

constexpr lua_Unsigned MAXBY10 = ((lua_Unsigned)(LUA_MAXINTEGER / 10));
constexpr int MAXLASTD = ((int)(LUA_MAXINTEGER % 10));

static size_t l_str2int(std::string_view s, size_t off, lua_Integer* result)
{
  lua_Unsigned a = 0;
  int empty = 1;
  int neg;
  size_t p = off;
  while (lisspace(sv_byteat(s, p)))
    p++; /* skip initial spaces */
  neg = isneg(s, &p);
  if (sv_byteat(s, p) == '0' &&
      (sv_byteat(s, p + 1) == 'x' || sv_byteat(s, p + 1) == 'X'))
  {         /* hex? */
    p += 2; /* skip '0x' */
    for (; lisxdigit(sv_byteat(s, p)); p++)
    {
      a = a * 16 + luaO_hexavalue(s[p]);
      empty = 0;
    }
  }
  else
  { /* decimal */
    for (; lisdigit(sv_byteat(s, p)); p++)
    {
      int d = s[p] - '0';
      if (a >= MAXBY10 && (a > MAXBY10 || d > MAXLASTD + neg)) /* overflow? */
        return std::string_view::npos;                            /* do not accept it (as integer) */
      a = a * 10 + ((unsigned int)(d));
      empty = 0;
    }
  }
  while (lisspace(sv_byteat(s, p)))
    p++; /* skip trailing spaces */
  if (empty || p != s.size())
    return std::string_view::npos; /* something wrong in the numeral */
  else
  {
    *result = ((lua_Integer)((neg) ? 0u - a : a));
    return p;
  }
}

size_t luaO_str2num(std::string_view s, TValue* o)
{
  lua_Integer i;
  lua_Number n;
  size_t e;
  if ((e = l_str2int(s, 0, &i)) != std::string_view::npos)
  { /* try as an integer */
    setivalue(o, i);
  }
  else if ((e = l_str2d(s, 0, &n)) != std::string_view::npos)
  { /* else try as a float */
    setfltvalue(o, n);
  }
  else
    return 0; /* conversion failed */
  assert(e == s.size());
  return e + 1; /* success; return string size plus one */
}

int luaO_utf8esc(std::span<char> buff, l_uint32 x)
{
  int n = 1; /* number of bytes put in buffer (backwards) */
  assert(x <= 0x7FFFFFFFu);
  if (x < 0x80) /* ASCII? */
    buff[UTF8BUFFSZ - 1] = ((char)(x));
  else
  {                          /* need continuation bytes */
    unsigned int mfb = 0x3f; /* maximum that fits in first byte */
    do
    { /* add continuation bytes */
      buff[UTF8BUFFSZ - (n++)] = ((char)(0x80 | (x & 0x3f)));
      x >>= 6;   /* remove added bits */
      mfb >>= 1; /* now there is one less bit available in first byte */
    } while (x > mfb); /* still needs continuation byte? */
    buff[UTF8BUFFSZ - n] = ((char)((~mfb << 1) | x)); /* add first byte */
  }
  return n;
}

/*
** The size of the buffer for the conversion of a number to a string
** 'LUA_N2SBUFFSZ' must be enough to accommodate both the integer and
** the float conversions.  For a long long int, this is 19 digits plus a
** sign and a final '\0', adding to 21. For a long double, it can go to
** a sign, the dot, an exponent letter, an exponent sign, 4 exponent
** digits, the final '\0', plus the significant digits, which are
** approximately the *_DIG attribute.
*/
static_assert(LUA_N2SBUFFSZ >= 20 + DBL_DIG, "invalid value for LUA_N2SBUFFSZ");

/*
** Convert a float to a string, adding it to a buffer. First try with
** a not too large number of digits, to avoid noise (for instance,
** 1.1 going to "1.1000000000000001"). If that lose precision, so
** that reading the result back gives a different number, then do the
** conversion again with extra precision. Moreover, if the numeral looks
** like an integer (without a decimal point or an exponent), add ".0" to
** its end.
*/
static int tostringbuffFloat(lua_Number n, char* buff)
{
  /* first conversion, with 15 significant digits;
     'std::to_chars' with explicit precision produces the printf '%g'
     shape, locale-free */
  std::to_chars_result res = std::to_chars(
      buff, buff + LUA_N2SBUFFSZ - 3, n, std::chars_format::general, 15);
  assert(res.ec == std::errc());
  int len = ((int)(res.ptr - buff));
  buff[len] = '\0';
  lua_Number check;
  std::from_chars(buff + (buff[0] == '-'), buff + len, check,
                  std::chars_format::general); /* read it back */
  if (buff[0] == '-')
    check = -check;
  if (check != n)
  { /* not enough precision? convert again with 17 digits (FMT_N) */
    res = std::to_chars(buff, buff + LUA_N2SBUFFSZ - 3, n,
                        std::chars_format::general, 17);
    assert(res.ec == std::errc());
    len = ((int)(res.ptr - buff));
    buff[len] = '\0';
  }
  /* looks like an integer? */
  if (buff[strspn(buff, "-0123456789")] == '\0')
  {
    buff[len++] = '.';
    buff[len++] = '0'; /* adds '.0' to result */
  }
  { /* output locale shim: the core is '.'-canonical; present the
       locale radix character (a no-op in the "C" locale) */
    char point = ((char)(lua_getlocaledecpoint()));
    if (point != '.')
    {
      char* pdot = strchr(buff, '.');
      if (pdot != nullptr)
        *pdot = point;
    }
  }
  return len;
}

/*
** Convert a number object to a string, adding it to a buffer.
*/
unsigned luaO_tostringbuff(const TValue* obj, std::span<char> buff)
{
  int len;
  assert((((obj)->tt_ & NOVARIANT_MASK) == LUA_TNUMBER));
  assert(buff.size() >= LUA_N2SBUFFSZ);
  if (((obj)->tt_ == LUA_VNUMINT))
  {
    std::string s = std::to_string(ivalue(obj));
    assert(s.size() < LUA_N2SBUFFSZ);
    /* Java: System.arraycopy of the digits (plus terminator) */
    memcpy(buff.data(), s.c_str(), s.size() + 1);
    len = ((int)(s.size()));
  }
  else
  {
    assert(obj->tt_ == LUA_VNUMFLT);
    /* host-conversion seam: 'tostringbuffFloat' runs on to_chars/
       strchr over a NUL-terminated scratch (Java: its own byte[]
       digit assembly) */
    len = tostringbuffFloat(fltvalue(obj), buff.data());
  }
  assert(len < LUA_N2SBUFFSZ);
  return ((unsigned int)(len));
}

/*
** Convert a number object to a Lua string, replacing the value at 'obj'
*/
void luaO_tostring(lua_State* L, TValue* obj)
{
  char buff[LUA_N2SBUFFSZ];
  unsigned len = luaO_tostringbuff(obj, buff);
  setsvalue(L, obj, luaS_newlstr(L, std::string_view{buff, len}));
}

/*
** {==================================================================
** 'luaO_pushvfstring'
** ===================================================================
*/

/*
** Buffer used by 'luaO_pushvfstring': a ByteBuilder whose bytes are
** ADOPTED by the result string (luaS_take — zero copy). 'err' == 2
** signals a length overflow while building. (Growth memory errors
** now surface as host allocation failure — the recorded payload
** accounting status quo extended to builders.)
*/
struct BuffFS
{
  lua_State* L;
  ByteBuilder bb;
  int err;
};

static void initbuff(lua_State* L, BuffFS* buff)
{
  buff->L = L;
  buff->err = 0;
}

/*
** Push final result from 'luaO_pushvfstring'. This function may raise
** errors creating the string, so it must run protected.
*/
static void pushbuff(lua_State* L, void* ud)
{
  BuffFS* buff = ((BuffFS*)(ud));
  if (buff->err == 2) /* length overflow: mark truncation */
    buff->bb.append("...");
  TString* ts = luaS_take(L, buff->bb.finish()); /* adopt the bytes */
  setslots(L, L->top, ts);
  L->top++;
}

static const InternalString* clearbuff(BuffFS* buff)
{
  lua_State* L = buff->L;
  const InternalString* res;
  if (luaD_rawrunprotected(L, pushbuff, buff) != LUA_OK) /* errors? */
    res = nullptr;                                          /* error message is on the top of the stack */
  else
  {
    StkId s = L->top - 1; /* result string is on the top of the stack */
    assert((slottag(L, s) & NOVARIANT_MASK) == LUA_TSTRING);
    res = &slottsval(L, s)->data; /* borrowed view of the pushed string */
  }
  return res;
}

static void addstr2buff(BuffFS* buff, std::string_view str, size_t off,
                        size_t slen)
{
  if (buff->err) /* do nothing else after an error */
    return;
  if (slen > (MAX_SIZE / 2) - buff->bb.size())
  {                /* overflow? */
    buff->err = 2; /* doesn't add anything else; "..." marks it */
    return;
  }
  buff->bb.append(str, off, slen);
}

static void addstr2buff(BuffFS* buff, std::string_view str)
{
  addstr2buff(buff, str, 0, str.size());
}

/*
** Add a numeral to the buffer.
*/
static void addnum2buff(BuffFS* buff, TValue* num)
{
  char numbuff[LUA_N2SBUFFSZ];
  unsigned len = luaO_tostringbuff(num, numbuff);
  addstr2buff(buff, std::string_view{numbuff, len});
}

/*
** Typed appends for 'FmtArgs' (see lobject.h): each records the
** directive it answers plus the value, to be consumed in order by
** 'luaO_pushvfstring'.
*/

FmtArgs& FmtArgs::str(const char* s)
{
  assert(n < MAXFMTARGS);
  arg[n].tag = 's';
  arg[n].s = s;
  n++;
  return *this;
}

FmtArgs& FmtArgs::str(const InternalString* s)
{
  assert(n < MAXFMTARGS);
  arg[n].tag = 'S';
  arg[n].is = s;
  n++;
  return *this;
}

FmtArgs& FmtArgs::chr(int c)
{
  assert(n < MAXFMTARGS);
  arg[n].tag = 'c';
  arg[n].i = c;
  n++;
  return *this;
}

FmtArgs& FmtArgs::num(int d)
{
  assert(n < MAXFMTARGS);
  arg[n].tag = 'd';
  arg[n].i = d;
  n++;
  return *this;
}

FmtArgs& FmtArgs::lint(lua_Integer i)
{
  assert(n < MAXFMTARGS);
  arg[n].tag = 'I';
  arg[n].i = i;
  n++;
  return *this;
}

FmtArgs& FmtArgs::flt(lua_Number f)
{
  assert(n < MAXFMTARGS);
  arg[n].tag = 'f';
  arg[n].n = f;
  n++;
  return *this;
}

FmtArgs& FmtArgs::ptr(const void* p)
{
  assert(n < MAXFMTARGS);
  arg[n].tag = 'p';
  arg[n].p = p;
  n++;
  return *this;
}

FmtArgs& FmtArgs::utf8(unsigned long u)
{
  assert(n < MAXFMTARGS);
  arg[n].tag = 'U';
  arg[n].i = ((lua_Integer)(u));
  n++;
  return *this;
}

/*
** this function handles only '%d', '%c', '%f', '%p', '%s', and '%%'
   conventional formats, plus Lua-specific '%I' and '%U'.
** The format cursor is (InternalString*, offset) — never a raw
** pointer walked through the bytes.
*/
const InternalString* luaO_pushvfstring(lua_State* L,
                                        const InternalString* fmt,
                                        const FmtArgs& args)
{
  BuffFS buff;  /* holds last part of the result */
  std::string_view f = fmt->view();
  size_t pos = 0; /* scan offset into 'fmt' */
  size_t e;       /* offset of next '%' */
  int argn = 0;   /* number of arguments already consumed */
  initbuff(L, &buff);
  while ((e = f.find('%', pos)) != std::string_view::npos)
  {
    addstr2buff(&buff, f, pos, e - pos); /* add 'fmt' up to '%' */
    if (e + 1 >= f.size())
    { /* format ends with a lone '%': keep it in the result */
      addstr2buff(&buff, "%");
      pos = f.size();
      break;
    }
    switch (f[e + 1])
    { /* conversion specifier */
    case 's':
    { /* a string */
      assert(argn < args.n &&
             (args.arg[argn].tag == 's' || args.arg[argn].tag == 'S'));
      if (args.arg[argn].tag == 'S')
      { /* engine string: counted bytes */
        const InternalString* is = args.arg[argn++].is;
        addstr2buff(&buff, is->view());
      }
      else
      { /* transitional zero-terminated form */
        const char* s = args.arg[argn++].s;
        if (s == nullptr)
          s = "(null)";
        addstr2buff(&buff, std::string_view{s, strlen(s)});
      }
      break;
    }
    case 'c':
    { /* an 'int' as a character */
      assert(argn < args.n && args.arg[argn].tag == 'c');
      char c = ((char)(args.arg[argn++].i));
      addstr2buff(&buff, std::string_view{&c, sizeof(char)});
      break;
    }
    case 'd':
    { /* an 'int' */
      assert(argn < args.n && args.arg[argn].tag == 'd');
      TValue num;
      setivalue(&num, args.arg[argn++].i);
      addnum2buff(&buff, &num);
      break;
    }
    case 'I':
    { /* a 'lua_Integer' */
      assert(argn < args.n && args.arg[argn].tag == 'I');
      TValue num;
      setivalue(&num, args.arg[argn++].i);
      addnum2buff(&buff, &num);
      break;
    }
    case 'f':
    { /* a 'lua_Number' */
      assert(argn < args.n && args.arg[argn].tag == 'f');
      TValue num;
      setfltvalue(&num, args.arg[argn++].n);
      addnum2buff(&buff, &num);
      break;
    }
    case 'p':
    {                         /* a pointer, as "0x" + lowercase hex digits */
      char bf[LUA_N2SBUFFSZ];
      assert(argn < args.n && args.arg[argn].tag == 'p');
      const void* p = args.arg[argn++].p;
      bf[0] = '0';
      bf[1] = 'x';
      std::to_chars_result res =
          std::to_chars(bf + 2, bf + LUA_N2SBUFFSZ, ((uintptr_t)(p)), 16);
      assert(res.ec == std::errc());
      addstr2buff(&buff, std::string_view{bf, ((size_t)(res.ptr - bf))});
      break;
    }
    case 'U':
    { /* a codepoint as a UTF-8 sequence */
      char bf[UTF8BUFFSZ];
      assert(argn < args.n && args.arg[argn].tag == 'U');
      int len = luaO_utf8esc(bf, ((l_uint32)(args.arg[argn++].i)));
      addstr2buff(&buff, std::string_view{bf, UTF8BUFFSZ},
                  ((size_t)(UTF8BUFFSZ - len)), ((unsigned int)(len)));
      break;
    }
    case '%':
    {
      addstr2buff(&buff, "%");
      break;
    }
    default:
    {
      addstr2buff(&buff, f, e, 2); /* keep unknown format in the result */
      break;
    }
    }
    pos = e + 2; /* skip '%' and the specifier */
  }
  addstr2buff(&buff, f, pos, f.size() - pos); /* rest of 'fmt' */
  assert(argn == args.n);  /* all arguments consumed */
  return clearbuff(&buff); /* empty buffer into a new string */
}

const InternalString* luaO_pushfstring(lua_State* L,
                                       const InternalString* fmt,
                                       const FmtArgs& args)
{
  const InternalString* msg = luaO_pushvfstring(L, fmt, args);
  if (msg == nullptr) /* error? */
    luaD_throw(L, LUA_ERRMEM);
  return msg;
}

const InternalString* luaO_pushfstring(lua_State* L, const InternalString* fmt)
{
  return luaO_pushfstring(L, fmt, FmtArgs());
}

/*
** TRANSITIONAL const char* forms: deep-copy fmt at the seam (never
** retained) and delegate. Used by the staging C API layer.
*/
const InternalString* luaO_pushvfstring(lua_State* L, const char* fmt,
                                        const FmtArgs& args)
{
  InternalString f(fmt, strlen(fmt));
  return luaO_pushvfstring(L, &f, args);
}

const InternalString* luaO_pushfstring(lua_State* L, const char* fmt,
                                       const FmtArgs& args)
{
  InternalString f(fmt, strlen(fmt));
  return luaO_pushfstring(L, &f, args);
}

const InternalString* luaO_pushfstring(lua_State* L, const char* fmt)
{
  return luaO_pushfstring(L, fmt, FmtArgs());
}

/* }================================================================== */

constexpr char RETS[] = "...";
constexpr char PRE[] = "[string \"";
constexpr char POS[] = "\"]";

void luaO_chunkid(std::span<char> out, std::string_view source)
{
  assert(out.size() >= LUA_IDSIZE);
  size_t srclen = source.size();
  size_t bufflen = LUA_IDSIZE; /* free space in buffer */
  size_t pos = 0;              /* write position in 'out' */
  if (sv_byteat(source, 0) == '=')
  {                        /* 'literal' source */
    if (srclen <= bufflen) /* small enough? */
    { /* copy the content and terminate explicitly (upstream read the
         payload's NUL; the view is bounded) */
      memcpy(out.data(), source.data() + 1, (srclen - 1) * sizeof(char));
      out[srclen - 1] = '\0';
    }
    else
    { /* truncate it */
      memcpy(out.data(), source.data() + 1, (bufflen - 1) * sizeof(char));
      out[bufflen - 1] = '\0';
    }
  }
  else if (sv_byteat(source, 0) == '@')
  {                        /* file name */
    if (srclen <= bufflen) /* small enough? */
    { /* copy the content and terminate explicitly */
      memcpy(out.data(), source.data() + 1, (srclen - 1) * sizeof(char));
      out[srclen - 1] = '\0';
    }
    else
    { /* add '...' before rest of name */
      memcpy(out.data(), RETS, (sizeof(RETS) / sizeof(char) - 1) * sizeof(char));
      pos += sizeof(RETS) / sizeof(char) - 1;
      bufflen -= (sizeof(RETS) / sizeof(char) - 1);
      /* last bufflen-1 content bytes plus an explicit terminator
         (upstream's copy included the payload's NUL) */
      memcpy(out.data() + pos, source.data() + 1 + srclen - bufflen,
             (bufflen - 1) * sizeof(char));
      out[pos + bufflen - 1] = '\0';
    }
  }
  else
  {                                                 /* string; format as [string "source"] */
    size_t nl = source.find('\n'); /* first new line (if any); npos if none */
    memcpy(out.data(), PRE, (sizeof(PRE) / sizeof(char) - 1) * sizeof(char)); /* add prefix */
    pos += sizeof(PRE) / sizeof(char) - 1;
    bufflen -= (sizeof(PRE) - 1) + (sizeof(RETS) - 1) + (sizeof(POS) - 1) + 1; /* save space for prefix+suffix+'\0' */
    if (srclen < bufflen && nl == std::string_view::npos)
    { /* small one-line source? keep it */
      memcpy(out.data() + pos, source.data(), srclen * sizeof(char));
      pos += srclen;
    }
    else
    {
      if (nl != std::string_view::npos)
        srclen = nl; /* stop at first newline */
      if (srclen > bufflen)
        srclen = bufflen;
      memcpy(out.data() + pos, source.data(), srclen * sizeof(char));
      pos += srclen;
      memcpy(out.data() + pos, RETS, (sizeof(RETS) / sizeof(char) - 1) * sizeof(char));
      pos += sizeof(RETS) / sizeof(char) - 1;
    }
    memcpy(out.data() + pos, POS, ((sizeof(POS) / sizeof(char) - 1) + 1) * sizeof(char));
  }
}
