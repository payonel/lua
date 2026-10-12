package lua.platform;

import java.io.FileDescriptor;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.lang.management.ManagementFactory;
import java.math.BigInteger;
import java.nio.channels.ClosedChannelException;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.AccessDeniedException;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.DirectoryNotEmptyException;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.FileSystemException;
import java.nio.file.FileSystemLoopException;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.NotDirectoryException;
import java.nio.file.OpenOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.PosixFilePermissions;
import java.time.Instant;
import java.time.ZoneId;
import java.time.zone.ZoneRules;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.OptionalLong;
import java.util.TimeZone;
import java.util.concurrent.ThreadLocalRandom;

import lua.api.HostString;
import lua.api.lua_CFunction;
import lua.bytes.ByteSpan;
import lua.bytes.ByteView;
import lua.bytes.InternalString;

/*
** The operating system as the engine sees it (os_api.h), on the JVM: what
** the C++ takes from the C library and POSIX, with the same results and
** the same error conventions (errno codes, and the messages glibc's
** 'strerror' gives on Linux). Paths, commands and environment names cross
** to Java as UTF-8 (HostString's conversion), cut at their first '\0' as
** a C string is.
*/
public final class OsApi
{
  private OsApi()
  {
  }

  /* C's 'errno' (the last error of the functions here, as the C library sets it) */
  public static int errno = 0;

  /* stdio's BUFSIZ (glibc's) */
  public static final int BUFSIZ = 8192;

  public static final int OS_EOF = -1;

  /* <cstdlib>'s exit statuses */
  public static final int EXIT_SUCCESS = 0;
  public static final int EXIT_FAILURE = 1;

  /* errno values (Linux) */
  public static final int EPERM = 1;
  public static final int ENOENT = 2;
  public static final int EIO = 5;
  public static final int ENOEXEC = 8;
  public static final int EBADF = 9;
  public static final int ECHILD = 10;
  public static final int EAGAIN = 11;
  public static final int ENOMEM = 12;
  public static final int EACCES = 13;
  public static final int EEXIST = 17;
  public static final int EXDEV = 18;
  public static final int ENOTDIR = 20;
  public static final int EISDIR = 21;
  public static final int EINVAL = 22;
  public static final int ESPIPE = 29;
  public static final int EPIPE = 32;
  public static final int ENOTEMPTY = 39;
  public static final int ELOOP = 40;
  public static final int EOVERFLOW = 75;

  /* glibc's messages, by errno */
  private static final String[] errmsgs = {
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

  private static final HashMap<String, Integer> errcodes = new HashMap<>();

  static
  {
    for (int i = errmsgs.length - 1; i > 0; i--)
      errcodes.put(errmsgs[i], i);
    errcodes.put("Stream closed", EBADF);
    errcodes.put("Bad file number", EBADF);
  }

  /* the errno of a Java I/O error */
  static int errnoOf(Throwable e)
  {
    if (e instanceof OsFile.WriteError w)
      e = w.cause;
    if (e instanceof NoSuchFileException)
      return ENOENT;
    if (e instanceof AccessDeniedException)
      return EACCES;
    if (e instanceof FileAlreadyExistsException)
      return EEXIST;
    if (e instanceof DirectoryNotEmptyException)
      return ENOTEMPTY;
    if (e instanceof NotDirectoryException)
      return ENOTDIR;
    if (e instanceof AtomicMoveNotSupportedException)
      return EXDEV;
    if (e instanceof FileSystemLoopException)
      return ELOOP;
    if (e instanceof ClosedChannelException)
      return EBADF;
    if (e instanceof FileSystemException fse && fse.getReason() != null)
    {
      int c = errnoOfMessage(fse.getReason());
      if (c != 0)
        return c;
    }
    String msg = e.getMessage();
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
      int eq = msg.indexOf("error=");
      if (eq >= 0)
      {
        int s = eq + 6, t = s;
        while (t < msg.length() && Character.isDigit(msg.charAt(t)))
          t++;
        if (t > s)
          return Integer.parseInt(msg.substring(s, t));
      }
    }
    if (e instanceof FileNotFoundException)
      return ENOENT;
    return EIO;
  }

  private static int errnoOfMessage(String m)
  {
    Integer c = errcodes.get(m);
    if (c != null)
      return c;
    for (int i = 1; i < errmsgs.length; i++)
      if (m.startsWith(errmsgs[i]) && !errmsgs[i].startsWith("Unknown error"))
        return i;
    return 0;
  }

  /* a C string's text: the bytes up to the first '\0', as UTF-8 */
  private static String cstr(ByteView v)
  {
    int z = v.find(0);
    return HostString.from_bytes((z == ByteView.npos) ? v : v.substr(0, z)).to_os();
  }

  /* the path a C string names ("" names none) */
  private static Path path(ByteView v) throws IOException
  {
    String s = cstr(v);
    if (s.isEmpty())
      throw new NoSuchFileException(s);
    try
    {
      return Path.of(s);
    }
    catch (java.nio.file.InvalidPathException e)
    {
      throw new NoSuchFileException(s);
    }
  }

  /*
  ** {======================================================
  ** Standard streams, and what C's exit flushes
  ** =======================================================
  */

  private static final LinkedHashSet<OsFile> openfiles = new LinkedHashSet<>();

  static void remember(OsFile f)
  {
    synchronized (openfiles)
    {
      openfiles.add(f);
    }
  }

  static void forget(OsFile f)
  {
    synchronized (openfiles)
    {
      openfiles.remove(f);
    }
  }

  /* C's 'isatty' (glibc decides a stream's buffering by it) */
  public static boolean isatty(int fd)
  {
    try
    {
      return isttypath(Files.readSymbolicLink(Path.of("/proc/self/fd/" + fd)).toString());
    }
    catch (Exception e)
    {
      return fd != 2 && System.console() != null;
    }
  }

  private static boolean isttypath(String p)
  {
    return p.startsWith("/dev/pts/") || p.startsWith("/dev/tty") || p.equals("/dev/console");
  }

  private static final OsFile stdinf = OsFile.standard(FileDescriptor.in, true, isatty(0) ? OsFile.IOLBF : OsFile.IOFBF);
  private static final OsFile stdoutf = OsFile.standard(FileDescriptor.out, false, isatty(1) ? OsFile.IOLBF : OsFile.IOFBF);
  private static final OsFile stderrf = OsFile.standard(FileDescriptor.err, false, OsFile.IONBF);

  static
  {
    remember(stdinf);
    remember(stdoutf);
    remember(stderrf);
    /* C's 'exit' (and a return from 'main') flushes the open streams */
    Runtime.getRuntime().addShutdownHook(new Thread(OsApi::os_flushall));
  }

  /* C's 'exit': the process ends, the open streams flushed */
  public static void exit(int status)
  {
    os_flushall();
    System.exit(status);
  }

  /* glibc flushes a line-buffered stdout before reading from a line-buffered or unbuffered stream */
  static void flushlinestdout(OsFile reading)
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
    String n = cstr(name);
    String v = n.isEmpty() ? null : System.getenv(n);
    if (v == null)
      return null;
    return new ByteView(v.getBytes(StandardCharsets.UTF_8));
  }

  public static ByteView os_strerror(int err)
  {
    String m = (err >= 0 && err < errmsgs.length) ? errmsgs[err] : "Unknown error " + err;
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
      java.lang.management.OperatingSystemMXBean os = ManagementFactory.getOperatingSystemMXBean();
      if (os instanceof com.sun.management.OperatingSystemMXBean sun)
        ns = sun.getProcessCpuTime();
    }
    catch (Throwable e)
    {
      ns = -1;
    }
    if (ns < 0)
      ns = ProcessHandle.current().info().totalCpuDuration().map(java.time.Duration::toNanos).orElse(-1000L);
    if (ns < 0)
      return -1.0 / 1000000.0;
    return ((double) (ns / 1000)) / 1000000.0; /* (clock_t ticks: microseconds) */
  }

  public static long os_now()
  {
    return Math.floorDiv(System.currentTimeMillis(), 1000);
  }

  /* days since 1970-01-01 of a date of the proleptic Gregorian calendar */
  private static long days_from_civil(long y, int m, int d)
  {
    y -= (m <= 2) ? 1 : 0;
    long era = Math.floorDiv(y, 400);
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
  ** with no /etc/localtime, the JVM's zone, through java.time.)
  */

  /* a rule of a POSIX TZ string: when its time starts in a year (glibc's 'tz_rule') */
  private static final class TzRule
  {
    String name = "";
    int offset = 0;  /* seconds east of UTC */
    int type = J0;
    int m = 0;
    int n = 0;
    int d = 0;
    int secs = 0;    /* the time of day of the change */
  }

  private static final int J0 = 0; /* n: day of the year, from 0 */
  private static final int J1 = 1; /* Jn: day of the year, from 1, without February 29 */
  private static final int M = 2;  /* Mm.n.d: day 'd' of week 'n' of month 'm' */

  private static final class TzZone
  {
    boolean tzfile = false;      /* a TZif file (else: its rules only) */
    long[] trans = new long[0];  /* transition times */
    int[] idx = new int[0];      /* the type each one starts */
    int[] utoff = new int[0];    /* the types: offsets, */
    boolean[] isdst = new boolean[0]; /* daylight saving or not, */
    String[] abbr = new String[0];    /* abbreviations */
    TzRule[] rules = null;       /* the POSIX TZ string (standard time, daylight saving time) */
  }

  /* reading a POSIX TZ string (glibc's '__tzset_parse_tz') */
  private static final class TzParse
  {
    final String s;
    int i = 0;
    final TzRule[] rules = {new TzRule(), new TzRule()};

    TzParse(String s)
    {
      this.s = s;
    }

    int ch(int k)
    {
      return (k < s.length()) ? s.charAt(k) : 0;
    }

    static boolean isalpha(int c)
    {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    static boolean isdigit(int c)
    {
      return c >= '0' && c <= '9';
    }

    boolean parse_tzname(int whichrule)
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
    int consumed;

    int scanhms(int[] v)
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

    boolean parse_offset(int whichrule)
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
        rules[whichrule].offset = (int) (sign * (Math.min(v[2], 59) + Math.min(v[1], 59) * 60 + Math.min(v[0], 24) * 3600));
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

    boolean parse_rule(int whichrule)
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
        boolean negative = ch(i) == '-';
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
    TzRule[] parse()
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

  private static int be32(byte[] b, int p)
  {
    return ((b[p] & 0xFF) << 24) | ((b[p + 1] & 0xFF) << 16) | ((b[p + 2] & 0xFF) << 8) | (b[p + 3] & 0xFF);
  }

  private static long be64(byte[] b, int p)
  {
    return ((long) be32(b, p) << 32) | (be32(b, p + 4) & 0xFFFFFFFFL);
  }

  /* a TZif file, or null */
  private static TzZone readtzif(Path p)
  {
    try
    {
      byte[] b = Files.readAllBytes(p);
      if (b.length < 44 || b[0] != 'T' || b[1] != 'Z' || b[2] != 'i' || b[3] != 'f')
        return null;
      TzZone z = new TzZone();
      z.tzfile = true;
      int pos = 0;
      int tsize = 4;
      if (b[4] >= '2')
      { /* the 64-bit data, after the 32-bit data */
        int isutcnt = be32(b, 20), isstdcnt = be32(b, 24), leapcnt = be32(b, 28);
        int timecnt = be32(b, 32), typecnt = be32(b, 36), charcnt = be32(b, 40);
        pos = 44 + timecnt * 4 + timecnt + typecnt * 6 + charcnt + leapcnt * 8 + isstdcnt + isutcnt;
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
      z.isdst = new boolean[typecnt];
      int[] ai = new int[typecnt];
      for (int i = 0; i < typecnt; i++, q += 6)
      {
        z.utoff[i] = be32(b, q);
        z.isdst[i] = b[q + 4] != 0;
        ai[i] = b[q + 5] & 0xFF;
      }
      z.abbr = new String[typecnt];
      for (int i = 0; i < typecnt; i++)
      {
        int e = q + ai[i];
        while (e < q + charcnt && b[e] != 0)
          e++;
        z.abbr[i] = new String(b, q + ai[i], e - q - ai[i], StandardCharsets.ISO_8859_1);
      }
      q += charcnt + leapcnt * (tsize + 4) + isstdcnt + isutcnt;
      if (tsize == 8 && q < b.length && b[q] == '\n')
      {
        int e = q + 1;
        while (e < b.length && b[e] != '\n')
          e++;
        if (e > q + 1)
          z.rules = new TzParse(new String(b, q + 1, e - q - 1, StandardCharsets.ISO_8859_1)).parse();
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
    String tz = System.getenv("TZ");
    if (tz != null && tz.isEmpty())
      tz = "Universal";
    if (tz != null && tz.startsWith(":"))
      tz = tz.substring(1);
    String file = (tz == null) ? "/etc/localtime" : tz;
    String dir = System.getenv("TZDIR");
    Path p = file.startsWith("/") ? Path.of(file) : Path.of((dir != null) ? dir : "/usr/share/zoneinfo", file);
    TzZone z = file.isEmpty() ? null : readtzif(p);
    if (z != null)
      return z;
    z = new TzZone();
    if (tz == null || tz.isEmpty() || tz.equals("/etc/localtime"))
    { /* none: UTC (or, off Linux, the JVM's zone) */
      if (tz == null && !System.getProperty("os.name", "").startsWith("Linux"))
        return null;
      z.rules = new TzParse("UTC0").parse();
      return z;
    }
    z.rules = new TzParse(tz).parse();
    return z;
  }

  private static final TzZone tzzone = readzone();

  /* the zone without TZif data and TZ: the JVM's, through java.time */
  private static final ZoneRules jvmrules = (tzzone == null) ? ZoneId.systemDefault().getRules() : null;

  /* 400 years, in seconds: the calendar (and every yearly rule of a zone) repeats */
  private static final long CYCLE = 146097L * 86400;
  /* java.time works between these (years -100000 and 100000); times out of them fold in */
  private static final long TLO = days_from_civil(-100000, 1, 1) * 86400;
  private static final long THI = days_from_civil(100000, 1, 1) * 86400;

  private static long fold(long t)
  {
    if (t >= THI)
      return THI - CYCLE + Math.floorMod(t - THI, CYCLE);
    if (t < TLO)
      return TLO + Math.floorMod(t - TLO, CYCLE);
    return t;
  }

  private static final int[][] mon_ydays = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};

  private static boolean isleap(int y)
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

  private static final ByteView GMT = ByteView.of("GMT");

  /* the broken-down time of 't' in a zone 'off' seconds east (null if its year does not fit) */
  private static OsDate breakdown(long t, long off, int isdst, ByteView zonename)
  {
    long lt;
    try
    {
      lt = Math.addExact(t, off);
    }
    catch (ArithmeticException e)
    {
      return null;
    }
    long days = Math.floorDiv(lt, 86400);
    long rem = Math.floorMod(lt, 86400);
    long z = days + 719468;
    long era = Math.floorDiv(z, 146097);
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = (mp < 10) ? mp + 3 : mp - 9;
    long y = yoe + era * 400 + ((m <= 2) ? 1 : 0);
    long year = y - 1900;
    if (year < Integer.MIN_VALUE || year > Integer.MAX_VALUE)
      return null;
    OsDate r = new OsDate();
    r.year = (int) year;
    r.month = (int) m - 1;
    r.day = (int) d;
    r.hour = (int) (rem / 3600);
    r.min = (int) (rem % 3600 / 60);
    r.sec = (int) (rem % 60);
    r.yday = (int) (days - days_from_civil(y, 1, 1));
    r.wday = Math.floorMod(days + 4, 7);
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
    boolean dst = (c0 > c1) ? (t < c1 || t >= c0) : (t >= c0 && t < c1);
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
      Instant at = Instant.ofEpochSecond(fold(t));
      boolean dst = jvmrules.isDaylightSavings(at);
      String name = TimeZone.getDefault().getDisplayName(dst, TimeZone.SHORT, Locale.US);
      return breakdown(t, jvmrules.getOffset(at).getTotalSeconds(), dst ? 1 : 0, ByteView.of(name));
    }
    if (!z.tzfile)
      return byrules(t, z.rules);
    int n = z.trans.length;
    if (z.utoff.length == 0)
      return breakdown(t, 0, 0, ByteView.EMPTY);
    if (n == 0 || t < z.trans[0])
    { /* before the first transition: the first standard time */
      int i = 0;
      while (i < z.isdst.length && z.isdst[i])
        i++;
      return bytype(t, z, (i == z.isdst.length) ? 0 : i);
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

  private static boolean isdst_differ(int a, int b)
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

  public static OptionalLong os_mktime(OsDate tp)
  {
    int remaining_probes = 6;
    int sec = tp.sec;
    int min = tp.min;
    int hour = tp.hour;
    int mday = tp.day;
    int mon = tp.month;
    int year_requested = tp.year;
    int isdst = tp.isdst;
    boolean dst2 = false;
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
    boolean found = false;
    for (;;)
    {
      long[] tt = {t};
      tm = ranged_convert(tt);
      if (tm == null)
        return OptionalLong.empty();
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
        return OptionalLong.empty();
      }
      t1 = t2;
      t2 = t;
      t += dt;
      dst2 = tm.isdst != 0;
    }
    if (!found && isdst_differ(isdst, tm.isdst))
    { /* look for a time near with the requested 'isdst', and use its offset */
      final int stride = 601200;
      final int duration_max = 457243200;
      final int delta_bound = duration_max / 2 + stride;
      search:
      for (int delta = stride; delta < delta_bound; delta += stride)
        for (int direction = -1; direction <= 1; direction += 2)
        {
          long ot = t + (long) delta * direction;
          long[] ott = {ot};
          OsDate otm = ranged_convert(ott);
          if (otm == null)
            return OptionalLong.empty();
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
              break search;
            }
          }
        }
      if (!found)
      { /* none: the usual difference of one hour */
        int dst_difference = ((isdst == 0) ? 1 : 0) - ((tm.isdst == 0) ? 1 : 0);
        t += dst_difference * 60L * 60;
        tm = os_localtime(t);
        if (tm == null)
          return OptionalLong.empty();
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
        t = Math.addExact(t, sec_adjustment);
      }
      catch (ArithmeticException e)
      {
        errno = EOVERFLOW;
        return OptionalLong.empty();
      }
      tm = os_localtime(t);
      if (tm == null)
        return OptionalLong.empty();
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
    if (t == -1)
      return OptionalLong.empty();
    return OptionalLong.of(t);
  }

  /* is the year 'year' (since 1900) a leap year? */
  private static boolean isleap4(long year)
  {
    long y = year + 1900;
    return (y & 3) == 0 && (y % 100 != 0 || y % 400 == 0);
  }

  /* }------------------------------------------------------ */

  public static double os_timediff(long t1, long t2)
  {
    long d = t1 - t2;
    if (((t1 ^ t2) & (t1 ^ d)) < 0) /* overflow? */
      return BigInteger.valueOf(t1).subtract(BigInteger.valueOf(t2)).doubleValue();
    return (double) d;
  }

  private static final String[] wdays = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  private static final String[] months = {"January", "February", "March", "April", "May", "June", "July", "August",
                                          "September", "October", "November", "December"};

  /* the bytes of a 'strftime' result */
  private static final class Out
  {
    byte[] b = new byte[64];
    int n = 0;

    void add(int c)
    {
      if (n == b.length)
        b = java.util.Arrays.copyOf(b, n * 2);
      b[n++] = (byte) c;
    }

    void add(String s)
    {
      for (int i = 0; i < s.length(); i++)
        add(s.charAt(i));
    }

    void add(ByteView v)
    {
      for (int i = 0; i < v.size(); i++)
        add(v.at(i));
    }

    /* a number in at least 'width' digits, padded with 'pad' */
    void num(long v, int width, char pad)
    {
      String s = Long.toString(Math.abs(v));
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
        o.num(Math.floorDiv(year, 100), 1, '0');
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
        diff = Math.abs(diff) / 60;
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

  public static int os_strftime(ByteSpan out, int off, ByteView fmt, OsDate d)
  {
    int z = fmt.find(0);
    Out o = new Out();
    strftime(o, (z == ByteView.npos) ? fmt : fmt.substr(0, z), d);
    if (o.n >= out.size() - off)
      return 0;
    out.copy_from(new ByteView(o.b, 0, o.n), off);
    out.set(off + o.n, 0);
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
      Files.delete(path(path));
      return new OsResult(true, errno);
    }
    catch (IOException e)
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
      Path f = path(from);
      Path t = path(to);
      Files.move(f, t, StandardCopyOption.ATOMIC_MOVE);
      return new OsResult(true, errno);
    }
    catch (IOException e)
    {
      errno = errnoOf(e);
      return new OsResult(false, errno);
    }
  }

  private static final String TMPLETTERS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

  /* 'mkstemp' of "<dir>/<prefix>XXXXXX": the file made (mode 0600), or null */
  private static Path mkstemp(String prefix)
  {
    ThreadLocalRandom rnd = ThreadLocalRandom.current();
    for (int attempt = 0; attempt < 62 * 62 * 62; attempt++)
    {
      StringBuilder sb = new StringBuilder(prefix);
      for (int i = 0; i < 6; i++)
        sb.append(TMPLETTERS.charAt(rnd.nextInt(TMPLETTERS.length())));
      Path p = Path.of(sb.toString());
      try
      {
        Files.createFile(p, PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        return p;
      }
      catch (FileAlreadyExistsException e)
      {
        continue;
      }
      catch (IOException | UnsupportedOperationException e)
      {
        errno = (e instanceof IOException io) ? errnoOf(io) : EINVAL;
        return null;
      }
    }
    errno = EEXIST;
    return null;
  }

  public static InternalString os_tmpname()
  {
    Path p = mkstemp("/tmp/lua_");
    if (p == null)
      return null;
    return new InternalString(new ByteView(p.toString().getBytes(StandardCharsets.UTF_8)));
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Commands
  ** =======================================================
  */

  private static final String SHELL = "/bin/sh";

  public static boolean os_shell_available()
  {
    return Files.isExecutable(Path.of(SHELL));
  }

  /*
  ** A command runs as C's 'system' and 'popen' run it: "/bin/sh -c cmd"
  ** (with $0 "sh"). The JVM reports a death by signal n as the exit code
  ** 128 + n, as it reports an 'exit 128 + n': to tell them apart, the
  ** shell's EXIT trap (which runs when it exits, not when a signal kills
  ** it) writes its status to a file of its own.
  */
  private static final class Command
  {
    Process proc;
    Path marker;
  }

  private static Command launch(ByteView cmd, ProcessBuilder.Redirect in, ProcessBuilder.Redirect out)
      throws IOException
  {
    Command c = new Command();
    String text = cstr(cmd);
    String script = text;
    int saved = errno;
    c.marker = mkstemp(System.getProperty("java.io.tmpdir", "/tmp") + "/lua_st_");
    errno = saved;
    if (c.marker != null)
    {
      String m = c.marker.toString();
      if (m.matches("[A-Za-z0-9_./-]+"))
        script = "trap 'echo $? >" + m + "' EXIT; " + text;
      else
      {
        Files.deleteIfExists(c.marker);
        c.marker = null;
      }
    }
    ProcessBuilder pb = new ProcessBuilder(SHELL, "-c", script, "sh");
    pb.redirectInput(in);
    pb.redirectOutput(out);
    pb.redirectError(ProcessBuilder.Redirect.INHERIT);
    try
    {
      c.proc = pb.start();
    }
    catch (IOException e)
    {
      if (c.marker != null)
        Files.deleteIfExists(c.marker);
      throw e;
    }
    return c;
  }

  /* the wait status of the command, as 'waitpid' gives it */
  private static int waitstatus(Process proc, Path marker)
  {
    int v;
    boolean interrupted = false;
    for (;;)
    {
      try
      {
        v = proc.waitFor();
        break;
      }
      catch (InterruptedException e)
      {
        interrupted = true;
      }
    }
    if (interrupted)
      Thread.currentThread().interrupt();
    boolean exited = v <= 128;
    if (marker != null)
    {
      try
      {
        String s = Files.readString(marker).trim();
        if (!s.isEmpty() && Integer.parseInt(s) == v)
          exited = true;
      }
      catch (IOException | NumberFormatException e)
      {
        /* (no status written: the shell did not exit) */
      }
      try
      {
        Files.deleteIfExists(marker);
      }
      catch (IOException e)
      {
        /* (left behind) */
      }
    }
    if (!exited && v - 128 < 65)
      return v - 128;
    return (v & 0xFF) << 8;
  }

  /* a command that could not start: as C reports it (a shell that cannot run exits 127) */
  private static boolean execfailure(int en)
  {
    return en == ENOENT || en == EACCES || en == ENOEXEC;
  }

  public static OsSystemResult os_system(ByteView cmd)
  {
    errno = 0;
    try
    {
      Command c = launch(cmd, ProcessBuilder.Redirect.INHERIT, ProcessBuilder.Redirect.INHERIT);
      int stat = waitstatus(c.proc, c.marker);
      return new OsSystemResult(stat, 0);
    }
    catch (IOException e)
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
    if (((byte) ((stat & 0x7F) + 1) >> 1) > 0) /* WIFSIGNALED */
      return new OsExecStatus(false, stat & 0x7F);
    return new OsExecStatus(true, stat);
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Dynamic libraries: the JVM loads none (as stock Lua built without
  ** them: every load fails with its message)
  ** =======================================================
  */

  private static final ByteView DLMSG = ByteView.of("dynamic libraries not enabled; check your Lua installation");

  public static OsLib os_dlopen(ByteView path, boolean global)
  {
    return null;
  }

  public static Object os_dlsym(OsLib lib, ByteView sym)
  {
    return null;
  }

  public static lua_CFunction os_dlfunc(OsLib lib, ByteView sym)
  {
    return (lua_CFunction) os_dlsym(lib, sym);
  }

  public static void os_dlclose(OsLib lib)
  {
  }

  public static InternalString os_dlerror()
  {
    return new InternalString(DLMSG);
  }

  /* }====================================================== */

  /*
  ** {======================================================
  ** Streams
  ** =======================================================
  */

  /* the mode of 'fopen' (glibc's reading): null if invalid */
  private static final class Mode
  {
    boolean read;
    boolean write;
    boolean create;
    boolean truncate;
    boolean append;
    boolean excl;
  }

  private static Mode parsemode(ByteView mode)
  {
    String m = cstr(mode);
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
  private static OsFile.Device opendevice(Path p, Mode m) throws IOException
  {
    List<OpenOption> opts = new ArrayList<>();
    if (m.read)
      opts.add(StandardOpenOption.READ);
    if (m.write)
      opts.add(StandardOpenOption.WRITE);
    if (m.create)
      opts.add(m.excl ? StandardOpenOption.CREATE_NEW : StandardOpenOption.CREATE);
    if (m.truncate)
      opts.add(StandardOpenOption.TRUNCATE_EXISTING);
    boolean emulate = m.append && m.read; /* (Java opens no READ + APPEND: every write seeks its end) */
    if (m.append && !m.read)
      opts.add(StandardOpenOption.APPEND);
    FileChannel ch = FileChannel.open(p, opts.toArray(new OpenOption[0]));
    return new OsFile.ChannelDevice(ch, emulate);
  }

  private static int filebufmode(Path p)
  {
    String s = p.toString();
    if (s.startsWith("/dev/"))
    {
      try
      {
        if (isttypath(p.toRealPath().toString()))
          return OsFile.IOLBF;
      }
      catch (IOException e)
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
      Path p = path(path);
      OsFile f = new OsFile(opendevice(p, m), m.read, m.write, filebufmode(p));
      remember(f);
      return new OsOpenResult(f, 0);
    }
    catch (IOException e)
    {
      errno = errnoOf(e);
      return new OsOpenResult(null, errno);
    }
  }

  public static OsOpenResult os_popen(ByteView cmd, ByteView mode)
  {
    errno = 0;
    String m = cstr(mode);
    if (m.isEmpty() || (m.charAt(0) != 'r' && m.charAt(0) != 'w'))
    {
      errno = EINVAL;
      return new OsOpenResult(null, errno);
    }
    boolean reading = m.charAt(0) == 'r';
    try
    {
      Command c = reading ? launch(cmd, ProcessBuilder.Redirect.INHERIT, ProcessBuilder.Redirect.PIPE)
                          : launch(cmd, ProcessBuilder.Redirect.PIPE, ProcessBuilder.Redirect.INHERIT);
      OsFile f = new OsFile(new OsFile.PipeDevice(c.proc, reading, c.marker), reading, !reading, OsFile.IOFBF);
      remember(f);
      return new OsOpenResult(f, 0);
    }
    catch (IOException e)
    {
      errno = errnoOf(e);
      return new OsOpenResult(null, errno);
    }
  }

  public static OsOpenResult os_tmpfile()
  {
    errno = 0;
    Path p = mkstemp(System.getProperty("java.io.tmpdir", "/tmp") + "/tmpf");
    if (p == null)
      return new OsOpenResult(null, errno);
    try
    {
      FileChannel ch = FileChannel.open(p, StandardOpenOption.READ, StandardOpenOption.WRITE);
      Files.deleteIfExists(p); /* (unnamed, as C's) */
      OsFile f = new OsFile(new OsFile.ChannelDevice(ch, false), true, true, OsFile.IOFBF);
      remember(f);
      return new OsOpenResult(f, 0);
    }
    catch (IOException e)
    {
      errno = errnoOf(e);
      try
      {
        Files.deleteIfExists(p);
      }
      catch (IOException e2)
      {
        /* (left behind) */
      }
      return new OsOpenResult(null, errno);
    }
  }

  public static int os_freopen(OsFile f, ByteView path, ByteView mode)
  {
    errno = 0;
    f.lock.lock();
    try
    {
      f.flush();
      try
      {
        f.dev.close();
      }
      catch (IOException e)
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
        Path p = path(path);
        f.reset(opendevice(p, m), m.read, m.write, filebufmode(p));
        return 0;
      }
      catch (IOException e)
      {
        errno = errnoOf(e);
        f.open = false;
        forget(f);
        return errno;
      }
    }
    finally
    {
      f.lock.unlock();
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

  public static int os_fread(OsFile f, ByteSpan out, int off, int n)
  {
    assert off + n <= out.size();
    return f.read(out.array(), out.offset() + off, n);
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

  public static boolean os_readline(OsFile f, ByteSpan buff)
  {
    return f.readline(buff.array(), buff.offset(), buff.size());
  }

  public static boolean os_feof(OsFile f)
  {
    return f.eof;
  }

  public static boolean os_ferror(OsFile f)
  {
    return f.err;
  }

  public static void os_clearerr(OsFile f)
  {
    f.lock.lock();
    try
    {
      f.eof = false;
      f.err = false;
    }
    finally
    {
      f.lock.unlock();
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
    synchronized (openfiles)
    {
      all = openfiles.toArray(new OsFile[0]);
    }
    for (OsFile f : all)
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
    if (!(f.dev instanceof OsFile.PipeDevice pd))
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
  ** EAGAIN when nothing is. (Writes still block: Java's streams cannot
  ** write part of a block; files are not affected, as on Linux.)
  */
  public static int os_setnonblock(OsFile f)
  {
    f.nonblock = true;
    return 0;
  }

  public static void os_flockfile(OsFile f)
  {
    f.lock.lock();
  }

  public static void os_funlockfile(OsFile f)
  {
    f.lock.unlock();
  }

  /* }====================================================== */
}
