
#pragma once

#include <string>
#include <string_view>

class HostString
{
public:
  HostString() = default;

  HostString(std::string_view s) : str(s) {}

  HostString(const char* s) : str(s) {}

  std::string_view view() const noexcept { return std::string_view(str.data(), str.size()); }

  std::string_view to_utf8() const noexcept
  {
    return std::string_view(str.data(), str.size());
  }

  bool operator==(const char* s) const noexcept { return str == s; }
  bool operator==(const HostString& o) const noexcept { return str == o.str; }

private:
  std::string str;
};
