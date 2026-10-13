/*
** $Id: lctype.h $
** 'ctype' functions for Lua
** See Copyright Notice in lua.h
*/

#pragma once

#include "lua.h"

/*
** WARNING: the functions defined here do not necessarily correspond
** to the similar functions in the standard C ctype.h. They are
** optimized for the specific needs of Lua.
*/

static_assert('A' == 65 && '0' == 48, "this port supports only ASCII execution character sets");

#include <limits.h>

#include "llimits.h"

namespace lua::bytes
{

inline constexpr int ALPHABIT = 0;
inline constexpr int DIGITBIT = 1;
inline constexpr int PRINTBIT = 2;
inline constexpr int SPACEBIT = 3;
inline constexpr int XDIGITBIT = 4;

constexpr int MASK(int B)
{
  return 1 << B;
}

/* one entry for each character and for -1 (EOZ) */
extern const lu_byte luai_ctype_[UCHAR_MAX + 2];

/*
** add 1 to char to allow index -1 (EOZ)
*/
static l_forceinline bool testprop(int c, int p)
{
  return (luai_ctype_[c + 1] & p) != 0;
}

static l_forceinline bool lislalpha(int c)
{
  return testprop(c, MASK(ALPHABIT));
}

static l_forceinline bool lislalnum(int c)
{
  return testprop(c, MASK(ALPHABIT) | MASK(DIGITBIT));
}

static l_forceinline bool lisdigit(int c)
{
  return testprop(c, MASK(DIGITBIT));
}

static l_forceinline bool lisspace(int c)
{
  return testprop(c, MASK(SPACEBIT));
}

static l_forceinline bool lisprint(int c)
{
  return testprop(c, MASK(PRINTBIT));
}

static l_forceinline bool lisxdigit(int c)
{
  return testprop(c, MASK(XDIGITBIT));
}

static l_forceinline int ltolower(int c)
{
  assert(('A' <= c && c <= 'Z') || c == (c | ('A' ^ 'a')));
  return c | ('A' ^ 'a');
}

}
