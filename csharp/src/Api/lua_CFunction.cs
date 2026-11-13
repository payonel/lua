using System;
using Payonel.LungoLua.Compat;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Api;

public delegate int lua_CFunction(lua_State L);

