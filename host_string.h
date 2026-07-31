/*
** host_string.h
** HostString — the host platform text type (RULED 2026-07-31; see
** internal_string.h header comment for the InternalString side).
**
** HostString exists for the Java consumer, and nothing else: it is
** the type of string parameters/returns/callback arguments on the
** public API, because the SDK consumer passes and receives platform
** strings (java.lang.String, UTF-16). It never penetrates past the
** API conversion layer.
**
** For the C++ staging it wraps a std::string, but std::string access
** is CUT (RULED 2026-07-31): the only read is view(), the single
** sanctioned conversion point at the API boundary. At port time this
** class becomes java.lang.String and view() becomes the strict
** UTF-16 -> UTF-8 encode. string_view must never point into a
** HostString's storage past the boundary conversion — HostString
** will be UTF-16; string_view is bytes, never UTF-16.
**
** Nullability: a HostString is never null. A nullable parameter or
** return is std::optional<HostString> (pass std::nullopt) — the
** string_view constructor does not accept null views.
**
** Placeholder shape — to be expanded later.
*/

#pragma once

#include <string>
#include <string_view>

class HostString
{
public:
  HostString() = default;

  /* from a literal / utf8 text bytes; null not allowed (nullable
     params are std::optional<HostString> — pass std::nullopt) */
  HostString(std::string_view s) : str(s) {}

  /* const char* form: C++ won't chain two user-defined conversions
     (const char* -> string_view -> HostString), so consumer literal
     call sites need this one directly */
  HostString(const char* s) : str(s) {}

  /* The single sanctioned access — the API-boundary conversion point
     ONLY (at port time: the strict UTF-16 -> UTF-8 encode). Any other
     caller is a bug. */
  std::string_view view() const noexcept { return std::string_view(str.data(), str.size()); }

  /* Consumer-side comparison against a literal (RULED 2026-07-31:
     interim shape for the test host; Java: String.equals). */
  bool operator==(const char* s) const noexcept { return str == s; }
  bool operator==(const HostString& o) const noexcept { return str == o.str; }

private:
  std::string str;
};
