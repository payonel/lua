package com.payonel.lungolua.compiler;

import com.payonel.lungolua.bytes.MBuffer;
import com.payonel.lungolua.bytes.ZIO;
import com.payonel.lungolua.object.TString;
import com.payonel.lungolua.object.Table;
import com.payonel.lungolua.runtime.lua_State;

/* state of the scanner plus state of the parser when shared by all
   functions */
public final class LexState
{
  public int current;                   /* current character (charint) */
  public int linenumber;                /* input line counter */
  public int lastline;                  /* line of last token 'consumed' */
  public Token t = new Token();         /* current token */
  public Token lookahead = new Token(); /* look ahead token */
  public FuncState fs = null;           /* current function (parser) */
  public lua_State L = null;
  public ZIO z = null;         /* input stream */
  public MBuffer buff = null;  /* buffer for tokens */
  public Table h = null;       /* to avoid collection/reuse strings */
  public Dyndata dyd = null;   /* dynamic structures used by the parser */
  public TString source = null; /* current source name */
  public TString envn = null;   /* environment variable name */
  public TString brkn = null;   /* "break" name (used as a label) */
  public TString glbn = null;   /* "global" name (when not a reserved word) */

  public LexState()
  {
  }

  public LexState(LexState o)
  {
    current = o.current;
    linenumber = o.linenumber;
    lastline = o.lastline;
    t = new Token(o.t);
    lookahead = new Token(o.lookahead);
    fs = o.fs;
    L = o.L;
    z = o.z;
    buff = o.buff;
    h = o.h;
    dyd = o.dyd;
    source = o.source;
    envn = o.envn;
    brkn = o.brkn;
    glbn = o.glbn;
  }
}
