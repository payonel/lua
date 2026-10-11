/*
** $Id: llex.c $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/


#include <string.h>

#include "lua.h"

#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"
#include "lload.h"

#include "lhack.h"
#include "lauxlib.h"
#include "literals.h"

namespace lua::compiler
{

/*
** A load that a freeze can stop (see lload.h): a call at site 'n' of a
** function's frame records the frame (its locals, 'l') if a freeze passes
** through it; the function made again goes to the site of its frame.
*/
#define LZ_SITE(n, ...)                            \
  site_##n:                                        \
  try                                              \
  {                                                \
    __VA_ARGS__;                                   \
  }                                                \
  catch (...)                                      \
  {                                                \
    luaZ_record(ls->z->rz, FN, n, &l, sizeof(l));  \
    throw;                                         \
  }

#define LZ_RESUMING() l_unlikely(ls->z->resuming)
#define LZ_SITENO() luaZ_restore(ls->z->rz, FN, &l, sizeof(l))
#define LZ_BADSITE() luaZ_badstate(ls->z->rz)

struct NoLocals
{
  char none = 0;
};

static l_forceinline void next(LexState* ls)
{
  ls->current = (ls->z->pos < ls->z->chunk.size())
                    ? (unsigned char)ls->z->chunk[ls->z->pos++]
                    : luaZ_fill(ls->z);
}

/* minimum size for string buffer */
constexpr int LUA_MINBUFFER = 32;

static inline int currIsNewline(const LexState* ls)
{
  return ls->current == '\n' || ls->current == '\r';
}

/* ORDER RESERVED */
static const byte_view luaX_tokens[] = {
    lt::and_kw, lt::break_kw, lt::do_kw, lt::else_kw, lt::elseif,
    lt::end, lt::false_kw, lt::for_kw, lt::function, lt::global, lt::goto_kw, lt::if_kw,
    lt::in, lt::local, lt::nil, lt::not_kw, lt::or_kw, lt::repeat,
    lt::return_kw, lt::then, lt::true_kw, lt::until, lt::while_kw,
    lt::idiv_op, lt::dotdot, lt::ellipsis, lt::eq_op, lt::ge_op, lt::le_op, lt::ne_op,
    lt::shl_op, lt::shr_op, lt::dcolon, lt::eof,
    lt::number_1, lt::integer_1, lt::name_1, lt::string_1};


[[noreturn]] static void lexerror(LexState* ls, byte_view msg, int token);

static void save(LexState* ls, int c)
{
  MBuffer* b = ls->buff;
  if (b->size() + 1 > (MAX_SIZE / 3 * 2)) /* larger than MAX_SIZE/1.5 ? */
    lexerror(ls, lt::lexical_element_too_long, 0);
  if (b->size() + 1 > b->capacity())
  {
    size_t newsize = b->capacity() + (b->capacity() >> 1);
    if (newsize < LUA_MINBUFFER)
      newsize = LUA_MINBUFFER;
    b->growTo(newsize);
  }
  b->append((char)c);
}

static inline void save_and_next(LexState* ls)
{
  save(ls, ls->current);
  next(ls);
}

void luaX_init(lua_State* L)
{
  int i;
  TString* e = TString::luaS_createstr(L, lt::_ENV); /* create env name */
  assert(novariant(e->tt) >= LUA_TSTRING);
  luaC_fix(L, e);                  /* never collect this name */
  for (i = 0; i < NUM_RESERVED; i++)
  {
    byte_view bytes = luaX_tokens[i];
    TString* ts = TString::luaS_createstr(L, bytes);
    assert(novariant(ts->tt) >= LUA_TSTRING);
    luaC_fix(L, ts);     /* reserved words are never collected */
    ts->extra = (lu_byte)(i + 1); /* reserved word */
  }
}

byte_view luaX_token2str(LexState* ls, int token)
{
  if (token < FIRST_RESERVED)
  { /* single-byte symbols? */
    if (lisprint(token))
      return luaO_pushfstring(ls->L, lt::fmt_quoted_c, FmtArgs().chr(token))->view();
    else /* control character */
      return luaO_pushfstring(ls->L, lt::d, FmtArgs().num(token))->view();
  }
  else
  {
    byte_view s = luaX_tokens[token - FIRST_RESERVED];
    if (token < TK_EOS) /* fixed format (symbols and reserved words)? */
      return luaO_pushfstring(ls->L, lt::fmt_quoted_s, FmtArgs().str(s))->view();
    else /* names, strings, and numerals */
      return s;
  }
}

static byte_view txtToken(LexState* ls, int token)
{
  switch (token)
  {
  case TK_NAME:
  case TK_STRING:
  case TK_FLT:
  case TK_INT:
    return luaO_pushfstring(ls->L, lt::fmt_quoted_s, FmtArgs().str(luaX_newstring(ls, ls->buff->view(), 0, ls->buff->view().size())->data.view()))->view();
  default:
    return luaX_token2str(ls, token);
  }
}

[[noreturn]] static void lexerror(LexState* ls, byte_view msg, int token)
{
  msg = luaG_addinfo(ls->L, msg, ls->source, ls->linenumber);
  if (token)
    luaO_pushfstring(ls->L, lt::fmt_s_near_s, FmtArgs().str(msg).str(txtToken(ls, token)));
  luaD_throw(ls->L, LUA_ERRSYNTAX);
}

[[noreturn]] void luaX_syntaxerror(LexState* ls, byte_view msg)
{
  lexerror(ls, msg, ls->t.token);
}

/*
** Anchors a string in scanner's table so that it will not be collected
** until the end of the compilation; by that time it should be anchored
** somewhere. It also internalizes long strings, ensuring there is only
** one copy of each unique string.
*/
static TString* anchorstr(LexState* ls, TString* ts)
{
  lua_State* L = ls->L;
  TValue oldts;
  int tag = ls->h->luaH_getstr(ts, &oldts);
  if (!tagisempty(tag))     /* string already present? */
  {
    assert(ttisstring(&oldts));
    return tsvalue(&oldts); /* use stored value */
  }
  else
  {                                /* create a new entry */
    StkId stv = L->top++; /* reserve stack space for string */
    setslots(L, stv, ts);         /* push (anchor) the string on the stack */
    TValue sv = getslot(L, stv);
    ls->h->luaH_set(L, &sv, &sv);  /* t[string] = string */
    /* table is not a metatable, so it does not need to invalidate cache */
    luaC_checkGC(L);
    L->top--; /* remove string from stack */
    return ts;
  }
}

/*
** Creates a new string and anchors it in scanner's table.
*/
TString* luaX_newstring(LexState* ls, byte_view str, size_t off, size_t len)
{
  return anchorstr(ls, TString::luaS_createstr(ls->L, str.substr(off, len)));
}

/*
** increment line number and skips newline sequence (any of
** \n, \r, \n\r, or \r\n)
*/
static void inclinenumber(LexState* ls)
{
  constexpr int FN = LF_INCLINE;
  struct
  {
    int old;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  l.old = ls->current;
  assert(currIsNewline(ls));
  LZ_SITE(1, next(ls)); /* skip '\n' or '\r' */
  if (currIsNewline(ls) && ls->current != l.old)
  {
    LZ_SITE(2, next(ls)); /* skip '\n\r' or '\r\n' */
  }
  if (++ls->linenumber >= INT_MAX)
    lexerror(ls, lt::chunk_has_too_many_lines, 0);
}

void luaX_setinput(lua_State* L, LexState* ls, ZIO* z, TString* source, int firstchar)
{
  ls->t.token = 0;
  ls->L = L;
  ls->current = firstchar;
  ls->lookahead.token = TK_EOS; /* no look-ahead token */
  ls->z = z;
  ls->fs = nullptr;
  ls->linenumber = 1;
  ls->lastline = 1;
  ls->source = source;
  /* all three strings here ("_ENV", "break", "global") were fixed,
     so they cannot be collected */
  ls->envn = TString::luaS_createstr(L, lt::_ENV); /* get env string */
  byte_view breakstr = lt::break_kw;
  ls->brkn = TString::luaS_createstr(L, breakstr); /* get "break" string */
  /* initialize buffer */
  ls->buff->reset();
}

/*
** =======================================================
** LEXICAL ANALYZER
** =======================================================
*/

/* (with no frame of its own: made again, it goes the same way, see lload.h) */
static int check_next1(LexState* ls, int c)
{
  if (ls->current == c)
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
static int read_numeral(LexState* ls, Token* seminfo)
{
  constexpr int FN = LF_NUMERAL;
  struct
  {
    int first;
    int hex; /* exponent mark 'Pp' (else 'Ee') */
  } l;
  TValue obj;
  byte_view expo;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    case 6: goto site_6;
    default: LZ_BADSITE();
    }
  }
  l.first = ls->current;
  assert(lisdigit(ls->current));
  save(ls, ls->current);
  LZ_SITE(1, next(ls));
  l.hex = 0;
  if (l.first == '0' && (ls->current == 'x' || ls->current == 'X')) /* hexadecimal? */
  {
    save(ls, ls->current);
    LZ_SITE(2, next(ls));
    l.hex = 1;
  }
  for (;;)
  {
    expo = l.hex ? lt::Pp_chars : lt::Ee_chars;
    if (ls->current == expo[0] || ls->current == expo[1]) /* exponent mark? */
    {
      save(ls, ls->current);
      LZ_SITE(3, next(ls));
      if (ls->current == '-' || ls->current == '+') /* optional exponent sign */
      {
        save(ls, ls->current);
        LZ_SITE(4, next(ls));
      }
    }
    else if (lisxdigit(ls->current) || ls->current == '.') /* '%x|%.' */
    {
      save(ls, ls->current);
      LZ_SITE(5, next(ls));
    }
    else
      break;
  }
  if (lislalpha(ls->current)) /* is numeral touching a letter? */
  {
    save(ls, ls->current); /* force an error */
    LZ_SITE(6, next(ls));
  }
  if (luaO_str2num(ls->buff->view(), &obj) == 0) /* format error? */
    lexerror(ls, lt::malformed_number, TK_FLT);
  if (ttisinteger(&obj))
  {
    seminfo->i = ivalue(&obj);
    return TK_INT;
  }
  else
  {
    assert(ttisfloat(&obj));
    seminfo->r = fltvalue(&obj);
    return TK_FLT;
  }
}

/*
** read a sequence '[=*[' or ']=*]', leaving the last bracket. If
** sequence is well formed, return its number of '='s + 2; otherwise,
** return 1 if it is a single bracket (no '='s and no 2nd bracket);
** otherwise (an unfinished '[==...') return 0.
*/
static size_t skip_sep(LexState* ls)
{
  constexpr int FN = LF_SKIPSEP;
  struct
  {
    size_t count;
    int s;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  l.count = 0;
  l.s = ls->current;
  assert(l.s == '[' || l.s == ']');
  save(ls, ls->current);
  LZ_SITE(1, next(ls));
  while (ls->current == '=')
  {
    save(ls, ls->current);
    LZ_SITE(2, next(ls));
    l.count++;
  }
  return (ls->current == l.s) ? l.count + 2
         : (l.count == 0)     ? 1
                              : 0;
}

static void read_long_string(LexState* ls, Token* seminfo, size_t sep)
{
  constexpr int FN = LF_LONGSTRING;
  struct
  {
    int line; /* initial line (for error message) */
    size_t r;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    case 6: goto site_6;
    case 7: goto site_7;
    default: LZ_BADSITE();
    }
  }
  l.line = ls->linenumber;
  save(ls, ls->current);
  LZ_SITE(1, next(ls));  /* skip 2nd '[' */
  if (currIsNewline(ls)) /* string starts with a newline? */
  {
    LZ_SITE(2, inclinenumber(ls)); /* skip it */
  }
  for (;;)
  {
    switch (ls->current)
    {
    case EOZ:
    { /* error */
      byte_view what = seminfo ? lt::string_3 : lt::comment;
      const InternalString* msg = luaO_pushfstring(ls->L, lt::unfinished_long_s_starting_at_line_d, FmtArgs().str(what).num(l.line));
      lexerror(ls, msg->view(), TK_EOS);
      break; /* to avoid warnings */
    }
    case ']':
    {
      LZ_SITE(3, l.r = skip_sep(ls));
      if (l.r == sep)
      {
        save(ls, ls->current);
        LZ_SITE(4, next(ls)); /* skip 2nd ']' */
        if (seminfo)
          seminfo->ts = luaX_newstring(ls, ls->buff->view(), sep,
                                       ls->buff->size() - 2 * sep);
        return;
      }
      break;
    }
    case '\n':
    case '\r':
    {
      save(ls, '\n');
      LZ_SITE(5, inclinenumber(ls));
      if (!seminfo)
        ls->buff->reset(); /* avoid wasting space */
      break;
    }
    default:
    {
      if (seminfo)
      {
        save(ls, ls->current);
        LZ_SITE(6, next(ls));
      }
      else
      {
        LZ_SITE(7, next(ls));
      }
    }
    }
  }
}

static void esccheck(LexState* ls, int c, byte_view msg)
{
  constexpr int FN = LF_ESCCHECK;
  NoLocals l;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  if (!c)
  {
    if (ls->current != EOZ)
    {
      save(ls, ls->current); /* add current to buffer for error message */
      LZ_SITE(1, next(ls));
    }
    lexerror(ls, msg, TK_STRING);
  }
}

static int gethexa(LexState* ls)
{
  constexpr int FN = LF_GETHEXA;
  NoLocals l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  save(ls, ls->current);
  LZ_SITE(1, next(ls));
  LZ_SITE(2, esccheck(ls, lisxdigit(ls->current), lt::hexadecimal_digit_expected));
  return luaO_hexavalue(ls->current);
}

static int readhexaesc(LexState* ls)
{
  constexpr int FN = LF_HEXAESC;
  struct
  {
    int r;
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  LZ_SITE(1, l.r = gethexa(ls));
  {
    int h;
    LZ_SITE(2, h = gethexa(ls));
    l.r = (l.r << 4) + h;
  }
  ls->buff->drop(2); /* remove saved chars from buffer */
  return l.r;
}

/*
** When reading a UTF-8 escape sequence, save everything to the buffer
** for error reporting in case of errors; 'i' counts the number of
** saved characters, so that they can be removed if case of success.
*/
static int readutf8esc(LexState* ls)
{
  constexpr int FN = LF_UTF8ESC;
  struct
  {
    int r;
    int i; /* number of chars to be removed: start with #"\u{X" */
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    case 6: goto site_6;
    case 7: goto site_7;
    default: LZ_BADSITE();
    }
  }
  l.i = 4;
  save(ls, ls->current);
  LZ_SITE(1, next(ls)); /* skip 'u' */
  LZ_SITE(2, esccheck(ls, ls->current == '{', lt::missing_1));
  LZ_SITE(3, l.r = gethexa(ls)); /* must have at least one digit */
  for (;;)
  {
    save(ls, ls->current);
    LZ_SITE(4, next(ls));
    if (!lisxdigit(ls->current))
      break;
    l.i++;
    LZ_SITE(5, esccheck(ls, l.r <= (0x7FFFFFFF >> 4), lt::UTF_8_value_too_large));
    l.r = (l.r << 4) + luaO_hexavalue(ls->current);
  }
  LZ_SITE(6, esccheck(ls, ls->current == '}', lt::missing_2));
  LZ_SITE(7, next(ls));  /* skip '}' */
  ls->buff->drop(l.i); /* remove saved chars from buffer */
  return l.r;
}

static void utf8esc(LexState* ls)
{
  constexpr int FN = LF_UTF8;
  NoLocals l;
  byte_inline<UTF8BUFFSZ> buff;
  int c;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  LZ_SITE(1, c = readutf8esc(ls));
  {
    int n = luaO_utf8esc(buff, c);
    for (; n > 0; n--) /* add 'buff' to string */
      save(ls, buff[UTF8BUFFSZ - n]);
  }
}

static int readdecesc(LexState* ls)
{
  constexpr int FN = LF_DECESC;
  struct
  {
    int i;
    int r; /* result accumulator */
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    default: LZ_BADSITE();
    }
  }
  l.r = 0;
  for (l.i = 0; l.i < 3 && lisdigit(ls->current); l.i++)
  { /* read up to 3 digits */
    l.r = 10 * l.r + ls->current - '0';
    save(ls, ls->current);
    LZ_SITE(1, next(ls));
  }
  LZ_SITE(2, esccheck(ls, l.r <= UCHAR_MAX, lt::decimal_escape_too_large));
  ls->buff->drop(l.i); /* remove read digits from buffer */
  return l.r;
}

static void read_string(LexState* ls, int del, Token* seminfo)
{
  constexpr int FN = LF_STRING;
  struct
  {
    int c; /* final character to be saved */
  } l;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1: goto site_1;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    case 6: goto site_6;
    case 7: goto site_7;
    case 8: goto site_8;
    case 9: goto site_9;
    case 10: goto site_10;
    case 11: goto site_11;
    case 12: goto site_12;
    case 13: goto site_13;
    default: LZ_BADSITE();
    }
  }
  save(ls, ls->current);
  LZ_SITE(1, next(ls)); /* keep delimiter (for error messages) */
  while (ls->current != del)
  {
    switch (ls->current)
    {
    case EOZ:
      lexerror(ls, lt::unfinished_string, TK_EOS);
      break; /* to avoid warnings */
    case '\n':
    case '\r':
      lexerror(ls, lt::unfinished_string, TK_STRING);
      break; /* to avoid warnings */
    case '\\':
    { /* escape sequences */
      save(ls, ls->current);
      LZ_SITE(2, next(ls)); /* keep '\\' for error messages */
      switch (ls->current)
      {
      case 'a':
        l.c = '\a';
        goto read_save;
      case 'b':
        l.c = '\b';
        goto read_save;
      case 'f':
        l.c = '\f';
        goto read_save;
      case 'n':
        l.c = '\n';
        goto read_save;
      case 'r':
        l.c = '\r';
        goto read_save;
      case 't':
        l.c = '\t';
        goto read_save;
      case 'v':
        l.c = '\v';
        goto read_save;
      case 'x':
        LZ_SITE(3, l.c = readhexaesc(ls));
        goto read_save;
      case 'u':
        LZ_SITE(4, utf8esc(ls));
        goto no_save;
      case '\n':
      case '\r':
        LZ_SITE(5, inclinenumber(ls));
        l.c = '\n';
        goto only_save;
      case '\\':
      case '\"':
      case '\'':
        l.c = ls->current;
        goto read_save;
      case EOZ:
        goto no_save; /* will raise an error next loop */
      case 'z':
      {                    /* zap following span of spaces */
        ls->buff->drop(1); /* remove '\\' */
        LZ_SITE(6, next(ls)); /* skip the 'z' */
        while (lisspace(ls->current))
        {
          if (currIsNewline(ls))
          {
            LZ_SITE(7, inclinenumber(ls));
          }
          else
          {
            LZ_SITE(8, next(ls));
          }
        }
        goto no_save;
      }
      default:
      {
        LZ_SITE(9, esccheck(ls, lisdigit(ls->current), lt::invalid_escape_sequence));
        LZ_SITE(10, l.c = readdecesc(ls)); /* digital escape '\ddd' */
        goto only_save;
      }
      }
    read_save:
      LZ_SITE(11, next(ls));
      /* go through */
    only_save:
      ls->buff->drop(1); /* remove '\\' */
      save(ls, l.c);
      /* go through */
    no_save:
      break;
    }
    default:
      save(ls, ls->current);
      LZ_SITE(12, next(ls));
    }
  }
  save(ls, ls->current);
  LZ_SITE(13, next(ls)); /* skip delimiter */
  seminfo->ts = luaX_newstring(ls, ls->buff->view(), 1,
                               ls->buff->size() - 2);
}

/*
** Reads a token. Each one is a step of the budget of a load that a freeze
** can stop, and where the budget is spent, a check point (the leaf of the
** frames, see lload.h).
*/
static int llex(LexState* ls, Token* seminfo)
{
  constexpr int FN = LF_LLEX;
  struct
  {
    size_t sep;
    int c;
    int r;
  } l;
  LoadResume* rz = ls->z->rz;
  if (LZ_RESUMING())
  {
    switch (LZ_SITENO())
    {
    case 1:
      luaZ_resumed(rz); /* (the check point is behind) */
      goto checked;
    case 2: goto site_2;
    case 3: goto site_3;
    case 4: goto site_4;
    case 5: goto site_5;
    case 6: goto site_6;
    case 7: goto site_7;
    case 8: goto site_8;
    case 9: goto site_9;
    case 10: goto site_10;
    case 11: goto site_11;
    case 12: goto site_12;
    case 13: goto site_13;
    case 14: goto site_14;
    case 15: goto site_15;
    case 16: goto site_16;
    case 17: goto site_17;
    case 18: goto site_18;
    case 19: goto site_19;
    case 20: goto site_20;
    case 21: goto site_21;
    case 22: goto site_22;
    case 23: goto site_23;
    case 24: goto site_24;
    case 25: goto site_25;
    case 26: goto site_26;
    case 27: goto site_27;
    case 28: goto site_28;
    case 29: goto site_29;
    case 30: goto site_30;
    case 31: goto site_31;
    case 32: goto site_32;
    default: LZ_BADSITE();
    }
  }
  if (l_unlikely(rz != nullptr) && luaZ_checkdue(rz, ls->L))
  {
    try
    {
      luaZ_checkpoint(rz);
    }
    catch (...)
    {
      luaZ_record(rz, FN, 1, &l, sizeof(l));
      throw;
    }
  }
checked:
  ls->buff->reset();
  for (;;)
  {
    switch (ls->current)
    {
    case '\n':
    case '\r':
    { /* line breaks */
      LZ_SITE(2, inclinenumber(ls));
      break;
    }
    case ' ':
    case '\f':
    case '\t':
    case '\v':
    { /* spaces */
      LZ_SITE(3, next(ls));
      break;
    }
    case '-':
    { /* '-' or '--' (comment) */
      LZ_SITE(4, next(ls));
      if (ls->current != '-')
        return '-';
      /* else is a comment */
      LZ_SITE(5, next(ls));
      if (ls->current == '[')
      { /* long comment? */
        LZ_SITE(6, l.sep = skip_sep(ls));
        ls->buff->reset(); /* 'skip_sep' may dirty the buffer */
        if (l.sep >= 2)
        {
          LZ_SITE(7, read_long_string(ls, nullptr, l.sep)); /* skip long comment */
          ls->buff->reset();      /* previous call may dirty the buff. */
          break;
        }
      }
      /* else short comment */
      while (!currIsNewline(ls) && ls->current != EOZ)
      {
        LZ_SITE(8, next(ls)); /* skip until end of line (or end of file) */
      }
      break;
    }
    case '[':
    { /* long string or simply '[' */
      LZ_SITE(9, l.sep = skip_sep(ls));
      if (l.sep >= 2)
      {
        LZ_SITE(10, read_long_string(ls, seminfo, l.sep));
        return TK_STRING;
      }
      else if (l.sep == 0) /* '[=...' missing second bracket? */
        lexerror(ls, lt::invalid_long_string_delimiter, TK_STRING);
      return '[';
    }
    case '=':
    {
      LZ_SITE(11, next(ls));
      LZ_SITE(12, l.r = check_next1(ls, '='));
      if (l.r)
        return TK_EQ; /* '==' */
      else
        return '=';
    }
    case '<':
    {
      LZ_SITE(13, next(ls));
      LZ_SITE(14, l.r = check_next1(ls, '='));
      if (l.r)
        return TK_LE; /* '<=' */
      LZ_SITE(15, l.r = check_next1(ls, '<'));
      if (l.r)
        return TK_SHL; /* '<<' */
      else
        return '<';
    }
    case '>':
    {
      LZ_SITE(16, next(ls));
      LZ_SITE(17, l.r = check_next1(ls, '='));
      if (l.r)
        return TK_GE; /* '>=' */
      LZ_SITE(18, l.r = check_next1(ls, '>'));
      if (l.r)
        return TK_SHR; /* '>>' */
      else
        return '>';
    }
    case '/':
    {
      LZ_SITE(19, next(ls));
      LZ_SITE(20, l.r = check_next1(ls, '/'));
      if (l.r)
        return TK_IDIV; /* '//' */
      else
        return '/';
    }
    case '~':
    {
      LZ_SITE(21, next(ls));
      LZ_SITE(22, l.r = check_next1(ls, '='));
      if (l.r)
        return TK_NE; /* '~=' */
      else
        return '~';
    }
    case ':':
    {
      LZ_SITE(23, next(ls));
      LZ_SITE(24, l.r = check_next1(ls, ':'));
      if (l.r)
        return TK_DBCOLON; /* '::' */
      else
        return ':';
    }
    case '"':
    case '\'':
    { /* short literal strings */
      l.c = ls->current;
      LZ_SITE(25, read_string(ls, l.c, seminfo));
      return TK_STRING;
    }
    case '.':
    { /* '.', '..', '...', or number */
      save(ls, ls->current);
      LZ_SITE(26, next(ls));
      LZ_SITE(27, l.r = check_next1(ls, '.'));
      if (l.r)
      {
        LZ_SITE(28, l.r = check_next1(ls, '.'));
        if (l.r)
          return TK_DOTS; /* '...' */
        else
          return TK_CONCAT; /* '..' */
      }
      else if (!lisdigit(ls->current))
        return '.';
      else
      {
        LZ_SITE(29, l.r = read_numeral(ls, seminfo));
        return l.r;
      }
    }
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
    {
      LZ_SITE(30, l.r = read_numeral(ls, seminfo));
      return l.r;
    }
    case EOZ:
    {
      return TK_EOS;
    }
    default:
    {
      if (lislalpha(ls->current))
      { /* identifier or reserved word? */
        TString* ts;
        do
        {
          save(ls, ls->current);
          LZ_SITE(31, next(ls));
        } while (lislalnum(ls->current));
        /* find or create string */
        ts = TString::luaS_createstr(ls->L, ls->buff->view());
        if (isreserved(ts)) /* reserved word? */
          return ts->extra - 1 + FIRST_RESERVED;
        else
        {
          seminfo->ts = anchorstr(ls, ts);
          return TK_NAME;
        }
      }
      else
      { /* single-char tokens ('+', '*', '%', '{', '}', ...) */
        l.c = ls->current;
        LZ_SITE(32, next(ls));
        return l.c;
      }
    }
    }
  }
}

void luaX_next(LexState* ls)
{
  constexpr int FN = LF_NEXT;
  NoLocals l;
  if (LZ_RESUMING())
  {
    if (LZ_SITENO() != 1)
      LZ_BADSITE();
    goto site_1;
  }
  ls->lastline = ls->linenumber;
  if (ls->lookahead.token != TK_EOS)
  {                               /* is there a look-ahead token? */
    ls->t = ls->lookahead;        /* use this one */
    ls->lookahead.token = TK_EOS; /* and discharge it */
  }
  else
  {
    LZ_SITE(1, ls->t.token = llex(ls, &ls->t)); /* read next token */
  }
}

int luaX_lookahead(LexState* ls)
{
  assert(ls->lookahead.token == TK_EOS);
  ls->lookahead.token = llex(ls, &ls->lookahead); /* (no frame of its own: see 'check_next1') */
  return ls->lookahead.token;
}

}
