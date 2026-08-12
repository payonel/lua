#pragma once

#include <string_view>
#include <span>
#include <string.h>
#include <algorithm>
#include <assert.h>

struct byte_view
{
  static constexpr size_t npos = size_t(-1);

  byte_view() = delete;
  byte_view(std::string_view view, size_t len = byte_view::npos)
    : sv(view)
    , offset(0)
    , length(len == byte_view::npos ? sv.size() : len)
  {
  }

  byte_view(std::span<char> bytes, size_t len = byte_view::npos)
    : sv(std::string_view(bytes.data(), bytes.size()))
    , offset(0)
    , length(len == byte_view::npos ? bytes.size() : len)
  {}

  constexpr size_t size() const { return length; }
  constexpr const char* data() const { return sv.data() + offset; }
  constexpr std::string_view view() const { return std::string_view(sv.data() + offset, length); }

  constexpr bool empty() const { return view().empty(); }

  constexpr bool ends_with(char c) const
  {
    return view().ends_with(c);
  }

  constexpr size_t find_first_not_of(std::string_view chars, size_t pos = 0) const
  {
    return sv.substr(offset, length).find_first_not_of(chars, pos);
  }

  constexpr bool operator==(byte_view other) const
  {
    return view() == other.view();
  }

  constexpr bool operator==(std::string_view other) const
  {
    return view() == other;
  }

  constexpr char operator[] (size_t i) const
  {
    assert(i < length);
    return sv[offset + i];
  }

  constexpr byte_view substr(size_t off, size_t len = byte_view::npos) const
  {
    byte_view cp = *this;
    cp.offset = offset + off;
    cp.length = std::min<size_t>(len, length - off);
    return cp;
  }

  constexpr size_t find(char c, size_t pos = 0) const
  {
    return view().find(c, pos);
  }

  std::string_view sv {nullptr, 0};
  size_t offset = 0;
  size_t length = 0;
};

struct byte_span
{
  byte_span(std::span<char> bytes, size_t len = byte_view::npos)
    : sp(bytes)
    , offset(0)
    , length(len == byte_view::npos ? bytes.size() : len)
  {}
  byte_span(char* rw, size_t len) : sp(rw, len), offset(0), length(sp.size()) {}

  size_t size() const { return length; }
  size_t capacity() const { return sp.size(); }
  char* data() const { return sp.data() + offset; }

  inline size_t apply(std::string_view from)
  {
    assert(from.size() <= length);
    return std::copy_n(from.data(), from.size(), sp.data() + offset) - (sp.data() + offset);
  }

  byte_view view() const
  {
    return byte_view(std::string_view(sp.data(), sp.size())).substr(offset, length);
  }

  byte_view str_view() const
  {
    assert(offset + length <= sp.size());
    size_t n = ::strnlen(sp.data() + offset, length);
    return byte_view(std::string_view(sp.data(), sp.size())).substr(offset, n);
  }

  constexpr char& operator[] (size_t i)
  {
    assert(i < length);
    return sp[offset + i];
  }

  constexpr std::span<char> span() const
  {
    return sp.subspan(offset, length);
  }

  byte_span subspan(size_t off, size_t len = byte_view::npos) const
  {
    byte_span cp = *this;
    cp.offset = offset + off;
    cp.length = std::min<size_t>(len, length - off);
    return cp;
  }

  std::span<char> sp;
  size_t offset = 0;
  size_t length = 0;
};
