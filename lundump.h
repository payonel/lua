/*
** $Id: lundump.h $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#pragma once

#include <limits.h>

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"

/* data to catch conversion errors */
inline constexpr int LUAC_INT = -0x5678;
inline constexpr Instruction LUAC_INST = 0x12345678;
inline constexpr lua_Number LUAC_NUM = -370.5;

/*
** Encode major-minor version in one byte, one nibble for each
*/
inline constexpr int LUAC_VERSION = LUA_VERSION_MAJOR_N * 16 + LUA_VERSION_MINOR_N;

inline constexpr int LUAC_FORMAT = 0; /* this is the official format */

/* load one chunk; from lundump.c */
LUAI_FUNC LClosure* luaU_undump(lua_State* L, ZIO* Z, Table* anchor, std::string_view name, int fixed);

/* dump one chunk; from ldump.c */
LUAI_FUNC int luaU_dump(lua_State* L, const Proto* f, lua_Writer w, void* data, int strip);
