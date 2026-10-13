#include "locale_util.h"

#include <cctype>
#include <clocale>

namespace lua::platform
{

bool locale_matchclass(int c, int cls)
{
  switch (cls)
  {
  case 'a': return isalpha(c);
  case 'c': return iscntrl(c);
  case 'd': return isdigit(c);
  case 'g': return isgraph(c);
  case 'l': return islower(c);
  case 'p': return ispunct(c);
  case 's': return isspace(c);
  case 'u': return isupper(c);
  case 'w': return isalnum(c);
  case 'x': return isxdigit(c);
  case 'z': return (c == 0);
  default: return false;
  }
}

int locale_tolower(int c)
{
  return tolower(c);
}

int locale_toupper(int c)
{
  return toupper(c);
}

bool locale_islower(int c)
{
  return islower(c);
}

bool locale_isalpha(int c)
{
  return isalpha(c);
}

bool locale_isdigit(int c)
{
  return isdigit(c);
}

bool locale_isalnum(int c)
{
  return isalnum(c);
}

bool locale_isxdigit(int c)
{
  return isxdigit(c);
}

bool locale_isspace(int c)
{
  return isspace(c);
}

char locale_radixpoint()
{
  return localeconv()->decimal_point[0];
}

std::optional<byte_view> locale_setlocale(int category, std::optional<byte_view> locale)
{
  const char* res;
  if (locale.has_value())
  {
    std::string l(locale->data(), locale->size());
    res = setlocale(category, l.c_str());
  }
  else
    res = setlocale(category, nullptr);
  if (res == nullptr)
    return std::nullopt;
  return byte_view(res);
}

}
