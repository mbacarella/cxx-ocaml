// Port of asmcomp/amd64/reload.ml: reloading for amd64, Reloadgen's class
// specialized.
#include "../mach_passes_arch.hpp"

#include "cppcaml/typing/clflags.hpp"

namespace cppcaml::typing::mach_passes::reloadgen {

using namespace mach;
using reg::Reg;
using reg::Regs;
using MK = mach::Operation::K;
using IO = IntegerOperation;

std::pair<Regs, Regs> reload_operation(Self& self, const mach::Operation& op, const Regs& arg, const Regs& res) {
  switch (op.k) {
    case MK::Iintop:
      switch (op.intop.op) {
        case IO::Iadd:
        case IO::Isub:
        case IO::Iand:
        case IO::Ior:
        case IO::Ixor:
        case IO::Icheckbound:
          // One of the two arguments can reside in the stack, but not both
          if (stackp(arg[0]) && stackp(arg[1])) return {{arg[0], self.makereg(arg[1])}, res};
          return {arg, res};
        case IO::Icomp: {
          // The result must be a register (PR#11803)
          Regs res2 = self.makeregs(res);
          if (stackp(arg[0]) && stackp(arg[1])) return {{arg[0], self.makereg(arg[1])}, res2};
          return {arg, res2};
        }
        case IO::Imul:
          // First argument (= result) must be in register, second arg can
          // reside in the stack
          if (stackp(arg[0])) {
            Reg* r = self.makereg(arg[0]);
            return {{r, arg[1]}, {r}};
          }
          return {arg, res};
        default:  // Imulh, Idiv, Imod, Ilsl, Ilsr, Iasr
          return {arg, res};
      }
    case MK::Iintop_imm:
      if (op.intop.op == IO::Iadd && !same_loc(arg[0]->loc, res[0]->loc))
        // This add will be turned into a lea; args and results must be in
        // registers
        return self.reload_operation_generic(op, arg, res);
      if (op.intop.op == IO::Imul) {
        // The result (= the argument) must be a register (#10626)
        if (stackp(arg[0])) {
          Reg* r = self.makereg(arg[0]);
          return {{r}, {r}};
        }
        return {arg, res};
      }
      if (op.intop.op == IO::Icomp) return {arg, self.makeregs(res)};  // The result must be in a register (PR#11803)
      return {arg, res};
    case MK::Iaddf:
    case MK::Isubf:
    case MK::Imulf:
    case MK::Idivf:
      if (stackp(arg[0])) {
        Reg* r = self.makereg(arg[0]);
        return {{r, arg[1]}, {r}};
      }
      return {arg, res};
    case MK::Ifloatofint:
    case MK::Iintoffloat:
      // Result must be in register, but argument can be on stack
      return {arg, stackp(res[0]) ? Regs{self.makereg(res[0])} : res};
    case MK::Iconst_int:
      if (op.n <= 0x7FFFFFFFLL && op.n >= -0x80000000LL) return {arg, res};
      return self.reload_operation_generic(op, arg, res);
    case MK::Iconst_symbol:
      if (clflags::pic_code || clflags::dlcode) return self.reload_operation_generic(op, arg, res);
      return {arg, res};
    default:  // Other operations: all args and results in registers
      return self.reload_operation_generic(op, arg, res);
  }
}

Regs reload_test(Self& self, const Test& tst, const Regs& arg) {
  using FC = lambda::FloatComparison;
  switch (tst.k) {
    case Test::K::Iinttest:
      // One of the two arguments can reside on stack
      if (stackp(arg[0]) && stackp(arg[1])) return {self.makereg(arg[0]), arg[1]};
      return arg;
    case Test::K::Ifloattest:
      if (tst.fcmp == FC::CFlt || tst.fcmp == FC::CFnlt || tst.fcmp == FC::CFle || tst.fcmp == FC::CFnle) {
        // Cf. emit.mlp: we swap arguments in this case.  First argument
        // can be on stack, second must be in register
        if (stackp(arg[1])) return {arg[0], self.makereg(arg[1])};
        return arg;
      }
      // Second argument can be on stack, first must be in register
      if (stackp(arg[0])) return {self.makereg(arg[0]), arg[1]};
      return arg;
    default: return arg;  // The argument(s) can be either in register or on stack
  }
}

}  // namespace cppcaml::typing::mach_passes::reloadgen
