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
  { /* the read a freeze stopped (the leaf of the frames): the reader function gave its piece */
    luaZ_restore(z->rz, LF_FILL);
    luaZ_resumed(z->rz);
    lua_unlock(L);
    chunk = luaZ_resumeread(z->rz);
    lua_lock(L);
  }
  else
  {
    LoadFrame l;
    lua_unlock(L);
    try
    {
      chunk = z->reader->read(L);
    }
    catch (...)
    {
      luaZ_record(z->rz, LF_FILL, l);
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

/*
** (With no frame of its own, in a load that a freeze can stop: made
** again, it goes the same way, the buffer empty, see lload.h.)
*/
static bool checkbuffer(ZIO* z)
{
  if (z->pos == z->chunk.size())
  {                        /* no bytes in buffer? */
    if (luaZ_fill(z) == EOZ) /* try to read more */
      return false;            /* no more input */
    else
      z->pos--; /* luaZ_fill consumed first byte; put it back */
  }
  return true; /* now buffer has something */
}

/* the frame of 'luaZ_read' (see lload.h) */
struct ReadFrame : LoadFrame
{
  size_t n = 0;
  size_t outpos = 0;
  LoadFrame* copy() const override { return new (std::nothrow) ReadFrame(*this); }
  size_t size() const override { return sizeof(*this); }
};

size_t luaZ_read(ZIO* z, std::span<std::byte> b)
{
  ReadFrame l;
  if (l_unlikely(z->resuming))
    l = static_cast<const ReadFrame&>(luaZ_restore(z->rz, LF_ZREAD));
  try
  {
    for (;;)
    {
      switch (l.site)
      {
      case 0:
        l.n = b.size();
        l.site = 1;
        [[fallthrough]];
      case 1: /* the loop */
        if (l.n == 0)
          return 0;
        l.site = 2;
        [[fallthrough]];
      case 2:
      {
        if (!checkbuffer(z))
          return l.n; /* no more input; return number of missing bytes */
        size_t avail = z->chunk.size() - z->pos;
        size_t m = (l.n <= avail) ? l.n : avail;
        copy_bytes(b.subspan(l.outpos, m), z->chunk.substr(z->pos, m));
        z->pos += m;
        l.outpos += m;
        l.n -= m;
        l.site = 1;
        continue;
      }
      default:
        luaZ_badstate(z->rz);
      }
    }
  }
  catch (...)
  {
    luaZ_record(z->rz, LF_ZREAD, l);
    throw;
  }
}

/* (with no frame of its own: see 'checkbuffer') */
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

byte_owner MBuffer::handover()
{
  if (bytes_.data() != nullptr)
    luaM_alloc_char_span(L_->l_G, L_, bytes_, 0, false);
  len_ = 0;
  byte_owner b = std::move(bytes_);
  bytes_ = {};
  return b;
}

MBuffer::~MBuffer()
{
  if (bytes_.data() != nullptr)
    luaM_alloc_char_span(L_->l_G, L_, bytes_, 0, false);
}

}
