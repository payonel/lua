/*
** $Id: lzio.h $
** Buffered streams
** See Copyright Notice in lua.h
*/

#pragma once

#include "lua.h"

#include "lmem.h"

constexpr int EOZ = (-1); /* end of stream */

/*
** Internal byte buffer, the analog of stock 'Mbuffer'. Memory is
** managed through luaM with realloc semantics, so growth gets the
** emergency-GC retry and throws LUA_ERRMEM internally on failure —
** callers never check. GC-debt accounting is inherent to luaM.
** Bound to a lua_State; freed on destruction (unwind-safe).
*/
class MBuffer
{
public:
  MBuffer() = default;
  explicit MBuffer(lua_State* L) noexcept : L_(L) {}
  MBuffer(const MBuffer&) = delete;
  MBuffer& operator=(const MBuffer&) = delete;
  ~MBuffer()
  {
    if (bytes_.data() != nullptr)
      luaM_free_(L_, bytes_.data(), bytes_.size());
  }

  /* bind to a state; must happen before any growth */
  void attach(lua_State* L) noexcept
  {
    assert(bytes_.empty() || L == L_);
    L_ = L;
  }

  /* resize to exactly 'cap'; throws LUA_ERRMEM internally on failure */
  void growTo(size_t cap)
  {
    if (cap == bytes_.size())
      return;
    assert(L_ != nullptr);
    void* nb = luaM_saferealloc_(L_, bytes_.data(), bytes_.size(), cap);
    bytes_ = {static_cast<char*>(nb), cap};
  }

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

  std::string_view view() const noexcept { return {bytes_.data(), len_}; }

  std::span<char> area() noexcept { return bytes_; }

private:
  std::span<char> bytes_;
  size_t len_ = 0;
  lua_State* L_ = nullptr;
};

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
