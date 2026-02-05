#include "lua.h"

#include "lib_decls.h"

LUAMOD_API int luaopen_lib21 (lua_State *L) {
  return luaopen_lib2(L);
}
