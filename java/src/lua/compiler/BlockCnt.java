package lua.compiler;

/*
** nodes for block list (list of active blocks)
*/
final class BlockCnt
{
  BlockCnt previous; /* chain */
  int firstlabel;    /* index of first label in this block */
  int firstgoto;     /* index of first pending goto in this block */
  short nactvar;     /* number of active declarations at block entry */
  byte upval;        /* true if some variable in the block is an upvalue */
  byte isloop;       /* 1 if 'block' is a loop; 2 if it has pending breaks */
  byte insidetbc;    /* true if inside the scope of a to-be-closed var. */

  BlockCnt()
  {
  }

  BlockCnt(BlockCnt o)
  {
    copyfrom(o);
  }

  void copyfrom(BlockCnt o)
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
