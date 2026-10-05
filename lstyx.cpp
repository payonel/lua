/*
** Styx: persistence of Lua values (see docs/styx.md)
*/

#include "lua.h"

#include "lua_internal.h"
#include "lobject.h"

#include "internal_string.h"
#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "literals.h"

namespace lua::lib
{

static int styx_persist(lua_State* L)
{
  return luaL_errori(L, lt::not_implemented);
}

static int styx_unpersist(lua_State* L)
{
  return luaL_errori(L, lt::not_implemented);
}

static int styx_settings(lua_State* L)
{
  return luaL_errori(L, lt::not_implemented);
}

static CFuncInfo cfi_persist(styx_persist);
static CFuncInfo cfi_unpersist(styx_unpersist);
static CFuncInfo cfi_settings(styx_settings);

static const luaL_RegCFI funcs[] = {
  {lt::persist, &cfi_persist},
  {lt::unpersist, &cfi_unpersist},
  {lt::settings, &cfi_settings},
};

}

LUAMOD_API int luaopen_styx(lua_State* L)
{
  luaL_newlibcfi(L, lua::lib::funcs);
  return 1;
}
