#include <cstdio>
#include <string>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
static int calls=0, at=0, frz=0; static lua_State* tgt;
static void bud(lua_State* L){ calls++; if(calls==at){ if(lua_freeze(L,tgt)) frz++; } }
static std::string S(lua_State*L,int i){auto s=lua_tolstring(L,i); return s?std::string(s->data(),s->size()):"(non-string type "+std::to_string(lua_type(L,i))+")";}
// mode 0: pcall(5) ; 1: pcall(nil) ; 2: pcall(function() error('boom') end) ; 3: handler errors on non-callable ; 4: handler errors, callable erroring
int main(){
  const char* names[]={"pcall number","pcall nil","pcall erroring fn","msgh errors, pcall number","msgh errors, pcall erroring fn"};
  for(int mode=0;mode<5;mode++){
    for(int a=0;a<=12;a++){
      lua_State*L=luaL_newstate(); luaL_openlibs(L); tgt=L;
      const char* hnd = mode>=3 ? "function(m) local s=0 for i=1,10 do s=s+i end error('H:'..tostring(m)) end"
                                : "function(m) local s=0 for i=1,10 do s=s+i end return 'H:'..tostring(m)..':'..s end";
      std::string code = std::string("return ")+hnd;
      luaL_loadstring(L,code.c_str()); lua_call(L,0,1); // handler at index 1
      if(mode==2||mode==4) luaL_loadstring(L,"error('boom')"); else if(mode==1) lua_pushnil(L); else lua_pushinteger(L,5);
      calls=0; at=a; frz=0; lua_setbudget(L,1,a?bud:nullptr);
      int st=lua_pcall(L,0,1,1);
      int th=0; bool froze=lua_isfrozen(L);
      lua_ResumeResult r{st,0};
      while(lua_isfrozen(L)&&th<100){ r=lua_thaw(L,nullptr); th++; }
      lua_setbudget(L,0,nullptr);
      int fs = froze? r.status: st;
      std::printf("%-32s at=%2d frozen=%d thaws=%d status=%d top=%d top-val=%s\n",names[mode],a,froze,th,fs,lua_gettop(L),S(L,-1).c_str());
      lua_close(L);
    }
  }
}
