#pragma once

#include <string_view>
#include <span>
#include <string.h>
#include <algorithm>
#include <assert.h>
#include <cstddef>
#include <charconv>
#include <system_error>

struct byte_view
{
  static constexpr size_t npos = size_t(-1);

  constexpr byte_view()
    : sv("", 0)
    // , offset(0)
    // , length(0)
  {}

  constexpr byte_view(const char* bytes)
    : sv(bytes)
    // , offset(0)
    // , length(sv.size())
  {}

  constexpr byte_view(const char* bytes, size_t len)
    : sv(bytes, len)
    // , offset(0)
    // , length(len)
  {}

  constexpr explicit byte_view(std::string_view v) : sv(v)/*, offset(0), length(v.size())*/ {}

  constexpr size_t size() const { return sv.size()/*length*/; }
  constexpr const char* data() const { return sv.data() /*+ offset*/; }

  constexpr bool empty() const { return sv.empty(); /*length == 0;*/ }

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
    return sv.data()[/*offset +*/ i];
  }

  constexpr bool starts_with(byte_view s) const
  {
    return view().starts_with(s.view());
  }

  constexpr bool starts_with(char c) const
  {
    return view().starts_with(c);
  }

  constexpr byte_view substr(size_t off, size_t len = byte_view::npos) const
  {
    return byte_view(sv.substr(off, len));
    // assert(off <= length);
    // byte_view cp { sv.data(), std::min<size_t>(len, length - off) };
    // cp.offset = offset + off;
    // return cp;
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

  parse_result parse_number(size_t off, std::chars_format fmt) const
  {
    double out {};
    std::from_chars_result r = std::from_chars(data() + off, data() + size(), out, fmt);
    return { ((size_t)(r.ptr - data())), r.ec, out };
  }

  std::string_view sv {nullptr, 0};
  // size_t offset = 0;
  // size_t length = 0;

private:
  constexpr std::string_view view() const
  {
    return sv; // std::string_view(sv.data() + offset, length);
  }
};

struct byte_span
{
  constexpr byte_span() = default;

  constexpr byte_span(char* rw, size_t len) : sp(rw, len)/*, offset(0), length(sp.size())*/ {}

  template <size_t N>
  constexpr byte_span(char (&arr)[N]) : sp(arr, N)/*, offset(0), length(N)*/ {}

  constexpr size_t size() const { return sp.size() /* + offset */; }
  constexpr char* data() const { return sp.data() /* + offset */; }

  constexpr size_t apply(byte_view from)
  {
    assert(from.size() <= size());
    return std::copy_n(from.data(), from.size(), data()) - data();
  }

  void copy_from(size_t off, byte_view src)
  {
    assert(off + src.size() <= size());
    memcpy(data() + off, src.data(), src.size());
  }

  void fill(size_t off, size_t n, std::byte v)
  {
    assert(off + n <= size());
    memset(data() + off, std::to_integer<int>(v), n);
  }

  void move_from(size_t off, byte_view src)
  {
    assert(off + src.size() <= size());
    memmove(data() + off, src.data(), src.size());
  }

  template <typename... Args> void write_number(Args&&...) = delete;

  size_t write_number(double value, std::chars_format fmt, int precision)
  {
    std::to_chars_result r = std::to_chars(data(), data() + size(), value, fmt, precision);
    assert(r.ec == std::errc());
    return ((size_t)(r.ptr - data()));
  }

  size_t write_number(long long value)
  {
    std::to_chars_result r = std::to_chars(data(), data() + size(), value);
    assert(r.ec == std::errc());
    return ((size_t)(r.ptr - data()));
  }

  size_t write_number(unsigned long value, int base)
  {
    std::to_chars_result r = std::to_chars(data(), data() + size(), value, base);
    assert(r.ec == std::errc());
    return ((size_t)(r.ptr - data()));
  }

  size_t write_number(int value)
  {
    std::to_chars_result r = std::to_chars(data(), data() + size(), value);
    assert(r.ec == std::errc());
    return ((size_t)(r.ptr - data()));
  }

  size_t write_number(unsigned long long value, int base)
  {
    std::to_chars_result r = std::to_chars(data(), data() + size(), value, base);
    assert(r.ec == std::errc());
    return ((size_t)(r.ptr - data()));
  }

  constexpr byte_view view() const
  {
    return byte_view(std::string_view(sp.data(), sp.size()));
    // return byte_view(data(), length);
  }

  constexpr byte_view str_view() const
  {
    byte_view sv = view();
    size_t n = sv.find('\0');
    sv = sv.substr(0, n == std::string_view::npos ? sv.size() : n);
    return sv;
  }

  constexpr char& operator[] (size_t i)
  {
    assert(i < sp.size());
    return sp[i];
    // assert(i < length);
    // return sp[offset + i];
  }

  constexpr byte_span subspan(size_t off, size_t len = byte_view::npos) const
  {
    byte_span cp;
    cp.sp = sp.subspan(off, len);
    // assert(off <= length);
    // byte_span cp = *this;
    // cp.offset = offset + off;
    // cp.length = std::min<size_t>(len, length - off);
    return cp;
  }

  constexpr bool empty() const { return sp.empty(); }
  // constexpr bool empty() const { return length == 0; }

  std::span<char> sp;
  // size_t offset = 0;
  // size_t length = 0;
};
