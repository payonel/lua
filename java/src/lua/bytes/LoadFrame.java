package lua.bytes;

/*
** The frame of a function of a load, as a freeze leaves it: where it was
** (its state, the call it was in, which it makes again after the thaw)
** and its locals. Each function with locals to keep has its own frame
** type, with them as its fields; this one has none.
*/
public class LoadFrame
{
  public int fn = 0;              /* the function ('LF_*', set when recorded) */
  public int site = 0;            /* its state (0: its start) */
  public LoadFrame below = null;  /* (in the box: the frame recorded before it) */

  public LoadFrame()
  {
  }

  /* the copy (C++'s copy constructor and assignment) */
  public LoadFrame(LoadFrame o)
  {
    fn = o.fn;
    site = o.site;
    below = o.below;
  }

  /* a copy, to record */
  public LoadFrame copy()
  {
    return new LoadFrame(this);
  }

  /* its size, counted in the box's room for frames (the C++ sizeof) */
  public int size()
  {
    return 24;
  }
}
