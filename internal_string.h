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
#include <cstring>
#include <span>
#include <string>
#include <string_view>

/*
** Boundary text type — see host_string.h (std::string access cut,
** RULED 2026-07-31; view() is the single sanctioned access).
*/
#include "host_string.h"

/*
** Bounded byte read over a view: the byte at offset 'i', or -1
** (EOZ-like, rejected by all lctype predicates) past the end.
** (Java: i < b.length ? b[i] & 0xFF : -1)
*/
inline int sv_byteat(std::string_view s, size_t i)
{
  return (i < s.size()) ? ((unsigned char)(s[i])) : -1;
}

class InternalString
{
public:
  /*
  ** Move-only (RULED 2026-07-29): copying is deleted so every deep
  ** copy is an explicit, visible act (the from-bytes ctor) and no
  ** accidental payload duplication can hide in a pass-by-value or
  ** assignment. Moves hand over the buffer (engine-internal only).
  */
  InternalString() = default;
  InternalString(const InternalString&) = delete;
  InternalString(InternalString&&) noexcept = default;
  InternalString& operator=(const InternalString&) = delete;
  InternalString& operator=(InternalString&&) noexcept = default;
  ~InternalString() = default;

  /* From raw bytes: always a deep copy; 'bytes' is never retained. */
  InternalString(const char* bytes, size_t len) : bytes_(bytes, len) {}

  /* deep copy of a sub-range of a view — the storage boundary where
     an (off, len) pair materializes (Java: new byte[] copyOfRange) */
  InternalString(std::string_view src, size_t off, size_t len)
      : bytes_(src.data() + off, len) {}

  /*
  ** From HostString: the boundary conversion boundary (deep copy). For
  ** the C++ staging host text is already UTF-8 bytes, so this is a
  ** byte copy. At port time this becomes the STRICT UTF-16 -> UTF-8
  ** encoding: malformed text (e.g. lone surrogates) is an API-level
  ** error code, the lua_State untouched.
  */
  explicit InternalString(const HostString& host) : bytes_(host.view()) {}

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
  ** none). Internal code must use view()/byteAt()/size(). Sanctioned
  ** c_str() readers after the raw-char* review (2026-07-30): the
  ** l_strcmp strcoll segment walk (LOCALE item) and ltests'
  ** printf shims. (The warn-callback boundary was re-ruled 2026-07-30:
  ** lua_WarnFunction now takes HostString, built via bytes()/size().)
  ** Everything else is a bug.
  */
  const char* c_str() const noexcept { return bytes_.c_str(); }

  /*
  ** Allocated payload capacity. Accounting (RESERVE-BEFORE-GROWTH,
  ** ruled 2026-07-29): the payload SIZE is luaM_reserve'd — memlimit
  ** veto honored — BEFORE allocation at creation, and released at
  ** destruction; capacity slack (allocator rounding/SSO) stays
  ** uncounted but is bounded by the counted size.
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

/*
** {==================================================================
** Literal wrapping (RULED: literals wrapped at their sites)
** ===================================================================
**
** lit<"...">() yields a stable, lazily-created InternalString for a
** source literal, passed around as const InternalString*. This is the
** C++ staging analogue of a Java string literal: a Java literal IS an
** interned String constant with stable identity, not a fresh object
** per evaluation. A site transcribes 1:1 —
**     luaS_new(L, lit<"__name">())   ->   luaS_new(L, "__name")
** (the Java side converting its literal to the InternalString
** constant however the port chooses to cache it).
**
** The per-literal static has C-literal lifetime (created on first
** use, lives for the program), so the returned pointer satisfies the
** InternalString* non-owning contract at every call depth.
*/

namespace istr_detail
{
/* Carries the literal's bytes as a non-type template parameter. */
template <size_t N>
struct LiteralBytes
{
  char v[N];
  constexpr LiteralBytes(const char (&s)[N])
  {
    for (size_t i = 0; i < N; i++)
      v[i] = s[i];
  }
};
} /* namespace istr_detail */

template <istr_detail::LiteralBytes S>
const InternalString* lit()
{
  static const InternalString s(S.v, sizeof(S.v) - 1);
  return &s;
}

/* }================================================================== */

/*
** {==================================================================
** ByteBuilder — the engine's growable byte accumulator
** ===================================================================
**
** Companion to InternalString (RULED): strings are BUILT here — the
** lexer token buffer, luaO_pushvfstring's BuffFS, and eventually the
** auxiliary-library buffer. In Java this is a plain byte-array
** builder (emphatically NOT java.lang.StringBuilder, which is UTF-16
** text). finish() hands the accumulated bytes to an InternalString by
** move — engine-owned buffers may be adopted zero-copy (the one
** sanctioned non-deep-copy path; nothing external ever holds the
** reference).
**
** Accounting (RESERVE-BEFORE-GROWTH model, ruled 2026-07-29): a
** builder whose growth is script-driven and unbounded must reserve
** each capacity growth with the lua_State BEFORE allocating and honor
** the memlimit veto — an untrusted script must never grow host memory
** the state's limit cannot refuse. Accounted clients: luaL_Buffer
** (io.read, string.rep/format/gsub) and the lexer token buffer
** (llex save(); a token is NOT bounded by any admitted allocation —
** a streaming reader in lua_load anchors only one piece at a time
** while the token accumulates them all, audit 2026-07-29; the
** parse-wide charge equals capacity() and is released by
** luaD_protectedparser). The one remaining uncounted builder is
** BuffFS, sanctioned because an error/format message is BOUNDED by
** its already-accounted inputs. Adoption is a CHECKED admission:
** luaS_take/createstrobj reserve the size (memlimit veto honored)
** before the state takes the bytes.
*/
class ByteBuilder
{
public:
  void append(char c)
  {
    size_t off = prep(1);
    bytes_[off] = c;
    commit(1);
  }
  void append(std::string_view b)
  {
    append(b, 0, b.size());
  }
  void append(std::string_view b, size_t off, size_t n)
  {
    if (n > 0)
    {
      size_t at = prep(n);
      /* Java: System.arraycopy(b, off, buf, at, n) */
      memcpy(&bytes_[at], b.data() + off, n);
      commit(n);
    }
  }
  void append(const InternalString* s)
  {
    append(s->view());
  }

  /* discard contents, keep capacity (builders are reused) */
  void reset() noexcept { len_ = 0; }

  /* remove the last 'k' bytes (lexer un-saves escapes/delimiters) */
  void drop(size_t k) { len_ -= k; }

  size_t size() const noexcept { return len_; }

  /*
  ** Committed contents, as a read-only view. Valid until the next
  ** append/reset/finish. Only NUL-terminated if the client appended a
  ** NUL (the lexer does so explicitly before C-string consumers).
  ** Java: the builder's byte[] reference (positions travel as ints
  ** alongside — see the view/offset discipline in the char* review).
  */
  std::string_view view() const noexcept { return {bytes_.data(), len_}; }

  /*
  ** The whole writable capacity (Java: the builder's byte[] itself).
  ** Writes go at offsets returned by 'prep'; never slice the span.
  ** Invalidated by growth (growTo/prep/append) — re-fetch after.
  */
  std::span<char> area() noexcept { return {bytes_.data(), bytes_.size()}; }

  /*
  ** prep/commit protocol (luaL_prepbuffsize/luaL_addsize): 'prep'
  ** ensures a writable area of at least 'sz' bytes past the committed
  ** length, growing capacity if needed (UNACCOUNTED growth — accounted
  ** clients must pre-grow via growTo below), and returns the write
  ** OFFSET into area(); 'commit' extends the committed length over
  ** bytes the client wrote.
  */
  size_t prep(size_t sz)
  {
    if (len_ + sz > bytes_.size())
      growTo(len_ + sz);
    return len_;
  }
  void commit(size_t sz) noexcept { len_ += sz; }

  /* writable capacity currently allocated */
  size_t capacity() const noexcept { return bytes_.size(); }

  /*
  ** Grow the writable area to exactly 'cap' bytes. The accounted
  ** growth boundary: clients under the reserve-before-growth contract
  ** reserve 'cap' with the lua_State BEFORE calling this.
  */
  void growTo(size_t cap) { bytes_.resize(cap); }

  /* hand the bytes over (zero-copy); the builder is left empty */
  InternalString finish() noexcept
  {
    bytes_.resize(len_);
    InternalString s(std::move(bytes_));
    bytes_.clear(); /* normalize the moved-from state */
    len_ = 0;
    return s;
  }

private:
  std::string bytes_; /* writable area; [0, len_) is committed content */
  size_t len_ = 0;    /* committed length */
};

/* }================================================================== */

constexpr const char* operator""_os (const char* s, std::size_t n)
{
  (void)n;
  return reinterpret_cast<const char*>(s);
}
