#pragma once

#include <charconv>
#include <cstdint>

#include "internal_string.h"

struct lua_State;

namespace util
{
  using lua_CFunction_t = int (*)(lua_State*);

  template <typename T, typename V> T safe_cast(V) = delete;

  template <typename T, typename V> T check_cast(V) = delete;

  template <typename T, typename V> T type_cast(V) = delete;

  inline InternalString func_to_string(lua_CFunction_t pf)
  {
    char buf[20] = { '0', 'x' };
    std::to_chars_result res =
        std::to_chars(buf + 2, buf + sizeof(buf), reinterpret_cast<uintptr_t>(pf), 16);
    return InternalString(std::string_view(buf, ((size_t)(res.ptr - buf))));
  }
  
  inline constexpr void* as_userdata(long long val) { return reinterpret_cast<void*>(static_cast<uintptr_t>(val)); } 
  inline constexpr void* as_userdata(int val) { return reinterpret_cast<void*>(static_cast<uintptr_t>(val)); } 

  template <typename T> T* remove_const_cast(const T*) = delete;

  inline constexpr void* remove_const_cast(const void* val) { return const_cast<void*>(val); }
  inline constexpr char* remove_const_cast(const char* val) { return const_cast<char*>(val); }

  template <typename T> T* mem_cast(T*) = delete;

  inline constexpr void* mem_cast(const char* val) { return static_cast<void*>(const_cast<char*>(val)); }
};
