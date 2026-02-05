#pragma once

struct lua_State;

extern "C" int lib1_export (lua_State *L);
extern "C" int onefunction (lua_State *L);
extern "C" int anotherfunc (lua_State *L);
extern "C" int luaopen_lib1_sub (lua_State *L);
extern "C" int luaopen_lib11 (lua_State *L);
extern "C" int luaopen_lib2 (lua_State *L);
extern "C" int luaopen_lib21 (lua_State *L);
