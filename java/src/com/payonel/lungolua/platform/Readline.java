package com.payonel.lungolua.platform;

import static com.payonel.lungolua.platform.OsApi.*;

import java.io.ByteArrayOutputStream;
import java.io.IOError;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.ArrayList;
import java.util.logging.Level;
import java.util.logging.Logger;

import com.payonel.lungolua.bytes.ByteView;

import org.jline.keymap.KeyMap;
import org.jline.reader.Binding;
import org.jline.reader.EndOfFileException;
import org.jline.reader.MaskingCallback;
import org.jline.reader.Reference;
import org.jline.reader.UserInterruptException;
import org.jline.reader.impl.DefaultParser;
import org.jline.reader.impl.LineReaderImpl;
import org.jline.reader.impl.history.DefaultHistory;
import org.jline.terminal.Attributes;
import org.jline.terminal.Terminal;
import org.jline.terminal.TerminalBuilder;
import org.jline.utils.InfoCmp.Capability;

import sun.misc.Signal;

/*
** The readline library of the JVM ("libreadline.so" to 'os_dlopen'): GNU
** readline's 'readline', 'add_history' and 'rl_readline_name'. When the
** standard input and output are terminals, 'readline' edits the line with
** JLine (java/lib): emacs keys, and the history of the lines given to
** 'add_history'. Otherwise it shows its prompt and reads a line from the
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

  /* whether 'readline' edits: the standard input and output are terminals */
  private static final boolean editing = !echo && isatty(1);

  /* the line editor, made by the first 'readline' that edits; 'noeditor' if it could not be made */
  private static Editor editor = null;
  private static boolean noeditor = false;

  private static final ByteView NL = ByteView.of("\n");

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
        catch (Exception | LinkageError e) /* no terminal, or no JLine: read as without one */
        {
          noeditor = true;
        }
      }
      if (editor != null)
        return editor.readline(prompt);
    }
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
      if (editor != null)
        editor.lines.add_history(string);
    }
  }

  public static final l_addhistT add_history = Readline::add_history;

  /*
  ** JLine's line reader as GNU readline under lua.c: one per process, on
  ** the system terminal (the FFM provider when native access is enabled,
  ** else 'stty'), read and written as UTF-8; emacs keys, no completion,
  ** the history in memory only, holding only what 'add_history' gives.
  */
  private static final class Editor extends LineReaderImpl
  {
    /* the history: 'add_history' adds any line; the reader adds none */
    private static final class History extends DefaultHistory
    {
      @Override
      public void add(Instant time, String line)
      {
      }

      void add_history(ByteView line)
      {
        internalAdd(Instant.now(), new String(line.toArray(), StandardCharsets.UTF_8));
      }
    }

    private static final Signal SIGINT = new Signal("INT");

    /* JLine's log; GNU readline reports nothing either */
    private static final Logger log = Logger.getLogger("org.jline");

    private final History lines = new History();

    /* the read ended by the interrupt character (the terminal sent SIGINT) */
    private boolean intr = false;

    private Editor(Terminal terminal) throws IOException
    {
      super(terminal, rl_readline_name[0]);
      setVariable(HISTORY_SIZE, Integer.MAX_VALUE);
      setVariable(WORDCHARS, ""); /* a word is letters and digits */
      setVariable(BLINK_MATCHING_PAREN, 0); /* GNU readline's blink-matching-paren is off */
      setVariable(SECONDARY_PROMPT_PATTERN, ""); /* a line of a recalled chunk starts with no prompt */
      option(Option.DISABLE_EVENT_EXPANSION, true); /* no '!' expansion; '\' stays */
      setParser(new DefaultParser().eofOnUnclosedQuote(false).eofOnEscapedNewLine(false)); /* Enter ends the line */
      setHistory(lines);
      lines.attach(this);
      KeyMap<Binding> keys = getKeyMaps().get(MAIN);
      arrow(keys, UP_HISTORY, Capability.key_up, 'A'); /* previous-history, not a search */
      arrow(keys, DOWN_HISTORY, Capability.key_down, 'B');
      arrow(keys, FORWARD_CHAR, Capability.key_right, 'C');
      arrow(keys, BACKWARD_CHAR, Capability.key_left, 'D');
      arrow(keys, BEGINNING_OF_LINE, Capability.key_home, 'H');
      arrow(keys, END_OF_LINE, Capability.key_end, 'F');
    }

    /* a key as GNU readline binds it: the terminal's sequence (terminfo) and the ANSI ones, "ESC [ k" and "ESC O k" */
    private void arrow(KeyMap<Binding> keys, String widget, Capability key, char k)
    {
      Reference r = new Reference(widget);
      String t = KeyMap.key(getTerminal(), key);
      if (t != null && !t.isEmpty())
        keys.bind(r, t);
      keys.bind(r, "\033[" + k, "\033O" + k);
    }

    static Editor make() throws IOException
    {
      log.setLevel(Level.OFF);
      Terminal terminal = TerminalBuilder.builder()
          .system(true)
          .systemOutput(TerminalBuilder.SystemOutput.SysOut)
          .providers("ffm,exec")
          .ffm(nativeAccess())
          .exec(true)
          .jni(false)
          .jansi(false)
          .jna(false)
          .dumb(false)
          .encoding(StandardCharsets.UTF_8)
          .stdinEncoding(StandardCharsets.UTF_8)
          .stdoutEncoding(StandardCharsets.UTF_8)
          .nativeSignals(true)
          .signalHandler(Terminal.SignalHandler.SIG_DFL)
          .build();
      Editor e = new Editor(terminal);
      synchronized (Readline.history)
      {
        for (ByteView l : Readline.history)
          e.lines.add_history(l);
      }
      return e;
    }

    /* whether the FFM provider may call C (JDK 22's Module.isNativeAccessEnabled), without the JVM's warning */
    private static boolean nativeAccess()
    {
      try
      {
        return (Boolean) Module.class.getMethod("isNativeAccessEnabled").invoke(Editor.class.getModule());
      }
      catch (ReflectiveOperationException e)
      {
        return false;
      }
    }

    ByteView readline(ByteView prompt)
    {
      String p = prompt(prompt);
      String buffer = null;
      os_flush_stdout(); /* what the program wrote comes before the prompt */
      for (;;)
      {
        String line;
        intr = false;
        try
        {
          line = readLine(p, null, (MaskingCallback) null, buffer);
        }
        catch (EndOfFileException | IOError e) /* the end of the input, or an error */
        {
          return null;
        }
        catch (UserInterruptException e)
        {
          /* as GNU readline: the signal goes to the program's handler (at lua.c's prompt, the default: the process ends) */
          Thread.interrupted();
          raise_sigint();
          buffer = e.getPartialLine(); /* the handler returned: the line goes on */
          continue;
        }
        if (Thread.interrupted()) /* a SIGINT as the line ended: the program's handler has it */
          raise_sigint();
        return new ByteView(line.getBytes(StandardCharsets.UTF_8));
      }
    }

    /*
    ** C's 'raise(SIGINT)'. The JVM raises a signal only for a handler of its
    ** own (sun.misc.Signal.raise); under the default action (lua.c's at the
    ** prompt) the signal ends the process, and the shell sends it.
    */
    private static void raise_sigint()
    {
      try
      {
        Signal.raise(SIGINT);
        return;
      }
      catch (IllegalArgumentException e) /* no handler in the JVM: the default action, or ignored */
      {
      }
      try
      {
        new ProcessBuilder("/bin/sh", "-c", "kill -INT " + ProcessHandle.current().pid())
            .redirectOutput(ProcessBuilder.Redirect.DISCARD)
            .redirectError(ProcessBuilder.Redirect.DISCARD)
            .start()
            .waitFor();
        Thread.sleep(1000); /* ends here, unless the signal is ignored */
      }
      catch (IOException | InterruptedException e)
      {
      }
    }

    /* the prompt as JLine takes it: '%' is its escape; \001 and \002 (GNU readline's bounds of invisible characters) are its %{ and %} */
    private static String prompt(ByteView prompt)
    {
      String s = new String(prompt.toArray(), StandardCharsets.UTF_8);
      StringBuilder b = new StringBuilder(s.length());
      for (int i = 0; i < s.length(); i++)
      {
        char ch = s.charAt(i);
        if (ch == '%')
          b.append("%%");
        else if (ch == '\001')
          b.append("%{");
        else if (ch == '\002')
          b.append("%}");
        else
          b.append(ch);
      }
      return b.toString();
    }

    @Override
    public Binding readBinding(KeyMap<Binding> keys, KeyMap<Binding> local)
    {
      try
      {
        return super.readBinding(keys, local);
      }
      catch (IOError e)
      {
        intr = e.getCause() instanceof InterruptedIOException;
        throw e;
      }
    }

    /* an interrupted line ends as in GNU readline: the interrupt character echoed as the terminal echoes it, no new line */
    @Override
    protected void doCleanup(boolean nl)
    {
      super.doCleanup(nl && !intr);
      if (intr)
      {
        Attributes a = getTerminal().getAttributes();
        int c = a.getControlChar(Attributes.ControlChar.VINTR);
        if (a.getLocalFlag(Attributes.LocalFlag.ECHOCTL))
        {
          if (c < 0x20 || c == 0x7F)
            getTerminal().writer().write(new char[] {'^', c == 0x7F ? '?' : (char) (c | 0x40)});
          else
            getTerminal().writer().write(c);
          getTerminal().flush();
        }
      }
    }
  }
}
