package lua.runtime;

/* the bytes charged for each kind of engine object ('cost::') */
public final class Cost
{
  private Cost()
  {
  }

  public static final int Byte = 1;
  public static final int Ref = 8;
  public static final int Instruction = 4;
  public static final int Value = 16;
  public static final int TValue = 24;
  public static final int Node = 48;
  public static final int LocVar = 16;
  public static final int Vardesc = 48;
  public static final int Upvaldesc = 24;
  public static final int Labeldesc = 24;
  public static final int AbsLineInfo = 8;
  public static final int CallInfo = 88;
  public static final int TString = 64;
  public static final int Udata = 72;
  public static final int Table = 96;
  public static final int CClosure = 48;
  public static final int LClosure = 48;
  public static final int CFuncInfo = 24;
  public static final int UpVal = 72;
  public static final int BudgetGroup = 104;
  public static final int Proto = 168;
  public static final int Thread = 280;
  public static final int Global = 1024;
}
