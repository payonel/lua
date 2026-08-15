/* implementation for lib2-v2 */

#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"

#include "lib_decls.h"

static int id (lua_State *L) {
  lua_pushboolean(L, 1);
  lua_insert(L, 1);
  return lua_gettop(L);
}


static int newstr (lua_State *L) {
  size_t len;
  byte_view sv = luaL_checklstring(L, 1);
  const char *str = sv.data();
  len = sv.size();
  char *buf = (char*)malloc(len + 1);
  if (buf == NULL) {
    lua_pushliteral(L, "not enough memory");
    lua_error(L);
  }
  memcpy(buf, str, len + 1);
  lua_pushlstring(L, byte_view{buf, len}, 0, len);
  free(buf);
  return 1;
}


static void initstr (lua_State *L) {
  lua_pushcfunction(L, newstr);
  lua_pushstring(L,
     "012345678901234567890123456789012345678901234567890123456789");
  lua_call(L, 1, 1);  /* call newstr("0123...") */
  luaL_ref(L, LUA_REGISTRYINDEX);  /* keep string in the registry */
}


static const struct luaL_Reg funcs[] = {
  {"id", id},
  {"newstr", newstr}
};

LUAMOD_API int luaopen_lib2 (lua_State *L) {
  lua_settop(L, 2);
  lua_setglobal(L, "y");  /* y gets 2nd parameter */
  lua_setglobal(L, "x");  /* x gets 1st parameter */
  initstr(L);
  luaL_newlib(L, funcs);
  return 1;
}
