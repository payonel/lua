using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using System.Threading;
using Payonel.LungoLua.Compat;
using IOException = System.IO.IOException;
using Process = System.Diagnostics.Process;
using HostString = Payonel.LungoLua.Api.HostString;
using lua_CFunction = Payonel.LungoLua.Api.lua_CFunction;
using ByteSpan = Payonel.LungoLua.Bytes.ByteSpan;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using InternalString = Payonel.LungoLua.Bytes.InternalString;
using Lt = Payonel.LungoLua.Bytes.Lt;

namespace Payonel.LungoLua.Platform;

/*
** The operating system as the engine sees it (os_api.h), on .NET: what
** the C++ takes from the C library and POSIX, with the same results and
** the same error conventions (errno codes, and the messages glibc's
** 'strerror' gives on Linux). Paths and commands cross to .NET as UTF-8
** (HostString's conversion), cut at their first '\0' as a C string is,
** and .NET converts them to the system's bytes as UTF-8; the environment
** is read as bytes (Environ).
*/
public sealed class OsApi
{
  private OsApi()
  {
  }

  /* C's 'errno' (the last error of the functions here, as the C library sets it) */
  public static int errno = 0;

  /* stdio's BUFSIZ (glibc's) */
  public const int BUFSIZ = 8192;

  public const int OS_EOF = -1;

  /* <cstdlib>'s exit statuses */
  public const int EXIT_SUCCESS = 0;
  public const int EXIT_FAILURE = 1;

  /* errno values (Linux) */
  public const int EPERM = 1;
  public const int ENOENT = 2;
  public const int EIO = 5;
  public const int ENOEXEC = 8;
  public const int EBADF = 9;
  public const int ECHILD = 10;
  public const int EAGAIN = 11;
  public const int ENOMEM = 12;
  public const int EACCES = 13;
  public const int EEXIST = 17;
  public const int EXDEV = 18;
  public const int ENOTDIR = 20;
  public const int EISDIR = 21;
  public const int EINVAL = 22;
  public const int ESPIPE = 29;
  public const int EPIPE = 32;
  public const int ENAMETOOLONG = 36;
  public const int ENOTEMPTY = 39;
  public const int ELOOP = 40;
  public const int EOVERFLOW = 75;

  /* glibc's messages, by errno */
  private static readonly string[] errmsgs = {
    "Success", /* 0 */
    "Operation not permitted", /* 1 */
    "No such file or directory", /* 2 */
    "No such process", /* 3 */
    "Interrupted system call", /* 4 */
    "Input/output error", /* 5 */
    "No such device or address", /* 6 */
    "Argument list too long", /* 7 */
    "Exec format error", /* 8 */
    "Bad file descriptor", /* 9 */
    "No child processes", /* 10 */
    "Resource temporarily unavailable", /* 11 */
    "Cannot allocate memory", /* 12 */
    "Permission denied", /* 13 */
    "Bad address", /* 14 */
    "Block device required", /* 15 */
    "Device or resource busy", /* 16 */
    "File exists", /* 17 */
    "Invalid cross-device link", /* 18 */
    "No such device", /* 19 */
    "Not a directory", /* 20 */
    "Is a directory", /* 21 */
    "Invalid argument", /* 22 */
    "Too many open files in system", /* 23 */
    "Too many open files", /* 24 */
    "Inappropriate ioctl for device", /* 25 */
    "Text file busy", /* 26 */
    "File too large", /* 27 */
    "No space left on device", /* 28 */
    "Illegal seek", /* 29 */
    "Read-only file system", /* 30 */
    "Too many links", /* 31 */
    "Broken pipe", /* 32 */
    "Numerical argument out of domain", /* 33 */
    "Numerical result out of range", /* 34 */
    "Resource deadlock avoided", /* 35 */
    "File name too long", /* 36 */
    "No locks available", /* 37 */
    "Function not implemented", /* 38 */
    "Directory not empty", /* 39 */
    "Too many levels of symbolic links", /* 40 */
    "Unknown error 41", /* 41 */
    "No message of desired type", /* 42 */
    "Identifier removed", /* 43 */
    "Channel number out of range", /* 44 */
    "Level 2 not synchronized", /* 45 */
    "Level 3 halted", /* 46 */
    "Level 3 reset", /* 47 */
    "Link number out of range", /* 48 */
    "Protocol driver not attached", /* 49 */
    "No CSI structure available", /* 50 */
    "Level 2 halted", /* 51 */
    "Invalid exchange", /* 52 */
    "Invalid request descriptor", /* 53 */
    "Exchange full", /* 54 */
    "No anode", /* 55 */
    "Invalid request code", /* 56 */
    "Invalid slot", /* 57 */
    "Unknown error 58", /* 58 */
    "Bad font file format", /* 59 */
    "Device not a stream", /* 60 */
    "No data available", /* 61 */
    "Timer expired", /* 62 */
    "Out of streams resources", /* 63 */
    "Machine is not on the network", /* 64 */
    "Package not installed", /* 65 */
    "Object is remote", /* 66 */
    "Link has been severed", /* 67 */
    "Advertise error", /* 68 */
    "Srmount error", /* 69 */
    "Communication error on send", /* 70 */
    "Protocol error", /* 71 */
    "Multihop attempted", /* 72 */
    "RFS specific error", /* 73 */
    "Bad message", /* 74 */
    "Value too large for defined data type", /* 75 */
    "Name not unique on network", /* 76 */
    "File descriptor in bad state", /* 77 */
    "Remote address changed", /* 78 */
    "Can not access a needed shared library", /* 79 */
    "Accessing a corrupted shared library", /* 80 */
    ".lib section in a.out corrupted", /* 81 */
    "Attempting to link in too many shared libraries", /* 82 */
    "Cannot exec a shared library directly", /* 83 */
    "Invalid or incomplete multibyte or wide character", /* 84 */
    "Interrupted system call should be restarted", /* 85 */
    "Streams pipe error", /* 86 */
    "Too many users", /* 87 */
    "Socket operation on non-socket", /* 88 */
    "Destination address required", /* 89 */
    "Message too long", /* 90 */
    "Protocol wrong type for socket", /* 91 */
    "Protocol not available", /* 92 */
    "Protocol not supported", /* 93 */
    "Socket type not supported", /* 94 */
    "Operation not supported", /* 95 */
    "Protocol family not supported", /* 96 */
    "Address family not supported by protocol", /* 97 */
    "Address already in use", /* 98 */
    "Cannot assign requested address", /* 99 */
    "Network is down", /* 100 */
    "Network is unreachable", /* 101 */
    "Network dropped connection on reset", /* 102 */
    "Software caused connection abort", /* 103 */
    "Connection reset by peer", /* 104 */
    "No buffer space available", /* 105 */
    "Transport endpoint is already connected", /* 106 */
    "Transport endpoint is not connected", /* 107 */
    "Cannot send after transport endpoint shutdown", /* 108 */
    "Too many references: cannot splice", /* 109 */
    "Connection timed out", /* 110 */
    "Connection refused", /* 111 */
    "Host is down", /* 112 */
    "No route to host", /* 113 */
    "Operation already in progress", /* 114 */
    "Operation now in progress", /* 115 */
    "Stale file handle", /* 116 */
    "Structure needs cleaning", /* 117 */
    "Not a XENIX named type file", /* 118 */
    "No XENIX semaphores available", /* 119 */
    "Is a named type file", /* 120 */
    "Remote I/O error", /* 121 */
    "Disk quota exceeded", /* 122 */
    "No medium found", /* 123 */
    "Wrong medium type", /* 124 */
    "Operation canceled", /* 125 */
    "Required key not available", /* 126 */
    "Key has expired", /* 127 */
    "Key has been revoked", /* 128 */
    "Key was rejected by service", /* 129 */
    "Owner died", /* 130 */
    "State not recoverable", /* 131 */
    "Operation not possible due to RF-kill", /* 132 */
    "Memory page has hardware error" /* 133 */
  };

  private static readonly HashMap<string, int> errcodes = new ();

  private static readonly bool static_init_1 = static_init_1_run();
  private static bool static_init_1_run()
  {
    for (int i = errmsgs.Length - 1; i > 0; i--)
      errcodes.put(errmsgs[i], i);
    errcodes.put("Stream closed", EBADF);
    errcodes.put("Bad file number", EBADF);
  return true; }

  /* the errno of a .NET I/O error */
  internal static int errnoOf(Exception e)
  {
    if (e is OsFile.WriteError w)
      e = w.cause;
    if (e is Win32Exception x) /* (a process that did not start) */
      return x.NativeErrorCode;
    if (e is UnauthorizedAccessException && e.InnerException is IOException)
      e = e.InnerException; /* (EACCES, EPERM, EBADF: the errno's error inside) */
    if (e is IOException && e.HResult > 0 && e.HResult < errmsgs.Length)
      return e.HResult; /* (.NET's I/O errors carry their errno so) */
    if (e is FileNotFoundException)
      return ENOENT;
    if (e is DirectoryNotFoundException)
    { /* ENOENT or ENOTDIR: the path is in its message */
      string m = e.Message;
      int a = m.indexOf('\''), b = m.lastIndexOf('\'');
      return (a >= 0 && b > a) ? enoent(m.substring(a + 1, b)) : ENOENT;
    }
    if (e is PathTooLongException)
      return ENAMETOOLONG;
    if (e is UnauthorizedAccessException)
      return EACCES;
    if (e is ObjectDisposedException)
      return EBADF;
    string msg = e.Message;
    if (msg != null)
    {
      int c = errnoOfMessage(msg);
      if (c != 0)
        return c;
      int open = msg.lastIndexOf('(');
      if (open >= 0 && msg.endsWith(")"))
      {
        c = errnoOfMessage(msg.substring(open + 1, msg.length() - 1));
        if (c != 0)
          return c;
      }
    }
    return EIO;
  }

  /* is it an error of the system's (not of the program)? */
  internal static bool isioerror(Exception e)
  {
    return e is IOException || e is UnauthorizedAccessException || e is Win32Exception || e is ObjectDisposedException
        || e is NotSupportedException || e is ArgumentException;
  }

  /* the error of a path that names nothing: ENOTDIR if one of its directories is not one */
  private static int enoent(string p)
  {
    for (int i = p.indexOf('/', 1); i > 0; i = p.indexOf('/', i + 1))
      if (File.Exists(p.substring(0, i)))
        return ENOTDIR;
    return ENOENT;
  }

  /* an error of errno 'en' (as .NET's own carry it) */
  private static IOException ioerror(int en)
  {
    return new IOException(errmsgs[en], en);
  }

  private static int errnoOfMessage(string m)
  {
    int c = errcodes.getOrDefault(m, 0);
    if (c != 0)
      return c;
    for (int i = 1; i < errmsgs.Length; i++)
      if (m.startsWith(errmsgs[i]) && !errmsgs[i].startsWith("Unknown error"))
        return i;
    return 0;
  }

  /* a C string's text: the bytes up to the first '\0', as UTF-8 */
  private static string cstr(ByteView v)
  {
    int z = v.find(0);
    return HostString.from_bytes((z == ByteView.npos) ? v : v.substr(0, z)).to_os();
  }

  /* the path a C string names ("" names none) */
  private static string path(ByteView v)
  {
    string s = cstr(v);
    if (s.isEmpty())
      throw ioerror(ENOENT);
    return s;
  }

  /* the file a path names, its links followed (as 'realpath', but the links of its directories stay) */
  private static string realpath(string p)
  {
    string full = System.IO.Path.GetFullPath(p);
    FileSystemInfo i = Directory.Exists(full) ? new DirectoryInfo(full) : new FileInfo(full);
    FileSystemInfo t = (i.LinkTarget != null) ? i.ResolveLinkTarget(true) : i;
    if (t == null || !t.Exists)
      throw ioerror(enoent(full));
    return t.FullName;
  }

  /* does it name a directory (not a link to one)? */
  private static bool isdir(string p)
  {
    return Directory.Exists(p) && new DirectoryInfo(p).LinkTarget == null;
  }

  /*
  ** {======================================================
  ** Standard streams, and what C's exit flushes
  ** =======================================================
  */

  private static readonly System.Collections.Generic.List<OsFile> openfiles = new ();

  internal static void remember(OsFile f)
  {
    lock (openfiles)
    {
      if (!openfiles.Contains(f))
        openfiles.Add(f);
    }
  }

  internal static void forget(OsFile f)
  {
    lock (openfiles)
    {
      openfiles.Remove(f);
    }
  }

  /* C's 'isatty' (glibc decides a stream's buffering by it) */
  public static bool isatty(int fd)
  {
    try
    {
      string l = new FileInfo("/proc/self/fd/" + fd).LinkTarget;
      if (l == null)
        throw ioerror(EINVAL);
      return isttypath(l);
    }
    catch (Exception e)
    {
      return fd != 2 && !Console.IsInputRedirected && !Console.IsOutputRedirected;
    }
  }

  private static bool isttypath(string p)
  {
    return p.startsWith("/dev/pts/") || p.startsWith("/dev/tty") || p.equals("/dev/console");
  }

  private static readonly OsFile stdinf = OsFile.standard(0, true, isatty(0) ? OsFile.IOLBF : OsFile.IOFBF);
  private static readonly OsFile stdoutf = OsFile.standard(1, false, isatty(1) ? OsFile.IOLBF : OsFile.IOFBF);
  private static readonly OsFile stderrf = OsFile.standard(2, false, OsFile.IONBF);

  private static readonly bool static_init_2 = static_init_2_run();
  private static bool static_init_2_run()
  {
    remember(stdinf);
    remember(stdoutf);
    remember(stderrf);
    /* C's 'exit' (and a return from 'main') flushes the open streams */
    AppDomain.CurrentDomain.ProcessExit += (sender, e) => os_flushall();
  return true; }

  /* C's 'exit': the process ends, the open streams flushed */
  public static void exit(int status)
  {
    os_flushall();
    JSystem.exit(status);
  }

  /* glibc flushes a line-buffered stdout before reading from a line-buffered or unbuffered stream */
  internal static void flushlinestdout(OsFile reading)
  {
    if (reading != stdoutf && stdoutf.bufmode == OsFile.IOLBF && stdoutf.wlen > 0)
      stdoutf.flushout();
  }

  /* }====================================================== */

  public static void os_print_stdout(ByteView utf8)
  {
    stdoutf.write(utf8.array(), utf8.offset(), utf8.size());
  }

  public static void os_print_stderr(ByteView utf8)
  {
    stderrf.write(utf8.array(), utf8.offset(), utf8.size());
  }

  public static void os_flush_stdout()
  {
    stdoutf.flush();
  }

  public static void os_flush_stderr()
  {
    stderrf.flush();
  }

  public static ByteView os_getenv(ByteView name)
  {
    int z = name.find(0);
    return Environ.getenv((z == ByteView.npos) ? name : name.substr(0, z));
  }

  public static ByteView os_strerror(int err)
  {
    string m = (err >= 0 && err < errmsgs.Length) ? errmsgs[err] : "Unknown error " + err;
    return ByteView.of(m);
  }

  /* 'strcoll' in the "C" locale: 'strcmp' of the C strings */
  public static int os_strcoll(ByteView a, ByteView b)
  {
    for (int i = 0;; i++)
    {
      int ca = (i < a.size()) ? a.uat(i) : 0;
      int cb = (i < b.size()) ? b.uat(i) : 0;
      if (ca != cb)
        return ca - cb;
      if (ca == 0)
        return 0;
    }
  }

  /*
  ** {======================================================
  ** Time
  ** =======================================================
  */

  public static double os_cpuclock()
  {
    long ns = -1;
    try
    {
      ns = Environment.CpuUsage.TotalTime.Ticks * 100;
    }
    catch (Exception e)
    {
      ns = -1;
    }
    if (ns < 0)
      return -1.0 / 1000000.0;
    return ((double) (ns / 1000)) / 1000000.0; /* (clock_t ticks: microseconds) */
  }

  public static long os_now()
  {
    return JMath.floorDiv(JSystem.currentTimeMillis(), 1000);
  }

  /* days since 1970-01-01 of a date of the proleptic Gregorian calendar */
  private static long days_from_civil(long y, int m, int d)
  {
    y -= (m <= 2) ? 1 : 0;
    long era = JMath.floorDiv(y, 400);
    long yoe = y - era * 400;
    long doy = (153 * (m + ((m > 2) ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
  }

  /*
  ** The local zone as glibc reads it ('tzset', 'tzfile'): the TZif file
  ** that TZ names (or /etc/localtime), with the POSIX TZ string at its end
  ** for the times after its last transition; or TZ itself as a POSIX TZ
  ** string; or UTC. (Without a zone file and with TZ unset, as on systems
  ** with no /etc/localtime, .NET's zone, TimeZoneInfo.Local.)
  */

  /* a rule of a POSIX TZ string: when its time starts in a year (glibc's 'tz_rule') */
  private sealed class TzRule
  {
    internal string name = "";
    internal int offset = 0;  /* seconds east of UTC */
    internal int type = J0;
    internal int m = 0;
    internal int n = 0;
    internal int d = 0;
    internal int secs = 0;    /* the time of day of the change */
  }

  private const int J0 = 0; /* n: day of the year, from 0 */
  private const int J1 = 1; /* Jn: day of the year, from 1, without February 29 */
  private const int M = 2;  /* Mm.n.d: day 'd' of week 'n' of month 'm' */

  private sealed class TzZone
  {
    internal bool tzfile = false;      /* a TZif file (else: its rules only) */
    internal long[] trans = new long[0];  /* transition times */
    internal int[] idx = new int[0];      /* the type each one starts */
    internal int[] utoff = new int[0];    /* the types: offsets, */
    internal bool[] isdst = new bool[0]; /* daylight saving or not, */
    internal string[] abbr = new string[0];    /* abbreviations */
    internal TzRule[] rules = null;       /* the POSIX TZ string (standard time, daylight saving time) */
  }

  /* reading a POSIX TZ string (glibc's '__tzset_parse_tz') */
  private sealed class TzParse
  {
    internal readonly string s;
    internal int i = 0;
    internal readonly TzRule[] rules = {new TzRule(), new TzRule()};

    internal TzParse(string s)
    {
      this.s = s;
    }

    internal int ch(int k)
    {
      return (k < s.length()) ? s.charAt(k) : 0;
    }

    internal static bool isalpha(int c)
    {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    internal static bool isdigit(int c)
    {
      return c >= '0' && c <= '9';
    }

    internal bool parse_tzname(int whichrule)
    {
      int start = i;
      int p = start;
      while (isalpha(ch(p)))
        ++p;
      int len = p - start;
      if (len < 3)
      {
        p = i;
        if (ch(p++) != '<')
          return false;
        start = p;
        while (isalpha(ch(p)) || isdigit(ch(p)) || ch(p) == '+' || ch(p) == '-')
          ++p;
        len = p - start;
        if (ch(p++) != '>' || len < 3)
          return false;
      }
      rules[whichrule].name = s.substring(start, start + len);
      i = p;
      return true;
    }

    /* up to three numbers separated by ':' ("%hu%n:%hu%n:%hu%n"): their count; the end in 'consumed' */
    internal int consumed;

    internal int scanhms(int[] v)
    {
      int p = i, k = 0;
      consumed = 0;
      while (k < 3)
      {
        if (k > 0)
        {
          if (ch(p) != ':')
            break;
          p++;
        }
        if (!isdigit(ch(p)))
          break;
        int x = 0;
        while (isdigit(ch(p)))
          x = (x * 10 + (ch(p++) - '0')) & 0xFFFF;
        v[k++] = x;
        consumed = p - i;
      }
      return k;
    }

    internal bool parse_offset(int whichrule)
    {
      if (whichrule == 0 && (ch(i) == 0 || (ch(i) != '+' && ch(i) != '-' && !isdigit(ch(i)))))
        return false;
      long sign;
      if (ch(i) == '+' || ch(i) == '-')
        sign = (ch(i++) == '-') ? 1L : -1L;
      else
        sign = -1L;
      int[] v = {0, 0, 0};
      if (scanhms(v) > 0)
        rules[whichrule].offset = (int) (sign * (JMath.min(v[2], 59) + JMath.min(v[1], 59) * 60 + JMath.min(v[0], 24) * 3600));
      else if (whichrule == 0)
      {
        rules[0].offset = 0;
        return false;
      }
      else
        rules[1].offset = rules[0].offset + 60 * 60;
      i += consumed;
      return true;
    }

    internal bool parse_rule(int whichrule)
    {
      TzRule tzr = rules[whichrule];
      if (ch(i) == ',')
        i++;
      if (ch(i) == 'J' || isdigit(ch(i)))
      {
        tzr.type = (ch(i) == 'J') ? J1 : J0;
        if (tzr.type == J1 && !isdigit(ch(++i)))
          return false;
        int e = i;
        long d = 0;
        while (isdigit(ch(e)) && d <= 365)
          d = d * 10 + (ch(e++) - '0');
        if (e == i || d > 365)
          return false;
        if (tzr.type == J1 && d == 0)
          return false;
        tzr.d = (int) d;
        i = e;
      }
      else if (ch(i) == 'M')
      {
        tzr.type = M;
        int[] v = new int[3];
        int p = i + 1, k = 0;
        while (k < 3)
        {
          if (k > 0)
          {
            if (ch(p) != '.')
              break;
            p++;
          }
          if (!isdigit(ch(p)))
            break;
          int x = 0;
          while (isdigit(ch(p)))
            x = (x * 10 + (ch(p++) - '0')) & 0xFFFF;
          v[k++] = x;
        }
        if (k != 3 || v[0] < 1 || v[0] > 12 || v[1] < 1 || v[1] > 5 || v[2] > 6)
          return false;
        tzr.m = v[0];
        tzr.n = v[1];
        tzr.d = v[2];
        i = p;
      }
      else if (ch(i) == 0)
      { /* the U.S. rules: "M3.2.0,M11.1.0" */
        tzr.type = M;
        tzr.m = (whichrule == 0) ? 3 : 11;
        tzr.n = (whichrule == 0) ? 2 : 1;
        tzr.d = 0;
      }
      else
        return false;
      if (ch(i) != 0 && ch(i) != '/' && ch(i) != ',')
        return false;
      else if (ch(i) == '/')
      {
        ++i;
        if (ch(i) == 0)
          return false;
        bool negative = ch(i) == '-';
        if (negative)
          i++;
        int[] v = {2, 0, 0};
        scanhms(v);
        i += consumed;
        tzr.secs = (negative ? -1 : 1) * (v[0] * 60 * 60 + v[1] * 60 + v[2]);
      }
      else
        tzr.secs = 2 * 60 * 60;
      return true;
    }

    /* the rules of the string (as glibc leaves them when it stops) */
    internal TzRule[] parse()
    {
      if (parse_tzname(0) && parse_offset(0))
      {
        if (ch(i) != 0)
        {
          if (parse_tzname(1))
            parse_offset(1);
          if (parse_rule(0))
            parse_rule(1);
        }
        else
        {
          rules[1].name = rules[0].name;
          rules[1].offset = rules[0].offset;
        }
      }
      return rules;
    }
  }

  private static int be32(sbyte[] b, int p)
  {
    return ((b[p] & 0xFF) << 24) | ((b[p + 1] & 0xFF) << 16) | ((b[p + 2] & 0xFF) << 8) | (b[p + 3] & 0xFF);
  }

  private static long be64(sbyte[] b, int p)
  {
    return ((long) be32(b, p) << 32) | (be32(b, p + 4) & 0xFFFFFFFFL);
  }

  /* a TZif file, or null */
  private static TzZone readtzif(string p)
  {
    try
    {
      sbyte[] b = (sbyte[]) (object) File.ReadAllBytes(p);
      if (b.Length < 44 || b[0] != 'T' || b[1] != 'Z' || b[2] != 'i' || b[3] != 'f')
        return null;
      TzZone z = new TzZone();
      z.tzfile = true;
      int pos = 0;
      int tsize = 4;
      if (b[4] >= '2')
      { /* the 64-bit data, after the 32-bit data */
        int isutcnt0 = be32(b, 20), isstdcnt0 = be32(b, 24), leapcnt0 = be32(b, 28);
        int timecnt0 = be32(b, 32), typecnt0 = be32(b, 36), charcnt0 = be32(b, 40);
        pos = 44 + timecnt0 * 4 + timecnt0 + typecnt0 * 6 + charcnt0 + leapcnt0 * 8 + isstdcnt0 + isutcnt0;
        tsize = 8;
      }
      int isutcnt = be32(b, pos + 20), isstdcnt = be32(b, pos + 24), leapcnt = be32(b, pos + 28);
      int timecnt = be32(b, pos + 32), typecnt = be32(b, pos + 36), charcnt = be32(b, pos + 40);
      int q = pos + 44;
      z.trans = new long[timecnt];
      for (int i = 0; i < timecnt; i++, q += tsize)
        z.trans[i] = (tsize == 8) ? be64(b, q) : be32(b, q);
      z.idx = new int[timecnt];
      for (int i = 0; i < timecnt; i++)
        z.idx[i] = b[q++] & 0xFF;
      z.utoff = new int[typecnt];
      z.isdst = new bool[typecnt];
      int[] ai = new int[typecnt];
      for (int i = 0; i < typecnt; i++, q += 6)
      {
        z.utoff[i] = be32(b, q);
        z.isdst[i] = b[q + 4] != 0;
        ai[i] = b[q + 5] & 0xFF;
      }
      z.abbr = new string[typecnt];
      for (int i = 0; i < typecnt; i++)
      {
        int e = q + ai[i];
        while (e < q + charcnt && b[e] != 0)
          e++;
        z.abbr[i] =  JString.of(b, q + ai[i], e - q - ai[i], JCharsets.ISO_8859_1);
      }
      q += charcnt + leapcnt * (tsize + 4) + isstdcnt + isutcnt;
      if (tsize == 8 && q < b.Length && b[q] == '\n')
      {
        int e = q + 1;
        while (e < b.Length && b[e] != '\n')
          e++;
        if (e > q + 1)
          z.rules = new TzParse( JString.of(b, q + 1, e - q - 1, JCharsets.ISO_8859_1)).parse();
      }
      return z;
    }
    catch (Exception e)
    {
      return null;
    }
  }

  private static TzZone readzone()
  {
    string tz = JSystem.getenv("TZ");
    if (tz != null && tz.isEmpty())
      tz = "Universal";
    if (tz != null && tz.startsWith(":"))
      tz = tz.substring(1);
    string file = (tz == null) ? "/etc/localtime" : tz;
    string dir = JSystem.getenv("TZDIR");
    string p = file.startsWith("/") ? file : ((dir != null) ? dir : "/usr/share/zoneinfo") + "/" + file;
    TzZone z = file.isEmpty() ? null : readtzif(p);
    if (z != null)
      return z;
    z = new TzZone();
    if (tz == null || tz.isEmpty() || tz.equals("/etc/localtime"))
    { /* none: UTC (or, off Linux, .NET's zone) */
      if (tz == null && !OperatingSystem.IsLinux())
        return null;
      z.rules = new TzParse("UTC0").parse();
      return z;
    }
    z.rules = new TzParse(tz).parse();
    return z;
  }

  private static readonly TzZone tzzone = readzone();

  /* the zone without TZif data and TZ: .NET's */
  private static readonly TimeZoneInfo netzone = (tzzone == null) ? TimeZoneInfo.Local : null;

  /* 400 years, in seconds: the calendar (and every yearly rule of a zone) repeats */
  private const long CYCLE = 146097L * 86400;
  /* .NET's DateTimeOffset works between these (years 1 and 9999); times out of them fold in */
  private static readonly long TLO = days_from_civil(1, 1, 1) * 86400;
  private static readonly long THI = days_from_civil(10000, 1, 1) * 86400;

  private static long fold(long t)
  {
    if (t >= THI)
      return THI - CYCLE + JMath.floorMod(t - THI, CYCLE);
    if (t < TLO)
      return TLO + JMath.floorMod(t - TLO, CYCLE);
    return t;
  }

  private static readonly int[][] mon_ydays = {
    new[] {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    new[] {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};

  private static bool isleap(int y)
  {
    return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  }

  /* when the time of 'rule' starts in 'year' (glibc's 'compute_change', its int arithmetic too) */
  private static long compute_change(TzRule rule, int year)
  {
    long t;
    if (year > 1970)
      t = (long) ((year - 1970) * 365 + ((year - 1) / 4 - 1970 / 4) - ((year - 1) / 100 - 1970 / 100)
                  + ((year - 1) / 400 - 1970 / 400)) * 86400;
    else
      t = 0;
    switch (rule.type)
    {
    case J1:
      t += (long) (rule.d - 1) * 86400;
      if (rule.d >= 60 && isleap(year))
        t += 86400;
      break;
    case J0:
      t += (long) rule.d * 86400;
      break;
    default:
    {
      int[] myday = mon_ydays[isleap(year) ? 1 : 0];
      t += (long) myday[rule.m - 1] * 86400;
      int m1 = (rule.m + 9) % 12 + 1;
      int yy0 = (rule.m <= 2) ? (year - 1) : year;
      int yy1 = yy0 / 100;
      int yy2 = yy0 % 100;
      int dow = ((26 * m1 - 2) / 10 + 1 + yy2 + yy2 / 4 + yy1 / 4 - 2 * yy1) % 7;
      if (dow < 0)
        dow += 7;
      int d = rule.d - dow;
      if (d < 0)
        d += 7;
      for (int i = 1; i < rule.n; ++i)
      {
        if (d + 7 >= myday[rule.m] - myday[rule.m - 1])
          break;
        d += 7;
      }
      t += (long) d * 86400;
      break;
    }
    }
    return t - rule.offset + rule.secs;
  }

  private static readonly ByteView GMT = ByteView.of("GMT");

  /* the broken-down time of 't' in a zone 'off' seconds east (null if its year does not fit) */
  private static OsDate breakdown(long t, long off, int isdst, ByteView zonename)
  {
    long lt;
    try
    {
      lt = JMath.addExact(t, off);
    }
    catch (System.ArithmeticException e)
    {
      return null;
    }
    long days = JMath.floorDiv(lt, 86400);
    long rem = JMath.floorMod(lt, 86400);
    long z = days + 719468;
    long era = JMath.floorDiv(z, 146097);
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = (mp < 10) ? mp + 3 : mp - 9;
    long y = yoe + era * 400 + ((m <= 2) ? 1 : 0);
    long year = y - 1900;
    if (year < JInteger.MIN_VALUE || year > JInteger.MAX_VALUE)
      return null;
    OsDate r = new OsDate();
    r.year = (int) year;
    r.month = (int) m - 1;
    r.day = (int) d;
    r.hour = (int) (rem / 3600);
    r.min = (int) (rem % 3600 / 60);
    r.sec = (int) (rem % 60);
    r.yday = (int) (days - days_from_civil(y, 1, 1));
    r.wday = (int)(JMath.floorMod(days + 4, 7));
    r.isdst = isdst;
    r.gmtoff = off;
    r.zone = zonename;
    return r;
  }

  public static OsDate os_gmtime(long t)
  {
    return breakdown(t, 0, 0, GMT);
  }

  /* the local time by the rules of a TZ string (glibc's '__tz_compute'): null if 't' has no UTC date */
  private static OsDate byrules(long t, TzRule[] rules)
  {
    OsDate u = os_gmtime(t);
    if (u == null)
      return null;
    int year = 1900 + u.year;
    long c0 = compute_change(rules[0], year);
    long c1 = compute_change(rules[1], year);
    bool dst = (c0 > c1) ? (t < c1 || t >= c0) : (t >= c0 && t < c1);
    TzRule r = rules[dst ? 1 : 0];
    return breakdown(t, r.offset, dst ? 1 : 0, ByteView.of(r.name));
  }

  private static OsDate bytype(long t, TzZone z, int type)
  {
    return breakdown(t, z.utoff[type], z.isdst[type] ? 1 : 0, ByteView.of(z.abbr[type]));
  }

  public static OsDate os_localtime(long t)
  {
    TzZone z = tzzone;
    if (z == null)
    {
      DateTimeOffset at = DateTimeOffset.FromUnixTimeSeconds(fold(t));
      bool dst = netzone.IsDaylightSavingTime(at);
      string name = dst ? netzone.DaylightName : netzone.StandardName;
      return breakdown(t, (long) netzone.GetUtcOffset(at).TotalSeconds, dst ? 1 : 0, ByteView.of(name));
    }
    if (!z.tzfile)
      return byrules(t, z.rules);
    int n = z.trans.Length;
    if (z.utoff.Length == 0)
      return breakdown(t, 0, 0, ByteView.EMPTY);
    if (n == 0 || t < z.trans[0])
    { /* before the first transition: the first standard time */
      int i = 0;
      while (i < z.isdst.Length && z.isdst[i])
        i++;
      return bytype(t, z, (i == z.isdst.Length) ? 0 : i);
    }
    if (t >= z.trans[n - 1])
    {
      if (z.rules != null && os_gmtime(t) != null)
        return byrules(t, z.rules);
      return bytype(t, z, z.idx[n - 1]);
    }
    int lo = 0, hi = n - 1;
    while (lo < hi)
    {
      int mid = (lo + hi + 1) >>> 1;
      if (z.trans[mid] <= t)
        lo = mid;
      else
        hi = mid - 1;
    }
    return bytype(t, z, z.idx[lo]);
  }

  /*
  ** {------------------------------------------------------
  ** 'mktime': glibc's (gnulib's 'mktime_internal'), on 'os_localtime':
  ** its probes from the offset of its last call, its answers for times
  ** in a gap or an overlap and for an 'isdst' that is not the time's, and
  ** its seconds out of 0-59 added after the rest is found.
  ** -------------------------------------------------------
  */

  /* the offset of the last call (glibc's static 'localtime_offset': seconds west, an int) */
  private static int localtime_offset = 0;

  /* days of (year - 1900, yday) since 1970-01-01 */
  private static long ydays(long year, long yday)
  {
    return days_from_civil(year + 1900, 1, 1) + yday;
  }

  private static long ydhms_diff(long year1, long yday1, int hour1, int min1, int sec1,
                                 long year0, long yday0, int hour0, int min0, int sec0)
  {
    long days = ydays(year1, yday1) - ydays(year0, yday0);
    long hours = 24 * days + hour1 - hour0;
    long minutes = 60 * hours + min1 - min0;
    return 60 * minutes + sec1 - sec0;
  }

  private static long tm_diff(long year, long yday, int hour, int min, int sec, OsDate tp)
  {
    return ydhms_diff(year, yday, hour, min, sec, tp.year, tp.yday, tp.hour, tp.min, tp.sec);
  }

  private static bool isdst_differ(int a, int b)
  {
    return ((a == 0) != (b == 0)) && 0 <= a && 0 <= b;
  }

  /* 'os_localtime' of *t, or of the nearest time that has one (stored in t[0]); null if none */
  private static OsDate ranged_convert(long[] t)
  {
    OsDate r = os_localtime(t[0]);
    if (r != null)
      return r;
    long bad = t[0];
    long ok = 0;
    OsDate oktm = null;
    for (;;)
    {
      long mid = (ok >> 1) + (bad >> 1) + (ok & bad & 1);
      if (mid == ok || mid == bad)
        break;
      OsDate m = os_localtime(mid);
      if (m != null)
      {
        ok = mid;
        oktm = m;
      }
      else
        bad = mid;
    }
    if (oktm == null)
      return null;
    t[0] = ok;
    return oktm;
  }

  private static long mktime(OsDate tp)
  {
    int remaining_probes = 6;
    int sec = tp.sec;
    int min = tp.min;
    int hour = tp.hour;
    int mday = tp.day;
    int mon = tp.month;
    int year_requested = tp.year;
    int isdst = tp.isdst;
    bool dst2 = false;
    int mon_remainder = mon % 12;
    int negative_mon_remainder = (mon_remainder < 0) ? 1 : 0;
    int mon_years = mon / 12 - negative_mon_remainder;
    long year = (long) year_requested + mon_years;
    int mon_yday = mon_ydays[isleap4(year) ? 1 : 0][mon_remainder + 12 * negative_mon_remainder] - 1;
    long yday = mon_yday + (long) mday;
    int off = localtime_offset;
    int sec_requested = sec;
    if (sec < 0)
      sec = 0;
    if (59 < sec)
      sec = 59;
    int negative_offset_guess = 0 - off;
    long t0 = ydhms_diff(year, yday, hour, min, sec, 70, 0, 0, 0, negative_offset_guess);
    long t = t0, t1 = t0, t2 = t0;
    OsDate tm;
    bool found = false;
    for (;;)
    {
      long[] tt = {t};
      tm = ranged_convert(tt);
      if (tm == null)
        return -1;
      t = tt[0];
      long dt = tm_diff(year, yday, hour, min, sec, tm);
      if (dt == 0)
        break;
      if (t == t1 && t != t2 && (tm.isdst < 0 || ((isdst < 0) ? (!dst2 || tm.isdst != 0) : (isdst != 0) != (tm.isdst != 0))))
      {
        found = true; /* (oscillating in a gap: the time 'dt' away) */
        break;
      }
      remaining_probes--;
      if (remaining_probes == 0)
      {
        errno = EOVERFLOW;
        return -1;
      }
      t1 = t2;
      t2 = t;
      t += dt;
      dst2 = tm.isdst != 0;
    }
    if (!found && isdst_differ(isdst, tm.isdst))
    { /* look for a time near with the requested 'isdst', and use its offset */
       int stride = 601200;
       int duration_max = 457243200;
       int delta_bound = duration_max / 2 + stride;

      for (int delta = stride; delta < delta_bound; delta += stride)
        for (int direction = -1; direction <= 1; direction += 2)
        {
          long ot = t + (long) delta * direction;
          long[] ott = {ot};
          OsDate otm = ranged_convert(ott);
          if (otm == null)
            return -1;
          ot = ott[0];
          if (!isdst_differ(isdst, otm.isdst))
          {
            long gt = ot + tm_diff(year, yday, hour, min, sec, otm);
            OsDate g = os_localtime(gt);
            if (g != null)
            {
              tm = g;
              t = gt;
              found = true;
              goto search_break;
            }
          }
        } search_break:;
      if (!found)
      { /* none: the usual difference of one hour */
        int dst_difference = ((isdst == 0) ? 1 : 0) - ((tm.isdst == 0) ? 1 : 0);
        t += dst_difference * 60L * 60;
        tm = os_localtime(t);
        if (tm == null)
          return -1;
      }
    }
    localtime_offset = (int) (t - t0 - negative_offset_guess);
    if (sec_requested != tm.sec)
    {
      long sec_adjustment = (sec == 0 && tm.sec == 60) ? 1 : 0;
      sec_adjustment -= sec;
      sec_adjustment += sec_requested;
      try
      {
        t = JMath.addExact(t, sec_adjustment);
      }
      catch (System.ArithmeticException e)
      {
        errno = EOVERFLOW;
        return -1;
      }
      tm = os_localtime(t);
      if (tm == null)
        return -1;
    }
    tp.year = tm.year;
    tp.month = tm.month;
    tp.day = tm.day;
    tp.hour = tm.hour;
    tp.min = tm.min;
    tp.sec = tm.sec;
    tp.yday = tm.yday;
    tp.wday = tm.wday;
    tp.isdst = tm.isdst;
    tp.gmtoff = tm.gmtoff;
    tp.zone = tm.zone;
    return t;
  }

  public static OptionalLong os_mktime(OsDate d)
  {
    d.wday = -1; /* if call succeeds, it will set this value */
    long t = mktime(d);
    if (d.wday == -1) /* error if tm_wday was not set */
      return OptionalLong.empty();
    return OptionalLong.of(t);
  }

  /* is the year 'year' (since 1900) a leap year? */
  private static bool isleap4(long year)
  {
    long y = year + 1900;
    return (y & 3) == 0 && (y % 100 != 0 || y % 400 == 0);
  }

  /* }------------------------------------------------------ */

  public static double os_timediff(long t1, long t2)
  {
    long d = t1 - t2;
    if (((t1 ^ t2) & (t1 ^ d)) < 0) /* overflow? */
      return (double) ((Int128) t1 - t2);
    return (double) d;
  }

  private static readonly string[] wdays = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  private static readonly string[] months = {"January", "February", "March", "April", "May", "June", "July", "August",
                                          "September", "October", "November", "December"};

  /* the bytes of a 'strftime' result */
  private sealed class Out
  {
    internal sbyte[] b = new sbyte[64];
    internal int n = 0;

    internal void add(int c)
    {
      if (n == b.Length)
        b = JArrays.copyOf(b, n * 2);
      b[n++] = (sbyte) c;
    }

    internal void add(string s)
    {
      for (int i = 0; i < s.length(); i++)
        add(s.charAt(i));
    }

    internal void add(ByteView v)
    {
      for (int i = 0; i < v.size(); i++)
        add(v.at(i));
    }

    /* a number in at least 'width' digits, padded with 'pad' */
    internal void num(long v, int width, char pad)
    {
      string s = JLong.toString(JMath.abs(v));
      if (v < 0)
      {
        add('-');
        width--;
      }
      for (int i = s.length(); i < width; i++)
        add(pad);
      add(s);
    }
  }

  /* the days since the first day of the ISO week-based year (glibc's 'iso_week_days') */
  private static int iso_week_days(int yday, int wday)
  {
    int big_enough_multiple_of_7 = (366 / 7 + 2) * 7;
    return yday - (yday - wday + 4 + big_enough_multiple_of_7) % 7 + 4 - 1;
  }

  /* the zone's name when the date has none (glibc's 'tzname') */
  private static ByteView localabbr(int isdst)
  {
    TzZone z = tzzone;
    if (z != null && z.rules != null)
      return ByteView.of(z.rules[(isdst > 0) ? 1 : 0].name);
    OsDate now = os_localtime(os_now());
    return (now != null && now.zone != null) ? now.zone : ByteView.EMPTY;
  }

  /* C's 'strftime' in the "C" locale, as glibc formats */
  private static void strftime(Out o, ByteView f, OsDate d)
  {
    int year = d.year + 1900; /* (wraps as glibc's int does) */
    for (int i = 0; i < f.size(); i++)
    {
      int c = f.uat(i);
      if (c != '%')
      {
        o.add(c);
        continue;
      }
      int start = i;
      if (i + 1 < f.size() && (f.uat(i + 1) == 'E' || f.uat(i + 1) == 'O'))
        i++;
      if (i + 1 >= f.size())
      { /* a '%' at the end: copied */
        for (int k = start; k < f.size(); k++)
          o.add(f.at(k));
        break;
      }
      c = f.uat(++i);
      switch (c)
      {
      case 'a':
        o.add((d.wday < 0 || d.wday > 6) ? "?" : wdays[d.wday].substring(0, 3));
        break;
      case 'A':
        o.add((d.wday < 0 || d.wday > 6) ? "?" : wdays[d.wday]);
        break;
      case 'b':
      case 'h':
        o.add((d.month < 0 || d.month > 11) ? "?" : months[d.month].substring(0, 3));
        break;
      case 'B':
        o.add((d.month < 0 || d.month > 11) ? "?" : months[d.month]);
        break;
      case 'c':
        strftime(o, ByteView.of("%a %b %e %H:%M:%S %Y"), d);
        break;
      case 'C':
        o.num(JMath.floorDiv(year, 100), 1, '0');
        break;
      case 'd':
        o.num(d.day, 2, '0');
        break;
      case 'D':
      case 'x':
        strftime(o, ByteView.of("%m/%d/%y"), d);
        break;
      case 'e':
        o.num(d.day, 2, ' ');
        break;
      case 'F':
        strftime(o, ByteView.of("%Y-%m-%d"), d);
        break;
      case 'g':
      case 'G':
      case 'V':
      {
        int y4 = d.year + ((d.year < 0) ? 1900 % 400 : 1900 % 400 - 400);
        int year_adjust = 0;
        int days = iso_week_days(d.yday, d.wday);
        if (days < 0)
        { /* this ISO week belongs to the previous year */
          year_adjust = -1;
          days = iso_week_days(d.yday + (365 + (isleap(y4 - 1) ? 1 : 0)), d.wday);
        }
        else
        {
          int dd = iso_week_days(d.yday - (365 + (isleap(y4) ? 1 : 0)), d.wday);
          if (0 <= dd)
          { /* this ISO week belongs to the next year */
            year_adjust = 1;
            days = dd;
          }
        }
        int g = year + year_adjust;
        if (c == 'g')
          o.num((g % 100 + 100) % 100, 2, '0');
        else if (c == 'G')
          o.num(g, 1, '0');
        else
          o.num(days / 7 + 1, 2, '0');
        break;
      }
      case 'H':
        o.num(d.hour, 2, '0');
        break;
      case 'I':
        o.num((d.hour % 12 == 0) ? 12 : d.hour % 12, 2, '0');
        break;
      case 'j':
        o.num(d.yday + 1L, 3, '0');
        break;
      case 'm':
        o.num(d.month + 1L, 2, '0');
        break;
      case 'M':
        o.num(d.min, 2, '0');
        break;
      case 'n':
        o.add('\n');
        break;
      case 'p':
        o.add((d.hour > 11) ? "PM" : "AM");
        break;
      case 'r':
        strftime(o, ByteView.of("%I:%M:%S %p"), d);
        break;
      case 'R':
        strftime(o, ByteView.of("%H:%M"), d);
        break;
      case 'S':
        o.num(d.sec, 2, '0');
        break;
      case 't':
        o.add('\t');
        break;
      case 'T':
      case 'X':
        strftime(o, ByteView.of("%H:%M:%S"), d);
        break;
      case 'u':
        o.num((d.wday - 1 + 7) % 7 + 1, 1, '0');
        break;
      case 'U':
        o.num((d.yday - d.wday + 7) / 7, 2, '0');
        break;
      case 'w':
        o.num(d.wday, 1, '0');
        break;
      case 'W':
        o.num((d.yday - (d.wday - 1 + 7) % 7 + 7) / 7, 2, '0');
        break;
      case 'y':
        o.num((d.year % 100 + 100) % 100, 2, '0');
        break;
      case 'Y':
        o.num(year, 1, '0');
        break;
      case 'z':
      {
        long diff = d.gmtoff;
        o.add((diff < 0) ? '-' : '+');
        diff = JMath.abs(diff) / 60;
        o.num((diff / 60) * 100 + diff % 60, 4, '0');
        break;
      }
      case 'Z':
        o.add((d.zone != null) ? d.zone : localabbr(d.isdst));
        break;
      case '%':
        o.add('%');
        break;
      default: /* not a conversion: copied */
        for (int k = start; k <= i; k++)
          o.add(f.at(k));
        break;
      }
    }
  }

  public static int os_strftime(ByteSpan @out, int off, ByteView fmt, OsDate d)
  {
    int z = fmt.find(0);
    Out o = new Out();
    strftime(o, (z == ByteView.npos) ? fmt : fmt.substr(0, z), d);
    if (o.n >= @out.size() - off)
      return 0;
    @out.copy_from(new ByteView(o.b, 0, o.n), off);
    @out.set(off + o.n, 0);
    return o.n;
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Files by name
  ** =======================================================
  */

  public static OsResult os_remove(ByteView path)
  {
    errno = 0;
    try
    {
      string p = OsApi.path(path);
      if (isdir(p))
        Directory.Delete(p, false);
      else
      {
        FileInfo i = new FileInfo(p);
        if (!i.Exists && i.LinkTarget == null) /* (.NET's File.Delete of nothing succeeds) */
          throw ioerror(enoent(p));
        File.Delete(p);
      }
      return new OsResult(true, errno);
    }
    catch (Exception e) when (isioerror(e))
    {
      errno = errnoOf(e);
      return new OsResult(false, errno);
    }
  }

  public static OsResult os_rename(ByteView from, ByteView to)
  {
    errno = 0;
    try
    {
      string f = path(from);
      string t = path(to);
      if (isdir(f))
        Directory.Move(f, t);
      else
      {
        FileInfo i = new FileInfo(f);
        if (!i.Exists && i.LinkTarget == null)
          throw ioerror(enoent(f));
        if (isdir(t))
          throw ioerror(EISDIR);
        File.Move(f, t, true);
      }
      return new OsResult(true, errno);
    }
    catch (Exception e) when (isioerror(e))
    {
      errno = errnoOf(e);
      return new OsResult(false, errno);
    }
  }

  private const string TMPLETTERS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

  /* 'mkstemp' of "<dir>/<prefix>XXXXXX": the file made (mode 0600), or null */
  private static string mkstemp(string prefix)
  {
    Random rnd = Random.Shared;
    for (int attempt = 0; attempt < 62 * 62 * 62; attempt++)
    {
      StringBuilder sb = new StringBuilder(prefix);
      for (int i = 0; i < 6; i++)
        sb.append(TMPLETTERS.charAt(rnd.Next(TMPLETTERS.length())));
      string p = sb.toString();
      try
      {
        FileStreamOptions o = new FileStreamOptions();
        o.Mode = FileMode.CreateNew;
        o.Access = FileAccess.Write;
        o.Share = FileShare.ReadWrite | FileShare.Delete;
        o.UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite;
        new FileStream(p, o).Dispose();
        return p;
      }
      catch (Exception e) when (isioerror(e))
      {
        if (e.HResult == EEXIST)
          continue;
        errno = errnoOf(e);
        return null;
      }
    }
    errno = EEXIST;
    return null;
  }

  public static InternalString os_tmpname()
  {
    string p = mkstemp("/tmp/lua_");
    if (p == null)
      return null;
    return new InternalString(new ByteView(p.getBytes(JCharsets.UTF_8)));
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Commands
  ** =======================================================
  */

  private const string SHELL = "/bin/sh";

  public static bool os_shell_available()
  {
    try
    {
      return File.Exists(SHELL) && (File.GetUnixFileMode(SHELL)
          & (UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute)) != 0;
    }
    catch (Exception e) when (isioerror(e))
    {
      return false;
    }
  }

  /*
  ** A command runs as C's 'system' and 'popen' run it: "/bin/sh -c cmd"
  ** (with $0 "sh"). .NET reports a death by signal n as the exit code
  ** 128 + n, as it reports an 'exit 128 + n': to tell them apart, the
  ** shell's EXIT trap (which runs when it exits, not when a signal kills
  ** it) writes its status to a file of its own.
  */
  private sealed class Command
  {
    internal Process proc;
    internal string marker;
  }

  /* (.NET keeps a command's pipes open after it exits, as C's 'popen' does: no named pipe is needed) */
  private static Command launch(ByteView cmd, bool pipein, bool pipeout)

  {
    Command c = new Command();
    string text = cstr(cmd);
    string script = text;
    int saved = errno;
    c.marker = mkstemp(SysProps.getProperty("java.io.tmpdir", "/tmp") + "/lua_st_");
    errno = saved;
    if (c.marker != null)
    {
      string m = c.marker;
      if (m.matches("[A-Za-z0-9_./-]+"))
        script = "trap 'echo $? >" + m + "' EXIT; " + text;
      else
      {
        File.Delete(c.marker);
        c.marker = null;
      }
    }
    ProcessStartInfo pb = new ProcessStartInfo(SHELL);
    pb.ArgumentList.Add("-c");
    pb.ArgumentList.Add(script);
    pb.ArgumentList.Add("sh");
    pb.UseShellExecute = false;
    Environ.restore(pb);
    pb.RedirectStandardInput = pipein;
    pb.RedirectStandardOutput = pipeout;
    pb.RedirectStandardError = false;
    try
    {
      c.proc = Process.Start(pb);
    }
    catch (Exception e) when (isioerror(e))
    {
      if (c.marker != null)
        File.Delete(c.marker);
      throw;
    }
    return c;
  }

  /* the wait status of the command, as 'waitpid' gives it */
  private static int waitstatus(Process proc, string marker)
  {
    proc.WaitForExit();
    int v = proc.ExitCode;
    bool exited = v <= 128;
    if (marker != null)
    {
      try
      {
        string s = File.ReadAllText(marker).trim();
        if (!s.isEmpty() && JInteger.parseInt(s) == v)
          exited = true;
      }
      catch (Exception e) when (isioerror(e) || e is FormatException || e is OverflowException)
      {
        /* (no status written: the shell did not exit) */
      }
      try
      {
        File.Delete(marker);
      }
      catch (Exception e) when (isioerror(e))
      {
        /* (left behind) */
      }
    }
    if (!exited && v - 128 < 65)
      return v - 128;
    return (v & 0xFF) << 8;
  }

  /* a command that could not start: as C reports it (a shell that cannot run exits 127) */
  private static bool execfailure(int en)
  {
    return en == ENOENT || en == EACCES || en == ENOEXEC;
  }

  public static OsSystemResult os_system(ByteView cmd)
  {
    errno = 0;
    try
    {
      Command c = launch(cmd, false, false);
      int stat = waitstatus(c.proc, c.marker);
      return new OsSystemResult(stat, 0);
    }
    catch (Exception e) when (isioerror(e))
    {
      int en = errnoOf(e);
      if (execfailure(en))
        return new OsSystemResult(127 << 8, 0);
      errno = en;
      return new OsSystemResult(-1, errno);
    }
  }

  public static OsExecStatus os_decode_exec(int stat)
  {
    if ((stat & 0x7F) == 0) /* WIFEXITED */
      return new OsExecStatus(true, (stat >> 8) & 0xFF);
    if (((sbyte) ((stat & 0x7F) + 1) >> 1) > 0) /* WIFSIGNALED */
      return new OsExecStatus(false, stat & 0x7F);
    return new OsExecStatus(true, stat);
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Dynamic libraries. .NET loads no C code here: a library is a C# class
  ** that stands in for a C one, and its symbols are the class's public
  ** static fields (a 'lua_CFunction' for a C function). 'os_dlopen'
  ** answers as glibc's 'dlopen' (RTLD_NOW) does: a path with a '/' loads
  ** the C# library that its file's base name (no directory, no
  ** extension) names, if the file exists (what it holds does not matter:
  ** a C build's library serves); a name without a '/', the system library
  ** of that name; "", the program. The undefined symbols of a library must
  ** be defined by the program or by a library loaded with global symbols.
  ** Errors have glibc's messages.
  ** =======================================================
  */

  /* the C# libraries: base name, class, undefined symbols */
  private static readonly string[][] dllibs = {
    new[] {"lib1", "Payonel.LungoLua.Tests.Libs.Lib1"},
    new[] {"lib11", "Payonel.LungoLua.Tests.Libs.Lib11", "lib1_export"},
    new[] {"lib2", "Payonel.LungoLua.Tests.Libs.Lib2"},
    new[] {"lib21", "Payonel.LungoLua.Tests.Libs.Lib21", "luaopen_lib2"},
    new[] {"lib2-v2", "Payonel.LungoLua.Tests.Libs.Lib22"},
    new[] {"libreadline", "Payonel.LungoLua.Platform.Readline"},
  };

  /* the system libraries: name, base name of its C# library */
  private static readonly string[][] dlsyslibs = {
    new[] {"libreadline.so", "libreadline"},
    new[] {"libreadline.so.8", "libreadline"},
  };

  /* the program's own symbols: name, class */
  private static readonly string[][] dlprogsyms = {
    new[] {"luaopen_base", "Payonel.LungoLua.Lib.Lbaselib"},
    new[] {"luaopen_coroutine", "Payonel.LungoLua.Lib.Lcorolib"},
    new[] {"luaopen_debug", "Payonel.LungoLua.Lib.Ldblib"},
    new[] {"luaopen_io", "Payonel.LungoLua.Lib.Liolib"},
    new[] {"luaopen_math", "Payonel.LungoLua.Lib.Lmathlib"},
    new[] {"luaopen_os", "Payonel.LungoLua.Lib.Loslib"},
    new[] {"luaopen_package", "Payonel.LungoLua.Lib.Loadlib"},
    new[] {"luaopen_string", "Payonel.LungoLua.Lib.Lstrlib"},
    new[] {"luaopen_table", "Payonel.LungoLua.Lib.Ltablib"},
    new[] {"luaopen_utf8", "Payonel.LungoLua.Lib.Lutf8lib"},
  };

  /* the program's name, as the error messages give it (its 'argv[0]') */
  private static sbyte[] dlprogname()
  {
    string n = SysProps.getProperty("lua.progname");
    if (n == null)
      n = Environment.ProcessPath ?? "";
    return n.getBytes(JCharsets.UTF_8);
  }

  /* the program */
  private static readonly OsLib dlprogram = new OsLib(dlprogname(), null, null);

  /* the loaded libraries, by what they were loaded from */
  private static readonly HashMap<object, OsLib> dlloaded = new ();

  /* the libraries loaded with global symbols, in the order they got them */
  private static readonly ArrayList<OsLib> dlglobals = new ();

  /* the last error ('os_dlerror' gives it once) */
  private static sbyte[] dlerr = null;

  /* the bytes of a C string (up to its first '\0') */
  private static sbyte[] cbytes(ByteView v)
  {
    int z = v.find(0);
    return ((z == ByteView.npos) ? v : v.substr(0, z)).toArray();
  }

  private static T dlfail<T>(sbyte[] name, string what, sbyte[] sym) where T : class
  {
    sbyte[] w = (": " + what).getBytes(JCharsets.UTF_8);
    sbyte[] e = JArrays.copyOf(name, name.Length + w.Length + sym.Length);
    JSystem.arraycopy(w, 0, e, name.Length, w.Length);
    JSystem.arraycopy(sym, 0, e, name.Length + w.Length, sym.Length);
    dlerr = e;
    return null;
  }

  private static string dlstrerror(int err)
  {
    return os_strerror(err).toString();
  }

  private static string[] dlentry(string[][] table, string name)
  {
    foreach (string[] e in table)
      if (e[0].equals(name))
        return e;
    return null;
  }

  /* the public static field 'sym' of class 'cls' (null if none) */
  private static object dlfield(Type cls, string sym)
  {
    try
    {
      FieldInfo f = cls.GetField(sym, BindingFlags.Public | BindingFlags.Static);
      if (f == null) /* (a C# class has no field and method of one name: a function's field is 'name_fn') */
        f = cls.GetField(sym + "_fn", BindingFlags.Public | BindingFlags.Static);
      return (f != null) ? f.GetValue(null) : null;
    }
    catch (Exception e)
    {
      return null;
    }
  }

  /* the class of that name: the engine's, or the tests' (the assembly "lungolua-tests", beside the engine's); null if none */
  private static Type dlclass(string name)
  {
    Type t = typeof(OsApi).Assembly.GetType(name);
    if (t != null)
      return t;
    Assembly a;
    try
    {
      a = Assembly.Load(new AssemblyName("lungolua-tests"));
    }
    catch (Exception e)
    {
      string dir = System.IO.Path.GetDirectoryName(typeof(OsApi).Assembly.Location);
      if (string.IsNullOrEmpty(dir))
        dir = AppContext.BaseDirectory;
      try
      {
        a = AssemblyLoadContext.Default.LoadFromAssemblyPath(System.IO.Path.Combine(dir, "lungolua-tests.dll"));
      }
      catch (Exception e2)
      {
        return null;
      }
    }
    return a.GetType(name);
  }

  /* a symbol of the global scope: the program's, then the global libraries' */
  private static object dlglobal(string sym)
  {
    string[] ps = dlentry(dlprogsyms, sym);
    if (ps != null)
    {
      Type c = dlclass(ps[1]);
      return (c != null) ? dlfield(c, sym) : null;
    }
    foreach (OsLib lib in dlglobals)
    {
      object o = dlfield(lib.cls, sym);
      if (o != null)
        return o;
    }
    return null;
  }

  /* why a file that names no C# library does not load */
  private static string dlnotjava(string f)
  {
    sbyte[] h = new sbyte[64]; /* an ELF header */
    int n;
    try { using (FileStream @in = new FileStream(f, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 0))
    {
      n = @in.ReadAtLeast((byte[]) (object) h, h.Length, false);
    } }
    catch (Exception e) when (isioerror(e))
    {
      return "cannot read file data: " + dlstrerror(errnoOf(e));
    }
    if (n < h.Length)
      return "file too short";
    if (h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F')
      return "invalid ELF header";
    return "cannot load C code into .NET";
  }

  /* can the file be opened for reading? */
  private static bool readable(string f)
  {
    try
    {
      File.OpenHandle(f, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete).Dispose();
      return true;
    }
    catch (Exception e) when (isioerror(e))
    {
      return false;
    }
  }

  private static OsLib dlload(sbyte[] name)
  {
    string p =  JString.of(name, JCharsets.UTF_8);
    string[] e;
    object key;
    sbyte[] none = new sbyte[0];
    if (p.isEmpty())
      return dlprogram;
    else if (p.indexOf('/') < 0)
    { /* a name: the system's libraries */
      string[] s = dlentry(dlsyslibs, p);
      if (s == null)
        return dlfail<OsLib>(name, "cannot open shared object file: " + dlstrerror(ENOENT), none);
      e = dlentry(dllibs, s[1]);
      key = e;
    }
    else
    { /* a file */
      string f;
      try
      {
        f = realpath(p);
      }
      catch (Exception x) when (isioerror(x))
      {
        return dlfail<OsLib>(name, "cannot open shared object file: " + dlstrerror(errnoOf(x)), none);
      }
      if (Directory.Exists(f))
        return dlfail<OsLib>(name, "cannot read file data: " + dlstrerror(EISDIR), none);
      if (!readable(f))
        return dlfail<OsLib>(name, "cannot open shared object file: " + dlstrerror(EACCES), none);
      string @base = p.substring(p.lastIndexOf('/') + 1);
      if (@base.indexOf('.') >= 0)
        @base = @base.substring(0, @base.indexOf('.'));
      e = dlentry(dllibs, @base);
      if (e == null)
        return dlfail<OsLib>(name, dlnotjava(f), none);
      key = f;
    }
    OsLib lib = dlloaded.get(key);
    if (lib != null)
      return lib; /* already loaded */
    Type cls = dlclass(e[1]);
    if (cls == null)
      return dlfail<OsLib>(name, "cannot load C code into .NET", none);
    for (int i = 2; i < e.Length; i++) /* bind its undefined symbols */
      if (dlglobal(e[i]) == null)
        return dlfail<OsLib>(name, "undefined symbol: ", e[i].getBytes(JCharsets.UTF_8));
    lib = new OsLib(name, key, cls);
    dlloaded.put(key, lib);
    return lib;
  }

  public static OsLib os_dlopen(ByteView path, bool global)
  {
    lock (dlloaded)
    {
      OsLib lib = dlload(cbytes(path));
      if (lib != null && lib != dlprogram)
      {
        lib.refs++;
        if (global && !lib.global)
        {
          lib.global = true;
          dlglobals.add(lib);
        }
      }
      return lib;
    }
  }

  public static object os_dlsym(OsLib lib, ByteView sym)
  {
    sbyte[] s = cbytes(sym);
    string n =  JString.of(s, JCharsets.UTF_8);
    lock (dlloaded)
    {
      object o = (lib == dlprogram) ? dlglobal(n) : dlfield(lib.cls, n);
      if (o == null)
        return dlfail<object>(lib.name, "undefined symbol: ", s);
      return o;
    }
  }

  /* (a symbol that is not a 'lua_CFunction' is not one .NET can call as one) */
  public static lua_CFunction os_dlfunc(OsLib lib, ByteView sym)
  {
    object f = os_dlsym(lib, sym);
    if (f == null || f is lua_CFunction)
      return (lua_CFunction) f;
    lock (dlloaded)
    {
      return dlfail<lua_CFunction>(lib.name, "not a lua_CFunction: ", cbytes(sym));
    }
  }

  public static void os_dlclose(OsLib lib)
  {
    lock (dlloaded)
    {
      if (lib != dlprogram && --lib.refs == 0)
      { /* unload it */
        dlloaded.remove(lib.key);
        dlglobals.removeItem(lib);
      }
    }
  }

  public static InternalString os_dlerror()
  {
    sbyte[] e;
    lock (dlloaded)
    {
      e = dlerr;
      dlerr = null;
    }
    ByteView sve = (e != null) ? new ByteView(e) : Lt.unknown_dynamic_link_error;
    return new InternalString(sve);
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Streams
  ** =======================================================
  */

  /* the mode of 'fopen' (glibc's reading): null if invalid */
  private sealed class Mode
  {
    internal bool read;
    internal bool write;
    internal bool create;
    internal bool truncate;
    internal bool append;
    internal bool excl;
  }

  private static Mode parsemode(ByteView mode)
  {
    string m = cstr(mode);
    if (m.isEmpty())
      return null;
    Mode r = new Mode();
    switch (m.charAt(0))
    {
    case 'r':
      r.read = true;
      break;
    case 'w':
      r.write = r.create = r.truncate = true;
      break;
    case 'a':
      r.write = r.create = r.append = true;
      break;
    default:
      return null;
    }
    for (int i = 1; i < 7 && i < m.length(); i++)
    {
      switch (m.charAt(i))
      {
      case '+':
        r.read = r.write = true;
        break;
      case 'x':
        r.excl = true;
        break;
      default:
        break;
      }
    }
    return r;
  }

  /* opens 'path' as 'fopen' does: the stream's descriptor */
  private static OsFile.Device opendevice(string p, Mode m)
  {
    if (Directory.Exists(p))
    { /* (.NET opens no directory: as Linux opens one) */
      if (m.create && m.excl)
        throw ioerror(EEXIST);
      if (m.write)
        throw ioerror(EISDIR);
      return new OsFile.DirDevice();
    }
    FileStreamOptions o = new FileStreamOptions();
    o.Access = (m.read && m.write) ? FileAccess.ReadWrite : m.write ? FileAccess.Write : FileAccess.Read;
    o.Mode = !m.create ? FileMode.Open : m.excl ? FileMode.CreateNew : m.truncate ? FileMode.Create : FileMode.OpenOrCreate;
    o.Share = FileShare.ReadWrite | FileShare.Delete; /* (.NET locks the least so: C locks nothing) */
    o.BufferSize = 0;
    bool emulate = m.append; /* (.NET opens no O_APPEND: every write seeks its end) */
    FileStream ch = new FileStream(p, o);
    return new OsFile.ChannelDevice(ch, emulate);
  }

  private static int filebufmode(string p)
  {
    string s = p;
    if (s.startsWith("/dev/"))
    {
      try
      {
        if (isttypath(realpath(p)))
          return OsFile.IOLBF;
      }
      catch (Exception e) when (isioerror(e))
      {
        /* (not a terminal) */
      }
    }
    return OsFile.IOFBF;
  }

  public static OsOpenResult os_fopen(ByteView path, ByteView mode)
  {
    errno = 0;
    Mode m = parsemode(mode);
    if (m == null)
    {
      errno = EINVAL;
      return new OsOpenResult(null, errno);
    }
    try
    {
      string p = OsApi.path(path);
      OsFile f = new OsFile(opendevice(p, m), m.read, m.write, filebufmode(p));
      remember(f);
      return new OsOpenResult(f, 0);
    }
    catch (Exception e) when (isioerror(e))
    {
      errno = errnoOf(e);
      return new OsOpenResult(null, errno);
    }
  }

  public static OsOpenResult os_popen(ByteView cmd, ByteView mode)
  {
    errno = 0;
    string m = cstr(mode);
    if (m.isEmpty() || (m.charAt(0) != 'r' && m.charAt(0) != 'w'))
    {
      errno = EINVAL;
      return new OsOpenResult(null, errno);
    }
    bool reading = m.charAt(0) == 'r';
    try
    {
      Command c = reading ? launch(cmd, false, true) : launch(cmd, true, false);
      OsFile f = new OsFile(new OsFile.PipeDevice(c.proc, reading ? c.proc.StandardOutput.BaseStream : null,
                                                  reading ? null : c.proc.StandardInput.BaseStream, c.marker),
                            reading, !reading, OsFile.IOFBF);
      remember(f);
      return new OsOpenResult(f, 0);
    }
    catch (Exception e) when (isioerror(e))
    {
      errno = errnoOf(e);
      return new OsOpenResult(null, errno);
    }
  }

  public static OsOpenResult os_tmpfile()
  {
    errno = 0;
    string p = mkstemp(SysProps.getProperty("java.io.tmpdir", "/tmp") + "/tmpf");
    if (p == null)
      return new OsOpenResult(null, errno);
    try
    {
      FileStream ch = new FileStream(p, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite | FileShare.Delete, 0);
      File.Delete(p); /* (unnamed, as C's) */
      OsFile f = new OsFile(new OsFile.ChannelDevice(ch, false), true, true, OsFile.IOFBF);
      remember(f);
      return new OsOpenResult(f, 0);
    }
    catch (Exception e) when (isioerror(e))
    {
      errno = errnoOf(e);
      try
      {
        File.Delete(p);
      }
      catch (Exception e2) when (isioerror(e2))
      {
        /* (left behind) */
      }
      return new OsOpenResult(null, errno);
    }
  }

  public static int os_freopen(OsFile f, ByteView path, ByteView mode)
  {
    errno = 0;
    Monitor.Enter(f.@lock);
    try
    {
      f.flush();
      try
      {
        f.dev.close();
      }
      catch (Exception e) when (isioerror(e))
      {
        /* (ignored, as glibc ignores it) */
      }
      Mode m = parsemode(mode);
      if (m == null)
      {
        errno = EINVAL;
        f.open = false;
        forget(f);
        return errno;
      }
      try
      {
        string p = OsApi.path(path);
        f.reset(opendevice(p, m), m.read, m.write, filebufmode(p));
        return 0;
      }
      catch (Exception e) when (isioerror(e))
      {
        errno = errnoOf(e);
        f.open = false;
        forget(f);
        return errno;
      }
    }
    finally
    {
      Monitor.Exit(f.@lock);
    }
  }

  public static OsFile os_stdin()
  {
    return stdinf;
  }

  public static OsFile os_stdout()
  {
    return stdoutf;
  }

  public static OsFile os_stderr()
  {
    return stderrf;
  }

  public static int os_fread(OsFile f, ByteSpan @out, int off, int n)
  {
    { if (JAssert.on && !( off + n <= @out.size())) throw new AssertionError(); }
    return f.read(@out.array(), @out.offset() + off, n);
  }

  public static int os_fwrite(OsFile f, ByteView bytes)
  {
    return f.write(bytes.array(), bytes.offset(), bytes.size());
  }

  public static int os_getbyte(OsFile f)
  {
    return f.getbyte();
  }

  public static void os_ungetbyte(OsFile f, int c)
  {
    f.ungetbyte(c);
  }

  public static bool os_readline(OsFile f, ByteSpan buff)
  {
    return f.readline(buff.array(), buff.offset(), buff.size());
  }

  public static bool os_feof(OsFile f)
  {
    return f.eof;
  }

  public static bool os_ferror(OsFile f)
  {
    return f.err;
  }

  public static void os_clearerr(OsFile f)
  {
    Monitor.Enter(f.@lock);
    try
    {
      f.eof = false;
      f.err = false;
    }
    finally
    {
      Monitor.Exit(f.@lock);
    }
  }

  public static int os_fseek(OsFile f, long off, int whence)
  {
    errno = 0;
    return f.seek(off, whence);
  }

  public static long os_ftell(OsFile f)
  {
    return f.tell();
  }

  public static int os_setvbuf(OsFile f, int mode, long size)
  {
    errno = 0;
    return f.setvbuf(mode);
  }

  public static int os_fflush(OsFile f)
  {
    errno = 0;
    return f.flush();
  }

  public static void os_flushall()
  {
    OsFile[] all;
    lock (openfiles)
    {
      all = openfiles.ToArray();
    }
    foreach (OsFile f in all)
      f.flushout();
  }

  public static int os_fclose(OsFile f)
  {
    errno = 0;
    return f.close();
  }

  public static OsSystemResult os_pclose(OsFile f)
  {
    errno = 0;
    if (!(f.dev is OsFile.PipeDevice pd))
    {
      f.close();
      errno = ECHILD;
      return new OsSystemResult(-1, errno);
    }
    f.close();
    int stat = waitstatus(pd.proc, pd.marker);
    errno = 0;
    return new OsSystemResult(stat, 0);
  }

  /*
  ** C's 'fcntl(fileno(f), F_SETFL, O_NONBLOCK)' (for the tests): reads of a
  ** pipe or a standard stream then take only what is there, failing with
  ** EAGAIN when nothing is (where .NET tells what is there: a file read as
  ** the standard input); an unbuffered write to a pipe takes at most
  ** NONBLOCKROOM bytes, then fails with EAGAIN (.NET's streams cannot tell
  ** how full the pipe is; that much never fills Linux's 64K); files are
  ** not affected, as on Linux.
  */
  public static int os_setnonblock(OsFile f)
  {
    f.nonblock = true;
    return 0;
  }

  public static void os_flockfile(OsFile f)
  {
    Monitor.Enter(f.@lock);
  }

  public static void os_funlockfile(OsFile f)
  {
    Monitor.Exit(f.@lock);
  }

  /* }====================================================== */
}
