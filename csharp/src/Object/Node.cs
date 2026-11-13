using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Object;

/* a table's node: its value ('val', the node itself) and its key */
public sealed class Node : TValue
{
  public sbyte key_tt;   /* key type */
  public int next;      /* for chaining */
  public long key_prim; /* key value */
  public object key_ref;
}
