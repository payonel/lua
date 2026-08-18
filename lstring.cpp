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

void luaS_forcehash(TString* ts)
{
  byte_view bytes = ts->data.view();
  ts->hash = ((unsigned int)(InternalString::hashBytes(bytes.data(), bytes.size())));
  ts->hashed = 1;
}

static TString* createstrobj(lua_State* L, InternalString&& str, size_t h,
                             int hashed, int external)
{
  TString* ts = luaM_malloc_TString(L);
  luaC_link(L, ts, LUA_VSTRING);
  ts->hash = ((unsigned int)(h));
  ts->hashed = ((lu_byte)(hashed));
  ts->hnext = nullptr;
  ts->extra = external ? STREXTRA_EXTERNAL : 0;
  if (!external && l_unlikely(!luaM_reserve(L, str.size())))
    luaD_throw(L, LUA_ERRMEM);
  ts->data = std::move(str);
  return ts;
}

#if !defined(MINSTRTABSIZE)
#define MINSTRTABSIZE 128
#endif

/*
** Find an interned string by content. Deliberately returns DEAD
** (unmarked, not-yet-swept) strings too: the caller resurrects them
** via changewhite. Skipping dead entries here would create a duplicate
** TString while the dead twin is still chained, transiently breaking
** the one-TString-per-short-content invariant that eqshrstr (pointer
** equality) relies on. Chain memory is always valid: freeobj unlinks
** a string (luaS_remove) before freeing it.
*/
static TString* internfind(global_State* g, byte_view sv, size_t h)
{
  const auto& buckets = g->strt.buckets;
  if (buckets.empty())
    return nullptr;
  TString* ts = buckets[h & (buckets.size() - 1)];
  while (ts != nullptr)
  {
    if (ts->hash == ((unsigned int)(h)) && ts->data.view() == sv)
      return ts;
    ts = ts->hnext;
  }
  return nullptr;
}

static void internresize(global_State* g, size_t newsize)
{
  assert((newsize & (newsize - 1)) == 0);
  std::vector<TString*> nb(newsize, nullptr);
  for (TString* ts : g->strt.buckets)
  {
    while (ts != nullptr)
    {
      TString* next = ts->hnext;
      size_t i = ts->hash & (newsize - 1);
      ts->hnext = nb[i];
      nb[i] = ts;
      ts = next;
    }
  }
  g->strt.buckets.swap(nb);
}

static void interninsert(global_State* g, TString* ts)
{
  auto& st = g->strt;
  if (st.count >= st.buckets.size())
    internresize(g, st.buckets.empty() ? MINSTRTABSIZE : st.buckets.size() * 2);
  size_t i = ts->hash & (st.buckets.size() - 1);
#if defined(LUA_DEBUG)
  for (TString* c = st.buckets[i]; c != nullptr; c = c->hnext)
    assert(!(c->hash == ts->hash && c->data == ts->data));
#endif
  ts->hnext = st.buckets[i];
  st.buckets[i] = ts;
  st.count++;
}

void luaS_init(lua_State* L)
{
  global_State* g = L->l_G;
  /* pre-create memory-error message */
  g->memerrmsg = luaS_createstr(L, lt::not_enough_memory, 0, lt::not_enough_memory.size());
  assert((g->memerrmsg->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaC_fix(L, g->memerrmsg); /* it should never be collected */
}

constexpr int LUAI_MAXSHORTLEN = 40;

TString* luaS_createstr(lua_State* L, byte_view str, size_t off, size_t len)
{
  global_State* g = L->l_G;
  byte_view sv = str.substr(off, len);
  const char* s = sv.data();
  size_t l = len;
  if (l <= LUAI_MAXSHORTLEN)
  {
    size_t h = luaS_hash(s, l);
    TString* ts = internfind(g, sv, h);
    if (ts != nullptr)
    {
      if (isdead(g, ts))
        changewhite(ts); /* resurrect it */
      return ts;
    }
    ts = createstrobj(L, InternalString(sv, 0, l), h, 1, 0);
    interninsert(g, ts);
    return ts;
  }
  return createstrobj(L, InternalString(sv, 0, l), 0, 0, 0);
}

TString* luaS_take(lua_State* L, InternalString&& str)
{
  global_State* g = L->l_G;
  if (str.size() <= LUAI_MAXSHORTLEN)
  {
    byte_view bytes = str.view();
    size_t h = InternalString::hashBytes(bytes.data(), bytes.size());
    TString* ts = internfind(g, bytes, h);
    if (ts != nullptr)
    {
      if (isdead(g, ts))
        changewhite(ts); /* resurrect it */
      return ts;
    }
    ts = createstrobj(L, std::move(str), h, 1, 0);
    interninsert(g, ts);
    return ts;
  }
  return createstrobj(L, std::move(str), 0, 0, 0);
}

TString* luaS_takeexternal(lua_State* L, InternalString&& str)
{
  if (str.size() <= LUAI_MAXSHORTLEN)
    return luaS_take(L, std::move(str));
  return createstrobj(L, std::move(str), 0, 0, 1);
}

void luaS_remove(lua_State* L, TString* ts)
{
  if (ts->data.size() <= LUAI_MAXSHORTLEN)
  {
    global_State* g = L->l_G;
    auto& st = g->strt;
    if (st.buckets.empty())
      return;
    TString** p = &st.buckets[ts->hash & (st.buckets.size() - 1)];
    while (*p != nullptr)
    {
      if (*p == ts)
      {
        *p = ts->hnext;
        st.count--;
        return;
      }
      p = &(*p)->hnext;
    }
  }
}

Udata* luaS_newudata(lua_State* L, size_t s, unsigned short nuvalue)
{
  global_State* g = L->l_G;
  Udata* u;
  int i;
  if (l_unlikely(s >= MAX_SIZE - sizeof(Udata)))
    luaM_toobig(L);
  u = luaM_malloc_Udata(L);
  luaC_link(L, u, LUA_VUSERDATA);
  u->metatable = nullptr;
  u->uv = {};
  u->zb = s == 0;
  u->data = std::span<char>{};
  lu_byte oldgcstop = g->gcstopem;
  g->gcstopem = 1;
  std::span<TValue> uv = luaM_alloc_TValue_span(L->l_G, L, {}, nuvalue, false);
  if (uv.data() != nullptr || nuvalue == 0)
  {
    u->uv = uv;
    u->data = luaM_alloc_char_span(L->l_G, L, {}, s == 0 ? 1 : s, false);

  }
  g->gcstopem = oldgcstop;
  if ((uv.data() == nullptr && nuvalue > 0) || u->data.data() == nullptr)
    luaD_throw(L, LUA_ERRMEM);
  for (i = 0; i < nuvalue; i++)
    (u->uv[i].tt_ = LUA_VNIL);
  return u;
}
