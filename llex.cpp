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
** A load that a freeze can stop (see lload.h): a function with calls a
** freeze can pass through runs as a state machine, its state and its
** locals in its frame ('l'). Each such call is made at a state of its own,
** the one its function goes to again when it is made again after the
** thaw (the call made again, which goes on where it stopped); a freeze
** passing records the frame. A call with no frame of its own (as
** 'check_next1') goes the same way when made again.
*/

/* the frames of the lexer's functions (see lload.h) */
struct InclineFrame : LoadFrame
{
  int old = 0;
  LoadFrame* copy() const override { return new (std::nothrow) InclineFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct NumeralFrame : LoadFrame
{
  int first = 0;
  int hex = 0; /* exponent mark 'Pp' (else 'Ee') */
  LoadFrame* copy() const override { return new (std::nothrow) NumeralFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct SepFrame : LoadFrame
{
  size_t count = 0;
  int s = 0;
  LoadFrame* copy() const override { return new (std::nothrow) SepFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct LongStringFrame : LoadFrame
{
  int line = 0; /* initial line (for error message) */
  size_t r = 0;
  LoadFrame* copy() const override { return new (std::nothrow) LongStringFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct HexaEscFrame : LoadFrame
{
  int r = 0;
  LoadFrame* copy() const override { return new (std::nothrow) HexaEscFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct Utf8EscFrame : LoadFrame
{
  int r = 0;
  int i = 0; /* number of chars to be removed: start with #"\u{X" */
  LoadFrame* copy() const override { return new (std::nothrow) Utf8EscFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct DecEscFrame : LoadFrame
{
  int i = 0;
  int r = 0; /* result accumulator */
  LoadFrame* copy() const override { return new (std::nothrow) DecEscFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct StringFrame : LoadFrame
{
  int c = 0; /* final character to be saved */
  LoadFrame* copy() const override { return new (std::nothrow) StringFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

struct LexFrame : LoadFrame
{
  size_t sep = 0;
  int c = 0;
  LoadFrame* copy() const override { return new (std::nothrow) LexFrame(*this); }
  size_t size() const override { return sizeof(*this); }
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
  InclineFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const InclineFrame&>(luaZ_restore(ls->z->rz, LF_INCLINE));
  try
  {
    switch (l.site)
    {
    case 0:
      l.old = ls->current;
      assert(currIsNewline(ls));
      l.site = 1;
      [[fallthrough]];
    case 1:
      next(ls); /* skip '\n' or '\r' */
      if (!(currIsNewline(ls) && ls->current != l.old))
        break;
      l.site = 2;
      [[fallthrough]];
    case 2:
      next(ls); /* skip '\n\r' or '\r\n' */
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_INCLINE, l);
    throw;
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
  NumeralFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const NumeralFrame&>(luaZ_restore(ls->z->rz, LF_NUMERAL));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.first = ls->current;
        assert(lisdigit(ls->current));
        save(ls, ls->current);
        l.site = 1;
        [[fallthrough]];
      case 1:
        next(ls);
        if (l.first == '0' && (ls->current == 'x' || ls->current == 'X')) /* hexadecimal? */
        {
          save(ls, ls->current);
          l.site = 2;
          continue;
        }
        l.site = 3;
        continue;
      case 2:
        next(ls);
        l.hex = 1;
        l.site = 3;
        [[fallthrough]];
      case 3: /* the loop */
      {
        byte_view expo = l.hex ? lt::Pp_chars : lt::Ee_chars;
        if (ls->current == expo[0] || ls->current == expo[1]) /* exponent mark? */
        {
          save(ls, ls->current);
          l.site = 4;
        }
        else if (lisxdigit(ls->current) || ls->current == '.') /* '%x|%.' */
        {
          save(ls, ls->current);
          l.site = 5;
        }
        else if (lislalpha(ls->current)) /* is numeral touching a letter? */
        {
          save(ls, ls->current); /* force an error */
          l.site = 6;
        }
        else
          l.site = 7;
        continue;
      }
      case 4:
        next(ls);
        if (ls->current == '-' || ls->current == '+') /* optional exponent sign */
        {
          save(ls, ls->current);
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
        [[fallthrough]];
      case 7:
      {
        TValue obj;
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
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_NUMERAL, l);
    throw;
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
  SepFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const SepFrame&>(luaZ_restore(ls->z->rz, LF_SKIPSEP));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.s = ls->current;
        assert(l.s == '[' || l.s == ']');
        save(ls, ls->current);
        l.site = 1;
        [[fallthrough]];
      case 1:
        next(ls);
        l.site = 2;
        [[fallthrough]];
      case 2: /* the loop */
        if (ls->current != '=')
          return (ls->current == l.s) ? l.count + 2
                 : (l.count == 0)     ? 1
                                      : 0;
        save(ls, ls->current);
        l.site = 3;
        [[fallthrough]];
      case 3:
        next(ls);
        l.count++;
        l.site = 2;
        continue;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_SKIPSEP, l);
    throw;
  }
}

static void read_long_string(LexState* ls, Token* seminfo, size_t sep)
{
  LongStringFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const LongStringFrame&>(luaZ_restore(ls->z->rz, LF_LONGSTRING));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.line = ls->linenumber;
        save(ls, ls->current);
        l.site = 1;
        [[fallthrough]];
      case 1:
        next(ls);                             /* skip 2nd '[' */
        l.site = currIsNewline(ls) ? 2 : 3; /* string starts with a newline? */
        continue;
      case 2:
        inclinenumber(ls); /* skip it */
        l.site = 3;
        [[fallthrough]];
      case 3: /* the loop */
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
          l.site = 4;
          break;
        case '\n':
        case '\r':
          save(ls, '\n');
          l.site = 6;
          break;
        default:
          if (seminfo)
            save(ls, ls->current);
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
        save(ls, ls->current);
        l.site = 5;
        [[fallthrough]];
      case 5:
        next(ls); /* skip 2nd ']' */
        if (seminfo)
          seminfo->ts = luaX_newstring(ls, ls->buff->view(), sep,
                                       ls->buff->size() - 2 * sep);
        return;
      case 6:
        inclinenumber(ls);
        if (!seminfo)
          ls->buff->reset(); /* avoid wasting space */
        l.site = 3;
        continue;
      case 7:
        next(ls);
        l.site = 3;
        continue;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_LONGSTRING, l);
    throw;
  }
}

static void esccheck(LexState* ls, int c, byte_view msg)
{
  LoadFrame l;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_ESCCHECK);
  try
  {
    switch (l.site)
    {
    case 0:
      if (c)
        return;
      if (ls->current == EOZ)
        break;
      save(ls, ls->current); /* add current to buffer for error message */
      l.site = 1;
      [[fallthrough]];
    case 1:
      next(ls);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_ESCCHECK, l);
    throw;
  }
  lexerror(ls, msg, TK_STRING);
}

static int gethexa(LexState* ls)
{
  LoadFrame l;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_GETHEXA);
  try
  {
    switch (l.site)
    {
    case 0:
      save(ls, ls->current);
      l.site = 1;
      [[fallthrough]];
    case 1:
      next(ls);
      l.site = 2;
      [[fallthrough]];
    case 2:
      esccheck(ls, lisxdigit(ls->current), lt::hexadecimal_digit_expected);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_GETHEXA, l);
    throw;
  }
  return luaO_hexavalue(ls->current);
}

static int readhexaesc(LexState* ls)
{
  HexaEscFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const HexaEscFrame&>(luaZ_restore(ls->z->rz, LF_HEXAESC));
  try
  {
    switch (l.site)
    {
    case 0:
      l.r = gethexa(ls);
      l.site = 1;
      [[fallthrough]];
    case 1:
      l.r = (l.r << 4) + gethexa(ls);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_HEXAESC, l);
    throw;
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
  Utf8EscFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const Utf8EscFrame&>(luaZ_restore(ls->z->rz, LF_UTF8ESC));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.i = 4;
        save(ls, ls->current);
        l.site = 1;
        [[fallthrough]];
      case 1:
        next(ls); /* skip 'u' */
        l.site = 2;
        [[fallthrough]];
      case 2:
        esccheck(ls, ls->current == '{', lt::missing_1);
        l.site = 3;
        [[fallthrough]];
      case 3:
        l.r = gethexa(ls); /* must have at least one digit */
        l.site = 4;
        [[fallthrough]];
      case 4: /* the loop */
        save(ls, ls->current);
        l.site = 5;
        [[fallthrough]];
      case 5:
        next(ls);
        if (!lisxdigit(ls->current))
        {
          l.site = 7;
          continue;
        }
        l.i++;
        l.site = 6;
        [[fallthrough]];
      case 6:
        esccheck(ls, l.r <= (0x7FFFFFFF >> 4), lt::UTF_8_value_too_large);
        l.r = (l.r << 4) + luaO_hexavalue(ls->current);
        l.site = 4;
        continue;
      case 7:
        esccheck(ls, ls->current == '}', lt::missing_2);
        l.site = 8;
        [[fallthrough]];
      case 8:
        next(ls);            /* skip '}' */
        ls->buff->drop(l.i); /* remove saved chars from buffer */
        return l.r;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_UTF8ESC, l);
    throw;
  }
}

static void utf8esc(LexState* ls)
{
  LoadFrame l;
  byte_inline<UTF8BUFFSZ> buff;
  int c = 0;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_UTF8);
  try
  {
    switch (l.site)
    {
    case 0:
      c = readutf8esc(ls);
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_UTF8, l);
    throw;
  }
  int n = luaO_utf8esc(buff, c);
  for (; n > 0; n--) /* add 'buff' to string */
    save(ls, buff[UTF8BUFFSZ - n]);
}

static int readdecesc(LexState* ls)
{
  DecEscFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const DecEscFrame&>(luaZ_restore(ls->z->rz, LF_DECESC));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0: /* the loop: read up to 3 digits */
        if (l.i < 3 && lisdigit(ls->current))
        {
          l.r = 10 * l.r + ls->current - '0';
          save(ls, ls->current);
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
        esccheck(ls, l.r <= UCHAR_MAX, lt::decimal_escape_too_large);
        ls->buff->drop(l.i); /* remove read digits from buffer */
        return l.r;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_DECESC, l);
    throw;
  }
}

static void read_string(LexState* ls, int del, Token* seminfo)
{
  StringFrame l;
  if (l_unlikely(ls->z->resuming))
    l = static_cast<const StringFrame&>(luaZ_restore(ls->z->rz, LF_STRING));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        save(ls, ls->current); /* keep delimiter (for error messages) */
        l.site = 1;
        [[fallthrough]];
      case 1:
        next(ls);
        l.site = 2;
        [[fallthrough]];
      case 2: /* the loop */
        if (ls->current == del)
        {
          save(ls, ls->current);
          l.site = 14;
          continue;
        }
        switch (ls->current)
        {
        case EOZ:
          lexerror(ls, lt::unfinished_string, TK_EOS);
          break; /* to avoid warnings */
        case '\n':
        case '\r':
          lexerror(ls, lt::unfinished_string, TK_STRING);
          break; /* to avoid warnings */
        case '\\': /* escape sequences */
          save(ls, ls->current);
          l.site = 3;
          break;
        default:
          save(ls, ls->current);
          l.site = 1;
        }
        continue;
      case 3:
        next(ls); /* keep '\\' for error messages */
        switch (ls->current)
        {
        case 'a':
          l.c = '\a';
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
          l.c = '\v';
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
          l.c = ls->current;
          l.site = 12;
          break;
        case EOZ:
          l.site = 2; /* will raise an error next loop */
          break;
        case 'z':            /* zap following span of spaces */
          ls->buff->drop(1); /* remove '\\' */
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
        [[fallthrough]];
      case 8: /* the span of spaces */
        if (!lisspace(ls->current))
          l.site = 2;
        else if (currIsNewline(ls))
          l.site = 9;
        else
          l.site = 7;
        continue;
      case 9:
        inclinenumber(ls);
        l.site = 8;
        continue;
      case 10:
        esccheck(ls, lisdigit(ls->current), lt::invalid_escape_sequence);
        l.site = 11;
        [[fallthrough]];
      case 11:
        l.c = readdecesc(ls); /* digital escape '\ddd' */
        l.site = 13;
        continue;
      case 12: /* read and save */
        next(ls);
        l.site = 13;
        [[fallthrough]];
      case 13:               /* only save */
        ls->buff->drop(1); /* remove '\\' */
        save(ls, l.c);
        l.site = 2;
        continue;
      case 14:
        next(ls); /* skip delimiter */
        seminfo->ts = luaX_newstring(ls, ls->buff->view(), 1,
                                     ls->buff->size() - 2);
        return;
      default:
        luaZ_badstate(ls->z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_STRING, l);
    throw;
  }
}

/*
** Reads a token. Each one is a step of the budget of a load that a freeze
** can stop, and where the budget is spent, a check point (the leaf of the
** frames, see lload.h).
*/
static int llex(LexState* ls, Token* seminfo)
{
  LexFrame l;
  LoadResume* rz = ls->z->rz;
  if (l_unlikely(ls->z->resuming))
  {
    l = static_cast<const LexFrame&>(luaZ_restore(rz, LF_LLEX));
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
        if (l_unlikely(rz != nullptr) && luaZ_checkdue(rz, ls->L))
        {
          l.site = 1;
          luaZ_checkpoint(rz);
        }
        l.site = 2;
        [[fallthrough]];
      case 2:
        ls->buff->reset();
        l.site = 3;
        [[fallthrough]];
      case 3: /* the loop */
        switch (ls->current)
        {
        case '\n':
        case '\r': /* line breaks */
          l.site = 4;
          break;
        case ' ':
        case '\f':
        case '\t':
        case '\v': /* spaces */
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
          l.c = ls->current;
          l.site = 28;
          break;
        case '.': /* '.', '..', '...', or number */
          save(ls, ls->current);
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
          if (lislalpha(ls->current)) /* identifier or reserved word? */
            l.site = 33;
          else
          { /* single-char tokens ('+', '*', '%', '{', '}', ...) */
            l.c = ls->current;
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
        if (ls->current != '-')
          return '-';
        /* else is a comment */
        l.site = 7;
        [[fallthrough]];
      case 7:
        next(ls);
        if (ls->current != '[') /* not a long comment? */
        {
          l.site = 10;
          continue;
        }
        l.site = 8;
        [[fallthrough]];
      case 8:
        l.sep = skip_sep(ls);
        ls->buff->reset(); /* 'skip_sep' may dirty the buffer */
        if (l.sep < 2)
        {
          l.site = 10;
          continue;
        }
        l.site = 9;
        [[fallthrough]];
      case 9:
        read_long_string(ls, nullptr, l.sep); /* skip long comment */
        ls->buff->reset();                    /* previous call may dirty the buff. */
        l.site = 3;
        continue;
      case 10: /* short comment */
        if (currIsNewline(ls) || ls->current == EOZ)
        {
          l.site = 3;
          continue;
        }
        l.site = 11;
        [[fallthrough]];
      case 11:
        next(ls); /* skip until end of line (or end of file) */
        l.site = 10;
        continue;
      case 12:
        l.sep = skip_sep(ls);
        if (l.sep < 2)
        {
          if (l.sep == 0) /* '[=...' missing second bracket? */
            lexerror(ls, lt::invalid_long_string_delimiter, TK_STRING);
          return '[';
        }
        l.site = 13;
        [[fallthrough]];
      case 13:
        read_long_string(ls, seminfo, l.sep);
        return TK_STRING;
      case 14:
        next(ls);
        l.site = 15;
        [[fallthrough]];
      case 15:
        if (check_next1(ls, '='))
          return TK_EQ; /* '==' */
        else
          return '=';
      case 16:
        next(ls);
        l.site = 17;
        [[fallthrough]];
      case 17:
        if (check_next1(ls, '='))
          return TK_LE; /* '<=' */
        l.site = 18;
        [[fallthrough]];
      case 18:
        if (check_next1(ls, '<'))
          return TK_SHL; /* '<<' */
        else
          return '<';
      case 19:
        next(ls);
        l.site = 20;
        [[fallthrough]];
      case 20:
        if (check_next1(ls, '='))
          return TK_GE; /* '>=' */
        l.site = 21;
        [[fallthrough]];
      case 21:
        if (check_next1(ls, '>'))
          return TK_SHR; /* '>>' */
        else
          return '>';
      case 22:
        next(ls);
        l.site = 23;
        [[fallthrough]];
      case 23:
        if (check_next1(ls, '/'))
          return TK_IDIV; /* '//' */
        else
          return '/';
      case 24:
        next(ls);
        l.site = 25;
        [[fallthrough]];
      case 25:
        if (check_next1(ls, '='))
          return TK_NE; /* '~=' */
        else
          return '~';
      case 26:
        next(ls);
        l.site = 27;
        [[fallthrough]];
      case 27:
        if (check_next1(ls, ':'))
          return TK_DBCOLON; /* '::' */
        else
          return ':';
      case 28:
        read_string(ls, l.c, seminfo);
        return TK_STRING;
      case 29:
        next(ls);
        l.site = 30;
        [[fallthrough]];
      case 30:
        if (check_next1(ls, '.'))
        {
          l.site = 31;
          continue;
        }
        else if (!lisdigit(ls->current))
          return '.';
        l.site = 32;
        continue;
      case 31:
        if (check_next1(ls, '.'))
          return TK_DOTS; /* '...' */
        else
          return TK_CONCAT; /* '..' */
      case 32:
        return read_numeral(ls, seminfo);
      case 33: /* identifier or reserved word */
        save(ls, ls->current);
        l.site = 34;
        [[fallthrough]];
      case 34:
      {
        next(ls);
        if (lislalnum(ls->current))
        {
          l.site = 33;
          continue;
        }
        /* find or create string */
        TString* ts = TString::luaS_createstr(ls->L, ls->buff->view());
        if (isreserved(ts)) /* reserved word? */
          return ts->extra - 1 + FIRST_RESERVED;
        else
        {
          seminfo->ts = anchorstr(ls, ts);
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
  catch (...)
  {
    luaZ_record(rz, LF_LLEX, l);
    throw;
  }
}

void luaX_next(LexState* ls)
{
  LoadFrame l;
  if (l_unlikely(ls->z->resuming))
    l = luaZ_restore(ls->z->rz, LF_NEXT);
  try
  {
    switch (l.site)
    {
    case 0:
      ls->lastline = ls->linenumber;
      if (ls->lookahead.token != TK_EOS)
      {                               /* is there a look-ahead token? */
        ls->t = ls->lookahead;        /* use this one */
        ls->lookahead.token = TK_EOS; /* and discharge it */
        break;
      }
      l.site = 1;
      [[fallthrough]];
    case 1:
      ls->t.token = llex(ls, &ls->t); /* read next token */
      break;
    default:
      luaZ_badstate(ls->z->rz);
    }
  }
  catch (...)
  {
    luaZ_record(ls->z->rz, LF_NEXT, l);
    throw;
  }
}

int luaX_lookahead(LexState* ls)
{
  assert(ls->lookahead.token == TK_EOS);
  ls->lookahead.token = llex(ls, &ls->lookahead); /* (no frame of its own: see 'check_next1') */
  return ls->lookahead.token;
}

}
