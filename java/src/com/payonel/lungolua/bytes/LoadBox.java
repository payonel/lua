package com.payonel.lungolua.bytes;

import static com.payonel.lungolua.bytes.Lload.*;
import static com.payonel.lungolua.runtime.Lmem.*;

import com.payonel.lungolua.api.lua_UserObject;
import com.payonel.lungolua.compiler.Dyndata;
import com.payonel.lungolua.runtime.global_State;
import com.payonel.lungolua.runtime.lua_State;

/*
** The box of a load that a freeze can stop, in a stack slot of the C
** function that loads (made at its first stop point). What it keeps is
** counted by the host, not in the Lua heap (a program does not see it):
** one block, given back when the collector frees the box.
** - its frames, recorded when a freeze passes (charged to it), how it
**   stopped and where the stream was; while stopped, the parser's arrays
**   and buffer (charged to it as room for them, at a stop point; they are
**   counted in it while it holds them) and its anchor table (a user value);
** - what is kept outside the process (see 'luaZ_loadk'): the input read
**   (the pieces of the reader function) and the stop points passed, from
**   which a restored load goes again to where it stopped.
*/
public final class LoadBox implements lua_UserObject
{
  public global_State g = null;
  public long hosted = 0;          /* (the bytes the host counts for it) */
  public LoadFrame frames = null;  /* (the last one recorded: the outermost) */
  public long framebytes = 0;      /* (the bytes they take) */
  public boolean lost = false;     /* (a frame had no memory to be recorded) */
  public long charged = 0;         /* (the room for its frames) */
  public long room = 0;            /* (the room for the parser's arrays and buffer) */
  public byte stop = 0;
  public byte zsrc = ZS_NONE;
  public int zpos = 0;
  public boolean given = false;    /* (a string chunk: was it given?) */
  public boolean holds = false;    /* (the arrays and buffer below, and the anchor) */
  public boolean complete = true;  /* is 'input' all the input read? */
  public boolean replay = false;   /* restored from outside the process: frames gone */
  public long points = 0;          /* the stop points passed when it stopped */
  public int clhash = 0;           /* the hash its function was given (0: not made when it stopped) */
  public long ends = 0;            /* the reads that gave the end (nil or an empty piece) */
  public int anchoridx = 0;        /* the stack slot of the anchor table */
  public final Dyndata dyd = new Dyndata();
  public ByteOwner buff = new ByteOwner();
  public int bufflen = 0;
  public byte[] input = new byte[0]; /* (charged to it) */
  public int inputlen = 0;

  public void destroy()
  {
    freeframes(this);
    if (holds)
    { /* (counted in its room) */
      dyd.actvar.arr = null;
      dyd.gt.arr = null;
      dyd.label.arr = null;
    }
  }

  @Override
  public void freed(lua_State L)
  {
    luaM_shrinkhost(L.l_G, hosted, 0);
    hosted = 0;
  }

  @Override
  public String persistkind()
  {
    return "load";
  }

  @Override
  public int persistints(long[] out)
  {
    out[LB_STOP] = stop;
    out[LB_POINTS] = points;
    out[LB_GIVEN] = given ? 1 : 0;
    out[LB_ANCHOR] = anchoridx;
    out[LB_ENDS] = ends;
    out[LB_COMPLETE] = complete ? 1 : 0;
    out[LB_CLHASH] = Integer.toUnsignedLong(clhash);
    return LB_NINTS;
  }

  @Override
  public ByteView persistbytes()
  {
    return new ByteView(input, 0, inputlen);
  }

  @Override
  public boolean persistuservalues()
  {
    return false;
  }
}
