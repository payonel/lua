using System;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;

namespace Payonel.LungoLua.Platform;

/*
** The C library's locale functions as they are in the "C" locale, the
** only one the JVM port has: the character classes of ASCII, '.' as the
** radix point, and 'setlocale' that accepts "C", "POSIX" and "" (when the
** environment names one of those).
*/
public sealed class LocaleUtil
{
  private LocaleUtil()
  {
  }

  /* <clocale>'s categories (glibc's values) */
  public const int LC_CTYPE = 0;
  public const int LC_NUMERIC = 1;
  public const int LC_TIME = 2;
  public const int LC_COLLATE = 3;
  public const int LC_MONETARY = 4;
  public const int LC_MESSAGES = 5;
  public const int LC_ALL = 6;

  private static readonly ByteView C_LOCALE = ByteView.of("C");

  private static bool isupper(int c)
  {
    return c >= 'A' && c <= 'Z';
  }

  private static bool iscntrl(int c)
  {
    return (c >= 0 && c < 32) || c == 127;
  }

  private static bool isgraph(int c)
  {
    return c > 32 && c < 127;
  }

  private static bool ispunct(int c)
  {
    return isgraph(c) && !locale_isalnum(c);
  }

  public static bool locale_matchclass(int c, int cls)
  {
    switch (cls)
    {
    case 'a': return locale_isalpha(c);
    case 'c': return iscntrl(c);
    case 'd': return locale_isdigit(c);
    case 'g': return isgraph(c);
    case 'l': return locale_islower(c);
    case 'p': return ispunct(c);
    case 's': return locale_isspace(c);
    case 'u': return isupper(c);
    case 'w': return locale_isalnum(c);
    case 'x': return locale_isxdigit(c);
    case 'z': return c == 0;
    default: return false;
    }
  }

  public static int locale_tolower(int c)
  {
    return isupper(c) ? c + ('a' - 'A') : c;
  }

  public static int locale_toupper(int c)
  {
    return (locale_islower(c)) ? c - ('a' - 'A') : c;
  }

  public static bool locale_islower(int c)
  {
    return c >= 'a' && c <= 'z';
  }

  public static bool locale_isalpha(int c)
  {
    return isupper(c) || locale_islower(c);
  }

  public static bool locale_isdigit(int c)
  {
    return c >= '0' && c <= '9';
  }

  public static bool locale_isalnum(int c)
  {
    return locale_isalpha(c) || locale_isdigit(c);
  }

  public static bool locale_isxdigit(int c)
  {
    return locale_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  }

  public static bool locale_isspace(int c)
  {
    return c == ' ' || (c >= '\t' && c <= '\r');
  }

  public static sbyte locale_radixpoint()
  {
    return (sbyte)('.');
  }

  private static readonly string[] catenv = {
    "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE", "LC_MONETARY", "LC_MESSAGES"};

  private static bool available(string name)
  {
    return name.equals("C") || name.equals("POSIX");
  }

  /* (the environment the program was started with: 'getenv''s, not the
     locale the launcher may have given the JVM) */
  private static string fromenv(string var)
  {
    ByteView v = OsApi.os_getenv(ByteView.of(var));
    return (v == null || v.size() == 0) ? null : v.toString();
  }

  /* the locale "" names for 'category' (as glibc reads the environment) */
  private static bool envavailable(int category)
  {
    string all = fromenv("LC_ALL");
    if (all != null)
      return available(all);
    string lang = fromenv("LANG");
    for (int c = 0; c < catenv.Length; c++)
    {
      if (category != LC_ALL && category != c)
        continue;
      string v = fromenv(catenv[c]);
      if (v == null)
        v = lang;
      if (v != null && !available(v))
        return false;
    }
    return true;
  }

  public static ByteView locale_setlocale(int category, ByteView locale)
  {
    if (category < 0 || category > LC_ALL)
      return null;
    if (locale == null)
      return C_LOCALE;
    int z = locale.find(0);
    string name = locale.substr(0, (z == ByteView.npos) ? locale.size() : z).toString();
    if (name.isEmpty() ? !envavailable(category) : !available(name))
      return null;
    return C_LOCALE;
  }
}
