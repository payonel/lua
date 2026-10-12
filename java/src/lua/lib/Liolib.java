package lua.lib;

import static lua.api.LuaApi.*;
import static lua.api.LuaAuxLib.*;
import static lua.bytes.InternalString.sv_byteat;
import static lua.platform.LocaleUtil.*;
import static lua.platform.OsApi.*;
import static lua.runtime.Lhack.LUAL_BUFFERSIZE;

import lua.api.HostString;
import lua.api.luaL_Buffer;
import lua.api.luaL_RegCFI;
import lua.api.luaL_Stream;
import lua.api.lua_CFunction;
import lua.bytes.ByteOwner;
import lua.bytes.ByteSpan;
import lua.bytes.ByteView;
import lua.bytes.InternalString;
import lua.bytes.Lt;
import lua.object.CFuncInfo;
import lua.platform.OsFile;
import lua.platform.OsOpenResult;
import lua.platform.OsSystemResult;
import lua.runtime.lua_State;

/*
** $Id: liolib.c $
** Standard I/O (and system) library
** See Copyright Notice in lua.h
*/
public final class Liolib
{
  private Liolib()
  {
  }

  private static int l_checkmode(ByteView mode)
  {
    if (mode.empty() || Lt.rwa_modes.find(mode.at(0)) == ByteView.npos)
      return 0;
    int i = 1;
    if (i < mode.size() && mode.at(i) == '+') /* skip if char is '+' */
      i++;
    return (mode.find_first_not_of(Lt.b_char, i) == ByteView.npos) ? 1 : 0;
  }

  /* By default, Lua accepts only "r" or "w" as valid modes */
  private static int l_checkmodep(ByteView m)
  {
    return (m.size() == 1 && (m.at(0) == 'r' || m.at(0) == 'w')) ? 1 : 0;
  }

  /* }====================================================== */


  /* }====================================================== */

  private static luaL_Stream tolstream(lua_State L)
  {
    return (luaL_Stream) luaL_checkuserobjecti(L, 1, LUA_FILEHANDLE);
  }

  private static int isclosed(luaL_Stream p)
  {
    return (p.closef == null) ? 1 : 0;
  }

  private static int io_type(lua_State L)
  {
    luaL_Stream p;
    luaL_checkany(L, 1);
    p = (luaL_Stream) luaL_testuserobjecti(L, 1, LUA_FILEHANDLE);
    if (p == null)
      luaL_pushfail(L); /* not a file */
    else if (isclosed(p) != 0)
      lua_pushistringref(L, Lt.closed_file);
    else
      lua_pushistringref(L, Lt.file);
    return 1;
  }

  private static final lua_CFunction io_type = Liolib::io_type;

  private static int f_tostring(lua_State L)
  {
    luaL_Stream p = tolstream(L);
    if (isclosed(p) != 0)
      lua_pushistringref(L, Lt.file_closed);
    else
    {
      lua_pushlightuserdata(L, p.f);
      lua_pushfstringi(L, Lt.file_p);
    }
    return 1;
  }

  private static final lua_CFunction f_tostring = Liolib::f_tostring;

  private static OsFile tofile(lua_State L)
  {
    luaL_Stream p = tolstream(L);
    if (isclosed(p) != 0)
      luaL_errori(L, Lt.attempt_to_use_a_closed_file);
    assert p.f != null;
    return p.f;
  }

  /*
  ** When creating file handles, always creates a 'closed' file handle
  ** before opening the actual file; so, if there is a memory error, the
  ** handle is in a consistent state.
  */
  private static final int LSTREAM_SIZE = 16;

  private static luaL_Stream newprefile(lua_State L)
  {
    luaL_Stream o = new luaL_Stream();
    luaL_Stream p = o;
    p.closef = null; /* mark file handle as 'closed' */
    lua_newuserobject(L, o, LSTREAM_SIZE, 0);
    luaL_setmetatablei(L, LUA_FILEHANDLE);
    return p;
  }

  /*
  ** Calls the 'close' function from a file handle. The 'volatile' avoids
  ** a bug in some versions of the Clang compiler (e.g., clang 3.0 for
  ** 32 bits).
  */
  private static int aux_close(lua_State L)
  {
    luaL_Stream p = tolstream(L);
    lua_CFunction cf = p.closef;
    p.closef = null; /* mark stream as closed */
    return cf.call(L);  /* close it */
  }

  private static int f_close(lua_State L)
  {
    tofile(L); /* make sure argument is an open stream */
    return aux_close(L);
  }

  private static final lua_CFunction f_close = Liolib::f_close;

  private static int io_close(lua_State L)
  {
    if (lua_isnone(L, 1) != 0)                            /* no argument? */
      lua_getfieldi(L, LUA_REGISTRYINDEX, Lt._IO_output); /* use default output */
    return f_close(L);
  }

  private static final lua_CFunction io_close = Liolib::io_close;

  private static int f_gc(lua_State L)
  {
    luaL_Stream p = tolstream(L);
    if (isclosed(p) == 0 && p.f != null)
      aux_close(L); /* ignore closed and incompletely open files */
    return 0;
  }

  private static final lua_CFunction f_gc = Liolib::f_gc;

  /*
  ** function to close regular files
  */
  private static int io_fclose(lua_State L)
  {
    luaL_Stream p = tolstream(L);
    errno = 0;
    int err = os_fclose(p.f);
    return luaL_fileresulti(L, (err == 0) ? 1 : 0, null, err);
  }

  private static final lua_CFunction io_fclose = Liolib::io_fclose;

  /*
  ** What a handle freed without its '__gc' having run still has open (its
  ** group was dropped, or the state closed with the '__gc' waiting):
  ** 'luaL_Stream::freed' closes it, as the '__gc' would. (The standard files
  ** stay open.)
  */
  public static void closeleft(luaL_Stream p)
  {
    if (p.closef == null || p.f == null)
      return;
    if (p.closef == io_fclose)
      os_fclose(p.f);
    else if (p.closef == io_pclose)
      os_pclose(p.f);
  }

  private static luaL_Stream newfile(lua_State L)
  {
    luaL_Stream p = newprefile(L);
    p.f = null;
    p.closef = io_fclose;
    return p;
  }

  private static void opencheck(lua_State L, ByteView fname, ByteView mode)
  {
    luaL_Stream p = newfile(L);
    OsOpenResult o = os_fopen(fname, mode);
    p.f = o.f;
    if (p.f == null)
    {
      lua_pushistringref(L, fname);
      ByteView errmsg = os_strerror(o.err);
      lua_pushlstring(L, errmsg, 0, errmsg.size());
      luaL_errori(L, Lt.cannot_open_file_s_s);
    }
  }

  private static int io_open(lua_State L)
  {
    InternalString filename = luaL_checkistring(L, 1);
    InternalString mode = luaL_optistring(L, 2, Lt.r_mode_i);
    luaL_Stream p = newfile(L);
    luaL_argchecki(L, l_checkmode(mode.view()) != 0, 2, Lt.invalid_mode);
    OsOpenResult o = os_fopen(filename.view(), mode.view());
    p.f = o.f;
    return (p.f == null) ? luaL_fileresulti(L, 0, filename.view(), o.err) : 1;
  }

  private static final lua_CFunction io_open = Liolib::io_open;

  /*
  ** function to close 'popen' files
  */
  private static int io_pclose(lua_State L)
  {
    luaL_Stream p = tolstream(L);
    OsSystemResult r = os_pclose(p.f);
    return luaL_execresulti(L, r.stat, r.err);
  }

  private static final lua_CFunction io_pclose = Liolib::io_pclose;

  private static int io_popen(lua_State L)
  {
    InternalString filename = luaL_checkistring(L, 1);
    InternalString mode = luaL_optistring(L, 2, Lt.r_mode_i);
    luaL_Stream p = newprefile(L);
    luaL_argchecki(L, l_checkmodep(mode.view()) != 0, 2, Lt.invalid_mode);
    os_flushall();
    OsOpenResult o = os_popen(filename.view(), mode.view());
    p.f = o.f;
    p.closef = io_pclose;
    return (p.f == null) ? luaL_fileresulti(L, 0, filename.view(), o.err) : 1;
  }

  private static final lua_CFunction io_popen = Liolib::io_popen;

  private static int io_tmpfile(lua_State L)
  {
    luaL_Stream p = newfile(L);
    OsOpenResult o = os_tmpfile();
    p.f = o.f;
    return (p.f == null) ? luaL_fileresulti(L, 0, null, o.err) : 1;
  }

  private static final lua_CFunction io_tmpfile = Liolib::io_tmpfile;

  private static OsFile getiofile(lua_State L, ByteView findex)
  {
    luaL_Stream p;
    lua_getfieldi(L, LUA_REGISTRYINDEX, findex);
    p = (luaL_Stream) lua_touserobject(L, -1);
    if (isclosed(p) != 0)
    {
      ByteView name = findex.substr(Lt._IO_.size());
      lua_pushlstring(L, name, 0, name.size());
      luaL_errori(L, Lt.default_s_file_is_closed);
    }
    return p.f;
  }

  private static int g_iofile(lua_State L, ByteView f, ByteView mode)
  {
    if (lua_isnoneornil(L, 1) == 0)
    {
      if (lua_isstring(L, 1) != 0)
      {
        ByteView filename = lua_toistring(L, 1).view();
        opencheck(L, filename, mode);
      }
      else
      {
        tofile(L); /* check that it's a valid file handle */
        lua_pushvalue(L, 1);
      }
      lua_setfieldi(L, LUA_REGISTRYINDEX, f);
    }
    /* return current value */
    lua_getfieldi(L, LUA_REGISTRYINDEX, f);
    return 1;
  }

  private static int io_input(lua_State L)
  {
    return g_iofile(L, Lt._IO_input, Lt.r_mode);
  }

  private static final lua_CFunction io_input = Liolib::io_input;

  private static int io_output(lua_State L)
  {
    return g_iofile(L, Lt._IO_output, Lt.w_mode);
  }

  private static final lua_CFunction io_output = Liolib::io_output;

  /*
  ** maximum number of arguments to 'f:lines'/'io.lines' (it + 3 must fit
  ** in the limit for upvalues of a closure)
  */
  private static final int MAXARGLINE = 250;

  /*
  ** Auxiliary function to create the iteration function for 'lines'.
  ** The iteration function is a closure over 'io_readline', with
  ** the following upvalues:
  ** 1) The file being read (first value in the stack)
  ** 2) the number of arguments to read
  ** 3) a boolean, true iff file has to be closed when finished ('toclose')
  ** *) a variable number of format arguments (rest of the stack)
  */
  private static void aux_lines(lua_State L, int toclose)
  {
    int n = lua_gettop(L) - 1; /* number of arguments to read */
    luaL_argchecki(L, n <= MAXARGLINE, MAXARGLINE + 2, Lt.too_many_arguments);
    lua_pushvalue(L, 1);         /* file */
    lua_pushinteger(L, n);       /* number of arguments to read */
    lua_pushboolean(L, toclose); /* close/not close file when finished */
    lua_rotate(L, 2, 3);         /* move the three values to their positions */
    lua_pushcclosure(L, io_readline, 3 + n);
  }

  private static int f_lines(lua_State L)
  {
    tofile(L); /* check that it's a valid file handle */
    aux_lines(L, 0);
    return 1;
  }

  private static final lua_CFunction f_lines = Liolib::f_lines;

  /*
  ** Return an iteration function for 'io.lines'. If file has to be
  ** closed, also returns the file itself as a second result (to be
  ** closed as the state at the exit of a generic for).
  */
  private static int io_lines(lua_State L)
  {
    int toclose;
    if (lua_isnone(L, 1) != 0)
      lua_pushnil(L); /* at least one argument */
    if (lua_isnil(L, 1) != 0)
    {                                               /* no file name? */
      lua_getfieldi(L, LUA_REGISTRYINDEX, Lt._IO_input); /* get default input */
      lua_replace(L, 1);                            /* put it at index 1 */
      tofile(L);                                    /* check that it's a valid file handle */
      toclose = 0;                                  /* do not close it after iteration */
    }
    else
    { /* open a new file */
      ByteView filename = luaL_checkistring(L, 1).view();
      opencheck(L, filename, Lt.r_mode);
      lua_replace(L, 1); /* put file at index 1 */
      toclose = 1;       /* close it after iteration */
    }
    aux_lines(L, toclose); /* push iteration function */
    if (toclose != 0)
    {
      lua_pushnil(L);      /* state */
      lua_pushnil(L);      /* control */
      lua_pushvalue(L, 1); /* file is the to-be-closed variable (4th result) */
      return 4;
    }
    else
      return 1;
  }

  private static final lua_CFunction io_lines = Liolib::io_lines;

  /*
  ** {======================================================
  ** READ
  ** =======================================================
  */

  /* maximum length of a numeral */
  private static final int L_MAXLENNUM = 200;

  /* auxiliary structure used by 'read_number' */
  private static final class RN
  {
    OsFile f;                    /* file being read */
    int c;                      /* current character (look ahead) */
    int n;                      /* number of elements in buffer 'buff' */
    byte[] buff = new byte[L_MAXLENNUM + 1]; /* +1 for ending '\0' */
  }

  /*
  ** Add current char to buffer (if not out of space) and read next one
  */
  private static int nextc(RN rn)
  {
    if (rn.n >= L_MAXLENNUM)
    {                     /* buffer overflow? */
      rn.buff[0] = '\0'; /* invalidate result */
      return 0;           /* fail */
    }
    else
    {
      rn.buff[rn.n++] = (byte) rn.c; /* save current char */
      rn.c = os_getbyte(rn.f);                   /* read next one */
      return 1;
    }
  }

  /*
  ** Accept current char if it is in 'set' (of size 2)
  */
  private static int test2(RN rn, ByteView set)
  {
    if (rn.c == set.at(0) || rn.c == set.at(1))
      return nextc(rn);
    else
      return 0;
  }

  /*
  ** Read a sequence of (hex)digits
  */
  private static int readdigits(RN rn, int hex)
  {
    int count = 0;
    while ((hex != 0 ? locale_isxdigit(rn.c) : locale_isdigit(rn.c)) != 0 && nextc(rn) != 0)
      count++;
    return count;
  }

  /*
  ** Read a number: first reads a valid prefix of a numeral into a buffer.
  ** Then it calls 'lua_stringtonumber' to check whether the format is
  ** correct and to convert it to a Lua number.
  */
  private static int read_number(lua_State L, OsFile f)
  {
    RN rn = new RN();
    int count = 0;
    int hex = 0;
    byte[] decp = new byte[2];
    rn.f = f;
    rn.n = 0;
    decp[0] = locale_radixpoint(); /* get decimal point from locale */
    decp[1] = '.';                               /* always accept a dot */
    os_flockfile(rn.f);
    do
    {
      rn.c = os_getbyte(rn.f);
    } while (locale_isspace(rn.c) != 0); /* skip spaces */
    test2(rn, Lt.fmt_flags_sign);        /* optional sign */
    if (test2(rn, Lt.zero_zero) != 0)
    {
      if (test2(rn, Lt.xX_chars) != 0)
        hex = 1; /* numeral is hexadecimal */
      else
        count = 1; /* count initial '0' as a valid digit */
    }
    count += readdigits(rn, hex);   /* integral part */
    if (test2(rn, new ByteView(decp, 0, decp.length)) != 0)
      count += readdigits(rn, hex); /* fractional part */
    if (count > 0 && test2(rn, (hex != 0 ? Lt.pP_chars : Lt.eE_chars)) != 0)
    {                     /* exponent mark? */
      test2(rn, Lt.fmt_flags_sign);   /* exponent sign */
      readdigits(rn, 0); /* exponent digits */
    }
    os_ungetbyte(rn.f, rn.c); /* unread look-ahead char */
    os_funlockfile(rn.f);
    rn.buff[rn.n] = '\0'; /* finish string */
    ByteView buffsv = new ByteView(rn.buff, 0, rn.n);
    if (lua_stringtonumber(L, HostString.from_bytes(buffsv)) != 0)
      return 1; /* ok, it is a valid number */
    else
    {                 /* invalid format */
      lua_pushnil(L); /* "result" to be removed */
      return 0;       /* read fails */
    }
  }

  private static int test_eof(lua_State L, OsFile f)
  {
    int c = os_getbyte(f);
    os_ungetbyte(f, c);
    lua_pushistringref(L, Lt.empty_str);
    return (c != OS_EOF) ? 1 : 0;
  }

  private static int read_line(lua_State L, OsFile f, int chop)
  {
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      int c = 0;
      luaL_buffinit(L, b);
      do
      {                                   /* may need to read several chunks to get whole line */
        int off = luaL_prepbuffer(b); /* preallocate buffer space */
        ByteSpan buff = luaL_buffaddr(b);
        int i = 0;
        os_flockfile(f); /* no memory errors can happen inside the lock */
        while (i < LUAL_BUFFERSIZE && (c = os_getbyte(f)) != OS_EOF && c != '\n')
          buff.set(off + i++, c); /* read up to end of line or buffer limit */
        os_funlockfile(f);
        luaL_addsize(b, i);
      } while (c != OS_EOF && c != '\n'); /* repeat until end of line */
      if (chop == 0 && c == '\n')   /* want a newline and have one? */
        luaL_addchar(b, (byte) '\n'); /* add ending newline to result */
      luaL_pushresult(b);      /* close buffer */
      /* return ok if read something (either a newline or something else) */
      return (c == '\n' || lua_rawlen(L, -1) > 0) ? 1 : 0;
    }
    finally
    {
      b.release();
    }
  }

  private static void read_all(lua_State L, OsFile f)
  {
    int nr, want;
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, b);
      do
      { /* read file in chunks, growing the buffer as needed */
        int off = luaL_prepbuffer(b);
        ByteSpan area = luaL_buffaddr(b);
        want = area.size() - off;
        nr = os_fread(f, area, off, want);
        luaL_addsize(b, nr);
      } while (nr == want); /* buffer was filled completely: more may remain */
      luaL_pushresult(b); /* close buffer */
    }
    finally
    {
      b.release();
    }
  }

  private static int read_chars(lua_State L, OsFile f, long n)
  {
    int nr; /* number of chars actually read */
    luaL_Buffer b = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, b);
      int off = luaL_prepbuffsize(b, n); /* prepare buffer to read whole block */
      nr = os_fread(f, luaL_buffaddr(b), off, (int) n); /* try to read 'n' chars */
      luaL_addsize(b, nr);
      luaL_pushresult(b); /* close buffer */
      return (nr > 0) ? 1 : 0;     /* true iff read something */
    }
    finally
    {
      b.release();
    }
  }

  private static int g_read(lua_State L, OsFile f, int first)
  {
    int nargs = lua_gettop(L) - 1;
    int n, success;
    os_clearerr(f);
    errno = 0;
    if (nargs == 0)
    { /* no arguments? */
      success = read_line(L, f, 1);
      n = first + 1; /* to return 1 result */
    }
    else
    {
      /* ensure stack space for all results and for auxlib's buffer */
      luaL_checkstacki(L, nargs + LUA_MINSTACK, Lt.too_many_arguments);
      success = 1;
      for (n = first; nargs-- != 0 && success != 0; n++)
      {
        if (lua_type(L, n) == LUA_TNUMBER)
        {
          long l = luaL_checkinteger(L, n);
          success = (l == 0) ? test_eof(L, f) : read_chars(L, f, l);
        }
        else
        {
          InternalString fmt = luaL_checkistring(L, n);
          int fp = 0;
          if (sv_byteat(fmt.view(), fp) == '*')
            fp++; /* skip optional '*' (for compatibility) */
          switch (sv_byteat(fmt.view(), fp))
          {
          case 'n': /* number */
            success = read_number(L, f);
            break;
          case 'l': /* line */
            success = read_line(L, f, 1);
            break;
          case 'L': /* line with end-of-line */
            success = read_line(L, f, 0);
            break;
          case 'a':         /* file */
            read_all(L, f); /* read entire file */
            success = 1;    /* always success */
            break;
          default:
            return luaL_argerrori(L, n, Lt.invalid_format);
          }
        }
      }
    }
    if (os_ferror(f))
      return luaL_fileresulti(L, 0, null, errno);
    if (success == 0)
    {
      lua_pop(L, 1);    /* remove last result */
      luaL_pushfail(L); /* push nil instead */
    }
    return n - first;
  }

  private static int io_read(lua_State L)
  {
    return g_read(L, getiofile(L, Lt._IO_input), 1);
  }

  private static final lua_CFunction io_read = Liolib::io_read;

  private static int f_read(lua_State L)
  {
    return g_read(L, tofile(L), 2);
  }

  private static final lua_CFunction f_read = Liolib::f_read;

  /*
  ** Iteration function for 'lines'.
  */
  private static int io_readline(lua_State L)
  {
    luaL_Stream p = (luaL_Stream) lua_touserobject(L, lua_upvalueindex(1));
    int i;
    int n = (int) lua_tointeger(L, lua_upvalueindex(2));
    if (isclosed(p) != 0) /* file is already closed? */
      return luaL_errori(L, Lt.file_is_already_closed);
    lua_settop(L, 1);
    luaL_checkstacki(L, n, Lt.too_many_arguments);
    for (i = 1; i <= n; i++) /* push arguments to 'g_read' */
      lua_pushvalue(L, lua_upvalueindex(3 + i));
    n = g_read(L, p.f, 2);   /* 'n' is number of results */
    assert n > 0;        /* should return at least a nil */
    if (lua_toboolean(L, -n) != 0) /* read at least one value? */
      return n;               /* return them */
    else
    { /* first result is false: EOF or error */
      if (n > 1)
      { /* is there error information? */
        /* 2nd result is error message */
        lua_pushvalue(L, -n + 1);
        return luaL_errori(L, Lt.fmt_s);
      }
      if (lua_toboolean(L, lua_upvalueindex(3)) != 0)
      {                                        /* generator created file? */
        lua_settop(L, 0);                      /* clear stack */
        lua_pushvalue(L, lua_upvalueindex(1)); /* push file at index 1 */
        aux_close(L);                          /* close it */
      }
      return 0;
    }
  }

  private static final lua_CFunction io_readline = Liolib::io_readline;

  /* }====================================================== */

  private static int g_write(lua_State L, OsFile f, int arg)
  {
    int nargs = lua_gettop(L) - arg;
    long totalbytes = 0; /* total number of bytes written */
    errno = 0;
    for (; nargs-- != 0; arg++)
    { /* for each argument */
      int numbytes; /* bytes written in one call to 'fwrite' */
      ByteOwner numbuff = new ByteOwner(LUA_N2SBUFFSZ);
      ByteView numtxt = lua_numbertocstringi(L, arg, numbuff.span()); /* try as a number */
      ByteView bv = (numtxt != null)
        ? numbuff.view().substr(0, numtxt.size())
        : luaL_checklstring(L, arg);
      numbytes = os_fwrite(f, bv);
      totalbytes += numbytes;
      if (numbytes < bv.size())
      { /* write error? */
        int n = luaL_fileresulti(L, 0, null, errno);
        lua_pushinteger(L, totalbytes);
        return n + 1; /* return fail, error msg., error code, and counter */
      }
    }
    return 1; /* no errors; file handle already on stack top */
  }

  private static int io_write(lua_State L)
  {
    return g_write(L, getiofile(L, Lt._IO_output), 1);
  }

  private static final lua_CFunction io_write = Liolib::io_write;

  private static int f_write(lua_State L)
  {
    OsFile f = tofile(L);
    lua_pushvalue(L, 1); /* push file at the stack top (to be returned) */
    return g_write(L, f, 2);
  }

  private static final lua_CFunction f_write = Liolib::f_write;

  private static int f_seek(lua_State L)
  {
    ByteView[] modenames = {Lt.set, Lt.cur, Lt.end};
    OsFile f = tofile(L);
    int op = luaL_checkoptioni(L, 2, Lt.cur, modenames);
    long p3 = luaL_optinteger(L, 3, 0);
    long offset = p3;
    luaL_argchecki(L, offset == p3, 3, Lt.not_an_integer_in_proper_range);
    int serr = os_fseek(f, offset, op);
    if (serr != 0)
      return luaL_fileresulti(L, 0, null, serr); /* error */
    else
    {
      lua_pushinteger(L, os_ftell(f));
      return 1;
    }
  }

  private static final lua_CFunction f_seek = Liolib::f_seek;

  private static int f_setvbuf(lua_State L)
  {
    ByteView[] modenames = {Lt.no, Lt.full, Lt.line};
    OsFile f = tofile(L);
    int op = luaL_checkoptioni(L, 2, null, modenames);
    long sz = luaL_optinteger(L, 3, LUAL_BUFFERSIZE);
    int res = os_setvbuf(f, op, sz);
    return luaL_fileresulti(L, (res == 0) ? 1 : 0, null, res);
  }

  private static final lua_CFunction f_setvbuf = Liolib::f_setvbuf;

  private static int aux_flush(lua_State L, OsFile f)
  {
    errno = 0;
    int err = os_fflush(f);
    return luaL_fileresulti(L, (err == 0) ? 1 : 0, null, err);
  }

  private static int f_flush(lua_State L)
  {
    return aux_flush(L, tofile(L));
  }

  private static final lua_CFunction f_flush = Liolib::f_flush;

  private static int io_flush(lua_State L)
  {
    return aux_flush(L, getiofile(L, Lt._IO_output));
  }

  private static final lua_CFunction io_flush = Liolib::io_flush;

  /*
  ** functions for 'io' library
  */
  private static final CFuncInfo cfi_io_close = new CFuncInfo(io_close, "io.io_close");
  private static final CFuncInfo cfi_io_flush = new CFuncInfo(io_flush, "io.io_flush");
  private static final CFuncInfo cfi_io_input = new CFuncInfo(io_input, "io.io_input");
  private static final CFuncInfo cfi_io_lines = new CFuncInfo(io_lines, "io.io_lines");
  private static final CFuncInfo cfi_io_open = new CFuncInfo(io_open, "io.io_open");
  private static final CFuncInfo cfi_io_output = new CFuncInfo(io_output, "io.io_output");
  private static final CFuncInfo cfi_io_popen = new CFuncInfo(io_popen, "io.io_popen");
  private static final CFuncInfo cfi_io_read = new CFuncInfo(io_read, "io.io_read");
  private static final CFuncInfo cfi_io_tmpfile = new CFuncInfo(io_tmpfile, "io.io_tmpfile");
  private static final CFuncInfo cfi_io_type = new CFuncInfo(io_type, "io.io_type");
  private static final CFuncInfo cfi_io_write = new CFuncInfo(io_write, "io.io_write");

  private static final luaL_RegCFI[] iolib = {
    new luaL_RegCFI(Lt.close_kw, cfi_io_close),
    new luaL_RegCFI(Lt.flush, cfi_io_flush),
    new luaL_RegCFI(Lt.input, cfi_io_input),
    new luaL_RegCFI(Lt.lines, cfi_io_lines),
    new luaL_RegCFI(Lt.open_kw, cfi_io_open),
    new luaL_RegCFI(Lt.output, cfi_io_output),
    new luaL_RegCFI(Lt.popen, cfi_io_popen),
    new luaL_RegCFI(Lt.read_kw, cfi_io_read),
    new luaL_RegCFI(Lt.tmpfile, cfi_io_tmpfile),
    new luaL_RegCFI(Lt.type, cfi_io_type),
    new luaL_RegCFI(Lt.write_kw, cfi_io_write),
  };

  /*
  ** methods for file handles
  */
  private static final CFuncInfo cfi_f_read = new CFuncInfo(f_read, "io.f_read");
  private static final CFuncInfo cfi_f_write = new CFuncInfo(f_write, "io.f_write");
  private static final CFuncInfo cfi_f_lines = new CFuncInfo(f_lines, "io.f_lines");
  private static final CFuncInfo cfi_f_flush = new CFuncInfo(f_flush, "io.f_flush");
  private static final CFuncInfo cfi_f_seek = new CFuncInfo(f_seek, "io.f_seek");
  private static final CFuncInfo cfi_f_close = new CFuncInfo(f_close, "io.f_close");
  private static final CFuncInfo cfi_f_setvbuf = new CFuncInfo(f_setvbuf, "io.f_setvbuf");

  private static final luaL_RegCFI[] meth = {
    new luaL_RegCFI(Lt.read_kw, cfi_f_read),
    new luaL_RegCFI(Lt.write_kw, cfi_f_write),
    new luaL_RegCFI(Lt.lines, cfi_f_lines),
    new luaL_RegCFI(Lt.flush, cfi_f_flush),
    new luaL_RegCFI(Lt.seek, cfi_f_seek),
    new luaL_RegCFI(Lt.close_kw, cfi_f_close),
    new luaL_RegCFI(Lt.setvbuf, cfi_f_setvbuf),
  };

  /*
  ** metamethods for file handles
  */
  private static final CFuncInfo cfi_f_gc = new CFuncInfo(f_gc, "io.f_gc");
  private static final CFuncInfo cfi_f_tostring = new CFuncInfo(f_tostring, "io.f_tostring");

  private static final luaL_RegCFI[] metameth = {
    new luaL_RegCFI(Lt.mm_index, null),
    new luaL_RegCFI(Lt.mm_gc, cfi_f_gc),
    new luaL_RegCFI(Lt.mm_close, cfi_f_gc),
    new luaL_RegCFI(Lt.mm_tostring, cfi_f_tostring),
  };

  private static void createmeta(lua_State L)
  {
    luaL_newmetatablei(L, LUA_FILEHANDLE); /* metatable for file handles */
    luaL_setfuncscfi(L, metameth);        /* add metamethods to new metatable */
    lua_createtable(L, 0, meth.length - 1);            /* create method table */
    luaL_setfuncscfi(L, meth);            /* add file methods to method table */
    lua_setfieldi(L, -2, Lt.mm_index);       /* metatable.__index = method table */
    lua_pop(L, 1);                        /* pop metatable */
  }

  /*
  ** function to (not) close the standard files stdin, stdout, and stderr
  */
  private static int io_noclose(lua_State L)
  {
    luaL_Stream p = tolstream(L);
    p.closef = io_noclose; /* keep file opened */
    luaL_pushfail(L);
    lua_pushistringref(L, Lt.cannot_close_standard_file);
    return 2;
  }

  private static final lua_CFunction io_noclose = Liolib::io_noclose;

  private static void createstdfile(lua_State L, OsFile f, ByteView k,
                                    ByteView fname)
  {
    luaL_Stream p = newprefile(L);
    p.f = f;
    p.closef = io_noclose;
    if (!k.empty())
    {
      lua_pushvalue(L, -1);
      lua_setfieldi(L, LUA_REGISTRYINDEX, k); /* add file to registry */
    }
    lua_setfieldi(L, -2, fname); /* add file to module */
  }

  public static int luaopen_io(lua_State L)
  {
    luaL_newlibcfi(L, iolib);
    createmeta(L);
    /* create (and set) default files */
    createstdfile(L, os_stdin(), Lt._IO_input, Lt.stdin_2);
    createstdfile(L, os_stdout(), Lt._IO_output, Lt.stdout_);
    createstdfile(L, os_stderr(), Lt.empty_str, Lt.stderr_);
    return 1;
  }

  public static final lua_CFunction luaopen_io = Liolib::luaopen_io;
}
