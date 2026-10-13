package com.payonel.lungolua.lib;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.api.LuaInternal.*;

import com.payonel.lungolua.api.KNamer;
import com.payonel.lungolua.api.luaL_Box;
import com.payonel.lungolua.api.luaL_Buffer;
import com.payonel.lungolua.api.luaL_RegCFI;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_KFunction;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.CFuncInfo;
import com.payonel.lungolua.runtime.lua_State;

public final class Lutf8lib
{
  private Lutf8lib()
  {
  }

  private static final int MAXUNICODE = 0x10FFFF;

  private static final int MAXUTF = 0x7FFFFFFF;

  private static boolean iscont(int c)
  {
    return (c & 0xC0) == 0x80;
  }

  private static int ubyte(InternalString s, int i)
  {
    return (i < s.size()) ? s.byteAt(i) : 0;
  }

  private static boolean iscontat(InternalString s, int i)
  {
    return iscont(ubyte(s, i));
  }

  /* from strlib */
  /* translate a relative string position: negative means back from end */
  private static long u_posrelat(long pos, int len)
  {
    if (pos >= 0)
      return pos;
    else if (pos < -(long) len)
      return 0;
    else
      return (long) len + pos + 1;
  }

  private static final class Utf8Code
  {
    int next;
    int code;

    Utf8Code(int next, int code)
    {
      this.next = next;
      this.code = code;
    }
  }

  private static final int[] limits =
      {Integer.MAX_VALUE, 0x80, 0x800, 0x10000, 0x200000, 0x4000000};

  private static Utf8Code utf8_decode(InternalString s, int pos, boolean strict)
  {
    int c = ubyte(s, pos);
    int res = 0; /* final result */
    if (c < 0x80)     /* ASCII? */
      res = c;
    else if (c >= 0xfe) /* c >= 1111 1110b ? */
      return new Utf8Code(0, 0);         /* would need six or more continuation bytes */
    else
    {
      int count = 0; /* to count number of continuation bytes */
      for (; (c & 0x40) != 0; c <<= 1)
      {                                                            /* while it needs continuation bytes... */
        int cc = ubyte(s, pos + ++count); /* read next byte */
        if (!iscont(cc))                                     /* not a continuation byte? */
          return new Utf8Code(0, 0);                                                /* invalid byte sequence */
        res = (res << 6) | (cc & 0x3F);                            /* add lower 6 bits from cont. byte */
      }
      assert count <= 5;
      res |= (c & 0x7F) << (count * 5); /* add first byte */
      if (res > MAXUTF || res < limits[count])
        return new Utf8Code(0, 0);             /* invalid byte sequence */
      pos += count;  /* skip continuation bytes read */
    }
    if (strict)
    {
      /* check for invalid code points; too large or surrogates */
      if (res > MAXUNICODE || (0xD800 <= res && res <= 0xDFFF))
        return new Utf8Code(0, 0);
    }
    return new Utf8Code(pos + 1, res); /* +1 to include first byte */
  }

  /*
  ** utf8len(s [, i [, j [, lax]]]) --> number of characters that
  ** start in the range [i,j], or nil + current position if 's' is not
  ** well formed in that interval
  */
  private static int utflen_from(lua_State L, InternalString s, long posi, long posj, boolean lax,
                                 long n)
  {
    long mark = posi;
    while (posi <= posj)
    {
      int s1 = utf8_decode(s, (int) posi, !lax).next;
      if (s1 == 0)
      {                               /* conversion error? */
        luaL_pushfail(L);             /* return fail ... */
        lua_pushinteger(L, posi + 1); /* ... and current position */
        return 2;
      }
      posi = s1;
      n++;
      if (posi - mark >= (long) LUAL_SLICE)
      {
        if (luaL_slicespent(L, (long) (posi - mark)) && posi <= posj)
        {
          if (lua_gettop(L) == 4)
            luaL_newbox(L);
          luaL_Box box = luaL_tobox(L, 5);
          box.ints[0] = posi;
          box.ints[1] = n;
          lua_checkpointk(L, 0, utflenk);
        }
        mark = posi;
      }
    }
    lua_pushinteger(L, n);
    return 1;
  }

  private static int utflenk(lua_State L, int status, long ctx)
  {
    luaL_Box box = (lua_touserobject(L, 5) instanceof luaL_Box x) ? x : null;
    if (box == null || lua_type(L, 1) != LUA_TSTRING)
      luaL_errori(L, Lt.invalid_state_of_a_stopped_call);
    InternalString s = lua_toistring(L, 1);
    int len = s.size();
    long posj = u_posrelat(luaL_optinteger(L, 3, -1), len) - 1;
    if (posj >= (long) len || box.ints[0] < 0 || box.ints[0] > posj || box.ints[1] < 0)
      luaL_errori(L, Lt.invalid_state_of_a_stopped_call);
    return utflen_from(L, s, box.ints[0], posj, lua_toboolean(L, 4), box.ints[1]);
  }

  private static final lua_KFunction utflenk = Lutf8lib::utflenk;

  private static final KNamer knamer_utflenk = new KNamer("utf8.utflenk", utflenk);

  private static int utflen(lua_State L)
  {
    InternalString s = luaL_checkistring(L, 1);
    int len = s.size(); /* string length in bytes */
    long posi = u_posrelat(luaL_optinteger(L, 2, 1), len);
    long posj = u_posrelat(luaL_optinteger(L, 3, -1), len);
    boolean lax = lua_toboolean(L, 4);
    luaL_argchecki(L, 1 <= posi && --posi <= (long) len, 2, Lt.initial_position_out_of_bounds);
    luaL_argchecki(L, --posj < (long) len, 3, Lt.final_position_out_of_bounds);
    long n = 0; /* counter for the number of characters */
    lua_settop(L, 4);
    return utflen_from(L, s, posi, posj, lax, n);
  }

  private static final lua_CFunction utflen = Lutf8lib::utflen;

  /*
  ** codepoint(s, [i, [j [, lax]]]) -> returns codepoints for all
  ** characters that start in the range [i,j]
  */
  private static int codepoint(lua_State L)
  {
    InternalString s = luaL_checkistring(L, 1);
    int len = s.size();
    long posi = u_posrelat(luaL_optinteger(L, 2, 1), len);
    long pose = u_posrelat(luaL_optinteger(L, 3, posi), len);
    boolean lax = lua_toboolean(L, 4);
    int n;
    int se;
    luaL_argchecki(L, posi >= 1, 2, Lt.out_of_bounds);
    luaL_argchecki(L, pose <= (long) len, 3, Lt.out_of_bounds);
    if (posi > pose)
      return 0;                 /* empty interval; return no values */
    if (pose - posi >= Integer.MAX_VALUE) /* (lua_Integer -> int) overflow? */
      return luaL_errori(L, Lt.string_slice_too_long);
    n = (int) (pose - posi) + 1; /* upper bound for number of returns */
    luaL_checkstacki(L, n, Lt.string_slice_too_long);
    n = 0;             /* count the number of returns */
    se = (int) pose; /* string end */
    for (int pos = (int) posi - 1; pos < se;)
    {
      Utf8Code dec = utf8_decode(s, pos, !lax);
      pos = dec.next;
      if (pos == 0)
        return luaL_errori(L, Lt.invalid_UTF_8_code);
      lua_pushinteger(L, dec.code);
      n++;
    }
    return n;
  }

  private static final lua_CFunction codepoint = Lutf8lib::codepoint;

  private static void pushutfchar(lua_State L, int arg)
  {
    long code = luaL_checkinteger(L, arg);
    luaL_argchecki(L, 0 <= code && code <= MAXUTF, arg, Lt.value_out_of_range);
    lua_pushinteger(L, code);
    lua_pushfstringi(L, Lt.fmt_U);
  }

  /*
  ** utfchar(n1, n2, ...)  -> char(n1)..char(n2)...
  */
  private static int utfchar(lua_State L)
  {
    int n = lua_gettop(L); /* number of arguments */
    if (n == 1)            /* optimize common case of single char */
      pushutfchar(L, 1);
    else
    {
      int i;
      luaL_Buffer b = new luaL_Buffer();
      try
      {
        luaL_buffinit(L, b);
        for (i = 1; i <= n; i++)
        {
          pushutfchar(L, i);
          luaL_addvalue(b);
        }
        luaL_pushresult(b);
      }
      finally
      {
        b.release();
      }
    }
    return 1;
  }

  private static final lua_CFunction utfchar = Lutf8lib::utfchar;

  private static final int OFS_START = 0;
  private static final int OFS_BACK = 1;
  private static final int OFS_FORWARD = 2;
  private static final int OFS_LAST = 3;

  private static void byteoffsetpoint(lua_State L, int[] work, int phase, long posi, long n,
                                      long first)
  {
    if (++work[0] < LUAL_SLICE)
      return;
    boolean spent = luaL_slicespent(L, work[0]);
    work[0] = 0;
    if (!spent)
      return;
    if (lua_gettop(L) == 3)
      luaL_newbox(L);
    luaL_Box box = luaL_tobox(L, 4);
    box.ints[0] = phase;
    box.ints[1] = posi;
    box.ints[2] = n;
    box.ints[3] = first;
    lua_checkpointk(L, 0, byteoffsetk);
  }

  private static int byteoffset_from(lua_State L, InternalString s, int phase, long posi, long n,
                                     long first)
  {
    int len = s.size();
    int[] work = {0};
    switch (phase)
    {
    case OFS_START:
      /* find beginning of current byte sequence */
      while (posi > 0 && iscontat(s, (int) posi))
      {
        posi--;
        byteoffsetpoint(L, work, OFS_START, posi, n, first);
      }
      break;
    case OFS_BACK:
      while (n < 0 && posi > 0)
      { /* move back */
        posi--;
        if (posi == 0 || !iscontat(s, (int) posi))
          n++;
        byteoffsetpoint(L, work, OFS_BACK, posi, n, first);
      }
      break;
    case OFS_FORWARD:
      while (n > 0 && posi < (long) len)
      {
        posi++;
        if (!iscontat(s, (int) posi))
          n--;
        byteoffsetpoint(L, work, OFS_FORWARD, posi, n, first);
      }
      break;
    default:
      break;
    }
    if (phase != OFS_LAST)
    {
      if (n != 0)
      { /* did not find given character? */
        luaL_pushfail(L);
        return 1;
      }
      first = posi;
      if ((ubyte(s, (int) posi) & 0x80) == 0)
      { /* else one-byte character: final position is the initial one */
        lua_pushinteger(L, first + 1); /* initial position */
        lua_pushinteger(L, posi + 1);
        return 2;
      }
      if (iscont(ubyte(s, (int) posi)))
        return luaL_errori(L, Lt.initial_position_is_a_continuation_byte);
    }
    /* multi-byte character? */
    while (iscontat(s, (int) posi + 1))
    {
      posi++; /* skip to last continuation byte */
      byteoffsetpoint(L, work, OFS_LAST, posi, n, first);
    }
    lua_pushinteger(L, first + 1); /* initial position */
    lua_pushinteger(L, posi + 1); /* 'posi' now is the final position */
    return 2;
  }

  private static int byteoffsetk(lua_State L, int status, long ctx)
  {
    luaL_Box box = (lua_touserobject(L, 4) instanceof luaL_Box x) ? x : null;
    if (box == null || lua_type(L, 1) != LUA_TSTRING || box.ints[0] < OFS_START || box.ints[0] > OFS_LAST ||
        box.ints[1] < 0 || box.ints[1] > (long) lua_rawlen(L, 1) ||
        box.ints[3] < 0 || box.ints[3] > box.ints[1])
      luaL_errori(L, Lt.invalid_state_of_a_stopped_call);
    return byteoffset_from(L, lua_toistring(L, 1), (int) box.ints[0], box.ints[1], box.ints[2], box.ints[3]);
  }

  private static final lua_KFunction byteoffsetk = Lutf8lib::byteoffsetk;

  private static final KNamer knamer_byteoffsetk = new KNamer("utf8.byteoffsetk", byteoffsetk);

  /*
  ** offset(s, n, [i])  -> indices where n-th character counting from
  **   position 'i' starts and ends; 0 means character at 'i'.
  */
  private static int byteoffset(lua_State L)
  {
    InternalString s = luaL_checkistring(L, 1);
    int len = s.size();
    long n = luaL_checkinteger(L, 2);
    long posi = (n >= 0) ? 1 : (long) len + 1;
    posi = u_posrelat(luaL_optinteger(L, 3, posi), len);
    luaL_argchecki(L, 1 <= posi && --posi <= (long) len, 3, Lt.position_out_of_bounds);
    lua_settop(L, 3);
    if (n == 0)
      return byteoffset_from(L, s, OFS_START, posi, n, 0);
    if (iscontat(s, (int) posi))
      return luaL_errori(L, Lt.initial_position_is_a_continuation_byte);
    if (n < 0)
      return byteoffset_from(L, s, OFS_BACK, posi, n, 0);
    n--; /* do not move for 1st character */
    return byteoffset_from(L, s, OFS_FORWARD, posi, n, 0);
  }

  private static final lua_CFunction byteoffset = Lutf8lib::byteoffset;

  private static int iter_aux(lua_State L, boolean strict)
  {
    InternalString s = luaL_checkistring(L, 1);
    int len = s.size();
    long n = lua_tointeger(L, 2);
    if (Long.compareUnsigned(n, len) < 0)
    {
      while (iscontat(s, (int) n))
        n++; /* go to next character */
    }
    if (Long.compareUnsigned(n, len) >= 0) /* (also handles original 'n' being negative) */
      return 0;   /* no more codepoints */
    else
    {
      Utf8Code dec = utf8_decode(s, (int) n, strict);
      if (dec.next == 0 || iscontat(s, dec.next))
        return luaL_errori(L, Lt.invalid_UTF_8_code);
      lua_pushinteger(L, n + 1);
      lua_pushinteger(L, dec.code);
      return 2;
    }
  }

  private static int iter_auxstrict(lua_State L)
  {
    return iter_aux(L, true);
  }

  private static final lua_CFunction iter_auxstrict = Lutf8lib::iter_auxstrict;

  private static int iter_auxlax(lua_State L)
  {
    return iter_aux(L, false);
  }

  private static final lua_CFunction iter_auxlax = Lutf8lib::iter_auxlax;

  private static final CFuncInfo cfi_iter_auxlax = new CFuncInfo(iter_auxlax, "utf8.iter_auxlax");
  private static final CFuncInfo cfi_iter_auxstrict = new CFuncInfo(iter_auxstrict, "utf8.iter_auxstrict");

  private static int iter_codes(lua_State L)
  {
    boolean lax = lua_toboolean(L, 2);
    InternalString s = luaL_checkistring(L, 1);
    luaL_argchecki(L, !iscontat(s, 0), 1, Lt.invalid_UTF_8_code);
    lua_pushcfuncinfo(L, lax ? cfi_iter_auxlax : cfi_iter_auxstrict);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    return 3;
  }

  private static final lua_CFunction iter_codes = Lutf8lib::iter_codes;

  private static final CFuncInfo cfi_byteoffset = new CFuncInfo(byteoffset, "utf8.byteoffset");
  private static final CFuncInfo cfi_codepoint = new CFuncInfo(codepoint, "utf8.codepoint");
  private static final CFuncInfo cfi_utfchar = new CFuncInfo(utfchar, "utf8.utfchar");
  private static final CFuncInfo cfi_utflen = new CFuncInfo(utflen, "utf8.utflen");
  private static final CFuncInfo cfi_iter_codes = new CFuncInfo(iter_codes, "utf8.iter_codes");

  private static final luaL_RegCFI[] funcs = {
    new luaL_RegCFI(Lt.offset, cfi_byteoffset),
    new luaL_RegCFI(Lt.codepoint, cfi_codepoint),
    new luaL_RegCFI(Lt.char_kw, cfi_utfchar),
    new luaL_RegCFI(Lt.len, cfi_utflen),
    new luaL_RegCFI(Lt.codes, cfi_iter_codes),
    new luaL_RegCFI(Lt.charpattern, null),
  };

  public static int luaopen_utf8(lua_State L)
  {
    luaL_newlibcfi(L, funcs);
    ByteView patt = Lt.utf8patt;
    lua_pushlstring(L, patt, 0, patt.size());
    lua_setfieldi(L, -2, Lt.charpattern);
    return 1;
  }

  public static final lua_CFunction luaopen_utf8 = Lutf8lib::luaopen_utf8;
}
