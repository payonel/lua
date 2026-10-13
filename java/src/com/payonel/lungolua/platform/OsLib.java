package com.payonel.lungolua.platform;

/*
** A loaded dynamic library. The JVM loads no C code: its libraries are
** Java classes that stand in for C ones, and their symbols are the
** classes' public static fields (see OsApi's 'os_dlopen').
*/
public final class OsLib
{
  final byte[] name; /* the name it was loaded with (as 'dlerror' names it) */
  final Object key; /* what it was loaded from: its file, or its class */
  final Class<?> cls; /* the class whose fields are its symbols (null for the program) */
  boolean global; /* do its symbols resolve those of the libraries loaded later? */
  int refs; /* the 'os_dlopen's not yet closed */

  OsLib(byte[] name, Object key, Class<?> cls)
  {
    this.name = name;
    this.key = key;
    this.cls = cls;
  }
}
