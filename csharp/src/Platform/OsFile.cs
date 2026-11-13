using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;
using Payonel.LungoLua.Compat;
using IOException = System.IO.IOException;
using Process = System.Diagnostics.Process;

namespace Payonel.LungoLua.Platform;

/*
** A C stdio stream on .NET: a file (a FileStream), a standard stream
** (the descriptors 0, 1 and 2) or a pipe to or from a command, with its own
** buffer as glibc's FILE has (fully buffered, line buffered or unbuffered),
** bytes pushed back ('ungetc'), and the end-of-file and error indicators.
** Errors set 'OsApi.errno' as the C library sets 'errno'.
*/
public sealed class OsFile
{
  /* buffering modes, numbered as 'os_setvbuf' takes them */
  internal const int IONBF = 0;
  internal const int IOFBF = 1;
  internal const int IOLBF = 2;

  /* glibc's buffer for a file is its block size */
  internal const int BLKSIZE = 4096;

  /* a write that failed after 'done' bytes */
  internal sealed class WriteError : IOException
  {
    private const long serialVersionUID = 1L;
    internal readonly int done;
    internal readonly Exception cause;

    internal WriteError(int done, Exception cause) : base(cause.Message, cause)
    {

      this.done = done;
      this.cause = cause;
    }
  }

  /* the descriptor under the stream */
  internal abstract class Device
  {
    internal abstract int read(sbyte[] b, int off, int len)  ;

    internal abstract void write(sbyte[] b, int off, int len)  ;

    virtual internal long position()
    {
      throw new IOException("Illegal seek");
    }

    virtual internal void position(long p)
    {
      throw new IOException("Illegal seek");
    }

    virtual internal long size()
    {
      throw new IOException("Illegal seek");
    }

    internal abstract void close()  ;

    /* the bytes that can be read without blocking (-1: unknown) */
    virtual internal int available()
    {
      return -1;
    }
  }

  /* a file opened by name ('append': every write goes to its end, as O_APPEND) */
  internal sealed class ChannelDevice : Device
  {
    internal readonly FileStream ch;
    internal readonly bool append;

    internal ChannelDevice(FileStream ch, bool append)
    {
      this.ch = ch;
      this.append = append;
    }

    override internal int read(sbyte[] b, int off, int len)
    {
      return ch.Read((byte[]) (object) b, off, len);
    }

    override internal void write(sbyte[] b, int off, int len)
    {
      try
      {
        if (append)
          ch.Position = ch.Length;
        ch.Write((byte[]) (object) b, off, len);
      }
      catch (Exception e) when (OsApi.isioerror(e))
      {
        throw new WriteError(0, e); /* (.NET's FileStream does not tell what a failed write wrote) */
      }
    }

    override internal long position()
    {
      if (!ch.CanSeek)
        throw new IOException("Illegal seek");
      return ch.Position;
    }

    override internal void position(long p)
    {
      if (!ch.CanSeek)
        throw new IOException("Illegal seek");
      ch.Position = p;
    }

    override internal long size()
    {
      if (!ch.CanSeek)
        throw new IOException("Illegal seek");
      return ch.Length;
    }

    override internal void close()
    {
      ch.Dispose();
    }
  }

  /* a directory opened for reading (.NET opens none: its reads fail, as on Linux) */
  internal sealed class DirDevice : Device
  {
    internal long pos = 0;

    override internal int read(sbyte[] b, int off, int len)
    {
      throw new IOException("Is a directory");
    }

    override internal void write(sbyte[] b, int off, int len)
    {
      throw new WriteError(0, new IOException("Bad file descriptor"));
    }

    override internal long position()
    {
      return pos;
    }

    override internal void position(long p)
    {
      pos = p;
    }

    override internal long size()
    {
      return 0;
    }

    override internal void close()
    {
    }
  }

  /* a standard stream: read and written through its descriptor, positioned through its descriptor */
  internal sealed class StdDevice : Device
  {
    internal readonly int fd;
    internal readonly bool input;
    internal readonly FileStream fs; /* the descriptor (null: it is not open) */
    internal readonly Stream io;

    /*
    ** .NET's FileStream reads and writes a seekable file at an offset of its
    ** own (pread, pwrite), and no managed call moves a descriptor's offset:
    ** a seekable standard stream is read and written through .NET's console
    ** streams (read, write), and positioned with libc's 'lseek', so that its
    ** offset is the one the commands it runs share, as in C.
    */
    [DllImport("libc", SetLastError = true)]
    private static extern long lseek(int fd, long offset, int whence);

    internal StdDevice(int fd, bool input)
    {
      this.fd = fd;
      this.input = input;
      try
      {
        fs = new FileStream(new SafeFileHandle((IntPtr) fd, true), input ? FileAccess.Read : FileAccess.Write, 0);
        io = !fs.CanSeek ? fs : input ? Console.OpenStandardInput() : (fd == 1) ? Console.OpenStandardOutput() : Console.OpenStandardError();
      }
      catch (Exception e)
      {
        fs = null;
        io = null;
      }
    }

    private void check()
    {
      if (fs == null)
        throw new IOException("Bad file descriptor");
    }

    private long seek(long off, int whence)
    {
      check();
      if (!fs.CanSeek)
        throw new IOException("Illegal seek");
      long r = lseek(fd, off, whence);
      if (r < 0)
      {
        int en = Marshal.GetLastPInvokeError();
        throw new IOException(OsApi.os_strerror(en).ToString(), en);
      }
      return r;
    }

    override internal int read(sbyte[] b, int off, int len)
    {
      check();
      return io.Read((byte[]) (object) b, off, len);
    }

    override internal void write(sbyte[] b, int off, int len)
    {
      try
      {
        check();
        io.Write((byte[]) (object) b, off, len);
      }
      catch (Exception e) when (OsApi.isioerror(e))
      {
        throw new WriteError(0, e);
      }
    }

    override internal long position()
    {
      return seek(0, 1);
    }

    override internal void position(long p)
    {
      seek(p, 0);
    }

    override internal long size()
    {
      check();
      if (!fs.CanSeek)
        throw new IOException("Illegal seek");
      return fs.Length;
    }

    override internal void close()
    {
      check();
      if (io != fs)
        io.Dispose();
      fs.Dispose();
    }

    /* (.NET tells no descriptor's pending bytes: a terminal or a pipe answers "unknown") */
    override internal int available()
    {
      if (!input || fs == null || !fs.CanSeek)
        return -1;
      return (int) JMath.min(JMath.max(fs.Length - position(), 0), JInteger.MAX_VALUE);
    }
  }

  /* a pipe from ('in') or to ('out') a command */
  internal sealed class PipeDevice : Device
  {
    internal readonly Stream @in;
    internal readonly Stream @out;
    internal readonly Process proc;
    internal readonly string marker;

    internal PipeDevice(Process proc, Stream @in, Stream @out, string marker)
    {
      this.proc = proc;
      this.@in = @in;
      this.@out = @out;
      this.marker = marker;
    }

    override internal int read(sbyte[] b, int off, int len)
    {
      return @in.Read((byte[]) (object) b, off, len);
    }

    override internal void write(sbyte[] b, int off, int len)
    {
      try
      {
        @out.Write((byte[]) (object) b, off, len);
        @out.Flush();
      }
      catch (Exception e) when (OsApi.isioerror(e))
      {
        throw new WriteError(0, e);
      }
    }

    override internal void close()
    {
      if (@in != null)
        @in.Dispose();
      else
        @out.Dispose();
    }

    /* (.NET's pipes do not tell what they hold: 'available' stays "unknown") */
  }

  internal Device dev;
  internal bool canread;
  internal bool canwrite;
  internal int bufmode;
  internal readonly sbyte[] buf = new sbyte[BLKSIZE];
  internal int rpos = 0;                  /* read buffer: buf[rpos..rlen) */
  internal int rlen = 0;
  internal int wlen = 0;                  /* write buffer: buf[0..wlen) */
  internal sbyte[] ub = new sbyte[4];       /* bytes pushed back, the last on top */
  internal int ublen = 0;
  internal bool eof = false;
  internal bool err = false;
  internal bool open = true;
  internal bool nonblock = false;      /* O_NONBLOCK (see 'os_setnonblock') */
  internal const int NONBLOCKROOM = 32768; /* what an unbuffered write to a pipe takes before EAGAIN */
  internal readonly object @lock = new object();

  internal OsFile(Device dev, bool canread, bool canwrite, int bufmode)
  {
    this.dev = dev;
    this.canread = canread;
    this.canwrite = canwrite;
    this.bufmode = bufmode;
  }

  internal static OsFile standard(int fd, bool input, int bufmode)
  {
    StdDevice d = new StdDevice(fd, input);
    return new OsFile(d, input, !input, bufmode);
  }

  private void fail(Exception e)
  {
    err = true;
    OsApi.errno = OsApi.errnoOf(e);
  }

  private void failbadf()
  {
    err = true;
    OsApi.errno = OsApi.EBADF;
  }

  /* a read of the descriptor (one without blocking, if so set) */
  private int devread(sbyte[] b, int off, int len)
  {
    if (nonblock)
    {
      int av = dev.available();
      if (av == 0)
        throw new IOException("Resource temporarily unavailable");
      if (av > 0)
        len = JMath.min(len, av);
    }
    return dev.read(b, off, len);
  }

  /* the buffered write data goes out (or is dropped, as glibc drops it when the write fails): 0, or the error code */
  private int flushbuf()
  {
    if (wlen == 0)
      return 0;
    try
    {
      dev.write(buf, 0, wlen);
      wlen = 0;
      return 0;
    }
    catch (Exception e) when (OsApi.isioerror(e))
    {
      wlen = 0;
      fail(e);
      return OsApi.errno;
    }
  }

  /* the position the program sees: the descriptor's, less what is read ahead, plus what is buffered */
  private long logicalpos()
  {
    return dev.position() - (rlen - rpos) - ublen + wlen;
  }

  /* reading after writing: the written data goes out first */
  private bool toread()
  {
    if (!canread)
    {
      failbadf();
      return false;
    }
    return wlen == 0 || flushbuf() == 0;
  }

  /* writing after reading: the descriptor goes back to where the program read up to */
  private bool towrite()
  {
    if (!canwrite)
    {
      failbadf();
      return false;
    }
    if (rlen > rpos || ublen > 0)
    {
      try
      {
        dev.position(logicalpos());
      }
      catch (Exception e) when (OsApi.isioerror(e))
      {
        /* (unseekable: the read-ahead is dropped) */
      }
    }
    rpos = rlen = 0;
    ublen = 0;
    return true;
  }

  /* refills the buffer: the number of bytes in it, or -1 at the end or on an error */
  private int underflow()
  {
    if (eof)
      return -1;
    if (!toread())
      return -1;
    if (bufmode != IOFBF)
      OsApi.flushlinestdout(this);
    int cap = (bufmode == IONBF) ? 1 : buf.Length;
    try
    {
      int n = devread(buf, 0, cap);
      rpos = 0;
      if (n <= 0)
      {
        rlen = 0;
        eof = true;
        return -1;
      }
      rlen = n;
      return n;
    }
    catch (Exception e) when (OsApi.isioerror(e))
    {
      rpos = rlen = 0;
      fail(e);
      return -1;
    }
  }

  internal int getbyte()
  {
    if (ublen > 0)
      return ub[--ublen] & 0xFF;
    if (rpos < rlen)
      return buf[rpos++] & 0xFF;
    if (underflow() < 0)
      return OsApi.OS_EOF;
    return buf[rpos++] & 0xFF;
  }

  internal void ungetbyte(int c)
  {
    if (c == OsApi.OS_EOF)
      return;
    Monitor.Enter(@lock);
    try
    {
      if (ublen == 0 && rpos > 0 && wlen == 0)
        buf[--rpos] = (sbyte) c;
      else
      {
        if (ublen == ub.Length)
          ub = JArrays.copyOf(ub, ub.Length * 2);
        ub[ublen++] = (sbyte) c;
      }
      eof = false;
    }
    finally
    {
      Monitor.Exit(@lock);
    }
  }

  internal int read(sbyte[] @out, int off, int n)
  {
    Monitor.Enter(@lock);
    try
    {
      int total = 0;
      while (total < n && ublen > 0)
        @out[off + total++] = ub[--ublen];
      while (total < n)
      {
        if (rpos < rlen)
        {
          int k = JMath.min(rlen - rpos, n - total);
          JSystem.arraycopy(buf, rpos, @out, off + total, k);
          rpos += k;
          total += k;
        }
        else if (bufmode == IONBF || n - total >= buf.Length)
        {
          if (eof || !toread())
            break;
          int r;
          try
          {
            r = devread(@out, off + total, n - total);
          }
          catch (Exception e) when (OsApi.isioerror(e))
          {
            fail(e);
            break;
          }
          if (r <= 0)
          {
            eof = true;
            break;
          }
          total += r;
        }
        else if (underflow() < 0)
          break;
      }
      return total;
    }
    finally
    {
      Monitor.Exit(@lock);
    }
  }

  internal bool readline(sbyte[] @out, int off, int size)
  {
    Monitor.Enter(@lock);
    try
    {
      if (size <= 0)
        return false;
      int i = 0;
      while (i < size - 1)
      {
        int c = getbyte();
        if (c == OsApi.OS_EOF)
          break;
        @out[off + i++] = (sbyte) c;
        if (c == '\n')
          break;
      }
      @out[off + i] = 0;
      return i > 0 || size == 1;
    }
    finally
    {
      Monitor.Exit(@lock);
    }
  }

  private static int lastnewline(sbyte[] b, int off, int n)
  {
    for (int i = n - 1; i >= 0; i--)
      if (b[off + i] == '\n')
        return i;
    return -1;
  }

  /* buffers 'n' bytes (writing out a full buffer, or a large write directly): the number taken */
  private int bufferbytes(sbyte[] src, int off, int n)
  {
    int i = 0;
    while (i < n)
    {
      if (wlen == 0 && n - i >= buf.Length)
      {
        try
        {
          dev.write(src, off + i, n - i);
          return n;
        }
        catch (Exception e) when (OsApi.isioerror(e))
        {
          fail(e);
          return i + ((e is WriteError w) ? w.done : 0);
        }
      }
      int k = JMath.min(buf.Length - wlen, n - i);
      JSystem.arraycopy(src, off + i, buf, wlen, k);
      wlen += k;
      i += k;
      if (wlen == buf.Length && flushbuf() != 0)
        return i - wlen;
    }
    return n;
  }

  internal int write(sbyte[] src, int off, int n)
  {
    Monitor.Enter(@lock);
    try
    {
      if (n == 0)
        return 0;
      if (!towrite())
        return 0;
      if (bufmode == IONBF)
      {
        if (flushbuf() != 0)
          return 0;
        try
        {
          if (nonblock && dev is PipeDevice && n > NONBLOCKROOM)
          {
            dev.write(src, off, NONBLOCKROOM);
            throw new WriteError(NONBLOCKROOM, new IOException("Resource temporarily unavailable"));
          }
          dev.write(src, off, n);
          return n;
        }
        catch (Exception e) when (OsApi.isioerror(e))
        {
          fail(e);
          return (e is WriteError w) ? w.done : 0;
        }
      }
      if (bufmode == IOLBF)
      {
        int nl = lastnewline(src, off, n);
        if (nl >= 0)
        {
          int taken = bufferbytes(src, off, nl + 1);
          if (taken < nl + 1 || flushbuf() != 0)
            return taken;
          return taken + bufferbytes(src, off + nl + 1, n - nl - 1);
        }
      }
      return bufferbytes(src, off, n);
    }
    finally
    {
      Monitor.Exit(@lock);
    }
  }

  internal int flush()
  {
    Monitor.Enter(@lock);
    try
    {
      if (wlen > 0)
        return flushbuf();
      if (rlen > rpos || ublen > 0)
      {
        try
        {
          dev.position(logicalpos());
          rpos = rlen = 0;
          ublen = 0;
        }
        catch (Exception e) when (OsApi.isioerror(e))
        {
          /* (an unseekable stream keeps what it read ahead) */
        }
      }
      return 0;
    }
    finally
    {
      Monitor.Exit(@lock);
    }
  }

  /* the buffered output goes out (for 'os_flushall'): no error reported */
  internal void flushout()
  {
    Monitor.Enter(@lock);
    try
    {
      if (open && wlen > 0)
        flushbuf();
    }
    finally
    {
      Monitor.Exit(@lock);
    }
  }

  internal int seek(long off, int whence)
  {
    Monitor.Enter(@lock);
    try
    {
      if (wlen > 0 && flushbuf() != 0)
        return OsApi.errno;
      try
      {
        long @base = (whence == 0) ? 0 : (whence == 1) ? logicalpos() : dev.size();
        long target = @base + off;
        if (((@base ^ target) & (off ^ target)) < 0)
        {
          OsApi.errno = OsApi.EOVERFLOW;
          return OsApi.errno;
        }
        if (target < 0)
        {
          OsApi.errno = OsApi.EINVAL;
          return OsApi.errno;
        }
        dev.position(target);
      }
      catch (Exception e) when (OsApi.isioerror(e))
      {
        OsApi.errno = OsApi.errnoOf(e);
        return OsApi.errno;
      }
      rpos = rlen = 0;
      ublen = 0;
      eof = false;
      return 0;
    }
    finally
    {
      Monitor.Exit(@lock);
    }
  }

  internal long tell()
  {
    Monitor.Enter(@lock);
    try
    {
      return logicalpos();
    }
    catch (Exception e) when (OsApi.isioerror(e))
    {
      OsApi.errno = OsApi.errnoOf(e);
      return -1;
    }
    finally
    {
      Monitor.Exit(@lock);
    }
  }

  internal int setvbuf(int mode)
  {
    Monitor.Enter(@lock);
    try
    {
      if (mode < IONBF || mode > IOLBF)
      {
        OsApi.errno = OsApi.EINVAL;
        return OsApi.errno;
      }
      if (wlen > 0)
        flushbuf();
      bufmode = mode;
      return 0;
    }
    finally
    {
      Monitor.Exit(@lock);
    }
  }

  /* closes the descriptor: 0, or the error code (the stream is closed either way) */
  internal int close()
  {
    Monitor.Enter(@lock);
    try
    {
      int r = (wlen > 0) ? flushbuf() : 0;
      try
      {
        dev.close();
      }
      catch (Exception e) when (OsApi.isioerror(e))
      {
        if (r == 0)
        {
          OsApi.errno = OsApi.errnoOf(e);
          r = OsApi.errno;
        }
      }
      open = false;
      OsApi.forget(this);
      return r;
    }
    finally
    {
      Monitor.Exit(@lock);
    }
  }

  /* the stream on another descriptor (as 'freopen' leaves it) */
  internal void reset(Device d, bool canread, bool canwrite, int bufmode)
  {
    this.dev = d;
    this.canread = canread;
    this.canwrite = canwrite;
    this.bufmode = bufmode;
    rpos = rlen = wlen = 0;
    ublen = 0;
    eof = false;
    err = false;
    open = true;
  }
}
