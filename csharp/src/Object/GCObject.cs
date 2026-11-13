using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Object;

/* Common type for all collectable objects */
public class GCObject
{
  public GCObject next;
  public sbyte tt;
  public sbyte marked;
  public int owner; /* the id of the budget group in effect when its '__gc' was registered (0: none); 16 bits */
  public int hash;  /* as a table key: given when made (luaC_link), if it hashes by identity */

  public GCObject()
  {
  }

  public GCObject(GCObject next, sbyte tt, sbyte marked, int owner, int hash)
  {
    this.next = next;
    this.tt = tt;
    this.marked = marked;
    this.owner = owner;
    this.hash = hash;
  }
}
