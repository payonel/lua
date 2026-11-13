using System;
using Payonel.LungoLua.Compat;
using ByteOwner = Payonel.LungoLua.Bytes.ByteOwner;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Runtime.Lmem;

namespace Payonel.LungoLua.Api;

/*
** The state of a library function that a freeze may stop in its middle
** (see 'lua_freezeonly'): integers and the bytes of a string buffer, in a
** userdata on the stack, where its continuation finds them.
*/
public sealed class luaL_Box : lua_UserObject
{
  public long[] ints = new long[4];
  public ByteOwner bytes = new ByteOwner(); /* a buffer saved by 'luaL_buffsave' */
  public int used = 0;

  public void freed(lua_State L)
  {
    if (!bytes.empty())
      luaM_alloc_char_span(L.l_G, null, bytes, 0, false); /* an abandoned buffer */
    bytes = new ByteOwner();
  }

  public string persistkind()
  {
    return "box";
  }

  /* its state: the four integers, the bytes used, then the bytes */

  public int persistints(long[] @out)
  {
    for (int i = 0; i < 4; i++)
      @out[i] = ints[i];
    @out[4] = (long) used;
    return 5;
  }

  public ByteView persistbytes()
  {
    return bytes.view().substr(0, used);
  }
}
