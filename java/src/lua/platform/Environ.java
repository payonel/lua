package lua.platform;

import java.io.IOException;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Map;

import lua.bytes.ByteView;

/*
** The process environment as C's 'environ' holds it: "name=value" entries
** of bytes, in their order, read once from /proc/self/environ (where that
** cannot be read, made from System.getenv's strings as UTF-8).
**
** The JVM converts paths, commands and the environment between strings
** and the system's bytes with the charset of the locale it starts in, and
** no property changes that. So that the conversion is UTF-8's, as the
** engine's is, the launchers (java/lua, java/lua-debug, java/test_*) start
** the JVM with LC_ALL=C.UTF-8 when the locale's charset is not UTF-8, and
** pass the locale's variables as they were, as properties: "lua.env" lists
** their names (separated by ','), and "lua.env.NAME" holds the value NAME
** had (no such property: NAME was unset). The program sees the environment
** it was started with: 'getenv' answers with those variables as they were,
** and a command gets back the ones the launcher changed (the others reach
** it as the JVM got them, byte for byte).
*/
final class Environ
{
  private Environ()
  {
  }

  /* the variables the launcher changed, and the values they had (null: unset) */
  private static final String[] names;
  private static final String[] values;

  /* the environment, the launcher's changes undone */
  private static final byte[][] entries;

  static
  {
    ArrayList<byte[]> env = new ArrayList<>();
    byte[][] raw = split(read("/proc/self/environ"));
    if (raw != null)
      env.addAll(Arrays.asList(raw));
    else
      for (Map.Entry<String, String> e : System.getenv().entrySet())
        env.add((e.getKey() + "=" + e.getValue()).getBytes(StandardCharsets.UTF_8));
    ArrayList<String> ns = new ArrayList<>();
    ArrayList<String> vs = new ArrayList<>();
    String list = System.getProperty("lua.env");
    if (list != null)
    {
      byte[][] cl = split(read("/proc/self/cmdline"));
      for (String n : list.split(","))
      {
        if (n.isEmpty() || n.indexOf('=') >= 0 || n.indexOf('\0') >= 0)
          continue;
        String value = System.getProperty("lua.env." + n);
        byte[] name = n.getBytes(StandardCharsets.UTF_8);
        byte[] v = (value == null) ? null : rawoption(cl, "-Dlua.env." + n + "=", value);
        int at = -1;
        for (int j = env.size() - 1; j >= 0; j--)
          if (nameis(env.get(j), name))
            at = j;
        byte[] cur = (at < 0) ? null : Arrays.copyOfRange(env.get(at), name.length + 1, env.get(at).length);
        if (Arrays.equals(cur, v)) /* (not replaced: left as it is) */
          continue;
        if (at >= 0)
          for (int j = env.size() - 1; j >= at; j--)
            if (nameis(env.get(j), name))
              env.remove(j);
        if (v != null)
        {
          byte[] e = new byte[name.length + 1 + v.length];
          System.arraycopy(name, 0, e, 0, name.length);
          e[name.length] = '=';
          System.arraycopy(v, 0, e, name.length + 1, v.length);
          env.add((at < 0) ? env.size() : at, e);
        }
        ns.add(n);
        vs.add(value);
      }
    }
    names = ns.toArray(new String[0]);
    values = vs.toArray(new String[0]);
    entries = env.toArray(new byte[0][]);
  }

  private static byte[] read(String file)
  {
    try
    {
      return Files.readAllBytes(Path.of(file));
    }
    catch (IOException | RuntimeException e)
    {
      return null;
    }
  }

  /* the '\0'-terminated strings of a /proc file */
  private static byte[][] split(byte[] b)
  {
    if (b == null)
      return null;
    ArrayList<byte[]> l = new ArrayList<>();
    int s = 0;
    for (int i = 0; i < b.length; i++)
    {
      if (b[i] == 0)
      {
        l.add(Arrays.copyOfRange(b, s, i));
        s = i + 1;
      }
    }
    if (s < b.length)
      l.add(Arrays.copyOfRange(b, s, b.length));
    return l.toArray(new byte[0][]);
  }

  /* whether entry 'e' is "name=..." */
  private static boolean nameis(byte[] e, byte[] name)
  {
    return e.length > name.length && e[name.length] == '=' && Arrays.equals(e, 0, name.length, name, 0, name.length);
  }

  /* the bytes of the JVM option "<option><value>" as the command line has
     them (the property is their text in the JVM's charset), else 'value' as
     UTF-8 */
  private static byte[] rawoption(byte[][] cl, String option, String value)
  {
    if (cl != null)
    {
      byte[] o = option.getBytes(StandardCharsets.UTF_8);
      Charset jnu;
      try
      {
        jnu = Charset.forName(System.getProperty("sun.jnu.encoding"));
      }
      catch (RuntimeException e)
      {
        jnu = null;
      }
      for (int i = 1; jnu != null && i < cl.length; i++)
      {
        byte[] e = cl[i];
        if (e.length >= o.length && Arrays.equals(e, 0, o.length, o, 0, o.length) &&
            new String(e, o.length, e.length - o.length, jnu).equals(value))
          return Arrays.copyOfRange(e, o.length, e.length);
      }
    }
    return value.getBytes(StandardCharsets.UTF_8);
  }

  /* C's 'getenv' (glibc's): the value of the first entry "name=value" */
  static ByteView getenv(ByteView name)
  {
    int len = name.size();
    if (len == 0)
      return null;
    byte[] n = name.array();
    int off = name.offset();
    for (byte[] e : entries)
    {
      if (e.length > len && e[len] == '=' && Arrays.equals(e, 0, len, n, off, off + len))
        return new ByteView(e, len + 1, e.length - len - 1);
    }
    return null;
  }

  /* a command's environment: the variables the launcher changed, as they
     were (if none, the command inherits the JVM's environment untouched) */
  static void restore(ProcessBuilder pb)
  {
    if (names.length == 0)
      return;
    Map<String, String> env = pb.environment();
    for (int i = 0; i < names.length; i++)
    {
      if (values[i] == null)
        env.remove(names[i]);
      else
        env.put(names[i], values[i]);
    }
  }
}
