using System;
using Payonel.LungoLua.Compat;

namespace Payonel.LungoLua.Runtime;

/* the bytes charged for each kind of engine object ('cost::') */
public sealed class Cost
{
  private Cost()
  {
  }

  public const int Byte = 1;
  public const int Ref = 8;
  public const int Instruction = 4;
  public const int Value = 16;
  public const int TValue = 24;
  public const int Node = 48;
  public const int LocVar = 16;
  public const int Vardesc = 48;
  public const int Upvaldesc = 24;
  public const int Labeldesc = 24;
  public const int AbsLineInfo = 8;
  public const int CallInfo = 88;
  public const int TString = 64;
  public const int Udata = 72;
  public const int Table = 96;
  public const int CClosure = 48;
  public const int LClosure = 48;
  public const int CFuncInfo = 24;
  public const int UpVal = 72;
  public const int BudgetGroup = 104;
  public const int Proto = 168;
  public const int Thread = 280;
  public const int Global = 1024;
}
