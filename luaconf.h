/*
** $Id: luaconf.h $
** Configuration file for Lua
** See Copyright Notice in lua.h
*/

#pragma once

#include <limits.h>

#define LUA_API extern "C"
#define LUALIB_API LUA_API
#define LUAMOD_API extern "C"

constexpr int LUA_MAXINPUT = 512;
constexpr long long LUA_MAXINTEGER = LLONG_MAX;
constexpr long long LUA_MININTEGER = LLONG_MIN;
constexpr unsigned long long LUA_MAXUNSIGNED = ULLONG_MAX;

#define luai_likely(x)   (__builtin_expect(((x) != 0), 1))
#define luai_unlikely(x) (__builtin_expect(((x) != 0), 0))

/* }================================================================== */

constexpr int LUA_IDSIZE = 60;
