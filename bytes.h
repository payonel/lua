#pragma once

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
    return { ((size_t)(r.ptr - data())), r.ec, out };
  }

  double toDouble(size_t pos = 0) const
  {
    double result {};
    memcpy(reinterpret_cast<void*>(&result), sv.data() + pos, sizeof(double));
    return result;
  }

  float toFloat(size_t pos = 0) const
  {
    float result {};
    memcpy(reinterpret_cast<void*>(&result), sv.data() + pos, sizeof(float));
    return result;
  }

  static byte_view from_double(const double& f)
  {
    return byte_view(std::string_view(reinterpret_cast<const char*>(&f), sizeof(double)));
  }

  static byte_view from_float(const float& f)
  {
    return byte_view(std::string_view(reinterpret_cast<const char*>(&f), sizeof(float)));
  }

  std::string_view sv {nullptr, 0};

private:
  constexpr std::string_view view() const
  {
    return sv;
  }
};

struct byte_span
{
  byte_span() = default;

  explicit byte_span(size_t n)
  {
    char* raw = new char[sizeof(size_t) + n]();
    memcpy(raw, &n, sizeof(size_t));
    storage = std::shared_ptr<char>(raw + sizeof(size_t), [raw](char*) { delete[] raw; });
  }

  explicit byte_span(byte_view view) : byte_span(view.size()) { apply(view); }

  byte_span(const byte_span&) = default;
  byte_span& operator=(const byte_span&) = default;
  byte_span(byte_span&&) = default;
  byte_span& operator=(byte_span&&) = default;

  byte_span(char* rw, size_t len) = delete;
  template <size_t N> constexpr byte_span(char (&arr)[N]) = delete;

  size_t size() const
  {
    if (!storage)
      return 0;
    size_t n;
    memcpy(&n, storage.get() - sizeof(size_t), sizeof(size_t));
    return n;
  }

  char* data() const { return storage.get(); }

  size_t apply(byte_view from, size_t pos = 0)
  {
    assert(from.size() + pos <= size());
    return std::copy_n(from.data(), from.size(), data() + pos) - data();
  }

  void copy_from(byte_view src, size_t pos = 0)
  {
    assert(pos + src.size() <= size());
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

  byte_span reverse(size_t pos = 0, size_t len = byte_view::npos)
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
    return *this;
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
    return ((size_t)(r.ptr - data())) - pos;
  }

public:
  size_t write_number(double value, std::chars_format fmt, int precision, size_t pos = 0)
  {
    assert(pos < size());
    std::to_chars_result r = std::to_chars(data() + pos, data() + size() - 1, value, fmt, precision);
    assert(r.ec == std::errc());
    *r.ptr = '\0';
    return ((size_t)(r.ptr - data())) - pos;
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

  private:
  std::shared_ptr<char> storage; /* points past the size_t length header; null when empty */
};
