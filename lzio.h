/*
** $Id: lzio.h $
** Buffered streams
** See Copyright Notice in lua.h
*/

#pragma once

#include "lua.h"

#include "literals.h"
#include "lmem.h"

constexpr int EOZ = (-1); /* end of stream */

class MBuffer
{
public:
  MBuffer() = default;
  explicit MBuffer(lua_State* L) noexcept : L_(L) {}
  MBuffer(const MBuffer&) = delete;
  MBuffer& operator=(const MBuffer&) = delete;
  ~MBuffer();

  void attach(lua_State* L) noexcept
  {
    assert(bytes_.empty() || L == L_);
    L_ = L;
  }

  void growTo(size_t cap);

  void append(char c)
  {
    if (len_ + 1 > bytes_.size())
      growTo(len_ + 1);
    bytes_[len_++] = c;
  }

  void reset() noexcept { len_ = 0; }

  void drop(size_t k) { len_ -= k; }

  size_t size() const noexcept { return len_; }

  size_t capacity() const noexcept { return bytes_.size(); }

  byte_view view() const noexcept { return {bytes_.data(), len_}; }

  byte_span area() noexcept { return bytes_; }

private:
  byte_span bytes_;
  size_t len_ = 0;
  lua_State* L_ = nullptr;
};

struct ZIO;

LUAI_FUNC void luaZ_init(lua_State* L, ZIO* z, lua_Reader* reader);
LUAI_FUNC size_t luaZ_read(ZIO* z, void* b, size_t n); /* read next n bytes */
LUAI_FUNC const void* luaZ_getaddr(ZIO* z, size_t n);

/* --------- Private Part ------------------ */

struct ZIO
{
  byte_view chunk {lt::empty_str};
  size_t pos = 0;
  lua_Reader* reader = nullptr;
  lua_State* L = nullptr;
};

LUAI_FUNC int luaZ_fill(ZIO* z);
