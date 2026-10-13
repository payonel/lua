#pragma once

#include <optional>
#include <clocale>
#include "bytes.h"

namespace lua::platform
{

bool locale_matchclass(int c, int cls);

int locale_tolower(int c);
int locale_toupper(int c);
bool locale_islower(int c);
bool locale_isalpha(int c);
bool locale_isdigit(int c);
bool locale_isalnum(int c);
bool locale_isxdigit(int c);
bool locale_isspace(int c);

char locale_radixpoint();

std::optional<byte_view> locale_setlocale(int category, std::optional<byte_view> locale);

}
