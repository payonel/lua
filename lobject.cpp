/*
** $Id: lobject.c $
** Some generic functions over Lua objects
** See Copyright Notice in lua.h
*/


#include <charconv>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lgc.h"
#include "locale_util.h"
#include "lstate.h"
#include "util_cast.h"
#include "lstring.h"
#include "lvm.h"

#include "lua_internal.h"
#include "lauxlib.h"
#include "literals.h"

namespace lua::object
{

/*
** Computes ceil(log2(x)), which is the smallest integer n such that
** x <= (1 << n).
*/
int luaO_checkliveness(lua_State* L, const TValue* obj)
{
  (void) L;
  if (iscollectable(obj))
  {
    assert((ttypetag(obj) == gcvalue(obj)->tt));
    assert(L == nullptr || !isdead(L->l_G, gcvalue(obj)));
  }
  return 1;
}

int luaO_ceillog2(int x)
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
  return l + log_2[x];
}

/*
** Encodes 'p'% as a floating-point byte, represented as (eeeexxxx).
** The exponent is represented using excess-7. Mimicking IEEE 754, the
** representation normalizes the number when possible, assuming an extra
** 1 before the mantissa (xxxx) and adding one to the exponent (eeee)
** to signal that. So, the real value is (1xxxx) * 2^(eeee - 7 - 1) if
** eeee != 0, and (xxxx) * 2^-7 otherwise (subnormal numbers).
*/
int luaO_codeparam(lua_Integer p)
{
  if (p < 0 || p >= ((lua_Integer)0x1F << (0xF - 7 - 1)) * 100) /* overflow? */
    return 0xFF;                                         /* return maximum value */
  else
  {
    int q = (int)((p * 128 + 99) / 100); /* round up the division */
    if (q < 0x10)
    { /* subnormal number? */
      /* exponent bits are already zero; nothing else to do */
      return q;
    }
    else
    { /* p >= 0x10 implies ceil(log2(p + 1)) >= 5 */
      /* preserve 5 bits in 'p' */
      int log = luaO_ceillog2(q + 1) - 5;
      return ((q >> log) - 0x10) | ((log + 1) << 4);
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
l_mem luaO_applyparam(int p, l_mem x)
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
    return l_intadd(v1, v2);
  case LUA_OPSUB:
    return l_intsub(v1, v2);
  case LUA_OPMUL:
    return l_intmul(v1, v2);
  case LUA_OPMOD:
    return luaV_mod(L, v1, v2);
  case LUA_OPIDIV:
    return luaV_idiv(L, v1, v2);
  case LUA_OPBAND:
    return l_intband(v1, v2);
  case LUA_OPBOR:
    return l_intbor(v1, v2);
  case LUA_OPBXOR:
    return l_intbxor(v1, v2);
  case LUA_OPSHL:
    return luaV_shiftl(v1, v2);
  case LUA_OPSHR:
    return luaV_shiftr(v1, v2);
  case LUA_OPUNM:
    return l_intunm(v1);
  case LUA_OPBNOT:
    return l_intbnot(v1);
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
    IntResult i1;
    IntResult i2;
    if (tointegerns(p1, &i1) && tointegerns(p2, &i2))
    {
      setivalue(res, intarith(L, op, i1.v, i2.v));
      return 1;
    }
    else
      return 0; /* fail */
  }
  case LUA_OPDIV:
  case LUA_OPPOW:
  { /* operate only on floats */
    NumResult n1;
    NumResult n2;
    if (tonumberns(p1, &n1) && tonumberns(p2, &n2))
    {
      setfltvalue(res, numarith(L, op, n1.v, n2.v));
      return 1;
    }
    else
      return 0; /* fail */
  }
  default:
  { /* other operations */
    NumResult n1;
    NumResult n2;
    if (ttisinteger(p1) && ttisinteger(p2))
    {
      setivalue(res, intarith(L, op, ivalue(p1), ivalue(p2)));
      return 1;
    }
    else if (tonumberns(p1, &n1) && tonumberns(p2, &n2))
    {
      setfltvalue(res, numarith(L, op, n1.v, n2.v));
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

struct Sign
{
  int neg;
  size_t off;
};

static Sign isneg(byte_view s, size_t off)
{
  if (sv_byteat(s, off) == '-')
    return {1, off + 1};
  else if (sv_byteat(s, off) == '+')
    return {0, off + 1};
  return {0, off};
}

struct NumParse
{
  size_t end;
  lua_Number value;
};

struct IntParse
{
  size_t end;
  lua_Integer value;
};

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
static NumParse lua_strx2number(byte_view s, size_t off)
{
  int dot = '.';
  lua_Number r = 0.0; /* result (accumulator) */
  int sigdig = 0;               /* number of significant digits */
  int nosigdig = 0;             /* number of non-significant digits */
  int e = 0;                    /* exponent correction */
  int neg;                      /* 1 if number is negative */
  int hasdot = 0;               /* true after seen a dot */
  size_t p = off;
  size_t endpos = off;                /* nothing is valid yet */
  while (lisspace(sv_byteat(s, p)))
    p++;                                                    /* skip initial spaces */
  Sign sign = isneg(s, p);                                       /* check sign */
  neg = sign.neg;
  p = sign.off;
  if (!(sv_byteat(s, p) == '0' && (sv_byteat(s, p + 1) == 'x' || sv_byteat(s, p + 1) == 'X'))) /* check '0x' */
    return {endpos, 0.0};                                   /* invalid format (no '0x') */
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
    return {endpos, 0.0};     /* invalid format */
  endpos = p;                /* valid up to here */
  e *= 4;                     /* each digit multiplies/divides value by 2^4 */
  if (sv_byteat(s, p) == 'p' || sv_byteat(s, p) == 'P')
  {                     /* exponent part? */
    int exp1 = 0;       /* exponent value */
    int neg1;           /* exponent sign */
    p++;                /* skip 'p' */
    Sign sign1 = isneg(s, p); /* sign */
    neg1 = sign1.neg;
    p = sign1.off;
    if (!lisdigit(sv_byteat(s, p)))
      return {endpos, 0.0};          /* invalid; must have at least one digit */
    while (lisdigit(sv_byteat(s, p))) /* read exponent */
      exp1 = exp1 * 10 + s[p++] - '0';
    if (neg1)
      exp1 = -exp1;
    e += exp1;
    endpos = p; /* valid up to here */
  }
  if (neg)
    r = -r;
  return {endpos, ldexp(r, e)};
}

/* }====================================================== */

/* maximum length of a numeral to be converted to a number */
static constexpr int L_MAXLENNUM = 200;

static NumParse str2dec(byte_view s, size_t off)
{
  size_t p = off;
  while (lisspace(sv_byteat(s, p)))
    p++;
  int neg = 0;
  if (sv_byteat(s, p) == '+' || sv_byteat(s, p) == '-')
  {
    neg = (s[p] == '-');
    p++;
  }
  if (sv_byteat(s, p) == '+' || sv_byteat(s, p) == '-')
    return {off, 0.0};
  byte_view::parse_result res = s.parse_number(p, std::chars_format::general);
  lua_Number v = res.result;
  size_t endp = res.next;
  if (res.ec == std::errc::result_out_of_range)
  {
    long e10 = 0;
    int frac = 0;
    int sig = 0;
    long intdigits = 0;
    long fraczeros = 0;
    size_t q;
    for (q = p; q < endp; q++)
    {
      if (s[q] == '.')
        frac = 1;
      else if (s[q] == 'e' || s[q] == 'E')
      {
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
      {
        if (!sig && s[q] != '0')
          sig = 1;
        if (sig && !frac)
          intdigits++;
        else if (!sig && frac)
          fraczeros++;
      }
    }
    long magnitude = e10 + ((intdigits > 0) ? intdigits : -fraczeros);
    v = (magnitude > 0) ? (HUGE_VAL) : 0.0;
  }
  else if (res.ec != std::errc())
    return {off, 0.0};
  return {endp, neg ? -v : v};
}

static NumParse l_str2dloc(byte_view s, size_t off, int mode)
{
  NumParse r = (mode == 'x') ? lua_strx2number(s, off) : str2dec(s, off);
  if (r.end == off)
    return {byte_view::npos, r.value}; /* nothing recognized? */
  while (lisspace(sv_byteat(s, r.end)))
    r.end++; /* skip trailing spaces */
  return {(r.end == s.size()) ? r.end : byte_view::npos, r.value};
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
static NumParse l_str2d(byte_view s, size_t off)
{
  size_t pmode = s.find_first_of(lt::xXnN_chars, off); /* look for special chars */
  int mode = (pmode != byte_view::npos)
                 ? ltolower(((unsigned char)(s[pmode])))
                 : 0;
  if (mode == 'n') /* reject 'inf' and 'nan' */
    return {byte_view::npos, 0.0};
  NumParse r = l_str2dloc(s, off, mode); /* try to convert */
  if (r.end == byte_view::npos)
  {
    byte_inline<L_MAXLENNUM + 1> buff;
    char point = locale_radixpoint();
    size_t ppoint = (point != '.') ? s.find(point, off) : byte_view::npos;
    size_t len = s.size() - off;
    if (ppoint == byte_view::npos || len > L_MAXLENNUM)
      return {byte_view::npos, r.value};
    buff.apply(s.substr(off, len));
    buff[ppoint - off] = '.';
    r = l_str2dloc(buff.view().substr(0, len), 0, mode); /* try again */
    if (r.end != byte_view::npos)
      r.end = off + r.end; /* make relative to 's' */
  }
  return r;
}

constexpr lua_Unsigned MAXBY10 = (LUA_MAXINTEGER / 10);
constexpr int MAXLASTD = (LUA_MAXINTEGER % 10);

static IntParse l_str2int(byte_view s, size_t off)
{
  lua_Unsigned a = 0;
  int empty = 1;
  int neg;
  size_t p = off;
  while (lisspace(sv_byteat(s, p)))
    p++; /* skip initial spaces */
  Sign sign = isneg(s, p);
  neg = sign.neg;
  p = sign.off;
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
        return {byte_view::npos, 0};                            /* do not accept it (as integer) */
      a = a * 10 + d;
      empty = 0;
    }
  }
  while (lisspace(sv_byteat(s, p)))
    p++; /* skip trailing spaces */
  if (empty || p != s.size())
    return {byte_view::npos, 0}; /* something wrong in the numeral */
  else
    return {p, ((lua_Integer)((neg) ? 0u - a : a))};
}

size_t luaO_str2num(byte_view s, TValue* o)
{
  IntParse i = l_str2int(s, 0);
  size_t e = i.end;
  if (e != byte_view::npos)
  { /* try as an integer */
    setivalue(o, i.value);
  }
  else
  { /* else try as a float */
    NumParse n = l_str2d(s, 0);
    e = n.end;
    if (e == byte_view::npos)
      return 0; /* conversion failed */
    setfltvalue(o, n.value);
  }
  assert(e == s.size());
  return e + 1;
}

int luaO_utf8esc(byte_span buff, lua_Integer x)
{
  int n = 1; /* number of bytes put in buffer (backwards) */
  assert(0 <= x && x <= 0x7FFFFFFF);
  if (x < 0x80) /* ASCII? */
    buff[UTF8BUFFSZ - 1] = ((char)(x));
  else
  {                          /* need continuation bytes */
    int mfb = 0x3f; /* maximum that fits in first byte */
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
static size_t tostringbuffFloat(lua_Number n, byte_span buff)
{
  size_t len = buff.write_number(n, std::chars_format::general, 15);
  byte_view view = buff.view().substr(0, len);
  bool bneg = buff.view().starts_with('-');
  byte_view::parse_result ret = view.parse_number(bneg ? 1u : 0u, std::chars_format::general);
  lua_Number check = ret.result;
  if (bneg)
  {
    check = -check;
  }
  if (check != n)
  {
    len = buff.write_number(n, std::chars_format::general, 17);
    view = buff.view().substr(0, len); /* refresh: len just changed */
  }
  if (view.find_first_not_of(lt::digits_and_dash) == byte_view::npos)
  {
    buff[len++] = '.';
    buff[len++] = '0'; /* adds '.0' to result */
  }
  char point = locale_radixpoint();
  if (point != '.')
  {
    view = buff.view().substr(0, len); // update as len could move
    size_t dotpos = view.find('.');
    if (dotpos != byte_view::npos)
      buff[dotpos] = point;
  }
  return len;
}

/*
** Convert a number object to a string, adding it to a buffer.
*/
size_t luaO_tostringbuff(const TValue* obj, byte_span buff)
{
  size_t len;
  assert(ttisnumber(obj));
  assert(buff.size() >= LUA_N2SBUFFSZ);
  if (ttisinteger(obj))
  {
    byte_span bs(buff);
    len = bs.write_number(ivalue(obj));
  }
  else
  {
    assert(ttisfloat(obj));
    len = tostringbuffFloat(fltvalue(obj), buff);
  }
  assert(len < LUA_N2SBUFFSZ);
  return len;
}

/*
** Convert a number object to a Lua string, replacing the value at 'obj'
*/
void luaO_tostring(lua_State* L, TValue* obj)
{
  byte_inline<LUA_N2SBUFFSZ> buff;
  size_t len = luaO_tostringbuff(obj, buff);
  byte_view bytes = buff.view().substr(0, len);
  setsvalue(L, obj, TString::luaS_createstr(L, bytes));
}

/*
** {==================================================================
** 'luaO_pushvfstring'
** ===================================================================
*/

struct BuffFS
{
  lua_State* L;
  byte_owner b;
  size_t blen;
  int err;
};

static void initbuff(lua_State* L, BuffFS* buff)
{
  buff->L = L;
  buff->b = {};
  buff->blen = 0;
  buff->err = 0;
}

static void freebuff(BuffFS* buff)
{
  global_State* g = buff->L->l_G;
  if (buff->b.data() != nullptr)
  {
    (void)luaM_alloc_char_span(g, nullptr, buff->b, 0, false);
    buff->b = {};
  }
}

static void pushbuff(lua_State* L, BuffFS* buff)
{
  if (buff->err == 1)
    luaD_throw(L, LUA_ERRMEM);
  InternalString res(byte_view(buff->b.data(), buff->b.size()), 0, buff->blen);
  if (buff->err == 2)
    res = res.concat(lt::ellipsis);
  TString* ts = TString::luaS_take(L, std::move(res));
  setslots(L, L->top, ts);
  L->top++;
}

struct PushBuff : ProtectedCall
{
  BuffFS* buff;
  void run(lua_State* L) override { pushbuff(L, buff); }
};

static const InternalString* clearbuff(BuffFS* buff)
{
  lua_State* L = buff->L;
  const InternalString* res;
  PushBuff pb;
  pb.buff = buff;
  if (luaD_rawrunprotected(L, &pb) != LUA_OK) /* errors? */
    res = nullptr;                                          /* error message is on the top of the stack */
  else
  {
    StkId s = L->top - 1;
    assert(novariant(slottag(L, s)) == LUA_TSTRING);
    res = &slottsval(L, s)->data;
  }
  freebuff(buff);
  return res;
}

static void addstr2buff(BuffFS* buff, byte_view bv)
{
  if (buff->err) /* do nothing else after an error */
    return;
  size_t slen = bv.size();
  if (slen > (MAX_SIZE / 2) - buff->blen)
  {                /* overflow? */
    buff->err = 2;
    return;
  }
  if (buff->blen + slen > buff->b.size())
  {
    global_State* g = buff->L->l_G;
    size_t newsize = std::max<size_t>(buff->blen + slen, 256);
    byte_owner nb = luaM_alloc_char_span(g, nullptr, buff->b, newsize, false);
    if (l_unlikely(nb.data() == nullptr))
    {
      buff->err = 1; /* memory error */
      return;
    }
    buff->b = std::move(nb);
  }
  byte_span(buff->b).copy_from(bv, buff->blen);
  buff->blen += slen;
}

/*
** Add a numeral to the buffer.
*/
static void addnum2buff(BuffFS* buff, TValue* num)
{
  byte_inline<LUA_N2SBUFFSZ> numbuff;
  size_t len = luaO_tostringbuff(num, numbuff);
  addstr2buff(buff, numbuff.view().substr(0, len));
}


FmtArgs& FmtArgs::str(byte_view s)
{
  assert(n < MAXFMTARGS);
  assert(s.data() != nullptr);
  arg[n].tag = 'S';
  arg[n].s = s;
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

FmtArgs& FmtArgs::utf8(lua_Integer u)
{
  assert(n < MAXFMTARGS);
  arg[n].tag = 'U';
  arg[n].i = u;
  n++;
  return *this;
}

const InternalString* luaO_pushvfstring(lua_State* L,
                                        byte_view fmt,
                                        const FmtArgs& args)
{
  BuffFS buff;  /* holds last part of the result */
  byte_view f = fmt;
  int argn = 0;
  initbuff(L, &buff);
  FmtScan sc{f};
  while (sc.next())
  {
    addstr2buff(&buff, byte_view(f).substr(sc.litStart, sc.litEnd - sc.litStart));
    switch (sc.spec)
    { /* conversion specifier */
    case 's':
    {
      assert(argn < args.n && args.arg[argn].tag == 'S');
      addstr2buff(&buff, args.arg[argn++].s);
      break;
    }
    case 'c':
    { /* an 'int' as a character */
      assert(argn < args.n && args.arg[argn].tag == 'c');
      char c[1] = {((char)(args.arg[argn++].i))};
      addstr2buff(&buff, byte_view(c, std::size(c)));
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
    {
      byte_inline<LUA_N2SBUFFSZ> bf;
      assert(argn < args.n && args.arg[argn].tag == 'p');
      const void* p = args.arg[argn++].p;
      size_t bfn = util::pointertext(bf, 0, p);
      addstr2buff(&buff, bf.view().substr(0, bfn));
      break;
    }
    case 'U':
    {
      assert(argn < args.n && args.arg[argn].tag == 'U');
      byte_inline<UTF8BUFFSZ> bf;
      int len = luaO_utf8esc(bf, args.arg[argn++].i);
      addstr2buff(&buff, bf.view().substr(UTF8BUFFSZ - len));
      break;
    }
    case -1:
    case '%':
    {
      addstr2buff(&buff, lt::percent_ch);
      break;
    }
    default:
    {
      addstr2buff(&buff, byte_view(f).substr(sc.escPos, 2)); /* keep unknown format in the result */
      break;
    }
    }
  }
  addstr2buff(&buff, byte_view(f).substr(sc.litStart, sc.litEnd - sc.litStart)); /* rest of 'fmt' */
  assert(argn == args.n);
  return clearbuff(&buff); /* empty buffer into a new string */
}

int luaO_fmtargcount(byte_view fmt)
{
  int n = 0;
  FmtScan sc{fmt};
  while (sc.next())
  {
    switch (sc.spec)
    {
    case 's':
    case 'c':
    case 'd':
    case 'I':
    case 'f':
    case 'p':
    case 'U':
      n++;
      break;
    default:
      break;
    }
  }
  return n;
}

const InternalString* luaO_pushfstring(lua_State* L, byte_view fmt, const FmtArgs& args)
{
  const InternalString* msg = luaO_pushvfstring(L, fmt, args);
  if (msg == nullptr) /* error? */
    luaD_throw(L, LUA_ERRMEM);
  return msg;
}

/* }================================================================== */

void luaO_chunkid(byte_span out, byte_view source)
{
  assert(out.size() >= LUA_IDSIZE);
  size_t srclen = source.size();
  size_t bufflen = LUA_IDSIZE; /* free space in buffer */
  size_t pos = 0;
  if (sv_byteat(source, 0) == '=')
  {                        /* 'literal' source */
    if (srclen <= bufflen) /* small enough? */
    {
      byte_span(out).copy_from(source.substr(1, srclen - 1));
      out[srclen - 1] = '\0';
    }
    else
    { /* truncate it */
      byte_span(out).copy_from(source.substr(1, bufflen - 1));
      out[bufflen - 1] = '\0';
    }
  }
  else if (sv_byteat(source, 0) == '@')
  {                        /* file name */
    if (srclen <= bufflen) /* small enough? */
    {
      byte_span(out).copy_from(source.substr(1, srclen - 1));
      out[srclen - 1] = '\0';
    }
    else
    { /* add '...' before rest of name */
      byte_view bytes = lt::ellipsis;
      byte_span(out).copy_from( bytes);
      pos += lt::ellipsis.size();
      bufflen -= lt::ellipsis.size();
      byte_span(out).copy_from(source.substr(1 + srclen - bufflen, bufflen - 1), pos);
      out[pos + bufflen - 1] = '\0';
    }
  }
  else
  {                                                 /* string; format as [string "source"] */
    size_t nl = source.find('\n');
    byte_view bytes = lt::string_2;
    byte_span(out).copy_from(bytes); /* add prefix */
    pos += lt::string_2.size();
    bufflen -= lt::string_2.size() + lt::ellipsis.size() + lt::backquote.size() + 1; /* save space for prefix+suffix+'\0' */
    if (srclen < bufflen && nl == byte_view::npos)
    {
      byte_span(out).copy_from(source.substr(0, srclen), pos);
      pos += srclen;
    }
    else
    {
      if (nl != byte_view::npos)
        srclen = nl; /* stop at first newline */
      if (srclen > bufflen)
        srclen = bufflen;
      byte_span(out).copy_from(source.substr(0, srclen), pos);
      pos += srclen;
      bytes = lt::ellipsis;
      byte_span(out).copy_from(bytes, pos);
      pos += lt::ellipsis.size();
    }
    bytes = lt::backquote;
    byte_span(out).copy_from(bytes,pos);
    out[pos + lt::backquote.size()] = '\0';
  }
}

}
