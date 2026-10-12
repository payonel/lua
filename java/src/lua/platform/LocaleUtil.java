package lua.platform;

import lua.bytes.ByteView;

/*
** The C library's locale functions as they are in the "C" locale, the
** only one the JVM port has: the character classes of ASCII, '.' as the
** radix point, and 'setlocale' that accepts "C", "POSIX" and "" (when the
** environment names one of those).
*/
public final class LocaleUtil
{
  private LocaleUtil()
  {
  }

  /* <clocale>'s categories (glibc's values) */
  public static final int LC_CTYPE = 0;
  public static final int LC_NUMERIC = 1;
  public static final int LC_TIME = 2;
  public static final int LC_COLLATE = 3;
  public static final int LC_MONETARY = 4;
  public static final int LC_MESSAGES = 5;
  public static final int LC_ALL = 6;

  private static final ByteView C_LOCALE = ByteView.of("C");

  private static int isupper(int c)
  {
    return (c >= 'A' && c <= 'Z') ? 1 : 0;
  }

  private static int iscntrl(int c)
  {
    return ((c >= 0 && c < 32) || c == 127) ? 1 : 0;
  }

  private static int isgraph(int c)
  {
    return (c > 32 && c < 127) ? 1 : 0;
  }

  private static int ispunct(int c)
  {
    return (isgraph(c) != 0 && locale_isalnum(c) == 0) ? 1 : 0;
  }

  public static int locale_matchclass(int c, int cls)
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
    case 'z': return (c == 0) ? 1 : 0;
    default: return 0;
    }
  }

  public static int locale_tolower(int c)
  {
    return (isupper(c) != 0) ? c + ('a' - 'A') : c;
  }

  public static int locale_toupper(int c)
  {
    return (locale_islower(c) != 0) ? c - ('a' - 'A') : c;
  }

  public static int locale_islower(int c)
  {
    return (c >= 'a' && c <= 'z') ? 1 : 0;
  }

  public static int locale_isalpha(int c)
  {
    return (isupper(c) != 0 || locale_islower(c) != 0) ? 1 : 0;
  }

  public static int locale_isdigit(int c)
  {
    return (c >= '0' && c <= '9') ? 1 : 0;
  }

  public static int locale_isalnum(int c)
  {
    return (locale_isalpha(c) != 0 || locale_isdigit(c) != 0) ? 1 : 0;
  }

  public static int locale_isxdigit(int c)
  {
    return (locale_isdigit(c) != 0 || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) ? 1 : 0;
  }

  public static int locale_isspace(int c)
  {
    return (c == ' ' || (c >= '\t' && c <= '\r')) ? 1 : 0;
  }

  public static byte locale_radixpoint()
  {
    return '.';
  }

  private static final String[] catenv = {
    "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE", "LC_MONETARY", "LC_MESSAGES"};

  private static boolean available(String name)
  {
    return name.equals("C") || name.equals("POSIX");
  }

  private static String fromenv(String var)
  {
    String v = System.getenv(var);
    return (v == null || v.isEmpty()) ? null : v;
  }

  /* the locale "" names for 'category' (as glibc reads the environment) */
  private static boolean envavailable(int category)
  {
    String all = fromenv("LC_ALL");
    if (all != null)
      return available(all);
    String lang = fromenv("LANG");
    for (int c = 0; c < catenv.length; c++)
    {
      if (category != LC_ALL && category != c)
        continue;
      String v = fromenv(catenv[c]);
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
    String name = locale.substr(0, (z == ByteView.npos) ? locale.size() : z).toString();
    if (name.isEmpty() ? !envavailable(category) : !available(name))
      return null;
    return C_LOCALE;
  }
}
