#pragma once

#include "packages.h"
#include "lhack.h"

struct lua_State;
int luaB_opentests(lua_State* L);
void luai_tracegctest(lua_State* L, int first);
lua_State* luai_newstate();
void luai_openlibstest(lua_State*);
