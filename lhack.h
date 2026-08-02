#pragma once

#ifdef LUA_DEBUG

#include "lua.h"
#include <stdint.h>

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


#define LUAL_BUFFERSIZE 23
#define MINSTRTABSIZE   2
#define MAXIWTHABS      3

#define MAXINDEXRK 1

#define LUAI_MAXSTACK 68000

#define LUAI_MAXCCALLS 180

#define luai_tracegc(L, f)  luai_tracegctest(L, f)

#define luaL_newstate() luai_newstate()
#define luai_openlibs(L) luai_openlibstest(L)

#else

#define luai_userstateopen(L) ((void) L)
#define luai_userstateclose(L) ((void) L)
#define luai_userstatethread(L, L1) ((void) L)
#define luai_userstatefree(L, L1) ((void) L)

#define lua_lock(L)   ((void) 0)
#define lua_unlock(L) ((void) 0)

#define LUAL_BUFFERSIZE ((int) (16 * sizeof(void*) * sizeof(lua_Number)))

#define MAXIWTHABS 128

#define MAXINDEXRK MAXARG_B

#if 1000000 < (INT_MAX / 2)
#define LUAI_MAXSTACK 1000000
#else
#define LUAI_MAXSTACK (INT_MAX / 2u)
#endif

#define LUAI_MAXCCALLS 200

#define luai_openlibs(L) luaL_openselectedlibs(L, ~0, 0)
#define luai_tracegc(L, f) ((void) 0)

#endif

#define LUA_EXTRASPACE (sizeof(void*))
