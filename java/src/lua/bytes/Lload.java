package lua.bytes;

import static lua.api.LuaApi.*;
import static lua.api.LuaAuxLib.*;
import static lua.bytes.Lzio.*;
import static lua.gc.Lgc.*;
import static lua.object.Lnames.*;
import static lua.object.Lobject.*;
import static lua.runtime.Ldo.*;
import static lua.runtime.Lhack.*;
import static lua.runtime.Lmem.*;
import static lua.runtime.Lstate.*;

import java.util.Arrays;

import lua.api.lua_LoadSpec;
import lua.api.lua_ObjectFactory;
import lua.api.lua_Reader;
import lua.api.lua_UserObject;
import lua.compiler.Dyndata;
import lua.object.FmtArgs;
import lua.object.GCObject;
import lua.object.TValue;
import lua.object.Udata;
import lua.runtime.CallInfo;
import lua.runtime.Cost;
import lua.runtime.lua_State;

public final class Lload
{
  private Lload()
  {
  }

  /*
  ** A load (a parse or a binary load) that a freeze can stop: in a call of
  ** its reader function, or at a check point (each token read is a step of
  ** the budget). Each function of the load with a call a freeze can pass
  ** through runs as a state machine, each such call at a state of its own;
  ** when a freeze passes, each records its frame (its state and its locals)
  ** as the freeze leaves it, innermost first. After the thaw, the
  ** continuation of the C function that loads makes the calls again from
  ** the frames, outermost first, each function going to its state, and the
  ** load goes on where it stopped. (A function with nothing to keep before
  ** its call keeps no frame: made again, it goes the same way.)
  */

  /* where a load stopped */
  public static final byte LS_READ = 1;  /* in a call of its reader function */
  public static final byte LS_CHECK = 2; /* at a check point */

  /* the functions with frames ('luaZ_record'): a frame made again is checked to be its function's */
  public static final byte LF_FILL = 1;
  public static final byte LF_ZREAD = 2;
  public static final byte LF_FPARSER = 3;
  /* the lexer */
  public static final byte LF_LLEX = 4;
  public static final byte LF_NEXT = 5;
  public static final byte LF_INCLINE = 6;
  public static final byte LF_NUMERAL = 7;
  public static final byte LF_SKIPSEP = 8;
  public static final byte LF_LONGSTRING = 9;
  public static final byte LF_ESCCHECK = 10;
  public static final byte LF_GETHEXA = 11;
  public static final byte LF_HEXAESC = 12;
  public static final byte LF_UTF8ESC = 13;
  public static final byte LF_UTF8 = 14;
  public static final byte LF_DECESC = 15;
  public static final byte LF_STRING = 16;
  /* the parser */
  public static final byte LF_PARSER = 17;
  public static final byte LF_MAINFUNC = 18;
  public static final byte LF_INITGLOBAL = 19;
  public static final byte LF_STATLIST = 20;
  public static final byte LF_FIELDSEL = 21;
  public static final byte LF_YINDEX = 22;
  public static final byte LF_RECFIELD = 23;
  public static final byte LF_FIELD = 24;
  public static final byte LF_CONSTRUCTOR = 25;
  public static final byte LF_PARLIST = 26;
  public static final byte LF_BODY = 27;
  public static final byte LF_EXPLIST = 28;
  public static final byte LF_FUNCARGS = 29;
  public static final byte LF_PRIMARYEXP = 30;
  public static final byte LF_SUFFIXEDEXP = 31;
  public static final byte LF_SIMPLEEXP = 32;
  public static final byte LF_SUBEXPR = 33;
  public static final byte LF_BLOCK = 34;
  public static final byte LF_RESTASSIGN = 35;
  public static final byte LF_COND = 36;
  public static final byte LF_BREAKSTAT = 37;
  public static final byte LF_LABELSTAT = 38;
  public static final byte LF_WHILESTAT = 39;
  public static final byte LF_REPEATSTAT = 40;
  public static final byte LF_EXP1 = 41;
  public static final byte LF_FORBODY = 42;
  public static final byte LF_FORNUM = 43;
  public static final byte LF_FORLIST = 44;
  public static final byte LF_FORSTAT = 45;
  public static final byte LF_TESTTHEN = 46;
  public static final byte LF_IFSTAT = 47;
  public static final byte LF_LOCALFUNC = 48;
  public static final byte LF_VARATTRIB = 49;
  public static final byte LF_LOCALSTAT = 50;
  public static final byte LF_GLOBALNAMES = 51;
  public static final byte LF_GLOBALSTAT = 52;
  public static final byte LF_GLOBALFUNC = 53;
  public static final byte LF_GLOBALSTATFUNC = 54;
  public static final byte LF_FUNCNAME = 55;
  public static final byte LF_FUNCSTAT = 56;
  public static final byte LF_EXPRSTAT = 57;
  public static final byte LF_RETSTAT = 58;
  public static final byte LF_STATEMENT = 59;
  /* the binary loader */
  public static final byte LF_UNDUMP = 60;
  public static final byte LF_LOADVARINT = 61;
  public static final byte LF_LOADNUMBER = 62;
  public static final byte LF_LOADSTRING = 63;
  public static final byte LF_LOADCODE = 64;
  public static final byte LF_LOADCONSTANTS = 65;
  public static final byte LF_LOADUPVALUES = 66;
  public static final byte LF_LOADPROTOS = 67;
  public static final byte LF_LOADDEBUG = 68;
  public static final byte LF_LOADFUNCTION = 69;
  public static final byte LF_CHECKLITERAL = 70;
  public static final byte LF_CHECKNUM = 71;
  public static final byte LF_CHECKHEADER = 72;

  /* a stop point of a load that a freeze can stop: is a check point due there? */
  public static boolean luaZ_checkdue(LoadResume rz, lua_State L)
  {
    rz.points++;
    if (rz.replaying)
    {
      luaZ_replaypoint(rz);
      return false; /* (its steps were taken before) */
    }
    return (L.hookmask & LUA_MASKBUDGET) != 0 && luaE_spent(L, 1);
  }

  /* is 'rz' a stopped load whose frames are being made again (outermost first)? */
  public static boolean luaZ_resuming(LoadResume rz)
  {
    return rz != null && rz.z.resuming;
  }

  /* where the stream's piece was when the load stopped */
  static final byte ZS_NONE = 0;   /* none, or all read */
  static final byte ZS_STRING = 1; /* the chunk, a string (at 1) */
  static final byte ZS_PIECE = 2;  /* the piece the reader function gave (at 'pieceidx') */
  static final byte ZS_INPUT = 3;  /* the input read before a restore, read again */

  /* the bytes of frames a stopped load may have for each level of C calls it made */
  private static final long FRAMES_PER_LEVEL = 1024;
  private static final long FRAMES_BASE = 2048;
  /* the bytes a frame takes besides its own (the allocator's, as counted here) */
  private static final long FRAME_HEADER = 2 * Cost.Ref;

  private static final long LOADBOX_SIZE = 192; /* (its own size, its frames and input aside) */

  /* the frames of 'box' given back */
  static void freeframes(LoadBox box)
  {
    while (box.frames != null)
    {
      LoadFrame f = box.frames;
      box.frames = f.below;
    }
    box.framebytes = 0;
    box.lost = false;
  }

  /*
  ** 'n' more bytes kept by the box, counted by the host. (Called outside
  ** the parse, which is unlocked: a memory error leaves the state locked, as
  ** an error raised through the API does.)
  */
  private static void charge(lua_State L, LoadBox box, long n)
  {
    lua_lock(L);
    luaM_growhost(L, box.hosted, box.hosted + n);
    box.hosted += n;
    lua_unlock(L);
  }

  /* a new box, at slot 'boxidx' (outside the parse) */
  private static LoadBox newbox(lua_State L, int boxidx)
  {
    lua_lock(L);
    luaD_checkstack(L, 1);
    lua_unlock(L);
    LoadBox o = new LoadBox();
    LoadBox box = o;
    box.g = L.l_G;
    lua_newuserobject(L, o, 0, 1);
    lua_replace(L, boxidx);
    charge(L, box, LOADBOX_SIZE);
    return box;
  }

  /* 'b' kept at the end of the box's input (charged as it grows) */
  private static void keep(lua_State L, LoadBox box, ByteView b)
  {
    if (b.size() > box.input.length - box.inputlen)
    {
      int cap = Math.max(box.inputlen + b.size(), 2 * box.input.length);
      charge(L, box, cap - box.input.length);
      box.input = Arrays.copyOf(box.input, cap);
    }
    System.arraycopy(b.array(), b.offset(), box.input, box.inputlen, b.size());
    box.inputlen += b.size();
  }

  /* the blocks of the parser's arrays and buffer (bytes, as counted when made) */
  private static final int NHELD = 4;

  private static void heldblocks(Dyndata dyd, MBuffer buff, long[] b)
  {
    b[0] = (long) dyd.actvar.size * Cost.Vardesc;
    b[1] = (long) dyd.gt.size * Cost.Labeldesc;
    b[2] = (long) dyd.label.size * Cost.Labeldesc;
    b[3] = (long) buff.capacity() * Cost.Byte;
  }

  private static long heldsize(Dyndata dyd, MBuffer buff)
  {
    long[] b = new long[NHELD];
    heldblocks(dyd, buff, b);
    return b[0] + b[1] + b[2] + b[3];
  }

  /* kept outside the process: how it stopped, the points passed, the input */
  static final int LB_STOP = 0;
  static final int LB_POINTS = 1;
  static final int LB_GIVEN = 2;
  static final int LB_ANCHOR = 3; /* (0: it never stopped, a box left from a load that ended) */
  static final int LB_ENDS = 4;
  static final int LB_COMPLETE = 5;
  static final int LB_CLHASH = 6;
  static final int LB_NINTS = 7;

  private static lua_UserObject makeloadbox(lua_State L, long[] ints, int nints, ByteView bytes)
  {
    if (nints != LB_NINTS || (ints[LB_STOP] != LS_READ && ints[LB_STOP] != LS_CHECK) || ints[LB_POINTS] < 0 ||
        (ints[LB_GIVEN] != 0 && ints[LB_GIVEN] != 1) || ints[LB_ANCHOR] < 0 || ints[LB_ANCHOR] > Integer.MAX_VALUE ||
        ints[LB_ENDS] < 0 || (ints[LB_COMPLETE] != 0 && ints[LB_COMPLETE] != 1) || ints[LB_CLHASH] < 0 ||
        ints[LB_CLHASH] > 0xFFFFFFFFL)
      return null;
    LoadBox box = new LoadBox();
    box.g = L.l_G;
    box.stop = (byte) ints[LB_STOP];
    box.points = ints[LB_POINTS];
    box.given = ints[LB_GIVEN] != 0;
    box.anchoridx = (int) ints[LB_ANCHOR];
    box.ends = ints[LB_ENDS];
    box.complete = ints[LB_COMPLETE] != 0;
    box.clhash = (int) ints[LB_CLHASH];
    /* (a load that stopped goes again over its input, if it kept all of it;
       another is not stopped, and one that did not keep its input cannot go
       on: an error if it is made to) */
    box.replay = (box.anchoridx != 0 && box.complete);
    luaM_growhost(L, 0, LOADBOX_SIZE + bytes.size()); /* (counted by the host, as in the original) */
    box.hosted = LOADBOX_SIZE + bytes.size();
    box.input = new byte[bytes.size()];
    System.arraycopy(bytes.array(), bytes.offset(), box.input, 0, bytes.size());
    box.inputlen = bytes.size();
    return box;
  }

  private static final lua_ObjectFactory makeloadbox = Lload::makeloadbox;

  static
  {
    lua_registerobjectkind("load", makeloadbox);
  }

  /* a frame not as expected: an error */
  /* (in the parse, which runs locked) */
  public static RuntimeException luaZ_badstate(LoadResume rz)
  {
    rz.z.resuming = false;
    luaO_pushfstring(rz.L, Lt.invalid_state_of_a_stopped_load, new FmtArgs());
    throw luaD_throw(rz.L, (byte) LUA_ERRSYNTAX);
  }

  /* 'f' recorded in the box (nullptr: there was no memory for it) */
  private static void pushframe(LoadBox box, int fn, LoadFrame f)
  {
    if (f == null)
    { /* (the load cannot go on: see 'luaZ_loadk') */
      box.lost = true;
      return;
    }
    f.fn = fn;
    f.below = box.frames;
    box.frames = f;
    box.framebytes += f.size() + FRAME_HEADER;
    assert box.framebytes <= box.charged; /* (the bound of 'prepstop') */
  }

  /* the frame 'l' of function 'fn', as a freeze leaves it (a copy, kept by the box) */
  /* (as a freeze passes: no error can be raised, it would take its place) */
  public static void luaZ_record(LoadResume rz, int fn, LoadFrame l)
  {
    if (rz == null || !rz.recording || rz.box == null)
      return; /* (not a freeze of a stop point) */
    pushframe(rz.box, fn, l.copy());
  }

  /* the frame of 'fn' made again (its type is the one 'fn' records) */
  public static LoadFrame luaZ_restore(LoadResume rz, int fn)
  {
    LoadFrame f = rz.rframe;
    if (f == null || f.fn != fn)
      luaZ_badstate(rz);
    rz.rframe = f.below;
    return f;
  }

  /* the last frame made again: the load goes on from its stop */
  public static void luaZ_resumed(LoadResume rz)
  {
    if (rz.rframe != null)
      luaZ_badstate(rz);
    rz.z.resuming = false;
  }

  /* did the frames of a freeze record themselves? */
  public static boolean luaZ_recorded(LoadResume rz)
  {
    return rz.box.frames != null || rz.box.lost;
  }

  /* a stop point passed again by a restored load ('replaying') */
  /*
  ** A stop point passed again by a load restored from outside the process
  ** (see 'luaZ_loadk'): the one it stopped at is behind it.
  */
  public static void luaZ_replaypoint(LoadResume rz)
  {
    if (rz.points == rz.box.points && rz.box.stop == LS_CHECK)
      rz.replaying = false; /* (the load goes on from here) */
  }

  /*
  ** A stop point of the load: a freeze may pass. The box is made (at its
  ** slot) the first time; it gets room for the frames, charged to it, and
  ** keeps how the load stops and where the stream is.
  */
  private static void prepstop(LoadResume rz, byte kind)
  {
    lua_State L = rz.L;
    LoadBox box = rz.box;
    if (box == null)
    {
      box = newbox(L, rz.boxidx);
      box.complete = (rz.pieces == 0);
      box.ends = rz.ends;
      rz.box = box;
    }
    long levels = (long) getCcalls(L) - rz.baseccalls;
    long bound = FRAMES_BASE + (levels + 1) * FRAMES_PER_LEVEL;
    if (bound > box.charged)
    {
      charge(L, box, bound - box.charged);
      box.charged = bound;
    }
    long held = heldsize(rz.dyd, rz.buff);
    if (held > box.room)
    { /* (a freeze here cannot be refused the memory the box holds then) */
      charge(L, box, held - box.room);
      box.room = held;
    }
    freeframes(box);
    box.stop = kind;
    box.points = rz.points;
    box.clhash = rz.clhash;
    ZIO z = rz.z;
    ByteView d = z.chunk;
    ByteView s;
    box.zpos = z.pos;
    box.given = rz.given;
    if (z.pos == z.chunk.size())
    { /* (all read: what the piece was does not matter; an empty piece, which
         ends the stream, leaves the one before it, no longer kept) */
      box.zsrc = ZS_NONE;
      box.zpos = 0;
    }
    else if (rz.readeridx == 0 && (s = lua_tolstring(L, 1)) != null && d.array() == s.array() && d.offset() == s.offset())
      box.zsrc = ZS_STRING;
    else if (rz.readeridx > 0 && (s = lua_tolstring(L, rz.pieceidx)) != null && d.array() == s.array() && d.offset() == s.offset())
      box.zsrc = ZS_PIECE;
    else if (d.array() == box.input && d.offset() == 0)
      box.zsrc = ZS_INPUT;
    else
    { /* (cannot be: the stream reads from one of those) */
      lua_pushistringref(L, Lt.invalid_state_of_a_stopped_load);
      lua_error(L);
    }
    rz.recording = true;
  }

  /*
  ** The value the reader function gave, on the top: the next piece (kept in
  ** the box, if it keeps the input), or the end.
  */
  private static ByteView readerresult(lua_State L, LoadResume rz)
  {
    LoadBox box = rz.box;
    if (lua_isnil(L, -1) != 0)
    {
      lua_pop(L, 1); /* pop result */
      if (box == null)
        rz.ends++;
      else
        box.ends++;
      return null;
    }
    else if (lua_isstring(L, -1) == 0)
      luaL_errori(L, Lt.reader_function_must_return_a_string);
    lua_replace(L, rz.pieceidx); /* save string in its slot */
    ByteView piece = lua_tolstring(L, rz.pieceidx);
    if (piece.empty())
    { /* (an empty piece ends the stream too) */
      if (box == null)
        rz.ends++;
      else
        box.ends++;
    }
    else if (box == null)
      rz.pieces++;
    else if (box.complete)
      keep(L, box, piece);
    return piece;
  }

  /* the piece the reader function gave to the read a freeze stopped (see 'luaZ_fill') */
  public static ByteView luaZ_resumeread(LoadResume rz)
  {
    return readerresult(rz.L, rz);
  }

  /* a check point of the load */
  /*
  ** A check point of the load (a token read, when the budget is spent): the
  ** callback runs, and may freeze the load here. (The parse runs locked; the
  ** box and the check point are made through the API.)
  */
  public static void luaZ_checkpoint(LoadResume rz)
  {
    lua_unlock(rz.L);
    prepstop(rz, LS_CHECK);
    lua_checkpointk(rz.L, rz.ctx, rz.k);
    rz.recording = false;
    lua_lock(rz.L);
  }

  /*
  ** The reader function of 'load', called as stock calls it, inside the
  ** parse (its errors call the message handler in effect, where they
  ** happen). With a budget, a freeze can pass through the call.
  */
  private static final class FunctionReader implements lua_Reader
  {
    LoadResume rz;

    @Override
    public ByteView read(lua_State L)
    {
      luaL_checkstacki(L, 2, Lt.too_many_nested_functions);
      lua_pushvalue(L, rz.readeridx); /* get function */
      if ((L.hookmask & LUA_MASKBUDGET) != 0)
      {
        prepstop(rz, LS_READ);
        int status = lua_pcallkeep(L, 0, 1, rz.ctx, rz.k);
        rz.recording = false;
        if (status != LUA_OK)
        { /* (as it was: the handler has seen it) */
          lua_lock(L);
          luaD_throw(L, (byte) status);
        }
      }
      else
        lua_call(L, 0, 1); /* call it */
      return readerresult(L, rz);
    }
  }

  /* a chunk that is a string (at 1), or that is in the box ('boxed'), read at once */
  private static final class StringReader implements lua_Reader
  {
    LoadResume rz = null;
    boolean boxed = false;

    @Override
    public ByteView read(lua_State L)
    {
      if (rz.given)
        return null;
      rz.given = true;
      if (boxed)
        return new ByteView(rz.box.input, 0, rz.box.inputlen);
      return lua_tolstring(L, 1);
    }
  }

  /*
  ** A load restored from outside the process goes again over the input it
  ** had read, then (stopped in a call of the reader function) gets the
  ** piece that call gave, and then reads as before.
  */
  private static final class ReplayReader implements lua_Reader
  {
    LoadResume rz;
    lua_Reader next;
    int phase = 0;
    long ends = 0;

    @Override
    public ByteView read(lua_State L)
    {
      LoadBox box = rz.box;
      if (phase == 0)
      {
        phase = 1;
        if (box.inputlen != 0)
          return new ByteView(box.input, 0, box.inputlen);
      }
      if (phase == 1)
      {
        if (ends < box.ends)
        { /* (the reads that gave the end) */
          ends++;
          return null;
        }
        phase = 2;
        if (rz.replaying && box.stop == LS_READ)
        { /* the piece of the read it stopped in */
          rz.replaying = false;
          lua_getiuservalue(L, rz.boxidx, 1);
          lua_pushnil(L);
          lua_setiuservalue(L, rz.boxidx, 1);
          return readerresult(L, rz);
        }
      }
      return next.read(L);
    }
  }

  /* the chunk of a load from a reader that calls no code, read whole into its box */
  /*
  ** The chunk of a load from a reader that calls no code (a file), read
  ** whole into a new box at 'ls.boxidx' (with a budget: see 'lua_loadkread';
  ** outside the parse). It is kept as it is, perhaps long: its room is trimmed.
  */
  public static void luaZ_loadread(lua_State L, lua_LoadSpec ls, lua_Reader source)
  {
    LoadBox box = newbox(L, ls.boxidx);
    for (;;)
    {
      ByteView b = source.read(L);
      if (b == null || b.empty())
        break;
      keep(L, box, b);
    }
    int cap = box.input.length;
    if (cap > box.inputlen)
    {
      charge(L, box, box.inputlen); /* (the trimmed copy, before it is made) */
      box.input = Arrays.copyOf(box.input, box.inputlen);
      if (luaM_shrinkhost(L.l_G, box.hosted, box.hosted - cap))
        box.hosted -= cap;
    }
  }

  /* the function of the load is made ('cl', by the parser or the binary loader) */
  /*
  ** The function of the load is made. A load restored from outside the
  ** process makes it again as it goes over its input: it takes the hash the
  ** original's had (and none of the sequence, which the restored state has
  ** past it), so the objects made after it hash as in the original.
  */
  public static void luaZ_madefunction(LoadResume rz, GCObject cl)
  {
    if (rz == null)
      return;
    if (rz.replaying && rz.box.clhash != 0)
    {
      luaC_givebackhash(rz.L.l_G, cl);
      cl.hash = rz.box.clhash;
    }
    rz.clhash = cl.hash;
  }

  /* the load ended, with status 'st': what its box keeps is given back */
  private static int ended(lua_State L, LoadResume rz, int st)
  {
    LoadBox box = rz.box;
    if (box != null && !box.holds)
    {
      freeframes(box);
      box.input = new byte[0];
      box.inputlen = 0;
      box.anchoridx = 0; /* (not stopped: inert, if kept outside the process) */
      if (luaM_shrinkhost(L.l_G, box.hosted, LOADBOX_SIZE))
      {
        box.hosted = LOADBOX_SIZE;
        box.charged = box.room = 0;
      }
    }
    return st;
  }

  /* the load with spec 'ls' starts, or goes on after a thaw */
  /*
  ** A load with spec 'ls' starts, or goes on after a thaw ('resume': its box
  ** at 'ls.boxidx', the result of the reader function on the top if it
  ** stopped in a call of it):
  ** - a load stopped in this process makes its frames again;
  ** - a load restored from outside the process (its box keeps its input and
  **   the stop points it passed) goes again over that input from the start,
  **   taking no step and making no check point up to where it stopped.
  ** Returns its status: the function on the top, or the error.
  */
  public static int luaZ_loadk(lua_State L, lua_LoadSpec ls, boolean resume)
  {
    LoadResume rz = new LoadResume();
    rz.L = L;
    rz.k = ls.k;
    rz.ctx = ls.ctx;
    rz.boxidx = ls.boxidx;
    rz.pieceidx = ls.pieceidx;
    rz.readeridx = ls.readeridx;
    rz.baseccalls = getCcalls(L);
    FunctionReader freader = new FunctionReader();
    StringReader sreader = new StringReader();
    ReplayReader rreader = new ReplayReader();
    freader.rz = rz;
    sreader.rz = rz;
    sreader.boxed = (ls.readeridx < 0);
    lua_Reader reader = (ls.readeridx > 0) ? (lua_Reader) freader : sreader;
    ZIO z = new ZIO();
    luaZ_init(L, z, reader);
    z.rz = rz;
    rz.z = z;
    if (!resume && ls.readeridx < 0)
    { /* (its box, made by 'luaZ_loadread') */
      rz.box = (lua_touserobject(L, ls.boxidx) instanceof LoadBox b) ? b : null;
      if (rz.box == null)
      {
        lua_pushistringref(L, Lt.invalid_state_of_a_stopped_load);
        return LUA_ERRSYNTAX;
      }
    }
    else if (resume)
    {
      LoadBox box = (lua_touserobject(L, ls.boxidx) instanceof LoadBox b) ? b : null;
      if (box == null || (!box.holds && !box.replay))
      {
        lua_pushistringref(L, Lt.invalid_state_of_a_stopped_load);
        return LUA_ERRSYNTAX;
      }
      rz.box = box;
      if (!box.replay)
        rz.clhash = box.clhash; /* (its function is held: made) */
      if (box.replay)
      { /* restored from outside the process: from the start again */
        if (box.anchoridx > lua_gettop(L) || (box.stop == LS_READ) != (lua_gettop(L) > box.anchoridx) ||
            lua_gettop(L) > box.anchoridx + 1)
        {
          lua_pushistringref(L, Lt.invalid_state_of_a_stopped_load);
          return LUA_ERRSYNTAX;
        }
        if (box.stop == LS_READ)
          lua_setiuservalue(L, ls.boxidx, 1); /* (the piece, until the read gets it) */
        lua_settop(L, box.anchoridx - 1);
        box.replay = false;
        rz.replaying = true;
        rz.given = (ls.readeridx < 0); /* (a chunk in the box: the replay gives it first) */
        rreader.rz = rz;
        rreader.next = reader;
        z.reader = rreader;
        lua_lock(L);
        int st = luaD_protectedparser(L, z, ls.name, ls.mode, rz);
        lua_unlock(L);
        return ended(L, rz, st);
      }
      if (box.lost)
      { /* (a frame had no memory to be recorded) */
        lua_pushistringref(L, Lt.not_enough_memory);
        return ended(L, rz, LUA_ERRMEM);
      }
      z.resuming = true;
      rz.rframe = box.frames;
      ByteView s;
      rz.given = box.given;
      if (box.zsrc == ZS_STRING && ls.readeridx == 0 && (s = lua_tolstring(L, 1)) != null)
        z.chunk = s;
      else if (box.zsrc == ZS_PIECE && ls.readeridx != 0 && (s = lua_tolstring(L, ls.pieceidx)) != null)
        z.chunk = s;
      else if (box.zsrc == ZS_INPUT)
        z.chunk = new ByteView(box.input, 0, box.inputlen);
      else if (box.zsrc != ZS_NONE)
      {
        lua_pushistringref(L, Lt.invalid_state_of_a_stopped_load);
        return LUA_ERRSYNTAX;
      }
      if (box.zpos > z.chunk.size())
      {
        lua_pushistringref(L, Lt.invalid_state_of_a_stopped_load);
        return LUA_ERRSYNTAX;
      }
      z.pos = box.zpos;
      rz.points = box.points;
    }
    lua_lock(L);
    int st = luaD_protectedparser(L, z, ls.name, ls.mode, rz);
    lua_unlock(L);
    return ended(L, rz, st);
  }

  /* the parser's arrays and buffer, held by the box while the load is stopped */
  /*
  ** The parser's arrays and buffer, and its anchor table (at stack index
  ** 'anchoridx', the first time), go to the box of a load a freeze stopped:
  ** the box keeps them, and what is half made is not left in the stack (it is
  ** not kept outside the process).
  */
  public static void luaZ_hold(LoadResume rz, CallInfo ci, int anchoridx)
  {
    lua_State L = rz.L;
    LoadBox box = rz.box;
    Dyndata dyd = rz.dyd;
    MBuffer buff = rz.buff;
    long[] b = new long[NHELD];
    heldblocks(dyd, buff, b);
    assert b[0] + b[1] + b[2] + b[3] <= box.room; /* (as at the stop point) */
    for (long n : b)
      luaM_freeblock(L, n); /* (counted in the box's room now) */
    box.dyd.copyfrom(dyd);
    dyd.copyfrom(new Dyndata());
    box.bufflen = buff.size();
    box.buff = buff.release();
    if (box.anchoridx == 0)
      box.anchoridx = anchoridx;
    TValue bv = getslot(L, ci.func + rz.boxidx);
    Udata u = uvalue(bv);
    TValue a = getslot(L, ci.func + box.anchoridx);
    copyobj(u.uv[0], a);
    luaC_barrier(L, u, u.uv[0]);
    setslotnil(L, ci.func + box.anchoridx);
    box.holds = true;
  }

  /*
  ** ... and come back from it when the load goes on (a memory error if they
  ** cannot be counted on their own again: the box still holds them)
  */
  public static void luaZ_unhold(LoadResume rz, CallInfo ci)
  {
    lua_State L = rz.L;
    LoadBox box = rz.box;
    Dyndata dyd = rz.dyd;
    MBuffer buff = rz.buff;
    long[] b = new long[NHELD];
    b[0] = (long) box.dyd.actvar.size * Cost.Vardesc;
    b[1] = (long) box.dyd.gt.size * Cost.Labeldesc;
    b[2] = (long) box.dyd.label.size * Cost.Labeldesc;
    b[3] = (long) box.buff.size() * Cost.Byte;
    int i = 0;
    try
    {
      for (; i < NHELD; i++)
        luaM_takeblock(L, b[i]);
    }
    catch (Throwable e)
    {
      while (i-- > 0)
        luaM_freeblock(L, b[i]);
      throw e;
    }
    dyd.copyfrom(box.dyd);
    box.dyd.copyfrom(new Dyndata());
    buff.adopt(ByteOwner.move(box.buff), box.bufflen);
    box.bufflen = 0;
    if (box.room != 0 && luaM_shrinkhost(box.g, box.hosted, box.hosted - box.room))
    { /* (else the box keeps its room) */
      box.hosted -= box.room;
      box.room = 0;
    }
    TValue bv = getslot(L, ci.func + rz.boxidx);
    Udata u = uvalue(bv);
    TValue a = getobj(u.uv[0]);
    setslot(L, ci.func + box.anchoridx, a);
    setnilvalue(u.uv[0]);
    box.holds = false;
  }
}
