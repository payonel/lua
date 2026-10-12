package lua.compiler;

/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
final class LHS_assign
{
  LHS_assign prev;
  final expdesc v = new expdesc(); /* variable (global, local, upvalue, or indexed) */

  LHS_assign()
  {
  }

  LHS_assign(LHS_assign o)
  {
    copyfrom(o);
  }

  void copyfrom(LHS_assign o)
  {
    prev = o.prev;
    v.copyfrom(o.v);
  }
}
