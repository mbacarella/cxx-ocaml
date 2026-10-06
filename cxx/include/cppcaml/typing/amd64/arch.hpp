// Port of asmcomp/amd64/arch.ml: the addressing modes and specific
// operations of amd64.  Included by mach.hpp (through arch.hpp), after Reg.
#pragma once

namespace cppcaml::typing::arch {

struct AddressingMode {
  enum class K : std::uint8_t { Ibased, Iindexed, Iindexed2, Iscaled, Iindexed2scaled } k;
  std::string_view sym;  // Ibased
  long scale = 0;        // Iscaled / Iindexed2scaled
  long displ = 0;
};
inline AddressingMode iindexed(long n) { return {AddressingMode::K::Iindexed, {}, 0, n}; }
AddressingMode offset_addressing(const AddressingMode& addr, long delta);
inline AddressingMode identity_addressing() { return iindexed(0); }

enum class FloatOperation : std::uint8_t { Ifloatadd, Ifloatsub, Ifloatmul, Ifloatdiv };
struct SpecificOperation {
  enum class K : std::uint8_t {
    Ilea, Istore_int, Ioffset_loc, Ifloatarithmem, Ibswap, Iclz, Ictz, Isqrtf, Ifloatsqrtf, Isextend32, Izextend32
  } k;
  AddressingMode addr{};
  std::int64_t n = 0;  // Istore_int's constant, Ioffset_loc's delta, Ibswap's width
  bool is_assign = false;
  FloatOperation fop = FloatOperation::Ifloatadd;
};
bool operation_is_pure(const SpecificOperation& op);
inline bool operation_can_raise(const SpecificOperation&) { return false; }

constexpr bool division_crashes_on_overflow = true;

// A total order consistent with OCaml's structural equality (CSE's
// Rhs_map compares operations with Stdlib.compare: only equality matters)
int compare_addressing(const AddressingMode& a, const AddressingMode& b);
int compare_specific_operation(const SpecificOperation& a, const SpecificOperation& b);

// Printing (the register printer is Printmach's); [off]: the first of
// [arg] the addressing mode reads
void print_addressing(format::Formatter& ppf, const AddressingMode& addr, const reg::Regs& arg, std::size_t off = 0);
void print_specific_operation(format::Formatter& ppf, const SpecificOperation& op, const reg::Regs& arg);

}  // namespace cppcaml::typing::arch
