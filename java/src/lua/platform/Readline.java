package lua.platform;

import static lua.platform.OsApi.*;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;

import lua.bytes.ByteView;

/*
** The readline library of the JVM ("libreadline.so" to 'os_dlopen'): GNU
** readline's 'readline', 'add_history' and 'rl_readline_name', without
** line editing. 'readline' shows its prompt and reads a line from the
** standard input; when that is not a terminal, it echoes the line, as GNU
** readline does (on a terminal, the terminal echoes it).
*/
public final class Readline
{
  private Readline()
  {
  }

  /* the types of 'readline' and 'add_history' (as lua.cpp names them) */
  public interface l_readlineT
  {
    ByteView call(ByteView prompt);
  }

  public interface l_addhistT
  {
    void call(ByteView string);
  }

  /* the program's name, for the conditionals of an init file */
  public static final String[] rl_readline_name = {"other"};

  /* the lines 'add_history' was given */
  private static final ArrayList<ByteView> history = new ArrayList<>();

  private static final boolean echo = !isatty(0);

  private static final ByteView NL = ByteView.of("\n");

  /* the line read, without its '\n'; null at the end of the input (or on an error) with no line */
  public static ByteView readline(ByteView prompt)
  {
    OsFile in = os_stdin();
    ByteArrayOutputStream line = new ByteArrayOutputStream();
    int c;
    os_print_stdout(prompt);
    os_flush_stdout();
    os_flockfile(in);
    try
    {
      while ((c = os_getbyte(in)) != OS_EOF && c != '\n')
        line.write(c);
    }
    finally
    {
      os_funlockfile(in);
    }
    if (c == OS_EOF && line.size() == 0)
      return null;
    ByteView l = new ByteView(line.toByteArray());
    if (echo)
    {
      os_print_stdout(l);
      os_print_stdout(NL);
      os_flush_stdout();
    }
    return l;
  }

  public static final l_readlineT readline = Readline::readline;

  public static void add_history(ByteView string)
  {
    synchronized (history)
    {
      history.add(new ByteView(string.toArray()));
    }
  }

  public static final l_addhistT add_history = Readline::add_history;
}
