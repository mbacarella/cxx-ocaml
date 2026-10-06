// Port of asmcomp/arm64/reload.ml: reloading for the ARM 64 bits,
// Reloadgen's class specialized.
#include "../mach_passes_arch.hpp"

namespace cppcaml::typing::mach_passes::reloadgen {

using reg::Regs;

std::pair<Regs, Regs> reload_operation(Self& self, const mach::Operation& op, const Regs& arg, const Regs& res) {
  if (op.k == mach::Operation::K::Ispecific && op.spec.k == arch::SpecificOperation::K::Imove32) {
    // Like Imove: argument or result can be on stack but not both
    if (stackp(arg[0]) && stackp(res[0]) && !same_loc(arg[0]->loc, res[0]->loc)) return {{self.makereg(arg[0])}, res};
    return {arg, res};
  }
  return self.reload_operation_generic(op, arg, res);
}

Regs reload_test(Self& self, const mach::Test& tst, const Regs& arg) { return self.reload_test_generic(tst, arg); }

}  // namespace cppcaml::typing::mach_passes::reloadgen
