package lua.runtime;

public final class Ltm
{
  private Ltm()
  {
  }

  /*
   * WARNING: if you change the order of this enumeration,
   * grep "ORDER TM" and "ORDER OP"
   */
  public static final int TM_INDEX = 0;
  public static final int TM_NEWINDEX = 1;
  public static final int TM_GC = 2;
  public static final int TM_MODE = 3;
  public static final int TM_LEN = 4;
  public static final int TM_EQ = 5; /* last tag method with fast access */
  public static final int TM_ADD = 6;
  public static final int TM_SUB = 7;
  public static final int TM_MUL = 8;
  public static final int TM_MOD = 9;
  public static final int TM_POW = 10;
  public static final int TM_DIV = 11;
  public static final int TM_IDIV = 12;
  public static final int TM_BAND = 13;
  public static final int TM_BOR = 14;
  public static final int TM_BXOR = 15;
  public static final int TM_SHL = 16;
  public static final int TM_SHR = 17;
  public static final int TM_UNM = 18;
  public static final int TM_BNOT = 19;
  public static final int TM_LT = 20;
  public static final int TM_LE = 21;
  public static final int TM_CONCAT = 22;
  public static final int TM_CALL = 23;
  public static final int TM_CLOSE = 24;
  public static final int TM_N = 25; /* number of elements in the enum */
}
