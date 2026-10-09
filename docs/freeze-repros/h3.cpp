#include <cstdlib>
#include <cstdio>
#include <string>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
static int calls=0, at=0; static lua_State* tgt;
static void bud(lua_State* L){ calls++; if(calls==at) lua_freeze(L,tgt); }
static std::string S(lua_State*L,int i){auto s=lua_tolstring(L,i); return s?std::string(s->data(),s->size()):"(type "+std::to_string(lua_type(L,i))+")";}
static const char* progs[]={
 // A: handler errors, xpcall of non-callable, in coroutine, then a later error
 "local co=coroutine.create(function()\n"
 " local a,b=xpcall(5,function(m) error('H1') end)\n"
 " local c,d=xpcall(function() error('E2') end,function(m) return 'ok:'..m end)\n"
 " return tostring(a)..'|'..tostring(b)..'|'..tostring(c)..'|'..tostring(d) end)\n"
 "return coroutine.resume(co)",
 // B: handler errors in an erroring function (recover point is xpcall; error raised in callee)
 "local co=coroutine.create(function()\n"
 " local a,b=xpcall(function() error('E1') end,function(m) error('H1') end)\n"
 " local c,d=xpcall(5,function(m) return 'ok:'..m end)\n"
 " return tostring(a)..'|'..tostring(b)..'|'..tostring(c)..'|'..tostring(d) end)\n"
 "return coroutine.resume(co)",
 // C: handler errors with a yield after
 "local co=coroutine.create(function()\n"
 " local a,b=xpcall(5,function(m) error('H1') end)\n"
 " coroutine.yield('y')\n"
 " local c,d=pcall(error,'E2')\n"
 " return tostring(a)..'|'..tostring(b)..'|'..tostring(c)..'|'..tostring(d) end)\n"
 "local r1,r2=coroutine.resume(co) local r3,r4=coroutine.resume(co) return tostring(r1)..tostring(r2)..'//'..tostring(r3)..tostring(r4)",
 // D: handler that errors by calling non-callable
 "local co=coroutine.create(function()\n"
 " local a,b=xpcall(5,function(m) local x=nil; x() end)\n"
 " return tostring(a)..'|'..tostring(b) end)\n"
 "return coroutine.resume(co)",
};
int main(int argc,char**argv){
  for(int p=0;p<4;p++) for(int a=0;a<=(argc>1?atoi(argv[1]):0);a+= (argc>1?3:1)){
    lua_State*L=luaL_newstate(); luaL_openlibs(L); tgt=L; calls=0; at=a;
    lua_setbudget(L,1,a?bud:nullptr);
    int st=luaL_loadstring(L,progs[p]); st=lua_pcall(L,0,LUA_MULTRET,0);
    int th=0; lua_ResumeResult r{st,0}; bool fr=lua_isfrozen(L);
    while(lua_isfrozen(L)&&th<1000){ r=lua_thaw(L,nullptr); th++; }
    lua_setbudget(L,0,nullptr);
    std::printf("prog %d at=%d frozen=%d thaws=%d status=%d top=%d :",p,a,fr,th,fr?r.status:st,lua_gettop(L));
    for(int i=1;i<=lua_gettop(L);i++) std::printf(" [%s]",S(L,i).c_str());
    std::printf("\n"); lua_close(L);
  }
}
