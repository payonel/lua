package lua.api;

import lua.bytes.ByteView;

/*
** 'load' of the base library: a load that a freeze can stop (in a call of
** its reader function, or at a check point: each token read is a step) and
** that goes on in 'k' after the thaw ('lua_loadkresume' there, with the
** status 'k' got). The chunk is the string at 1 ('readeridx' 0) or what
** the reader function at 'readeridx' gives; the piece being read is kept
** at 'pieceidx' and the stopped load in a box at 'boxidx' (slots after the
** arguments, nil at first). Returns the status: the function on the top,
** or the error.
*/
public final class lua_LoadSpec
{
  public int readeridx;
  public int pieceidx;
  public int boxidx;
  public ByteView name;
  public ByteView mode;
  public long ctx;
  public lua_KFunction k;

  public lua_LoadSpec()
  {
  }

  public lua_LoadSpec(int readeridx, int pieceidx, int boxidx, ByteView name, ByteView mode, long ctx,
                      lua_KFunction k)
  {
    this.readeridx = readeridx;
    this.pieceidx = pieceidx;
    this.boxidx = boxidx;
    this.name = name;
    this.mode = mode;
    this.ctx = ctx;
    this.k = k;
  }
}
