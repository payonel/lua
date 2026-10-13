package com.payonel.lungolua.compiler;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.bytes.Lctype.*;
import static com.payonel.lungolua.bytes.Lload.*;
import static com.payonel.lungolua.bytes.Lzio.*;
import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.object.Lstring.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lhack.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.vm.Ldebug.*;

import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.InternalString;
import com.payonel.lungolua.bytes.LoadFrame;
import com.payonel.lungolua.bytes.LoadResume;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.bytes.MBuffer;
import com.payonel.lungolua.bytes.ZIO;
import com.payonel.lungolua.object.FmtArgs;
import com.payonel.lungolua.object.TString;
import com.payonel.lungolua.object.TValue;
import com.payonel.lungolua.runtime.lua_State;

public final class Llex
{
  private Llex()
  {
  }

  /*
  ** Single-char tokens (terminal symbols) are represented by their own
  ** numeric code. Other tokens start at the following value.
  */
  public static final int FIRST_RESERVED = 0xFF + 1;

  /*
   * WARNING: if you change the order of this enumeration,
   * grep "ORDER RESERVED"
   */
  /* enum RESERVED */
  /* terminal symbols denoted by reserved words */
  public static final int TK_AND = FIRST_RESERVED;
  public static final int TK_BREAK = FIRST_RESERVED + 1;
  public static final int TK_DO = FIRST_RESERVED + 2;
  public static final int TK_ELSE = FIRST_RESERVED + 3;
  public static final int TK_ELSEIF = FIRST_RESERVED + 4;
  public static final int TK_END = FIRST_RESERVED + 5;
  public static final int TK_FALSE = FIRST_RESERVED + 6;
  public static final int TK_FOR = FIRST_RESERVED + 7;
  public static final int TK_FUNCTION = FIRST_RESERVED + 8;
  public static final int TK_GLOBAL = FIRST_RESERVED + 9;
  public static final int TK_GOTO = FIRST_RESERVED + 10;
  public static final int TK_IF = FIRST_RESERVED + 11;
  public static final int TK_IN = FIRST_RESERVED + 12;
  public static final int TK_LOCAL = FIRST_RESERVED + 13;
  public static final int TK_NIL = FIRST_RESERVED + 14;
  public static final int TK_NOT = FIRST_RESERVED + 15;
  public static final int TK_OR = FIRST_RESERVED + 16;
  public static final int TK_REPEAT = FIRST_RESERVED + 17;
  public static final int TK_RETURN = FIRST_RESERVED + 18;
  public static final int TK_THEN = FIRST_RESERVED + 19;
  public static final int TK_TRUE = FIRST_RESERVED + 20;
  public static final int TK_UNTIL = FIRST_RESERVED + 21;
  public static final int TK_WHILE = FIRST_RESERVED + 22;
  /* other terminal symbols */
  public static final int TK_IDIV = FIRST_RESERVED + 23;
  public static final int TK_CONCAT = FIRST_RESERVED + 24;
  public static final int TK_DOTS = FIRST_RESERVED + 25;
  public static final int TK_EQ = FIRST_RESERVED + 26;
  public static final int TK_GE = FIRST_RESERVED + 27;
  public static final int TK_LE = FIRST_RESERVED + 28;
  public static final int TK_NE = FIRST_RESERVED + 29;
  public static final int TK_SHL = FIRST_RESERVED + 30;
  public static final int TK_SHR = FIRST_RESERVED + 31;
  public static final int TK_DBCOLON = FIRST_RESERVED + 32;
  public static final int TK_EOS = FIRST_RESERVED + 33;
  public static final int TK_FLT = FIRST_RESERVED + 34;
  public static final int TK_INT = FIRST_RESERVED + 35;
  public static final int TK_NAME = FIRST_RESERVED + 36;
  public static final int TK_STRING = FIRST_RESERVED + 37;

  /* number of reserved words */
  public static final int NUM_RESERVED = TK_WHILE - FIRST_RESERVED + 1;

  /*
  ** A load that a freeze can stop (see lload.h): a function with calls a
  ** freeze can pass through runs as a state machine, its state and its
  ** locals in its frame ('l'). Each such call is made at a state of its own,
  ** the one its function goes to again when it is made again after the
  ** thaw (the call made again, which goes on where it stopped); a freeze
  ** passing records the frame. A call with no frame of its own (as
  ** 'check_next1') goes the same way when made again.
  */

  /* the frames of the lexer's functions (see lload.h) */
  private static final class InclineFrame extends LoadFrame
  {
    int old = 0;

    InclineFrame()
    {
    }

    InclineFrame(InclineFrame o)
    {
      super(o);
      old = o.old;
    }

    @Override
    public LoadFrame copy()
    {
      return new InclineFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static final class NumeralFrame extends LoadFrame
  {
    int first = 0;
    int hex = 0; /* exponent mark 'Pp' (else 'Ee') */

    NumeralFrame()
    {
    }

    NumeralFrame(NumeralFrame o)
    {
      super(o);
      first = o.first;
      hex = o.hex;
    }

    @Override
    public LoadFrame copy()
    {
      return new NumeralFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static final class SepFrame extends LoadFrame
  {
    int count = 0;
    int s = 0;

    SepFrame()
    {
    }

    SepFrame(SepFrame o)
    {
      super(o);
      count = o.count;
      s = o.s;
    }

    @Override
    public LoadFrame copy()
    {
      return new SepFrame(this);
    }

    @Override
    public int size()
    {
      return 40;
    }
  }

  private static final class LongStringFrame extends LoadFrame
  {
    int line = 0; /* initial line (for error message) */
    int r = 0;

    LongStringFrame()
    {
    }

    LongStringFrame(LongStringFrame o)
    {
      super(o);
      line = o.line;
      r = o.r;
    }

    @Override
    public LoadFrame copy()
    {
      return new LongStringFrame(this);
    }

    @Override
    public int size()
    {
      return 40;
    }
  }

  private static final class HexaEscFrame extends LoadFrame
  {
    int r = 0;

    HexaEscFrame()
    {
    }

    HexaEscFrame(HexaEscFrame o)
    {
      super(o);
      r = o.r;
    }

    @Override
    public LoadFrame copy()
    {
      return new HexaEscFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static final class Utf8EscFrame extends LoadFrame
  {
    int r = 0;
    int i = 0; /* number of chars to be removed: start with #"\\u{X" */

    Utf8EscFrame()
    {
    }

    Utf8EscFrame(Utf8EscFrame o)
    {
      super(o);
      r = o.r;
      i = o.i;
    }

    @Override
    public LoadFrame copy()
    {
      return new Utf8EscFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static final class DecEscFrame extends LoadFrame
  {
    int i = 0;
    int r = 0; /* result accumulator */

    DecEscFrame()
    {
    }

    DecEscFrame(DecEscFrame o)
    {
      super(o);
      i = o.i;
      r = o.r;
    }

    @Override
    public LoadFrame copy()
    {
      return new DecEscFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static final class StringFrame extends LoadFrame
  {
    int c = 0; /* final character to be saved */

    StringFrame()
    {
    }

    StringFrame(StringFrame o)
    {
      super(o);
      c = o.c;
    }

    @Override
    public LoadFrame copy()
    {
      return new StringFrame(this);
    }

    @Override
    public int size()
    {
      return 32;
    }
  }

  private static final class LexFrame extends LoadFrame
  {
    int sep = 0;
    int c = 0;

    LexFrame()
    {
    }

    LexFrame(LexFrame o)
    {
      super(o);
      sep = o.sep;
      c = o.c;
    }

    @Override
    public LoadFrame copy()
    {
      return new LexFrame(this);
    }

    @Override
    public int size()
    {
      return 40;
    }
  }

  private static void next(LexState ls)
  {
    ls.current = (ls.z.pos < ls.z.chunk.size())
                     ? ls.z.chunk.at(ls.z.pos++) & 0xFF
                     : luaZ_fill(ls.z);
  }

  /* minimum size for string buffer */
  private static final int LUA_MINBUFFER = 32;

  private static int currIsNewline(LexState ls)
  {
    return (ls.current == '\n' || ls.current == '\r') ? 1 : 0;
  }

  /* ORDER RESERVED */
  private static final ByteView[] luaX_tokens = {
      Lt.and_kw, Lt.break_kw, Lt.do_kw, Lt.else_kw, Lt.elseif,
      Lt.end, Lt.false_kw, Lt.for_kw, Lt.function, Lt.global, Lt.goto_kw, Lt.if_kw,
      Lt.in, Lt.local, Lt.nil, Lt.not_kw, Lt.or_kw, Lt.repeat,
      Lt.return_kw, Lt.then, Lt.true_kw, Lt.until, Lt.while_kw,
      Lt.idiv_op, Lt.dotdot, Lt.ellipsis, Lt.eq_op, Lt.ge_op, Lt.le_op, Lt.ne_op,
      Lt.shl_op, Lt.shr_op, Lt.dcolon, Lt.eof,
      Lt.number_1, Lt.integer_1, Lt.name_1, Lt.string_1};

  private static void save(LexState ls, int c)
  {
    MBuffer b = ls.buff;
    if (b.size() + 1 > (MAX_SIZE / 3 * 2)) /* larger than MAX_SIZE/1.5 ? */
      lexerror(ls, Lt.lexical_element_too_long, 0);
    if (b.size() + 1 > b.capacity())
    {
      long newsize = b.capacity() + (long) (b.capacity() >> 1);
      if (newsize < LUA_MINBUFFER)
        newsize = LUA_MINBUFFER;
      b.growTo(newsize);
    }
    b.append((byte) c);
  }

  private static void save_and_next(LexState ls)
  {
    save(ls, ls.current);
    next(ls);
  }

  public static void luaX_init(lua_State L)
  {
    int i;
    TString e = TString.luaS_createstr(L, Lt._ENV); /* create env name */
    assert novariant(e.tt) >= LUA_TSTRING;
    luaC_fix(L, e);                  /* never collect this name */
    for (i = 0; i < NUM_RESERVED; i++)
    {
      ByteView bytes = luaX_tokens[i];
      TString ts = TString.luaS_createstr(L, bytes);
      assert novariant(ts.tt) >= LUA_TSTRING;
      luaC_fix(L, ts);     /* reserved words are never collected */
      ts.extra = (byte) (i + 1); /* reserved word */
    }
  }

  public static ByteView luaX_token2str(LexState ls, int token)
  {
    if (token < FIRST_RESERVED)
    { /* single-byte symbols? */
      if (lisprint(token) != 0)
        return luaO_pushfstring(ls.L, Lt.fmt_quoted_c, new FmtArgs().chr(token)).view();
      else /* control character */
        return luaO_pushfstring(ls.L, Lt.d, new FmtArgs().num(token)).view();
    }
    else
    {
      ByteView s = luaX_tokens[token - FIRST_RESERVED];
      if (token < TK_EOS) /* fixed format (symbols and reserved words)? */
        return luaO_pushfstring(ls.L, Lt.fmt_quoted_s, new FmtArgs().str(s)).view();
      else /* names, strings, and numerals */
        return s;
    }
  }

  private static ByteView txtToken(LexState ls, int token)
  {
    switch (token)
    {
    case TK_NAME:
    case TK_STRING:
    case TK_FLT:
    case TK_INT:
      return luaO_pushfstring(ls.L, Lt.fmt_quoted_s, new FmtArgs().str(luaX_newstring(ls, ls.buff.view(), 0, ls.buff.view().size()).data.view())).view();
    default:
      return luaX_token2str(ls, token);
    }
  }

  private static RuntimeException lexerror(LexState ls, ByteView msg, int token)
  {
    msg = luaG_addinfo(ls.L, msg, ls.source, ls.linenumber);
    if (token != 0)
      luaO_pushfstring(ls.L, Lt.fmt_s_near_s, new FmtArgs().str(msg).str(txtToken(ls, token)));
    throw luaD_throw(ls.L, (byte) LUA_ERRSYNTAX);
  }

  public static RuntimeException luaX_syntaxerror(LexState ls, ByteView msg)
  {
    throw lexerror(ls, msg, ls.t.token);
  }

  /*
  ** Anchors a string in scanner's table so that it will not be collected
  ** until the end of the compilation; by that time it should be anchored
  ** somewhere. It also internalizes long strings, ensuring there is only
  ** one copy of each unique string.
  */
  private static TString anchorstr(LexState ls, TString ts)
  {
    lua_State L = ls.L;
    TValue oldts = new TValue();
    int tag = ls.h.luaH_getstr(ts, oldts);
    if (!tagisempty(tag))     /* string already present? */
    {
      assert ttisstring(oldts);
      return tsvalue(oldts); /* use stored value */
    }
    else
    {                                /* create a new entry */
      int stv = L.top++; /* reserve stack space for string */
      setslots(L, stv, ts);         /* push (anchor) the string on the stack */
      TValue sv = getslot(L, stv);
      ls.h.luaH_set(L, sv, sv);  /* t[string] = string */
      /* table is not a metatable, so it does not need to invalidate cache */
      luaC_checkGC(L);
      L.top--; /* remove string from stack */
      return ts;
    }
  }

  /*
  ** Creates a new string and anchors it in scanner's table.
  */
  public static TString luaX_newstring(LexState ls, ByteView str, int off, int len)
  {
    return anchorstr(ls, TString.luaS_createstr(ls.L, str.substr(off, len)));
  }

  /*
  ** increment line number and skips newline sequence (any of
  ** \n, \r, \n\r, or \r\n)
  */
  private static void inclinenumber(LexState ls)
  {
    InclineFrame l = new InclineFrame();
    if (ls.z.resuming)
      l = new InclineFrame((InclineFrame) luaZ_restore(ls.z.rz, LF_INCLINE));
    try
    {
      switch (l.site)
      {
      case 0:
        l.old = ls.current;
        assert currIsNewline(ls) != 0;
        l.site = 1;
      case 1:
        next(ls); /* skip '\n' or '\r' */
        if (!(currIsNewline(ls) != 0 && ls.current != l.old))
          break;
        l.site = 2;
      case 2:
        next(ls); /* skip '\n\r' or '\r\n' */
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_INCLINE, l);
      throw e;
    }
    if (++ls.linenumber >= Integer.MAX_VALUE)
      lexerror(ls, Lt.chunk_has_too_many_lines, 0);
  }

  public static void luaX_setinput(lua_State L, LexState ls, ZIO z, TString source, int firstchar)
  {
    ls.t.token = 0;
    ls.L = L;
    ls.current = firstchar;
    ls.lookahead.token = TK_EOS; /* no look-ahead token */
    ls.z = z;
    ls.fs = null;
    ls.linenumber = 1;
    ls.lastline = 1;
    ls.source = source;
    /* all three strings here ("_ENV", "break", "global") were fixed,
       so they cannot be collected */
    ls.envn = TString.luaS_createstr(L, Lt._ENV); /* get env string */
    ByteView breakstr = Lt.break_kw;
    ls.brkn = TString.luaS_createstr(L, breakstr); /* get "break" string */
    if (!LUA_DEBUG)
    {
      /* compatibility mode: "global" is not a reserved word */
      ls.glbn = TString.luaS_createstr(L, Lt.global); /* get "global" string */
      ls.glbn.extra = 0;                               /* mark it as not reserved */
    }
    /* initialize buffer */
    ls.buff.reset();
  }

  /*
  ** =======================================================
  ** LEXICAL ANALYZER
  ** =======================================================
  */

  /* (with no frame of its own: made again, it goes the same way, see lload.h) */
  private static int check_next1(LexState ls, int c)
  {
    if (ls.current == c)
    {
      next(ls);
      return 1;
    }
    else
      return 0;
  }

  /* LUA_NUMBER */
  /*
  ** This function is quite liberal in what it accepts, as 'luaO_str2num'
  ** will reject ill-formed numerals. Roughly, it accepts the following
  ** pattern:
  **
  **   %d(%x|%.|([Ee][+-]?))* | 0[Xx](%x|%.|([Pp][+-]?))*
  **
  ** The only tricky part is to accept [+-] only after a valid exponent
  ** mark, to avoid reading '3-4' or '0xe+1' as a single number.
  **
  ** The caller might have already read an initial dot.
  */
  private static int read_numeral(LexState ls, Token seminfo)
  {
    NumeralFrame l = new NumeralFrame();
    if (ls.z.resuming)
      l = new NumeralFrame((NumeralFrame) luaZ_restore(ls.z.rz, LF_NUMERAL));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.first = ls.current;
          assert lisdigit(ls.current) != 0;
          save(ls, ls.current);
          l.site = 1;
        case 1:
          next(ls);
          if (l.first == '0' && (ls.current == 'x' || ls.current == 'X')) /* hexadecimal? */
          {
            save(ls, ls.current);
            l.site = 2;
            continue;
          }
          l.site = 3;
          continue;
        case 2:
          next(ls);
          l.hex = 1;
          l.site = 3;
        case 3: /* the loop */
        {
          ByteView expo = l.hex != 0 ? Lt.Pp_chars : Lt.Ee_chars;
          if (ls.current == expo.at(0) || ls.current == expo.at(1)) /* exponent mark? */
          {
            save(ls, ls.current);
            l.site = 4;
          }
          else if (lisxdigit(ls.current) != 0 || ls.current == '.') /* '%x|%.' */
          {
            save(ls, ls.current);
            l.site = 5;
          }
          else if (lislalpha(ls.current) != 0) /* is numeral touching a letter? */
          {
            save(ls, ls.current); /* force an error */
            l.site = 6;
          }
          else
            l.site = 7;
          continue;
        }
        case 4:
          next(ls);
          if (ls.current == '-' || ls.current == '+') /* optional exponent sign */
          {
            save(ls, ls.current);
            l.site = 5;
          }
          else
            l.site = 3;
          continue;
        case 5:
          next(ls);
          l.site = 3;
          continue;
        case 6:
          next(ls);
          l.site = 7;
        case 7:
        {
          TValue obj = new TValue();
          if (luaO_str2num(ls.buff.view(), obj) == 0) /* format error? */
            lexerror(ls, Lt.malformed_number, TK_FLT);
          if (ttisinteger(obj))
          {
            seminfo.i = ivalue(obj);
            return TK_INT;
          }
          else
          {
            assert ttisfloat(obj);
            seminfo.r = fltvalue(obj);
            return TK_FLT;
          }
        }
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_NUMERAL, l);
      throw e;
    }
  }

  /*
  ** read a sequence '[=*[' or ']=*]', leaving the last bracket. If
  ** sequence is well formed, return its number of '='s + 2; otherwise,
  ** return 1 if it is a single bracket (no '='s and no 2nd bracket);
  ** otherwise (an unfinished '[==...') return 0.
  */
  private static int skip_sep(LexState ls)
  {
    SepFrame l = new SepFrame();
    if (ls.z.resuming)
      l = new SepFrame((SepFrame) luaZ_restore(ls.z.rz, LF_SKIPSEP));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.s = ls.current;
          assert l.s == '[' || l.s == ']';
          save(ls, ls.current);
          l.site = 1;
        case 1:
          next(ls);
          l.site = 2;
        case 2: /* the loop */
          if (ls.current != '=')
            return (ls.current == l.s) ? l.count + 2
                   : (l.count == 0)     ? 1
                                        : 0;
          save(ls, ls.current);
          l.site = 3;
        case 3:
          next(ls);
          l.count++;
          l.site = 2;
          continue;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_SKIPSEP, l);
      throw e;
    }
  }

  private static void read_long_string(LexState ls, Token seminfo, int sep)
  {
    LongStringFrame l = new LongStringFrame();
    if (ls.z.resuming)
      l = new LongStringFrame((LongStringFrame) luaZ_restore(ls.z.rz, LF_LONGSTRING));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.line = ls.linenumber;
          save(ls, ls.current);
          l.site = 1;
        case 1:
          next(ls);                             /* skip 2nd '[' */
          l.site = currIsNewline(ls) != 0 ? 2 : 3; /* string starts with a newline? */
          continue;
        case 2:
          inclinenumber(ls); /* skip it */
          l.site = 3;
        case 3: /* the loop */
          switch (ls.current)
          {
          case EOZ:
          { /* error */
            ByteView what = seminfo != null ? Lt.string_3 : Lt.comment;
            InternalString msg = luaO_pushfstring(ls.L, Lt.unfinished_long_s_starting_at_line_d, new FmtArgs().str(what).num(l.line));
            lexerror(ls, msg.view(), TK_EOS);
            break; /* to avoid warnings */
          }
          case ']':
            l.site = 4;
            break;
          case '\n':
          case '\r':
            save(ls, '\n');
            l.site = 6;
            break;
          default:
            if (seminfo != null)
              save(ls, ls.current);
            l.site = 7;
          }
          continue;
        case 4:
          l.r = skip_sep(ls);
          if (l.r != sep)
          {
            l.site = 3;
            continue;
          }
          save(ls, ls.current);
          l.site = 5;
        case 5:
          next(ls); /* skip 2nd ']' */
          if (seminfo != null)
            seminfo.ts = luaX_newstring(ls, ls.buff.view(), sep,
                                        ls.buff.size() - 2 * sep);
          return;
        case 6:
          inclinenumber(ls);
          if (seminfo == null)
            ls.buff.reset(); /* avoid wasting space */
          l.site = 3;
          continue;
        case 7:
          next(ls);
          l.site = 3;
          continue;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_LONGSTRING, l);
      throw e;
    }
  }

  private static void esccheck(LexState ls, int c, ByteView msg)
  {
    LoadFrame l = new LoadFrame();
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_ESCCHECK));
    try
    {
      switch (l.site)
      {
      case 0:
        if (c != 0)
          return;
        if (ls.current == EOZ)
          break;
        save(ls, ls.current); /* add current to buffer for error message */
        l.site = 1;
      case 1:
        next(ls);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_ESCCHECK, l);
      throw e;
    }
    lexerror(ls, msg, TK_STRING);
  }

  private static int gethexa(LexState ls)
  {
    LoadFrame l = new LoadFrame();
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_GETHEXA));
    try
    {
      switch (l.site)
      {
      case 0:
        save(ls, ls.current);
        l.site = 1;
      case 1:
        next(ls);
        l.site = 2;
      case 2:
        esccheck(ls, lisxdigit(ls.current), Lt.hexadecimal_digit_expected);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_GETHEXA, l);
      throw e;
    }
    return luaO_hexavalue(ls.current);
  }

  private static int readhexaesc(LexState ls)
  {
    HexaEscFrame l = new HexaEscFrame();
    if (ls.z.resuming)
      l = new HexaEscFrame((HexaEscFrame) luaZ_restore(ls.z.rz, LF_HEXAESC));
    try
    {
      switch (l.site)
      {
      case 0:
        l.r = gethexa(ls);
        l.site = 1;
      case 1:
        l.r = (l.r << 4) + gethexa(ls);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_HEXAESC, l);
      throw e;
    }
    ls.buff.drop(2); /* remove saved chars from buffer */
    return l.r;
  }

  /*
  ** When reading a UTF-8 escape sequence, save everything to the buffer
  ** for error reporting in case of errors; 'i' counts the number of
  ** saved characters, so that they can be removed if case of success.
  */
  private static int readutf8esc(LexState ls)
  {
    Utf8EscFrame l = new Utf8EscFrame();
    if (ls.z.resuming)
      l = new Utf8EscFrame((Utf8EscFrame) luaZ_restore(ls.z.rz, LF_UTF8ESC));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          l.i = 4;
          save(ls, ls.current);
          l.site = 1;
        case 1:
          next(ls); /* skip 'u' */
          l.site = 2;
        case 2:
          esccheck(ls, (ls.current == '{') ? 1 : 0, Lt.missing_1);
          l.site = 3;
        case 3:
          l.r = gethexa(ls); /* must have at least one digit */
          l.site = 4;
        case 4: /* the loop */
          save(ls, ls.current);
          l.site = 5;
        case 5:
          next(ls);
          if (lisxdigit(ls.current) == 0)
          {
            l.site = 7;
            continue;
          }
          l.i++;
          l.site = 6;
        case 6:
          esccheck(ls, (l.r <= (0x7FFFFFFF >> 4)) ? 1 : 0, Lt.UTF_8_value_too_large);
          l.r = (l.r << 4) + luaO_hexavalue(ls.current);
          l.site = 4;
          continue;
        case 7:
          esccheck(ls, (ls.current == '}') ? 1 : 0, Lt.missing_2);
          l.site = 8;
        case 8:
          next(ls);            /* skip '}' */
          ls.buff.drop(l.i); /* remove saved chars from buffer */
          return l.r;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_UTF8ESC, l);
      throw e;
    }
  }

  private static void utf8esc(LexState ls)
  {
    LoadFrame l = new LoadFrame();
    ByteOwner buff = new ByteOwner(UTF8BUFFSZ);
    int c = 0;
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_UTF8));
    try
    {
      switch (l.site)
      {
      case 0:
        c = readutf8esc(ls);
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_UTF8, l);
      throw e;
    }
    int n = luaO_utf8esc(buff.span(), c);
    for (; n > 0; n--) /* add 'buff' to string */
      save(ls, buff.get(UTF8BUFFSZ - n));
  }

  private static int readdecesc(LexState ls)
  {
    DecEscFrame l = new DecEscFrame();
    if (ls.z.resuming)
      l = new DecEscFrame((DecEscFrame) luaZ_restore(ls.z.rz, LF_DECESC));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0: /* the loop: read up to 3 digits */
          if (l.i < 3 && lisdigit(ls.current) != 0)
          {
            l.r = 10 * l.r + ls.current - '0';
            save(ls, ls.current);
            l.site = 1;
          }
          else
            l.site = 2;
          continue;
        case 1:
          next(ls);
          l.i++;
          l.site = 0;
          continue;
        case 2:
          esccheck(ls, (l.r <= 0xFF) ? 1 : 0, Lt.decimal_escape_too_large);
          ls.buff.drop(l.i); /* remove read digits from buffer */
          return l.r;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_DECESC, l);
      throw e;
    }
  }

  private static void read_string(LexState ls, int del, Token seminfo)
  {
    StringFrame l = new StringFrame();
    if (ls.z.resuming)
      l = new StringFrame((StringFrame) luaZ_restore(ls.z.rz, LF_STRING));
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          save(ls, ls.current); /* keep delimiter (for error messages) */
          l.site = 1;
        case 1:
          next(ls);
          l.site = 2;
        case 2: /* the loop */
          if (ls.current == del)
          {
            save(ls, ls.current);
            l.site = 14;
            continue;
          }
          switch (ls.current)
          {
          case EOZ:
            lexerror(ls, Lt.unfinished_string, TK_EOS);
            break; /* to avoid warnings */
          case '\n':
          case '\r':
            lexerror(ls, Lt.unfinished_string, TK_STRING);
            break; /* to avoid warnings */
          case '\\': /* escape sequences */
            save(ls, ls.current);
            l.site = 3;
            break;
          default:
            save(ls, ls.current);
            l.site = 1;
          }
          continue;
        case 3:
          next(ls); /* keep '\\' for error messages */
          switch (ls.current)
          {
          case 'a':
            l.c = '\u0007';
            l.site = 12;
            break;
          case 'b':
            l.c = '\b';
            l.site = 12;
            break;
          case 'f':
            l.c = '\f';
            l.site = 12;
            break;
          case 'n':
            l.c = '\n';
            l.site = 12;
            break;
          case 'r':
            l.c = '\r';
            l.site = 12;
            break;
          case 't':
            l.c = '\t';
            l.site = 12;
            break;
          case 'v':
            l.c = '\u000B';
            l.site = 12;
            break;
          case 'x':
            l.site = 4;
            break;
          case 'u':
            l.site = 5;
            break;
          case '\n':
          case '\r':
            l.site = 6;
            break;
          case '\\':
          case '\"':
          case '\'':
            l.c = ls.current;
            l.site = 12;
            break;
          case EOZ:
            l.site = 2; /* will raise an error next loop */
            break;
          case 'z':            /* zap following span of spaces */
            ls.buff.drop(1); /* remove '\\' */
            l.site = 7;
            break;
          default:
            l.site = 10;
          }
          continue;
        case 4:
          l.c = readhexaesc(ls);
          l.site = 12;
          continue;
        case 5:
          utf8esc(ls);
          l.site = 2;
          continue;
        case 6:
          inclinenumber(ls);
          l.c = '\n';
          l.site = 13;
          continue;
        case 7:
          next(ls); /* skip the 'z', or a space */
          l.site = 8;
        case 8: /* the span of spaces */
          if (lisspace(ls.current) == 0)
            l.site = 2;
          else if (currIsNewline(ls) != 0)
            l.site = 9;
          else
            l.site = 7;
          continue;
        case 9:
          inclinenumber(ls);
          l.site = 8;
          continue;
        case 10:
          esccheck(ls, lisdigit(ls.current), Lt.invalid_escape_sequence);
          l.site = 11;
        case 11:
          l.c = readdecesc(ls); /* digital escape '\ddd' */
          l.site = 13;
          continue;
        case 12: /* read and save */
          next(ls);
          l.site = 13;
        case 13:               /* only save */
          ls.buff.drop(1); /* remove '\\' */
          save(ls, l.c);
          l.site = 2;
          continue;
        case 14:
          next(ls); /* skip delimiter */
          seminfo.ts = luaX_newstring(ls, ls.buff.view(), 1,
                                      ls.buff.size() - 2);
          return;
        default:
          luaZ_badstate(ls.z.rz);
        }
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_STRING, l);
      throw e;
    }
  }

  /*
  ** Reads a token. Each one is a step of the budget of a load that a freeze
  ** can stop, and where the budget is spent, a check point (the leaf of the
  ** frames, see lload.h).
  */
  private static int llex(LexState ls, Token seminfo)
  {
    LexFrame l = new LexFrame();
    LoadResume rz = ls.z.rz;
    if (ls.z.resuming)
    {
      l = new LexFrame((LexFrame) luaZ_restore(rz, LF_LLEX));
      if (l.site == 1)
      { /* (the check point, the leaf of the frames: behind it now) */
        luaZ_resumed(rz);
        l.site = 2;
      }
    }
    try
    {
      for (;;)
      {
        switch (l.site)
        {
        case 0:
          if (rz != null && luaZ_checkdue(rz, ls.L))
          {
            l.site = 1;
            luaZ_checkpoint(rz);
          }
          l.site = 2;
        case 2:
          ls.buff.reset();
          l.site = 3;
        case 3: /* the loop */
          switch (ls.current)
          {
          case '\n':
          case '\r': /* line breaks */
            l.site = 4;
            break;
          case ' ':
          case '\f':
          case '\t':
          case '\u000B': /* spaces */
            l.site = 5;
            break;
          case '-': /* '-' or '--' (comment) */
            l.site = 6;
            break;
          case '[': /* long string or simply '[' */
            l.site = 12;
            break;
          case '=':
            l.site = 14;
            break;
          case '<':
            l.site = 16;
            break;
          case '>':
            l.site = 19;
            break;
          case '/':
            l.site = 22;
            break;
          case '~':
            l.site = 24;
            break;
          case ':':
            l.site = 26;
            break;
          case '"':
          case '\'': /* short literal strings */
            l.c = ls.current;
            l.site = 28;
            break;
          case '.': /* '.', '..', '...', or number */
            save(ls, ls.current);
            l.site = 29;
            break;
          case '0':
          case '1':
          case '2':
          case '3':
          case '4':
          case '5':
          case '6':
          case '7':
          case '8':
          case '9':
            l.site = 32;
            break;
          case EOZ:
            return TK_EOS;
          default:
            if (lislalpha(ls.current) != 0) /* identifier or reserved word? */
              l.site = 33;
            else
            { /* single-char tokens ('+', '*', '%', '{', '}', ...) */
              l.c = ls.current;
              l.site = 35;
            }
          }
          continue;
        case 4:
          inclinenumber(ls);
          l.site = 3;
          continue;
        case 5:
          next(ls);
          l.site = 3;
          continue;
        case 6:
          next(ls);
          if (ls.current != '-')
            return '-';
          /* else is a comment */
          l.site = 7;
        case 7:
          next(ls);
          if (ls.current != '[') /* not a long comment? */
          {
            l.site = 10;
            continue;
          }
          l.site = 8;
        case 8:
          l.sep = skip_sep(ls);
          ls.buff.reset(); /* 'skip_sep' may dirty the buffer */
          if (l.sep < 2)
          {
            l.site = 10;
            continue;
          }
          l.site = 9;
        case 9:
          read_long_string(ls, null, l.sep); /* skip long comment */
          ls.buff.reset();                    /* previous call may dirty the buff. */
          l.site = 3;
          continue;
        case 10: /* short comment */
          if (currIsNewline(ls) != 0 || ls.current == EOZ)
          {
            l.site = 3;
            continue;
          }
          l.site = 11;
        case 11:
          next(ls); /* skip until end of line (or end of file) */
          l.site = 10;
          continue;
        case 12:
          l.sep = skip_sep(ls);
          if (l.sep < 2)
          {
            if (l.sep == 0) /* '[=...' missing second bracket? */
              lexerror(ls, Lt.invalid_long_string_delimiter, TK_STRING);
            return '[';
          }
          l.site = 13;
        case 13:
          read_long_string(ls, seminfo, l.sep);
          return TK_STRING;
        case 14:
          next(ls);
          l.site = 15;
        case 15:
          if (check_next1(ls, '=') != 0)
            return TK_EQ; /* '==' */
          else
            return '=';
        case 16:
          next(ls);
          l.site = 17;
        case 17:
          if (check_next1(ls, '=') != 0)
            return TK_LE; /* '<=' */
          l.site = 18;
        case 18:
          if (check_next1(ls, '<') != 0)
            return TK_SHL; /* '<<' */
          else
            return '<';
        case 19:
          next(ls);
          l.site = 20;
        case 20:
          if (check_next1(ls, '=') != 0)
            return TK_GE; /* '>=' */
          l.site = 21;
        case 21:
          if (check_next1(ls, '>') != 0)
            return TK_SHR; /* '>>' */
          else
            return '>';
        case 22:
          next(ls);
          l.site = 23;
        case 23:
          if (check_next1(ls, '/') != 0)
            return TK_IDIV; /* '//' */
          else
            return '/';
        case 24:
          next(ls);
          l.site = 25;
        case 25:
          if (check_next1(ls, '=') != 0)
            return TK_NE; /* '~=' */
          else
            return '~';
        case 26:
          next(ls);
          l.site = 27;
        case 27:
          if (check_next1(ls, ':') != 0)
            return TK_DBCOLON; /* '::' */
          else
            return ':';
        case 28:
          read_string(ls, l.c, seminfo);
          return TK_STRING;
        case 29:
          next(ls);
          l.site = 30;
        case 30:
          if (check_next1(ls, '.') != 0)
          {
            l.site = 31;
            continue;
          }
          else if (lisdigit(ls.current) == 0)
            return '.';
          l.site = 32;
          continue;
        case 31:
          if (check_next1(ls, '.') != 0)
            return TK_DOTS; /* '...' */
          else
            return TK_CONCAT; /* '..' */
        case 32:
          return read_numeral(ls, seminfo);
        case 33: /* identifier or reserved word */
          save(ls, ls.current);
          l.site = 34;
        case 34:
        {
          next(ls);
          if (lislalnum(ls.current) != 0)
          {
            l.site = 33;
            continue;
          }
          /* find or create string */
          TString ts = TString.luaS_createstr(ls.L, ls.buff.view());
          if (isreserved(ts) != 0) /* reserved word? */
            return ts.extra - 1 + FIRST_RESERVED;
          else
          {
            seminfo.ts = anchorstr(ls, ts);
            return TK_NAME;
          }
        }
        case 35:
          next(ls);
          return l.c;
        default:
          luaZ_badstate(rz);
        }
      }
    }
    catch (Throwable e)
    {
      luaZ_record(rz, LF_LLEX, l);
      throw e;
    }
  }

  public static void luaX_next(LexState ls)
  {
    LoadFrame l = new LoadFrame();
    if (ls.z.resuming)
      l = new LoadFrame(luaZ_restore(ls.z.rz, LF_NEXT));
    try
    {
      switch (l.site)
      {
      case 0:
        ls.lastline = ls.linenumber;
        if (ls.lookahead.token != TK_EOS)
        {                               /* is there a look-ahead token? */
          ls.t = new Token(ls.lookahead); /* use this one */
          ls.lookahead.token = TK_EOS; /* and discharge it */
          break;
        }
        l.site = 1;
      case 1:
        ls.t.token = llex(ls, ls.t); /* read next token */
        break;
      default:
        luaZ_badstate(ls.z.rz);
      }
    }
    catch (Throwable e)
    {
      luaZ_record(ls.z.rz, LF_NEXT, l);
      throw e;
    }
  }

  public static int luaX_lookahead(LexState ls)
  {
    assert ls.lookahead.token == TK_EOS;
    ls.lookahead.token = llex(ls, ls.lookahead); /* (no frame of its own: see 'check_next1') */
    return ls.lookahead.token;
  }
}
