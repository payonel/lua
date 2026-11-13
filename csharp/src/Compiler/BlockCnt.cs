using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Compiler;

/*
** nodes for block list (list of active blocks)
*/
public sealed class BlockCnt
{
  internal BlockCnt previous; /* chain */
  internal int firstlabel;    /* index of first label in this block */
  internal int firstgoto;     /* index of first pending goto in this block */
  internal short nactvar;     /* number of active declarations at block entry */
  internal bool upval;     /* true if some variable in the block is an upvalue */
  internal sbyte isloop;       /* 1 if 'block' is a loop; 2 if it has pending breaks */
  internal bool insidetbc; /* true if inside the scope of a to-be-closed var. */

  internal BlockCnt()
  {
  }

  internal BlockCnt(BlockCnt o)
  {
    copyfrom(o);
  }

  internal void copyfrom(BlockCnt o)
  {
    previous = o.previous;
    firstlabel = o.firstlabel;
    firstgoto = o.firstgoto;
    nactvar = o.nactvar;
    upval = o.upval;
    isloop = o.isloop;
    insidetbc = o.insidetbc;
  }
}
