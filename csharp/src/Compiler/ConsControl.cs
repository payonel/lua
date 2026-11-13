using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Compiler;

public sealed class ConsControl
{
  internal readonly expdesc v = new expdesc(); /* last list item read */
  internal expdesc t;      /* table descriptor */
  internal int nh;         /* total number of 'record' elements */
  internal int na;         /* number of array elements already stored */
  internal int tostore;    /* number of array elements pending to be stored */
  internal int maxtostore; /* maximum number of pending elements */

  internal ConsControl()
  {
  }

  internal ConsControl(ConsControl o)
  {
    copyfrom(o);
  }

  internal void copyfrom(ConsControl o)
  {
    v.copyfrom(o.v);
    t = o.t;
    nh = o.nh;
    na = o.na;
    tostore = o.tostore;
    maxtostore = o.maxtostore;
  }
}
