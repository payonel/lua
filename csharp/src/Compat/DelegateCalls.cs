using Payonel.LungoLua.Bytes;
using Payonel.LungoLua.Api;
using Payonel.LungoLua.Runtime;
namespace Payonel.LungoLua.Compat;

public static class lua_CFunction_call
{
  public static int call(this lua_CFunction f, lua_State L)
  {
    return f(L);
  }
}

public static class lua_KFunction_call
{
  public static int call(this lua_KFunction f, lua_State L, int status, long ctx)
  {
    return f(L, status, ctx);
  }
}

public static class lua_Budget_call
{
  public static void call(this lua_Budget f, lua_State user_co, lua_State spender)
  {
    f(user_co, spender);
  }
}

public static class lua_Hook_call
{
  public static void call(this lua_Hook f, lua_State L, lua_Debug ar)
  {
    f(L, ar);
  }
}

public static class lua_ObjectFactory_call
{
  public static lua_UserObject call(this lua_ObjectFactory f, lua_State L, long[] ints, int nints, ByteView bytes)
  {
    return f(L, ints, nints, bytes);
  }
}

public static class lua_CClosureKind_call
{
  public static int call(this lua_CClosureKind.Save f, Payonel.LungoLua.Object.CClosure cl, long[] ints)
  {
    return f(cl, ints);
  }

  public static bool call(this lua_CClosureKind.Make f, lua_State L, Payonel.LungoLua.Object.CClosure cl, long[] ints, int nints)
  {
    return f(L, cl, ints, nints);
  }
}
