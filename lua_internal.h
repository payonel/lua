
#pragma once

#include "lua.h"
#include "internal_string.h"
#include "literals.h"
#include "lauxlib.h"
#include <array>

namespace lua::bytes
{
class InternalString;
}

int lua_islightcfunction(lua_State* L, int idx);

const InternalString* lua_toistring(lua_State* L, int idx);

int lua_loadi(lua_State* L, lua_Reader* reader, std::optional<byte_view> chunkname, std::optional<byte_view> mode);


void lua_pushistring(lua_State* L, InternalString&& s);
void lua_pushistringext(lua_State* L, InternalString&& s);
void lua_pushistringref(lua_State* L, byte_view bytes);


void* lua_newuserdata(lua_State* L, size_t sz);
int lua_resetthread(lua_State* L);


const InternalString* luaL_checkistring(lua_State* L, int arg);
const InternalString* luaL_optistring(lua_State* L, int arg, const InternalString* def);
const InternalString* luaL_toistring(lua_State* L, int idx);
int luaL_toistringk(lua_State* L, int idx, lua_KContext ctx, lua_KFunction k);
const InternalString* luaL_tostringresult(lua_State* L);
void luaL_tostringdefault(lua_State* L, int idx);

int luaL_loadbufferi(lua_State* L, byte_view buff, std::optional<byte_view> name, std::optional<byte_view> mode);

namespace lua::runtime
{
struct global_State;
}

/*
** The state of a library function that a freeze may stop in its middle
** (see 'lua_freezeonly'): integers and the bytes of a string buffer, in a
** userdata on the stack, where its continuation finds them.
*/
struct luaL_Box : lua_UserObject
{
  ::lua::runtime::global_State* g = nullptr;
  std::array<lua_Integer, 4> ints{};
  byte_owner bytes = {}; /* a buffer saved by 'luaL_buffsave' */
  size_t used = 0;
  ~luaL_Box() override;
};

luaL_Box* luaL_newbox(lua_State* L);
luaL_Box* luaL_tobox(lua_State* L, int idx);
void luaL_buffsave(luaL_Buffer* B, luaL_Box* box);
void luaL_buffload(luaL_Buffer* B, luaL_Box* box);
lua_Integer luaL_lenk(lua_State* L, int idx, lua_KContext ctx, lua_KFunction k);
lua_Integer luaL_lenresult(lua_State* L);
int luaL_loadfilei(lua_State* L, std::optional<byte_view> filename, std::optional<byte_view> mode);

int lua_getfieldi(lua_State* L, int idx, byte_view k);
int lua_getfieldik(lua_State* L, int idx, byte_view key, lua_KContext ctx, lua_KFunction k);
void lua_setfieldik(lua_State* L, int idx, byte_view key, lua_KContext ctx, lua_KFunction k);
/* freezing: resume and thaw by a C function with continuation 'k', which
   a freeze passing through goes on with (getting the status of 'L'); the
   number of results of 'L' then; the coroutine a frozen 'L' resumed */
lua_ResumeResult lua_resumek(lua_State* L, lua_State* from, int nargs, lua_KContext ctx, lua_KFunction k);
lua_ResumeResult lua_thawk(lua_State* L, lua_State* from, lua_KContext ctx, lua_KFunction k);
int lua_resumenres(lua_State* L, int status);
lua_State* lua_frozenchild(lua_State* L);
/* the calls with continuations of the running C function freeze but do
   not yield; C work of 'n' steps (true when the budget is spent); a check
   point there (goes on in 'k' after a thaw) */
void lua_freezeonly(lua_State* L);
lua_State* lua_budgetroot(lua_State* co);
void lua_setbudgetvalue(lua_State* L, lua_State* co);
void lua_pushbudgetvalue(lua_State* L, lua_State* co, int effective);
lua_State* lua_budgetthread(lua_State* L);
void lua_setbudgetthread(lua_State* L, lua_State* B);
int lua_budgeted(lua_State* L);
[[noreturn]] void lua_closeself(lua_State* L);
void lua_callhook(lua_State* L, int nargs);
int lua_hasdrain(lua_State* L);
void lua_pushdrain(lua_State* L);
void lua_pushthreadi(lua_State* L, lua_State* th);
int lua_rawlessthan(lua_State* L, int index1, int index2);
int lua_isplaintable(lua_State* L, int idx);
int lua_plaingetrange(lua_State* L, int idx, lua_Integer i, int n);
lua_Integer lua_plainmove(lua_State* L, int idx1, lua_Integer s, int idx2, lua_Integer d,
                          lua_Integer n, int up);
int lua_spend(lua_State* L, int n);
void lua_checkpointk(lua_State* L, lua_KContext ctx, lua_KFunction k);
void lua_setfieldi(lua_State* L, int idx, byte_view k);

const InternalString* lua_pushfstringi(lua_State* L, byte_view fmt);

int lua_getglobali(lua_State* L, byte_view name);
void lua_setglobali(lua_State* L, byte_view name);

void lua_warningi(lua_State* L, byte_view msg, int tocont);

int luaL_errori(lua_State* L, byte_view fmt);
int luaL_argerrori(lua_State* L, int arg, byte_view extramsg);
int luaL_typeerrori(lua_State* L, int arg, byte_view tname);
int luaL_checkoptioni(lua_State* L, int arg, std::optional<byte_view> def,
                      std::span<const byte_view> lst);

struct luaL_Regi
{
  byte_view name;
  lua_CFunction func;
};

struct luaL_RegCFI
{
  byte_view name;
  CFuncInfo* cfi;
};

void luaL_setfuncsi(lua_State* L, std::span<const luaL_Regi> funcs, int nup);
void luaL_newlibi(lua_State* L, std::span<const luaL_Regi> funcs);
void luaL_setfuncscfi(lua_State* L, std::span<const luaL_RegCFI> funcs);
void luaL_newlibcfi(lua_State* L, std::span<const luaL_RegCFI> funcs);

void luaL_argchecki(lua_State* L, bool cond, int arg, byte_view extramsg);
void luaL_argexpectedi(lua_State* L, bool cond, int arg, byte_view tname);

void luaL_checkstacki(lua_State* L, int space, std::optional<byte_view> msg);

int luaL_getmetafieldi(lua_State* L, int obj, byte_view event);
int luaL_callmetai(lua_State* L, int obj, byte_view event);

int luaL_newmetatablei(lua_State* L, byte_view tname);
void luaL_setmetatablei(lua_State* L, byte_view tname);
void* luaL_testudatai(lua_State* L, int ud, byte_view tname);
void* luaL_checkudatai(lua_State* L, int ud, byte_view tname);
lua_UserObject* luaL_testuserobjecti(lua_State* L, int ud, byte_view tname);
lua_UserObject* luaL_checkuserobjecti(lua_State* L, int ud, byte_view tname);
int luaL_getmetatablei(lua_State* L, byte_view tname);

int luaL_getsubtablei(lua_State* L, int idx, byte_view fname);
void luaL_requirefi(lua_State* L, byte_view modname, lua_CFunction openf, int glb);
void luaL_tracebacki(lua_State* L, lua_State* L1, std::optional<byte_view> msg, int level);

InternalString lua_funcaddressi(lua_State* L, int idx);

byte_view lua_typenamei(lua_State* L, int tp);

std::optional<byte_view> lua_numbertocstringi(lua_State* L, int idx, byte_span buff);

int luaL_fileresulti(lua_State* L, int stat, std::optional<byte_view> fname, int en);
int luaL_execresulti(lua_State* L, int stat, int en);

struct luaL_Buffer;
void luaL_addgsubi(luaL_Buffer* b, byte_view s, byte_view p, byte_view r);
byte_view luaL_gsubi(lua_State* L, byte_view s, byte_view p, byte_view r);

struct InternalDebug
{
  int event = 0;
  std::optional<byte_view> name;
  std::optional<byte_view> namewhat;
  byte_view what;
  byte_view source;
  int currentline = 0;
  int linedefined = 0;
  int lastlinedefined = 0;
  int nups = 0;
  int nparams = 0;
  char isvararg = 0;
  int extraargs = 0;
  char istailcall = 0;
  int ftransfer = 0;
  int ntransfer = 0;
  byte_inline<LUA_IDSIZE> short_src_out; /* (S) */
  CallInfo* i_ci = nullptr;
};

std::optional<InternalDebug> lua_getstacki(lua_State* L, int level);
std::optional<InternalDebug> lua_getinfoi(lua_State* L, byte_view options, const InternalDebug& ar);
const InternalString* lua_getlocali(lua_State* L, const InternalDebug* ar, int n);
const InternalString* lua_setlocali(lua_State* L, const InternalDebug* ar, int n);
const InternalString* lua_getupvaluei(lua_State* L, int funcindex, int n);
const InternalString* lua_setupvaluei(lua_State* L, int funcindex, int n);

