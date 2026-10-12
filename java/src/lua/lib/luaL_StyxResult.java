package lua.lib;

import lua.api.HostString;

/*
** Styx: the kernel's persistence. The host persists the whole state while
** it is at rest (no Lua code runs: the kernel has frozen or suspended its
** programs and yielded back to the host) and restores it into a new
** state; what it needs back it anchors in the registry (the main thread's
** stack is not part of a state). Scripts cannot persist.
**
** luaL_persiststate writes the state to 'w'; 'perms' is the stack index of
** the permanents table (object -> key), or 0 for none. Nothing is
** allocated in the state and no Lua code runs. 'L' is the main thread, or
** a thread the state holds (for a frozen main thread, the host's other
** thread, anchored before): luaL_restorestate gives it back. Gives LUA_OK,
** or an error status with a message.
*/
public final class luaL_StyxResult
{
  public int status;
  public HostString msg = new HostString();

  public luaL_StyxResult()
  {
  }

  public luaL_StyxResult(int status, HostString msg)
  {
    this.status = status;
    this.msg = msg;
  }
}
