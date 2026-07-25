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
inline constexpr std::string_view LUAC_DATA = "\x19\x93\r\n\x1a\n";

inline constexpr int LUAC_INT = -0x5678;
inline constexpr Instruction LUAC_INST = 0x12345678;
inline constexpr lua_Number LUAC_NUM = -370.5;

/*
** Encode major-minor version in one byte, one nibble for each
*/
inline constexpr int LUAC_VERSION = LUA_VERSION_MAJOR_N * 16 + LUA_VERSION_MINOR_N;

/*
** Format 1 (this port): every field has a defined width and
** little-endian byte order — varints for sizes/integers, u32
** instructions, binary64 number bit patterns, per-field structs.
** No native size probes, no alignment padding, no fixed-buffer
** aliasing. Format 0 (upstream "official") chunks are rejected.
*/
inline constexpr int LUAC_FORMAT = 1;

/* load one chunk; from lundump.c */
LUAI_FUNC LClosure* luaU_undump(lua_State* L, ZIO* Z, Table* anchor,
                                const char* name);

/* dump one chunk; from ldump.c */
LUAI_FUNC int luaU_dump(lua_State* L, const Proto* f, lua_Writer w,
                        void* data, int strip);
