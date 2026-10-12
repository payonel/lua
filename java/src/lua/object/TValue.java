package lua.object;

/* a tagged value: never shared; 'getobj'/'copyobj' copy its fields */
public class TValue
{
  public long prim;
  public Object ref;
  public byte tt_;

  public TValue()
  {
  }

  public TValue(long prim, Object ref, byte tt)
  {
    this.prim = prim;
    this.ref = ref;
    this.tt_ = tt;
  }
}
