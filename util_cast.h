#pragma once

#include <charconv>
#include <cstdint>
#include <string>

struct lua_State;

namespace util
{
  using lua_CFunction_t = int (*)(lua_State*);

  // safe casts lose no data and is functionally equal. e.g. float->double, in32_t->int64_T
  template <typename T, typename V> T safe_cast(V) = delete;

  // check cast is a safe cast but where data could be lost, but is checked. e.g. int->uint or uint->int
  template <typename T, typename V> T check_cast(V) = delete;

  // type casts lose no data but loses type. e.g. float -> int32_t
  template <typename T, typename V> T type_cast(V) = delete;

  // formats a light C function pointer as a string for display/identity
  inline std::string func_to_string(lua_CFunction_t pf)
  {
    char buf[20] = { '0', 'x' };
    std::to_chars_result res =
        std::to_chars(buf + 2, buf + sizeof(buf), reinterpret_cast<uintptr_t>(pf), 16);
    return std::string(buf, res.ptr);
  }
  
  // userdata is user defined. it could be a real address but lua side does not know
  inline constexpr void* as_userdata(long long val) { return reinterpret_cast<void*>(static_cast<uintptr_t>(val)); } 
  inline constexpr void* as_userdata(int val) { return reinterpret_cast<void*>(static_cast<uintptr_t>(val)); } 

  // remove const cast justification may be due to poor arg type
  template <typename T> T* remove_const_cast(const T*) = delete;

  inline constexpr void* remove_const_cast(const void* val) { return const_cast<void*>(val); }
  inline constexpr char* remove_const_cast(const char* val) { return const_cast<char*>(val); }

  // type casts to memory address needed for c memcpy, memmove, free, realloc, etc
  template <typename T> T* mem_cast(T*) = delete;

  inline constexpr void* mem_cast(const char* val) { return static_cast<void*>(const_cast<char*>(val)); }
};
