package lua.object;

/* a table's node: its value ('val', the node itself) and its key */
public final class Node extends TValue
{
  public byte key_tt;   /* key type */
  public int next;      /* for chaining */
  public long key_prim; /* key value */
  public Object key_ref;
}
