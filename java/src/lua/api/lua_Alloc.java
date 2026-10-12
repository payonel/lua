package lua.api;

public interface lua_Alloc
{
  boolean permit(long osize, long nsize, int tag);
}
