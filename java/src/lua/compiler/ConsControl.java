package lua.compiler;

final class ConsControl
{
  final expdesc v = new expdesc(); /* last list item read */
  expdesc t;      /* table descriptor */
  int nh;         /* total number of 'record' elements */
  int na;         /* number of array elements already stored */
  int tostore;    /* number of array elements pending to be stored */
  int maxtostore; /* maximum number of pending elements */

  ConsControl()
  {
  }

  ConsControl(ConsControl o)
  {
    copyfrom(o);
  }

  void copyfrom(ConsControl o)
  {
    v.copyfrom(o.v);
    t = o.t;
    nh = o.nh;
    na = o.na;
    tostore = o.tostore;
    maxtostore = o.maxtostore;
  }
}
