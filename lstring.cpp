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
** — the caller's pointer is never retained).
** RESERVE-BEFORE-GROWTH (ruled 2026-07-29): the payload SIZE is
** reserved against the lua_State — with the memlimit veto HONORED —
** before any allocation happens; freeobj releases the same size.
** Capacity slack (allocator rounding/SSO) stays uncounted but is
** bounded by the counted size.
*/
static TString* createstrobj(lua_State* L, const char* str, size_t l,
                             size_t h)
{
  GCObject* o = luaC_newobj(L, LUA_VSTRING, sizeof(TString));
  assert((o->tt & NOVARIANT_MASK) == LUA_TSTRING);
  TString* ts = static_cast<TString*>(o);
  ts->hash = ((unsigned int)(h));
  ts->extra = 0;
  new (&ts->data) InternalString(); /* valid empty payload: sweepable if we throw */
  if (l_unlikely(!luaM_reserve(L, l))) /* checked BEFORE the payload allocation */
    luaD_throw(L, LUA_ERRMEM);
  ts->data = InternalString(str, l); /* allocate only after admission */
  return ts;
}

/*
** Move form: adopts engine-owned bytes (builder finish) — zero copy.
** The bytes exist already, so the reserve here is the CHECKED
** admission of an allocation made while building (bounded builders)
** — the memlimit veto still applies before the state adopts them.
*/
static TString* createstrobj(lua_State* L, InternalString&& str, size_t h)
{
  GCObject* o = luaC_newobj(L, LUA_VSTRING, sizeof(TString));
  assert((o->tt & NOVARIANT_MASK) == LUA_TSTRING);
  TString* ts = static_cast<TString*>(o);
  ts->hash = ((unsigned int)(h));
  ts->extra = 0;
  new (&ts->data) InternalString(); /* valid empty payload: sweepable if we throw */
  if (l_unlikely(!luaM_reserve(L, str.size()))) /* checked admission */
    luaD_throw(L, LUA_ERRMEM);
  ts->data = std::move(str);
  return ts;
}

/*
** Initialize the string table
*/
void luaS_init(lua_State* L)
{
  global_State* g = G(L);
  /* pre-create memory-error message */
  g->memerrmsg = luaS_newlstr(L, std::string_view{MEMERRMSG, sizeof(MEMERRMSG) - 1});
  assert((g->memerrmsg->tt & NOVARIANT_MASK) >= LUA_TSTRING);
  luaC_fix(L, const_cast<GCObject*>(static_cast<const GCObject*>(g->memerrmsg))); /* it should never be collected */
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
TString* luaS_createstr(lua_State* L, std::string_view str, size_t off, size_t len)
{
  global_State* g = G(L);
  /* storage boundary: the ingested sub-range materializes exactly once,
     here (Java: hash/copy loops over (b, off, len)) */
  const char* s = str.data() + off;
  size_t l = len;
  size_t h = luaS_hash(s, l);
  if (l <= LUAI_MAXSHORTLEN)
  {
    std::string_view sv(s, l);
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
    TString* ts = createstrobj(L, s, l, h);
    g->strt.emplace(global_State::InternKey{ts->data.view(), h}, ts);
    return ts;
  }
  /* long string: no interning */
  return createstrobj(L, s, l, h);
}

/*
** Create a string ADOPTING engine-owned bytes (ByteBuilder finish):
** interning applies as usual for short strings (the buffer is simply
** discarded on a hit); otherwise the TString takes the buffer with no
** copy. Never call with bytes the host can still reach.
*/
TString* luaS_take(lua_State* L, InternalString&& str)
{
  global_State* g = G(L);
  size_t h = InternalString::hashBytes(str.bytes(), str.size());
  if (str.size() <= LUAI_MAXSHORTLEN)
  {
    global_State::InternKey key{str.view(), h};
    auto it = g->strt.find(key);
    if (it != g->strt.end())
    {
      TString* ts = it->second;
      if (isdead(g, ts))
        changewhite(ts); /* resurrect it */
      return ts;         /* 'str' is discarded */
    }
    TString* ts = createstrobj(L, std::move(str), h);
    g->strt.emplace(global_State::InternKey{ts->data.view(), h}, ts);
    return ts;
  }
  return createstrobj(L, std::move(str), h);
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
    auto it = g->strt.find(global_State::InternKey{ts->data.view(), h});
    if (it != g->strt.end() && it->second == ts) /* identity guard: a
        string that failed admission (empty payload, foreign hash) was
        never interned and must not erase a legitimate entry */
      g->strt.erase(it);
  }
}

/*
** new string (whole view, or an explicit (off, len) sub-range)
*/
TString* luaS_newlstr(lua_State* L, std::string_view str)
{
  return luaS_createstr(L, str, 0, str.size());
}

TString* luaS_newlstr(lua_State* L, std::string_view str, size_t off, size_t len)
{
  return luaS_createstr(L, str, off, len);
}

/*
** New string from an InternalString (wrapped literals and other
** engine-held strings). Deep copy as always — 'str' is never
** retained; interning may return an existing TString. (The char*
** overload and the pointer-identity strcache that fronted it are
** gone: the API layer converts by (bytes, len), engine literals are
** wrapped InternalStrings or pre-interned TStrings.)
*/
TString* luaS_new(lua_State* L, const InternalString* str)
{
  return luaS_createstr(L, str->view(), 0, str->size());
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
