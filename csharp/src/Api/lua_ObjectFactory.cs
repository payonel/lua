using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Api;

/*
** Kinds of user objects that can be kept outside the process (see
** 'lua_UserObject::persistkind'): the factory makes the object from its
** state (nullptr if the state is not valid).
*/
public delegate lua_UserObject lua_ObjectFactory(lua_State L, long[] ints, int nints, ByteView bytes);

