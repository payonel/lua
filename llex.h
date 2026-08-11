/*
** $Id: llex.h $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#pragma once

#include <limits.h>

#include "lobject.h"
#include "lzio.h"
#include "literals.h"

/*
** Single-char tokens (terminal symbols) are represented by their own
** numeric code. Other tokens start at the following value.
*/
constexpr int FIRST_RESERVED = (UCHAR_MAX + 1);

/*
 * WARNING: if you change the order of this enumeration,
 * grep "ORDER RESERVED"
 */
enum RESERVED
{
  /* terminal symbols denoted by reserved words */
  TK_AND = FIRST_RESERVED,
  TK_BREAK,
  TK_DO,
  TK_ELSE,
  TK_ELSEIF,
  TK_END,
  TK_FALSE,
  TK_FOR,
  TK_FUNCTION,
  TK_GLOBAL,
  TK_GOTO,
  TK_IF,
  TK_IN,
  TK_LOCAL,
  TK_NIL,
  TK_NOT,
  TK_OR,
  TK_REPEAT,
  TK_RETURN,
  TK_THEN,
  TK_TRUE,
  TK_UNTIL,
  TK_WHILE,
  /* other terminal symbols */
  TK_IDIV,
  TK_CONCAT,
  TK_DOTS,
  TK_EQ,
  TK_GE,
  TK_LE,
  TK_NE,
  TK_SHL,
  TK_SHR,
  TK_DBCOLON,
  TK_EOS,
  TK_FLT,
  TK_INT,
  TK_NAME,
  TK_STRING
};

/* number of reserved words */
constexpr int NUM_RESERVED = (((int)(TK_WHILE - FIRST_RESERVED + 1)));

struct Token
{
  int token;
  lua_Number r;
  lua_Integer i;
  TString* ts;
};

class MBuffer;

/* state of the scanner plus state of the parser when shared by all
   functions */
struct LexState
{
  int current;          /* current character (charint) */
  int linenumber;       /* input line counter */
  int lastline;         /* line of last token 'consumed' */
  Token t;              /* current token */
  Token lookahead;      /* look ahead token */
  struct FuncState* fs; /* current function (parser) */
  struct lua_State* L;
  ZIO* z;              /* input stream */
  MBuffer* buff;   /* buffer for tokens */
  Table* h;            /* to avoid collection/reuse strings */
  struct Dyndata* dyd; /* dynamic structures used by the parser */
  TString* source;     /* current source name */
  TString* envn;       /* environment variable name */
  TString* brkn;       /* "break" name (used as a label) */
  TString* glbn;       /* "global" name (when not a reserved word) */
};

LUAI_FUNC void                  luaX_init(lua_State* L);
LUAI_FUNC void                  luaX_setinput(lua_State* L, LexState* ls, ZIO* z, TString* source, int firstchar);
LUAI_FUNC TString*              luaX_newstring(LexState* ls, std::string_view str, size_t off, size_t len);
LUAI_FUNC void                  luaX_next(LexState* ls);
LUAI_FUNC int                   luaX_lookahead(LexState* ls);
LUAI_FUNC l_noret               luaX_syntaxerror(LexState* ls, std::string_view s);
LUAI_FUNC std::string_view      luaX_token2str(LexState* ls, int token);
