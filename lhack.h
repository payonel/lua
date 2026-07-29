#pragma once

#ifdef LUA_DEBUG

#include "lua.h"
#include <stdint.h>

/* memory-allocator control variables */
struct Memcontrol
{
  int failnext;
  unsigned long numblocks;
  unsigned long total;
  unsigned long maxmem;
  unsigned long memlimit;
  unsigned long countlimit;
  unsigned long objcount[LUA_NUMTYPES];
};

LUA_API Memcontrol l_memcontrol;

#define ismainthread(l)             ((l) == mainthread(G(l)))
#define _getlock(l)                 (*((uint64_t*)(lua_getextraspace(l))))
#define _getplock(l)                (ismainthread(l) ? ((uint64_t*)(lua_getextraspace(l))) : ((uint64_t*)(*((void**)(lua_getextraspace(l))))))

#define luai_userstateopen(l)       (_getlock(l) = 0)
#define luai_userstateclose(l)      assert(_getlock(l) == 1 && ismainthread(l))
#define luai_userstatethread(l, l1) assert(_getplock(l1) == _getplock(l))
#define luai_userstatefree(l, l1)   assert(_getplock(l) == _getplock(l1))
#define lua_lock(l)                 assert((*_getplock(l))++ == 0)
#define lua_unlock(l)               assert(--(*_getplock(l)) == 0)

/* change some sizes to give some bugs a chance */

#define LUAL_BUFFERSIZE 23
#define MINSTRTABSIZE   2
#define MAXIWTHABS      3

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

#define luai_openlibs(L) luaL_openselectedlibs(L, ~0, 0)
#define luai_tracegc(L, f) ((void) 0)

#endif

#define LUA_EXTRASPACE (sizeof(void*))
