package com.payonel.lungolua.object;

/* Common type for all collectable objects */
public class GCObject
{
  public GCObject next;
  public byte tt;
  public byte marked;
  public int owner; /* the id of the budget group in effect when its '__gc' was registered (0: none); 16 bits */
  public int hash;  /* as a table key: given when made (luaC_link), if it hashes by identity */

  public GCObject()
  {
  }

  public GCObject(GCObject next, byte tt, byte marked, int owner, int hash)
  {
    this.next = next;
    this.tt = tt;
    this.marked = marked;
    this.owner = owner;
    this.hash = hash;
  }
}
