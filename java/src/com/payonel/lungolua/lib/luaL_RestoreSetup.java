package com.payonel.lungolua.lib;

import com.payonel.lungolua.runtime.lua_State;

/*
** The kernel's restore, in one call: a new state with allocator 'f', which
** hashes as the persisting state did (its tables keep their order), set
** up by 'setup' (if given), which opens the same libraries and makes the
** host's permanents, in the same order as for the persisting state, and
** pushes the inverse permanents table (key -> object), or nil; then the
** whole state in 'data' (luaL_persiststate) is restored into it (a frozen
** main thread into its main thread). Gives the restored thread the state
** was persisted from (its main thread, or the host's other thread), or
** NULL with a message (nothing is left open).
*/
public interface luaL_RestoreSetup
{
  void setup(lua_State L);
}
