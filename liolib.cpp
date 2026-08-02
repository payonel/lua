/*
** $Id: liolib.c $
** Standard I/O (and system) library
** See Copyright Notice in lua.h
*/



#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lua_internal.h"
#include "os_api.h"

#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"

/* Check whether 'mode' matches '[rwa]%+?[b]*' */
static int l_checkmode(std::string_view mode)
{
  if (mode.empty() || lit<"rwa">()->view().find(mode[0]) == std::string_view::npos)
    return 0;
  size_t i = 1;
  if (i < mode.size() && mode[i] == '+') /* skip if char is '+' */
    i++;
  return mode.find_first_not_of(lit<"b">()->view(), i) == std::string_view::npos;
}

/* By default, Lua accepts only "r" or "w" as valid modes */
static inline int l_checkmodep(std::string_view m)
{
  return m.size() == 1 && (m[0] == 'r' || m[0] == 'w');
}

/* }====================================================== */

#include <sys/types.h>

/* }====================================================== */

static const InternalString* const IO_PREFIX = lit<"_IO_">();
static const size_t IOPREF_LEN = IO_PREFIX->view().size();
static const InternalString* const IO_INPUT = lit<"_IO_input">();
static const InternalString* const IO_OUTPUT = lit<"_IO_output">();

typedef luaL_Stream LStream;

static inline LStream* tolstream(lua_State* L)
{
  return (LStream*)luaL_checkudatai(L, 1, LUA_FILEHANDLE->view());
}

static inline int isclosed(const LStream* p)
{
  return p->closef == nullptr;
}

static int io_type(lua_State* L)
{
  LStream* p;
  luaL_checkany(L, 1);
  p = (LStream*) luaL_testudatai(L, 1, LUA_FILEHANDLE->view());
  if (p == nullptr)
    luaL_pushfail(L); /* not a file */
  else if (isclosed(p))
    lua_pushistringref(L, lit<"closed file">()->view());
  else
    lua_pushistringref(L, lit<"file">()->view());
  return 1;
}

static int f_tostring(lua_State* L)
{
  LStream* p = tolstream(L);
  if (isclosed(p))
    lua_pushistringref(L, lit<"file (closed)">()->view());
  else
  {
    lua_pushlightuserdata(L, p->f);
    lua_pushfstringi(L, lit<"file (%p)">()->view());
  }
  return 1;
}

static OsFile* tofile(lua_State* L)
{
  LStream* p = tolstream(L);
  if (l_unlikely(isclosed(p)))
    luaL_errori(L, lit<"attempt to use a closed file">()->view());
  assert(p->f);
  return p->f;
}

/*
** When creating file handles, always creates a 'closed' file handle
** before opening the actual file; so, if there is a memory error, the
** handle is in a consistent state.
*/
static LStream* newprefile(lua_State* L)
{
  LStream* p = (LStream*) lua_newuserdatauv(L, sizeof(LStream), 0);
  p->closef = nullptr; /* mark file handle as 'closed' */
  luaL_setmetatablei(L, LUA_FILEHANDLE->view());
  return p;
}

/*
** Calls the 'close' function from a file handle. The 'volatile' avoids
** a bug in some versions of the Clang compiler (e.g., clang 3.0 for
** 32 bits).
*/
static int aux_close(lua_State* L)
{
  LStream* p = tolstream(L);
  volatile lua_CFunction cf = p->closef;
  p->closef = nullptr; /* mark stream as closed */
  return (*cf)(L);  /* close it */
}

static int f_close(lua_State* L)
{
  tofile(L); /* make sure argument is an open stream */
  return aux_close(L);
}

static int io_close(lua_State* L)
{
  if (lua_isnone(L, 1))                            /* no argument? */
    lua_getfieldi(L, LUA_REGISTRYINDEX, IO_OUTPUT->view()); /* use default output */
  return f_close(L);
}

static int f_gc(lua_State* L)
{
  LStream* p = tolstream(L);
  if (!isclosed(p) && p->f != nullptr)
    aux_close(L); /* ignore closed and incompletely open files */
  return 0;
}

/*
** function to close regular files
*/
static int io_fclose(lua_State* L)
{
  LStream* p = tolstream(L);
  errno = 0;
  int err = os_fclose(p->f);
  return luaL_fileresulti(L, err == 0, {}, err);
}

static LStream* newfile(lua_State* L)
{
  LStream* p = newprefile(L);
  p->f = nullptr;
  p->closef = &io_fclose;
  return p;
}

static void opencheck(lua_State* L, const InternalString* fname, std::string_view mode)
{
  LStream* p = newfile(L);
  OsOpenResult o = os_fopen(fname->view(), mode);
  p->f = o.f;
  if (l_unlikely(p->f == nullptr))
  {
    lua_pushistringref(L, fname->view());
    InternalString errmsg = os_strerror(o.err);
    lua_pushlstring(L, errmsg.view(), 0, errmsg.size());
    luaL_errori(L, lit<"cannot open file '%s' (%s)">()->view());
  }
}

static int io_open(lua_State* L)
{
  const InternalString* filename = luaL_checkistring(L, 1);
  const InternalString* mode = luaL_optistring(L, 2, lit<"r">());
  LStream* p = newfile(L);
  luaL_argchecki(L, l_checkmode(mode->view()), 2, lit<"invalid mode">()->view());
  OsOpenResult o = os_fopen(filename->view(), mode->view());
  p->f = o.f;
  return (p->f == nullptr) ? luaL_fileresulti(L, 0, filename->view(), o.err) : 1;
}

/*
** function to close 'popen' files
*/
static int io_pclose(lua_State* L)
{
  LStream* p = tolstream(L);
  OsSystemResult r = os_pclose(p->f);
  return luaL_execresulti(L, r.stat, r.err);
}

static int io_popen(lua_State* L)
{
  const InternalString* filename = luaL_checkistring(L, 1);
  const InternalString* mode = luaL_optistring(L, 2, lit<"r">());
  LStream* p = newprefile(L);
  luaL_argchecki(L, l_checkmodep(mode->view()), 2, lit<"invalid mode">()->view());
  os_flushall();
  OsOpenResult o = os_popen(filename->view(), mode->view());
  p->f = o.f;
  p->closef = &io_pclose;
  return (p->f == nullptr) ? luaL_fileresulti(L, 0, filename->view(), o.err) : 1;
}

static int io_tmpfile(lua_State* L)
{
  LStream* p = newfile(L);
  OsOpenResult o = os_tmpfile();
  p->f = o.f;
  return (p->f == nullptr) ? luaL_fileresulti(L, 0, {}, o.err) : 1;
}

static OsFile* getiofile(lua_State* L, std::string_view findex)
{
  LStream* p;
  lua_getfieldi(L, LUA_REGISTRYINDEX, findex);
  p = (LStream*) lua_touserdata(L, -1);
  if (l_unlikely(isclosed(p)))
  {
    std::string_view name = findex.substr(IOPREF_LEN);
    lua_pushlstring(L, name, 0, name.size());
    luaL_errori(L, lit<"default %s file is closed">()->view());
  }
  return p->f;
}

static int g_iofile(lua_State* L, std::string_view f, const char* mode)
{
  if (!lua_isnoneornil(L, 1))
  {
    if (lua_isstring(L, 1))
    {
      const InternalString* filename = lua_toistring(L, 1);
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

static int io_input(lua_State* L)
{
  return g_iofile(L, IO_INPUT->view(), "r");
}

static int io_output(lua_State* L)
{
  return g_iofile(L, IO_OUTPUT->view(), "w");
}

static int io_readline(lua_State* L);

/*
** maximum number of arguments to 'f:lines'/'io.lines' (it + 3 must fit
** in the limit for upvalues of a closure)
*/
constexpr int MAXARGLINE = 250;

/*
** Auxiliary function to create the iteration function for 'lines'.
** The iteration function is a closure over 'io_readline', with
** the following upvalues:
** 1) The file being read (first value in the stack)
** 2) the number of arguments to read
** 3) a boolean, true iff file has to be closed when finished ('toclose')
** *) a variable number of format arguments (rest of the stack)
*/
static void aux_lines(lua_State* L, int toclose)
{
  int n = lua_gettop(L) - 1; /* number of arguments to read */
  luaL_argchecki(L, n <= MAXARGLINE, MAXARGLINE + 2, lit<"too many arguments">()->view());
  lua_pushvalue(L, 1);         /* file */
  lua_pushinteger(L, n);       /* number of arguments to read */
  lua_pushboolean(L, toclose); /* close/not close file when finished */
  lua_rotate(L, 2, 3);         /* move the three values to their positions */
  lua_pushcclosure(L, io_readline, 3 + n);
}

static int f_lines(lua_State* L)
{
  tofile(L); /* check that it's a valid file handle */
  aux_lines(L, 0);
  return 1;
}

/*
** Return an iteration function for 'io.lines'. If file has to be
** closed, also returns the file itself as a second result (to be
** closed as the state at the exit of a generic for).
*/
static int io_lines(lua_State* L)
{
  int toclose;
  if (lua_isnone(L, 1))
    lua_pushnil(L); /* at least one argument */
  if (lua_isnil(L, 1))
  {                                               /* no file name? */
    lua_getfieldi(L, LUA_REGISTRYINDEX, IO_INPUT->view()); /* get default input */
    lua_replace(L, 1);                            /* put it at index 1 */
    tofile(L);                                    /* check that it's a valid file handle */
    toclose = 0;                                  /* do not close it after iteration */
  }
  else
  { /* open a new file */
    const InternalString* filename = luaL_checkistring(L, 1);
    opencheck(L, filename, "r");
    lua_replace(L, 1); /* put file at index 1 */
    toclose = 1;       /* close it after iteration */
  }
  aux_lines(L, toclose); /* push iteration function */
  if (toclose)
  {
    lua_pushnil(L);      /* state */
    lua_pushnil(L);      /* control */
    lua_pushvalue(L, 1); /* file is the to-be-closed variable (4th result) */
    return 4;
  }
  else
    return 1;
}

/*
** {======================================================
** READ
** =======================================================
*/

/* maximum length of a numeral */
static constexpr int L_MAXLENNUM = 200;

/* auxiliary structure used by 'read_number' */
struct RN
{
  OsFile* f;                    /* file being read */
  int c;                      /* current character (look ahead) */
  int n;                      /* number of elements in buffer 'buff' */
  char buff[L_MAXLENNUM + 1]; /* +1 for ending '\0' */
};

/*
** Add current char to buffer (if not out of space) and read next one
*/
static int nextc(RN* rn)
{
  if (l_unlikely(rn->n >= L_MAXLENNUM))
  {                     /* buffer overflow? */
    rn->buff[0] = '\0'; /* invalidate result */
    return 0;           /* fail */
  }
  else
  {
    rn->buff[rn->n++] = ((char)(rn->c)); /* save current char */
    rn->c = os_getbyte(rn->f);                   /* read next one */
    return 1;
  }
}

/*
** Accept current char if it is in 'set' (of size 2)
*/
static int test2(RN* rn, std::string_view set)
{
  if (rn->c == set[0] || rn->c == set[1])
    return nextc(rn);
  else
    return 0;
}

/*
** Read a sequence of (hex)digits
*/
static int readdigits(RN* rn, int hex)
{
  int count = 0;
  while ((hex ? isxdigit(rn->c) : isdigit(rn->c)) && nextc(rn))
    count++;
  return count;
}

/*
** Read a number: first reads a valid prefix of a numeral into a buffer.
** Then it calls 'lua_stringtonumber' to check whether the format is
** correct and to convert it to a Lua number.
*/
static int read_number(lua_State* L, OsFile* f)
{
  RN rn;
  int count = 0;
  int hex = 0;
  char decp[2];
  rn.f = f;
  rn.n = 0;
  decp[0] = ((char)(lua_getlocaledecpoint())); /* get decimal point from locale */
  decp[1] = '.';                               /* always accept a dot */
  os_flockfile(rn.f);
  do
  {
    rn.c = os_getbyte(rn.f);
  } while (isspace(rn.c)); /* skip spaces */
  test2(&rn, lit<"-+">()->view());        /* optional sign */
  if (test2(&rn, lit<"00">()->view()))
  {
    if (test2(&rn, lit<"xX">()->view()))
      hex = 1; /* numeral is hexadecimal */
    else
      count = 1; /* count initial '0' as a valid digit */
  }
  count += readdigits(&rn, hex);   /* integral part */
  if (test2(&rn, std::string_view{decp, 2}))
    count += readdigits(&rn, hex); /* fractional part */
  if (count > 0 && test2(&rn, (hex ? lit<"pP">()->view() : lit<"eE">()->view())))
  {                     /* exponent mark? */
    test2(&rn, lit<"-+">()->view());   /* exponent sign */
    readdigits(&rn, 0); /* exponent digits */
  }
  os_ungetbyte(rn.f, rn.c); /* unread look-ahead char */
  os_funlockfile(rn.f);
  rn.buff[rn.n] = '\0'; /* finish string */
  if (l_likely(lua_stringtonumber(L, rn.buff)))
    return 1; /* ok, it is a valid number */
  else
  {                 /* invalid format */
    lua_pushnil(L); /* "result" to be removed */
    return 0;       /* read fails */
  }
}

static int test_eof(lua_State* L, OsFile* f)
{
  int c = os_getbyte(f);
  os_ungetbyte(f, c); /* no-op when c == OS_EOF */
  lua_pushistringref(L, lit<"">()->view());
  return (c != OS_EOF);
}

static int read_line(lua_State* L, OsFile* f, int chop)
{
  luaL_Buffer b;
  int c;
  luaL_buffinit(L, &b);
  do
  {                                   /* may need to read several chunks to get whole line */
    size_t off = luaL_prepbuffer(&b); /* preallocate buffer space */
    std::span<char> buff = luaL_buffarea(&b);
    unsigned i = 0;
    os_flockfile(f); /* no memory errors can happen inside the lock */
    while (i < LUAL_BUFFERSIZE && (c = os_getbyte(f)) != OS_EOF && c != '\n')
      buff[off + i++] = ((char)(c)); /* read up to end of line or buffer limit */
    os_funlockfile(f);
    luaL_addsize(&b, i);
  } while (c != OS_EOF && c != '\n'); /* repeat until end of line */
  if (!chop && c == '\n')   /* want a newline and have one? */
    luaL_addchar(&b, '\n'); /* add ending newline to result */
  luaL_pushresult(&b);      /* close buffer */
  /* return ok if read something (either a newline or something else) */
  return (c == '\n' || lua_rawlen(L, -1) > 0);
}

static void read_all(lua_State* L, OsFile* f)
{
  size_t nr;
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  do
  { /* read file in chunks of LUAL_BUFFERSIZE bytes */
    size_t off = luaL_prepbuffer(&b);
    nr = os_fread(f, std::span<char>{luaL_buffarea(&b).data() + off, LUAL_BUFFERSIZE});
    luaL_addsize(&b, nr);
  } while (nr == LUAL_BUFFERSIZE);
  luaL_pushresult(&b); /* close buffer */
}

static int read_chars(lua_State* L, OsFile* f, size_t n)
{
  size_t nr; /* number of chars actually read */
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  size_t off = luaL_prepbuffsize(&b, n); /* prepare buffer to read whole block */
  nr = os_fread(f, std::span<char>{luaL_buffarea(&b).data() + off, n}); /* try to read 'n' chars */
  luaL_addsize(&b, nr);
  luaL_pushresult(&b); /* close buffer */
  return (nr > 0);     /* true iff read something */
}

static int g_read(lua_State* L, OsFile* f, int first)
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
    luaL_checkstacki(L, nargs + LUA_MINSTACK, lit<"too many arguments">()->view());
    success = 1;
    for (n = first; nargs-- && success; n++)
    {
      if (lua_type(L, n) == LUA_TNUMBER)
      {
        size_t l = (size_t) luaL_checkinteger(L, n);
        success = (l == 0) ? test_eof(L, f) : read_chars(L, f, l);
      }
      else
      {
        const InternalString* fmt = luaL_checkistring(L, n);
        size_t fp = 0;
        if (sv_byteat(fmt->view(), fp) == '*')
          fp++; /* skip optional '*' (for compatibility) */
        switch (sv_byteat(fmt->view(), fp))
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
          return luaL_argerrori(L, n, lit<"invalid format">()->view());
        }
      }
    }
  }
  if (os_ferror(f))
    return luaL_fileresulti(L, 0, {}, errno);
  if (!success)
  {
    lua_pop(L, 1);    /* remove last result */
    luaL_pushfail(L); /* push nil instead */
  }
  return n - first;
}

static int io_read(lua_State* L)
{
  return g_read(L, getiofile(L, IO_INPUT->view()), 1);
}

static int f_read(lua_State* L)
{
  return g_read(L, tofile(L), 2);
}

/*
** Iteration function for 'lines'.
*/
static int io_readline(lua_State* L)
{
  LStream* p = (LStream*) lua_touserdata(L, lua_upvalueindex(1));
  int i;
  int n = (int) lua_tointeger(L, lua_upvalueindex(2));
  if (isclosed(p)) /* file is already closed? */
    return luaL_errori(L, lit<"file is already closed">()->view());
  lua_settop(L, 1);
  luaL_checkstacki(L, n, lit<"too many arguments">()->view());
  for (i = 1; i <= n; i++) /* push arguments to 'g_read' */
    lua_pushvalue(L, lua_upvalueindex(3 + i));
  n = g_read(L, p->f, 2);   /* 'n' is number of results */
  assert(n > 0);        /* should return at least a nil */
  if (lua_toboolean(L, -n)) /* read at least one value? */
    return n;               /* return them */
  else
  { /* first result is false: EOF or error */
    if (n > 1)
    { /* is there error information? */
      /* 2nd result is error message */
      lua_pushvalue(L, -n + 1);
      return luaL_errori(L, lit<"%s">()->view());
    }
    if (lua_toboolean(L, lua_upvalueindex(3)))
    {                                        /* generator created file? */
      lua_settop(L, 0);                      /* clear stack */
      lua_pushvalue(L, lua_upvalueindex(1)); /* push file at index 1 */
      aux_close(L);                          /* close it */
    }
    return 0;
  }
}

/* }====================================================== */

static int g_write(lua_State* L, OsFile* f, int arg)
{
  int nargs = lua_gettop(L) - arg;
  size_t totalbytes = 0; /* total number of bytes written */
  errno = 0;
  for (; nargs--; arg++)
  { /* for each argument */
    const char* s;
    size_t len;
    size_t numbytes; /* bytes written in one call to 'fwrite' */
    char numbuff[LUA_N2SBUFFSZ];
    std::string_view numtxt = lua_numbertocstringi(L, arg, numbuff); /* try as a number */
    if (numtxt.data() != nullptr)
    { /* did conversion work (value was a number)? */
      s = numtxt.data();
      len = numtxt.size();
    }
    else
    { /* must be a string */
      std::string_view sv = luaL_checklstring(L, arg);
      s = sv.data();
      len = sv.size();
    }
    numbytes = os_fwrite(f, std::string_view{s, len});
    totalbytes += numbytes;
    if (numbytes < len)
    { /* write error? */
      int n = luaL_fileresulti(L, 0, {}, errno);
      lua_pushinteger(L, ((lua_Integer)(totalbytes)));
      return n + 1; /* return fail, error msg., error code, and counter */
    }
  }
  return 1; /* no errors; file handle already on stack top */
}

static int io_write(lua_State* L)
{
  return g_write(L, getiofile(L, IO_OUTPUT->view()), 1);
}

static int f_write(lua_State* L)
{
  OsFile* f = tofile(L);
  lua_pushvalue(L, 1); /* push file at the stack top (to be returned) */
  return g_write(L, f, 2);
}

static int f_seek(lua_State* L)
{
  static const std::string_view modenames[] = {lit<"set">()->view(), lit<"cur">()->view(), lit<"end">()->view()};
  static const std::string_view defmode = lit<"cur">()->view();
  OsFile* f = tofile(L);
  int op = luaL_checkoption(L, 2, defmode, modenames);
  lua_Integer p3 = luaL_optinteger(L, 3, 0);
  long long offset = (long long) p3;
  luaL_argchecki(L, (lua_Integer) offset == p3, 3, lit<"not an integer in proper range">()->view());
  int serr = os_fseek(f, offset, op);
  if (l_unlikely(serr != 0))
    return luaL_fileresulti(L, 0, {}, serr); /* error */
  else
  {
    lua_pushinteger(L, (lua_Integer) os_ftell(f));
    return 1;
  }
}

static int f_setvbuf(lua_State* L)
{
  static const std::string_view modenames[] = {lit<"no">()->view(), lit<"full">()->view(), lit<"line">()->view()};
  OsFile* f = tofile(L);
  int op = luaL_checkoption(L, 2, {}, modenames);
  lua_Integer sz = luaL_optinteger(L, 3, LUAL_BUFFERSIZE);
  int res = os_setvbuf(f, op, (size_t) sz);
  return luaL_fileresulti(L, res == 0, {}, res);
}

static int aux_flush(lua_State* L, OsFile* f)
{
  errno = 0;
  int err = os_fflush(f);
  return luaL_fileresulti(L, err == 0, {}, err);
}

static int f_flush(lua_State* L)
{
  return aux_flush(L, tofile(L));
}

static int io_flush(lua_State* L)
{
  return aux_flush(L, getiofile(L, IO_OUTPUT->view()));
}

/*
** functions for 'io' library
*/
static const luaL_Reg iolib[] = {
    {lit<"close">()->view(), io_close},
    {lit<"flush">()->view(), io_flush},
    {lit<"input">()->view(), io_input},
    {lit<"lines">()->view(), io_lines},
    {lit<"open">()->view(), io_open},
    {lit<"output">()->view(), io_output},
    {lit<"popen">()->view(), io_popen},
    {lit<"read">()->view(), io_read},
    {lit<"tmpfile">()->view(), io_tmpfile},
    {lit<"type">()->view(), io_type},
    {lit<"write">()->view(), io_write},
    {{}, nullptr}};

/*
** methods for file handles
*/
static const luaL_Reg meth[] = {
    {lit<"read">()->view(), f_read},
    {lit<"write">()->view(), f_write},
    {lit<"lines">()->view(), f_lines},
    {lit<"flush">()->view(), f_flush},
    {lit<"seek">()->view(), f_seek},
    {lit<"close">()->view(), f_close},
    {lit<"setvbuf">()->view(), f_setvbuf},
    {{}, nullptr}};

/*
** metamethods for file handles
*/
static const luaL_Reg metameth[] = {
    {lit<"__index">()->view(), nullptr}, /* placeholder */
    {lit<"__gc">()->view(), f_gc},
    {lit<"__close">()->view(), f_gc},
    {lit<"__tostring">()->view(), f_tostring},
    {{}, nullptr}};

static void createmeta(lua_State* L)
{
  luaL_newmetatablei(L, LUA_FILEHANDLE->view()); /* metatable for file handles */
  luaL_setfuncs(L, metameth, 0);        /* add metamethods to new metatable */
  lua_createtable(L, 0, sizeof(meth) / sizeof((meth)[0]) - 1);            /* create method table */
  luaL_setfuncs(L, meth, 0);            /* add file methods to method table */
  lua_setfieldi(L, -2, lit<"__index">()->view());       /* metatable.__index = method table */
  lua_pop(L, 1);                        /* pop metatable */
}

/*
** function to (not) close the standard files stdin, stdout, and stderr
*/
static int io_noclose(lua_State* L)
{
  LStream* p = tolstream(L);
  p->closef = &io_noclose; /* keep file opened */
  luaL_pushfail(L);
  lua_pushistringref(L, lit<"cannot close standard file">()->view());
  return 2;
}

static void createstdfile(lua_State* L, OsFile* f, std::string_view k,
                          const InternalString* fname)
{
  LStream* p = newprefile(L);
  p->f = f;
  p->closef = &io_noclose;
  if (!k.empty())
  {
    lua_pushvalue(L, -1);
    lua_setfieldi(L, LUA_REGISTRYINDEX, k); /* add file to registry */
  }
  lua_setfieldi(L, -2, fname->view()); /* add file to module */
}

LUAMOD_API int luaopen_io(lua_State* L)
{
  (luaL_checkversion(L), lua_createtable(L, 0, sizeof(iolib) / sizeof((iolib)[0]) - 1), luaL_setfuncs(L, iolib, 0)); /* new module */
  createmeta(L);
  /* create (and set) default files */
  createstdfile(L, os_stdin(), IO_INPUT->view(), lit<"stdin">());
  createstdfile(L, os_stdout(), IO_OUTPUT->view(), lit<"stdout">());
  createstdfile(L, os_stderr(), "", lit<"stderr">());
  return 1;
}
