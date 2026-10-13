package com.payonel.lungolua.compiler;

/* list of labels or gotos */
public final class Labellist
{
  public Labeldesc[] arr = new Labeldesc[0]; /* array */
  public int n;    /* number of entries in use */
  public int size; /* array size */

  public Labellist()
  {
  }

  public Labellist(Labellist o)
  {
    copyfrom(o);
  }

  public void copyfrom(Labellist o)
  {
    arr = o.arr;
    n = o.n;
    size = o.size;
  }
}
