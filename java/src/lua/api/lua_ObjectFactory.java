package lua.api;

import lua.bytes.ByteView;
import lua.runtime.lua_State;

/*
** Kinds of user objects that can be kept outside the process (see
** 'lua_UserObject::persistkind'): the factory makes the object from its
** state (nullptr if the state is not valid).
*/
@FunctionalInterface
public interface lua_ObjectFactory
{
  lua_UserObject call(lua_State L, long[] ints, int nints, ByteView bytes);
}
