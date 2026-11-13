using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Platform;

/*
** A loaded dynamic library. .NET loads no C code here: its libraries are
** C# classes that stand in for C ones, and their symbols are the
** classes' public static fields (see OsApi's 'os_dlopen').
*/
public sealed class OsLib
{
  internal readonly sbyte[] name; /* the name it was loaded with (as 'dlerror' names it) */
  internal readonly object key; /* what it was loaded from: its file, or its class */
  internal readonly Type cls; /* the class whose fields are its symbols (null for the program) */
  internal bool global; /* do its symbols resolve those of the libraries loaded later? */
  internal int refs; /* the 'os_dlopen's not yet closed */

  internal OsLib(sbyte[] name, object key, Type cls)
  {
    this.name = name;
    this.key = key;
    this.cls = cls;
  }
}
