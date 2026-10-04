
#pragma once

#include "lua.h"
#include "internal_string.h"
#include "literals.h"
#include <array>

class InternalString;

int lua_islightcfunction(lua_State* L, int idx);

const InternalString* lua_toistring(lua_State* L, int idx);

int lua_loadi(lua_State* L, lua_Reader* reader, std::optional<byte_view> chunkname, std::optional<byte_view> mode);

int lua_reservebytes(lua_State* L, size_t sz);
void lua_releasebytes(lua_State* L, size_t sz);

void lua_pushistring(lua_State* L, InternalString&& s);
void lua_pushistringext(lua_State* L, InternalString&& s);
void lua_pushistringref(lua_State* L, byte_view bytes);

void lua_setmemlimit(lua_State* L, size_t limit);
size_t lua_getmemlimit(lua_State* L);

void* lua_newuserdata(lua_State* L, size_t sz);
int lua_resetthread(lua_State* L);


const InternalString* luaL_checkistring(lua_State* L, int arg);
const InternalString* luaL_optistring(lua_State* L, int arg, const InternalString* def);
const InternalString* luaL_toistring(lua_State* L, int idx);

int luaL_loadbufferi(lua_State* L, byte_view buff, std::optional<byte_view> name, std::optional<byte_view> mode);
int luaL_loadfilei(lua_State* L, std::optional<byte_view> filename, std::optional<byte_view> mode);

int lua_getfieldi(lua_State* L, int idx, byte_view k);
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
  byte_owner short_src_out; /* lazily allocated by funcinfo() when the 'S' option is requested */
  CallInfo* i_ci = nullptr;

  InternalDebug() = default;
  InternalDebug(InternalDebug&&) = default;
  InternalDebug& operator=(InternalDebug&&) = default;
  InternalDebug(const InternalDebug& o)
      : event(o.event), name(o.name), namewhat(o.namewhat), what(o.what), source(o.source),
        currentline(o.currentline), linedefined(o.linedefined), lastlinedefined(o.lastlinedefined),
        nups(o.nups), nparams(o.nparams), isvararg(o.isvararg), extraargs(o.extraargs),
        istailcall(o.istailcall), ftransfer(o.ftransfer), ntransfer(o.ntransfer),
        short_src_out(o.short_src_out.data() != nullptr ? byte_owner(o.short_src_out.view()) : byte_owner()), i_ci(o.i_ci)
  {
  }
};

std::optional<InternalDebug> lua_getstacki(lua_State* L, int level);
std::optional<InternalDebug> lua_getinfoi(lua_State* L, byte_view options, const InternalDebug& ar);
const InternalString* lua_getlocali(lua_State* L, const InternalDebug* ar, int n);
const InternalString* lua_setlocali(lua_State* L, const InternalDebug* ar, int n);
const InternalString* lua_getupvaluei(lua_State* L, int funcindex, int n);
const InternalString* lua_setupvaluei(lua_State* L, int funcindex, int n);

