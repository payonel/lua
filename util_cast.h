#pragma once

#include <stddef.h>
#include <type_traits>

namespace util
{
  // safe casts lose no data and is functionally equal. e.g. float->double, in32_t->int64_T
  template <typename T, typename V> T safe_cast(V) = delete;

  // check cast is a safe cast but where data could be lost, but is checked. e.g. int->uint or uint->int
  template <typename T, typename V> T check_cast(V) = delete;

  // type casts lose no data but loses type. e.g. float -> int32_t
  template <typename T, typename V> T type_cast(V) = delete;

  // to_address is used to take the address of objects or functions
  static constexpr void* address_of(int (*pf)(lua_State*)) { return reinterpret_cast<void*>(pf); } 
  
  // userdata is user defined. it could be a real address but lua side does not know
  static constexpr void* as_userdata(lua_Integer val) { return reinterpret_cast<void*>(val); } 
  static constexpr void* as_userdata(int val) { return reinterpret_cast<void*>(val); } 

  // remove const cast justification may be due to poor arg type
  template <typename T> T* remove_const_cast(const T*) = delete;

  static constexpr void* remove_const_cast(const void* val) { return const_cast<void*>(val); }

  // type casts to memory address needed for c memcpy, memmove, free, realloc, etc
  template <typename T> T* mem_cast(T*) = delete;

  static constexpr void* mem_cast(const char* val) { return static_cast<void*>(const_cast<char*>(val)); }
};
