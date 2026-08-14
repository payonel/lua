
#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <algorithm>

#include "host_string.h"
#include "bytes.h"

inline int sv_byteat(byte_view s, size_t i)
{
  return (i < s.size()) ? ((unsigned char)(s[i])) : -1;
}

struct FmtScan
{
  byte_view fmt;
  size_t pos = 0;
  size_t litStart = 0;
  size_t litEnd = 0;
  size_t escPos = 0;
  int spec = -1;

  bool next()
  {
    litStart = pos;
    size_t e = fmt.find('%', pos);
    if (e == byte_view::npos)
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

  InternalString(byte_view src, size_t off, size_t len)
  {
    off = std::min(off, src.size());
    len = std::min(len, src.size() - off);
    byte_view sub = src.substr(off, len);
    bytes_ = std::string(sub.data(), sub.size());
  }
  InternalString(byte_view src) : InternalString(src, 0, src.size()) {}

  size_t size() const noexcept { return bytes_.size(); }
  bool empty() const noexcept { return bytes_.empty(); }

  int byteAt(size_t i) const { return static_cast<unsigned char>(bytes_[i]); }

  byte_view view() const noexcept { return byte_view(bytes_.data(), bytes_.size()); }

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

  InternalString substr(size_t off, size_t len = byte_view::npos) const
  {
    assert(off <= bytes_.size());
    byte_view sv(bytes_.data(), bytes_.size());
    return InternalString(sv, off, len);
  }

  InternalString concat(byte_view o) const
  {
    return InternalString::take(bytes_ + std::string(o.data(), o.size()));
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

