using System;
using System.Collections;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;

namespace Payonel.LungoLua.Platform;

/*
** The process environment as C's 'environ' holds it: "name=value" entries
** of bytes, in their order, read once from /proc/self/environ (where that
** cannot be read, made from Environment.GetEnvironmentVariables' strings as UTF-8).
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
public sealed class Environ
{
  private Environ()
  {
  }

  /* the variables the launcher changed, and the values they had (null: unset) */
  private static readonly string[] names;
  private static readonly string[] values;

  /* the environment, the launcher's changes undone */
  private static readonly sbyte[][] entries;

  static Environ()
  {
    ArrayList<sbyte[]> env = new ();
    sbyte[][] raw = split(read("/proc/self/environ"));
    if (raw != null)
      env.addAll(JArrays.asList(raw));
    else
      foreach (DictionaryEntry e in Environment.GetEnvironmentVariables())
        env.add((e.Key + "=" + e.Value).getBytes(JCharsets.UTF_8));
    ArrayList<string> ns = new ();
    ArrayList<string> vs = new ();
    string list = JSystem.getProperty("lua.env");
    if (list != null)
    {
      sbyte[][] cl = split(read("/proc/self/cmdline"));
      foreach (string n in list.split(","))
      {
        if (n.isEmpty() || n.indexOf('=') >= 0 || n.indexOf('\u0000') >= 0)
          continue;
        string value = JSystem.getProperty("lua.env." + n);
        sbyte[] name = n.getBytes(JCharsets.UTF_8);
        sbyte[] v = (value == null) ? null : rawoption(cl, "-Dlua.env." + n + "=", value);
        int at = -1;
        for (int j = env.size() - 1; j >= 0; j--)
          if (nameis(env.get(j), name))
            at = j;
        sbyte[] cur = (at < 0) ? null : JArrays.copyOfRange(env.get(at), name.Length + 1, env.get(at).Length);
        if (JArrays.equals(cur, v)) /* (not replaced: left as it is) */
          continue;
        if (at >= 0)
          for (int j = env.size() - 1; j >= at; j--)
            if (nameis(env.get(j), name))
              env.remove(j);
        if (v != null)
        {
          sbyte[] e = new sbyte[name.Length + 1 + v.Length];
          JSystem.arraycopy(name, 0, e, 0, name.Length);
          e[name.Length] = (sbyte)('=');
          JSystem.arraycopy(v, 0, e, name.Length + 1, v.Length);
          env.add((at < 0) ? env.size() : at, e);
        }
        ns.add(n);
        vs.add(value);
      }
    }
    names = ns.toArray(new string[0]);
    values = vs.toArray(new string[0]);
    entries = env.toArray(new sbyte[0][]);
  }

  private static sbyte[] read(string file)
  {
    try
    {
      return (sbyte[]) (object) System.IO.File.ReadAllBytes(file);
    }
    catch (Exception e)
    {
      return null;
    }
  }

  /* the '\0'-terminated strings of a /proc file */
  private static sbyte[][] split(sbyte[] b)
  {
    if (b == null)
      return null;
    ArrayList<sbyte[]> l = new ();
    int s = 0;
    for (int i = 0; i < b.Length; i++)
    {
      if (b[i] == 0)
      {
        l.add(JArrays.copyOfRange(b, s, i));
        s = i + 1;
      }
    }
    if (s < b.Length)
      l.add(JArrays.copyOfRange(b, s, b.Length));
    return l.toArray(new sbyte[0][]);
  }

  /* whether entry 'e' is "name=..." */
  private static bool nameis(sbyte[] e, sbyte[] name)
  {
    return e.Length > name.Length && e[name.Length] == '=' && JArrays.equals(e, 0, name.Length, name, 0, name.Length);
  }

  /* the bytes of the JVM option "<option><value>" as the command line has
     them (the property is their text in the JVM's charset), else 'value' as
     UTF-8 */
  private static sbyte[] rawoption(sbyte[][] cl, string option, string value)
  {
    if (cl != null)
    {
      sbyte[] o = option.getBytes(JCharsets.UTF_8);
      System.Text.Encoding jnu = JCharsets.UTF_8; /* .NET decodes its arguments as UTF-8 */
      for (int i = 1; jnu != null && i < cl.Length; i++)
      {
        sbyte[] e = cl[i];
        if (e.Length >= o.Length && JArrays.equals(e, 0, o.Length, o, 0, o.Length) &&
             JString.of(e, o.Length, e.Length - o.Length, jnu).equals(value))
          return JArrays.copyOfRange(e, o.Length, e.Length);
      }
    }
    return value.getBytes(JCharsets.UTF_8);
  }

  /* C's 'getenv' (glibc's): the value of the first entry "name=value" */
  internal static ByteView getenv(ByteView name)
  {
    int len = name.size();
    if (len == 0)
      return null;
    sbyte[] n = name.array();
    int off = name.offset();
    foreach (sbyte[] e in entries)
    {
      if (e.Length > len && e[len] == '=' && JArrays.equals(e, 0, len, n, off, off + len))
        return new ByteView(e, len + 1, e.Length - len - 1);
    }
    return null;
  }

  /* a command's environment: the variables the launcher changed, as they
     were (if none, the command inherits the JVM's environment untouched) */
  internal static void restore(System.Diagnostics.ProcessStartInfo pb)
  {
    if (names.Length == 0)
      return;
    System.Collections.Generic.IDictionary<string, string> env = pb.Environment;
    for (int i = 0; i < names.Length; i++)
    {
      if (values[i] == null)
        env.Remove(names[i]);
      else
        env[names[i]] = values[i];
    }
  }
}
