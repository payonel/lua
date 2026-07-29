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
  return (a == b) || (a->data == b->data);
}

static size_t luaS_hash(const char* str, size_t l)
{
  return InternalString::hashBytes(str, l);
}

/*
** Create a new TString. The byte payload is an InternalString,
** placement-constructed into the manually-managed TString (deep copy
** — the caller's pointer is never retained). The luaM_reserve seam
** counts the payload capacity into GC debt (accounting status quo;
** released in freeobj).
*/
static TString* createstrobj(lua_State* L, const char* str, size_t l,
                             size_t h)
{
  GCObject* o = luaC_newobj(L, LUA_VSTRING, sizeof(TString));
  assert((o->tt & NOVARIANT_MASK) == LUA_TSTRING);
  TString* ts = static_cast<TString*>(o);
  ts->hash = ((unsigned int)(h));
  ts->extra = 0;
  new (&ts->data) InternalString(str, l);
  luaM_reserve(L, ts->data.capacity());
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
  g->memerrmsg = luaS_newlstr(L, MEMERRMSG, sizeof(MEMERRMSG) - 1);
  assert((g->memerrmsg->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaC_fix(L, const_cast<GCObject*>(static_cast<const GCObject*>(g->memerrmsg))); /* it should never be collected */
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
    g->strt.emplace(global_State::InternKey{ts->data.view(), h}, ts);
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
  if (ts->data.size() <= LUAI_MAXSHORTLEN)
  {
    global_State* g = G(L);
    size_t h = ts->hash;
    g->strt.erase(global_State::InternKey{ts->data.view(), h});
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

/*
** New string from an InternalString (wrapped literals and other
** engine-held strings). Deep copy as always — 'str' is never
** retained; interning may return an existing TString.
*/
TString* luaS_new(lua_State* L, const InternalString* str)
{
  return luaS_createstr(L, str->bytes(), str->size());
}

/*
** A userdata is three luaM blocks: the header (a GC object), the user
** values array, and the binary data area. The header is created first,
** in a consistent empty state (nuvalue == 0, data == nullptr), so the
** collector can free it at any point; the raw blocks are then
** allocated with emergency collections stopped and committed to the
** header as they arrive. No unwind cleanup is needed on any path —
** see luaF_newCclosure for the reasoning.
*/
Udata* luaS_newudata(lua_State* L, size_t s, unsigned short nuvalue)
{
  global_State* g = G(L);
  Udata* u;
  int i;
  if (l_unlikely(s >= MAX_SIZE - sizeof(Udata)))
    luaM_toobig(L);
  GCObject* o = luaC_newobj(L, LUA_VUSERDATA, sizeof(Udata));
  assert(o->tt == LUA_VUSERDATA);
  u = static_cast<Udata*>(o);
  u->len = 0; /* no data block yet */
  u->nuvalue = 0; /* no user-value array yet */
  u->metatable = nullptr;
  u->uv = nullptr;
  u->data = nullptr;
  lu_byte oldgcstop = g->gcstopem;
  g->gcstopem = 1; /* no emergency collection: the header is unanchored */
  TValue* uv = (TValue*)luaM_realloc_(L, nullptr, 0, (size_t)nuvalue * sizeof(TValue));
  if (uv != nullptr || nuvalue == 0)
  { /* commit the array before the next allocation can fail */
    u->uv = uv;
    u->nuvalue = nuvalue;
    u->data = (char*)luaM_realloc_(L, nullptr, 0, udatadatasize(s));
  }
  g->gcstopem = oldgcstop;
  if ((uv == nullptr && nuvalue > 0) || u->data == nullptr)
    luaD_throw(L, LUA_ERRMEM);
  u->len = s;
  for (i = 0; i < nuvalue; i++)
    ((&u->uv[i])->tt_ = LUA_VNIL);
  return u;
}
