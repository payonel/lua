using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Api;

public interface lua_UserObject
{
  /*
  ** Gives back what the object holds outside its memory (counted bytes, an
  ** open file): called once, when the userdata holding it is freed, or when
  ** that userdata could not be made.
  */
   void freed(lua_State L)
  {
  }

  /*
  ** For a tool that keeps a state outside the process: the name of the kind
  ** the object is registered under ('lua_registerobjectkind'), or nullptr
  ** if it cannot be kept; and its state as up to 8 integers (put in 'ints',
  ** their count returned) and bytes that stay valid while the object does.
  */
   string persistkind()
  {
    return null;
  }

   int persistints(long[] ints)
  {
    return 0;
  }

   ByteView persistbytes()
  {
    return ByteView.EMPTY;
  }

  /* are its user values part of its state (else they are kept as nil)? */
   bool persistuservalues()
  {
    return true;
  }
}
