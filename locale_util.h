#pragma once

#include <optional>
#include <string_view>
#include <clocale>

int locale_matchclass(int c, int cls);

int locale_tolower(int c);
int locale_toupper(int c);
int locale_islower(int c);

char locale_radixpoint();

std::optional<std::string_view> locale_setlocale(int category, std::optional<std::string_view> locale);
