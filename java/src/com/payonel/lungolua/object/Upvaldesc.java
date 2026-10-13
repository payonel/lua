package com.payonel.lungolua.object;

/*
** Description of an upvalue for function prototypes
*/
public final class Upvaldesc
{
  public TString name;    /* upvalue name (for debug information) */
  public boolean instack; /* whether it is in stack (register) */
  public int idx;         /* index of upvalue (in stack or in outer function's list) */
  public byte kind;       /* kind of corresponding variable */
}
