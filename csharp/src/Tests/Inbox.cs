using System;
using System.Collections.Concurrent;
using System.Threading;
using Payonel.LungoLua.Api;
using Payonel.LungoLua.Compat;
using lua_State = Payonel.LungoLua.Runtime.lua_State;
using static Payonel.LungoLua.Api.LuaApi;
using static Payonel.LungoLua.Api.LuaAuxLib;
using static Payonel.LungoLua.Lib.Linit;
using static Payonel.LungoLua.Runtime.Ldo;
using static Payonel.LungoLua.Runtime.Lstate;
using static Payonel.LungoLua.Vm.Ldebug;

namespace Payonel.LungoLua.Tests;

/*
** Host data reaching Lua code that never yields, through freeze and thaw:
** ModularComputers' docs/examples/InboxExample.java (its manual's "inbox"
** page) on the C# engine, with checks, and the same with thaw stops, the
** form of the mod's input interrupts.
**
** 1. Inbox. A budget freezes the program every STEPS steps; while it is
**    frozen no Lua runs, so the host moves what a producer on another
**    thread queued into a table the program reads, then thaws it: the
**    program finds each message at its first look after that thaw.
** 2. Interrupts. The budgeted program is a scheduler ('init') running a
**    child that never yields and never looks at the inbox. The host thaws
**    with a stop above the child only when it delivered data: the
**    scheduler's resume of the child returns at once (the child stays
**    frozen), the scheduler takes the message, and thaws the child to go
**    on. Without data the host thaws plainly and the scheduler does not
**    wake: the child is preempted for the interrupt, never asked to yield.
*/
public static class Inbox
{
  private const long STEPS = 10_000;
  private const int MESSAGES = 5;
  private const int PRODUCER_PAUSE_MS = 50;

  private static int checks = 0;
  private static int failures = 0;

  private static void CHECK(bool ok, string what)
  {
    checks++;
    if (!ok)
    {
      failures++;
      Console.WriteLine("FAIL: " + what);
    }
  }

  private const string PROGRAM = @"
    local inbox = ...
    local head = 1
    local spins = 0
    local ok = pcall(function()                -- a freeze is not an error: pcall never sees it
      while head <= MESSAGES do
        local message = inbox[head]
        if message ~= nil then
          inbox[head] = nil
          head = head + 1
        end
        spins = spins + 1
      end
    end)
    assert(ok)
    return spins
  ";

  private const string SCHEDULER = @"
    local inbox, log = ...
    local work = 0
    local child = coroutine.create(function()
      pcall(function() while true do work = work + 1 end end) -- never yields, never reads the inbox
    end)
    log.child = child
    local head = 1
    while head <= MESSAGES do
      -- runs the child; returns only when a thaw stops above it
      local ok = (debug.isfrozen(child) and debug.thaw or coroutine.resume)(child)
      assert(ok and debug.isfrozen(child))
      local message = inbox[head]
      assert(message ~= nil, 'woken with no message')
      inbox[head] = nil
      log[head] = message
      log.work = work
      head = head + 1
    end
    return head - 1
  ";

  private static readonly lua_Budget FREEZE = (userCo, spender) => { throw lua_freeze(spender); };

  /* a producer on another thread: MESSAGES strings, PRODUCER_PAUSE_MS apart */
  private static Thread producer(ConcurrentQueue<string> outbox)
  {
    Thread t = new Thread(() =>
    {
      for (int i = 1; i <= MESSAGES; i++)
      {
        Thread.Sleep(PRODUCER_PAUSE_MS);
        outbox.Enqueue("reading " + i);
      }
    });
    t.Start();
    return t;
  }

  private static lua_State program(lua_State L, string code, int extra)
  {
    lua_State co = lua_newthread(L);
    lua_setfield(L, LUA_REGISTRYINDEX, new HostString("program"));
    int st = luaL_loadstring(co, new HostString(code.Replace("MESSAGES", MESSAGES.ToString())));
    CHECK(st == LUA_OK, "load");
    lua_createtable(L, 0, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, new HostString("inbox"));
    lua_xmove(L, co, 1);
    for (int i = 0; i < extra; i++)
    {
      lua_createtable(L, 0, 0);
      lua_pushvalue(L, -1);
      lua_setfield(L, LUA_REGISTRYINDEX, new HostString("log"));
      lua_xmove(L, co, 1);
    }
    lua_setbudget(co, STEPS, FREEZE);
    return co;
  }

  /* the inbox's slot 'i' as a string, or null */
  private static string slot(lua_State L, string table, long i)
  {
    lua_getfield(L, LUA_REGISTRYINDEX, new HostString(table));
    lua_rawgeti(L, -1, i);
    HostString s = lua_isstring(L, -1) ? lua_tostring(L, -1) : null;
    lua_pop(L, 2);
    return s?.ToString();
  }

  private static void deliver(lua_State L, string message, long tail)
  {
    lua_getfield(L, LUA_REGISTRYINDEX, new HostString("inbox"));
    lua_pushstring(L, new HostString(message));
    lua_rawseti(L, -2, tail);
    lua_pop(L, 1);
  }

  private static void inbox()
  {
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    lua_State co = program(L, PROGRAM, 0);
    var outbox = new ConcurrentQueue<string>();
    Thread prod = producer(outbox);
    long tail = 1, freezes = 0;
    long delivered = 0; /* the last slot delivered at the last freeze, to see it taken by the next */
    lua_ResumeResult result = lua_resume(co, null, 1);
    while (result.status == LUA_OK && lua_isfrozen(co))
    {
      freezes++;
      if (delivered != 0)
      { /* the thaw after a delivery: the program took it at its first look */
        CHECK(slot(L, "inbox", delivered) == null, "message " + delivered + " still in the inbox a slice after its delivery");
        delivered = 0;
      }
      while (outbox.TryDequeue(out string message))
      {
        delivered = tail;
        deliver(L, message, tail++);
      }
      result = lua_thaw(co, null);
    }
    CHECK(result.status == LUA_OK, "the program ended without error");
    CHECK(!lua_isfrozen(co), "not frozen once returned");
    CHECK(tail == MESSAGES + 1, "every message delivered");
    CHECK(freezes > MESSAGES, "frozen between deliveries (" + freezes + " freezes)");
    long spins = lua_tointegerx(co, -1).orElse(-1);
    CHECK(spins > freezes, "the program spun between freezes");
    Console.WriteLine("  inbox: " + MESSAGES + " messages, " + freezes + " freezes, " + spins + " spins");
    prod.Join();
    lua_close(L);
  }

  private static void interrupts()
  {
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    lua_State co = program(L, SCHEDULER, 1);
    var outbox = new ConcurrentQueue<string>();
    Thread prod = producer(outbox);
    long tail = 1, freezes = 0, stops = 0, plain = 0;
    lua_State child = null;
    lua_ResumeResult result = lua_resume(co, null, 2);
    while (result.status == LUA_OK && lua_isfrozen(co))
    {
      freezes++;
      if (child == null)
      {
        lua_getfield(L, LUA_REGISTRYINDEX, new HostString("log"));
        lua_getfield(L, -1, new HostString("child"));
        child = lua_tothread(L, -1);
        lua_pop(L, 2);
        CHECK(child != null, "the scheduler's child");
      }
      bool any = false;
      while (outbox.TryDequeue(out string message))
      {
        deliver(L, message, tail++);
        any = true;
      }
      if (any)
      { /* an interrupt: the scheduler wakes above its child */
        stops++;
        result = lua_thaw(co, null, new[] { child });
      }
      else
      {
        plain++;
        result = lua_thaw(co, null);
      }
    }
    CHECK(result.status == LUA_OK, "the scheduler ended without error" + (result.status != LUA_OK ? ": " + lua_tostring(co, -1) : ""));
    CHECK(lua_tointegerx(co, -1).orElse(-1) == MESSAGES, "the scheduler took every message");
    for (int i = 1; i <= MESSAGES; i++)
      CHECK(slot(L, "log", i) == "reading " + i, "message " + i + " in order");
    CHECK(stops >= 1 && stops <= MESSAGES, "the scheduler woke only for deliveries (" + stops + " interrupts)");
    CHECK(plain > stops, "the child ran on through plain thaws (" + plain + ")");
    CHECK(child != null && lua_isfrozen(child), "the child left frozen, never having yielded");
    lua_getfield(L, LUA_REGISTRYINDEX, new HostString("log"));
    lua_getfield(L, -1, new HostString("work"));
    long work = lua_tointegerx(L, -1).orElse(0);
    lua_pop(L, 2);
    CHECK(work > 0, "the child worked");
    Console.WriteLine("  interrupts: " + MESSAGES + " messages, " + freezes + " freezes, " + stops + " thaws stopped above the child, " + plain + " plain");
    prod.Join();
    lua_close(L);
  }

  /*
  ** 3. The firmware in Lua, as the mod's (machine.lua): the host only fills
  ** a mailbox, from another thread; the firmware (unbudgeted, on the main
  ** thread) drains it with a C function at each freeze, appends to a ring
  ** the kernel reads, and thaws with a stop above the kernel's child when
  ** there is news.
  */
  private const string FIRMWARE = @"
    local drain = ...
    local ring = {head = 0, tail = 0}
    local kernel = coroutine.create(function(ring)
      local work = 0
      local child = coroutine.create(function()
        pcall(function() while true do work = work + 1 end end)
      end)
      ring.child = child
      local got = {}
      while #got < MESSAGES do
        local ok = (debug.isfrozen(child) and debug.thaw or coroutine.resume)(child)
        assert(ok and debug.isfrozen(child))
        assert(ring.head ~= ring.tail, 'woken with an empty ring')
        while ring.head ~= ring.tail do
          ring.head = ring.head + 1
          got[#got + 1] = ring[ring.head]
        end
      end
      return table.concat(got, ','), work
    end)
    debug.setbudget(kernel, STEPS, function() debug.freeze() end)
    local r = table.pack(coroutine.resume(kernel, ring))
    local freezes, stops = 0, 0
    while debug.isfrozen(kernel) do
      freezes = freezes + 1
      local news = false
      for _, v in ipairs(drain()) do
        ring.tail = ring.tail + 1
        ring[ring.tail] = v
        news = true
      end
      if news then
        stops = stops + 1
        r = table.pack(debug.thaw(kernel, ring.child))
      else
        r = table.pack(debug.thaw(kernel))
      end
    end
    assert(r[1], r[2])
    return r[2], r[3], freezes, stops
  ";

  private static ConcurrentQueue<string> mailbox;

  private static int drain(lua_State L)
  {
    lua_createtable(L, 0, 0);
    long n = 0;
    while (mailbox.TryDequeue(out string m))
    {
      lua_pushstring(L, new HostString(m));
      lua_rawseti(L, -2, ++n);
    }
    return 1;
  }

  private static readonly lua_CFunction drain_fn = Inbox.drain;

  private static void firmware()
  {
    lua_State L = luaL_newstate();
    luaL_openlibs(L);
    mailbox = new ConcurrentQueue<string>();
    string code = FIRMWARE.Replace("MESSAGES", MESSAGES.ToString()).Replace("STEPS", STEPS.ToString());
    CHECK(luaL_loadstring(L, new HostString(code)) == LUA_OK, "load the firmware");
    lua_pushcfunction(L, drain_fn);
    Thread prod = producer(mailbox);
    int st = lua_pcall(L, 1, 4, 0);
    CHECK(st == LUA_OK, "the firmware ran" + (st != LUA_OK ? ": " + lua_tostring(L, -1) : ""));
    if (st == LUA_OK)
    {
      string got = lua_tostring(L, 1).ToString();
      string want = "reading 1";
      for (int i = 2; i <= MESSAGES; i++)
        want += ",reading " + i;
      CHECK(got == want, "the kernel got every message in order: " + got);
      CHECK(lua_tointegerx(L, 2).orElse(0) > 0, "the child worked");
      long freezes = lua_tointegerx(L, 3).orElse(0), stops = lua_tointegerx(L, 4).orElse(0);
      CHECK(stops >= 1 && stops <= MESSAGES && freezes > stops, "stops only for news (" + stops + " of " + freezes + " freezes)");
      Console.WriteLine("  firmware: " + MESSAGES + " messages, " + freezes + " freezes, " + stops + " thaws stopped above the child");
    }
    prod.Join();
    lua_close(L);
  }

  public static void main(string[] args)
  {
    Console.WriteLine("\tStarting inbox tests");
    inbox();
    interrupts();
    firmware();
    Console.WriteLine("\t" + (failures == 0 ? "OK" : "FAILED") + " - inbox: " + checks + " checks, " + failures + " failures");
    JSystem.exit(failures == 0 ? 0 : 1);
  }
}
