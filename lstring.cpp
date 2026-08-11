/*
** $Id: lstring.c $
** String table (keeps all strings handled by Lua)
** See Copyright Notice in lua.h
*/


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

static TString* createstrobj(lua_State* L, InternalString&& str, size_t h, int external)
{
  GCObject* o = luaC_newobj(L, LUA_VSTRING, sizeof(TString));
  assert((o->tt & NOVARIANT_MASK) == LUA_TSTRING);
  TString* ts = static_cast<TString*>(o);
  ts->hash = ((unsigned int)(h));
  ts->extra = external ? STREXTRA_EXTERNAL : 0;
  new (&ts->data) InternalString();
  if (!external && l_unlikely(!luaM_reserve(L, str.size())))
    luaD_throw(L, LUA_ERRMEM);
  ts->data = std::move(str);
  return ts;
}

void luaS_init(lua_State* L)
{
  global_State* g = L->l_G;
  /* pre-create memory-error message */
  g->memerrmsg = luaS_createstr(L, lt::not_enough_memory, 0, lt::not_enough_memory.size());
  assert((g->memerrmsg->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaC_fix(L, const_cast<GCObject*>(static_cast<const GCObject*>(g->memerrmsg))); /* it should never be collected */
}

constexpr int LUAI_MAXSHORTLEN = 40;

TString* luaS_createstr(lua_State* L, std::string_view str, size_t off, size_t len)
{
  global_State* g = L->l_G;
  const char* s = str.data() + off;
  size_t l = len;
  std::string_view sv(s, l);
  size_t h = luaS_hash(s, l);
  if (l <= LUAI_MAXSHORTLEN)
  {
    global_State::InternKey key{sv, h};
    auto it = g->strt.find(key);
    if (it != g->strt.end())
    {
      TString* ts = it->second;
      if (isdead(g, ts))
        changewhite(ts); /* resurrect it */
      return ts;
    }
    TString* ts = createstrobj(L, InternalString(sv, 0, l), h, 0);
    g->strt.emplace(global_State::InternKey{ts->data.view(), h}, ts);
    return ts;
  }
  return createstrobj(L, InternalString(sv, 0, l), h, 0);
}

TString* luaS_take(lua_State* L, InternalString&& str)
{
  global_State* g = L->l_G;
  std::string_view bytes = str.view();
  size_t h = InternalString::hashBytes(bytes.data(), bytes.size());
  if (str.size() <= LUAI_MAXSHORTLEN)
  {
    global_State::InternKey key{str.view(), h};
    auto it = g->strt.find(key);
    if (it != g->strt.end())
    {
      TString* ts = it->second;
      if (isdead(g, ts))
        changewhite(ts); /* resurrect it */
      return ts;
    }
    TString* ts = createstrobj(L, std::move(str), h, 0);
    g->strt.emplace(global_State::InternKey{ts->data.view(), h}, ts);
    return ts;
  }
  return createstrobj(L, std::move(str), h, 0);
}

TString* luaS_takeexternal(lua_State* L, InternalString&& str)
{
  if (str.size() <= LUAI_MAXSHORTLEN)
    return luaS_take(L, std::move(str));
  std::string_view bytes = str.view();
  size_t h = InternalString::hashBytes(bytes.data(), bytes.size());
  return createstrobj(L, std::move(str), h, 1);
}

void luaS_remove(lua_State* L, TString* ts)
{
  if (ts->data.size() <= LUAI_MAXSHORTLEN)
  {
    global_State* g = L->l_G;
    size_t h = ts->hash;
    auto it = g->strt.find(global_State::InternKey{ts->data.view(), h});
    if (it != g->strt.end() && it->second == ts)
      g->strt.erase(it);
  }
}

Udata* luaS_newudata(lua_State* L, size_t s, unsigned short nuvalue)
{
  global_State* g = L->l_G;
  Udata* u;
  int i;
  if (l_unlikely(s >= MAX_SIZE - sizeof(Udata)))
    luaM_toobig(L);
  GCObject* o = luaC_newobj(L, LUA_VUSERDATA, sizeof(Udata));
  assert(o->tt == LUA_VUSERDATA);
  u = static_cast<Udata*>(o);
  u->nuvalue = 0;
  u->metatable = nullptr;
  u->uv = nullptr;
  u->data = std::span<char>{};
  lu_byte oldgcstop = g->gcstopem;
  g->gcstopem = 1;
  TValue* uv = (TValue*)luaM_realloc_(L, nullptr, 0, (size_t)nuvalue * sizeof(TValue));
  if (uv != nullptr || nuvalue == 0)
  {
    u->uv = uv;
    u->nuvalue = nuvalue;
    char* p = (char*)luaM_realloc_(L, nullptr, 0, udatadatasize(s));
    if (p != nullptr)
      u->data = std::span<char>{p, s};
  }
  g->gcstopem = oldgcstop;
  if ((uv == nullptr && nuvalue > 0) || u->data.data() == nullptr)
    luaD_throw(L, LUA_ERRMEM);
  for (i = 0; i < nuvalue; i++)
    ((&u->uv[i])->tt_ = LUA_VNIL);
  return u;
}
