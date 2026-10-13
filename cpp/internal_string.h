
#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <algorithm>

#include "host_string.h"
#include "bytes.h"

namespace lua::bytes
{

inline int sv_byteat(byte_view s, size_t i)
{
  return (i < s.size()) ? (unsigned char)s[i] : -1;
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
    bytes_ = byte_owner(src.substr(off, len));
    len_ = len;
  }
  InternalString(byte_view src) : InternalString(src, 0, src.size()) {}

  /* takes 'block' as its bytes: the first 'len' of them */
  InternalString(byte_owner&& block, size_t len) noexcept : bytes_(std::move(block)), len_(len)
  {
    assert(len <= bytes_.size());
  }

  size_t size() const noexcept { return len_; }
  bool empty() const noexcept { return len_ == 0; }

  int byteAt(size_t i) const { return static_cast<unsigned char>(bytes_.data()[i]); }

  byte_view view() const noexcept { return byte_view(bytes_.data(), len_); }

  /* its bytes, for its maker to write before the string is shared */
  byte_span area() const noexcept { return bytes_.span().subspan(0, len_); }

  bool operator==(const InternalString& o) const noexcept
  {
    return view() == o.view();
  }

  int compare(const InternalString& o) const noexcept
  {
    return std::string_view(bytes_.data(), len_).compare(std::string_view(o.bytes_.data(), o.len_));
  }

  static size_t hashBytes(const char* str, size_t l) noexcept
  {
    unsigned int h = 0x5bd1e995 ^ (unsigned int)l;
    for (size_t i = 0; i < l; i++)
      h ^= (h << 5) + (h >> 2) + (unsigned char)str[i];
    return h;
  }
  size_t hash() const noexcept { return hashBytes(bytes_.data(), len_); }

  InternalString substr(size_t off, size_t len = byte_view::npos) const
  {
    assert(off <= len_);
    return InternalString(view(), off, len);
  }

  InternalString concat(byte_view o) const
  {
    byte_owner b(len_ + o.size());
    b.copy_from(view(), 0);
    b.copy_from(o, len_);
    return InternalString(std::move(b), len_ + o.size());
  }

private:
  byte_owner bytes_;
  size_t len_ = 0;
};

}
