/*
** Stable names for the functions the engine keeps in its state: the
** continuations of C functions (CallInfo 'k'), the light C functions that
** are part of the engine (the libraries' and the ones the engine pushes
** itself), and budget callbacks. A name stays the same from one build to
** the next, so a tool that stores a state outside the process can say which
** function a frame or a value holds, and find it again.
*/
#include <cstring>
#include <map>
#include <string>

#include "lua.h"
#include "lobject.h"
#include "lua_internal.h"

namespace
{
/* (the functions are keys by identity: in Java, IdentityHashMaps of their single instances) */
struct Names
{
  std::map<std::string, lua_KFunction, std::less<>> kbyname;
  std::map<lua_KFunction, std::string> knames;
  std::map<std::string, CFuncInfo*, std::less<>> cfibyname;
  std::map<const CFuncInfo*, std::string> cfinames;
  std::map<std::string, lua_Budget, std::less<>> budgetbyname;
  std::map<lua_Budget, std::string> budgetnames;
  std::map<lua_Budget, void (*)(lua_State*)> budgetprepare;
  std::map<std::string, lua_ObjectFactory, std::less<>> objectkinds;
  std::map<lua_CFunction, lua_CClosureKind> cclosures;
  std::map<std::string, const lua_CClosureKind*, std::less<>> cclosurebyname;
};

Names& names()
{
  static Names n; /* (made on first use: registrations run at static initialization) */
  return n;
}

std::string_view view(byte_view v)
{
  return std::string_view(v.data(), v.size());
}
}

void lua_registerk(const char* name, lua_KFunction k)
{
  names().kbyname[name] = k;
  names().knames[k] = name;
}

const char* lua_kname(lua_KFunction k)
{
  auto it = names().knames.find(k);
  return it == names().knames.end() ? nullptr : it->second.c_str();
}

lua_KFunction lua_kbyname(byte_view name)
{
  auto it = names().kbyname.find(view(name));
  return it == names().kbyname.end() ? nullptr : it->second;
}

void lua_registerbudget(const char* name, lua_Budget f)
{
  lua_registerbudget(name, f, nullptr);
}

void lua_registerbudget(const char* name, lua_Budget f, void (*prepare)(lua_State*))
{
  names().budgetbyname[name] = f;
  names().budgetnames[f] = name;
  if (prepare != nullptr)
    names().budgetprepare[f] = prepare;
}

void lua_preparebudget(lua_State* L, lua_Budget f)
{
  auto it = names().budgetprepare.find(f);
  if (it != names().budgetprepare.end())
    it->second(L);
}

const char* lua_budgetname(lua_Budget f)
{
  auto it = names().budgetnames.find(f);
  return it == names().budgetnames.end() ? nullptr : it->second.c_str();
}

lua_Budget lua_budgetbyname(byte_view name)
{
  auto it = names().budgetbyname.find(view(name));
  return it == names().budgetbyname.end() ? nullptr : it->second;
}

void lua_registerobjectkind(const char* name, lua_ObjectFactory f)
{
  names().objectkinds[name] = f;
}

lua_ObjectFactory lua_objectkind(byte_view name)
{
  auto it = names().objectkinds.find(view(name));
  return it == names().objectkinds.end() ? nullptr : it->second;
}

void lua_registercclosure(const lua_CClosureKind& kind)
{
  auto& slot = names().cclosures[kind.f];
  slot = kind;
  names().cclosurebyname[kind.name] = &slot;
}

const lua_CClosureKind* lua_cclosurekind(lua_CFunction f)
{
  auto it = names().cclosures.find(f);
  return it == names().cclosures.end() ? nullptr : &it->second;
}

const lua_CClosureKind* lua_cclosurekindbyname(byte_view name)
{
  auto it = names().cclosurebyname.find(view(name));
  return it == names().cclosurebyname.end() ? nullptr : it->second;
}

namespace lua::object
{
CFuncInfo::CFuncInfo(lua_CFunction f, const char* name)
  : GCObject{nullptr, LUA_VLCF, (1 << 5) | 4 /* BLACK | G_OLD */, 0, 0}
  , fptr(f)
{
  names().cfibyname[name] = this;
  names().cfinames[this] = name;
  /* as a table key, it hashes by its name: the same in every process (FNV-1a; never 0) */
  unsigned int h = 2166136261u;
  for (const char* c = name; *c != '\0'; c++)
    h = (h ^ static_cast<unsigned char>(*c)) * 16777619u;
  hash = (h != 0) ? h : 1;
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
