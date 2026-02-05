#include "lua.h"

#include "lib_decls.h"

LUAMOD_API int luaopen_lib11 (lua_State *L) {
  return lib1_export(L);
}
