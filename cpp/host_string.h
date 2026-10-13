
#pragma once

#include <string>
#include "bytes.h"

class HostString
{
public:
  HostString() = default;

  HostString(const char* s) : str(s) {}
  HostString(byte_view bytes)
  {
    *this = HostString::from_bytes(bytes);
  }

  byte_view to_utf8() const noexcept
  {
    return byte_view(str.data(), str.size());
  }

  const char* to_os() const noexcept
  {
    return str.c_str();
  }

  static HostString from_bytes(byte_view bytes)
  {
    HostString s;
    s.str = std::string(bytes.data(), bytes.size());
    return s;
  }

  bool operator==(const char* s) const noexcept { return str == s; }
  bool operator==(const HostString& o) const noexcept { return str == o.str; }

private:
  std::string str;
};

inline HostString operator""_hs(const char* s, size_t len)
{
  return HostString(byte_view(s, len));
}
