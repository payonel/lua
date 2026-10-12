package lua.gc;

import lua.object.GCObject;

/* the entries of ephemerons that wait for their keys (see 'convergeephemerons') */
public final class EphIndex
{
  public GCObject[] keys = new GCObject[0];   /* the slots' keys (nullptr: an empty slot) */
  public GCObject[] values = new GCObject[0]; /* and their values */
  public int cap = 0; /* the slots (a power of 2; at most half of them used) */
  public int n = 0;
  public GCObject[] ready = new GCObject[0]; /* keys marked, whose entries wait ('cap / 2' of room) */
  public int nready = 0;
  public boolean strong = false; /* (the host did not allow more room) */
}
