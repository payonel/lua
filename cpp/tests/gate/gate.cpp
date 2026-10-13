/*
** The freeze gate: runs a Lua script on the main thread with a step
** budget whose callback freezes at every budget point, and thaws it from
** the host until it ends. The script must end as it does unbudgeted (the
** upstream suite and the deep tests are run this way: 'make gate').
**
** usage: gate <steps> <script.lua> [args...]
*/
#include <cstdio>
#include <cstdlib>
#include <string>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

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

int main(int argc, char** argv)
{
  lua_registerbudget("gate.freezer", freezer);
  if (argc < 3)
  {
    std::fprintf(stderr, "usage: %s <steps> <script.lua> [args...]\n", argv[0]);
    return 2;
  }
  long steps = std::atol(argv[1]);
  const char* script = argv[2];
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  lua_pushcfunction(L, msghandler);
  lua_setfield(L, LUA_REGISTRYINDEX, "_GATEMSGH");
  /* the 'arg' table, as the standalone interpreter makes it (the tests that
     run a child interpreter find it in arg[-1]: GATE_LUA, '../cpp/lua') */
  lua_createtable(L, argc - 3, 2);
  const char* interp = std::getenv("GATE_LUA");
  lua_pushstring(L, interp != nullptr ? interp : "../cpp/lua");
  lua_rawseti(L, -2, -1);
  for (int i = 2; i < argc; i++)
  {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - 2);
  }
  lua_setglobal(L, "arg");
  if (luaL_dostring(L, "_TMP = '_1'; GATE = true") != LUA_OK)
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
  while (st == LUA_OK && lua_isfrozen(L))
  {
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
  lua_close(L);
  return 0;
}
