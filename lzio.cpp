/*
** $Id: lzio.c $
** Buffered streams
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "llimits.h"
#include "lmem.h"
#include "lstate.h"
#include "lzio.h"

int luaZ_fill(ZIO* z)
{
  size_t size;
  lua_State* L = z->L;
  const char* buff;
  lua_unlock(L);
  buff = z->reader(L, z->data, &size);
  lua_lock(L);
  if (buff == nullptr || size == 0)
    return EOZ;
  /* reader boundary: adopt the piece as a view (Java: the reader hands
     back a byte[] the ZIO scans by offset) */
  z->chunk = std::string_view{buff, size};
  z->pos = 1; /* consuming the first byte, returned below */
  return ((unsigned char)(z->chunk[0]));
}

void luaZ_init(lua_State* L, ZIO* z, lua_Reader reader, void* data)
{
  z->L = L;
  z->reader = reader;
  z->data = data;
  z->chunk = std::string_view{};
  z->pos = 0;
}

/* --------------------------------------------------------------- read --- */

static int checkbuffer(ZIO* z)
{
  if (z->pos == z->chunk.size())
  {                          /* no bytes in buffer? */
    if (luaZ_fill(z) == EOZ) /* try to read more */
      return 0;              /* no more input */
    else
      z->pos--; /* luaZ_fill consumed first byte; put it back */
  }
  return 1; /* now buffer has something */
}

size_t luaZ_read(ZIO* z, void* b, size_t n)
{
  char* out = (char*) b; /* destination bytes (raw-memory boundary) */
  size_t outpos = 0;
  while (n)
  {
    size_t m;
    if (!checkbuffer(z))
      return n; /* no more input; return number of missing bytes */
    size_t avail = z->chunk.size() - z->pos;
    m = (n <= avail) ? n : avail; /* min. between n and available bytes */
    /* Java: System.arraycopy(chunk, pos, out, outpos, m) */
    memcpy(out + outpos, z->chunk.data() + z->pos, m);
    z->pos += m;
    outpos += m;
    n -= m;
  }
  return 0;
}

const void* luaZ_getaddr(ZIO* z, size_t n)
{
  const void* res;
  if (!checkbuffer(z))
    return nullptr; /* no more input */
  if (z->chunk.size() - z->pos < n) /* not enough bytes? */
    return nullptr; /* block not whole; cannot give an address */
  /* fixed-buffer aliasing boundary (mode 'B', a C++-side optimization:
     the managed port loads copy-wise and never takes this address) */
  res = z->chunk.data() + z->pos;
  z->pos += n; /* consume these bytes */
  return res;
}
