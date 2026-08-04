
#pragma once

#include <string>
#include <string_view>

class HostString
{
public:
  HostString() = default;

  HostString(std::string_view s) : str(s) {}

  HostString(const char* s) : str(s) {}

  std::string_view to_utf8() const noexcept
  {
    return std::string_view(str.data(), str.size());
  }

  const char* to_os() const noexcept
  {
    return str.c_str();
  }

  static HostString from_bytes(std::string_view bytes)
  {
    HostString hs;
    hs.str = bytes;
    return hs;
  }

  bool operator==(const char* s) const noexcept { return str == s; }
  bool operator==(const HostString& o) const noexcept { return str == o.str; }

private:
  std::string str;
};

inline HostString operator""_hs(const char* s, size_t len)
{
  return HostString(std::string_view(s, len));
}
