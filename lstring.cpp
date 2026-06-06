/*
** $Id: lstring.c $
** String table (keeps all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

#include <new>
#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"

/*
** generic equality for strings
*/
int luaS_eqstr(TString* a, TString* b)
{
  return (a == b) || (a->content == b->content);
}

static size_t luaS_hash(const char* str, size_t l)
{
  unsigned int h = 0x5bd1e995 ^ cast_uint(l);
  for (; l > 0; l--)
    h ^= ((h << 5) + (h >> 2) + cast_byte(str[l - 1]));
  return h;
}

/*
** Create a new TString with placement-new for std::string
*/
static TString* createstrobj(lua_State* L, const char* str, size_t l,
                             size_t h)
{
  GCObject* o = luaC_newobj(L, LUA_VSTRING, sizeof(TString));
  TString* ts = gco2ts(o);
  ts->hash = cast_uint(h);
  ts->extra = 0;
  new (&ts->content) std::string(str, l);
  luaM_reserve(L, ts->content.capacity());
  return ts;
}

/*
** Clear API string cache. (Entries cannot be empty, so fill them with
** a non-collectable string.)
*/
void luaS_clearcache(global_State* g)
{
  int i, j;
  for (i = 0; i < STRCACHE_N; i++)
    for (j = 0; j < STRCACHE_M; j++)
    {
      if (iswhite(g->strcache[i][j]))     /* will entry be collected? */
        g->strcache[i][j] = g->memerrmsg; /* replace it with something fixed */
    }
}

/*
** Initialize the string table and the string cache
*/
void luaS_init(lua_State* L)
{
  global_State* g = G(L);
  int i, j;
  /* pre-create memory-error message */
  g->memerrmsg = luaS_newliteral(L, MEMERRMSG);
  luaC_fix(L, obj2gco(g->memerrmsg)); /* it should never be collected */
  for (i = 0; i < STRCACHE_N; i++)    /* fill cache with valid strings */
    for (j = 0; j < STRCACHE_M; j++)
      g->strcache[i][j] = g->memerrmsg;
}

/*
** Maximum length for strings that are internalized.
*/
#if !defined(LUAI_MAXSHORTLEN)
#define LUAI_MAXSHORTLEN 40
#endif

/*
** Create a new string, interning short strings in the global table.
*/
TString* luaS_createstr(lua_State* L, const char* str, size_t l)
{
  global_State* g = G(L);
  size_t h = luaS_hash(str, l);
  if (l <= LUAI_MAXSHORTLEN)
  {
    std::string_view sv(str, l);
    global_State::InternKey key{sv, h};
    auto it = g->strt.find(key);
    if (it != g->strt.end())
    {
      TString* ts = it->second;
      if (isdead(g, ts))
        changewhite(ts); /* resurrect it */
      return ts;
    }
    /* create new short string and intern it */
    TString* ts = createstrobj(L, str, l, h);
    g->strt.emplace(global_State::InternKey{std::string_view(ts->content), h}, ts);
    return ts;
  }
  /* long string: no interning */
  return createstrobj(L, str, l, h);
}

/*
** Remove a string from the interning table (called during GC sweep)
*/
void luaS_remove(lua_State* L, TString* ts)
{
  if (ts->content.size() <= LUAI_MAXSHORTLEN)
  {
    global_State* g = G(L);
    std::string_view sv(ts->content);
    size_t h = ts->hash;
    g->strt.erase(global_State::InternKey{sv, h});
  }
}

/*
** new string (with explicit length)
*/
TString* luaS_newlstr(lua_State* L, const char* str, size_t l)
{
  return luaS_createstr(L, str, l);
}

/*
** Create or reuse a zero-terminated string, first checking in the
** cache (using the string address as a key). The cache can contain
** only zero-terminated strings, so it is safe to use 'strcmp' to
** check hits.
*/
TString* luaS_new(lua_State* L, const char* str)
{
  unsigned int i = point2uint(str) % STRCACHE_N; /* hash */
  int j;
  TString** p = G(L)->strcache[i];
  for (j = 0; j < STRCACHE_M; j++)
  {
    if (strcmp(str, getstr(p[j])) == 0) /* hit? */
      return p[j];                      /* that is it */
  }
  /* normal route */
  for (j = STRCACHE_M - 1; j > 0; j--)
    p[j] = p[j - 1]; /* move out last element */
  /* new element is first in the list */
  p[0] = luaS_newlstr(L, str, strlen(str));
  return p[0];
}

Udata* luaS_newudata(lua_State* L, size_t s, unsigned short nuvalue)
{
  Udata* u;
  int i;
  GCObject* o;
  if (l_unlikely(s > MAX_SIZE - udatamemoffset(nuvalue)))
    luaM_toobig(L);
  o = luaC_newobj(L, LUA_VUSERDATA, sizeudata(nuvalue, s));
  u = gco2u(o);
  u->len = s;
  u->nuvalue = nuvalue;
  u->metatable = NULL;
  for (i = 0; i < nuvalue; i++)
    setnilvalue(&u->uv[i].uv);
  return u;
}
