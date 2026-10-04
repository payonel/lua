/*
** $Id: lopnames.h $
** Opcode names
** See Copyright Notice in lua.h
*/

#pragma once

#include "internal_string.h"
#include "literals.h"

namespace lua::vm
{

/* ORDER OP */

static const byte_view opnames[] =
{
  lt::MOVE,
  lt::LOADI,
  lt::LOADF,
  lt::LOADK,
  lt::LOADKX,
  lt::LOADFALSE,
  lt::LFALSESKIP,
  lt::LOADTRUE,
  lt::LOADNIL,
  lt::GETUPVAL,
  lt::SETUPVAL,
  lt::GETTABUP,
  lt::GETTABLE,
  lt::GETI,
  lt::GETFIELD,
  lt::SETTABUP,
  lt::SETTABLE,
  lt::SETI,
  lt::SETFIELD,
  lt::NEWTABLE,
  lt::SELF,
  lt::ADDI,
  lt::ADDK,
  lt::SUBK,
  lt::MULK,
  lt::MODK,
  lt::POWK,
  lt::DIVK,
  lt::IDIVK,
  lt::BANDK,
  lt::BORK,
  lt::BXORK,
  lt::SHLI,
  lt::SHRI,
  lt::ADD,
  lt::SUB,
  lt::MUL,
  lt::MOD,
  lt::POW,
  lt::DIV,
  lt::IDIV,
  lt::BAND,
  lt::BOR,
  lt::BXOR,
  lt::SHL,
  lt::SHR,
  lt::MMBIN,
  lt::MMBINI,
  lt::MMBINK,
  lt::UNM,
  lt::BNOT,
  lt::NOT_kw,
  lt::LEN,
  lt::CONCAT,
  lt::CLOSE_kw,
  lt::TBC,
  lt::JMP,
  lt::EQ,
  lt::LT,
  lt::LE,
  lt::EQK,
  lt::EQI,
  lt::LTI,
  lt::LEI,
  lt::GTI,
  lt::GEI,
  lt::TEST,
  lt::TESTSET,
  lt::CALL,
  lt::TAILCALL,
  lt::RETURN_kw,
  lt::RETURN0,
  lt::RETURN1,
  lt::FORLOOP,
  lt::FORPREP,
  lt::TFORPREP,
  lt::TFORCALL,
  lt::TFORLOOP,
  lt::SETLIST,
  lt::CLOSURE,
  lt::VARARG,
  lt::GETVARG,
  lt::ERRNNIL,
  lt::VARARGPREP,
  lt::EXTRAARG
};

}
