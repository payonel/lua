using System.Threading;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Host;

/*
** The program's entry: the leading '-Dname=value' arguments are the
** system properties (taken before any engine class starts, as their
** statics read them), the rest are lua.c's; it runs on a thread with a
** large stack, and Lua.main ends the process.
*/
public static class Program
{
  private const int STACK = 64 * 1024 * 1024;

  public static void Main(string[] args)
  {
    string[] rest = SysProps.parse(args);
    Thread t = new Thread(() => Lua.main(rest), STACK);
    t.Start();
    t.Join();
  }
}
