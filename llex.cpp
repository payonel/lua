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

#include "lhack.h"
#include "lauxlib.h"
#include "literals.h"

static l_forceinline void next(LexState* ls)
{
  ls->current = (ls->z->pos < ls->z->chunk.size())
                    ? (unsigned char)(ls->z->chunk[ls->z->pos++])
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


static l_noret lexerror(LexState* ls, byte_view msg, int token);

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
  b->append(((char)(c)));
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
  assert((e->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaC_fix(L, e);                  /* never collect this name */
  for (i = 0; i < NUM_RESERVED; i++)
  {
    byte_view bytes = luaX_tokens[i];
    TString* ts = TString::luaS_createstr(L, bytes);
    assert((ts->tt & NOVARIANT_MASK) >= LUA_TSTRING);
    luaC_fix(L, ts);     /* reserved words are never collected */
    ts->extra = ((lu_byte)(i + 1)); /* reserved word */
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

static l_noret lexerror(LexState* ls, byte_view msg, int token)
{
  msg = luaG_addinfo(ls->L, msg, ls->source, ls->linenumber);
  if (token)
    luaO_pushfstring(ls->L, lt::fmt_s_near_s, FmtArgs().str(msg).str(txtToken(ls, token)));
  luaD_throw(ls->L, LUA_ERRSYNTAX);
}

l_noret luaX_syntaxerror(LexState* ls, byte_view msg)
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
  if (!(((tag) & NOVARIANT_MASK) == LUA_TNIL))     /* string already present? */
  {
    assert((oldts.tt_ & NOVARIANT_MASK) == LUA_TSTRING);
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
  int old = ls->current;
  assert(currIsNewline(ls));
  next(ls); /* skip '\n' or '\r' */
  if (currIsNewline(ls) && ls->current != old)
    next(ls); /* skip '\n\r' or '\r\n' */
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

/*
** Check whether current char is in set 'set' (with two chars) and
** saves it
*/
static int check_next2(LexState* ls, byte_view set)
{
  assert(set.size() == 2);
  if (ls->current == set[0] || ls->current == set[1])
  {
    save_and_next(ls);
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
  TValue obj;
  byte_view expo = lt::Ee_chars;
  int first = ls->current;
  assert(lisdigit(ls->current));
  save_and_next(ls);
  if (first == '0' && check_next2(ls, lt::xX_chars)) /* hexadecimal? */
    expo = lt::Pp_chars;
  for (;;)
  {
    if (check_next2(ls, expo))                     /* exponent mark? */
      check_next2(ls, lt::fmt_flags_sign);                /* optional exponent sign */
    else if (lisxdigit(ls->current) || ls->current == '.') /* '%x|%.' */
      save_and_next(ls);
    else
      break;
  }
  if (lislalpha(ls->current)) /* is numeral touching a letter? */
    save_and_next(ls);        /* force an error */
  if (luaO_str2num(ls->buff->view(), &obj) == 0) /* format error? */
    lexerror(ls, lt::malformed_number, TK_FLT);
  if ((obj.tt_ == LUA_VNUMINT))
  {
    seminfo->i = ivalue(&obj);
    return TK_INT;
  }
  else
  {
    assert((obj.tt_ == LUA_VNUMFLT));
    assert(obj.tt_ == LUA_VNUMFLT);
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
  size_t count = 0;
  int s = ls->current;
  assert(s == '[' || s == ']');
  save_and_next(ls);
  while (ls->current == '=')
  {
    save_and_next(ls);
    count++;
  }
  return (ls->current == s) ? count + 2
         : (count == 0)     ? 1
                            : 0;
}

static void read_long_string(LexState* ls, Token* seminfo, size_t sep)
{
  int line = ls->linenumber; /* initial line (for error message) */
  save_and_next(ls);         /* skip 2nd '[' */
  if (currIsNewline(ls))     /* string starts with a newline? */
    inclinenumber(ls);       /* skip it */
  for (;;)
  {
    switch (ls->current)
    {
    case EOZ:
    { /* error */
      byte_view what = seminfo ? lt::string_3 : lt::comment;
      const InternalString* msg = luaO_pushfstring(ls->L, lt::unfinished_long_s_starting_at_line_d, FmtArgs().str(what).num(line));
      lexerror(ls, msg->view(), TK_EOS);
      break; /* to avoid warnings */
    }
    case ']':
    {
      if (skip_sep(ls) == sep)
      {
        save_and_next(ls); /* skip 2nd ']' */
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
      inclinenumber(ls);
      if (!seminfo)
        ls->buff->reset(); /* avoid wasting space */
      break;
    }
    default:
    {
      if (seminfo)
        save_and_next(ls);
      else
        next(ls);
    }
    }
  }
}

static void esccheck(LexState* ls, int c, byte_view msg)
{
  if (!c)
  {
    if (ls->current != EOZ)
      save_and_next(ls); /* add current to buffer for error message */
    lexerror(ls, msg, TK_STRING);
  }
}

static int gethexa(LexState* ls)
{
  save_and_next(ls);
  esccheck(ls, lisxdigit(ls->current), lt::hexadecimal_digit_expected);
  return luaO_hexavalue(ls->current);
}

static int readhexaesc(LexState* ls)
{
  int r = gethexa(ls);
  r = (r << 4) + gethexa(ls);
  ls->buff->drop((size_t)(2)); /* remove saved chars from buffer */
  return r;
}

/*
** When reading a UTF-8 escape sequence, save everything to the buffer
** for error reporting in case of errors; 'i' counts the number of
** saved characters, so that they can be removed if case of success.
*/
static l_uint32 readutf8esc(LexState* ls)
{
  l_uint32 r;
  int i = 4;         /* number of chars to be removed: start with #"\u{X" */
  save_and_next(ls); /* skip 'u' */
  esccheck(ls, ls->current == '{', lt::missing_1);
  r = ((unsigned int)(gethexa(ls))); /* must have at least one digit */
  while (((void)(save_and_next(ls))), lisxdigit(ls->current))
  {
    i++;
    esccheck(ls, r <= (0x7FFFFFFFu >> 4), lt::UTF_8_value_too_large);
    r = (r << 4) + luaO_hexavalue(ls->current);
  }
  esccheck(ls, ls->current == '}', lt::missing_2);
  next(ls);                     /* skip '}' */
  ls->buff->drop((size_t)(i)); /* remove saved chars from buffer */
  return r;
}

static void utf8esc(LexState* ls)
{
  byte_span buff(UTF8BUFFSZ);
  int n = luaO_utf8esc(buff, readutf8esc(ls));
  for (; n > 0; n--) /* add 'buff' to string */
    save(ls, buff[UTF8BUFFSZ - n]);
}

static int readdecesc(LexState* ls)
{
  int i;
  int r = 0; /* result accumulator */
  for (i = 0; i < 3 && lisdigit(ls->current); i++)
  { /* read up to 3 digits */
    r = 10 * r + ls->current - '0';
    save_and_next(ls);
  }
  esccheck(ls, r <= UCHAR_MAX, lt::decimal_escape_too_large);
  ls->buff->drop((size_t)(i)); /* remove read digits from buffer */
  return r;
}

static void read_string(LexState* ls, int del, Token* seminfo)
{
  save_and_next(ls); /* keep delimiter (for error messages) */
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
    {                    /* escape sequences */
      save_and_next(ls); /* keep '\\' for error messages */
      switch (ls->current)
      {
      case 'a':
        next(ls);
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, '\a');
        break;
      case 'b':
        next(ls);
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, '\b');
        break;
      case 'f':
        next(ls);
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, '\f');
        break;
      case 'n':
        next(ls);
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, '\n');
        break;
      case 'r':
        next(ls);
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, '\r');
        break;
      case 't':
        next(ls);
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, '\t');
        break;
      case 'v':
        next(ls);
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, '\v');
        break;
      case 'x':
      {
        int c = readhexaesc(ls); /* final character to be saved */
        next(ls);
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, c);
        break;
      }
      case 'u':
        utf8esc(ls);
        break;
      case '\n':
      case '\r':
        inclinenumber(ls);
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, '\n');
        break;
      case '\\':
      case '\"':
      case '\'':
      {
        int c = ls->current; /* final character to be saved */
        next(ls);
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, c);
        break;
      }
      case EOZ:
        break; /* will raise an error next loop */
      case 'z':
      {                               /* zap following span of spaces */
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        next(ls);                     /* skip the 'z' */
        while (lisspace(ls->current))
        {
          if (currIsNewline(ls))
            inclinenumber(ls);
          else
            next(ls);
        }
        break;
      }
      default:
      {
        esccheck(ls, lisdigit(ls->current), lt::invalid_escape_sequence);
        int c = readdecesc(ls); /* digital escape '\ddd' */
        ls->buff->drop((size_t)(1)); /* remove '\\' */
        save(ls, c);
        break;
      }
      }
      break;
    }
    default:
      save_and_next(ls);
    }
  }
  save_and_next(ls); /* skip delimiter */
  seminfo->ts = luaX_newstring(ls, ls->buff->view(), 1,
                               ls->buff->size() - 2);
}

static int llex(LexState* ls, Token* seminfo)
{
  ls->buff->reset();
  for (;;)
  {
    switch (ls->current)
    {
    case '\n':
    case '\r':
    { /* line breaks */
      inclinenumber(ls);
      break;
    }
    case ' ':
    case '\f':
    case '\t':
    case '\v':
    { /* spaces */
      next(ls);
      break;
    }
    case '-':
    { /* '-' or '--' (comment) */
      next(ls);
      if (ls->current != '-')
        return '-';
      /* else is a comment */
      next(ls);
      if (ls->current == '[')
      { /* long comment? */
        size_t sep = skip_sep(ls);
        ls->buff->reset(); /* 'skip_sep' may dirty the buffer */
        if (sep >= 2)
        {
          read_long_string(ls, nullptr, sep); /* skip long comment */
          ls->buff->reset();      /* previous call may dirty the buff. */
          break;
        }
      }
      /* else short comment */
      while (!currIsNewline(ls) && ls->current != EOZ)
        next(ls); /* skip until end of line (or end of file) */
      break;
    }
    case '[':
    { /* long string or simply '[' */
      size_t sep = skip_sep(ls);
      if (sep >= 2)
      {
        read_long_string(ls, seminfo, sep);
        return TK_STRING;
      }
      else if (sep == 0) /* '[=...' missing second bracket? */
        lexerror(ls, lt::invalid_long_string_delimiter, TK_STRING);
      return '[';
    }
    case '=':
    {
      next(ls);
      if (check_next1(ls, '='))
        return TK_EQ; /* '==' */
      else
        return '=';
    }
    case '<':
    {
      next(ls);
      if (check_next1(ls, '='))
        return TK_LE; /* '<=' */
      else if (check_next1(ls, '<'))
        return TK_SHL; /* '<<' */
      else
        return '<';
    }
    case '>':
    {
      next(ls);
      if (check_next1(ls, '='))
        return TK_GE; /* '>=' */
      else if (check_next1(ls, '>'))
        return TK_SHR; /* '>>' */
      else
        return '>';
    }
    case '/':
    {
      next(ls);
      if (check_next1(ls, '/'))
        return TK_IDIV; /* '//' */
      else
        return '/';
    }
    case '~':
    {
      next(ls);
      if (check_next1(ls, '='))
        return TK_NE; /* '~=' */
      else
        return '~';
    }
    case ':':
    {
      next(ls);
      if (check_next1(ls, ':'))
        return TK_DBCOLON; /* '::' */
      else
        return ':';
    }
    case '"':
    case '\'':
    { /* short literal strings */
      read_string(ls, ls->current, seminfo);
      return TK_STRING;
    }
    case '.':
    { /* '.', '..', '...', or number */
      save_and_next(ls);
      if (check_next1(ls, '.'))
      {
        if (check_next1(ls, '.'))
          return TK_DOTS; /* '...' */
        else
          return TK_CONCAT; /* '..' */
      }
      else if (!lisdigit(ls->current))
        return '.';
      else
        return read_numeral(ls, seminfo);
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
      return read_numeral(ls, seminfo);
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
          save_and_next(ls);
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
        int c = ls->current;
        next(ls);
        return c;
      }
    }
    }
  }
}

void luaX_next(LexState* ls)
{
  ls->lastline = ls->linenumber;
  if (ls->lookahead.token != TK_EOS)
  {                               /* is there a look-ahead token? */
    ls->t = ls->lookahead;        /* use this one */
    ls->lookahead.token = TK_EOS; /* and discharge it */
  }
  else
    ls->t.token = llex(ls, &ls->t); /* read next token */
}

int luaX_lookahead(LexState* ls)
{
  assert(ls->lookahead.token == TK_EOS);
  ls->lookahead.token = llex(ls, &ls->lookahead);
  return ls->lookahead.token;
}
