#include "lua.h"
#include "lauxlib.h"

#include "lib_decls.h"

static int id (lua_State *L) {
  return lua_gettop(L);
}

static const struct luaL_Reg funcs[] = {
  {"id", id}
};

/* function used by lib11.c */
LUAMOD_API int lib1_export (lua_State *L) {
  lua_pushstring(L, "exported");
  return 1;
}

LUAMOD_API int onefunction (lua_State *L) {
  luaL_checkversion(L);
  lua_settop(L, 2);
  lua_pushvalue(L, 1);
  return 2;
}

LUAMOD_API int anotherfunc (lua_State *L) {
  luaL_checkversion(L);
  lua_pushinteger(L, lua_tointeger(L, 1));
  lua_pushinteger(L, lua_tointeger(L, 2));
  lua_pushfstring(L, "%d%%%d\n");
  return 1;
} 

LUAMOD_API int luaopen_lib1_sub (lua_State *L) {
  lua_setglobal(L, "y");  /* 2nd arg: extra value (file name) */
  lua_setglobal(L, "x");  /* 1st arg: module name */
  luaL_newlib(L, funcs);
  return 1;
}

