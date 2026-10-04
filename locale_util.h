#pragma once

#include <optional>
#include <clocale>
#include "bytes.h"

namespace lua::platform
{

int locale_matchclass(int c, int cls);

int locale_tolower(int c);
int locale_toupper(int c);
int locale_islower(int c);
int locale_isalpha(int c);
int locale_isdigit(int c);
int locale_isalnum(int c);
int locale_isxdigit(int c);
int locale_isspace(int c);

char locale_radixpoint();

std::optional<byte_view> locale_setlocale(int category, std::optional<byte_view> locale);

}
