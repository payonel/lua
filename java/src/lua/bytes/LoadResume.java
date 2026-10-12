package lua.bytes;

import lua.api.lua_KFunction;
import lua.compiler.Dyndata;
import lua.runtime.lua_State;

public final class LoadResume
{
  public lua_State L = null;
  public lua_KFunction k = null;    /* the continuation of the C function that loads */
  public long ctx = 0;
  public int boxidx = 0;            /* the stack slot of its box (made at the first stop) */
  public int pieceidx = 0;          /* the stack slot that keeps the piece being read */
  public int readeridx = 0;         /* the reader function's slot (0: a string chunk at 1) */
  public int baseccalls = 0;        /* the C calls when the load began (a bound of its frames) */
  public ZIO z = null;
  public Dyndata dyd = null;        /* the parser's arrays and buffer (held by the box while stopped) */
  public MBuffer buff = null;
  public boolean given = false;     /* (a string chunk: did its reader give it?) */
  public LoadBox box = null;
  public boolean recording = false; /* a freeze passes: the frames record themselves */
  public LoadFrame rframe = null;   /* (the next frame to make again) */
  public long points = 0;           /* the stop points it passed (tokens read, checks of the binary loader) */
  public long pieces = 0;           /* the pieces read before it had a box */
  public long ends = 0;             /* the reads that gave the end before it had a box */
  public boolean replaying = false; /* restored from outside the process: going again to where it stopped */
  public int clhash = 0;            /* the hash its function was given (0: not made yet) */
}
