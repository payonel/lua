/*
** Stable names for the functions the engine keeps in its state: the
** continuations of C functions (CallInfo 'k'), the light C functions that
** are part of the engine (the libraries' and the ones the engine pushes
** itself), and budget callbacks. A name stays the same from one build to
** the next, so a tool that stores a state outside the process can say which
** function a frame or a value holds, and find it again.
*/
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include "lua.h"
#include "lobject.h"
#include "lua_internal.h"

namespace
{
template <class F>
std::uintptr_t key(F f)
{
  return reinterpret_cast<std::uintptr_t>(f);
}

struct Names
{
  std::map<std::string, lua_KFunction, std::less<>> kbyname;
  std::map<std::uintptr_t, std::string> knames;
  std::map<std::string, CFuncInfo*, std::less<>> cfibyname;
  std::map<const CFuncInfo*, std::string> cfinames;
  std::map<std::string, lua_Budget, std::less<>> budgetbyname;
  std::map<std::uintptr_t, std::string> budgetnames;
};

Names& names()
{
  static Names n; /* (made on first use: registrations run at static initialization) */
  return n;
}

std::string_view view(byte_view v)
{
  return std::string_view(reinterpret_cast<const char*>(v.data()), v.size());
}
}

void lua_registerk(const char* name, lua_KFunction k)
{
  names().kbyname[name] = k;
  names().knames[key(k)] = name;
}

const char* lua_kname(lua_KFunction k)
{
  auto it = names().knames.find(key(k));
  return it == names().knames.end() ? nullptr : it->second.c_str();
}

lua_KFunction lua_kbyname(byte_view name)
{
  auto it = names().kbyname.find(view(name));
  return it == names().kbyname.end() ? nullptr : it->second;
}

void lua_registerbudget(const char* name, lua_Budget f)
{
  names().budgetbyname[name] = f;
  names().budgetnames[key(f)] = name;
}

const char* lua_budgetname(lua_Budget f)
{
  auto it = names().budgetnames.find(key(f));
  return it == names().budgetnames.end() ? nullptr : it->second.c_str();
}

lua_Budget lua_budgetbyname(byte_view name)
{
  auto it = names().budgetbyname.find(view(name));
  return it == names().budgetbyname.end() ? nullptr : it->second;
}

namespace lua::object
{
CFuncInfo::CFuncInfo(lua_CFunction f, const char* name)
  : GCObject{nullptr, LUA_VLCF, (1 << 5) | 4 /* BLACK | G_OLD */, 0, 0}
  , fptr(f)
{
  names().cfibyname[name] = this;
  names().cfinames[this] = name;
}
}

const char* lua_cfiname(const CFuncInfo* cfi)
{
  auto it = names().cfinames.find(cfi);
  return it == names().cfinames.end() ? nullptr : it->second.c_str();
}

CFuncInfo* lua_cfibyname(byte_view name)
{
  auto it = names().cfibyname.find(view(name));
  return it == names().cfibyname.end() ? nullptr : it->second;
}
