#pragma once

#include "packages.h"

#include <string_view>
#include <span>
#include <string.h>
#include <algorithm>
#include <assert.h>
#include <cstddef>
#include <charconv>
#include <system_error>
#include <memory>
#include <type_traits>
#include <array>
#include <bit>
#include <cstdint>

namespace lua::bytes
{

struct byte_view
{
  static constexpr size_t npos = size_t(-1);

  constexpr byte_view()
    : sv("", 0)
  {}

  constexpr byte_view(const char* bytes)
    : sv(bytes)
  {}

  constexpr byte_view(const char* bytes, size_t len)
    : sv(bytes, len)
  {}

  constexpr explicit byte_view(std::string_view v) : sv(v) {}

  constexpr size_t size() const { return sv.size(); }
  constexpr const char* data() const { return sv.data(); }

  constexpr bool empty() const { return sv.empty(); }

  constexpr bool ends_with(char c) const
  {
    return view().ends_with(c);
  }

  constexpr bool ends_with(byte_view s) const
  {
    return view().ends_with(s.view());
  }

  constexpr size_t find_first_not_of(byte_view chars, size_t pos = 0) const
  {
    return view().find_first_not_of(chars.view(), pos);
  }

  constexpr size_t find_first_of(byte_view chars, size_t pos = 0) const
  {
    return view().find_first_of(chars.view(), pos);
  }

  constexpr bool operator==(byte_view other) const
  {
    return view() == other.view();
  }

  constexpr char operator[] (size_t i) const
  {
    return sv.data()[i];
  }

  constexpr bool starts_with(byte_view s) const
  {
    return view().starts_with(s.view());
  }

  constexpr bool starts_with(char c) const
  {
    return view().starts_with(c);
  }

  constexpr byte_view substr(size_t pos, size_t len = byte_view::npos) const
  {
    return byte_view(sv.substr(pos, len));
  }

  constexpr size_t find(char c, size_t pos = 0) const
  {
    return view().find(c, pos);
  }

  constexpr size_t find(byte_view s, size_t pos = 0) const
  {
    return view().find(s.view(), pos);
  }

  constexpr int compare(size_t pos, size_t count, byte_view s) const
  {
    return view().compare(pos, count, s.view());
  }

  bool equals(byte_view other) const
  {
    return view() == other.view();
  }

  struct parse_result
  {
    size_t next;
    std::errc ec;
    double result;
  };

  parse_result parse_number(size_t pos, std::chars_format fmt) const
  {
    double out {};
    std::from_chars_result r = std::from_chars(data() + pos, data() + size(), out, fmt);
    return { (size_t)(r.ptr - data()), r.ec, out };
  }

  double toDouble(size_t pos = 0) const
  {
    uint64_t bits = 0;
    for (size_t i = 8; i-- > 0;)
      bits = (bits << 8) | static_cast<unsigned char>(sv[pos + i]);
    return std::bit_cast<double>(bits);
  }

  float toFloat(size_t pos = 0) const
  {
    uint32_t bits = 0;
    for (size_t i = 4; i-- > 0;)
      bits = (bits << 8) | static_cast<unsigned char>(sv[pos + i]);
    return std::bit_cast<float>(bits);
  }

  std::string_view sv {nullptr, 0};

private:
  constexpr std::string_view view() const
  {
    return sv;
  }
};

inline byte_view as_byte_view(std::span<const std::byte> b)
{
  return byte_view(reinterpret_cast<const char*>(b.data()), b.size());
}

inline void copy_bytes(std::span<std::byte> dst, byte_view src)
{
  assert(src.size() <= dst.size());
  if (!src.empty())
    memcpy(dst.data(), src.data(), src.size());
}

template <typename Self>
struct byte_writes
{
  size_t size() const { return static_cast<const Self*>(this)->size(); }
  char* data() const { return static_cast<const Self*>(this)->data(); }
  Self& self() { return *static_cast<Self*>(this); }

  size_t apply(byte_view from, size_t pos = 0)
  {
    assert(from.size() + pos <= size());
    return std::copy_n(from.data(), from.size(), data() + pos) - data();
  }

  void write_double(double v, size_t pos)
  {
    uint64_t bits = std::bit_cast<uint64_t>(v);
    for (size_t i = 0; i < 8; i++)
    {
      data()[pos + i] = static_cast<char>(bits & 0xFF);
      bits >>= 8;
    }
  }

  void write_float(float v, size_t pos)
  {
    uint32_t bits = std::bit_cast<uint32_t>(v);
    for (size_t i = 0; i < 4; i++)
    {
      data()[pos + i] = static_cast<char>(bits & 0xFF);
      bits >>= 8;
    }
  }

  void copy_from(byte_view src, size_t pos = 0)
  {
    assert(pos + src.size() <= size());
    if (!src.empty())
      memcpy(data() + pos, src.data(), src.size());
  }

  void fill(std::byte v, size_t pos = 0, size_t len = byte_view::npos)
  {
    size_t s = size();
    assert(pos <= s);
    len = std::min(len, s - pos);
    memset(data() + pos, static_cast<char>(v), len);
  }

  void move_from(byte_view src, size_t pos = 0)
  {
    assert(pos + src.size() <= size());
    memmove(data() + pos, src.data(), src.size());
  }

  Self& reverse(size_t pos = 0, size_t len = byte_view::npos)
  {
    size_t s = size();
    assert(pos <= s);
    len = std::min(len, s - pos);
    char* p = data();
    for (size_t i = 0; i < len / 2; ++i)
    {
      size_t ei = pos + len - 1 - i;
      char tmp = p[ei];
      p[ei] = p[i + pos];
      p[i + pos] = tmp;
    }
    return self();
  }

  template <typename... Args> void write_number(Args&&...) = delete;

private:
  template <typename T>
  size_t write_number_impl(T value, int base, size_t pos, bool prefix)
  {
    assert(pos < size());
    size_t start = pos;
    if (prefix)
    {
      assert(base == 16);
      assert(pos + 2 < size());
      data()[pos] = '0';
      data()[pos + 1] = 'x';
      start = pos + 2;
    }
    std::to_chars_result r = std::to_chars(data() + start, data() + size() - 1, value, base);
    assert(r.ec == std::errc());
    *r.ptr = '\0';
    return (r.ptr - data()) - pos;
  }

public:
  size_t write_number(double value, std::chars_format fmt, int precision, size_t pos = 0)
  {
    assert(pos < size());
    std::to_chars_result r = std::to_chars(data() + pos, data() + size() - 1, value, fmt, precision);
    assert(r.ec == std::errc());
    *r.ptr = '\0';
    return (r.ptr - data()) - pos;
  }

  size_t write_number(long long value, int base = 10, size_t pos = 0)
  {
    return write_number_impl(value, base, pos, false);
  }

  size_t write_number(int value, int base = 10, size_t pos = 0)
  {
    return write_number_impl(value, base, pos, false);
  }

  size_t write_number(unsigned long value, int base = 10, size_t pos = 0, bool prefix = false)
  {
    return write_number_impl(value, base, pos, prefix);
  }

  size_t write_number(unsigned long long value, int base = 10, size_t pos = 0, bool prefix = false)
  {
    return write_number_impl(value, base, pos, prefix);
  }

  byte_view view() const
  {
    return byte_view(std::string_view(data(), size()));
  }

  byte_view str_view() const
  {
    byte_view sv = view();
    size_t n = sv.find('\0');
    sv = sv.substr(0, n == std::string_view::npos ? sv.size() : n);
    return sv;
  }

  char& operator[] (size_t i)
  {
    assert(i < size());
    return data()[i];
  }

  bool empty() const { return size() == 0; }

};

struct byte_owner;

struct byte_span : byte_writes<byte_span>
{
  constexpr byte_span() = default;

  size_t size() const { return n_; }
  char* data() const { return p_; }

  byte_span subspan(size_t pos, size_t len = byte_view::npos) const
  {
    assert(pos <= n_);
    return byte_span(p_ + pos, std::min(len, n_ - pos));
  }

private:
  friend struct byte_owner;
  template <size_t N> friend struct byte_inline;
  constexpr byte_span(char* p, size_t n) : p_(p), n_(n) {}
  char* p_ = nullptr;
  size_t n_ = 0;
};

struct byte_owner : byte_writes<byte_owner>
{
  byte_owner() = default;
  explicit byte_owner(size_t n) : p_(new char[n]()), n_(n) {}
  explicit byte_owner(byte_view view) : byte_owner(view.size()) { apply(view); }

  byte_owner(const byte_owner&) = delete;
  byte_owner& operator=(const byte_owner&) = delete;
  byte_owner(byte_owner&& o) noexcept : p_(std::move(o.p_)), n_(o.n_) { o.n_ = 0; }
  byte_owner& operator=(byte_owner&& o) noexcept
  {
    p_ = std::move(o.p_);
    n_ = o.n_;
    o.n_ = 0;
    return *this;
  }

  size_t size() const { return n_; }
  char* data() const { return p_.get(); }

  byte_span span() const { return byte_span(p_.get(), n_); }
  operator byte_span() const { return span(); }

private:
  std::unique_ptr<char[]> p_;
  size_t n_ = 0;
};

template <size_t N>
struct byte_inline : byte_writes<byte_inline<N>>
{
  byte_inline() = default;
  byte_inline(const byte_inline&) = delete;
  byte_inline& operator=(const byte_inline&) = delete;

  size_t size() const { return N; }
  char* data() const { return a_; }

  byte_span span() const { return byte_span(data(), N); }
  operator byte_span() const { return span(); }

private:
  mutable char a_[N] = {};
};

}
