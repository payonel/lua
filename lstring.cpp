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

namespace lua::object
{

/*
** generic equality for strings
*/
int luaS_eqstr(TString* a, TString* b)
{
  return (a == b) || (a->data == b->data);
}

static size_t luaS_hash(byte_view str)
{
  return InternalString::hashBytes(str.data(), str.size());
}

void TString::luaS_forcehash()
{
  TString* ts = this;
  byte_view bytes = ts->data.view();
  ts->hash = (unsigned int)InternalString::hashBytes(bytes.data(), bytes.size());
  ts->hashed = 1;
}

TString* TString::createstrobj(lua_State* L, size_t l, size_t h, int hashed, int external)
{
  /* the string and its 'l' bytes are charged together, before either exists
     and before the string is known to the collector (a refusal may run an
     emergency collection); its maker gives it its bytes */
  TString* ts = luaM_malloc_TString(L, external ? 0 : l);
  luaC_link(L, ts, LUA_VSTRING);
  ts->hash = (unsigned int)h;
  ts->hashed = (lu_byte)hashed;
  ts->extra = external ? STREXTRA_EXTERNAL : 0;
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
    if (ts->hash == (unsigned int)h && ts->data.view() == sv)
      return ts;
    ts = ts->hnext;
  }
  return nullptr;
}

static void internresize(global_State* g, size_t newsize)
{
  assert(ispow2(newsize));
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
  g->memerrmsg = TString::luaS_createstr(L, lt::not_enough_memory);
  assert(novariant(g->memerrmsg->tt) >= LUA_TSTRING);
  luaC_fix(L, g->memerrmsg); /* it should never be collected */
  g->nofreezemsg = TString::luaS_createstr(L, lt::cannot_freeze); /* (made now: destroying a chain allocates nothing) */
  luaC_fix(L, g->nofreezemsg);
}

constexpr int LUAI_MAXSHORTLEN = 40;

TString* TString::luaS_createstr(lua_State* L, byte_view str)
{
  global_State* g = L->l_G;
  size_t l = str.size();
  if (l <= LUAI_MAXSHORTLEN)
  {
    size_t h = luaS_hash(str);
    TString* ts = internfind(g, str, h);
    if (ts != nullptr)
    {
      if (isdead(g, ts))
        changewhite(ts); /* resurrect it */
      return ts;
    }
    ts = TString::createstrobj(L, l, h, 1, 0);
    ts->data = InternalString(str);
    interninsert(g, ts);
    return ts;
  }
  TString* ts = TString::createstrobj(L, l, 0, 0, 0);
  ts->data = InternalString(str);
  return ts;
}

TString* TString::luaS_take(lua_State* L, InternalString&& str)
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
    ts = TString::createstrobj(L, str.size(), h, 1, 0);
    ts->data = std::move(str);
    interninsert(g, ts);
    return ts;
  }
  TString* ts = TString::createstrobj(L, str.size(), 0, 0, 0);
  ts->data = std::move(str);
  return ts;
}

TString* TString::luaS_takeexternal(lua_State* L, InternalString&& str)
{
  if (str.size() <= LUAI_MAXSHORTLEN)
    return TString::luaS_take(L, std::move(str));
  TString* ts = TString::createstrobj(L, str.size(), 0, 0, 1);
  ts->data = std::move(str);
  return ts;
}

/*
** A long string of 'l' zero bytes, made in its final place for its maker to
** write them before it is shared
*/
TString* TString::luaS_createlngstrobj(lua_State* L, size_t l)
{
  assert(l > LUAI_MAXSHORTLEN);
  TString* ts = TString::createstrobj(L, l, 0, 0, 0);
  ts->data = InternalString(byte_owner(l), l);
  return ts;
}

/*
** A string of the first 'len' bytes of 'block', a buffer whose maker no
** longer counts it: the block becomes the string's bytes, kept with the
** tail after them when that is at most half the string (a grown buffer's);
** a longer tail is not kept, the bytes are copied
*/
TString* TString::luaS_takeblock(lua_State* L, byte_owner&& block, size_t len)
{
  if (len <= LUAI_MAXSHORTLEN)
    return TString::luaS_createstr(L, byte_view(block.data(), len));
  TString* ts = TString::createstrobj(L, len, 0, 0, 0);
  if (block.size() - len > len / 2)
    ts->data = InternalString(byte_view(block.data(), len));
  else
    ts->data = InternalString(std::move(block), len);
  return ts;
}

void TString::luaS_remove(lua_State* L)
{
  TString* ts = this;
  if (ts->data.size() <= LUAI_MAXSHORTLEN)
  {
    global_State* g = L->l_G;
    auto& st = g->strt;
    if (st.buckets.empty())
      return;
    size_t b = ts->hash & (st.buckets.size() - 1);
    TString* prev = nullptr;
    for (TString* p = st.buckets[b]; p != nullptr; p = p->hnext)
    {
      if (p == ts)
      {
        if (prev != nullptr)
          prev->hnext = ts->hnext;
        else
          st.buckets[b] = ts->hnext;
        st.count--;
        return;
      }
      prev = p;
    }
  }
}

Udata* Udata::luaS_newudata(lua_State* L, size_t s, int nuvalue)
{
  global_State* g = L->l_G;
  Udata* u;
  int i;
  if (l_unlikely(s >= MAX_SIZE - cost::Udata))
    luaM_toobig(L);
  u = luaM_malloc_Udata(L);
  luaC_link(L, u, LUA_VUSERDATA);
  u->zb = s == 0;
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
    setnilvalue(&u->uv[i]);
  return u;
}

}
