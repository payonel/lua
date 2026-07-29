/*
** internal_string.h
** InternalString — the engine string type — and HostString, the
** platform text type. See todo.md "String representation audit" for
** the full design ruling (ryanbean 2026-07-29).
**
** Terminology: "host" = the platform runtime hosting the engine (the
** C++ standard library now, the JDK at port time) — distinct from the
** host-OS/hardware pins.
**
** InternalString is the lua string payload: an immutable, counted
** byte string (UTF-8 by convention; arbitrary bytes, embedded NULs
** and invalid UTF-8 included). In the Java port this becomes a class
** owning a byte[]. It is BYTES, not text — never transcribe it as
** java.lang.String.
**
** HostString is the platform text type. It appears ONLY in
** public-API parameter/return positions and is converted before
** values enter the engine; it is blocked from penetrating past the
** API layer. For the C++ staging it is std::string; at port time
** this typedef is deleted and replaced by java.lang.String.
**
** Copy semantics (RULED): every construction from bytes that the
** engine does not already own is a DEEP COPY. No external
** pointers/references are ever retained, in any scenario — the lua
** VM runtime is fully distinct from the host. Zero-copy adoption
** exists only for engine-OWNED buffers (builder finish, via the
** std::string&& ctor).
*/

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

/*
** Boundary text type (see header comment). A marker class may replace
** this typedef when the public API moves to HostString signatures.
*/
using HostString = std::string;

class InternalString
{
public:
  /*
  ** Value semantics, like std::string: copy = deep byte copy,
  ** move = buffer handover (engine-internal only).
  */
  InternalString() = default;
  InternalString(const InternalString&) = default;
  InternalString(InternalString&&) noexcept = default;
  InternalString& operator=(const InternalString&) = default;
  InternalString& operator=(InternalString&&) noexcept = default;
  ~InternalString() = default;

  /* From raw bytes: always a deep copy; 'bytes' is never retained. */
  InternalString(const char* bytes, size_t len) : bytes_(bytes, len) {}

  /*
  ** From HostString: the boundary conversion seam (deep copy). For
  ** the C++ staging host text is already UTF-8 bytes, so this is a
  ** byte copy. At port time this becomes the STRICT UTF-16 -> UTF-8
  ** encoding: malformed text (e.g. lone surrogates) is an API-level
  ** error code, the lua_State untouched.
  */
  explicit InternalString(const HostString& host) : bytes_(host) {}

  /*
  ** Adopt an engine-OWNED buffer (builder finish): zero-copy move.
  ** Never call this with a buffer the host can still reach.
  */
  explicit InternalString(std::string&& engineOwned) noexcept
      : bytes_(std::move(engineOwned))
  {
  }

  /* ---- observers: const only; the payload is immutable ---------- */

  size_t size() const noexcept { return bytes_.size(); }
  bool empty() const noexcept { return bytes_.empty(); }

  /*
  ** Byte at index i as an unsigned value 0..255. Centralizes the
  ** Java signed-byte hazard: always use this (never a raw char) when
  ** a byte indexes a table or participates in arithmetic/compares.
  */
  int byteAt(size_t i) const { return static_cast<unsigned char>(bytes_[i]); }

  /* Read-only view of the bytes (non-owning; valid while *this lives). */
  std::string_view view() const noexcept { return bytes_; }

  /* Raw byte pointer + explicit size (embedded NULs are legal). */
  const char* bytes() const noexcept { return bytes_.data(); }

  /*
  ** NUL-terminated view — TRANSITIONAL: only the const char* C API
  ** staging layer may depend on the terminator (Java byte[] has
  ** none). Internal code must use bytes()/size()/view().
  */
  const char* c_str() const noexcept { return bytes_.c_str(); }

  /*
  ** Allocated payload capacity — the GC-debt accounting seam (status
  ** quo RULED): bytes come from global new but are counted into GC
  ** debt via luaM_reserve(capacity()) at creation and
  ** luaM_release(capacity()) at destruction.
  */
  size_t capacity() const noexcept { return bytes_.capacity(); }

  /* ---- equality / ordering / hash: the intern-key contract ------ */

  /* Byte-wise equality. */
  bool operator==(const InternalString& o) const noexcept
  {
    return bytes_ == o.bytes_;
  }

  /*
  ** memcmp-order compare: <0, 0, >0 with bytes compared UNSIGNED
  ** (char_traits<char> already guarantees this in C++; in Java this
  ** is Arrays.compareUnsigned, NOT Arrays.compare). Collation is the
  ** locale item's business, not this class's.
  */
  int compare(const InternalString& o) const noexcept
  {
    return bytes_.compare(o.bytes_);
  }

  /*
  ** Byte hash (upstream luaS_hash algorithm). The class supplies the
  ** hash so it can serve as the intern-table key (Java: hashCode over
  ** the byte[]); the TString caches the result — the cache is VM
  ** state and stays out of this class.
  */
  static size_t hashBytes(const char* str, size_t l) noexcept
  {
    unsigned int h = 0x5bd1e995 ^ ((unsigned int)(l));
    for (; l > 0; l--)
      h ^= ((h << 5) + (h >> 2) + ((unsigned char)(str[l - 1])));
    return h;
  }
  size_t hash() const noexcept { return hashBytes(bytes_.data(), bytes_.size()); }

private:
  /*
  ** The byte payload. std::string used as a BYTE container for the
  ** C++ staging (char = byte, counted, arbitrary content); the Java
  ** port stores a byte[] + length here.
  */
  std::string bytes_;
};
