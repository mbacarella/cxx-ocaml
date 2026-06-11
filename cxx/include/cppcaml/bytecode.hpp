// The bytecode instruction stream (bytecomp/instruct.mli) and the `-dinstr`
// printer.  Stage after Lambda: Bytegen lowers the Lambda IR to instructions for
// the stack+accumulator VM, validated byte-for-byte against `ocamlc -dinstr` --
// the same dump-parity loop that drove lex/parse/type/lambda.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cppcaml/lambda.hpp"

namespace cppcaml::bytecode {

// instruction (bytecomp/instruct.mli), the subset we emit so far.
enum class Op {
  Label, Acc, Envacc, Push, Pop, Assign, PushRetaddr, Apply, Appterm, Return,
  Restart, Grab, Closure, Closurerec, Offsetclosure, Getglobal, Setglobal,
  Const, Makeblock, Makefloatblock, Getfield, Setfield, Getfloatfield,
  Setfloatfield, Vectlength, Getvectitem, Setvectitem, Getstringchar,
  Getbyteschar, Setbyteschar, Branch, Branchif, Branchifnot, Strictbranchif,
  Strictbranchifnot, Switch, Boolnot, Pushtrap, Poptrap, Raise, Reraise,
  RaiseNotrace, CheckSignals, Ccall, Negint, Addint, Subint, Mulint, Divint,
  Modint, Andint, Orint, Xorint, Lslint, Lsrint, Asrint, Eqint, Neqint, Ltint,
  Gtint, Leint, Geint, Physeq, Physneq, Offsetint, Offsetref, Isint, Isout,
  Perform, Getmethod, Getpubmet, Getdynmet, Stop
};

struct Instr {
  Op op;
  int a = 0;             // primary operand (n / label / field index)
  int b = 0;             // secondary operand (makeblock tag / appterm m)
  std::string str;       // global module name / ccall name
  lambda::LamPtr cst;    // Const payload (a lambda constant node)
  std::vector<int> labels;       // Switch consts then blocks / closurerec labels
  int nconsts = 0;               // Switch: number of const labels
};

// A persistent cons-list of instructions, mirroring Bytegen's continuations
// (cheap head-inspection + prepend, which the peephole helpers rely on).
struct ICell;
using Code = std::shared_ptr<const ICell>;
struct ICell { Instr head; Code tail; };

// Compile a module's Lambda term to its instruction stream (compile_implementation).
Code compile_implementation(const lambda::LamPtr& code, const std::string& module_name);

// Print in `-dinstr` format.
void print_dinstr(const Code& code, std::ostream& out);

}  // namespace cppcaml::bytecode
