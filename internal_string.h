
#pragma once

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

  InternalString(const char* bytes, size_t len) : bytes_(bytes, len) {}

  InternalString(std::string_view src, size_t off, size_t len)
      : bytes_(src.data() + off, len) {}

  explicit InternalString(const HostString& host) : bytes_(host.to_utf8()) {}

  explicit InternalString(std::string&& engineOwned) noexcept
      : bytes_(std::move(engineOwned))
  {
  }

  size_t size() const noexcept { return bytes_.size(); }
  bool empty() const noexcept { return bytes_.empty(); }

  int byteAt(size_t i) const { return static_cast<unsigned char>(bytes_[i]); }

  std::string_view view() const noexcept { return bytes_; }

  const char* bytes() const noexcept { return bytes_.data(); }

  const char* c_str() const noexcept { return bytes_.c_str(); }

  size_t capacity() const noexcept { return bytes_.capacity(); }

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
  static const InternalString s(S.v, sizeof(S.v) - 1);
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
    InternalString s(std::move(bytes_));
    bytes_.clear();
    len_ = 0;
    return s;
  }

private:
  std::string bytes_;
  size_t len_ = 0;
};


constexpr const char* operator""_os (const char* s, std::size_t n)
{
  (void)n;
  return reinterpret_cast<const char*>(s);
}
