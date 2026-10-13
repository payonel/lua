/*
** Styx test host: C++ tests plus a runner for the Lua scripts in this
** directory. Run from tests/styx (part of `make test`).
**
** Each script runs once per GC mode, in a fresh state, with the shared
** harness (support/harness.lua) on the package path. Debug builds also open
** the test library, so the harness checks the heap invariants.
*/

package com.payonel.lungolua.tests;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.api.LuaConf.*;
import static com.payonel.lungolua.lib.Linit.*;
import static com.payonel.lungolua.lib.Lstyx.*;
import static com.payonel.lungolua.object.Lnames.*;
import static com.payonel.lungolua.platform.OsApi.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lhack.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.tests.Ltests.luaB_opentests;
import static com.payonel.lungolua.vm.Ldebug.*;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.function.Consumer;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.lua_Alloc;
import com.payonel.lungolua.api.lua_Budget;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_Debug;
import com.payonel.lungolua.api.lua_Hook;
import com.payonel.lungolua.api.lua_KFunction;
import com.payonel.lungolua.api.lua_ObjectFactory;
import com.payonel.lungolua.api.lua_ResumeResult;
import com.payonel.lungolua.api.lua_UserObject;
import com.payonel.lungolua.api.lua_Writer;
import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.lib.Lstyx;
import com.payonel.lungolua.lib.luaL_Restored;
import com.payonel.lungolua.lib.luaL_RestoreSetup;
import com.payonel.lungolua.lib.luaL_StyxResult;
import com.payonel.lungolua.platform.OsFile;
import com.payonel.lungolua.runtime.lua_State;

public final class StyxTests
{
  private StyxTests()
  {
  }

  /*
  ** (The C++'s std::string and const char* are byte strings: here a String
  ** of one char per byte. 'view' and 'str' cross to and from the engine's
  ** bytes, 'printf' writes the bytes to the engine's stdout.)
  */
  private static ByteView view(CharSequence s)
  {
    return new ByteView(s.toString().getBytes(StandardCharsets.ISO_8859_1));
  }

  private static String str(ByteView b)
  {
    return new String(b.array(), b.offset(), b.size(), StandardCharsets.ISO_8859_1);
  }

  private static void printf(String fmt, Object... args)
  {
    os_print_stdout(view(String.format(Locale.ROOT, fmt, args)));
  }

  /* (a HostString given back through a pointer: 'HostString* msg'; to_os() is its bytes) */
  private static final class HostStringOut
  {
    HostString s = new HostString();

    String to_os()
    {
      return str(s.to_utf8());
    }
  }

  /* the Styx calls as these tests use them: the status, and the message in '*msg' */
  private static int styx_status(luaL_StyxResult r, HostStringOut msg)
  {
    msg.s = r.msg;
    return r.status;
  }

  private static int luaL_persiststate(lua_State L, int perms, lua_Writer w, HostStringOut msg)
  {
    return styx_status(Lstyx.luaL_persiststate(L, perms, w), msg);
  }

  private static int lua_styxpersistvalue(lua_State L, int perms, int idx, lua_Writer w, HostStringOut msg)
  {
    return styx_status(Lstyx.lua_styxpersistvalue(L, perms, idx, w), msg);
  }

  private static int lua_styxunpersistvalue(lua_State L, int perms, ByteView data, HostStringOut msg)
  {
    return styx_status(Lstyx.lua_styxunpersistvalue(L, perms, data), msg);
  }

  /* a setup made of a plain function */
  private static final class FnSetup implements luaL_RestoreSetup
  {
    Consumer<lua_State> f;

    FnSetup(Consumer<lua_State> fn)
    {
      f = fn;
    }

    @Override
    public void setup(lua_State L)
    {
      f.accept(L);
    }
  }

  /* a restore as these tests use it: the thread, or NULL and the message in '*msg' */
  private static lua_State restorestate(lua_Alloc a, ByteView data, Consumer<lua_State> setup, HostStringOut msg)
  {
    FnSetup fs = new FnSetup(setup);
    luaL_Restored rd = luaL_restorestate(a, data, fs);
    msg.s = rd.msg;
    return rd.L;
  }

  private static int g_checks = 0;
  private static int g_failures = 0;
  private static int g_test_failures = 0;
  private static String g_current = "";

  private static void check_impl(boolean ok, String expr, int line)
  {
    g_checks++;
    if (!ok)
    {
      g_failures++;
      g_test_failures++;
      printf("      FAIL [%s] line %d: %s\n", g_current, line, expr);
    }
  }

  /* (#define CHECK(cond) check_impl((cond), #cond, __LINE__): the line is the
     caller's, and the text of 'cond' is read from the source, when at hand) */
  private static void CHECK(boolean cond)
  {
    int line = cond ? 0 : StackWalker.getInstance().walk(s -> s.skip(1).findFirst()).get().getLineNumber();
    check_impl(cond, cond ? "" : source_text(line), line);
  }

  private static String source_text(int line)
  {
    try
    {
      List<String> lines = Files.readAllLines(Path.of(System.getProperty("styx.source", "")));
      StringBuilder t = new StringBuilder();
      int depth = 0;
      char quote = 0;
      for (int i = line - 1; i < lines.size(); i++)
      {
        String s = lines.get(i);
        int j;
        if (depth == 0)
        {
          j = s.indexOf("CHECK(");
          if (j < 0)
            return "?";
          j += 6;
          depth = 1;
        }
        else
        {
          s = s.strip();
          j = 0;
          t.append(' ');
        }
        for (; j < s.length(); j++)
        {
          char c = s.charAt(j);
          if (quote != 0)
          {
            if (c == '\\' && j + 1 < s.length())
            {
              t.append(c);
              c = s.charAt(++j);
            }
            else if (c == quote)
              quote = 0;
          }
          else if (c == '"' || c == '\'')
            quote = c;
          else if (c == '(')
            depth++;
          else if (c == ')' && --depth == 0)
            return t.toString();
          t.append(c);
        }
      }
    }
    catch (IOException | RuntimeException e)
    {
    }
    return "?";
  }

  /* the module of the test scripts (Styx's value round trip: the engine's
     libraries have none), requirable as "styx" */
  private static void preload_styx(lua_State L)
  {
    lua_getfield(L, LUA_REGISTRYINDEX, new HostString("_PRELOAD"));
    lua_pushcfunction(L, lua_styxtestmodule);
    lua_setfield(L, -2, new HostString("styx"));
    lua_pop(L, 1);
  }

  private static lua_State new_state()
  {
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    preload_styx(L);
    if (LUA_DEBUG)
    {
      luaL_requiref(L, new HostString("T"), luaB_opentests, 1);
      lua_pop(L, 1);
    }
    return L;
  }

  private static String to_string(lua_State L, int idx)
  {
    ByteView s = lua_tolstring(L, idx);
    return s != null ? str(s) : "";
  }

  private static void push_string(lua_State L, String s)
  {
    lua_pushlstring(L, view(s), 0, s.length());
  }

  /* pushes the module, as a script's `require "styx"` would */
  private static boolean push_styx(lua_State L)
  {
    lua_getglobal(L, new HostString("require"));
    lua_pushstring(L, new HostString("styx"));
    return lua_pcall(L, 1, 1, 0) == LUA_OK;
  }

  /* [args...] -> [results...] (or [error]); returns the lua_pcall status */
  private static int call_styx(lua_State L, String fn, int nargs, int nresults)
  {
    if (!push_styx(L))
      return LUA_ERRRUN;
    lua_getfield(L, -1, new HostString(fn));
    lua_remove(L, -2);
    lua_insert(L, -(nargs + 1));
    return lua_pcall(L, nargs, nresults, 0);
  }

  /* the value on top survives persist/unpersist; the stack is restored */
  private static boolean roundtrips(lua_State L)
  {
    int base = lua_gettop(L) - 1;
    lua_pushvalue(L, -1);
    boolean ok = call_styx(L, "persist", 1, 1) == LUA_OK &&
                 call_styx(L, "unpersist", 1, 1) == LUA_OK &&
                 lua_type(L, -1) == lua_type(L, -2) &&
                 lua_isinteger(L, -1) == lua_isinteger(L, -2) &&
                 lua_rawequal(L, -1, -2) != 0;
    lua_settop(L, base);
    return ok;
  }

  /* runs a chunk; returns its first result as a string ("" on error) */
  private static String eval(lua_State L, String code)
  {
    int top = lua_gettop(L);
    String r = "";
    if (luaL_loadstring(L, new HostString(code)) == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK)
      r = to_string(L, -1);
    else
      printf("      error: %s\n", to_string(L, -1));
    lua_settop(L, top);
    return r;
  }

  /* a script cannot persist: the engine's libraries have no "styx" (the
     module of the test scripts is the tests' own) */
  private static void test_no_script_library()
  {
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    CHECK(eval(L, "return tostring(pcall(require, 'styx'))").equals("false"));
    CHECK(lua_getglobal(L, new HostString("styx")) == LUA_TNIL);
    lua_close(L);
    L = new_state();
    CHECK(push_styx(L));
    CHECK(lua_type(L, -1) == LUA_TTABLE);
    lua_close(L);
  }

  private static void test_api_surface()
  {
    lua_State L = new_state();
    CHECK(push_styx(L));
    CHECK(lua_getfield(L, -1, new HostString("persist")) == LUA_TFUNCTION);
    lua_pop(L, 1);
    CHECK(lua_getfield(L, -1, new HostString("unpersist")) == LUA_TFUNCTION);
    lua_pop(L, 1);
    CHECK(lua_getfield(L, -1, new HostString("settings")) == LUA_TNIL);
    lua_close(L);
  }

  private static void test_primitives_roundtrip()
  {
    lua_State L = new_state();
    lua_pushnil(L);
    CHECK(roundtrips(L));
    lua_pushboolean(L, 1);
    CHECK(roundtrips(L));
    lua_pushboolean(L, 0);
    CHECK(roundtrips(L));
    lua_pushinteger(L, 42);
    CHECK(roundtrips(L));
    lua_pushinteger(L, LUA_MININTEGER);
    CHECK(roundtrips(L));
    lua_pushnumber(L, 3.5);
    CHECK(roundtrips(L));
    CHECK(lua_gettop(L) == 0);
    lua_close(L);
  }

  /* strings with every byte value, from C++ */
  private static void test_string_bytes()
  {
    lua_State L = new_state();
    String all = "";
    for (int i = 0; i < 256; i++)
      all += (char) i;
    push_string(L, all);
    CHECK(call_styx(L, "persist", 1, 1) == LUA_OK);
    CHECK(call_styx(L, "unpersist", 1, 1) == LUA_OK);
    CHECK(to_string(L, -1).equals(all));
    lua_close(L);
  }

  /* persist and unpersist leave the caller's stack as it was, also on error */
  private static void test_stack_balance()
  {
    lua_State L = new_state();
    lua_pushinteger(L, 1);
    lua_pushinteger(L, 2);
    lua_newtable(L);
    CHECK(call_styx(L, "persist", 1, 1) == LUA_OK);
    CHECK(lua_gettop(L) == 3 && lua_type(L, -1) == LUA_TSTRING);
    CHECK(call_styx(L, "unpersist", 1, 1) == LUA_OK);
    CHECK(lua_gettop(L) == 3 && lua_type(L, -1) == LUA_TTABLE);
    lua_pop(L, 1);
    lua_getglobal(L, new HostString("io"));
    lua_getfield(L, -1, new HostString("stdout"));
    lua_remove(L, -2);
    CHECK(call_styx(L, "persist", 1, 1) == LUA_ERRRUN);
    CHECK(lua_gettop(L) == 3 && lua_type(L, -1) == LUA_TSTRING);
    lua_pop(L, 1);
    CHECK(lua_tointeger(L, 1) == 1 && lua_tointeger(L, 2) == 2 && lua_gettop(L) == 2);
    lua_close(L);
  }

  /* moves a blob between two states: persist in A, return the bytes */
  private static String persist_in_new_state(String code)
  {
    lua_State A = new_state();
    String blob = eval(A, code);
    lua_close(A);
    return blob;
  }

  /* restores 'blob' as global 'v' in a fresh state after running 'prelude' */
  private static lua_State restore_in_new_state(String blob, String code)
  {
    lua_State B = new_state();
    push_string(B, blob);
    lua_setglobal(B, new HostString("blob"));
    CHECK(luaL_dostring(B, new HostString(code)) == LUA_OK);
    return B;
  }

  /* persist in one state, restore and resume in a separate one */
  private static void test_thread_across_states()
  {
    String blob = persist_in_new_state(
      "co = coroutine.create(function() local p = print; coroutine.yield(); p('hi', 42) end)\n" +
      "coroutine.resume(co)\n" +
      "return require('styx').persist({[_ENV]='env', [print]='print', [coroutine.yield]='yield'}, co)");
    CHECK(!blob.isEmpty());

    lua_State B = restore_in_new_state(blob,
      "perms = {env = _ENV, print = function(...) got = table.concat({...}, ',') end, yield = coroutine.yield}\n" +
      "co = require('styx').unpersist(perms, blob)\n" +
      "ok = coroutine.resume(co)\n" +
      "status = coroutine.status(co)");
    CHECK(lua_getglobal(B, new HostString("ok")) == LUA_TBOOLEAN && lua_toboolean(B, -1) != 0);
    CHECK(lua_getglobal(B, new HostString("got")) == LUA_TSTRING && ByteView.of("hi,42").equals(lua_tolstring(B, -1)));
    CHECK(lua_getglobal(B, new HostString("status")) == LUA_TSTRING && ByteView.of("dead").equals(lua_tolstring(B, -1)));
    lua_close(B);
  }

  /* a table graph with cycles and shared parts, persisted in one state */
  private static void test_tables_across_states()
  {
    String blob = persist_in_new_state(
      "local shared = {'s'}\n" +
      "local t = {1, 2, 3, name = 'n', a = shared, b = shared, [2.5] = true}\n" +
      "t.self = t\n" +
      "setmetatable(t, {__index = {extra = 'e'}})\n" +
      "return require('styx').persist(t)");
    CHECK(!blob.isEmpty());
    lua_State B = restore_in_new_state(blob, "t = require('styx').unpersist(blob)");
    CHECK(eval(B, "return tostring(t.self == t and t.a == t.b and t.a[1] == 's')").equals("true"));
    CHECK(eval(B, "return #t .. t.name .. tostring(t[2.5]) .. t.extra").equals("3ntruee"));
    lua_close(B);
  }

  /* closures sharing an upvalue, persisted in one state, run in another */
  private static void test_closures_across_states()
  {
    String blob = persist_in_new_state(
      "local function counter()\n" +
      "  local n = 10\n" +
      "  return {inc = function(k) n = n + k; return n end, get = function() return n end}\n" +
      "end\n" +
      "local c = counter(); c.inc(5)\n" +
      "return require('styx').persist(c)");
    lua_State B = restore_in_new_state(blob, "c = require('styx').unpersist(blob)");
    CHECK(eval(B, "return c.get()").equals("15"));
    CHECK(eval(B, "c.inc(1); return c.get()").equals("16"));
    lua_close(B);
  }

  /* every proper prefix of a stream is an error, and the state stays usable */
  private static void test_truncation_from_cpp()
  {
    lua_State L = new_state();
    String blob = eval(L,
      "local co = coroutine.create(function(a) local t = {a, 'x'}; coroutine.yield(t); return a end)\n" +
      "coroutine.resume(co, 7)\n" +
      "back = {_ENV, coroutine.yield}\n" +
      "return require('styx').persist({[_ENV] = 1, [coroutine.yield] = 2}, {co, {1, 2, {3}}, 'str'})");
    CHECK(!blob.isEmpty());
    int rejected = 0;
    for (int n = 0; n < blob.length(); n++)
    {
      lua_getglobal(L, new HostString("back"));
      push_string(L, blob.substring(0, n));
      if (call_styx(L, "unpersist", 2, 1) != LUA_OK)
        rejected++;
      lua_pop(L, 1);
    }
    CHECK(rejected == blob.length());
    CHECK(lua_gettop(L) == 0);
    CHECK(eval(L, "collectgarbage(); return 'alive'").equals("alive"));
    lua_close(L);
  }

  /* light userdata are restored through the permanents table */
  private static final Object here = new Object(), there = new Object();

  private static void test_light_userdata()
  {
    lua_State L = new_state();
    lua_pushlightuserdata(L, here);
    CHECK(call_styx(L, "persist", 1, 1) == LUA_ERRRUN);
    CHECK(to_string(L, -1).contains("persisting a userdata value is not supported yet"));
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushlightuserdata(L, here);
    lua_pushstring(L, new HostString("here"));
    lua_rawset(L, -3);
    lua_pushlightuserdata(L, here);
    CHECK(call_styx(L, "persist", 2, 1) == LUA_OK);
    lua_newtable(L);
    lua_pushlightuserdata(L, there);
    lua_setfield(L, -2, new HostString("here"));
    lua_insert(L, -2);
    CHECK(call_styx(L, "unpersist", 2, 1) == LUA_OK);
    CHECK(lua_touserdata(L, -1) == there);
    lua_close(L);
  }

  /*
  ** Host side persist (lua_styxpersistvalue, luaL_persiststate): called by the
  ** host on a state that is not running; it allocates nothing in the state.
  */

  private static final class StringWriter implements lua_Writer
  {
    StringBuilder out = new StringBuilder();
    int stop_after = -1; /* calls before reporting failure (-1: never) */

    @Override
    public int write(lua_State L, ByteView b)
    {
      if (stop_after == 0)
        return 1;
      if (stop_after > 0)
        stop_after--;
      if (b != null)
        out.append(str(b));
      return 0;
    }
  }

  /* counts requests to grow memory and can deny them (frees always pass) */
  private static final class CountingAlloc implements lua_Alloc
  {
    long grows = 0;
    boolean deny = false;
    long cap = 0;     /* when non zero, deny growth past this many bytes */
    long failnth = 0; /* when non zero, deny every growth from this number on (counting from grows = 0) */
    long used = 0;

    @Override
    public boolean permit(long osize, long nsize, int tag)
    {
      if (nsize > osize)
      {
        grows++;
        if (deny || (cap != 0 && used + (nsize - osize) > cap) || (failnth != 0 && grows >= failnth))
          return false;
      }
      used = used + nsize - osize;
      return true;
    }
  }

  private static lua_State new_counted_state(CountingAlloc a)
  {
    lua_State L = lua_newstate(a, 0);
    luaL_openlibs(L);
    preload_styx(L);
    return L;
  }

  /* runs 'code', which returns a value and its permanents table */
  private static boolean push_value_and_perms(lua_State L, String code)
  {
    if (luaL_loadstring(L, new HostString(code)) != LUA_OK || lua_pcall(L, 0, 2, 0) != LUA_OK)
    {
      printf("      error: %s\n", to_string(L, -1));
      lua_settop(L, 0);
      return false;
    }
    return true;
  }

  /* value at -2 and perms at -1: the script path's bytes */
  private static String script_persist(lua_State L)
  {
    lua_pushvalue(L, -1);
    lua_pushvalue(L, -3);
    if (call_styx(L, "persist", 2, 1) != LUA_OK)
    {
      printf("      script persist error: %s\n", to_string(L, -1));
      lua_pop(L, 1);
      return "";
    }
    String r = to_string(L, -1);
    lua_pop(L, 1);
    return r;
  }

  /* sets _PERMS: every C function and userdata reachable from _G, the
     registry and the string metatable, named by its path */
  /* sets _PERMS: every C function and userdata reachable from _G, the
     registry and the string metatable, and the metatable of each such
     userdata (see "Host API" in docs/styx.md), named by its path; keys are
     visited in sorted order, so two states with the same libraries agree on
     names; _BACK is the inverse */
  private static final String build_perms =
    "local perms = {} local seen = {} " +
    "local function upv(f) local _, x = debug.getupvalue(f, 1) return x end " +
    "local function walk(t, name) if seen[t] then return end seen[t] = true " +
    "local keys = {} for k in pairs(t) do keys[#keys + 1] = k end " +
    "table.sort(keys, function(a, b) return tostring(a) < tostring(b) end) " +
    "for _, k in ipairs(keys) do local v = t[k] local n = name .. '.' .. tostring(k) " +
    "if (type(v) == 'function' and debug.getinfo(v, 'S').what == 'C' and type(upv(v)) ~= 'table') or type(v) == 'userdata' then " +
    "if perms[v] == nil then perms[v] = n end " +
    "local mt = type(v) == 'userdata' and debug.getmetatable(v) " +
    "if mt and perms[mt] == nil then perms[mt] = n .. '<mt>' end " +
    "elseif type(v) == 'table' then walk(v, n) end end " +
    "local mt = getmetatable(t) if mt then walk(mt, name .. '<mt>') end end " +
    "walk(getmetatable(''), 'smt') walk(_G, '_G') walk(debug.getregistry(), 'R') " +
    "_PERMS = perms _BACK = {} for v, n in pairs(perms) do _BACK[n] = v end return 'ok'";

  private static final String[] graph_samples = {
    "return {1, 2.5, -0.0, true, false, 'str', ('long'):rep(40)}, {}",
    "local t = {a = 'x', [10] = 3.5, nested = {deep = {true}}} t.self = t " +
    "return setmetatable(t, {__index = {k = 1}}), {}",
    "local t = {1, nil, 3} t[7] = 7 local k = {} t[k] = k return t, {}",
    "local function counter() local n = 0 " +
    "return function() n = n + 1 return n end, function() return n end end " +
    "local inc, get = counter() inc() return {inc, get, counter()}, {}",
    "local co = coroutine.create(function(a) local n = a " +
    "coroutine.yield(function() n = n + 1 return n end) return n end) " +
    "local _, f = coroutine.resume(co, 5) " +
    "return {co, f}, {[_ENV] = 'env', [coroutine.yield] = 'yield'}",
    "local C = {__close = function() end} " +
    "local co = coroutine.create(function() local x <close> = setmetatable({}, C) " +
    "coroutine.yield() end) coroutine.resume(co) " +
    "return co, {[_ENV] = 'env', [coroutine.yield] = 'yield'}",
    "local function mk(i) return function() return i end end " +
    "local t = {} for i = 1, 5 do t[i] = mk(i) end return t, {}",
    "return {print, string.format, io.stdout}, " +
    "{[print] = 'print', [string.format] = 'fmt', [io.stdout] = 1}",
    "return {setmetatable({1}, {__persist = true}), 'x'}, {}",
    "local co = coroutine.create(function() local function f() error('boom') end f() end) " +
    "coroutine.resume(co) return co, {[error] = 'error', [_ENV] = 'env'}",
    "local function counter() local n = 0 " +
    "return function() n = n + 1 return n end, function() return n end end " +
    "local inc, get = counter() return {inc, get}, {[debug.upvalueid(inc, 1)] = 'count'}",
    "local log = {} local co = coroutine.create(function() for i = 1, 3 do coroutine.yield(i) end end) " +
    "debug.sethook(co, function(e, l) log[#log + 1] = l end, 'l', 4) coroutine.resume(co) " +
    "return co, {[_ENV] = 'env', [coroutine.yield] = 'yield', [log] = 'log'}",
    "local t = {} for i = 1, 50 do t['k' .. i] = i end " +
    "for i = 1, 50, 2 do t['k' .. i] = nil end return t, {}",
    "local mt = {__gc = function() end} local t = {} " +
    "for i = 1, 4 do t[i] = setmetatable({i}, mt) end t[5] = {t[3], t[1]} return t, {}",
    "local t = {} for i = 1, 30 do t['d' .. i] = i end local gone = {} " +
    "for i = 1, 30, 3 do t['d' .. i] = nil gone[#gone + 1] = 'd' .. i end " +
    "collectgarbage() collectgarbage() return {t, gone}, {}",
    "local t, gone = {}, {} for i = 1, 40 do t[(i % 2 == 0) and {} or function() end] = i end " +
    "for k, v in pairs(t) do if v % 3 == 0 then t[k] = nil gone[#gone + 1] = k end end " +
    "collectgarbage() collectgarbage() return {t, gone}, {}",
  };

  /* host persist of a value is byte for byte the script path's stream */
  private static void test_host_value_matches_script()
  {
    lua_State L = new_state();
    for (String code : graph_samples)
    {
      if (!push_value_and_perms(L, code))
      {
        CHECK(false);
        continue;
      }
      String script = script_persist(L);
      StringWriter w = new StringWriter();
      HostStringOut msg = new HostStringOut();
      int st = lua_styxpersistvalue(L, lua_gettop(L), lua_gettop(L) - 1, w, msg);
      if (st != LUA_OK)
        printf("      host persist error: %s\n", msg.to_os());
      CHECK(st == LUA_OK);
      CHECK(!script.isEmpty() && w.out.toString().equals(script));
      lua_settop(L, 0);
    }
    lua_close(L);
  }

  /* host persist asks for no memory at all, even when nothing is permitted */
  private static void test_host_persist_allocates_nothing()
  {
    CountingAlloc a = new CountingAlloc();
    lua_State L = new_counted_state(a);
    CHECK(eval(L, "local mt = {__gc = function() end} marked = {} " +
                  "for i = 1, 5 do marked[i] = setmetatable({}, mt) end " +
                  "dk = {} for i = 1, 20 do dk['k' .. i] = i end dkeys = {} " +
                  "for i = 1, 20, 2 do dk['k' .. i] = nil dkeys[#dkeys + 1] = 'k' .. i end " +
                  "collectgarbage() collectgarbage() " +
                  "for i = 1, 3 do setmetatable({}, mt) end return 'ok'").equals("ok"));
    CHECK(eval(L, build_perms).equals("ok"));
    CHECK(push_value_and_perms(L, "return {marked, dk, dkeys}, {}"));
    lua_getglobal(L, new HostString("_PERMS"));
    a.grows = 0;
    a.deny = true;
    StringWriter w1 = new StringWriter(), w2 = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(lua_styxpersistvalue(L, 2, 1, w1, msg) == LUA_OK);
    CHECK(luaL_persiststate(L, 3, w2, msg) == LUA_OK);
    a.deny = false;
    CHECK(a.grows == 0);
    CHECK(w1.out.length() != 0 && w2.out.length() != 0);
    lua_close(L);
  }

  /* a state whose scripts have run out of memory can still be persisted */
  private static void test_host_persist_at_memory_cap()
  {
    CountingAlloc a = new CountingAlloc();
    lua_State L = new_counted_state(a);
    CHECK(eval(L, build_perms).equals("ok"));
    lua_getglobal(L, new HostString("_PERMS"));  /* the permanents are ready before the cap */
    a.cap = a.used + 256 * 1024;
    CHECK(eval(L,
      "hog = {} " +
      "local ok, err = pcall(function() for i = 1, 1e9 do hog[i] = ('x'):rep(100) .. i end end) " +
      "return tostring(err)").contains("not enough memory"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    a.grows = 0;
    int st = luaL_persiststate(L, 1, w, msg);
    if (st != LUA_OK)
      printf("      host persist error: %s\n", msg.to_os());
    CHECK(st == LUA_OK);
    CHECK(a.grows == 0);
    CHECK(w.out.length() > 100 * 1000);
    lua_close(L);
  }

  /* no Lua code runs: a __persist function is not called */
  private static void test_host_persist_runs_no_lua()
  {
    lua_State L = new_state();
    CHECK(push_value_and_perms(L,
      "calls = 0 " +
      "local t = setmetatable({}, {__persist = function() calls = calls + 1 " +
      "return function() return {} end end}) return {inner = t}, {}"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(lua_styxpersistvalue(L, 2, 1, w, msg) == LUA_ERRRUN);
    CHECK(msg.to_os().contains("__persist must be a boolean (at root.inner)"));
    CHECK(eval(L, "return tostring(calls)").equals("0"));
    lua_close(L);
  }

  /* errors carry the path, and the stack is left as it was */
  private static void test_host_persist_errors()
  {
    lua_State L = new_state();
    CHECK(push_value_and_perms(L, "return {a = {file = io.stdout}}, {}"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(lua_styxpersistvalue(L, 2, 1, w, msg) == LUA_ERRRUN);
    CHECK(msg.to_os().equals("persisting a userdata value is not supported yet (at root.a.file)"));
    CHECK(lua_gettop(L) == 2);
    lua_close(L);
  }

  /* no depth limit: the walk keeps its own stack */
  private static void test_host_persist_deep()
  {
    lua_State L = new_state();
    CHECK(push_value_and_perms(L,
      "local t = {} local cur = t for i = 1, 200000 do cur.n = {} cur = cur.n end " +
      "cur.last = true return t, {}"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(lua_styxpersistvalue(L, 2, 1, w, msg) == LUA_OK);
    CHECK(w.out.length() > 200000);
    lua_settop(L, 0);
    CHECK(lua_styxunpersistvalue(L, 0, view(w.out), msg) == LUA_OK);
    lua_setglobal(L, new HostString("r"));
    CHECK(eval(L, "local n, cur = 0, r while cur.n do n = n + 1 cur = cur.n end " +
                  "return tostring(n == 200000 and cur.last)").equals("true"));
    lua_close(L);
  }

  /* a writer that fails stops the persist */
  private static void test_host_persist_writer_stops()
  {
    lua_State L = new_state();
    CHECK(push_value_and_perms(L,
      "local t = {} for i = 1, 100000 do t[i] = i end return t, {}"));
    StringWriter w = new StringWriter();
    w.stop_after = 1;
    HostStringOut msg = new HostStringOut();
    CHECK(lua_styxpersistvalue(L, 2, 1, w, msg) == LUA_ERRRUN);
    CHECK(msg.to_os().contains("writer"));
    lua_close(L);
  }

  private static int call_host_persist(lua_State L)
  {
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    lua_pushinteger(L, lua_styxpersistvalue(L, 0, 1, w, msg));
    lua_pushstring(L, msg.s);
    return 2;
  }

  private static final lua_CFunction call_host_persist = StyxTests::call_host_persist;

  /* a state that is running Lua code is refused */
  private static void test_host_persist_running_state()
  {
    lua_State L = new_state();
    lua_pushcfunction(L, call_host_persist);
    lua_setglobal(L, new HostString("host_persist"));
    CHECK(eval(L, "local st, msg = host_persist({}) return st .. ' ' .. msg")
          .contains("the state is running"));
    lua_close(L);
  }

  /* whole-state streams are deterministic and use their own root tag */
  private static void test_host_persist_state_stream()
  {
    lua_State L = new_state();
    CHECK(eval(L, "data = {list = {1, 2, 3}, name = 'state'} return 'ok'").equals("ok"));
    StringWriter w1 = new StringWriter(), w2 = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(L, 0, w1, msg) == LUA_ERRRUN);  /* C functions need perms */
    CHECK(msg.to_os().contains("cannot persist a C function"));
    CHECK(eval(L, build_perms).equals("ok"));
    lua_settop(L, 0);
    lua_getglobal(L, new HostString("_PERMS"));
    w1.out.setLength(0);
    int st = luaL_persiststate(L, 1, w1, msg);
    if (st != LUA_OK)
      printf("      host persist error: %s\n", msg.to_os());
    CHECK(st == LUA_OK);
    CHECK(luaL_persiststate(L, 1, w2, msg) == LUA_OK);
    CHECK(w1.out.length() != 0 && w1.out.toString().equals(w2.out.toString()));
    CHECK(w1.out.toString().startsWith("Styx"));
    lua_close(L);
  }

  /*
  ** The host persists one child coroutine (not the state): what it captured
  ** from outside (a table, a callback, a userdata) is in the permanents, so
  ** a second state restores it connected to its own objects.
  */
  private static void test_host_persists_child_thread()
  {
    lua_State A = new_state();
    CHECK(push_value_and_perms(A,
      "local config = {prefix = 'a:'} " +
      "local callback = function(s) end " +
      "local co = coroutine.create(function() " +
      "  local out = io.stdout " +
      "  while true do local m = coroutine.yield() callback(config.prefix .. m, out) end " +
      "end) " +
      "coroutine.resume(co) " +
      "return co, {[_ENV] = 'env', [coroutine.yield] = 'yield', [config] = 'config', " +
      "            [callback] = 'callback', [io.stdout] = 'out'}"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    int st = lua_styxpersistvalue(A, 2, 1, w, msg);
    if (st != LUA_OK)
      printf("      host persist error: %s\n", msg.to_os());
    CHECK(st == LUA_OK);
    lua_close(A);

    lua_State B = restore_in_new_state(w.out.toString(),
      "got = {} " +
      "local cfg = {prefix = 'b:'} " +
      "local cb = function(s, out) got[#got + 1] = s; got.out = out end " +
      "local back = {env = _ENV, yield = coroutine.yield, config = cfg, callback = cb, out = io.stderr} " +
      "local co = require('styx').unpersist(back, blob) " +
      "coroutine.resume(co, 'one') cfg.prefix = 'c:' coroutine.resume(co, 'two') " +
      "ok = got[1] == 'b:one' and got[2] == 'c:two' and got.out == io.stderr");
    CHECK(eval(B, "return tostring(ok)").equals("true"));
    lua_close(B);
  }

  /*
  ** Host side restore (luaL_restorestate): the host's setup opens the
  ** libraries, makes its own functions ('before') and the permanents, and
  ** leaves the inverse permanents; 'limit' then limits the allocator for
  ** the restore itself. What is restored is charged through the allocator.
  */
  private static final class Setup implements luaL_RestoreSetup
  {
    Consumer<lua_State> before;
    Runnable limit;

    Setup()
    {
      this(null, null);
    }

    Setup(Consumer<lua_State> b, Runnable l)
    {
      before = b;
      limit = l;
    }

    @Override
    public void setup(lua_State L)
    {
      luaL_openlibs(L);
      preload_styx(L);
      if (before != null)
        before.accept(L);
      eval(L, build_perms);
      lua_getglobal(L, new HostString("_BACK"));
      if (limit != null)
        limit.run();
    }
  }

  /* the state 'blob' is restored into (NULL, with 'msg', if it fails) */
  private static lua_State restore(CountingAlloc a, String blob, Setup s, HostStringOut msg)
  {
    Setup dflt = new Setup();
    luaL_Restored rd = luaL_restorestate(a, view(blob), s != null ? s : dflt);
    if (msg != null)
      msg.s = rd.msg;
    return rd.L;
  }

  /* the state 'blob' is restored into ('s': the host's own setup); if it
     fails, says so, and gives a plain state for the test to go on with */
  private static lua_State restored_state(CountingAlloc a, String blob, Setup s)
  {
    HostStringOut msg = new HostStringOut();
    lua_State B = restore(a, blob, s, msg);
    CHECK(B != null);
    if (B != null)
      return B;
    printf("      host restore error: %s\n", msg.to_os());
    return new_counted_state(a);
  }

  private static lua_State restored_state(CountingAlloc a, String blob)
  {
    return restored_state(a, blob, null);
  }

  /* the whole state, persisted with its permanents (_PERMS) */
  private static String persist_state(lua_State A)
  {
    lua_getglobal(A, new HostString("_PERMS"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    int st = luaL_persiststate(A, lua_gettop(A), w, msg);
    if (st != LUA_OK)
      printf("      host persist error: %s\n", msg.to_os());
    CHECK(st == LUA_OK);
    lua_pop(A, 1);
    return w.out.toString();
  }

  /* the main thread of the state of 'L' */
  private static lua_State main_of(lua_State L)
  {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State M = lua_tothread(L, -1);
    lua_pop(L, 1);
    return M;
  }

  /* a running program, persisted whole by the host and restored in a new state */
  private static final String program =
    "data = {list = {1, 2, 3}, name = 'state'} " +
    "local function counter() local n = 0 " +
    "  return function() n = n + 1 return n end, function() return n end end " +
    "inc, get = counter() inc() " +
    "local C = {__close = function(v) closed = (closed or '') .. v.name end} " +
    "worker = coroutine.create(function(a) " +
    "  local total = a " +
    "  local res <close> = setmetatable({name = 'res'}, C) " +
    "  peek = function() return total end " +
    "  while true do total = total + coroutine.yield(total) end " +
    "end) " +
    "coroutine.resume(worker, 10) " +
    "getmetatable('').__index.shout = function(s) return s:upper() .. '!' end " +
    "debug.setmetatable(0, {__index = {double = function(n) return n * 2 end}}) " +
    "main_ref = coroutine.running() " +
    "return 'ok'";

  private static void test_host_state_roundtrip()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, program).equals("ok"));
    CHECK(eval(A, build_perms).equals("ok"));
    lua_getglobal(A, new HostString("_PERMS"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(A, 1, w, msg) == LUA_OK);
    lua_close(A);

    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, w.out.toString());
    CHECK(lua_gettop(B) == 0);
    CHECK(eval(B, "return data.name .. #data.list").equals("state3"));
    CHECK(eval(B, "inc() return tostring(get())").equals("2"));
    CHECK(eval(B, "local _, t = coroutine.resume(worker, 5) return tostring(t) .. ',' .. peek()").equals("15,15"));
    CHECK(eval(B, "return ('hey'):shout()").equals("HEY!"));
    CHECK(eval(B, "return tostring((21):double())").equals("42"));
    CHECK(eval(B, "return tostring(main_ref == coroutine.running() and debug.getregistry()[3] == main_ref)").equals("true"));
    CHECK(eval(B, "coroutine.close(worker) return closed").equals("res"));
    CHECK(eval(B, "return tostring(print == _BACK and true or type(print))").equals("function"));
    lua_close(B);
  }

  /* a state persisted at its memory cap is restored and runs on */
  private static void test_host_restore_of_capped_state()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, build_perms).equals("ok"));
    lua_getglobal(A, new HostString("_PERMS"));
    a1.cap = a1.used + 256 * 1024;
    CHECK(eval(A,
      "hog = {} " +
      "local ok, err = pcall(function() for i = 1, 1e9 do hog[i] = ('x'):rep(100) .. i end end) " +
      "return tostring(err)").contains("not enough memory"));
    int count_a = lua_gc(A, LUA_GCCOUNT, 0, 0, 0);
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(A, 1, w, msg) == LUA_OK);
    lua_close(A);

    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, w.out.toString());
    lua_gc(B, LUA_GCCOLLECT, 0, 0, 0);
    int count_b = lua_gc(B, LUA_GCCOUNT, 0, 0, 0);
    CHECK(count_b > count_a / 2 && count_b < count_a * 2);
    CHECK(eval(B, "return tostring(#hog > 100 and hog[1] == ('x'):rep(100) .. 1)").equals("true"));
    /* the restored program runs under the same cap again */
    a2.cap = a2.used + 64 * 1024;
    CHECK(eval(B,
      "local ok, err = pcall(function() for i = 1, 1e9 do hog[#hog + 1] = ('y'):rep(100) .. i end end) " +
      "return tostring(err)").contains("not enough memory"));
    a2.cap = 0;
    lua_close(B);
  }

  /* a restore that runs out of memory fails as a whole: a message, nothing left */
  private static void test_host_restore_out_of_memory()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, "big = {} for i = 1, 20000 do big[i] = 'item' .. i end return 'ok'").equals("ok"));
    CHECK(eval(A, build_perms).equals("ok"));
    lua_getglobal(A, new HostString("_PERMS"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(A, 1, w, msg) == LUA_OK);
    lua_close(A);

    CountingAlloc a2 = new CountingAlloc();
    Setup s = new Setup(null, () -> a2.cap = a2.used + 64 * 1024);
    lua_State B = restore(a2, w.out.toString(), s, msg);
    a2.cap = 0;
    CHECK(B == null);
    CHECK(msg.to_os().contains("not enough memory"));
    CHECK(a2.used == 0);
    if (B != null)
      lua_close(B);
  }

  /* values: host restore takes the inverse permanents and may restore the main thread */
  private static void test_host_value_restore()
  {
    lua_State A = new_state();
    CHECK(push_value_and_perms(A,
      "local t = {list = {1, 2}, f = function() return 7 end} t.self = t " +
      "return {t, coroutine.running(), print}, {[print] = 'print'}"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(lua_styxpersistvalue(A, 2, 1, w, msg) == LUA_OK);
    lua_close(A);

    lua_State B = new_state();
    CHECK(eval(B, "_BACK = {print = tostring} return 'ok'").equals("ok"));
    lua_getglobal(B, new HostString("_BACK"));
    CHECK(lua_styxunpersistvalue(B, 1, view(w.out), msg) == LUA_OK);
    CHECK(lua_gettop(B) == 2 && lua_type(B, 2) == LUA_TTABLE);
    lua_setglobal(B, new HostString("v"));
    lua_settop(B, 0);
    CHECK(eval(B, "return tostring(v[1].self == v[1] and v[1].f() == 7 and #v[1].list == 2)").equals("true"));
    CHECK(eval(B, "return tostring(v[2] == coroutine.running() and v[3] == tostring)").equals("true"));
    lua_close(B);
  }

  /* the formats keep apart: scripts cannot read state streams, a state is
     restored neither from a value's stream nor from what is not a stream */
  private static void test_host_stream_kinds()
  {
    lua_State L = new_state();
    CHECK(eval(L, build_perms).equals("ok"));
    lua_getglobal(L, new HostString("_PERMS"));
    StringWriter ws = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(L, 1, ws, msg) == LUA_OK);
    push_string(L, ws.out.toString());
    CHECK(call_styx(L, "unpersist", 1, 1) == LUA_ERRRUN);
    lua_settop(L, 0);
    String value = persist_in_new_state("return require('styx').persist({1, 2})");
    CountingAlloc a = new CountingAlloc();
    CHECK(restore(a, value, null, msg) == null);
    CHECK(msg.to_os().startsWith("invalid persisted data"));
    CHECK(restore(a, "garbage", null, msg) == null);
    CHECK(msg.to_os().equals("not a persisted state"));
    CHECK(a.used == 0);
    CHECK(lua_styxunpersistvalue(L, 0, view(ws.out), msg) != LUA_OK);
    CHECK(lua_gettop(L) == 0);
    lua_close(L);
  }

  private static void external_hook(lua_State L, lua_Debug ar)
  {
  }

  private static final lua_Hook external_hook = StyxTests::external_hook;

  /* a hook set from C (not by the debug library) is not supported yet */
  private static void test_external_hook_not_supported()
  {
    lua_State L = new_state();
    CHECK(eval(L, "co = coroutine.create(function() coroutine.yield() end) coroutine.resume(co) return 'ok'").equals("ok"));
    lua_getglobal(L, new HostString("co"));
    lua_sethook(lua_tothread(L, -1), external_hook, LUA_MASKLINE, 0);
    lua_pop(L, 1);
    CHECK(eval(L, "local ok, err = pcall(require('styx').persist, " +
                  "{[_ENV] = 'env', [coroutine.yield] = 'y'}, {co}) return err")
          .equals("a hook not set by the debug library is not supported yet (at root[1])"));
    CHECK(eval(L, "_P = {[_ENV] = 'env', [coroutine.yield] = 'y'} return 'ok'").equals("ok"));
    lua_getglobal(L, new HostString("_P"));
    lua_getglobal(L, new HostString("co"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(lua_styxpersistvalue(L, 1, 2, w, msg) == LUA_ERRRUN);
    CHECK(msg.to_os().equals("a hook not set by the debug library is not supported yet (at root)"));
    lua_close(L);
  }

  /*
  ** Table order across states: restored with the persisting state's seed, a
  ** table iterates in the same order; with another seed its entries are all
  ** there (inserted again, in another order).
  */
  private static final String churn =
    "math.randomseed(42) t = {} " +
    "for i = 1, 200 do t['key' .. math.random(2000)] = i end " +
    "for k in pairs(t) do if math.random() < 0.3 then t[k] = nil end end " +
    "for i = 1, 50 do t['new' .. math.random(2000)] = i end " +
    "order = {} for k in pairs(t) do order[#order + 1] = k end " +
    "order = table.concat(order, ' ') return 'ok'";

  private static void test_table_order_across_states()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = lua_newstate(a1, 1234);
    luaL_openlibs(A);
    preload_styx(A);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, churn).equals("ok"));
    String expected = eval(A, "return order");
    String blob = persist_state(A);
    lua_close(A);
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    CHECK(eval(B, "local o = {} for k in pairs(t) do o[#o + 1] = k end return table.concat(o, ' ')").equals(expected));
    lua_close(B);
  }


  /*
  ** Finalization and the collector in a whole state (ADR-28): objects marked
  ** for finalization are part of the state until their finalizers run, so
  ** garbage waiting for its finalizer, and finalizers already due, run after
  ** a restore, once each, in the original's order; the collector's mode,
  ** parameters and running state are restored.
  */


  private static void test_host_state_marked_garbage()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, "collectgarbage('stop') finalized = {} " +
                  "local mt = {__gc = function(o) finalized[#finalized + 1] = o.name end} " +
                  "for i = 1, 5 do setmetatable({name = 'g' .. i}, mt) end " +
                  "kept = setmetatable({name = 'kept'}, mt) " +
                  "collectgarbage('restart') return 'ok'").equals("ok"));
    String blob = persist_state(A);
    String order = eval(A, "collectgarbage() collectgarbage() return table.concat(finalized, ' ')");
    CHECK(order.equals("g5 g4 g3 g2 g1"));
    lua_close(A);

    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    lua_getglobal(B, new HostString("finalized")); /* (read with no allocation: no step of the collector) */
    CHECK(lua_rawlen(B, -1) == 0); /* (the restore called none: the collector's next step does) */
    lua_pop(B, 1);
    CHECK(eval(B, "collectgarbage() collectgarbage() return table.concat(finalized, ' ')").equals(order));
    CHECK(eval(B, "kept = nil collectgarbage() collectgarbage() return finalized[#finalized]").equals("kept"));
    lua_close(B);
  }

  private static void test_host_state_pending_finalizers()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, "collectgarbage('incremental') count = 0 " +
                  "local mt = {__gc = function() count = count + 1 end} " +
                  "for i = 1, 3000 do setmetatable({}, mt) end return 'ok'").equals("ok"));
    /* step the collector until some finalizers ran and the others are due */
    int ran = 0;
    for (int i = 0; i < 100000 && ran == 0; i++)
    {
      lua_gc(A, LUA_GCSTEP, 0, 0, 0);
      lua_getglobal(A, new HostString("count"));
      ran = (int) lua_tointeger(A, -1);
      lua_pop(A, 1);
    }
    CHECK(ran > 0 && ran < 3000);
    String blob = persist_state(A);
    CHECK(eval(A, "collectgarbage() return tostring(count)").equals("3000"));
    lua_close(A);

    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    lua_getglobal(B, new HostString("count"));
    CHECK(lua_tointeger(B, -1) >= ran);
    lua_pop(B, 1);
    CHECK(eval(B, "collectgarbage() collectgarbage() return tostring(count)").equals("3000"));
    lua_close(B);
  }

  private static final String read_settings =
    "local p = {} for _, n in ipairs{'minormul', 'majorminor', 'minormajor', 'pause', 'stepmul', 'stepsize'} " +
    "do p[#p + 1] = collectgarbage('param', n) end " +
    "local running = collectgarbage('isrunning') " +
    "local mode = collectgarbage('incremental') collectgarbage(mode) " +
    "return table.concat(p, ' ') .. ' ' .. tostring(running) .. ' ' .. mode";

  private static void test_host_state_collector_settings()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, "collectgarbage('generational') collectgarbage('param', 'minormul', 33) " +
                  "collectgarbage('param', 'pause', 150) collectgarbage('param', 'stepmul', 300) " +
                  "collectgarbage('stop') return 'ok'").equals("ok"));
    String settings = eval(A, read_settings);
    CHECK(settings.contains("false generational"));
    String blob = persist_state(A);
    lua_close(A);
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    CHECK(eval(B, read_settings).equals(settings));
    lua_close(B);

    /* and back: a running incremental state restored over a stopped one */
    CountingAlloc a3 = new CountingAlloc();
    lua_State C = new_counted_state(a3);
    CHECK(eval(C, build_perms).equals("ok"));
    String defaults = eval(C, read_settings);
    CHECK(defaults.contains("true incremental"));
    blob = persist_state(C);
    lua_close(C);
    CountingAlloc a4 = new CountingAlloc();
    Setup s = new Setup(L -> {
                          CHECK(eval(L, "collectgarbage('generational') collectgarbage('param', 'pause', 400) " +
                                        "collectgarbage('stop') return 'ok'").equals("ok"));
                        },
                        null);
    lua_State D = restored_state(a4, blob, s);
    CHECK(eval(D, read_settings).equals(defaults));
    lua_close(D);
  }

  private static int noop(lua_State L)
  {
    return 0;
  }

  private static final lua_CFunction noop = StyxTests::noop;

  /* garbage with a finalizer is part of the state: a userdata is not supported yet */
  private static void test_host_state_garbage_userdata()
  {
    lua_State A = new_state();
    CHECK(eval(A, build_perms).equals("ok"));
    lua_newuserdatauv(A, 8, 0);
    lua_createtable(A, 0, 1);
    lua_pushcfunction(A, noop);
    lua_setfield(A, -2, new HostString("__gc"));
    lua_setmetatable(A, -2);
    lua_pop(A, 1);
    lua_getglobal(A, new HostString("_PERMS"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(A, 1, w, msg) == LUA_ERRRUN);
    CHECK(msg.to_os().equals("persisting a userdata value is not supported yet (at finalizer list)"));
    lua_gc(A, LUA_GCCOLLECT, 0, 0, 0); /* finalized: no longer marked */
    StringWriter w2 = new StringWriter();
    CHECK(luaL_persiststate(A, 1, w2, msg) == LUA_OK);
    lua_close(A);
  }

  /* next from a deleted key: with the seed kept, as before; with another, the contents */
  private static void test_dead_keys_across_states()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = lua_newstate(a1, 77);
    luaL_openlibs(A);
    preload_styx(A);
    CHECK(eval(A, "t = {} for i = 1, 40 do t['k' .. i] = i end gone = {} " +
                  "for i = 1, 40, 3 do t['k' .. i] = nil gone[#gone + 1] = 'k' .. i end " +
                  "collectgarbage() collectgarbage() " +
                  "local e = {} for i, k in ipairs(gone) do e[i] = tostring(next(t, k)) end " +
                  "expected = table.concat(e, ' ') return 'ok'").equals("ok"));
    String expected = eval(A, "return expected");
    String blob = eval(A, "return require('styx').persist({t = t, gone = gone})");
    lua_close(A);
    HostStringOut msg = new HostStringOut();
    for (int seed : new int[] {77, 78})
    {
      CountingAlloc a2 = new CountingAlloc();
      lua_State B = lua_newstate(a2, seed);
      luaL_openlibs(B);
      preload_styx(B);
      CHECK(lua_styxunpersistvalue(B, 0, view(blob), msg) == LUA_OK);
      lua_setglobal(B, new HostString("v"));
      CHECK(eval(B, "local n = 0 for _ in pairs(v.t) do n = n + 1 end return tostring(n == 26 and v.t.k2 == 2)").equals("true"));
      if (seed == 77)
        CHECK(eval(B, "local e = {} for i, k in ipairs(v.gone) do e[i] = tostring(next(v.t, k)) end " +
                      "return table.concat(e, ' ')").equals(expected));
      lua_close(B);
    }
  }


  /*
  ** Object keys across states (ADR-30): a table, function or thread keeps the
  ** hash it was given when created, so a restored table with object keys
  ** iterates in the same order in a state with the same seed; keys that are
  ** the restoring state's own objects (permanents) keep it when they were
  ** made the same way (the libraries, with the same seed).
  */
  private static final String object_keys_program =
    "math.randomseed(7) t, names = {}, {} " +
    "for i = 1, 200 do local r = math.random(3) " +
    "  local k = (r == 1) and {} or (r == 2) and function() return i end or coroutine.create(print) " +
    "  t[k] = i names[k] = 'o' .. i end " +
    "for k in pairs(t) do if math.random() < 0.3 then t[k] = nil end end " +
    "collectgarbage() collectgarbage() " +
    "function order(tt, nn) local o = {} for k in pairs(tt) do o[#o + 1] = nn[k] or '?' end " +
    "  return table.concat(o, ' ') end " +
    "return 'ok'";

  private static void test_object_key_order_across_states()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = lua_newstate(a1, 4321);
    luaL_openlibs(A);
    preload_styx(A);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, object_keys_program).equals("ok"));
    String expected = eval(A, "return order(t, names)");
    String blob = persist_state(A);
    lua_close(A);
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    CHECK(eval(B, "return order(t, names)").equals(expected));
    lua_close(B);
  }

  /* library objects as keys: made the same way with the same seed, they hash the same */
  private static void test_library_keys_across_states()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = lua_newstate(a1, 99);
    luaL_openlibs(A);
    preload_styx(A);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, "t = {} local i = 0 " +
                  "for _, k in ipairs{io.stdout, io.stderr, io.stdin, print, string.format, math.floor, " +
                  "                   coroutine.yield, table.insert} do i = i + 1 t[k] = i end " +
                  "for i = 1, 30 do t[{}] = 100 + i end " +
                  "local o = {} for k, v in pairs(t) do o[#o + 1] = v end expected = table.concat(o, ' ') " +
                  "return 'ok'").equals("ok"));
    String expected = eval(A, "return expected");
    String blob = persist_state(A);
    lua_close(A);
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    CHECK(eval(B, "local o = {} for k, v in pairs(t) do o[#o + 1] = v end return table.concat(o, ' ')").equals(expected));
    CHECK(eval(B, "return tostring(t[io.stdout] == 1 and t[print] == 4 and t[table.insert] == 8)").equals("true"));
    lua_close(B);
  }

  /* a whole state goes on giving new objects the hashes the original would */
  private static void test_state_next_hash()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, "function make() local u = {} for i = 1, 60 do u[(i % 2 == 0) and {} or function() end] = i end " +
                  "  local o = {} for k, v in pairs(u) do o[#o + 1] = v end return table.concat(o, ' ') end " +
                  "return 'ok'").equals("ok"));
    String blob = persist_state(A);
    lua_getglobal(A, new HostString("make"));
    CHECK(lua_pcall(A, 0, 1, 0) == LUA_OK);
    String expected = to_string(A, -1);
    lua_close(A);

    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    lua_getglobal(B, new HostString("make"));
    CHECK(lua_pcall(B, 0, 1, 0) == LUA_OK);
    CHECK(to_string(B, -1).equals(expected));
    lua_close(B);
  }


  /* a whole state is restored only into a state made for its stream */
  /* the restore makes its state with the persisted seed: the host gives none */
  private static void test_restore_takes_the_seed()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = lua_newstate(a1, 31337);
    luaL_openlibs(A);
    preload_styx(A);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, "data = {x = 1, y = 2} return 'ok'").equals("ok"));
    String blob = persist_state(A);
    lua_close(A);
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    CHECK(B.l_G.seed == 31337);
    CHECK(eval(B, "return tostring(data.x + data.y)").equals("3"));
    lua_close(B);
  }

  /*
  ** A VM persisted and restored anywhere goes on exactly as it would have:
  ** the original and a restored copy run the same code after the snapshot
  ** and must produce the same transcript (what is deterministic within one
  ** run: table order, object hashes, new keys, suspended loops; ADR-31).
  */
  private static final String vm_part1 =
    "P = {} " +
    "P.s = {} for i = 1, 300 do P.s['key' .. (i * 7 % 500)] = i end " +
    "for k in pairs(P.s) do if #k % 2 == 0 then P.s[k] = nil end end " +
    "P.o, P.names = {}, {} " +
    "for i = 1, 200 do local k = (i % 3 == 0) and {} or (i % 3 == 1) and function() return i end " +
    "  or coroutine.create(print) P.o[k] = i P.names[k] = 'o' .. i end " +
    "for k, v in pairs(P.o) do if v % 4 == 0 then P.o[k] = nil end end " +
    "P.gone = {} for k, v in pairs(P.s) do if v % 5 == 0 then P.s[k] = nil P.gone[#P.gone + 1] = k end end " +
    "for i = 1, 100 do local junk = 'later' .. i end " +
    "P.walk_s = coroutine.create(function() for k, v in pairs(P.s) do coroutine.yield(k .. '=' .. v) end end) " +
    "P.walk_o = coroutine.create(function() for k in pairs(P.o) do P.o[k] = nil coroutine.yield(P.names[k]) end end) " +
    "for i = 1, 10 do coroutine.resume(P.walk_s) coroutine.resume(P.walk_o) end " +
    "local n = 0 P.inc = function() n = n + 1 return n end P.get = function() return n end " +
    "P.list = {} for i = 1, 20 do P.list[i] = i * 3 end P.list[25] = 'far' " +
    "P.closer = coroutine.create(function() " +
    "  local res <close> = setmetatable({}, {__close = function() P.closed = 'yes' end}) " +
    "  coroutine.yield('opened') return 'done' end) " +
    "coroutine.resume(P.closer) " +
    "return 'ok'";

  private static final String vm_part2 =
    "local out = {} local function w(x) out[#out + 1] = tostring(x) end " +
    "for i = 1, 20 do w(select(2, coroutine.resume(P.walk_s))) w(select(2, coroutine.resume(P.walk_o))) end " +
    "for k in pairs(P.s) do w(k) end " +
    "for k in pairs(P.o) do w(P.names[k]) end " +
    "for _, k in ipairs(P.gone) do w(next(P.s, k)) end " +
    "for i = 1, 100 do P.s['later' .. i] = i end " +
    "for i = 1, 60 do local k = (i % 2 == 0) and {} or function() end P.o[k] = 1000 + i P.names[k] = 'n' .. i end " +
    "for k in pairs(P.s) do w(k) end " +
    "for k in pairs(P.o) do w(P.names[k]) end " +
    "local t = {} for i = 1, 40 do t[{}] = i end for _, v in pairs(t) do w(v) end " +
    "w(P.inc()) w(P.inc()) w(P.get()) " +
    "w(#P.list) table.insert(P.list, 1, 'first') w(#P.list) w(table.remove(P.list)) " +
    "local arr = {} for i = 1, 50 do arr[i] = (i * 37) % 101 end table.sort(arr) w(table.concat(arr, ',')) " +
    "w(select(2, coroutine.resume(P.closer))) w(P.closed) " +
    "w(select(2, pcall(function() local x = nil return x.field end))) " +
    "w(string.format('%5.2f %d %q', 3.14159, 42, 'q')) " +
    "for k in pairs(_G) do w(k) end for k in pairs(package.loaded) do w(k) end " +
    "for k in pairs(string) do w(k) end for k in pairs(debug.getregistry()) do w(type(k)) end " +
    "return table.concat(out, ' ')";

  private static void test_snapshot_continues_identically()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = lua_newstate(a1, 2024);
    luaL_openlibs(A);
    preload_styx(A);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, vm_part1).equals("ok"));
    String blob = persist_state(A);
    String expected = eval(A, vm_part2);
    CHECK(expected.length() > 1000);
    lua_close(A);

    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    String got = eval(B, vm_part2);
    if (!got.equals(expected))
    {
      int i = 0;
      while (i < got.length() && i < expected.length() && got.charAt(i) == expected.charAt(i))
        i++;
      printf("      first difference at %d: '%s' vs '%s'\n", i, expected.substring(i, Math.min(expected.length(), i + 40)),
             got.substring(i, Math.min(got.length(), i + 40)));
    }
    CHECK(got.equals(expected));
    lua_close(B);
  }


  /*
  ** Frozen threads in a whole state (ADR-34): a user program frozen by its
  ** budget at a point, persisted with the state and restored in a new one,
  ** goes on exactly as the original does. The cases are the library callbacks
  ** of tests/deep/libcases.lua (what the standard library calls back from C
  ** frames); each is frozen at every one of its budget points of 17 steps.
  */
  private static final String frozen_prelude = """

cases = dofile(CASES_FILE)
busyn = 0
function busy() busyn = busyn + 1 for i = 1, 3 do end end
-- a deterministic text of results
local function show(v, seen)
  local t = type(v)
  if t == "string" then return string.format("%q", v) end
  if t == "table" then
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    local parts = {}
    for _, k in ipairs(keys) do parts[#parts + 1] = show(k) .. "=" .. show(v[k]) end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  if t == "function" or t == "thread" or t == "userdata" then return t end
  return tostring(v)
end
function render(...)
  local parts = {}
  for i = 1, select("#", ...) do parts[i] = show((select(i, ...))) end
  return table.concat(parts, " ")
end
function start(i, n, k)
  local c = cases[i]
  fcount = 0
  co = coroutine.create(function() return render(c.f(busy)) end)
  if n > 0 then
    debug.setbudget(co, n, function() fcount = fcount + 1 if fcount == k then debug.freeze() end end)
  end
  local ok, r = coroutine.resume(co)
  if not ok then result = "ERR " .. tostring(r) return "done" end
  if debug.isfrozen(co) then return "frozen" end
  result = r
  return "done"
end
function finish()
  while debug.isfrozen(co) do
    local ok, r = debug.thaw(co)
    if not ok then result = "ERR " .. tostring(r) return end
    if not debug.isfrozen(co) then result = r end
  end
end
""";

  private static String g_cases_file = "../deep/libcases.lua";

  private static lua_State frozen_state(CountingAlloc a)
  {
    lua_State L = new_counted_state(a);
    CHECK(eval(L, build_perms).equals("ok"));
    CHECK(eval(L, "CASES_FILE = '" + g_cases_file + "'").equals(""));
    CHECK(eval(L, frozen_prelude).equals(""));
    return L;
  }

  /* (C's atoi) */
  private static int atoi(String s)
  {
    int i = 0, n = 0;
    boolean neg = false;
    while (i < s.length() && Character.isWhitespace(s.charAt(i)))
      i++;
    if (i < s.length() && (s.charAt(i) == '+' || s.charAt(i) == '-'))
      neg = s.charAt(i++) == '-';
    while (i < s.length() && s.charAt(i) >= '0' && s.charAt(i) <= '9')
      n = n * 10 + (s.charAt(i++) - '0');
    return neg ? -n : n;
  }

  private static void frozen_cases(int mincases, int minpoints)
  {
    CountingAlloc a0 = new CountingAlloc();
    lua_State P = frozen_state(a0);
    int ncases = atoi(eval(P, "return #cases"));
    CHECK(ncases >= mincases);
    int points = 0;
    for (int i = 1; i <= ncases; i++)
    {
      String plain = eval(P, "start(" + i + ", 0, 0) finish() return result");
      String name = eval(P, "return cases[" + i + "].name");
      CHECK(!plain.startsWith("ERR"));
      for (int k = 1; k < 400; k++)
      {
        CountingAlloc a1 = new CountingAlloc();
        lua_State A = frozen_state(a1);
        String st = eval(A, "return start(" + i + ", 17, " + k + ")");
        if (!st.equals("frozen"))
        {
          lua_close(A);
          break;
        }
        points++;
        String blob = persist_state(A);
        if (blob.isEmpty())
        { /* (it failed: said so; the first point of the case is enough) */
          printf("      case %d (%s), point %d: not persisted\n", i, name, k);
          lua_close(A);
          break;
        }
        CountingAlloc a2 = new CountingAlloc();
        int before = g_test_failures;
        lua_State B = restored_state(a2, blob);
        if (g_test_failures != before)
          printf("      (case %d (%s), point %d)\n", i, name, k);
        CHECK(eval(B, "if debug.isfrozen(co) then return 'yes' end return 'no'").equals("yes"));
        String ra = eval(A, "finish() return result");
        String rb = eval(B, "finish() return result");
        if (!ra.equals(plain) || !rb.equals(plain) || !eval(A, "return busyn").equals(eval(B, "return busyn")))
          printf("      case %d (%s), point %d: plain [%s] original [%s] restored [%s] busy %s/%s\n", i, name, k,
                 plain, ra, rb, eval(A, "return busyn"), eval(B, "return busyn"));
        CHECK(ra.equals(plain));
        CHECK(rb.equals(plain));
        CHECK(eval(A, "return busyn").equals(eval(B, "return busyn")));
        lua_close(A);
        lua_close(B);
      }
    }
    CHECK(points >= minpoints);
    lua_close(P);
  }

  private static void test_frozen_library_cases()
  {
    g_cases_file = "../deep/libcases.lua";
    frozen_cases(40, 500);
  }

  private static void test_frozen_engine_cases()
  {
    g_cases_file = "frozencases.lua";
    frozen_cases(8, 200);
  }


  /*
  ** Frozen threads with what the host provides (ADR-34): a continuation and a
  ** budget callback of its own persist when they have a name, and not before;
  ** and a restore that runs out of memory fails cleanly.
  */
  private static int host_k(lua_State L, int status, long ctx)
  {
    CHECK(status == LUA_YIELD || status == LUA_OK);
    lua_pushinteger(L, ctx);
    return 2; /* (the result of the call, and the context) */
  }

  private static final lua_KFunction host_k = StyxTests::host_k;

  private static int host_callk(lua_State L)
  {
    lua_settop(L, 2);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_callk(L, 1, 1, 77, host_k);
    return host_k(L, LUA_OK, 77);
  }

  private static final lua_CFunction host_callk = StyxTests::host_callk;

  private static int host_budget_calls = 0;

  private static void host_budget(lua_State user_co, lua_State spender)
  {
    host_budget_calls++;
    if (host_budget_calls == 2)
      lua_freeze(spender);
  }

  private static final lua_Budget host_budget = StyxTests::host_budget;

  /* a state with the host's function 'callk' and the permanents */
  private static lua_State host_state(CountingAlloc a)
  {
    lua_State L = new_counted_state(a);
    lua_pushcfunction(L, host_callk);
    lua_setglobal(L, new HostString("callk"));
    CHECK(eval(L, build_perms).equals("ok"));
    CHECK(eval(L, "CASES_FILE = 'frozencases.lua'").equals(""));
    CHECK(eval(L, frozen_prelude).equals(""));
    return L;
  }

  /* the host's function, made before a restore */
  private static void add_callk(lua_State L)
  {
    lua_pushcfunction(L, host_callk);
    lua_setglobal(L, new HostString("callk"));
  }

  /* the state restored for a host_state() (it has the host's function too) */
  private static lua_State restored_host_state(CountingAlloc a, String blob)
  {
    Setup s = new Setup(StyxTests::add_callk, null);
    return restored_state(a, blob, s);
  }

  private static final String host_program =
    "function work(n) local s = 0 for i = 1, n do s = s + i end return s end " +
    "log = {} " +
    "co = coroutine.create(function() local r, ctx = callk(work, 40) log[#log + 1] = r .. '/' .. ctx return r, ctx end) " +
    "fcount = 0 " +
    "debug.setbudget(co, 7, function() fcount = fcount + 1 if fcount == 3 then debug.freeze() end end) " +
    "coroutine.resume(co) " +
    "return debug.isfrozen(co) and 'frozen' or 'done'";

  private static void test_frozen_host_continuation()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = host_state(a1);
    CHECK(eval(A, host_program).equals("frozen"));
    /* the continuation has no name: the state is not persisted */
    lua_getglobal(A, new HostString("_PERMS"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(A, lua_gettop(A), w, msg) != LUA_OK);
    CHECK(msg.to_os().contains("a continuation that has no name"));
    lua_pop(A, 1);
    /* named, it is */
    lua_registerk("styxtest.host_k", host_k);
    String blob = persist_state(A);
    CHECK(!blob.isEmpty());
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_host_state(a2, blob);
    CHECK(eval(B, "if debug.isfrozen(co) then return 'yes' end return 'no'").equals("yes"));
    CHECK(eval(A, "finish() return table.concat(log, ',')").equals("820/77"));
    CHECK(eval(B, "finish() return table.concat(log, ',')").equals("820/77"));
    CHECK(eval(A, "return coroutine.status(co)").equals("dead") && eval(B, "return coroutine.status(co)").equals("dead"));
    lua_close(A);
    lua_close(B);
  }

  private static void test_frozen_host_budget_callback()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = host_state(a1);
    CHECK(eval(A, "co = coroutine.create(function() local s = 0 for i = 1, 60 do s = s + i end return s end) return 'ok'").equals("ok"));
    lua_getglobal(A, new HostString("co"));
    lua_State th = lua_tothread(A, -1);
    host_budget_calls = 0;
    lua_setbudget(th, 9, host_budget);
    lua_pop(A, 1);
    CHECK(eval(A, "coroutine.resume(co) return debug.isfrozen(co) and 'frozen' or 'done'").equals("frozen"));
    lua_getglobal(A, new HostString("_PERMS"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(A, lua_gettop(A), w, msg) != LUA_OK);
    CHECK(msg.to_os().contains("a budget callback that has no name"));
    lua_pop(A, 1);
    lua_registerbudget("styxtest.host_budget", host_budget);
    String blob = persist_state(A);
    CHECK(!blob.isEmpty());
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_host_state(a2, blob);
    int before = host_budget_calls;
    CHECK(eval(B, "while debug.isfrozen(co) do debug.thaw(co) end return coroutine.status(co)").equals("dead"));
    CHECK(host_budget_calls > before); /* (the callback of the restored group ran) */
    lua_close(A);
    lua_close(B);
  }

  /*
  ** What the host's refused closes of a frozen thread left on it (their
  ** messages) is not the thread's: the state is persisted without them, and
  ** the restored thread thaws to its own results only (frozen in a C
  ** function, 'table.unpack', that goes on from the top of its stack)
  */
  private static void test_frozen_refusals_not_persisted()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = host_state(a1);
    CHECK(eval(A, "for k = 1, 200 do " +
                  "  co = coroutine.create(function(a) local t = {} for i = 1, 20000 do t[i] = i end " +
                  "    local r = table.pack(table.unpack(t)) local same = true " +
                  "    for i = 1, r.n do same = same and r[i] == i end return r.n + a, same end) " +
                  "  fcount = 0 " +
                  "  debug.setbudget(co, 1000, function() fcount = fcount + 1 if fcount == k then debug.freeze() end end) " +
                  "  coroutine.resume(co, 5) " +
                  "  if debug.isfrozen(co) and debug.getinfo(co, 0, 'S').what == 'C' then return 'frozen' end " +
                  "end return 'none'").equals("frozen"));
    lua_getglobal(A, new HostString("co"));
    lua_State th = lua_tothread(A, -1);
    lua_pop(A, 1);
    for (int i = 0; i < 3; i++)
      CHECK(lua_closethread(th, A) == LUA_ERRRUN); /* (each leaves its message on 'th') */
    String blob = persist_state(A);
    CHECK(!blob.isEmpty());
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_host_state(a2, blob);
    String thaw = "local r repeat r = table.pack(debug.thaw(co)) until not debug.isfrozen(co) " +
                  "return r.n .. ' ' .. tostring(r[1]) .. ' ' .. tostring(r[2]) .. ' ' .. tostring(r[3])";
    CHECK(eval(B, thaw).equals("3 true 20005 true"));
    CHECK(eval(A, thaw).equals("3 true 20005 true"));
    lua_close(A);
    lua_close(B);
  }

  private static void test_frozen_restore_out_of_memory()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = host_state(a1);
    CHECK(eval(A, "co = coroutine.create(function() " +
                  "  local inner = coroutine.create(function() local t = {} for i = 1, 300 do t[i] = i end " +
                  "    for i = 1, 100 do local x = string.format('%d', i) end return #t end) " +
                  "  return coroutine.resume(inner) end) " +
                  "fcount = 0 " +
                  "debug.setbudget(co, 40, function() fcount = fcount + 1 if fcount == 3 then debug.freeze() end end) " +
                  "coroutine.resume(co) return debug.isfrozen(co) and 'frozen' or 'done'").equals("frozen"));
    String blob = persist_state(A);
    CHECK(!blob.isEmpty());
    String expected = eval(A, "finish() return tostring(coroutine.status(co))");
    /* with less and less room, a restore fails with a memory error and leaves nothing; with enough, it works */
    boolean failed = false, worked = false;
    for (long extra = 2 * 1024; extra <= 400 * 1024; extra += 6 * 1024)
    {
      CountingAlloc a2 = new CountingAlloc();
      long extra_ = extra;
      Setup s = new Setup(StyxTests::add_callk, () -> a2.cap = a2.used + extra_);
      HostStringOut msg = new HostStringOut();
      lua_State B = restore(a2, blob, s, msg);
      a2.cap = 0;
      if (B == null)
      {
        failed = true;
        CHECK(msg.to_os().contains("not enough memory"));
        CHECK(a2.used == 0);
      }
      else
      {
        worked = true;
        CHECK(eval(B, "finish() return tostring(coroutine.status(co))").equals(expected));
        lua_close(B);
      }
    }
    CHECK(failed && worked);
    lua_close(A);
  }


  /*
  ** The '__gc' that waits for a budget group (ADR-34): objects of a user
  ** program that died while no thread of its group ran wait in the group's
  ** queue, and run, once each and in the same order, when a thread of the
  ** group goes on after a restore; the objects marked and not yet dead keep
  ** their group; an object whose group is gone does not run its '__gc'.
  */
  private static void test_frozen_deferred_gc()
  {
    final String script =
      "log = {} " +
      "co = coroutine.create(function() " +
      "  do local mt = {__gc = function(o) log[#log + 1] = 'gc' .. o.id end} " +
      "     for i = 1, 3 do setmetatable({id = i}, mt) end " +
      "     alive = setmetatable({id = 9}, mt) end " +
      "  coroutine.yield('made') " +
      "  for i = 1, 5 do end " +
      "  coroutine.yield('ran') " +
      "  alive = nil " +
      "  return 'end' " +
      "end) " +
      "debug.setbudget(co, 1000000, function() end) " +
      "assert(coroutine.resume(co)) " +
      "kernel = setmetatable({}, {__gc = function() log[#log + 1] = 'kernel' end}) " +
      "kernel = nil " +
      "collectgarbage() " + /* (the kernel runs: the dead objects of the group wait in its queue) */
      "return #log";
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, script).equals("1"));              /* only the kernel's ran */
    CHECK(eval(A, "return log[1]").equals("kernel"));
    String blob = persist_state(A);
    CHECK(!blob.isEmpty());
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    CHECK(eval(B, "return #log").equals("1"));
    /* the group goes on: its queue is drained when it runs */
    CHECK(eval(A, "assert(coroutine.resume(co)) return table.concat(log, ',')").equals(
          eval(B, "assert(coroutine.resume(co)) return table.concat(log, ',')")));
    CHECK(eval(A, "return #log").equals("4"));
    CHECK(eval(A, "return table.concat(log, ',', 2)").contains("gc"));
    /* the live one dies with the program, in both alike: the thread is dead,
       so nobody drains its group's queue */
    CHECK(eval(A, "assert(coroutine.resume(co)) collectgarbage() return table.concat(log, ',')").equals(
          eval(B, "assert(coroutine.resume(co)) collectgarbage() return table.concat(log, ',')")));
    CHECK(eval(A, "return #log").equals(eval(B, "return #log")));
    lua_close(A);
    lua_close(B);
  }


  /*
  ** A thread called by the host (lua_call and lua_pcall on it, not resumed by
  ** a coroutine): when it freezes, the call has not returned, and the host's
  ** function, its message handler and its results are part of the frozen
  ** state (FZ_CALL, FZ_PCALL).
  */
  private static int hostcall_calls = 0;

  private static void hostcall_budget(lua_State user_co, lua_State spender)
  {
    hostcall_calls++;
    if (hostcall_calls == 3)
      lua_freeze(spender);
  }

  private static final lua_Budget hostcall_budget = StyxTests::hostcall_budget;

  private static void test_frozen_host_called_thread()
  {
    lua_registerbudget("styxtest.hostcall_budget", hostcall_budget);
    for (int variant = 0; variant < 2; variant++)
    {
      CountingAlloc a1 = new CountingAlloc();
      lua_State A = host_state(a1);
      CHECK(eval(A, "function work(n) local s = 0 for i = 1, n do s = s + i end return s, 'done' end " +
                    "handlers = 0 " +
                    "co = coroutine.create(print) return 'ok'").equals("ok"));
      lua_getglobal(A, new HostString("co"));
      lua_State th = lua_tothread(A, -1);
      lua_pop(A, 1);
      lua_getglobal(th, new HostString("work"));
      lua_pushinteger(th, 200);
      hostcall_calls = 0;
      lua_setbudget(th, 11, hostcall_budget);
      /* the host calls: 'work' (variant 0: lua_call; 1: lua_pcall with a message handler) */
      if (variant == 0)
      {
        lua_call(th, 1, 2);
      }
      else
      {
        lua_getglobal(th, new HostString("debug"));
        lua_getfield(th, -1, new HostString("traceback"));
        lua_remove(th, -2);
        lua_insert(th, 1);
        CHECK(lua_pcall(th, 1, 2, 1) == LUA_OK);
      }
      CHECK(lua_isfrozen(th) != 0);
      String blob = persist_state(A);
      CHECK(!blob.isEmpty());
      CountingAlloc a2 = new CountingAlloc();
      lua_State B = restored_host_state(a2, blob);
      lua_getglobal(B, new HostString("co"));
      lua_State thb = lua_tothread(B, -1);
      lua_pop(B, 1);
      CHECK(lua_isfrozen(thb) != 0);
      /* thaw both: the host's call returns with its results */
      for (lua_State X : new lua_State[] {th, thb})
      {
        lua_ResumeResult rr = lua_thaw(X, null);
        while (rr.status == LUA_OK && lua_isfrozen(X) != 0)
          rr = lua_thaw(X, null);
        CHECK(rr.status == LUA_OK);
        CHECK(lua_isfrozen(X) == 0);
        CHECK(lua_gettop(X) >= 2);
        CHECK(lua_tointeger(X, lua_gettop(X) - 1) == 20100);
        CHECK(to_string(X, lua_gettop(X)).equals("done"));
      }
      lua_close(A);
      lua_close(B);
    }
  }


  /*
  ** An allocation that fails at each point of a restore, and of a script's
  ** persist, of a frozen chain: always a clean error ("not enough memory"), a
  ** state that still works, and, once nothing fails, the right result.
  */
  private static void test_frozen_allocation_failures()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = host_state(a1);
    CHECK(eval(A, "co = coroutine.create(function() " +
                  "  local inner = coroutine.create(function() local t = {} for i = 1, 20 do t[i] = tostring(i) end " +
                  "    local s = 0 for _, v in ipairs(t) do s = s + #v end return s end) " +
                  "  return coroutine.resume(inner) end) " +
                  "fcount = 0 " +
                  "debug.setbudget(co, 9, function() fcount = fcount + 1 if fcount == 4 then debug.freeze() end end) " +
                  "coroutine.resume(co) return debug.isfrozen(co) and 'frozen' or 'done'").equals("frozen"));
    String blob = persist_state(A);
    CHECK(!blob.isEmpty());
    String expected = eval(A, "finish() return tostring(coroutine.status(co))");
    /* restore: the n-th growth fails */
    int failures = 0;
    boolean finished = false;
    for (long n = 1; n < 20000 && !finished; n++)
    {
      CountingAlloc a2 = new CountingAlloc();
      long n_ = n;
      Setup s = new Setup(StyxTests::add_callk, () -> {
                            a2.grows = 0;
                            a2.failnth = n_;
                          });
      HostStringOut msg = new HostStringOut();
      lua_State B = restore(a2, blob, s, msg);
      a2.failnth = 0;
      if (B == null)
      {
        failures++;
        CHECK(msg.to_os().contains("not enough memory"));
        CHECK(a2.used == 0);
      }
      else
      {
        finished = true;
        CHECK(eval(B, "finish() return tostring(coroutine.status(co))").equals(expected));
        lua_close(B);
      }
    }
    CHECK(finished && failures > 50);
    /* a script's persist of the frozen chain: the n-th growth fails (the
       budget callback, the kernel's, is a permanent) */
    CHECK(eval(A, "_PERMS[select(2, debug.getbudget(co))] = 'cb' return 'ok'").equals("ok"));
    failures = 0;
    finished = false;
    for (long n = 1; n < 20000 && !finished; n++)
    {
      a1.grows = 0;
      a1.failnth = n;
      String r = eval(A, "local ok, err = pcall(require('styx').persist, _PERMS, {co = co}) " +
                         "if ok then return 'ok' end return err");
      a1.failnth = 0;
      if (r.equals("ok"))
        finished = true;
      else
      {
        failures++;
        CHECK(r.contains("not enough memory") || r.isEmpty());
        CHECK(eval(A, "return 'usable'").equals("usable"));
      }
    }
    CHECK(finished && failures > 10);
    lua_close(A);
  }


  /*
  ** A user object of a kind the host registered (ADR-34): its state, its
  ** metatable and its user values come back; a finalizer in the metatable is
  ** marked; a stream with a bad state, or of a kind nobody registered, is
  ** rejected.
  */
  private static final class TestObj implements lua_UserObject
  {
    long a = 0, b = 0;
    boolean values = true; /* are its user values part of its state? */

    @Override
    public String persistkind()
    {
      return "styxtest.obj";
    }

    @Override
    public boolean persistuservalues()
    {
      return values;
    }

    @Override
    public int persistints(long[] out)
    {
      out[0] = a;
      out[1] = b;
      return 2;
    }

    @Override
    public ByteView persistbytes()
    {
      return ByteView.of("payload");
    }
  }

  private static lua_UserObject make_testobj(lua_State L, long[] ints, int n, ByteView bytes)
  {
    if (n != 2 || !bytes.equals(ByteView.of("payload")) || ints[0] < 0)
      return null;
    TestObj o = new TestObj();
    o.a = ints[0];
    o.b = ints[1];
    return o;
  }

  private static final lua_ObjectFactory make_testobj = StyxTests::make_testobj;

  private static int testobj_new(lua_State L)
  {
    TestObj o = new TestObj();
    o.a = luaL_checkinteger(L, 1);
    o.b = luaL_checkinteger(L, 2);
    lua_newuserobject(L, o, 64, 2);
    return 1;
  }

  private static final lua_CFunction testobj_new = StyxTests::testobj_new;

  /* one whose user values are not part of its state */
  private static int testobj_newnouv(lua_State L)
  {
    TestObj o = new TestObj();
    o.a = luaL_checkinteger(L, 1);
    o.b = luaL_checkinteger(L, 2);
    o.values = false;
    lua_newuserobject(L, o, 64, 2);
    return 1;
  }

  private static final lua_CFunction testobj_newnouv = StyxTests::testobj_newnouv;

  private static int testobj_gcs = 0;

  private static int testobj_note(lua_State L)
  {
    testobj_gcs++;
    return 0;
  }

  private static final lua_CFunction testobj_note = StyxTests::testobj_note;

  private static int testobj_get(lua_State L)
  {
    TestObj o = lua_touserobject(L, 1) instanceof TestObj t ? t : null;
    luaL_argcheck(L, o != null, 1, new HostString("a test object"));
    lua_pushinteger(L, o.a);
    lua_pushinteger(L, o.b);
    return 2;
  }

  private static final lua_CFunction testobj_get = StyxTests::testobj_get;

  private static void test_user_object_kind()
  {
    lua_registerobjectkind("styxtest.obj", make_testobj);
    for (int in_state = 0; in_state < 2; in_state++)
    {
      CountingAlloc a1 = new CountingAlloc();
      lua_State A = new_counted_state(a1);
      lua_pushcfunction(A, testobj_new);
      lua_setglobal(A, new HostString("newobj"));
      lua_pushcfunction(A, testobj_get);
      lua_setglobal(A, new HostString("getobj"));
      lua_pushcfunction(A, testobj_note);
      lua_setglobal(A, new HostString("note"));
      CHECK(eval(A, build_perms).equals("ok"));
      CHECK(eval(A, "mt = {__index = function(u, k) return 'idx:' .. k end, __gc = function(u) note() end} log = {} " +
                    "u = newobj(5, -6) debug.setmetatable(u, mt) debug.setuservalue(u, 'first', 1) " +
                    "debug.setuservalue(u, {2}, 2) same = {u, u} return 'ok'").equals("ok"));
      if (in_state == 0)
      {
        /* a value, in the same state */
        CHECK(eval(A, "local styx = require 'styx' local r = styx.unpersist(_BACK, styx.persist(_PERMS, {u = u, mt = mt, same = same})) " +
                      "local a, b = getobj(r.u) " +
                      "return table.concat({type(r.u), a, b, tostring(r.u ~= u), tostring(getmetatable(r.u) == r.mt), r.u.name, " +
                      "debug.getuservalue(r.u, 1), debug.getuservalue(r.u, 2)[1], tostring(r.same[1] == r.same[2] and r.same[1] == r.u)}, ' ')").equals(
              "userdata 5 -6 true true idx:name first 2 true"));
        /* the stream is rejected when its state is not valid or the kind unknown */
        String bad = eval(A, "local blob = require('styx').persist(_PERMS, u) return blob");
        CHECK(!bad.isEmpty());
        lua_registerobjectkind("styxtest.obj", (L, ints, n, bytes) -> null);
        push_string(A, bad);
        lua_setglobal(A, new HostString("blob"));
        CHECK(eval(A, "return tostring(pcall(require('styx').unpersist, _BACK, blob))").equals("false"));
        lua_registerobjectkind("styxtest.obj", make_testobj);
        CHECK(eval(A, "return tostring(pcall(require('styx').unpersist, _BACK, blob))").equals("true"));
        /* an object whose user values are not part of its state: they come back nil */
        lua_pushcfunction(A, testobj_newnouv);
        lua_setglobal(A, new HostString("newnouv"));
        CHECK(eval(A, "local v = newnouv(7, 8) debug.setuservalue(v, 'gone', 1) debug.setuservalue(v, {3}, 2) " +
                      "local r = require('styx').unpersist(_BACK, require('styx').persist(_PERMS, {v})) " +
                      "local a, b = getobj(r[1]) " +
                      "return table.concat({a, b, tostring(debug.getuservalue(r[1], 1)), tostring(debug.getuservalue(r[1], 2))}, ' ')").equals(
              "7 8 nil nil"));
        /* its finalizer was marked: the restored object runs it */
        CHECK(eval(A, "collectgarbage() collectgarbage() return 'ok'").equals("ok"));
        testobj_gcs = 0;
        CHECK(eval(A, "(function() local r = require('styx').unpersist(_BACK, blob) end)() collectgarbage() collectgarbage() return 'ok'").equals("ok"));
        CHECK(testobj_gcs == 1);
      }
      else
      {
        /* the whole state, restored in another */
        String blob = persist_state(A);
        CHECK(!blob.isEmpty());
        CountingAlloc a2 = new CountingAlloc();
        Setup s = new Setup(L -> {
                              lua_pushcfunction(L, testobj_new);
                              lua_setglobal(L, new HostString("newobj"));
                              lua_pushcfunction(L, testobj_get);
                              lua_setglobal(L, new HostString("getobj"));
                              lua_pushcfunction(L, testobj_note);
                              lua_setglobal(L, new HostString("note"));
                            },
                            null);
        lua_State B = restored_state(a2, blob, s);
        CHECK(eval(B, "local a, b = getobj(u) return table.concat({a, b, u.name, debug.getuservalue(u, 1), debug.getuservalue(u, 2)[1], " +
                      "tostring(getmetatable(u) == mt)}, ' ')").equals("5 -6 idx:name first 2 true"));
        testobj_gcs = 0;
        CHECK(eval(B, "u = nil same = nil collectgarbage() collectgarbage() return 'ok'").equals("ok"));
        CHECK(testobj_gcs == 1);
        lua_close(B);
      }
      lua_close(A);
    }
  }


  /*
  ** A user object that a program (budgeted code) marked for finalization is
  ** the program's: restored, it still is. Its '__gc' runs as user code, where
  ** the program can freeze, not at once as a kernel's would (with the budget
  ** lifted, where nothing can freeze).
  */
  private static int owner_canfreeze = -1; /* what 'debug.canfreeze' said in the '__gc' */

  private static int owner_note(lua_State L)
  {
    owner_canfreeze = lua_toboolean(L, 1);
    testobj_gcs++;
    return 0;
  }

  private static final lua_CFunction owner_note = StyxTests::owner_note;

  private static void test_user_object_owner()
  {
    lua_registerobjectkind("styxtest.obj", make_testobj);
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    lua_pushcfunction(A, testobj_new);
    lua_setglobal(A, new HostString("newobj"));
    lua_pushcfunction(A, owner_note);
    lua_setglobal(A, new HostString("note"));
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, "mt = {__gc = function() note(debug.canfreeze()) end} " +
                  "local function program() " +
                  "  local o = newobj(1, 2) debug.setmetatable(o, mt) " +
                  "  coroutine.yield() " +
                  "  o = nil collectgarbage() return 'done' " +
                  "end " +
                  "prog = coroutine.create(program) " +
                  "local cb = function() end _PERMS[cb] = 'cb' _BACK.cb = cb " +
                  "debug.setbudget(prog, 100000, cb) " +
                  "coroutine.resume(prog) return 'ok'").equals("ok"));
    /* the original, as a control: its '__gc' runs in the program, where it
       can freeze */
    testobj_gcs = 0;
    owner_canfreeze = -1;
    CHECK(eval(A, "local styx = require 'styx' r = styx.unpersist(_BACK, styx.persist(_PERMS, prog)) " +
                  "return select(2, coroutine.resume(prog))").equals("done"));
    CHECK(testobj_gcs == 1 && owner_canfreeze == 1);
    /* the restored program: the same */
    owner_canfreeze = -1;
    CHECK(eval(A, "return select(2, coroutine.resume(r))").equals("done"));
    CHECK(testobj_gcs == 2 && owner_canfreeze == 1);
    lua_close(A);
  }


  /*
  ** A program run on the main thread (the host's lua_pcall at base level),
  ** frozen by its budget, persists with the whole state: from another thread
  ** of the state, as the frozen main thread's stack is the program's. It is
  ** restored into the main thread of a new state (idle and empty, restored
  ** from another thread), and lua_thaw on it goes on to the same end as the
  ** original. A restore that fails leaves that main thread idle and empty.
  */
  private static int main_calls = 0;

  private static void main_budget(lua_State user_co, lua_State spender)
  {
    if (++main_calls == 3)
      lua_freeze(spender);
  }

  private static final lua_Budget main_budget = StyxTests::main_budget;

  private static void test_frozen_main_thread()
  {
    lua_registerbudget("styxtest.main_budget", main_budget);
    String prog = "local s = 0 for i = 1, 300 do s = s + i end " +
                  "local t <close> = setmetatable({}, {__close = function() closed = 'yes' end}) " +
                  "return s, 'end'";
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, build_perms).equals("ok"));
    lua_State H = lua_newthread(A); /* (the host's other thread, made before) */
    luaL_ref(A, LUA_REGISTRYINDEX);
    main_calls = 0;
    lua_setbudget(A, 50, main_budget);
    CHECK(luaL_loadstring(A, new HostString(prog)) == LUA_OK);
    CHECK(lua_pcall(A, 0, 2, 0) == LUA_OK && lua_isfrozen(A) != 0);
    /* persisted with another thread; the frozen main thread itself refuses */
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(A, 0, w, msg) == LUA_ERRRUN);
    CHECK(msg.to_os().equals("the main thread is frozen: use another thread of the state"));
    /* a script's persist refuses it (the host persists it) */
    CHECK(eval(H, "local m = debug.getregistry()[3] " +
                  "local ok, e = pcall(require('styx').persist, _PERMS, {m}) " +
                  "return tostring(debug.isfrozen(m) and not ok and e)").equals(
          "cannot persist the main thread from a script (at root[1])"));
    lua_getglobal(H, new HostString("_PERMS"));
    int st = luaL_persiststate(H, lua_gettop(H), w, msg);
    if (st != LUA_OK)
      printf("      host persist error: %s\n", msg.to_os());
    CHECK(st == LUA_OK);
    lua_pop(H, 1);
    String blob = w.out.toString();
    /* the original goes on */
    while (lua_isfrozen(A) != 0)
      CHECK(lua_thaw(A, null).status == LUA_OK);
    CHECK(lua_gettop(A) == 2 && lua_tointeger(A, 1) == 45150 && to_string(A, 2).equals("end"));
    lua_settop(A, 0);
    CHECK(eval(A, "return closed").equals("yes"));
    /* restored in a new state, into its main thread (and a stream cut short is refused) */
    for (int attempt = 0; attempt < 2; attempt++)
    {
      CountingAlloc a2 = new CountingAlloc();
      String data = (attempt == 1) ? blob.substring(0, blob.length() - 9) : blob;
      HostStringOut rmsg = new HostStringOut();
      lua_State Bh = restore(a2, data, null, rmsg); /* (the host's other thread, restored) */
      if (attempt == 0)
      {
        if (Bh == null)
          printf("      restore error: %s\n", rmsg.to_os());
        lua_State B = Bh != null ? main_of(Bh) : null;
        CHECK(B != null && B != Bh && lua_isfrozen(Bh) == 0 && lua_isfrozen(B) != 0);
        if (B != null)
        {
          while (lua_isfrozen(B) != 0)
            CHECK(lua_thaw(B, null).status == LUA_OK);
          CHECK(lua_gettop(B) == 2 && lua_tointeger(B, 1) == 45150 && to_string(B, 2).equals("end"));
          lua_settop(B, 0);
          CHECK(eval(B, "return closed").equals("yes"));
          lua_close(B);
        }
      }
      else
        CHECK(Bh == null);
      CHECK(a2.used == 0);
    }
    lua_close(A);
  }

  /*
  ** A frozen main thread restored is as it was persisted until the host
  ** thaws it: the restore closes none of its variables, and a collection
  ** before the thaw keeps all of its stack (frozen in the program's own
  ** frame, with a to-be-closed variable; and in 'table.unpack', a C
  ** function with values of its own on the stack). The restore gives back
  ** the host's other thread, from which the state persists again: restores
  ** of restores go on as the first.
  */
  private static int rest_calls = 0;
  private static int rest_at = 0;
  private static int rest_closes = 0;

  private static void rest_budget(lua_State user_co, lua_State spender)
  {
    if (++rest_calls == rest_at)
      lua_freeze(spender);
  }

  private static final lua_Budget rest_budget = StyxTests::rest_budget;

  private static int rest_close(lua_State L)
  {
    rest_closes++;
    return 0;
  }

  private static final lua_CFunction rest_close = StyxTests::rest_close;

  private static void add_rest_close(lua_State L)
  {
    lua_pushcfunction(L, rest_close);
    lua_setglobal(L, new HostString("rest_close"));
  }

  /* the main thread of a new state, frozen at the 'at'-th budget point of
     'prog' ('other': the host's other thread of the state; 'ctop': whether
     the main thread's top frame is a C function's) */
  private static lua_State frozen_main(CountingAlloc a, String prog, int at, lua_State[] other, boolean[] ctop)
  {
    lua_State A = new_counted_state(a);
    add_rest_close(A);
    CHECK(eval(A, build_perms).equals("ok"));
    other[0] = lua_newthread(A);
    luaL_ref(A, LUA_REGISTRYINDEX);
    rest_calls = 0;
    rest_at = at;
    lua_setbudget(A, 100, rest_budget);
    CHECK(luaL_loadstring(A, new HostString(prog)) == LUA_OK);
    CHECK(lua_pcall(A, 0, 2, 0) == LUA_OK);
    ctop[0] = false;
    if (lua_isfrozen(A) != 0)
    {
      lua_Debug ar = lua_getstack(A, 0);
      lua_Debug in = ar != null ? lua_getinfo(A, new HostString("S"), ar) : null;
      ctop[0] = in != null && in.what.equals("C");
    }
    return A;
  }

  private static void test_frozen_main_thread_restored_at_rest()
  {
    lua_registerbudget("styxtest.rest_budget", rest_budget);
    String[] progs = {
      "local s = 0 for i = 1, 300 do s = s + i end " +
      "local t <close> = setmetatable({}, {__close = rest_close}) " +
      "for i = 1, 3000 do s = s + i end return s, 'end'",
      "local t = {} for i = 1, 20000 do t[i] = i end local r = table.pack(table.unpack(t)) " +
      "local same = true for i = 1, r.n do same = same and r[i] == i end return r.n, same",
    };
    for (int which = 0; which < 2; which++)
    {
      /* frozen after the variable is made (the first), in 'table.unpack' (the second) */
      lua_State A = null;
      lua_State[] other = {null};
      boolean[] ctop = {false};
      for (int at = 2; at < 400; at++)
      {
        CountingAlloc probe = new CountingAlloc();
        A = frozen_main(probe, progs[which], at, other, ctop);
        boolean want = lua_isfrozen(A) != 0 && (which == 0 ? rest_calls > 40 : ctop[0]);
        lua_close(A);
        A = null;
        if (want)
        {
          rest_at = at;
          break;
        }
      }
      CHECK(rest_at < 400);
      CountingAlloc a1 = new CountingAlloc();
      A = frozen_main(a1, progs[which], rest_at, other, ctop);
      CHECK(lua_isfrozen(A) != 0);
      /* persisted from the host's other thread */
      StringWriter w = new StringWriter();
      HostStringOut msg = new HostStringOut();
      lua_getglobal(other[0], new HostString("_PERMS"));
      CHECK(luaL_persiststate(other[0], lua_gettop(other[0]), w, msg) == LUA_OK);
      lua_pop(other[0], 1);
      lua_close(A);
      /* restored, collected, persisted again from the thread the restore
         gives back (the host's other thread), three times over */
      rest_closes = 0;
      Setup s = new Setup(StyxTests::add_rest_close, null);
      String blob = w.out.toString();
      CountingAlloc[] a2 = {new CountingAlloc(), new CountingAlloc(), new CountingAlloc()};
      lua_State Bh = null;
      for (int round = 0; round < 3; round++)
      {
        if (Bh != null)
        {
          StringWriter w2 = new StringWriter();
          lua_getglobal(Bh, new HostString("_PERMS"));
          CHECK(luaL_persiststate(Bh, lua_gettop(Bh), w2, msg) == LUA_OK);
          lua_pop(Bh, 1);
          lua_close(Bh);
          blob = w2.out.toString();
        }
        Bh = restored_state(a2[round], blob, s);
        CHECK(Bh != main_of(Bh) && lua_isfrozen(Bh) == 0 && lua_isfrozen(main_of(Bh)) != 0);
        CHECK(rest_closes == 0); /* (the restore closed nothing) */
        lua_gc(Bh, LUA_GCCOLLECT, 0, 0, 0);
        lua_gc(Bh, LUA_GCCOLLECT, 0, 0, 0);
      }
      lua_State B = main_of(Bh);
      lua_ResumeResult r = new lua_ResumeResult(LUA_OK, 0);
      while (r.status == LUA_OK && lua_isfrozen(B) != 0)
        r = lua_thaw(B, null);
      CHECK(r.status == LUA_OK && lua_gettop(B) == 2);
      if (which == 0)
        CHECK(lua_tointeger(B, 1) == 45150 + 4501500 && to_string(B, 2).equals("end") && rest_closes == 1);
      else
        CHECK(lua_tointeger(B, 1) == 20000 && lua_toboolean(B, 2) != 0);
      lua_close(B);
    }
  }

  /*
  ** A program on the main thread that failed, frozen in a '__close' as its
  ** host call closes its variables (FZ_HOSTCLOSE): persisted and restored,
  ** the thaw finishes the close and gives the error, in both states.
  */
  private static void test_frozen_main_thread_close()
  {
    lua_registerbudget("styxtest.main_budget", main_budget);
    String prog = "local x <close> = setmetatable({}, {__close = function(_, e) " +
                  "  local s = 0 for i = 1, 300 do s = s + i end closed = tostring(e) .. ' ' .. s end}) " +
                  "error('boom', 0)";
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, build_perms).equals("ok"));
    lua_State H = lua_newthread(A);
    luaL_ref(A, LUA_REGISTRYINDEX);
    main_calls = 0;
    lua_setbudget(A, 50, main_budget);
    CHECK(luaL_loadstring(A, new HostString(prog)) == LUA_OK);
    CHECK(lua_pcall(A, 0, 0, 0) == LUA_OK && lua_isfrozen(A) != 0);
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    lua_getglobal(H, new HostString("_PERMS"));
    CHECK(luaL_persiststate(H, lua_gettop(H), w, msg) == LUA_OK);
    lua_pop(H, 1);
    String blob = w.out.toString();
    lua_ResumeResult r = new lua_ResumeResult(LUA_OK, 0);
    while (lua_isfrozen(A) != 0)
      r = lua_thaw(A, null);
    CHECK(r.status == LUA_ERRRUN && to_string(A, -1).equals("boom"));
    lua_settop(A, 0);
    CHECK(eval(A, "return closed").equals("boom 45150"));
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = main_of(restored_state(a2, blob));
    CHECK(lua_isfrozen(B) != 0);
    r = new lua_ResumeResult(LUA_OK, 0);
    while (lua_isfrozen(B) != 0)
      r = lua_thaw(B, null);
    CHECK(r.status == LUA_ERRRUN && to_string(B, -1).equals("boom"));
    lua_settop(B, 0);
    CHECK(eval(B, "return closed").equals("boom 45150"));
    lua_close(B);
    CHECK(a2.used == 0);
    lua_close(A);
  }

  /*
  ** A stream with bytes changed or cut short is not trusted to be valid, but
  ** a restore of it must end: with an error, or with a state that closes
  ** cleanly. No crash, no hang, no leak (the counting allocator's balance).
  */
  private static void test_frozen_corrupted_streams()
  {
    lua_registerk("styxtest.host_k", host_k);
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = host_state(a1);
    CHECK(eval(A, host_program).equals("frozen"));
    CHECK(eval(A, "other = coroutine.wrap(function() local t <close> = setmetatable({}, {__close = function() end}) coroutine.yield(1) end) other() " +
                  "return 'ok'").equals("ok"));
    String blob = persist_state(A);
    CHECK(!blob.isEmpty());
    lua_close(A);
    int stride = os_getenv(ByteView.of("STYX_CORRUPT_ALL")) != null ? 1 : blob.length() / 300 + 1;
    int rejected = 0, accepted = 0;
    for (int at = 0; at < blob.length(); at += stride)
    {
      for (int x : new int[] {0x01, 0x80, 0xff})
      {
        char[] bad = blob.toCharArray();
        bad[at] = (char) ((bad[at] & 0xFF) ^ x);
        CountingAlloc a2 = new CountingAlloc();
        Setup s = new Setup(StyxTests::add_callk, null);
        HostStringOut msg = new HostStringOut();
        lua_State B = restore(a2, new String(bad), s, msg);
        if (B != null)
        {
          accepted++;
          lua_close(B);
        }
        else
          rejected++;
        CHECK(a2.used == 0);
      }
    }
    for (int len = 0; len < blob.length(); len += stride * 3)
    {
      String cut = blob.substring(0, len);
      CountingAlloc a2 = new CountingAlloc();
      Setup s = new Setup(StyxTests::add_callk, null);
      HostStringOut msg = new HostStringOut();
      lua_State B = restore(a2, cut, s, msg);
      CHECK(B == null);
      if (B != null)
        lua_close(B);
      CHECK(a2.used == 0);
    }
    CHECK(rejected > 0);
    printf("      corrupted: %d rejected, %d accepted\n", rejected, accepted);
  }

  /*
  ** The kernel's persistence, as a host does it: a kernel coroutine runs the
  ** user programs, each with its budget, freezes them in its callback or
  ** lets them suspend, and yields back to the host; the host persists the
  ** whole state there, and restores it with luaL_restorestate into a new
  ** state (which hashes as the original: no seed to give), where it resumes
  ** the kernel from its anchor in the registry. Both go on exactly as a run
  ** with no persist: tables with churn iterate in their order, a load and a
  ** match stopped in their middle go on.
  */
  private static final String kernel_script = """

local function user1()
  local t = {}
  for i = 1, 120 do t["k" .. (i * 7 % 200)] = i end
  for k, v in pairs(t) do if v % 3 == 0 then t[k] = nil end end
  for k, v in pairs(t) do coroutine.yield(k .. "=" .. v) end
  return "u1 done"
end
local function user2()
  local o, names = {}, {}
  for i = 1, 60 do
    local k = (i % 2 == 0) and {} or function() return i end
    o[k], names[k] = i, "o" .. i
  end
  for k, v in pairs(o) do if v % 5 == 0 then o[k] = nil end end
  local n = 0
  local function inc() n = n + 1 return n end
  for k in pairs(o) do inc() coroutine.yield(names[k]) end
  local s = string.rep("ab", 50):gsub("b", function(c) return c:upper() end)
  coroutine.yield(s:sub(1, 20) .. "#" .. n)
  return "u2 done"
end
local function user3()
  local src = "local a = {} for i = 1, 30 do a[i] = (i * 13) % 31 end return a"
  local pos = 0
  local f = load(function()
    pos = pos + 5
    if pos - 5 >= #src then return nil end
    return src:sub(pos - 4, pos)
  end)
  local a = f()
  table.sort(a, function(x, y) return x > y end)
  coroutine.yield(table.concat(a, ","))
  coroutine.yield(tostring(("hello world from styx " .. string.rep("w", 40)):find("w(o)r")))
  return "u3 done"
end
local function user4()
  local t, names = {}, {}
  for name, f in pairs{print = print, type = type, pairs = pairs, format = string.format, insert = table.insert,
                       floor = math.floor, yield = coroutine.yield, next = next, find = string.find} do
    t[f], names[f] = true, name
  end
  for f in pairs(t) do coroutine.yield(names[f]) end
  return "u4 done"
end
local kernel = coroutine.create(function()
  local users = {}
  for u, f in ipairs{user1, user2, user3, user4} do
    local co = coroutine.create(f)
    debug.setbudget(co, 40 + u * 7, function() debug.freeze() end)
    users[u] = {co = co, out = {}}
  end
  local tick = 0
  repeat
    local alive = false
    for _, user in ipairs(users) do
      local co = user.co
      if coroutine.status(co) ~= "dead" then
        alive = true
        local ok, r
        if debug.isfrozen(co) then ok, r = debug.thaw(co) else ok, r = coroutine.resume(co) end
        if not ok then
          user.out[#user.out + 1] = "error:" .. tostring(r)
        elseif not debug.isfrozen(co) then
          user.out[#user.out + 1] = tostring(r)
        end
      end
    end
    tick = tick + 1
    coroutine.yield(tick)
  until not alive
  local parts = {}
  for u, user in ipairs(users) do parts[u] = table.concat(user.out, ",") end
  return table.concat(parts, " | ")
end)
debug.getregistry().kernel = kernel -- (the host's anchor)
""";

  /* the host's setup of a state to restore into: the same libraries and permanents */
  private static void kernel_setup(lua_State L)
  {
    luaL_openlibs(L);
    preload_styx(L);
    eval(L, build_perms);
    lua_getglobal(L, new HostString("_BACK"));
  }

  private static lua_State kernel_state(CountingAlloc a, int seed)
  {
    lua_State L = lua_newstate(a, seed);
    luaL_openlibs(L);
    preload_styx(L);
    CHECK(eval(L, build_perms).equals("ok"));
    eval(L, kernel_script);
    return L;
  }

  /* the host resumes the kernel once: true when it ended (its transcript) */
  private static boolean kernel_step(lua_State L, String[] transcript)
  {
    lua_getfield(L, LUA_REGISTRYINDEX, new HostString("kernel"));
    lua_State K = lua_tothread(L, -1);
    lua_pop(L, 1);
    lua_ResumeResult r = lua_resume(K, L, 0);
    if (r.status == LUA_YIELD)
    {
      lua_pop(K, r.nres);
      return false;
    }
    CHECK(r.status == LUA_OK);
    transcript[0] = (r.status == LUA_OK ? "" : "ERR ") + to_string(K, -1);
    lua_pop(K, r.nres);
    return true;
  }

  private static String persist_kernel(lua_State L)
  {
    lua_getglobal(L, new HostString("_PERMS"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    int st = luaL_persiststate(L, lua_gettop(L), w, msg);
    if (st != LUA_OK)
      printf("      host persist error: %s\n", msg.to_os());
    CHECK(st == LUA_OK);
    lua_pop(L, 1);
    return w.out.toString();
  }

  private static void test_kernel_persistence()
  {
    String[] plain = {""};
    {
      CountingAlloc a = new CountingAlloc();
      lua_State L = kernel_state(a, 777);
      while (!kernel_step(L, plain))
      {
      }
      lua_close(L);
    }
    CHECK(plain[0].contains("u1 done") && plain[0].contains("u3 done"));
    int persisted = 0;
    for (int k = 1; k < 10000; k++)
    {
      CountingAlloc a1 = new CountingAlloc();
      lua_State A = kernel_state(a1, 777);
      String[] ta = {""}, tb = {""};
      boolean done = false;
      for (int i = 0; i < k && !done; i++)
        done = kernel_step(A, ta);
      if (done)
      {
        lua_close(A);
        break;
      }
      persisted++;
      String blob = persist_kernel(A);
      CountingAlloc a2 = new CountingAlloc();
      HostStringOut msg = new HostStringOut();
      lua_State B = restorestate(a2, view(blob), StyxTests::kernel_setup, msg);
      CHECK(B != null);
      if (B == null)
      {
        printf("      restore error at tick %d: %s\n", k, msg.to_os());
        lua_close(A);
        break;
      }
      while (!kernel_step(A, ta))
      {
      }
      while (!kernel_step(B, tb))
      {
      }
      if (!ta[0].equals(plain[0]) || !tb[0].equals(plain[0]))
        printf("      tick %d:\n        plain    %s\n        original %s\n        restored %s\n", k, plain[0],
               ta[0], tb[0]);
      CHECK(ta[0].equals(plain[0]));
      CHECK(tb[0].equals(plain[0]));
      lua_close(A);
      lua_close(B);
    }
    CHECK(persisted > 10);
  }

  /*
  ** The kernel's state restored in another process (its own addresses, the
  ** same library): the host persists it to a file and the child restores it
  ** with luaL_restorestate and runs the kernel to its end; a table keyed by
  ** library functions keeps its order there (they hash by name).
  */
  private static String g_self = "../../java/test_styx";

  /* the child: restores the kernel's state in 'file' and prints its transcript */
  private static int restore_kernel_child(String file)
  {
    OsFile f = os_fopen(new HostString(file).to_utf8(), ByteView.of("rb")).f;
    if (f == null)
      return 2;
    StringBuilder blob = new StringBuilder();
    ByteOwner buf = new ByteOwner(4096);
    int n;
    while ((n = os_fread(f, buf.span(), 0, buf.size())) > 0)
      blob.append(str(new ByteView(buf.array(), 0, n)));
    os_fclose(f);
    CountingAlloc a = new CountingAlloc();
    HostStringOut msg = new HostStringOut();
    lua_State L = restorestate(a, view(blob), StyxTests::kernel_setup, msg);
    if (L == null)
    {
      printf("restore error: %s", msg.to_os());
      return 1;
    }
    String[] t = {""};
    while (!kernel_step(L, t))
    {
    }
    printf("%s", t[0]);
    lua_close(L);
    return 0;
  }

  /* (std::filesystem::temp_directory_path(), as libstdc++ finds it) */
  private static Path temp_directory_path()
  {
    for (String e : new String[] {"TMPDIR", "TMP", "TEMP", "TEMPDIR"})
    {
      ByteView v = os_getenv(ByteView.of(e));
      if (v != null)
        return Path.of(new HostString(v).to_os());
    }
    return Path.of("/tmp");
  }

  private static void test_kernel_in_another_process()
  {
    String[] plain = {""};
    {
      CountingAlloc a = new CountingAlloc();
      lua_State L = kernel_state(a, 4242);
      while (!kernel_step(L, plain))
      {
      }
      lua_close(L);
    }
    CHECK(plain[0].contains("u4 done"));
    Path dir = temp_directory_path();
    for (int k : new int[] {1, 3, 8})
    {
      CountingAlloc a = new CountingAlloc();
      lua_State A = kernel_state(a, 4242);
      String[] t = {""};
      for (int i = 0; i < k; i++)
        kernel_step(A, t);
      String blob = persist_kernel(A);
      lua_close(A);
      String file = dir.resolve("styx_kernel_" + k + ".bin").toString();
      String out = dir.resolve("styx_kernel_" + k + ".txt").toString();
      OsFile f = os_fopen(new HostString(file).to_utf8(), ByteView.of("wb")).f;
      CHECK(f != null);
      if (f == null)
        continue;
      os_fwrite(f, view(blob));
      os_fclose(f);
      String cmd = g_self + " --restore-kernel " + file + " > " + out;
      CHECK(os_system(new HostString(cmd).to_utf8()).stat == 0);
      String got = "";
      OsFile o = os_fopen(new HostString(out).to_utf8(), ByteView.of("rb")).f;
      if (o != null)
      {
        ByteOwner buf = new ByteOwner(4096);
        int n;
        while ((n = os_fread(o, buf.span(), 0, buf.size())) > 0)
          got += str(new ByteView(buf.array(), 0, n));
        os_fclose(o);
      }
      if (!got.equals(plain[0]))
        printf("      tick %d in another process:\n        plain %s\n        got   %s\n", k, plain[0],
               got);
      CHECK(got.equals(plain[0]));
      os_remove(new HostString(file).to_utf8());
      os_remove(new HostString(out).to_utf8());
    }
  }

  private static void push_one(lua_State L)
  {
    luaL_openlibs(L);
    preload_styx(L);
    lua_pushinteger(L, 1);
  }

  private static void raise_error(lua_State L)
  {
    lua_pushstring(L, new HostString("no setup here"));
    lua_error(L);
  }

  /*
  ** A thread whose frames reach the stack's limit (a C function that checked
  ** the stack for that many values: here 'coroutine.yield' called with as
  ** many values as fit) is persisted and restored, and goes on
  */
  private static void test_thread_at_stack_limit()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, "big = {} for i = 1, 1100000 do big[i] = i end " +
                  "local n = 1 while pcall(table.unpack, big, 1, n * 2) do n = n * 2 end " +
                  "local hi = n * 2 while hi - n > 1 do local m = (n + hi) // 2 " +
                  "  if pcall(table.unpack, big, 1, m) then n = m else hi = m end end " +
                  "for k = n, 1, -1 do " +
                  "  co = coroutine.create(function() return select('#', coroutine.yield(table.unpack(big, 1, k))) end) " +
                  "  coroutine.resume(co) " +
                  "  if coroutine.status(co) == 'suspended' then big = nil return 'ok' end " +
                  "end return 'none'").equals("ok"));
    String blob = persist_state(A);
    CHECK(!blob.isEmpty());
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    CHECK(eval(B, "return tostring(select(2, coroutine.resume(co, 1, 2, 3)))").equals("3"));
    CHECK(eval(A, "return tostring(select(2, coroutine.resume(co, 1, 2, 3)))").equals("3"));
    lua_close(B);
    lua_close(A);
  }

  /*
  ** A restore ends with a whole cycle of the collector, unless the program
  ** stopped it: what was garbage when the state was persisted (an object
  ** waiting for its '__gc', a dead entry of a weak table) goes then, so that
  ** a program that waits on its collector goes on even when the host
  ** restores more often than a cycle takes (a new state's first cycle starts
  ** only once its heap has grown by about its size)
  */
  private static void test_restore_collects()
  {
    String prog = "live = {} for i = 1, 200000 do live[i] = {} end collectgarbage() " +
                  "done = false setmetatable({}, {__gc = function() done = true end}) " +
                  "weak = setmetatable({}, {__mode = 'k'}) weak[{}] = 1 return 'ok'";
    for (int stop = 0; stop < 2; stop++)
    {
      CountingAlloc a1 = new CountingAlloc();
      lua_State A = new_counted_state(a1);
      CHECK(eval(A, build_perms).equals("ok"));
      CHECK(eval(A, prog).equals("ok"));
      if (stop != 0)
        CHECK(eval(A, "collectgarbage('stop') return 'ok'").equals("ok"));
      String blob = persist_state(A);
      CHECK(eval(A, "return tostring(done) .. ' ' .. tostring(next(weak) ~= nil)").equals("false true"));
      lua_close(A);
      CountingAlloc a2 = new CountingAlloc();
      lua_State B = restored_state(a2, blob);
      CHECK(eval(B, "return tostring(done) .. ' ' .. tostring(next(weak) ~= nil)").equals(
            (stop != 0 ? "false true" : "true false")));
      lua_close(B);
    }
  }

  /*
  ** A state is persisted from its main thread or from a thread it holds, and
  ** the restore gives that thread back
  */
  private static void test_persist_from_held_thread()
  {
    CountingAlloc a = new CountingAlloc();
    lua_State L = new_counted_state(a);
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(eval(L, build_perms).equals("ok"));
    lua_State loose = lua_newthread(L);
    lua_State held = lua_newthread(L);
    lua_setglobal(L, new HostString("held"));
    lua_getglobal(loose, new HostString("_PERMS"));
    CHECK(luaL_persiststate(loose, lua_gettop(loose), w, msg) == LUA_ERRRUN);
    CHECK(msg.to_os().equals("persist from the main thread or from a thread the state holds"));
    lua_getglobal(held, new HostString("_PERMS"));
    w.out.setLength(0);
    CHECK(luaL_persiststate(held, lua_gettop(held), w, msg) == LUA_OK);
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, w.out.toString());
    CHECK(B != main_of(B) && eval(B, "return tostring(held == select(1, coroutine.running()))").equals("true"));
    lua_close(B);
    lua_close(L);
  }

  /*
  ** A light userdata is persisted only as a permanent: one with the address
  ** of a variable written before (debug.upvalueid) is refused, not written as
  ** a reference to the variable (which the restore rejected)
  */
  private static void test_upvalue_id_not_persisted()
  {
    CountingAlloc a = new CountingAlloc();
    lua_State L = new_counted_state(a);
    CHECK(eval(L, build_perms).equals("ok"));
    CHECK(eval(L, "local x = 1 local function f() return x end " +
                  "t = {f, debug.upvalueid(f, 1)} return 'ok'").equals("ok"));
    CHECK(eval(L, "local ok, msg = debug.persistable(t) return tostring(ok) .. ' ' .. msg").equals(
          "false persisting a userdata value is not supported yet (at root[2])"));
    lua_getglobal(L, new HostString("_PERMS"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(L, lua_gettop(L), w, msg) == LUA_ERRRUN);
    CHECK(msg.to_os().contains("persisting a userdata value is not supported yet"));
    lua_close(L);
  }

  /*
  ** A variable shared by two closures, open in a coroutine, where the walk
  ** meets one closure first (a global) and the other in the coroutine's
  ** stack, which that first closure's variable writes: both share it after
  ** the restore (before, the second one was joined to the first one's
  ** variable before it was made, and kept a variable of its own, nil)
  */
  private static void test_open_variable_shared_in_its_thread()
  {
    String prog = "local co co = coroutine.create(function() " +
                  "  local x = 41 local function b() return x end " +
                  "  a = function() x = x + 1 return x, co end " +
                  "  coroutine.yield() return b() end) " +
                  "coroutine.resume(co) return 'ok'";
    String use = "local n, co = a() local ok, r = coroutine.resume(co) return n .. ' ' .. tostring(r)";
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = new_counted_state(a1);
    CHECK(eval(A, build_perms).equals("ok"));
    CHECK(eval(A, prog).equals("ok"));
    String blob = persist_state(A);
    CHECK(eval(A, use).equals("42 42"));
    lua_close(A);
    CountingAlloc a2 = new CountingAlloc();
    lua_State B = restored_state(a2, blob);
    CHECK(eval(B, use).equals("42 42"));
    lua_close(B);
  }

  /*
  ** A gmatch iterator whose strings were replaced (debug.setupvalue) cannot be
  ** kept as it is: the persist refuses it (before, it wrote it, and the
  ** restore refused the stream)
  */
  private static void test_changed_gmatch_not_persisted()
  {
    CountingAlloc a = new CountingAlloc();
    lua_State L = new_counted_state(a);
    CHECK(eval(L, build_perms).equals("ok"));
    CHECK(eval(L, "local it = string.gmatch('abc', '%a') debug.setupvalue(it, 1, {}) t = {it} return 'ok'").equals("ok"));
    CHECK(eval(L, "local ok, msg = debug.persistable(t) return tostring(ok) .. ' ' .. msg").equals(
          "false cannot persist a string.gmatch_aux closure as it is (its upvalues were changed) (at root[1])"));
    lua_getglobal(L, new HostString("_PERMS"));
    StringWriter w = new StringWriter();
    HostStringOut msg = new HostStringOut();
    CHECK(luaL_persiststate(L, lua_gettop(L), w, msg) == LUA_ERRRUN);
    CHECK(msg.to_os().contains("cannot persist a string.gmatch_aux closure as it is"));
    lua_close(L);
  }

  /* luaL_restorestate fails as a whole: a message, and nothing left allocated */
  private static void test_restore_errors()
  {
    CountingAlloc a1 = new CountingAlloc();
    lua_State A = kernel_state(a1, 99);
    String[] t = {""};
    kernel_step(A, t);
    String blob = persist_kernel(A);
    lua_close(A);
    final class Bad
    {
      String data;
      Consumer<lua_State> setup;
      String msg;

      Bad(String data, Consumer<lua_State> setup, String msg)
      {
        this.data = data;
        this.setup = setup;
        this.msg = msg;
      }
    }
    Bad[] bad = {
      new Bad("not a stream", StyxTests::kernel_setup, "not a persisted state"),
      new Bad(blob, StyxTests::push_one, "the setup must leave the permanents table (key -> object) or nil"),
      new Bad(blob, StyxTests::raise_error, "no setup here"),
      new Bad(blob.substring(0, blob.length() / 2), StyxTests::kernel_setup, "invalid persisted data"),
    };
    for (Bad b : bad)
    {
      CountingAlloc a = new CountingAlloc();
      HostStringOut msg = new HostStringOut();
      lua_State L = restorestate(a, view(b.data), b.setup, msg);
      CHECK(L == null);
      CHECK(msg.to_os().startsWith(b.msg));
      CHECK(a.used == 0);
      if (L != null)
        lua_close(L);
    }
  }

  private static void run_cpp_tests()
  {
    final class TestEntry
    {
      String name;
      Runnable fn;

      TestEntry(String name, Runnable fn)
      {
        this.name = name;
        this.fn = fn;
      }
    }
    final TestEntry[] tests = {
        new TestEntry("no_script_library", StyxTests::test_no_script_library),
        new TestEntry("api_surface", StyxTests::test_api_surface),
        new TestEntry("primitives_roundtrip", StyxTests::test_primitives_roundtrip),
        new TestEntry("string_bytes", StyxTests::test_string_bytes),
        new TestEntry("stack_balance", StyxTests::test_stack_balance),
        new TestEntry("thread_across_states", StyxTests::test_thread_across_states),
        new TestEntry("tables_across_states", StyxTests::test_tables_across_states),
        new TestEntry("closures_across_states", StyxTests::test_closures_across_states),
        new TestEntry("truncation_from_cpp", StyxTests::test_truncation_from_cpp),
        new TestEntry("light_userdata", StyxTests::test_light_userdata),
        new TestEntry("host_value_matches_script", StyxTests::test_host_value_matches_script),
        new TestEntry("host_persist_allocates_nothing", StyxTests::test_host_persist_allocates_nothing),
        new TestEntry("host_persist_at_memory_cap", StyxTests::test_host_persist_at_memory_cap),
        new TestEntry("host_persist_runs_no_lua", StyxTests::test_host_persist_runs_no_lua),
        new TestEntry("host_persist_errors", StyxTests::test_host_persist_errors),
        new TestEntry("host_persist_deep", StyxTests::test_host_persist_deep),
        new TestEntry("host_persist_writer_stops", StyxTests::test_host_persist_writer_stops),
        new TestEntry("host_persist_running_state", StyxTests::test_host_persist_running_state),
        new TestEntry("host_persist_state_stream", StyxTests::test_host_persist_state_stream),
        new TestEntry("host_persists_child_thread", StyxTests::test_host_persists_child_thread),
        new TestEntry("host_state_roundtrip", StyxTests::test_host_state_roundtrip),
        new TestEntry("host_restore_of_capped_state", StyxTests::test_host_restore_of_capped_state),
        new TestEntry("host_restore_out_of_memory", StyxTests::test_host_restore_out_of_memory),
        new TestEntry("host_value_restore", StyxTests::test_host_value_restore),
        new TestEntry("host_stream_kinds", StyxTests::test_host_stream_kinds),
        new TestEntry("external_hook_not_supported", StyxTests::test_external_hook_not_supported),
        new TestEntry("table_order_across_states", StyxTests::test_table_order_across_states),
        new TestEntry("host_state_marked_garbage", StyxTests::test_host_state_marked_garbage),
        new TestEntry("host_state_pending_finalizers", StyxTests::test_host_state_pending_finalizers),
        new TestEntry("host_state_collector_settings", StyxTests::test_host_state_collector_settings),
        new TestEntry("host_state_garbage_userdata", StyxTests::test_host_state_garbage_userdata),
        new TestEntry("dead_keys_across_states", StyxTests::test_dead_keys_across_states),
        new TestEntry("object_key_order_across_states", StyxTests::test_object_key_order_across_states),
        new TestEntry("library_keys_across_states", StyxTests::test_library_keys_across_states),
        new TestEntry("state_next_hash", StyxTests::test_state_next_hash),
        new TestEntry("restore_takes_the_seed", StyxTests::test_restore_takes_the_seed),
        new TestEntry("snapshot_continues_identically", StyxTests::test_snapshot_continues_identically),
        new TestEntry("frozen_library_cases", StyxTests::test_frozen_library_cases),
        new TestEntry("frozen_engine_cases", StyxTests::test_frozen_engine_cases),
        new TestEntry("frozen_deferred_gc", StyxTests::test_frozen_deferred_gc),
        new TestEntry("frozen_host_called_thread", StyxTests::test_frozen_host_called_thread),
        new TestEntry("frozen_host_continuation", StyxTests::test_frozen_host_continuation),
        new TestEntry("frozen_host_budget_callback", StyxTests::test_frozen_host_budget_callback),
        new TestEntry("frozen_refusals_not_persisted", StyxTests::test_frozen_refusals_not_persisted),
        new TestEntry("frozen_restore_out_of_memory", StyxTests::test_frozen_restore_out_of_memory),
        new TestEntry("frozen_allocation_failures", StyxTests::test_frozen_allocation_failures),
        new TestEntry("user_object_kind", StyxTests::test_user_object_kind),
        new TestEntry("user_object_owner", StyxTests::test_user_object_owner),
        new TestEntry("frozen_main_thread", StyxTests::test_frozen_main_thread),
        new TestEntry("frozen_main_thread_restored_at_rest", StyxTests::test_frozen_main_thread_restored_at_rest),
        new TestEntry("frozen_main_thread_close", StyxTests::test_frozen_main_thread_close),
        new TestEntry("frozen_corrupted_streams", StyxTests::test_frozen_corrupted_streams),
        new TestEntry("kernel_persistence", StyxTests::test_kernel_persistence),
        new TestEntry("kernel_in_another_process", StyxTests::test_kernel_in_another_process),
        new TestEntry("thread_at_stack_limit", StyxTests::test_thread_at_stack_limit),
        new TestEntry("restore_collects", StyxTests::test_restore_collects),
        new TestEntry("persist_from_held_thread", StyxTests::test_persist_from_held_thread),
        new TestEntry("upvalue_id_not_persisted", StyxTests::test_upvalue_id_not_persisted),
        new TestEntry("open_variable_shared_in_its_thread", StyxTests::test_open_variable_shared_in_its_thread),
        new TestEntry("changed_gmatch_not_persisted", StyxTests::test_changed_gmatch_not_persisted),
        new TestEntry("restore_errors", StyxTests::test_restore_errors),
    };
    printf("  styx_tests.cpp\n");
    ByteView only = os_getenv(ByteView.of("STYX_ONLY")); /* (to run one C++ test, under valgrind: no scripts then) */
    for (TestEntry t : tests)
    {
      if (only != null && !only.equals(ByteView.of(t.name)))
        continue;
      g_current = t.name;
      g_test_failures = 0;
      t.fn.run();
      if (g_test_failures == 0)
        printf("    %s ... OK\n", t.name);
      else
        printf("    %s ... FAIL\n", t.name);
    }
  }

  /* GC mode a script runs under: name and the code that selects it */
  private static final class GcMode
  {
    String name;
    String setup;

    GcMode(String name, String setup)
    {
      this.name = name;
      this.setup = setup;
    }
  }

  private static final GcMode[] gc_modes = {
      new GcMode("incremental", "collectgarbage('incremental')"),
      new GcMode("generational", "collectgarbage('generational')"),
      new GcMode("constant", "collectgarbage('incremental'); collectgarbage('param', 'pause', 0);" +
                             "collectgarbage('param', 'stepsize', 64)"),
  };

  /* runs one script in a fresh state; returns false if it did not complete */
  private static boolean run_script(Path path, GcMode mode,
                                    boolean verbose, int[] cases, int[] failures)
  {
    lua_State L = new_state();
    eval(L, "package.path = './support/?.lua;' .. package.path");
    lua_pushstring(L, new HostString(mode.name));
    lua_setglobal(L, new HostString("STYX_MODE"));
    lua_pushboolean(L, verbose ? 1 : 0);
    lua_setglobal(L, new HostString("STYX_VERBOSE"));
    boolean ok = luaL_dostring(L, new HostString(mode.setup)) == LUA_OK && luaL_dofile(L, new HostString(path.toString())) == LUA_OK;
    if (!ok)
      printf("    FAIL [%s] script error: %s\n", mode.name, to_string(L, -1));
    if (lua_getglobal(L, new HostString("STYX_RESULT")) == LUA_TTABLE)
    {
      lua_getfield(L, -1, new HostString("cases"));
      lua_getfield(L, -2, new HostString("failures"));
      cases[0] = (int) lua_tointeger(L, -2);
      failures[0] = (int) lua_tointeger(L, -1);
    }
    lua_close(L);
    return ok;
  }

  private static void run_scripts()
  {
    ArrayList<Path> scripts = new ArrayList<>();
    try (DirectoryStream<Path> ds = Files.newDirectoryStream(Path.of(".")))
    {
      for (Path e : ds)
      {
        String fn = e.getFileName().toString();
        int dot = fn.lastIndexOf('.');
        if (dot > 0 && fn.substring(dot).equals(".lua"))
          scripts.add(e);
      }
    }
    catch (IOException e)
    {
      throw new UncheckedIOException(e);
    }
    scripts.sort(null);
    for (Path s : scripts)
    {
      printf("  %s\n", s.getFileName().toString());
      boolean first = true;
      for (GcMode mode : gc_modes)
      {
        int[] cases = {0}, failures = {0};
        boolean ok = run_script(s, mode, first, cases, failures);
        g_checks += cases[0] + 1;
        g_failures += failures[0] + (ok ? 0 : 1);
        if (!first && ok && failures[0] == 0)
          printf("    (%s GC: %d cases OK)\n", mode.name, cases[0]);
        first = false;
      }
    }
  }

  public static void main(String[] args)
  {
    if (args.length == 2 && args[0].equals("--restore-kernel"))
    {
      exit(restore_kernel_child(args[1]));
      return;
    }
    String self = System.getProperty("styx.self");
    if (self != null)
      g_self = self;
    printf("\n\tStarting styx tests\n\n");
    run_cpp_tests();
    if (os_getenv(ByteView.of("STYX_ONLY")) == null)
      run_scripts();
    if (g_failures == 0)
      printf("\n\tOK - styx: %d checks, 0 failures\n\n", g_checks);
    else
      printf("\n\tFAILED - styx: %d checks, %d failures\n\n", g_checks, g_failures);
    exit(g_failures == 0 ? 0 : 1);
  }
}
