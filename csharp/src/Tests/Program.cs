using System;
using System.Threading;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Tests;

/* the test programs of the Java build's classes: 'lungolua-tests.dll [-Dname=value...] Name args' */
public static class Program
{
  public static int Main(string[] args)
  {
    string[] rest = SysProps.parse(args);
    if (rest.Length == 0)
    {
      Console.Error.WriteLine("usage: lungolua-tests [-Dname=value...] PublicApi|EngineTests|Gate|Inbox [args]");
      return 2;
    }
    string[] pargs = new string[rest.Length - 1];
    Array.Copy(rest, 1, pargs, 0, pargs.Length);
    Action<string[]> run = rest[0] switch
    {
      "PublicApi" => PublicApi.main,
      "EngineTests" => EngineTests.main,
      "Gate" => Gate.main,
      "Inbox" => Inbox.main,
      _ => null,
    };
    if (run == null)
    {
      Console.Error.WriteLine("lungolua-tests: no program " + rest[0]);
      return 2;
    }
    /* the interpreter's C stack in the C++ build: a thread of its own, with room */
    Exception failed = null;
    Thread t = new Thread(() =>
    {
      try
      {
        run(pargs);
      }
      catch (Exception e)
      {
        failed = e;
      }
    }, 256 * 1024 * 1024);
    t.Start();
    t.Join();
    if (failed != null)
    {
      Console.Error.WriteLine(failed);
      return 1;
    }
    return 0;
  }
}
