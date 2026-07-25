/*
** $Id: lzio.h $
** Buffered streams
** See Copyright Notice in lua.h
*/

#pragma once

#include "lua.h"

#include "lmem.h"

constexpr int EOZ = (-1); /* end of stream */

typedef struct Zio ZIO;

typedef struct Mbuffer
{
  char* buffer;
  size_t n;
  size_t buffsize;
} Mbuffer;

LUAI_FUNC void luaZ_init(lua_State* L, ZIO* z, lua_Reader reader,
                         void* data);
LUAI_FUNC size_t luaZ_read(ZIO* z, void* b, size_t n); /* read next n bytes */


/* --------- Private Part ------------------ */

struct Zio
{
  size_t n;          /* bytes still unread */
  const char* p;     /* current position in buffer */
  lua_Reader reader; /* reader function */
  void* data;        /* additional data */
  lua_State* L;      /* Lua state (for reader) */
};

LUAI_FUNC int luaZ_fill(ZIO* z);
