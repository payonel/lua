package com.payonel.lungolua.object;

import com.payonel.lungolua.api.lua_Budget;
import com.payonel.lungolua.runtime.lua_State;

/*
** A budget group: the budget shared by the threads of one user program
** ('lua_setbudget'). A thread keeps the group it belongs to ('bown') and
** the group in effect while it runs ('beff'). Collectable, so that a group
** that no thread refers to goes away.
*/
public final class BudgetGroup extends GCObject
{
  public long budget = 0;        /* steps between two calls of 'f' */
  public long left = 0;          /* steps left before the next call */
  public lua_Budget f = null;    /* the callback */
  public lua_State root = null;  /* the thread it was set on (weak: cleared when that thread is freed) */
  public TValue cb = new TValue(0, null, Lobject.LUA_VNIL); /* a value for the callback (the debug library keeps its Lua function here) */
  /* the dead objects of the group that wait for their '__gc' (a queue: the
     collector links them here; the group's threads call them); only 'next'
     of the head is used */
  public GCObject fnz = new GCObject();
  public GCObject fnztail = null; /* its last object (nullptr: the queue is empty) */
  public int id = 0;              /* its owner id (the objects it owns carry it) */
  public boolean qmarked = false; /* the collector marked its queue in this cycle (the group is alive) */
  public boolean nodrain = false; /* a thaw went on: the next budget point calls no '__gc' */
}
