
#pragma once

#include "lua.h"
#include "internal_string.h"
#include "literals.h"
#include <array>

class InternalString;

int lua_islightcfunction(lua_State* L, int idx);

const InternalString* lua_toistring(lua_State* L, int idx);

int lua_loadi(lua_State* L, lua_Reader reader, void* data, std::optional<std::string_view> chunkname, std::optional<std::string_view> mode);

int lua_reservebytes(lua_State* L, size_t sz);
void lua_releasebytes(lua_State* L, size_t sz);

void lua_pushistring(lua_State* L, InternalString&& s);
void lua_pushistringext(lua_State* L, InternalString&& s);
void lua_pushistringref(lua_State* L, std::string_view bytes);

void lua_setmemlimit(lua_State* L, size_t limit);
size_t lua_getmemlimit(lua_State* L);

void* lua_newuserdata(lua_State* L, size_t sz);
int lua_resetthread(lua_State* L);


const InternalString* luaL_checkistring(lua_State* L, int arg);
const InternalString* luaL_optistring(lua_State* L, int arg, const InternalString* def);
const InternalString* luaL_toistring(lua_State* L, int idx);

int luaL_loadbufferi(lua_State* L, byte_view buff, std::optional<std::string_view> name, std::optional<std::string_view> mode);
int luaL_loadfilei(lua_State* L, std::optional<std::string_view> filename, std::optional<std::string_view> mode);

int lua_getfieldi(lua_State* L, int idx, std::string_view k);
void lua_setfieldi(lua_State* L, int idx, std::string_view k);

const InternalString* lua_pushfstringi(lua_State* L, std::string_view fmt);

int lua_getglobali(lua_State* L, std::string_view name);
void lua_setglobali(lua_State* L, std::string_view name);

void lua_warningi(lua_State* L, std::string_view msg, int tocont);

int luaL_errori(lua_State* L, std::string_view fmt);
int luaL_argerrori(lua_State* L, int arg, std::string_view extramsg);
int luaL_typeerrori(lua_State* L, int arg, std::string_view tname);
int luaL_checkoptioni(lua_State* L, int arg, std::optional<std::string_view> def,
                      std::span<const std::string_view> lst);

struct luaL_Regi
{
  std::string_view name;
  lua_CFunction func;
};

struct luaL_RegCFI
{
  std::string_view name;
  CFuncInfo* cfi;
};

void luaL_setfuncsi(lua_State* L, std::span<const luaL_Regi> funcs, int nup);
void luaL_newlibi(lua_State* L, std::span<const luaL_Regi> funcs);
void luaL_setfuncscfi(lua_State* L, std::span<const luaL_RegCFI> funcs);
void luaL_newlibcfi(lua_State* L, std::span<const luaL_RegCFI> funcs);

void luaL_argchecki(lua_State* L, bool cond, int arg, std::string_view extramsg);
void luaL_argexpectedi(lua_State* L, bool cond, int arg, std::string_view tname);

void luaL_checkstacki(lua_State* L, int space, std::optional<std::string_view> msg);

int luaL_getmetafieldi(lua_State* L, int obj, std::string_view event);
int luaL_callmetai(lua_State* L, int obj, std::string_view event);

int luaL_newmetatablei(lua_State* L, std::string_view tname);
void luaL_setmetatablei(lua_State* L, std::string_view tname);
void* luaL_testudatai(lua_State* L, int ud, std::string_view tname);
void* luaL_checkudatai(lua_State* L, int ud, std::string_view tname);
int luaL_getmetatablei(lua_State* L, std::string_view tname);

int luaL_getsubtablei(lua_State* L, int idx, std::string_view fname);
void luaL_requirefi(lua_State* L, std::string_view modname, lua_CFunction openf, int glb);
void luaL_tracebacki(lua_State* L, lua_State* L1, std::optional<std::string_view> msg, int level);

InternalString lua_funcaddressi(lua_State* L, int idx);

std::string_view lua_typenamei(lua_State* L, int tp);

std::optional<std::string_view> lua_numbertocstringi(lua_State* L, int idx, std::span<char> buff);

int luaL_fileresulti(lua_State* L, int stat, std::optional<std::string_view> fname, int en);
int luaL_execresulti(lua_State* L, int stat, int en);

struct luaL_Buffer;
void luaL_addgsubi(luaL_Buffer* b, std::string_view s, std::string_view p, std::string_view r);
std::string_view luaL_gsubi(lua_State* L, std::string_view s, std::string_view p, std::string_view r);

struct InternalDebug
{
  int event = 0;
  std::optional<std::string_view> name;
  std::optional<std::string_view> namewhat;
  std::string_view what;
  std::string_view source;
  int currentline = 0;
  int linedefined = 0;
  int lastlinedefined = 0;
  unsigned char nups = 0;
  unsigned char nparams = 0;
  char isvararg = 0;
  unsigned char extraargs = 0;
  char istailcall = 0;
  int ftransfer = 0;
  int ntransfer = 0;
  std::array<char, LUA_IDSIZE> short_src_out = {};
  std::string_view short_src() const
  {
    size_t len = strnlen(short_src_out.data(), LUA_IDSIZE);
    return std::string_view(short_src_out.data(), len);
  }
  CallInfo* i_ci = nullptr;
};

std::optional<InternalDebug> lua_getstacki(lua_State* L, int level);
std::optional<InternalDebug> lua_getinfoi(lua_State* L, std::string_view options, const InternalDebug& ar);
const InternalString* lua_getlocali(lua_State* L, const InternalDebug* ar, int n);
const InternalString* lua_setlocali(lua_State* L, const InternalDebug* ar, int n);
const InternalString* lua_getupvaluei(lua_State* L, int funcindex, int n);
const InternalString* lua_setupvaluei(lua_State* L, int funcindex, int n);

