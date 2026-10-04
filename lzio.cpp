/*
** $Id: lzio.c $
** Buffered streams
** See Copyright Notice in lua.h
*/


#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "llimits.h"
#include "lmem.h"
#include "lstate.h"
#include "lzio.h"

namespace lua::bytes
{

int luaZ_fill(ZIO* z)
{
  lua_State* L = z->L;
  lua_unlock(L);
  std::optional<byte_view> chunk = z->reader->read(L);
  lua_lock(L);
  if (!chunk.has_value() || chunk->empty())
    return EOZ;
  z->chunk = *chunk;
  z->pos = 1;
  return ((unsigned char)(z->chunk[0]));
}

void luaZ_init(lua_State* L, ZIO* z, lua_Reader* reader)
{
  z->L = L;
  z->reader = reader;
  z->chunk = byte_view(lt::empty_str);
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

size_t luaZ_read(ZIO* z, std::span<std::byte> b)
{
  size_t n = b.size();
  size_t outpos = 0;
  while (n)
  {
    size_t m;
    if (!checkbuffer(z))
      return n; /* no more input; return number of missing bytes */
    size_t avail = z->chunk.size() - z->pos;
    m = (n <= avail) ? n : avail;
    copy_bytes(b.subspan(outpos, m), z->chunk.substr(z->pos, m));
    z->pos += m;
    outpos += m;
    n -= m;
  }
  return 0;
}

std::optional<byte_view> luaZ_getaddr(ZIO* z, size_t n)
{
  byte_view res;
  if (!checkbuffer(z))
    return std::nullopt; /* no more input */
  if (z->chunk.size() - z->pos < n) /* not enough bytes? */
    return std::nullopt; /* block not whole; cannot give an address */
  res = z->chunk.substr(z->pos, n);
  z->pos += n; /* consume these bytes */
  return res;
}

//
void MBuffer::growTo(size_t cap)
{
  if (cap == bytes_.size())
    return;
  assert(L_ != nullptr);
  bytes_ = luaM_alloc_char_span(L_->l_G, L_, bytes_, cap, true);
}

MBuffer::~MBuffer()
{
  if (bytes_.data() != nullptr)
    luaM_alloc_char_span(L_->l_G, L_, bytes_, 0, false);
}

}
