/*
** $Id: llex.c $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

#include <locale.h>
#include <string.h>

#include "lua.h"

#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lobject.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"

#include "lhack.h"

static l_forceinline void next(LexState* ls)
{
  ls->current = (ls->z->n-- > 0) ? (unsigned char)(*ls->z->p++) : luaZ_fill(ls->z);
}

/* minimum size for string buffer */
#if !defined(LUA_MINBUFFER)
#define LUA_MINBUFFER 32
#endif

static inline int currIsNewline(const LexState* ls)
{
  return ls->current == '\n' || ls->current == '\r';
}

/* ORDER RESERVED */
static const InternalString* const luaX_tokens[] = {
    lit<"and">(), lit<"break">(), lit<"do">(), lit<"else">(), lit<"elseif">(),
    lit<"end">(), lit<"false">(), lit<"for">(), lit<"function">(), lit<"global">(), lit<"goto">(), lit<"if">(),
    lit<"in">(), lit<"local">(), lit<"nil">(), lit<"not">(), lit<"or">(), lit<"repeat">(),
    lit<"return">(), lit<"then">(), lit<"true">(), lit<"until">(), lit<"while">(),
    lit<"//">(), lit<"..">(), lit<"...">(), lit<"==">(), lit<">=">(), lit<"<=">(), lit<"~=">(),
    lit<"<<">(), lit<">>">(), lit<"::">(), lit<"<eof>">(),
    lit<"<number>">(), lit<"<integer>">(), lit<"<name>">(), lit<"<string>">()};


static l_noret lexerror(LexState* ls, const InternalString* msg, int token);

static void save(LexState* ls, int c)
{
  Mbuffer* b = ls->buff;
  if (((b)->n) + 1 > ((b)->buffsize))
  {
    size_t newsize = ((b)->buffsize); /* get old size */
    ;
    if (newsize >= (MAX_SIZE / 3 * 2)) /* larger than MAX_SIZE/1.5 ? */
      lexerror(ls, lit<"lexical element too long">(), 0);
    newsize += (newsize >> 1); /* new size is 1.5 times the old one */
    ((b)->buffer = ((char*)(luaM_saferealloc_(ls->L, (b)->buffer, (b)->buffsize * sizeof(char), (newsize) * sizeof(char)))), (b)->buffsize = (newsize));
  }
  b->buffer[((b)->n)++] = ((char)(c));
}

static inline void save_and_next(LexState* ls)
{
  save(ls, ls->current);
  next(ls);
}

void luaX_init(lua_State* L)
{
  int i;
  TString* e = luaS_newlstr(L, LUA_ENV, sizeof(LUA_ENV) - 1); /* create env name */
  assert((e->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaC_fix(L, const_cast<GCObject*>(static_cast<const GCObject*>(e)));                  /* never collect this name */
  for (i = 0; i < NUM_RESERVED; i++)
  {
    TString* ts = luaS_new(L, luaX_tokens[i]);
    assert((ts->tt & NOVARIANT_MASK) >= LUA_TSTRING);
    luaC_fix(L, const_cast<GCObject*>(static_cast<const GCObject*>(ts)));     /* reserved words are never collected */
    ts->extra = ((lu_byte)(i + 1)); /* reserved word */
  }
}

const InternalString* luaX_token2str(LexState* ls, int token)
{
  if (token < FIRST_RESERVED)
  { /* single-byte symbols? */
    if (lisprint(token))
      return luaO_pushfstring(ls->L, lit<"'%c'">(), FmtArgs().chr(token));
    else /* control character */
      return luaO_pushfstring(ls->L, lit<"'<\\%d>'">(), FmtArgs().num(token));
  }
  else
  {
    const InternalString* s = luaX_tokens[token - FIRST_RESERVED];
    if (token < TK_EOS) /* fixed format (symbols and reserved words)? */
      return luaO_pushfstring(ls->L, lit<"'%s'">(), FmtArgs().str(s));
    else /* names, strings, and numerals */
      return s;
  }
}

static const InternalString* txtToken(LexState* ls, int token)
{
  switch (token)
  {
  case TK_NAME:
  case TK_STRING:
  case TK_FLT:
  case TK_INT:
    save(ls, '\0');
    return luaO_pushfstring(ls->L, lit<"'%s'">(), FmtArgs().str(((ls->buff)->buffer)));
  default:
    return luaX_token2str(ls, token);
  }
}

static l_noret lexerror(LexState* ls, const InternalString* msg, int token)
{
  msg = luaG_addinfo(ls->L, msg, ls->source, ls->linenumber);
  if (token)
    luaO_pushfstring(ls->L, lit<"%s near %s">(), FmtArgs().str(msg).str(txtToken(ls, token)));
  luaD_throw(ls->L, LUA_ERRSYNTAX);
}

/*
** InternalString form (wrapped literal sites). c_str() is the
** sanctioned transitional bridge into 'lexerror' until the
** message-plumbing (lexerror/addinfo) migrates off const char*.
*/
l_noret luaX_syntaxerror(LexState* ls, const InternalString* msg)
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
  int tag = luaH_getstr(ls->h, ts, &oldts);
  if (!(((tag) & NOVARIANT_MASK) == LUA_TNIL))     /* string already present? */
  {
    assert((oldts.tt_ & NOVARIANT_MASK) == LUA_TSTRING);
    return tsvalue(&oldts); /* use stored value */
  }
  else
  {                                /* create a new entry */
    StkId stv = L->top++; /* reserve stack space for string */
    setslots(L, stv, ts);         /* push (anchor) the string on the stack */
    TValue sv = getslot(L, stv); /* the slot anchors the string */
    luaH_set(L, ls->h, &sv, &sv);  /* t[string] = string */
    /* table is not a metatable, so it does not need to invalidate cache */
    luaC_checkGC(L);
    L->top--; /* remove string from stack */
    return ts;
  }
}

/*
** Creates a new string and anchors it in scanner's table.
*/
TString* luaX_newstring(LexState* ls, const char* str, size_t l)
{
  return anchorstr(ls, luaS_newlstr(ls->L, str, l));
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
    lexerror(ls, lit<"chunk has too many lines">(), 0);
}

void luaX_setinput(lua_State* L, LexState* ls, ZIO* z, TString* source,
                   int firstchar)
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
  ls->envn = luaS_newlstr(L, LUA_ENV, sizeof(LUA_ENV) - 1); /* get env string */
  ls->brkn = (luaS_newlstr(L, "" "break", (sizeof("break") / sizeof(char)) - 1)); /* get "break" string */
  /* initialize buffer */
  ls->buff->buffer = (char*)luaM_saferealloc_(ls->L, ls->buff->buffer,
                                              ls->buff->buffsize * sizeof(char),
                                              LUA_MINBUFFER * sizeof(char));
  ls->buff->buffsize = LUA_MINBUFFER;
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
static int check_next2(LexState* ls, const char* set)
{
  assert(set[2] == '\0');
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
  const char* expo = "Ee";
  int first = ls->current;
  assert(lisdigit(ls->current));
  save_and_next(ls);
  if (first == '0' && check_next2(ls, "xX")) /* hexadecimal? */
    expo = "Pp";
  for (;;)
  {
    if (check_next2(ls, expo))                             /* exponent mark? */
      check_next2(ls, "-+");                               /* optional exponent sign */
    else if (lisxdigit(ls->current) || ls->current == '.') /* '%x|%.' */
      save_and_next(ls);
    else
      break;
  }
  if (lislalpha(ls->current)) /* is numeral touching a letter? */
    save_and_next(ls);        /* force an error */
  save(ls, '\0');
  if (luaO_str2num(((ls->buff)->buffer), &obj) == 0) /* format error? */
    lexerror(ls, lit<"malformed number">(), TK_FLT);
  if (((&obj)->tt_ == LUA_VNUMINT))
  {
    seminfo->i = ivalue(&obj);
    return TK_INT;
  }
  else
  {
    assert(((&obj)->tt_ == LUA_VNUMFLT));
    assert((&obj)->tt_ == LUA_VNUMFLT);
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
      const char* what = (seminfo ? "string" : "comment");
      const InternalString* msg = luaO_pushfstring(ls->L,
                                         lit<"unfinished long %s (starting at line %d)">(),
                                         FmtArgs().str(what).num(line));
      lexerror(ls, msg, TK_EOS);
      break; /* to avoid warnings */
    }
    case ']':
    {
      if (skip_sep(ls) == sep)
      {
        save_and_next(ls); /* skip 2nd ']' */
        if (seminfo)
          seminfo->ts = luaX_newstring(ls, ((ls->buff)->buffer) + sep,
                                       ((ls->buff)->n) - 2 * sep);
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
        ((ls->buff)->n = 0); /* avoid wasting space */
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

static void esccheck(LexState* ls, int c, const InternalString* msg)
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
  esccheck(ls, lisxdigit(ls->current), lit<"hexadecimal digit expected">());
  return luaO_hexavalue(ls->current);
}

static int readhexaesc(LexState* ls)
{
  int r = gethexa(ls);
  r = (r << 4) + gethexa(ls);
  ((ls->buff)->n -= ((size_t)(2))); /* remove saved chars from buffer */
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
  esccheck(ls, ls->current == '{', lit<"missing '{'">());
  r = ((unsigned int)(gethexa(ls))); /* must have at least one digit */
  while (((void)(save_and_next(ls))), lisxdigit(ls->current))
  {
    i++;
    esccheck(ls, r <= (0x7FFFFFFFu >> 4), lit<"UTF-8 value too large">());
    r = (r << 4) + luaO_hexavalue(ls->current);
  }
  esccheck(ls, ls->current == '}', lit<"missing '}'">());
  next(ls);                     /* skip '}' */
  ((ls->buff)->n -= ((size_t)(i))); /* remove saved chars from buffer */
  return r;
}

static void utf8esc(LexState* ls)
{
  char buff[UTF8BUFFSZ];
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
  esccheck(ls, r <= UCHAR_MAX, lit<"decimal escape too large">());
  ((ls->buff)->n -= ((size_t)(i))); /* remove read digits from buffer */
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
      lexerror(ls, lit<"unfinished string">(), TK_EOS);
      break; /* to avoid warnings */
    case '\n':
    case '\r':
      lexerror(ls, lit<"unfinished string">(), TK_STRING);
      break; /* to avoid warnings */
    case '\\':
    {                    /* escape sequences */
      save_and_next(ls); /* keep '\\' for error messages */
      switch (ls->current)
      {
      case 'a':
        next(ls);
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
        save(ls, '\a');
        break;
      case 'b':
        next(ls);
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
        save(ls, '\b');
        break;
      case 'f':
        next(ls);
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
        save(ls, '\f');
        break;
      case 'n':
        next(ls);
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
        save(ls, '\n');
        break;
      case 'r':
        next(ls);
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
        save(ls, '\r');
        break;
      case 't':
        next(ls);
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
        save(ls, '\t');
        break;
      case 'v':
        next(ls);
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
        save(ls, '\v');
        break;
      case 'x':
      {
        int c = readhexaesc(ls); /* final character to be saved */
        next(ls);
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
        save(ls, c);
        break;
      }
      case 'u':
        utf8esc(ls); /* saves the sequence itself */
        break;
      case '\n':
      case '\r':
        inclinenumber(ls);
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
        save(ls, '\n');
        break;
      case '\\':
      case '\"':
      case '\'':
      {
        int c = ls->current; /* final character to be saved */
        next(ls);
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
        save(ls, c);
        break;
      }
      case EOZ:
        break; /* will raise an error next loop */
      case 'z':
      {                               /* zap following span of spaces */
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
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
        esccheck(ls, lisdigit(ls->current), lit<"invalid escape sequence">());
        int c = readdecesc(ls); /* digital escape '\ddd' */
        ((ls->buff)->n -= ((size_t)(1))); /* remove '\\' */
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
  seminfo->ts = luaX_newstring(ls, ((ls->buff)->buffer) + 1,
                               ((ls->buff)->n) - 2);
}

static int llex(LexState* ls, Token* seminfo)
{
  ((ls->buff)->n = 0);
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
        ((ls->buff)->n = 0); /* 'skip_sep' may dirty the buffer */
        if (sep >= 2)
        {
          read_long_string(ls, nullptr, sep); /* skip long comment */
          ((ls->buff)->n = 0);      /* previous call may dirty the buff. */
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
        lexerror(ls, lit<"invalid long string delimiter">(), TK_STRING);
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
        ts = luaS_newlstr(ls->L, ((ls->buff)->buffer),
                          ((ls->buff)->n));
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
