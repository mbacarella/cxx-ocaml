// Port of asmcomp/arm64/arch.ml: the addressing modes and specific
// operations of the ARM processor in 64-bit mode.  Included by mach.hpp
// (through arch.hpp), after Reg.
#pragma once

#include <optional>

#include "cppcaml/typing/config.hpp"

namespace cppcaml::typing::arch {

inline bool macosx() { return config::system == "macosx"; }
inline bool freebsd() { return config::system == "freebsd"; }
inline bool top_bits_ignore() { return config::system == "linux"; }

// Store-ordering strategy for the multicore memory-model barrier that
// precedes a non-initializing store to a mutable field or array element:
// a store-release (stlr; stlur with FEAT_LRCPC2) by default, a dmb ishld;
// str barrier with Config.model "arm64_barrier".  -flrcpc2, -fno-lrcpc2,
// -fbarrier-store, -fstore-release override the defaults.
extern bool store_release;
extern bool lrcpc2;

// Addressing modes
struct AddressingMode {
  enum class K : std::uint8_t { Iindexed, Ibased } k;  // reg + displ | global var + displ
  std::string_view sym;                              // Ibased
  long displ = 0;
};
inline AddressingMode iindexed(long n) { return {AddressingMode::K::Iindexed, {}, n}; }
AddressingMode offset_addressing(const AddressingMode& addr, long delta);
inline AddressingMode identity_addressing() { return iindexed(0); }

// Specific operations
enum class ArithOperation : std::uint8_t { Ishiftadd, Ishiftsub };
enum class FloatRounding : std::uint8_t { Rnearest_away, Rtoward_zero, Rtoward_pos, Rtoward_neg };
struct SpecificOperation {
  enum class K : std::uint8_t {
    Ipoll_far, Ialloc_far, Icheckbound_far, Icheckbound_imm_far, Ishiftarith, Ishiftcheckbound,
    Ishiftcheckbound_far, Imuladd, Imulsub, Inegmulf, Imuladdf, Inegmuladdf, Imulsubf, Inegmulsubf, Isqrtf,
    Iroundf, Ibswap, Imove32, Isignext, Iclz, Ictz
  } k;
  // Icheckbound_imm_far's bound; Ishiftarith's and Ishiftcheckbound(_far)'s
  // shift; Ibswap's and Isignext's width
  long n = 0;
  ArithOperation arith = ArithOperation::Ishiftadd;       // Ishiftarith
  FloatRounding rounding = FloatRounding::Rnearest_away;  // Iroundf
  std::optional<long> return_label;                       // Ipoll_far
  long bytes = 0;                                         // Ialloc_far
  std::vector<mach::AllocDbginfo> dbginfo;                // Ialloc_far
};
bool operation_is_pure(const SpecificOperation& op);
bool operation_can_raise(const SpecificOperation& op);

// Sizes, endianness; behavior of division
constexpr bool division_crashes_on_overflow = false;

// Recognition of logical immediate arguments
bool is_logical_immediate(std::int64_t x);

// A total order consistent with OCaml's structural equality (CSE's
// Rhs_map compares operations with Stdlib.compare: only equality matters)
int compare_addressing(const AddressingMode& a, const AddressingMode& b);
int compare_specific_operation(const SpecificOperation& a, const SpecificOperation& b);

// Printing (the register printer is Printmach's); [off]: the first of
// [arg] the addressing mode reads
void print_addressing(format::Formatter& ppf, const AddressingMode& addr, const reg::Regs& arg, std::size_t off = 0);
void print_specific_operation(format::Formatter& ppf, const SpecificOperation& op, const reg::Regs& arg);

}  // namespace cppcaml::typing::arch
