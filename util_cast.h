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
    byte_span bufsp(buf, sizeof(buf));
    std::to_chars_result res = std::to_chars(buf + 2, buf + sizeof(buf), reinterpret_cast<uintptr_t>(pf), 16);
    byte_view svbuff(bufsp.data(), res.ptr - buf);
    return InternalString(svbuff);
  }
  
  inline constexpr void* as_userdata(long long val) { return reinterpret_cast<void*>(static_cast<uintptr_t>(val)); } 
  inline constexpr void* as_userdata(int val) { return reinterpret_cast<void*>(static_cast<uintptr_t>(val)); } 
};
