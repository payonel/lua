#pragma once

#include <string_view>
#include <string>
#include <span>
#include <string.h>
#include <algorithm>
#include <assert.h>

struct byte_view
{
  static constexpr size_t npos = size_t(-1);

  constexpr byte_view()
    : sv("", 0)
    , offset(0)
    , length(0)
  {}

  constexpr byte_view(const char* bytes)
    : sv(bytes)
    , offset(0)
    , length(sv.size())
  {}

  constexpr byte_view(const char* bytes, size_t len)
    : sv(bytes, len)
    , offset(0)
    , length(len)
  {}

  constexpr explicit byte_view(std::span<char> bytes, size_t len = byte_view::npos)
    : sv(bytes.data(), bytes.size())
    , offset(0)
    , length(len == byte_view::npos ? bytes.size() : len)
  {}

  constexpr size_t size() const { return length; }
  constexpr const char* data() const { return sv.data() + offset; }

  constexpr bool empty() const { return length == 0; }

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
    return sv.data()[offset + i];
  }

  constexpr bool starts_with(byte_view s) const
  {
    return view().starts_with(s.view());
  }

  constexpr byte_view substr(size_t off, size_t len = byte_view::npos) const
  {
    assert(off <= length);
    byte_view cp { sv.data(), std::min<size_t>(len, length - off) };
    cp.offset = offset + off;
    return cp;
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

  std::string_view sv {nullptr, 0};
  size_t offset = 0;
  size_t length = 0;

private:
  constexpr std::string_view view() const
  {
    return std::string_view(sv.data() + offset, length);
  }
};

struct byte_span
{
  constexpr byte_span(std::span<char> bytes, size_t len = byte_view::npos)
    : sp(bytes)
    , offset(0)
    , length(len == byte_view::npos ? bytes.size() : len)
  {}
  constexpr byte_span(char* rw, size_t len) : sp(rw, len), offset(0), length(sp.size()) {}

  constexpr size_t size() const { return length; }
  constexpr char* data() const { return sp.data() + offset; }

  constexpr size_t apply(byte_view from)
  {
    assert(from.size() <= length);
    return std::copy_n(from.data(), from.size(), data()) - data();
  }

  constexpr byte_view view() const
  {
    return byte_view(sp.data() + offset, length);
  }

  constexpr byte_view str_view() const
  {
    assert(offset + length <= sp.size());
    size_t n = std::string_view(sp.data() + offset, length).find('\0');
    return byte_view(sp.data() + offset, n == byte_view::npos ? length : n);
  }

  constexpr char& operator[] (size_t i)
  {
    assert(i < length);
    return sp[offset + i];
  }

  constexpr byte_span subspan(size_t off, size_t len = byte_view::npos) const
  {
    assert(off <= length);
    byte_span cp = *this;
    cp.offset = offset + off;
    cp.length = std::min<size_t>(len, length - off);
    return cp;
  }

  std::span<char> sp;
  size_t offset = 0;
  size_t length = 0;
};
