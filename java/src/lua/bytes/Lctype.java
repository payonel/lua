package lua.bytes;

/*
** WARNING: the functions defined here do not necessarily correspond
** to the similar functions in the standard C ctype.h. They are
** optimized for the specific needs of Lua.
*/
public final class Lctype
{
  private Lctype()
  {
  }

  public static final int ALPHABIT = 0;
  public static final int DIGITBIT = 1;
  public static final int PRINTBIT = 2;
  public static final int SPACEBIT = 3;
  public static final int XDIGITBIT = 4;

  public static int MASK(int B)
  {
    return 1 << B;
  }

  private static final byte NONA = 0x00; /* default */

  /* one entry for each character and for -1 (EOZ) */
  public static final byte[] luai_ctype_ = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
      0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
      0x04, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16,
      0x16, 0x16, 0x16, 0x04, 0x04, 0x04, 0x04, 0x04,
      0x04, 0x04, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15,
      0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
      0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
      0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04,
      0x05, 0x04, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15,
      0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
      0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
      0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04,
      0x00, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, 0x00, 0x00, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, NONA, NONA,
      NONA, NONA, NONA, NONA, NONA, NONA, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00,
  };

  /*
  ** add 1 to char to allow index -1 (EOZ)
  */
  public static int testprop(int c, int p)
  {
    return luai_ctype_[c + 1] & p;
  }

  public static int lislalpha(int c)
  {
    return testprop(c, MASK(ALPHABIT));
  }

  public static int lislalnum(int c)
  {
    return testprop(c, MASK(ALPHABIT) | MASK(DIGITBIT));
  }

  public static int lisdigit(int c)
  {
    return testprop(c, MASK(DIGITBIT));
  }

  public static int lisspace(int c)
  {
    return testprop(c, MASK(SPACEBIT));
  }

  public static int lisprint(int c)
  {
    return testprop(c, MASK(PRINTBIT));
  }

  public static int lisxdigit(int c)
  {
    return testprop(c, MASK(XDIGITBIT));
  }

  public static int ltolower(int c)
  {
    assert ('A' <= c && c <= 'Z') || c == (c | ('A' ^ 'a'));
    return c | ('A' ^ 'a');
  }
}
