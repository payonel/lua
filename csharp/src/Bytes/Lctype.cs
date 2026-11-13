using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Bytes;

/*
** WARNING: the functions defined here do not necessarily correspond
** to the similar functions in the standard C ctype.h. They are
** optimized for the specific needs of Lua.
*/
public sealed class Lctype
{
  private Lctype()
  {
  }

  public const int ALPHABIT = 0;
  public const int DIGITBIT = 1;
  public const int PRINTBIT = 2;
  public const int SPACEBIT = 3;
  public const int XDIGITBIT = 4;

  public static int MASK(int B)
  {
    return 1 << B;
  }

  private const sbyte NONA = 0x00; /* default */

  /* one entry for each character and for -1 (EOZ) */
  public static readonly sbyte[] luai_ctype_ = {
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
  public static bool testprop(int c, int p)
  {
    return (luai_ctype_[c + 1] & p) != 0;
  }

  public static bool lislalpha(int c)
  {
    return testprop(c, MASK(ALPHABIT));
  }

  public static bool lislalnum(int c)
  {
    return testprop(c, MASK(ALPHABIT) | MASK(DIGITBIT));
  }

  public static bool lisdigit(int c)
  {
    return testprop(c, MASK(DIGITBIT));
  }

  public static bool lisspace(int c)
  {
    return testprop(c, MASK(SPACEBIT));
  }

  public static bool lisprint(int c)
  {
    return testprop(c, MASK(PRINTBIT));
  }

  public static bool lisxdigit(int c)
  {
    return testprop(c, MASK(XDIGITBIT));
  }

  public static int ltolower(int c)
  {
    { if (JAssert.on && !( ('A' <= c && c <= 'Z') || c == (c | ('A' ^ 'a')))) throw new AssertionError(); }
    return c | ('A' ^ 'a');
  }
}
