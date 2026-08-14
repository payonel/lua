#include "locale_util.h"

#include <cctype>
#include <clocale>
#include <string>

int locale_matchclass(int c, int cls)
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
  default: return 0;
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

int locale_islower(int c)
{
  return islower(c);
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
