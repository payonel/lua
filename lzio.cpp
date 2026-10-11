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
#include "lload.h"

namespace lua::bytes
{

int luaZ_fill(ZIO* z)
{
  lua_State* L = z->L;
  std::optional<byte_view> chunk;
  if (l_unlikely(z->resuming))
  { /* the read a freeze stopped: the reader function gave its piece */
    luaZ_restore(z->rz, LF_FILL, nullptr, 0);
    luaZ_resumed(z->rz);
    lua_unlock(L);
    chunk = luaZ_resumeread(z->rz);
    lua_lock(L);
  }
  else
  {
    lua_unlock(L);
    try
    {
      chunk = z->reader->read(L);
    }
    catch (...)
    {
      luaZ_record(z->rz, LF_FILL, 1, nullptr, 0);
      throw;
    }
    lua_lock(L);
  }
  if (!chunk.has_value() || chunk->empty())
    return EOZ;
  z->chunk = *chunk;
  z->pos = 1;
  return (unsigned char)z->chunk[0];
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
  struct
  {
    int c;
  } l;
  if (l_unlikely(z->resuming))
  {
    luaZ_restore(z->rz, LF_CHECKBUFFER, &l, sizeof(l));
    goto site_1;
  }
  if (z->pos == z->chunk.size())
  { /* no bytes in buffer? */
  site_1:
    try
    {
      l.c = luaZ_fill(z); /* try to read more */
    }
    catch (...)
    {
      luaZ_record(z->rz, LF_CHECKBUFFER, 1, &l, sizeof(l));
      throw;
    }
    if (l.c == EOZ)
      return 0; /* no more input */
    else
      z->pos--; /* luaZ_fill consumed first byte; put it back */
  }
  return 1; /* now buffer has something */
}

size_t luaZ_read(ZIO* z, std::span<std::byte> b)
{
  struct
  {
    size_t n, outpos;
  } l;
  int more;
  if (l_unlikely(z->resuming))
  {
    luaZ_restore(z->rz, LF_ZREAD, &l, sizeof(l));
    goto site_1;
  }
  l.n = b.size();
  l.outpos = 0;
  while (l.n)
  {
    size_t m;
  site_1:
    try
    {
      more = checkbuffer(z);
    }
    catch (...)
    {
      luaZ_record(z->rz, LF_ZREAD, 1, &l, sizeof(l));
      throw;
    }
    if (!more)
      return l.n; /* no more input; return number of missing bytes */
    size_t avail = z->chunk.size() - z->pos;
    m = (l.n <= avail) ? l.n : avail;
    copy_bytes(b.subspan(l.outpos, m), z->chunk.substr(z->pos, m));
    z->pos += m;
    l.outpos += m;
    l.n -= m;
  }
  return 0;
}

std::optional<byte_view> luaZ_getaddr(ZIO* z, size_t n)
{
  byte_view res;
  int more;
  if (l_unlikely(z->resuming))
  {
    luaZ_restore(z->rz, LF_GETADDR, nullptr, 0);
    goto site_1;
  }
site_1:
  try
  {
    more = checkbuffer(z);
  }
  catch (...)
  {
    luaZ_record(z->rz, LF_GETADDR, 1, nullptr, 0);
    throw;
  }
  if (!more)
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
