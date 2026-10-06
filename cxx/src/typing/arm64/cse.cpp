// Port of asmcomp/arm64/CSE.ml: CSE for ARM64, CSEgen's class specialized.
#include "../mach_passes_arch.hpp"

namespace cppcaml::typing::mach_passes::csegen {

OpClass class_of_operation(const mach::Operation& op) {
  if (op.k == mach::Operation::K::Ispecific)
    return op.spec.k == arch::SpecificOperation::K::Ishiftcheckbound ? OpClass::Op_checkbound : OpClass::Op_pure;
  return class_of_operation_generic(op);
}

bool is_cheap_operation(const mach::Operation& op) {
  return op.k == mach::Operation::K::Iconst_int && op.n <= 0x7FFF'FFFF && op.n >= -0x8000'0000LL;
}

}  // namespace cppcaml::typing::mach_passes::csegen
