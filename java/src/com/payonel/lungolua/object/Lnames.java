package com.payonel.lungolua.object;

import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.function.Consumer;

import com.payonel.lungolua.api.LuaAuxLib;
import com.payonel.lungolua.api.lua_Budget;
import com.payonel.lungolua.api.lua_CClosureKind;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_KFunction;
import com.payonel.lungolua.api.lua_ObjectFactory;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.Lload;
import com.payonel.lungolua.gc.Lgc;
import com.payonel.lungolua.lib.Lbaselib;
import com.payonel.lungolua.lib.Lcorolib;
import com.payonel.lungolua.lib.Ldblib;
import com.payonel.lungolua.lib.Liolib;
import com.payonel.lungolua.lib.Lmathlib;
import com.payonel.lungolua.lib.Loadlib;
import com.payonel.lungolua.lib.Loslib;
import com.payonel.lungolua.lib.Lstrlib;
import com.payonel.lungolua.lib.Ltablib;
import com.payonel.lungolua.lib.Lutf8lib;
import com.payonel.lungolua.runtime.lua_State;

/*
** Stable names for the functions the engine keeps in its state: the
** continuations of C functions (CallInfo 'k'), the light C functions that
** are part of the engine (the libraries' and the ones the engine pushes
** itself), and budget callbacks. A name stays the same from one build to
** the next, so a tool that stores a state outside the process can say which
** function a frame or a value holds, and find it again.
*/
public final class Lnames
{
  private Lnames()
  {
  }

  /* (the functions are keys by identity: in Java, IdentityHashMaps of their single instances) */
  private static final class Names
  {
    final HashMap<String, lua_KFunction> kbyname = new HashMap<>();
    final IdentityHashMap<lua_KFunction, String> knames = new IdentityHashMap<>();
    final HashMap<String, CFuncInfo> cfibyname = new HashMap<>();
    final IdentityHashMap<CFuncInfo, String> cfinames = new IdentityHashMap<>();
    final HashMap<String, lua_Budget> budgetbyname = new HashMap<>();
    final IdentityHashMap<lua_Budget, String> budgetnames = new IdentityHashMap<>();
    final IdentityHashMap<lua_Budget, Consumer<lua_State>> budgetprepare = new IdentityHashMap<>();
    final HashMap<String, lua_ObjectFactory> objectkinds = new HashMap<>();
    final IdentityHashMap<lua_CFunction, lua_CClosureKind> cclosures = new IdentityHashMap<>();
    final HashMap<String, lua_CClosureKind> cclosurebyname = new HashMap<>();
  }

  private static final Names n = new Names(); /* (made on first use: registrations run at static initialization) */

  private static Names names()
  {
    return n;
  }

  private static String view(ByteView v)
  {
    return new String(v.array(), v.offset(), v.size(), StandardCharsets.ISO_8859_1);
  }

  private static final Class<?>[] registrars = {
    LuaAuxLib.class, Lbaselib.class, Lcorolib.class, Ldblib.class, Lgc.class, Liolib.class, Lload.class,
    Lmathlib.class, Loadlib.class, Loslib.class, Lstrlib.class, Ltablib.class, Lutf8lib.class,
  };

  private static volatile boolean registered = false;

  private static void registerall()
  {
    if (registered)
      return;
    for (Class<?> c : registrars)
    {
      try
      {
        Class.forName(c.getName(), true, c.getClassLoader());
      }
      catch (ClassNotFoundException e)
      {
        throw new IllegalStateException(e);
      }
    }
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
  public static void lua_registerk(String name, lua_KFunction k)
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
  public static String lua_kname(lua_KFunction k)
  {
    return names().knames.get(k);
  }

  public static lua_KFunction lua_kbyname(ByteView name)
  {
    registerall();
    return names().kbyname.get(view(name));
  }

  public static void lua_registerbudget(String name, lua_Budget f)
  {
    lua_registerbudget(name, f, null);
  }

  public static void lua_registerbudget(String name, lua_Budget f, Consumer<lua_State> prepare)
  {
    names().budgetbyname.put(name, f);
    names().budgetnames.put(f, name);
    if (prepare != null)
      names().budgetprepare.put(f, prepare);
  }

  /* what a budget with callback 'f' needs of a state it comes into (such as the thread its callback runs on) */
  public static void lua_preparebudget(lua_State L, lua_Budget f)
  {
    Consumer<lua_State> it = names().budgetprepare.get(f);
    if (it != null)
      it.accept(L);
  }

  public static String lua_budgetname(lua_Budget f)
  {
    return names().budgetnames.get(f);
  }

  public static lua_Budget lua_budgetbyname(ByteView name)
  {
    registerall();
    return names().budgetbyname.get(view(name));
  }

  public static void lua_registerobjectkind(String name, lua_ObjectFactory f)
  {
    names().objectkinds.put(name, f);
  }

  public static lua_ObjectFactory lua_objectkind(ByteView name)
  {
    registerall();
    return names().objectkinds.get(view(name));
  }

  public static void lua_registercclosure(lua_CClosureKind kind)
  {
    lua_CClosureKind slot = names().cclosures.computeIfAbsent(kind.f, f -> new lua_CClosureKind());
    slot.assign(kind);
    names().cclosurebyname.put(kind.name, slot);
  }

  public static lua_CClosureKind lua_cclosurekind(lua_CFunction f)
  {
    return names().cclosures.get(f);
  }

  public static lua_CClosureKind lua_cclosurekindbyname(ByteView name)
  {
    registerall();
    return names().cclosurebyname.get(view(name));
  }

  public static void registercfi(CFuncInfo cfi, String name)
  {
    names().cfibyname.put(name, cfi);
    names().cfinames.put(cfi, name);
  }

  public static String lua_cfiname(CFuncInfo cfi)
  {
    return names().cfinames.get(cfi);
  }

  public static CFuncInfo lua_cfibyname(ByteView name)
  {
    registerall();
    return names().cfibyname.get(view(name));
  }
}
