using System;
using Payonel.LungoLua.Compat;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Api;

/*
** Functions to be called by the debugger in specific events
*/
public delegate void lua_Hook(lua_State L, lua_Debug ar);

