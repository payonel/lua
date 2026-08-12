
#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <algorithm>

#include "host_string.h"

inline int sv_byteat(std::string_view s, size_t i)
{
  return (i < s.size()) ? ((unsigned char)(s[i])) : -1;
}

struct FmtScan
{
  std::string_view fmt;
  size_t pos = 0;
  size_t litStart = 0;
  size_t litEnd = 0;
  size_t escPos = 0;
  int spec = -1;

  bool next()
  {
    litStart = pos;
    size_t e = fmt.find('%', pos);
    if (e == std::string_view::npos)
    {
      litEnd = pos = fmt.size();
      return false;
    }
    litEnd = escPos = e;
    spec = sv_byteat(fmt, e + 1);
    pos = (spec < 0) ? fmt.size() : e + 2;
    return true;
  }
};

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
    return InternalString::take(bytes_ + std::string(o));
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

struct byte_view
{
  // byte_view() = default;
  // byte_view(const char* cstr)                              : sv(cstr),      offset(0),   length(sv.size()) {}
  // byte_view(const char* zstr, size_t len)                  : sv(zstr, len), offset(0),   length(sv.size()) {}
  byte_view(std::string_view view)                         : sv(view),      offset(0),   length(sv.size()) {}
  byte_view(std::string_view view, size_t off, size_t len) : sv(view),      offset(off), length(len) {}
  byte_view(std::span<char> bytes)                         : sv(std::string_view(bytes.data(), bytes.size())), offset(0), length(bytes.size()) {}

  size_t size() const { return length; }
  const char* data() const { return sv.data() + offset; }

  bool ends_with(char c)
  {
    return length > 0 && sv.size() >= length + offset && sv.at(offset + length - 1) == c;
  }

  std::string_view view() const
  {
    return sv;
  }

  byte_view substr(size_t off, size_t len) const
  {
    byte_view cp = *this;
    cp.offset = offset + off;
    cp.length = len;
    return cp;
  }

  static size_t copy(std::string_view from, std::span<char> to)
  {
    assert(from.size() <= to.size());
    return std::copy_n(from.data(), from.size(), to.data()) - to.data();
  }

  std::string_view sv {nullptr, 0};
  size_t offset = 0;
  size_t length = 0;
};
