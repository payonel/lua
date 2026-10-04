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

  inline size_t pointertext(byte_span out, size_t pos, const void* p)
  {
    return out.write_number(reinterpret_cast<uintptr_t>(p), 16, pos, true);
  }

  inline size_t pointertext(byte_span out, size_t pos, lua_CFunction_t pf)
  {
    return out.write_number(reinterpret_cast<uintptr_t>(pf), 16, pos, true);
  }

  inline InternalString func_to_string(lua_CFunction_t pf)
  {
    byte_inline<20> buf;
    size_t n = pointertext(buf, 0, pf);
    return InternalString(buf.view().substr(0, n));
  }
  
  inline constexpr void* as_userdata(long long val) { return reinterpret_cast<void*>(static_cast<uintptr_t>(val)); } 
  inline constexpr void* as_userdata(int val) { return reinterpret_cast<void*>(static_cast<uintptr_t>(val)); } 
};
