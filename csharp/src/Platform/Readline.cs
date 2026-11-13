using System;
using System.Runtime.InteropServices;
using Payonel.LungoLua.Compat;
using ByteView = Payonel.LungoLua.Bytes.ByteView;
using static Payonel.LungoLua.Platform.OsApi;

namespace Payonel.LungoLua.Platform;

/*
** The readline library of .NET ("libreadline.so" to 'os_dlopen'): GNU
** readline's 'readline', 'add_history' and 'rl_readline_name'. When the
** standard input and output are terminals, 'readline' edits the line with
** Console.ReadKey: emacs keys, and the history of the lines given to
** 'add_history'. Otherwise it shows its prompt and reads a line from the
** standard input; when that is not a terminal, it echoes the line, as GNU
** readline does (on a terminal, the terminal echoes it).
*/
public sealed class Readline
{
  private Readline()
  {
  }

  /* the types of 'readline' and 'add_history' (as lua.cpp names them) */
  public delegate ByteView l_readlineT(ByteView prompt);

  public delegate void l_addhistT(ByteView @string);

  /* the program's name, for the conditionals of an init file */
  public static readonly string[] rl_readline_name = {"other"};

  /* the lines 'add_history' was given */
  private static readonly ArrayList<ByteView> history = new ();

  private static readonly bool echo = !isatty(0);

  /* whether 'readline' edits: the standard input and output are terminals */
  private static readonly bool editing = !echo && isatty(1);

  /* the line editor, made by the first 'readline' that edits; 'noeditor' if it could not be made */
  private static Editor editor = null;
  private static bool noeditor = false;

  private static readonly ByteView NL = ByteView.of("\n");

  /* the line read, without its '\n'; null at the end of the input (or on an error) with no line */
  public static ByteView readline(ByteView prompt)
  {
    if (editing && !noeditor)
    {
      if (editor == null)
      {
        try
        {
          editor = Editor.make();
        }
        catch (Exception e) /* no terminal: read as without one */
        {
          noeditor = true;
        }
      }
      if (editor != null)
        return editor.readline(prompt);
    }
    OsFile @in = os_stdin();
    System.IO.MemoryStream line = new System.IO.MemoryStream();
    int c;
    os_print_stdout(prompt);
    os_flush_stdout();
    os_flockfile(@in);
    try
    {
      while ((c = os_getbyte(@in)) != OS_EOF && c != '\n')
        line.WriteByte((byte) c);
    }
    finally
    {
      os_funlockfile(@in);
    }
    if (c == OS_EOF && line.Length == 0)
      return null;
    ByteView l = new ByteView((sbyte[]) (object) line.ToArray());
    if (echo)
    {
      os_print_stdout(l);
      os_print_stdout(NL);
      os_flush_stdout();
    }
    return l;
  }

  public static readonly l_readlineT readline_fn = Readline.readline;

  public static void add_history(ByteView @string)
  {
    lock (history)
    {
      history.add(new ByteView(@string.toArray()));
    }
  }

  public static readonly l_addhistT add_history_fn = Readline.add_history;

  /*
  ** The line editor as GNU readline under lua.c: one per process, on the
  ** console (Console.ReadKey), read and written as UTF-8; emacs keys, no
  ** completion, the history of 'add_history' in memory only. The terminal
  ** keeps its interrupt character: Ctrl-C sends SIGINT, and .NET restores
  ** the terminal before the default action.
  */
  private sealed class Editor
  {
    private readonly System.IO.Stream @out = Console.OpenStandardOutput();

    /* the line: one string per character (a surrogate pair is one) */
    private readonly System.Collections.Generic.List<string> buf = new ();

    /* the cursor, the terminal's width, and the column the line starts at */
    private int pos, cols, start;

    /* where the terminal's cursor is */
    private (int row, int col) drawn;

    /* the prompt as shown, and the columns of its last line */
    private sbyte[] shown;
    private int width;

    /* a SIGINT came while reading (the terminal sent it) */
    private volatile bool intr = false;

    private Editor()
    {
    }

    internal static Editor make()
    {
      if (Console.IsInputRedirected || Console.IsOutputRedirected)
        throw new InvalidOperationException();
      Console.InputEncoding = JCharsets.UTF_8;
      return new Editor();
    }

    internal ByteView readline(ByteView prompt)
    {
      prompt_(prompt);
      os_flush_stdout(); /* what the program wrote comes before the prompt */
      PosixSignalRegistration sigint = null;
      try
      {
        /* as GNU readline: the interrupt character echoed, then the program's handler (at lua.c's prompt, the default: the process ends) */
        sigint = PosixSignalRegistration.Create(PosixSignal.SIGINT, c => { intr = true; write("^C"); });
      }
      catch (Exception e)
      {
      }
      try
      {
        cols = Console.WindowWidth;
        if (cols <= 0)
          cols = 80;
        intr = false;
        buf.Clear();
        pos = 0;
        showprompt();
        int h;
        lock (Readline.history)
        {
          h = Readline.history.size();
        }
        string saved = "";
        for (;;)
        {
          ConsoleKeyInfo k = Console.ReadKey(true);
          if (intr) /* the handler returned: the line goes on, on a new line */
          {
            intr = false;
            System.Text.StringBuilder b = new System.Text.StringBuilder();
            move(b, drawn, at(buf.Count));
            write(b.Append("\r\n").ToString());
            showprompt();
          }
          char ch = k.KeyChar;
          if (k.Key == ConsoleKey.Enter || ch == '\r' || ch == '\n')
          {
            pos = buf.Count;
            refresh();
            write("\r\n");
            return new ByteView(text().getBytes(JCharsets.UTF_8));
          }
          else if (ch == '\u0004') /* Ctrl-D: the end of the input on an empty line, else delete-char */
          {
            if (buf.Count == 0)
              return null;
            if (pos < buf.Count)
              buf.RemoveAt(pos);
          }
          else if (k.Key == ConsoleKey.Backspace || ch == '\b' || ch == '\u007f')
          {
            if (pos > 0)
              buf.RemoveAt(--pos);
          }
          else if (k.Key == ConsoleKey.Delete)
          {
            if (pos < buf.Count)
              buf.RemoveAt(pos);
          }
          else if (k.Key == ConsoleKey.LeftArrow || ch == '\u0002')
          {
            if (pos > 0)
              pos--;
          }
          else if (k.Key == ConsoleKey.RightArrow || ch == '\u0006')
          {
            if (pos < buf.Count)
              pos++;
          }
          else if (k.Key == ConsoleKey.Home || ch == '\u0001')
            pos = 0;
          else if (k.Key == ConsoleKey.End || ch == '\u0005')
            pos = buf.Count;
          else if (k.Key == ConsoleKey.UpArrow || ch == '\u0010') /* previous-history */
          {
            lock (Readline.history)
            {
              if (h == 0)
                continue;
              if (h == Readline.history.size())
                saved = text();
              set(JString.of(Readline.history.get(--h).toArray(), JCharsets.UTF_8));
            }
          }
          else if (k.Key == ConsoleKey.DownArrow || ch == '\u000e') /* next-history */
          {
            lock (Readline.history)
            {
              if (h >= Readline.history.size())
                continue;
              h++;
              set((h == Readline.history.size()) ? saved : JString.of(Readline.history.get(h).toArray(), JCharsets.UTF_8));
            }
          }
          else if (ch == '\u000b') /* kill-line */
            buf.RemoveRange(pos, buf.Count - pos);
          else if (ch == '\u0015') /* unix-line-discard */
          {
            buf.RemoveRange(0, pos);
            pos = 0;
          }
          else if (char.IsLowSurrogate(ch) && pos > 0 && buf[pos - 1].Length == 1 && char.IsHighSurrogate(buf[pos - 1][0]))
            buf[pos - 1] += ch;
          else if (ch >= ' ' && ch != '\u007f')
            buf.Insert(pos++, ch.ToString());
          else
            continue;
          refresh();
        }
      }
      catch (Exception e) when (e is InvalidOperationException || e is System.IO.IOException) /* an error */
      {
        return null;
      }
      finally
      {
        sigint?.Dispose();
      }
    }

    private string text()
    {
      return string.Concat(buf);
    }

    /* the line becomes 's', the cursor at its end */
    private void set(string s)
    {
      buf.Clear();
      for (int i = 0; i < s.Length; i++)
      {
        if (char.IsSurrogatePair(s, i))
        {
          buf.Add(s.Substring(i, 2));
          i++;
        }
        else
          buf.Add(s[i].ToString());
      }
      pos = buf.Count;
    }

    /* the prompt as the terminal takes it: \001 and \002 (GNU readline's bounds of invisible characters) left out */
    private void prompt_(ByteView prompt)
    {
      sbyte[] p = prompt.toArray();
      System.IO.MemoryStream s = new System.IO.MemoryStream(p.Length);
      bool invisible = false;
      width = 0;
      foreach (sbyte c in p)
      {
        if (c == '\u0001')
          invisible = true;
        else if (c == '\u0002')
          invisible = false;
        else
        {
          s.WriteByte((byte) c);
          if (c == '\n' || c == '\r')
            width = 0;
          else if (!invisible && (c & 0xC0) != 0x80)
            width++;
        }
      }
      shown = (sbyte[]) (object) s.ToArray();
    }

    /* the prompt, the line after it (a line of a recalled chunk starts with no prompt) */
    private void showprompt()
    {
      @out.Write((byte[]) (object) shown);
      start = width % cols;
      if (width > 0 && start == 0)
        write("\r\n");
      drawn = (0, start);
      refresh();
    }

    /* the row and column of the place before character 'i' (row 0 is the prompt's last) */
    private (int row, int col) at(int i)
    {
      int row = 0, col = start;
      for (int j = 0; j < i; j++)
      {
        if (buf[j] == "\n")
        {
          row++;
          col = 0;
        }
        else
        {
          if (col == cols)
          {
            row++;
            col = 0;
          }
          col++;
        }
      }
      return (col == cols) ? (row + 1, 0) : (row, col);
    }

    private static void move(System.Text.StringBuilder b, (int row, int col) from, (int row, int col) to)
    {
      if (to.row < from.row)
        b.Append("\u001b[").Append(from.row - to.row).Append('A');
      else if (to.row > from.row)
        b.Append("\u001b[").Append(to.row - from.row).Append('B');
      if (to.col > from.col)
        b.Append("\u001b[").Append(to.col - from.col).Append('C');
      else if (to.col < from.col)
        b.Append("\u001b[").Append(from.col - to.col).Append('D');
    }

    /* the line drawn again from its start, the cursor at 'pos' */
    private void refresh()
    {
      System.Text.StringBuilder b = new System.Text.StringBuilder();
      move(b, drawn, (0, start));
      b.Append("\u001b[J");
      foreach (string c in buf)
        b.Append((c == "\n") ? "\r\n" : c);
      (int row, int col) end = at(buf.Count);
      if (buf.Count > 0 && buf[buf.Count - 1] != "\n" && end.col == 0) /* the last column filled: the cursor to the next line */
        b.Append("\r\n");
      drawn = at(pos);
      move(b, end, drawn);
      write(b.ToString());
    }

    private void write(string s)
    {
      byte[] b = (byte[]) (object) s.getBytes(JCharsets.UTF_8);
      lock (@out)
      {
        @out.Write(b, 0, b.Length);
        @out.Flush();
      }
    }
  }
}
