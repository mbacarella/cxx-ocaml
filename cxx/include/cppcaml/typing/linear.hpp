// Ports of asmcomp/linear.ml, linearize.ml, printlinear.ml and
// stackframegen.ml + amd64/stackframe.ml: the linearized machine code Emit
// consumes.
#pragma once

#include <optional>
#include <vector>

#include "cppcaml/typing/mach.hpp"

namespace cppcaml::typing::linear {

using cmm::Label;
using reg::Regs;

struct Instruction;
using Instr = Instruction*;
struct Instruction {
  enum class K : std::uint8_t {
    Lprologue, Lend, Lop, Lreloadretaddr, Lreturn, Llabel, Lbranch, Lcondbranch, Lcondbranch3, Lswitch, Lentertrap,
    Ladjust_trap_depth, Lpushtrap, Lpoptrap, Lraise
  } desc;
  const mach::Operation* op = mach::imove_op();  // Lop (the Mach instruction's)
  Label lbl = 0;                                  // Llabel, Lbranch, Lcondbranch, Lpushtrap's handler
  mach::Test test{mach::Test::K::Itruetest};      // Lcondbranch
  std::optional<Label> lbl0, lbl1, lbl2;          // Lcondbranch3
  std::vector<Label> lbls;                        // Lswitch
  long delta_traps = 0;                           // Ladjust_trap_depth
  lambda::RaiseKind raise = lambda::RaiseKind::Raise_regular;  // Lraise
  Instr next = nullptr;
  Regs arg;
  Regs res;
  debuginfo::t dbg;
  reg::Set live;
};

bool has_fallthrough(const Instruction& i);
mach::Test invert_test(const mach::Test& t);
Instr end_instr();

struct Fundecl {
  std::string_view fun_name;
  reg::Set fun_args;
  Instr fun_body;
  bool fun_fast;
  debuginfo::t fun_dbg;
  Label fun_tailrec_entry_point_label;
  bool fun_contains_nontail_calls;
  std::vector<long> fun_num_stack_slots;
  bool fun_frame_required;
  long fun_extra_stack_used;
};

Fundecl linearize(const mach::Fundecl& f);
void print_fundecl(format::Formatter& ppf, const Fundecl& f);  // Printlinear.fundecl

}  // namespace cppcaml::typing::linear
