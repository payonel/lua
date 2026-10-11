/*
** $Id: lzio.h $
** Buffered streams
** See Copyright Notice in lua.h
*/

#pragma once

#include "lua.h"

#include "literals.h"
#include "lmem.h"

namespace lua::bytes
{

constexpr int EOZ = -1; /* end of stream */

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

  /* its bytes (and their count in '*len') go elsewhere; it is left empty */
  byte_owner release(size_t* len) noexcept
  {
    *len = len_;
    len_ = 0;
    byte_owner b = std::move(bytes_);
    bytes_ = {};
    return b;
  }

  /* takes bytes given by 'release' */
  void adopt(byte_owner b, size_t len) noexcept
  {
    assert(bytes_.empty());
    bytes_ = std::move(b);
    len_ = len;
  }

private:
  byte_owner bytes_;
  size_t len_ = 0;
  lua_State* L_ = nullptr;
};

struct ZIO;

void luaZ_init(lua_State* L, ZIO* z, lua_Reader* reader);
size_t luaZ_read(ZIO* z, std::span<std::byte> b); /* read next n bytes */
std::optional<byte_view> luaZ_getaddr(ZIO* z, size_t n);

template <typename T>
std::span<T> luaZ_overlay(byte_view block)
{
  return std::span<T>(reinterpret_cast<T*>(const_cast<char*>(block.data())), block.size() / sizeof(T));
}

/* --------- Private Part ------------------ */

struct LoadResume;

struct ZIO
{
  byte_view chunk {lt::empty_str};
  size_t pos = 0;
  lua_Reader* reader = nullptr;
  lua_State* L = nullptr;
  LoadResume* rz = nullptr; /* a load that a freeze can stop (see lload.h) */
  bool resuming = false;    /* (its frames are being made again) */
};

int luaZ_fill(ZIO* z);

}
