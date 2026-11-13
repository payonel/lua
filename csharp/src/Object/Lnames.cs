using System;
using System.Runtime.CompilerServices;
using Payonel.LungoLua.Compat;
using LuaAuxLib = Payonel.LungoLua.Api.LuaAuxLib;
using lua_Budget = Payonel.LungoLua.Api.lua_Budget;
using lua_CClosureKind = Payonel.LungoLua.Api.lua_CClosureKind;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using lua_KFunction = Payonel.LungoLua.Api.lua_KFunction;
using lua_ObjectFactory = Payonel.LungoLua.Api.lua_ObjectFactory;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using Lload = Payonel.LungoLua.Bytes.Lload;
using Lgc = Payonel.LungoLua.Gc.Lgc;
using Lbaselib = Payonel.LungoLua.Lib.Lbaselib;
using Lcorolib = Payonel.LungoLua.Lib.Lcorolib;
using Ldblib = Payonel.LungoLua.Lib.Ldblib;
using Liolib = Payonel.LungoLua.Lib.Liolib;
using Lmathlib = Payonel.LungoLua.Lib.Lmathlib;
using Loadlib = Payonel.LungoLua.Lib.Loadlib;
using Loslib = Payonel.LungoLua.Lib.Loslib;
using Lstrlib = Payonel.LungoLua.Lib.Lstrlib;
using Ltablib = Payonel.LungoLua.Lib.Ltablib;
using Lutf8lib = Payonel.LungoLua.Lib.Lutf8lib;
using lua_State = Payonel.LungoLua.Runtime.lua_State;

namespace Payonel.LungoLua.Object;

/*
** Stable names for the functions the engine keeps in its state: the
** continuations of C functions (CallInfo 'k'), the light C functions that
** are part of the engine (the libraries' and the ones the engine pushes
** itself), and budget callbacks. A name stays the same from one build to
** the next, so a tool that stores a state outside the process can say which
** function a frame or a value holds, and find it again.
*/
public sealed class Lnames
{
  private Lnames()
  {
  }

  /* (the functions are keys by identity: in Java, IdentityHashMaps of their single instances) */
  private sealed class Names
  {
    internal readonly HashMap<string, lua_KFunction> kbyname = new ();
    internal readonly IdentityHashMap<lua_KFunction, string> knames = new ();
    internal readonly HashMap<string, CFuncInfo> cfibyname = new ();
    internal readonly IdentityHashMap<CFuncInfo, string> cfinames = new ();
    internal readonly HashMap<string, lua_Budget> budgetbyname = new ();
    internal readonly IdentityHashMap<lua_Budget, string> budgetnames = new ();
    internal readonly IdentityHashMap<lua_Budget, Consumer<lua_State>> budgetprepare_fn = new ();
    internal readonly HashMap<string, lua_ObjectFactory> objectkinds = new ();
    internal readonly IdentityHashMap<lua_CFunction, lua_CClosureKind> cclosures = new ();
    internal readonly HashMap<string, lua_CClosureKind> cclosurebyname = new ();
  }

  private static readonly Names n = new Names(); /* (made on first use: registrations run at static initialization) */

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static Names names()
  {
    return n;
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  private static string view(ByteView v)
  {
    return JString.of(v.array(), v.offset(), v.size(), JCharsets.ISO_8859_1);
  }

  private static readonly Type[] registrars = {
    typeof(LuaAuxLib), typeof(Lbaselib), typeof(Lcorolib), typeof(Ldblib), typeof(Lgc), typeof(Liolib), typeof(Lload),
    typeof(Lmathlib), typeof(Loadlib), typeof(Loslib), typeof(Lstrlib), typeof(Ltablib), typeof(Lutf8lib),
  };

  private static volatile bool registered = false;

  private static void registerall()
  {
    if (registered)
      return;
    foreach (Type c in registrars)
      System.Runtime.CompilerServices.RuntimeHelpers.RunClassConstructor(c.TypeHandle);
    registered = true;
  }

  /*
  ** Names for the host's continuations and budget callbacks. A tool that keeps
  ** a state outside the process (a frozen thread has continuations in its
  ** frames and a callback in its budget group) cannot keep an address: it
  ** keeps the name, and finds the function by it where it restores. The
  ** engine's own functions are named already; a name is stable from one run
  ** to the next, and not the same as another's.
  */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static void lua_registerk(string name, lua_KFunction k)
  {
    names().kbyname.put(name, k);
    names().knames.put(k, name);
  }

  /*
  ** Names of the functions the engine keeps in its state, stable from one
  ** build to the next (see lnames.cpp): continuations (a static 'KNamer' names one),
  ** the engine's light C functions, and budget callbacks. The lookups answer
  ** nullptr for what has no name.
  */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static string lua_kname(lua_KFunction k)
  {
    return names().knames.get(k);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static lua_KFunction lua_kbyname(ByteView name)
  {
    registerall();
    return names().kbyname.get(view(name));
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static void lua_registerbudget(string name, lua_Budget f)
  {
    lua_registerbudget(name, f, null);
  }

  public static void lua_registerbudget(string name, lua_Budget f, Consumer<lua_State> prepare)
  {
    names().budgetbyname.put(name, f);
    names().budgetnames.put(f, name);
    if (prepare != null)
      names().budgetprepare_fn.put(f, prepare);
  }

  /* what a budget with callback 'f' needs of a state it comes into (such as the thread its callback runs on) */
  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static void lua_preparebudget(lua_State L, lua_Budget f)
  {
    Consumer<lua_State> it = names().budgetprepare_fn.get(f);
    if (it != null)
      it.accept(L);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static string lua_budgetname(lua_Budget f)
  {
    return names().budgetnames.get(f);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static lua_Budget lua_budgetbyname(ByteView name)
  {
    registerall();
    return names().budgetbyname.get(view(name));
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static void lua_registerobjectkind(string name, lua_ObjectFactory f)
  {
    names().objectkinds.put(name, f);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static lua_ObjectFactory lua_objectkind(ByteView name)
  {
    registerall();
    return names().objectkinds.get(view(name));
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static void lua_registercclosure(lua_CClosureKind kind)
  {
    lua_CClosureKind slot = names().cclosures.computeIfAbsent(kind.f, f => new lua_CClosureKind());
    slot.assign(kind);
    names().cclosurebyname.put(kind.name, slot);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static lua_CClosureKind lua_cclosurekind(lua_CFunction f)
  {
    return names().cclosures.get(f);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static lua_CClosureKind lua_cclosurekindbyname(ByteView name)
  {
    registerall();
    return names().cclosurebyname.get(view(name));
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static void registercfi(CFuncInfo cfi, string name)
  {
    names().cfibyname.put(name, cfi);
    names().cfinames.put(cfi, name);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static string lua_cfiname(CFuncInfo cfi)
  {
    return names().cfinames.get(cfi);
  }

  [MethodImpl(MethodImplOptions.AggressiveInlining)]
  public static CFuncInfo lua_cfibyname(ByteView name)
  {
    registerall();
    return names().cfibyname.get(view(name));
  }
}
