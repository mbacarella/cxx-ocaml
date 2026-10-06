// Port of asmcomp/amd64/CSE.ml: CSE for amd64, CSEgen's class specialized.
#include "../mach_passes_arch.hpp"

namespace cppcaml::typing::mach_passes::csegen {

OpClass class_of_operation(const mach::Operation& op) {
  using SK = arch::SpecificOperation::K;
  if (op.k == mach::Operation::K::Ispecific) {
    switch (op.spec.k) {
      case SK::Ilea:
      case SK::Isextend32:
      case SK::Izextend32:
      case SK::Iclz:
      case SK::Ictz: return OpClass::Op_pure;
      case SK::Istore_int: return op.spec.is_assign ? OpClass::Op_store_assign : OpClass::Op_store_init;
      case SK::Ioffset_loc: return OpClass::Op_store_assign;
      case SK::Ifloatarithmem:
      case SK::Ifloatsqrtf: return OpClass::Op_load_mutable;
      case SK::Ibswap:
      case SK::Isqrtf: return class_of_operation_generic(op);
    }
  }
  return class_of_operation_generic(op);
}

bool is_cheap_operation(const mach::Operation& op) { return is_cheap_operation_generic(op); }

}  // namespace cppcaml::typing::mach_passes::csegen
