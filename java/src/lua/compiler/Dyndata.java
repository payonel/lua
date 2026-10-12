package lua.compiler;

/* dynamic structures used by the parser */
public final class Dyndata
{
  public static final class Actvar
  { /* list of all active local variables */
    public Vardesc[] arr = new Vardesc[0];
    public int n;
    public int size;
  }

  public final Actvar actvar = new Actvar();
  public final Labellist gt = new Labellist();    /* list of pending gotos */
  public final Labellist label = new Labellist(); /* list of active labels */

  public Dyndata()
  {
  }

  public Dyndata(Dyndata o)
  {
    copyfrom(o);
  }

  public void copyfrom(Dyndata o)
  {
    actvar.arr = o.actvar.arr;
    actvar.n = o.actvar.n;
    actvar.size = o.actvar.size;
    gt.copyfrom(o.gt);
    label.copyfrom(o.label);
  }
}
