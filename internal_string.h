
#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "host_string.h"

inline int sv_byteat(std::string_view s, size_t i)
{
  return (i < s.size()) ? ((unsigned char)(s[i])) : -1;
}

class InternalString
{
public:
  InternalString() = default;
  InternalString(const InternalString&) = delete;
  InternalString(InternalString&&) noexcept = default;
  InternalString& operator=(const InternalString&) = delete;
  InternalString& operator=(InternalString&&) noexcept = default;
  ~InternalString() = default;

  InternalString(std::string) = delete;
  InternalString(HostString) = delete;

  InternalString(std::string_view src, size_t off, size_t len)
  {
    off = std::min(off, src.size());
    len = std::min(len, src.size() - off);
    bytes_ = src.substr(off, len);
  }
  InternalString(std::string_view src) : InternalString(src, 0, src.size()) {}

  size_t size() const noexcept { return bytes_.size(); }
  bool empty() const noexcept { return bytes_.empty(); }

  int byteAt(size_t i) const { return static_cast<unsigned char>(bytes_[i]); }

  std::string_view view() const noexcept { return bytes_; }

  bool operator==(const InternalString& o) const noexcept
  {
    return bytes_ == o.bytes_;
  }

  int compare(const InternalString& o) const noexcept
  {
    return bytes_.compare(o.bytes_);
  }

  static size_t hashBytes(const char* str, size_t l) noexcept
  {
    unsigned int h = 0x5bd1e995 ^ ((unsigned int)(l));
    for (; l > 0; l--)
      h ^= ((h << 5) + (h >> 2) + ((unsigned char)(str[l - 1])));
    return h;
  }
  size_t hash() const noexcept { return hashBytes(bytes_.data(), bytes_.size()); }

  InternalString substr(size_t off, size_t len = std::string_view::npos) const
  {
    assert(off <= bytes_.size());
    return InternalString(std::string_view(bytes_), off, len);
  }

  InternalString concat(std::string_view o) const
  {
    std::string b;
    b.reserve(bytes_.size() + o.size());
    b.append(bytes_).append(o);
    InternalString is;
    is.bytes_ = std::move(b);
    return is;
  }

  static InternalString take(std::string&& s)
  {
    InternalString is;
    is.bytes_ = std::move(s);
    return is;
  }

private:
  std::string bytes_;
};


namespace istr_detail
{
template <size_t N>
struct LiteralBytes
{
  char v[N];
  constexpr LiteralBytes(const char (&s)[N])
  {
    for (size_t i = 0; i < N; i++)
      v[i] = s[i];
  }
};
}

template <istr_detail::LiteralBytes S>
const InternalString* lit()
{
  constexpr size_t len = sizeof(S.v) - 1;
  static const InternalString s(std::string_view(S.v, len), 0, len);
  return &s;
}


class ByteBuilder
{
public:
  void append(char c)
  {
    size_t off = prep(1);
    bytes_[off] = c;
    commit(1);
  }
  void append(std::string_view b)
  {
    append(b, 0, b.size());
  }
  void append(std::string_view b, size_t off, size_t n)
  {
    if (n > 0)
    {
      size_t at = prep(n);
      memcpy(&bytes_[at], b.data() + off, n);
      commit(n);
    }
  }
  void append(const InternalString* s)
  {
    append(s->view());
  }

  void reset() noexcept { len_ = 0; }

  void drop(size_t k) { len_ -= k; }

  size_t size() const noexcept { return len_; }

  std::string_view view() const noexcept { return {bytes_.data(), len_}; }

  std::span<char> area() noexcept { return {bytes_.data(), bytes_.size()}; }

  size_t prep(size_t sz)
  {
    if (len_ + sz > bytes_.size())
      growTo(len_ + sz);
    return len_;
  }
  void commit(size_t sz) noexcept { len_ += sz; }

  size_t capacity() const noexcept { return bytes_.size(); }

  void growTo(size_t cap) { bytes_.resize(cap); }

  InternalString finish() noexcept
  {
    bytes_.resize(len_);
    InternalString s = InternalString::take(std::move(bytes_));
    bytes_.clear();
    len_ = 0;
    return s;
  }

private:
  std::string bytes_;
  size_t len_ = 0;
};


