#pragma once

#ifdef LUA_DEBUG

#include "lua.h"
#include <stdint.h>

/* turn on assertions */
#define LUAI_ASSERT

/* test for sizes in 'l_sprintf' (make sure whole buffer is available) */
#define l_sprintf(s, sz, f, i) (memset(s, 0xAB, sz), snprintf(s, sz, f, i))

/* get a chance to test code without jump tables */
#define LUA_USE_JUMPTABLE 0

/* memory-allocator control variables */
typedef struct Memcontrol
{
  int failnext;
  unsigned long numblocks;
  unsigned long total;
  unsigned long maxmem;
  unsigned long memlimit;
  unsigned long countlimit;
  unsigned long objcount[LUA_NUMTYPES];
} Memcontrol;

LUA_API Memcontrol l_memcontrol;

#define ismainthread(l)             ((l) == mainthread(G(l)))
#define _getlock(l)                 (*((uint64_t*)(lua_getextraspace(l))))
#define _getplock(l)                (ismainthread(l) ? ((uint64_t*)(lua_getextraspace(l))) : ((uint64_t*)(*((void**)(lua_getextraspace(l))))))

#define luai_userstateopen(l)       (_getlock(l) = 0)
#define luai_userstateclose(l)      lua_assert(_getlock(l) == 1 && ismainthread(l))
#define luai_userstatethread(l, l1) lua_assert(_getplock(l1) == _getplock(l))
#define luai_userstatefree(l, l1)   lua_assert(_getplock(l) == _getplock(l1))
#define lua_lock(l)                 lua_assert((*_getplock(l))++ == 0)
#define lua_unlock(l)               lua_assert(--(*_getplock(l)) == 0)

/* change some sizes to give some bugs a chance */

#define LUAL_BUFFERSIZE 23
#define MINSTRTABSIZE   2
#define MAXIWTHABS      3

#define STRCACHE_N 23
#define STRCACHE_M 5

#define MAXINDEXRK 1

/*
** Reduce maximum stack size to make stack-overflow tests run faster.
** (But value is still large enough to overflow smaller integers.)
*/
#define LUAI_MAXSTACK 68000

/* test mode uses more stack space */
#define LUAI_MAXCCALLS 180

#define luai_tracegc(L, f)  luai_tracegctest(L, f)

#define luaL_newstate() luai_newstate()
#define luai_openlibs(L) luai_openlibstest(L)

#else

/*
@@ l_sprintf is equivalent to 'snprintf' or 'sprintf' in C89.
** (All uses in Lua have only one format item.)
*/
#define l_sprintf(s, sz, f, i) snprintf(s, sz, f, i)

/*
** By default, use jump tables in the main interpreter loop on gcc
** and compatible compilers.
*/
#define LUA_USE_JUMPTABLE 1

/*
** these macros allow user-specific actions when a thread is
** created/deleted
*/
#define luai_userstateopen(L) ((void) L)
#define luai_userstateclose(L) ((void) L)
#define luai_userstatethread(L, L1) ((void) L)
#define luai_userstatefree(L, L1) ((void) L)

/*
** macros that are executed whenever program enters the Lua core
** ('lua_lock') and leaves the core ('lua_unlock')
*/
#define lua_lock(L)   ((void) 0)
#define lua_unlock(L) ((void) 0)

/*
@@ LUAL_BUFFERSIZE is the initial buffer size used by the lauxlib
** buffer system.
*/
#define LUAL_BUFFERSIZE ((int) (16 * sizeof(void*) * sizeof(lua_Number)))

/*
** MAXimum number of successive Instructions WiTHout ABSolute line
** information. (A power of two allows fast divisions.)
*/
#define MAXIWTHABS 128

/*
** Size of cache for strings in the API. 'N' is the number of
** sets (better be a prime) and "M" is the size of each set.
** (M == 1 makes a direct cache.)
*/
#define STRCACHE_N 53
#define STRCACHE_M 2

#define MAXINDEXRK MAXARG_B

/*
** LUAI_MAXSTACK limits the size of the Lua stack.
** It must fit into INT_MAX/2.
*/
#if 1000000 < (INT_MAX / 2)
#define LUAI_MAXSTACK 1000000
#else
#define LUAI_MAXSTACK (INT_MAX / 2u)
#endif

/*
** Maximum depth for nested C calls, syntactical nested non-terminals,
** and other features implemented through recursion in C. (Value must
** fit in a 16-bit unsigned integer. It must also be compatible with
** the size of the C stack.)
*/
#define LUAI_MAXCCALLS 200

/*
@@ lua_strx2number converts a hexadecimal numeral to a number.
** In C99, 'strtod' does that conversion. Otherwise, you can
** leave 'lua_strx2number' undefined and Lua will provide its own
** implementation.
*/
#define lua_strx2number(s, p) lua_str2number(s, p)

/*
@@ lua_number2strx converts a float to a hexadecimal numeral.
** In C99, 'sprintf' (with format specifiers '%a'/'%A') does that.
** Otherwise, you can leave 'lua_number2strx' undefined and Lua will
** provide its own implementation.
*/
#define lua_number2strx(L, b, sz, f, n) ((void) L, l_sprintf(b, sz, f, (LUAI_UACNUMBER) (n)))

#define luai_openlibs(L) luaL_openselectedlibs(L, ~0, 0)
#define luai_tracegc(L, f) ((void) 0)

#endif

#define LUA_EXTRASPACE (sizeof(void*))
