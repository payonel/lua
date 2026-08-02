/*
** $Id: lzio.h $
** Buffered streams
** See Copyright Notice in lua.h
*/

#pragma once

#include "lua.h"

#include "lmem.h"

constexpr int EOZ = (-1); /* end of stream */

struct ZIO;

LUAI_FUNC void luaZ_init(lua_State* L, ZIO* z, lua_Reader reader,
                         void* data);
LUAI_FUNC size_t luaZ_read(ZIO* z, void* b, size_t n); /* read next n bytes */

LUAI_FUNC const void* luaZ_getaddr(ZIO* z, size_t n);

/* --------- Private Part ------------------ */

struct ZIO
{
  std::string_view chunk;
  size_t pos;
  lua_Reader reader;      /* reader function */
  void* data;             /* additional data */
  lua_State* L;           /* Lua state (for reader) */
};

LUAI_FUNC int luaZ_fill(ZIO* z);
