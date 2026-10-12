package lua.platform;

import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.Path;
import java.util.concurrent.locks.ReentrantLock;

/*
** A C stdio stream on the JVM: a file (a FileChannel), a standard stream
** (FileDescriptor.in/out/err) or a pipe to or from a command, with its own
** buffer as glibc's FILE has (fully buffered, line buffered or unbuffered),
** bytes pushed back ('ungetc'), and the end-of-file and error indicators.
** Errors set 'OsApi.errno' as the C library sets 'errno'.
*/
public final class OsFile
{
  /* buffering modes, numbered as 'os_setvbuf' takes them */
  static final int IONBF = 0;
  static final int IOFBF = 1;
  static final int IOLBF = 2;

  /* glibc's buffer for a file is its block size */
  static final int BLKSIZE = 4096;

  /* a write that failed after 'done' bytes */
  static final class WriteError extends IOException
  {
    private static final long serialVersionUID = 1L;
    final int done;
    final IOException cause;

    WriteError(int done, IOException cause)
    {
      super(cause.getMessage(), cause);
      this.done = done;
      this.cause = cause;
    }
  }

  /* the descriptor under the stream */
  abstract static class Device
  {
    abstract int read(byte[] b, int off, int len) throws IOException;

    abstract void write(byte[] b, int off, int len) throws IOException;

    long position() throws IOException
    {
      throw new IOException("Illegal seek");
    }

    void position(long p) throws IOException
    {
      throw new IOException("Illegal seek");
    }

    long size() throws IOException
    {
      throw new IOException("Illegal seek");
    }

    abstract void close() throws IOException;

    /* the bytes that can be read without blocking (-1: unknown) */
    int available() throws IOException
    {
      return -1;
    }
  }

  /* a file opened by name ('append': every write goes to its end, as O_APPEND) */
  static final class ChannelDevice extends Device
  {
    final FileChannel ch;
    final boolean append;

    ChannelDevice(FileChannel ch, boolean append)
    {
      this.ch = ch;
      this.append = append;
    }

    @Override
    int read(byte[] b, int off, int len) throws IOException
    {
      return ch.read(ByteBuffer.wrap(b, off, len));
    }

    @Override
    void write(byte[] b, int off, int len) throws IOException
    {
      ByteBuffer bb = ByteBuffer.wrap(b, off, len);
      try
      {
        if (append)
          ch.position(ch.size());
        while (bb.hasRemaining())
          ch.write(bb);
      }
      catch (IOException e)
      {
        throw new WriteError(bb.position() - off, e);
      }
    }

    @Override
    long position() throws IOException
    {
      return ch.position();
    }

    @Override
    void position(long p) throws IOException
    {
      ch.position(p);
    }

    @Override
    long size() throws IOException
    {
      return ch.size();
    }

    @Override
    void close() throws IOException
    {
      ch.close();
    }
  }

  /* a standard stream: read and written through its descriptor, positioned through its channel */
  static final class StdDevice extends Device
  {
    final FileInputStream in;
    final FileOutputStream out;
    final FileChannel ch;

    StdDevice(FileInputStream in, FileOutputStream out)
    {
      this.in = in;
      this.out = out;
      this.ch = (in != null) ? in.getChannel() : out.getChannel();
    }

    @Override
    int read(byte[] b, int off, int len) throws IOException
    {
      return in.read(b, off, len);
    }

    @Override
    void write(byte[] b, int off, int len) throws IOException
    {
      try
      {
        out.write(b, off, len);
      }
      catch (IOException e)
      {
        throw new WriteError(0, e);
      }
    }

    @Override
    long position() throws IOException
    {
      return ch.position();
    }

    @Override
    void position(long p) throws IOException
    {
      ch.position(p);
    }

    @Override
    long size() throws IOException
    {
      return ch.size();
    }

    @Override
    void close() throws IOException
    {
      if (in != null)
        in.close();
      else
        out.close();
    }

    @Override
    int available() throws IOException
    {
      return (in != null) ? in.available() : -1;
    }
  }

  /* a pipe from ('in') or to ('out') a command */
  static final class PipeDevice extends Device
  {
    final InputStream in;
    final OutputStream out;
    final Process proc;
    final Path marker;

    PipeDevice(Process proc, InputStream in, OutputStream out, Path marker)
    {
      this.proc = proc;
      this.in = in;
      this.out = out;
      this.marker = marker;
    }

    @Override
    int read(byte[] b, int off, int len) throws IOException
    {
      return in.read(b, off, len);
    }

    @Override
    void write(byte[] b, int off, int len) throws IOException
    {
      try
      {
        out.write(b, off, len);
        out.flush();
      }
      catch (IOException e)
      {
        throw new WriteError(0, e);
      }
    }

    @Override
    void close() throws IOException
    {
      if (in != null)
        in.close();
      else
        out.close();
    }

    @Override
    int available() throws IOException
    {
      if (in == null)
        return -1;
      int n = in.available();
      return (n == 0 && !proc.isAlive()) ? -1 : n; /* (at its end: a read sees it) */
    }
  }

  Device dev;
  boolean canread;
  boolean canwrite;
  int bufmode;
  final byte[] buf = new byte[BLKSIZE];
  int rpos = 0;                  /* read buffer: buf[rpos..rlen) */
  int rlen = 0;
  int wlen = 0;                  /* write buffer: buf[0..wlen) */
  byte[] ub = new byte[4];       /* bytes pushed back, the last on top */
  int ublen = 0;
  boolean eof = false;
  boolean err = false;
  boolean open = true;
  boolean nonblock = false;      /* O_NONBLOCK (see 'os_setnonblock') */
  static final int NONBLOCKROOM = 32768; /* what an unbuffered write to a pipe takes before EAGAIN */
  final ReentrantLock lock = new ReentrantLock();

  OsFile(Device dev, boolean canread, boolean canwrite, int bufmode)
  {
    this.dev = dev;
    this.canread = canread;
    this.canwrite = canwrite;
    this.bufmode = bufmode;
  }

  static OsFile standard(FileDescriptor fd, boolean input, int bufmode)
  {
    StdDevice d = input ? new StdDevice(new FileInputStream(fd), null) : new StdDevice(null, new FileOutputStream(fd));
    return new OsFile(d, input, !input, bufmode);
  }

  private void fail(IOException e)
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
  private int devread(byte[] b, int off, int len) throws IOException
  {
    if (nonblock)
    {
      int av = dev.available();
      if (av == 0)
        throw new IOException("Resource temporarily unavailable");
      if (av > 0)
        len = Math.min(len, av);
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
    catch (IOException e)
    {
      wlen = 0;
      fail(e);
      return OsApi.errno;
    }
  }

  /* the position the program sees: the descriptor's, less what is read ahead, plus what is buffered */
  private long logicalpos() throws IOException
  {
    return dev.position() - (rlen - rpos) - ublen + wlen;
  }

  /* reading after writing: the written data goes out first */
  private boolean toread()
  {
    if (!canread)
    {
      failbadf();
      return false;
    }
    return wlen == 0 || flushbuf() == 0;
  }

  /* writing after reading: the descriptor goes back to where the program read up to */
  private boolean towrite()
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
      catch (IOException e)
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
    int cap = (bufmode == IONBF) ? 1 : buf.length;
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
    catch (IOException e)
    {
      rpos = rlen = 0;
      fail(e);
      return -1;
    }
  }

  int getbyte()
  {
    if (ublen > 0)
      return ub[--ublen] & 0xFF;
    if (rpos < rlen)
      return buf[rpos++] & 0xFF;
    if (underflow() < 0)
      return OsApi.OS_EOF;
    return buf[rpos++] & 0xFF;
  }

  void ungetbyte(int c)
  {
    if (c == OsApi.OS_EOF)
      return;
    lock.lock();
    try
    {
      if (ublen == 0 && rpos > 0 && wlen == 0)
        buf[--rpos] = (byte) c;
      else
      {
        if (ublen == ub.length)
          ub = java.util.Arrays.copyOf(ub, ub.length * 2);
        ub[ublen++] = (byte) c;
      }
      eof = false;
    }
    finally
    {
      lock.unlock();
    }
  }

  int read(byte[] out, int off, int n)
  {
    lock.lock();
    try
    {
      int total = 0;
      while (total < n && ublen > 0)
        out[off + total++] = ub[--ublen];
      while (total < n)
      {
        if (rpos < rlen)
        {
          int k = Math.min(rlen - rpos, n - total);
          System.arraycopy(buf, rpos, out, off + total, k);
          rpos += k;
          total += k;
        }
        else if (bufmode == IONBF || n - total >= buf.length)
        {
          if (eof || !toread())
            break;
          int r;
          try
          {
            r = devread(out, off + total, n - total);
          }
          catch (IOException e)
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
      lock.unlock();
    }
  }

  boolean readline(byte[] out, int off, int size)
  {
    lock.lock();
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
        out[off + i++] = (byte) c;
        if (c == '\n')
          break;
      }
      out[off + i] = 0;
      return i > 0 || size == 1;
    }
    finally
    {
      lock.unlock();
    }
  }

  private static int lastnewline(byte[] b, int off, int n)
  {
    for (int i = n - 1; i >= 0; i--)
      if (b[off + i] == '\n')
        return i;
    return -1;
  }

  /* buffers 'n' bytes (writing out a full buffer, or a large write directly): the number taken */
  private int bufferbytes(byte[] src, int off, int n)
  {
    int i = 0;
    while (i < n)
    {
      if (wlen == 0 && n - i >= buf.length)
      {
        try
        {
          dev.write(src, off + i, n - i);
          return n;
        }
        catch (IOException e)
        {
          fail(e);
          return i + ((e instanceof WriteError w) ? w.done : 0);
        }
      }
      int k = Math.min(buf.length - wlen, n - i);
      System.arraycopy(src, off + i, buf, wlen, k);
      wlen += k;
      i += k;
      if (wlen == buf.length && flushbuf() != 0)
        return i - wlen;
    }
    return n;
  }

  int write(byte[] src, int off, int n)
  {
    lock.lock();
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
          if (nonblock && dev instanceof PipeDevice && n > NONBLOCKROOM)
          {
            dev.write(src, off, NONBLOCKROOM);
            throw new WriteError(NONBLOCKROOM, new IOException("Resource temporarily unavailable"));
          }
          dev.write(src, off, n);
          return n;
        }
        catch (IOException e)
        {
          fail(e);
          return (e instanceof WriteError w) ? w.done : 0;
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
      lock.unlock();
    }
  }

  int flush()
  {
    lock.lock();
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
        catch (IOException e)
        {
          /* (an unseekable stream keeps what it read ahead) */
        }
      }
      return 0;
    }
    finally
    {
      lock.unlock();
    }
  }

  /* the buffered output goes out (for 'os_flushall'): no error reported */
  void flushout()
  {
    lock.lock();
    try
    {
      if (open && wlen > 0)
        flushbuf();
    }
    finally
    {
      lock.unlock();
    }
  }

  int seek(long off, int whence)
  {
    lock.lock();
    try
    {
      if (wlen > 0 && flushbuf() != 0)
        return OsApi.errno;
      try
      {
        long base = (whence == 0) ? 0 : (whence == 1) ? logicalpos() : dev.size();
        long target = base + off;
        if (((base ^ target) & (off ^ target)) < 0)
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
      catch (IOException e)
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
      lock.unlock();
    }
  }

  long tell()
  {
    lock.lock();
    try
    {
      return logicalpos();
    }
    catch (IOException e)
    {
      OsApi.errno = OsApi.errnoOf(e);
      return -1;
    }
    finally
    {
      lock.unlock();
    }
  }

  int setvbuf(int mode)
  {
    lock.lock();
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
      lock.unlock();
    }
  }

  /* closes the descriptor: 0, or the error code (the stream is closed either way) */
  int close()
  {
    lock.lock();
    try
    {
      int r = (wlen > 0) ? flushbuf() : 0;
      try
      {
        dev.close();
      }
      catch (IOException e)
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
      lock.unlock();
    }
  }

  /* the stream on another descriptor (as 'freopen' leaves it) */
  void reset(Device d, boolean canread, boolean canwrite, int bufmode)
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
