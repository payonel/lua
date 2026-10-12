/*
** The freeze gate: runs a Lua script on the main thread with a step
** budget whose callback freezes at every budget point, and thaws it from
** the host until it ends. The script must end as it does unbudgeted (the
** upstream suite and the deep tests are run this way: 'make gate').
**
** With '--styx <n>', the Styx gate: at every n-th freeze the host also
** persists the whole state (luaL_persiststate, from its other thread),
** restores it into a new state (luaL_restorestate), closes the old one
** and goes on in the new one. A freeze where the state holds what cannot
** be persisted (an open file) is not persisted: it is counted and the next
** freeze is tried. The script must end as it does with no persist ('make
** styx-gate'); the upstream suite runs in its portable mode ('_port').
** Each restore must keep every object: a census of the original's objects
** and of the restored state's, by type, must match (see 'census').
**
** usage: gate <steps> [--styx <n>] <script.lua> [args...]
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lstate.h"
#include "lgc.h"

static long freezes = 0;

static std::string str(lua_State* L, int idx)
{
  auto s = lua_tolstring(L, idx);
  return s ? std::string(s->data(), s->size()) : std::string("(no message)");
}

static void freezer(lua_State*, lua_State* spender)
{
  freezes++;
  lua_freeze(spender);
}

/* the permanents: every C function and userdata reachable from the
   registry and the string metatable when the libraries are open, and the
   metatable of each such userdata (a library checks its userdata against
   it: 'luaL_checkudata'), named by its path (keys in sorted order, so that
   two states with the same libraries agree); kept in the registry
   (_GATEPERMS, object -> name), and the inverse (name -> object) is
   returned */
static const char* const perms_walk =
  "local perms, seen = {}, {} "
  "local function upv(f) local _, x = debug.getupvalue(f, 1) return x end "
  "local function walk(t, name) if seen[t] then return end seen[t] = true "
  "  local keys = {} for k in pairs(t) do keys[#keys + 1] = k end "
  "  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end) "
  "  for _, k in ipairs(keys) do local v = t[k] local n = name .. '.' .. tostring(k) "
  "    if (type(v) == 'function' and debug.getinfo(v, 'S').what == 'C' and type(upv(v)) ~= 'table') "
  "       or type(v) == 'userdata' then if perms[v] == nil then perms[v] = n end "
  "      local mt = type(v) == 'userdata' and debug.getmetatable(v) "
  "      if mt and perms[mt] == nil then perms[mt] = n .. '<mt>' end "
  "    elseif type(v) == 'table' then walk(v, n) end end "
  "  local mt = getmetatable(t) if mt then walk(mt, name .. '<mt>') end end "
  "walk(getmetatable(''), 'smt') walk(debug.getregistry(), 'R') "
  "local back = {} for v, n in pairs(perms) do back[n] = v end "
  "debug.getregistry()._GATEPERMS = perms return back";

/* the message handler of the standalone interpreter: a traceback */
static int msghandler(lua_State* L)
{
  auto msg = lua_tolstring(L, 1);
  if (!msg)
  {
    lua_pushstring(L, "(error object is not a string)");
    return 1;
  }
  luaL_traceback(L, L, std::string(msg->data(), msg->size()).c_str(), 1);
  return 1;
}

/* what the host provides a state: the libraries, its message handler (in
   the registry, so that it has a name), and the permanents (the inverse
   left on the stack) */
struct Provide : luaL_RestoreSetup
{
  void setup(lua_State* L) override
  {
    luaL_openlibs(L);
    lua_pushcfunction(L, msghandler);
    lua_setfield(L, LUA_REGISTRYINDEX, "_GATEMSGH");
    if (luaL_loadstring(L, perms_walk) != LUA_OK)
      lua_error(L);
    lua_call(L, 0, 1);
  }
};

struct StringWriter : lua_Writer
{
  std::string out;
  int write(lua_State*, std::optional<byte_view> b) override
  {
    if (b.has_value())
      out.append(b->data(), b->size());
    return 0;
  }
};

/*
** The objects a state's collector keeps, by type (a census): live objects
** and dead ones waiting for their '__gc'. Taken after a whole cycle that
** calls no '__gc': the restore ends with one, and the original gets one
** (it is closed next). Not counted, as not persisted: an open upvalue no
** closure refers to any more (its thread keeps it until its frame
** returns), and in the original, the half-made parse of a stopped load,
** held by its box (the restored load makes it again as it parses its
** kept input again).
*/
static std::map<int, long> census(lua_State* L, bool original)
{
  using namespace lua::object;
  lua::runtime::global_State* g = L->l_G;
  if (original)
  {
    for (GCObject* o = g->allgc.next; o != nullptr; o = o->next)
      if (o->tt == LUA_VUSERDATA)
      {
        auto* u = static_cast<Udata*>(o);
        const char* k = u->obj ? u->obj->persistkind() : nullptr;
        if (k != nullptr && std::strcmp(k, "load") == 0)
          for (TValue& v : u->uv)
            setnilvalue(&v);
      }
    lua::gc::luaC_fullgcdue(L);
  }
  std::vector<GCObject*> all;
  auto collect = [&all](GCObject* o) {
    for (; o != nullptr; o = o->next)
      all.push_back(o);
  };
  collect(g->allgc.next);
  collect(g->finobj.next);
  collect(g->tobefnz.next);
  collect(g->fixedgc);
  for (BudgetGroup* b : g->groupbyid)
    if (b != nullptr)
      collect(b->fnz.next);
  std::set<GCObject*> used; /* (the upvalues the closures refer to) */
  for (GCObject* o : all)
    if (o->tt == LUA_VLCL)
      for (UpVal* u : static_cast<LClosure*>(o)->upvals)
        used.insert(u);
  std::map<int, long> n;
  for (GCObject* o : all)
    if (o->tt != LUA_VUPVAL || used.count(o) != 0)
      n[o->tt]++;
  return n;
}

static long censuses = 0;
static long persists = 0;
static long unpersistable = 0;
static std::map<std::string, long> why; /* what could not be persisted, and how often */
static std::map<std::string, std::string> first; /* the first such message in full */
static size_t largest = 0;

/*
** Persist the state of 'L' (its main thread frozen) from the host's other
** thread '*H', restore it into a new state and close the old one: '*L' and
** '*H' are then the new state's. False if it could not be persisted.
*/
static bool persist_restore(lua_State** L, lua_State** H)
{
  StringWriter w;
  lua_getfield(*H, LUA_REGISTRYINDEX, "_GATEPERMS");
  luaL_StyxResult res = luaL_persiststate(*H, lua_gettop(*H), &w);
  lua_pop(*H, 1);
  if (res.status != LUA_OK)
  {
    std::string m(res.msg.to_os());
    size_t at = m.find(" (at ");
    std::string kind = at == std::string::npos ? m : m.substr(0, at);
    if (why[kind]++ == 0)
      first[kind] = m;
    unpersistable++;
    return false;
  }
  if (w.out.size() > largest)
    largest = w.out.size();
  Provide provide;
  luaL_Restored rd = luaL_restorestate(lua_getallocf(*L), byte_view(w.out.data(), w.out.size()), &provide);
  lua_State* H2 = rd.L;
  if (H2 == nullptr)
  {
    std::fprintf(stderr, "gate: restore failed after %ld freezes: %s\n", freezes, rd.msg.to_os());
    if (const char* dump = std::getenv("GATE_DUMP")) /* (the stream, to look into) */
      if (FILE* f = std::fopen(dump, "wb"))
      {
        std::fwrite(w.out.data(), 1, w.out.size(), f);
        std::fclose(f);
      }
    std::exit(1);
  }
  if (lua_gc(H2, LUA_GCISRUNNING, 0, 0, 0))
  { /* (the same objects in the original and in the restored state) */
    std::map<int, long> before = census(*L, true), after = census(H2, false);
    if (before != after)
    {
      std::fprintf(stderr, "gate: a restore lost or gained objects after %ld freezes (type: before after)\n", freezes);
      for (const auto& [t, k] : before)
        if (after[t] != k)
          std::fprintf(stderr, "gate:   %d: %ld %ld\n", t, k, after[t]);
      for (const auto& [t, k] : after)
        if (before.count(t) == 0)
          std::fprintf(stderr, "gate:   %d: 0 %ld\n", t, k);
      std::exit(1);
    }
    censuses++;
  }
  lua_close(*L);
  *H = H2;
  lua_pop(H2, 1); /* (its stack is as it was persisted: the permanents pushed for the persist too) */
  lua_rawgeti(H2, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
  *L = lua_tothread(H2, -1);
  lua_pop(H2, 1);
  persists++;
  return true;
}

int main(int argc, char** argv)
{
  lua_registerbudget("gate.freezer", freezer);
  long every = 0; /* (persist and restore at every n-th freeze) */
  if (argc >= 5 && std::strcmp(argv[2], "--styx") == 0)
  {
    every = std::atol(argv[3]);
    for (int i = 4; i <= argc; i++) /* (drop the option) */
      argv[i - 2] = argv[i];
    argc -= 2;
  }
  if (argc < 3)
  {
    std::fprintf(stderr, "usage: %s <steps> [--styx <n>] <script.lua> [args...]\n", argv[0]);
    return 2;
  }
  long steps = std::atol(argv[1]);
  const char* script = argv[2];
  lua_State* L = luaL_newstate();
  Provide().setup(L);
  lua_pop(L, 1);
  lua_State* H = lua_newthread(L); /* (the host's other thread, for a frozen main thread) */
  lua_setfield(L, LUA_REGISTRYINDEX, "_GATEHOST");
  /* the 'arg' table, as the standalone interpreter makes it (the tests that
     run a child interpreter find it in arg[-1]: GATE_LUA, '../lua') */
  lua_createtable(L, argc - 3, 2);
  const char* interp = std::getenv("GATE_LUA");
  lua_pushstring(L, interp != nullptr ? interp : "../lua");
  lua_rawseti(L, -2, -1);
  for (int i = 2; i < argc; i++)
  {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - 2);
  }
  lua_setglobal(L, "arg");
  if (luaL_dostring(L, "_TMP = '_1'; GATE = true") != LUA_OK)
    return 2;
  /* (the Styx gate runs the upstream suite in its portable mode: the C
     libraries its other tests load hold C functions that have no names,
     so no persist would be made after them; the freeze gate runs those) */
  if (every > 0 && luaL_dostring(L, "_port = true") != LUA_OK)
    return 2;
  lua_warning(L, "@on", 0);
  lua_getfield(L, LUA_REGISTRYINDEX, "_GATEMSGH");
  int base = lua_gettop(L);
  if (luaL_loadfile(L, script) != LUA_OK)
  {
    std::fprintf(stderr, "gate: %s\n", str(L, -1).c_str());
    return 2;
  }
  for (int i = 3; i < argc; i++)
    lua_pushstring(L, argv[i]);
  lua_setbudget(L, steps, freezer);
  int st = lua_pcall(L, argc - 3, 0, base);
  long thaws = 0;
  long next = every;
  while (st == LUA_OK && lua_isfrozen(L))
  {
    if (every > 0 && freezes >= next)
    {
      next = freezes + (persist_restore(&L, &H) ? every : 1);
      if (lua_gettop(H) != 0)
      { /* (what the host leaves on its thread is persisted with it, every time) */
        std::fprintf(stderr, "gate: the host's thread holds %d values after %ld persists\n", lua_gettop(H), persists);
        return 1;
      }
    }
    st = lua_thaw(L, nullptr).status;
    thaws++;
  }
  lua_setbudget(L, 0, nullptr);
  if (st != LUA_OK)
  {
    std::fprintf(stderr, "gate: failed after %ld freezes: %s\n", freezes, str(L, -1).c_str());
    return 1;
  }
  std::printf("gate: ok (%ld steps per budget point, %ld freezes, %ld thaws)\n", steps, freezes, thaws);
  if (every > 0)
  {
    std::printf("gate: %ld persists and restores (every %ld freezes; largest %zu bytes), "
                "%ld freezes not persisted\n", persists, every, largest, unpersistable);
    for (const auto& [m, n] : why)
      std::printf("gate:   %ld x %s (first: %s)\n", n, m.c_str(), first[m].c_str());
    std::printf("gate: census: %ld of %ld restores, every object kept\n", censuses, persists);
  }
  lua_close(L);
  return 0;
}
