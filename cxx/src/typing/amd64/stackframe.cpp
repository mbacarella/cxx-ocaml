// Port of asmcomp/amd64/stackframe.ml: Stackframegen's class specialized.
#include "../stackframegen.hpp"

#include "cppcaml/typing/config.hpp"

namespace cppcaml::typing::stackframegen {

const long trap_handler_size = 16;

bool is_call(const mach::Instruction& i) {
  using MK = mach::Operation::K;
  if (i.desc == mach::Instruction::K::Iop && (i.op->k == MK::Iintop || i.op->k == MK::Iintop_imm) &&
      i.op->intop.op == mach::IntegerOperation::Icheckbound)
    return true;
  return is_call_generic(i);
}

bool frame_required(const mach::Fundecl& f, bool contains_calls) {
  return config::with_frame_pointers || frame_required_generic(f, contains_calls);
}

}  // namespace cppcaml::typing::stackframegen
