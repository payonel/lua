package lua.runtime;

/* type of protected functions, to be ran by 'runprotected' */
public interface ProtectedCall
{
  void run(lua_State L);
}
