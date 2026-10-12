package lua.api;

import static lua.runtime.Lmem.*;

import lua.bytes.ByteOwner;
import lua.bytes.ByteView;
import lua.runtime.lua_State;

/*
** The state of a library function that a freeze may stop in its middle
** (see 'lua_freezeonly'): integers and the bytes of a string buffer, in a
** userdata on the stack, where its continuation finds them.
*/
public final class luaL_Box implements lua_UserObject
{
  public long[] ints = new long[4];
  public ByteOwner bytes = new ByteOwner(); /* a buffer saved by 'luaL_buffsave' */
  public int used = 0;

  @Override
  public void freed(lua_State L)
  {
    if (!bytes.empty())
      luaM_alloc_char_span(L.l_G, null, bytes, 0, false); /* an abandoned buffer */
    bytes = new ByteOwner();
  }

  @Override
  public String persistkind()
  {
    return "box";
  }

  /* its state: the four integers, the bytes used, then the bytes */
  @Override
  public int persistints(long[] out)
  {
    for (int i = 0; i < 4; i++)
      out[i] = ints[i];
    out[4] = (long) used;
    return 5;
  }

  @Override
  public ByteView persistbytes()
  {
    return bytes.view().substr(0, used);
  }
}
