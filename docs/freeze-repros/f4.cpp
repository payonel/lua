#include <cstdio>
#include <cstdlib>
#include <string>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
static lua_State* mainL; static int calls=0, at=1, frz=0, every=0;
static void bud(lua_State* L){ calls++; std::fprintf(stderr,"[bud call %d on %s]\n",calls,L==mainL?"main":"other"); if(calls==at||(every&&calls>=at)){ if(lua_freeze(L,L)) {frz++; std::fprintf(stderr,"[froze]\n");} else std::fprintf(stderr,"[freeze refused]\n"); } }
int main(int argc,char**argv){
  at = argc>1?atoi(argv[1]):1; every = argc>2?atoi(argv[2]):0; int budget=argc>3?atoi(argv[3]):1;
  lua_State*L=luaL_newstate(); luaL_openlibs(L); mainL=L;
  const char* prog=
   "ran=0 io.stdout:setvbuf('no')\n"
   "for i=1,5 do setmetatable({}, {__gc=function() local s=0 for j=1,20 do s=s+j end ran=ran+1 io.write('gc ',i,' done s=',s,'\\n') end}) end\n"
   "return 1";
  luaL_loadstring(L,prog); lua_pcall(L,0,0,0);
  lua_setbudget(L,budget,bud);
  std::printf("closing (at=%d every=%d budget=%d)\n",at,every,budget); std::fflush(stdout);
  lua_close(L);
  std::printf("closed: budget callback calls=%d freezes=%d\n",calls,frz);
}
