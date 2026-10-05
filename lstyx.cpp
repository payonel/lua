/*
** Styx: persistence of Lua values (see docs/styx.md)
*/

#include <cstring>

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

/* value tags in the persisted stream */
constexpr char TAG_NIL = 0;
constexpr char TAG_FALSE = 1;
constexpr char TAG_TRUE = 2;
constexpr char TAG_INTEGER = 3;
constexpr char TAG_FLOAT = 4;

static void put_raw(luaL_Buffer* B, const void* p, size_t n)
{
  luaL_addlstring(B, byte_view(static_cast<const char*>(p), n), 0, n);
}

static void persist_value(lua_State* L, luaL_Buffer* B, int idx)
{
  switch (lua_type(L, idx))
  {
  case LUA_TNIL:
    luaL_addchar(B, TAG_NIL);
    break;
  case LUA_TBOOLEAN:
    luaL_addchar(B, lua_toboolean(L, idx) ? TAG_TRUE : TAG_FALSE);
    break;
  case LUA_TNUMBER:
    if (lua_isinteger(L, idx))
    {
      lua_Integer i = lua_tointeger(L, idx);
      luaL_addchar(B, TAG_INTEGER);
      put_raw(B, &i, sizeof(i));
    }
    else
    {
      lua_Number n = lua_tonumber(L, idx);
      luaL_addchar(B, TAG_FLOAT);
      put_raw(B, &n, sizeof(n));
    }
    break;
  default:
    lua_pushstring(L, luaL_typename(L, idx));
    luaL_errori(L, lt::cannot_persist_a_s_value);
  }
}

struct Reader
{
  lua_State* L;
  byte_view data;
  size_t pos = 0;

  void need(size_t n)
  {
    if (data.size() - pos < n)
      luaL_errori(L, lt::invalid_persisted_data);
  }

  char get_byte()
  {
    need(1);
    return data[pos++];
  }

  void get_raw(void* p, size_t n)
  {
    need(n);
    std::memcpy(p, data.substr(pos, n).data(), n);
    pos += n;
  }
};

static void unpersist_value(Reader& r)
{
  lua_State* L = r.L;
  switch (r.get_byte())
  {
  case TAG_NIL:
    lua_pushnil(L);
    break;
  case TAG_FALSE:
    lua_pushboolean(L, 0);
    break;
  case TAG_TRUE:
    lua_pushboolean(L, 1);
    break;
  case TAG_INTEGER:
  {
    lua_Integer i;
    r.get_raw(&i, sizeof(i));
    lua_pushinteger(L, i);
    break;
  }
  case TAG_FLOAT:
  {
    lua_Number n;
    r.get_raw(&n, sizeof(n));
    lua_pushnumber(L, n);
    break;
  }
  default:
    luaL_errori(L, lt::invalid_persisted_data);
  }
}

/* persist([perms,] value) */
static int styx_persist(lua_State* L)
{
  luaL_checkany(L, 1);
  int value = 1;
  if (lua_gettop(L) >= 2)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    value = 2;
  }
  luaL_Buffer B;
  luaL_buffinit(L, &B);
  luaL_addlstring(&B, lt::styx_header, 0, lt::styx_header.size());
  persist_value(L, &B, value);
  luaL_pushresult(&B);
  return 1;
}

/* unpersist([perms,] string) */
static int styx_unpersist(lua_State* L)
{
  int arg = 1;
  if (lua_gettop(L) >= 2)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    arg = 2;
  }
  Reader r{L, luaL_checklstring(L, arg)};
  const byte_view header = lt::styx_header;
  if (r.data.size() < header.size() || r.data.substr(0, header.size()) != header)
    return luaL_errori(L, lt::invalid_persisted_data);
  r.pos = header.size();
  unpersist_value(r);
  if (r.pos != r.data.size())
    return luaL_errori(L, lt::invalid_persisted_data);
  return 1;
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
