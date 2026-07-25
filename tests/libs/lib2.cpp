#include "lua.h"
#include "lauxlib.h"

#include "lib_decls.h"

static int id (lua_State *L) {
  return lua_gettop(L);
}

static const struct luaL_Reg funcs[] = {
  {"id", id},
  {NULL, NULL}
};

LUAMOD_API int luaopen_lib2 (lua_State *L) {
  lua_settop(L, 2);
  lua_setglobal(L, "y");  /* y gets 2nd parameter */
  lua_setglobal(L, "x");  /* x gets 1st parameter */
  (luaL_checkversion(L), lua_createtable(L, 0, sizeof(funcs) / sizeof((funcs)[0]) - 1), luaL_setfuncs(L, funcs, 0));
  return 1;
}
