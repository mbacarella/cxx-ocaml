// Port of asmcomp/arm64/arch.ml (the operations on addressing modes and
// specific operations, their printing, logical immediates, the
// command-line options).  See arm64/arch.hpp.
#include "cppcaml/typing/mach.hpp"

#include "cppcaml/typing/arch_options.hpp"

namespace cppcaml::typing::arch {

using format::Formatter;
using format::fprintf;
using format::pr;
using printmach::reg;
using reg::Regs;

bool store_release = config::model != "arm64_barrier";
bool lrcpc2 = config::model == "lrcpc2";

// Machine-specific command-line options
std::vector<arg::Option> command_line_options() {
  auto unit = [](const char* key, const char* doc, void (*f)()) {
    arg::Option o{key, arg::Spec{}, doc};
    o.spec.k = arg::Spec::K::Unit;
    o.spec.unit = f;
    return o;
  };
  return {unit("-flrcpc2",
               " Use FEAT_LRCPC2 store-release with unscaled offset (stlur) for assignment stores (requires an "
               "Armv8.4+ assembler)",
               [] { lrcpc2 = true; }),
          unit("-fno-lrcpc2", " Do not use FEAT_LRCPC2 stlur", [] { lrcpc2 = false; }),
          unit("-fbarrier-store",
               " Use a dmb ishld; str barrier instead of a store-release for assignment stores (faster on some old "
               "cores)",
               [] { store_release = false; }),
          unit("-fstore-release", " Use a store-release (stlr/stlur) for assignment stores (default)",
               [] { store_release = true; })};
}

// arm64 on macOS (no frame pointers, CFI)
const std::vector<std::pair<const char*, const char*>>& supported_configuration() {
  static const std::vector<std::pair<const char*, const char*>> v = {{"architecture", "arm64"},
                                                                     {"system", "macosx"},
                                                                     {"with_frame_pointers", "false"},
                                                                     {"asm_cfi_supported", "true"},
                                                                     {"tsan", "false"}};
  return v;
}

// Operations on addressing modes
AddressingMode offset_addressing(const AddressingMode& addr, long delta) {
  AddressingMode a = addr;
  a.displ = addr.displ + delta;
  return a;
}

// Recognition of logical immediate arguments: an automaton to recognize
// ( 0+1+0* | 1+0+1* ), whose accepting states are 2, 3, 5 and 6
namespace {
struct AutoState {
  bool accepting;
  int next0, next1;
};
constexpr AutoState auto_table[] = {
    {false, 1, 4},  // state 0
    {false, 1, 2},  // state 1
    {true, 3, 2},   // state 2
    {true, 3, 7},   // state 3
    {false, 5, 4},  // state 4
    {true, 5, 6},   // state 5
    {true, 7, 6},   // state 6
    {false, 7, 7},  // state 7: error state
};

bool run_automata(int nbits, int state, std::uint64_t input) {
  for (;;) {
    const AutoState& s = auto_table[state];
    if (nbits <= 0) return s.accepting;
    --nbits;
    state = (input & 1) == 0 ? s.next0 : s.next1;
    input >>= 1;
  }
}

// A length [e] such that [x] is a repetition [BB...B] of a bit pattern [B]
// of length [e]: 64, 32, 16, 8, 4 or 2, the smallest
int logical_imm_length(std::uint64_t x) {
  // [test n]: the low [2n] bits of [x] are two occurrences of the same [n] bits
  auto test = [x](int n) {
    std::uint64_t mask = (std::uint64_t{1} << n) - 1;
    return (x & mask) == ((x >> n) & mask);
  };
  if (!test(32)) return 64;
  if (!test(16)) return 32;
  if (!test(8)) return 16;
  if (!test(4)) return 8;
  if (!test(2)) return 4;
  return 2;
}

int cmp_long(long a, long b) { return a < b ? -1 : a > b ? 1 : 0; }
}  // namespace

// A valid logical immediate is neither [0] nor [-1], a repetition of a
// bit-pattern [B] of length [e] whose low [e] bits match [0+1+0*] or [1+0+1*]
bool is_logical_immediate(std::int64_t x) {
  auto u = static_cast<std::uint64_t>(x);
  return x != 0 && x != -1 && run_automata(logical_imm_length(u), 0, u);
}

// Specific operations that are pure
bool operation_is_pure(const SpecificOperation& op) {
  using K = SpecificOperation::K;
  switch (op.k) {
    case K::Ialloc_far:
    case K::Icheckbound_far:
    case K::Icheckbound_imm_far:
    case K::Ishiftcheckbound:
    case K::Ishiftcheckbound_far: return false;
    default: return true;
  }
}

// Specific operations that can raise
bool operation_can_raise(const SpecificOperation& op) { return !operation_is_pure(op); }

int compare_addressing(const AddressingMode& a, const AddressingMode& b) {
  if (int c = cmp_long(static_cast<long>(a.k), static_cast<long>(b.k))) return c;
  if (int c = a.sym.compare(b.sym)) return c;
  return cmp_long(a.displ, b.displ);
}

int compare_specific_operation(const SpecificOperation& a, const SpecificOperation& b) {
  if (int c = cmp_long(static_cast<long>(a.k), static_cast<long>(b.k))) return c;
  if (int c = cmp_long(a.n, b.n)) return c;
  if (int c = cmp_long(static_cast<long>(a.arith), static_cast<long>(b.arith))) return c;
  if (int c = cmp_long(static_cast<long>(a.rounding), static_cast<long>(b.rounding))) return c;
  if (int c = cmp_long(a.return_label.has_value(), b.return_label.has_value())) return c;
  if (int c = cmp_long(a.return_label.value_or(0), b.return_label.value_or(0))) return c;
  if (int c = cmp_long(a.bytes, b.bytes)) return c;
  // Ialloc_far's debug info: only the far allocations Branch_relaxation
  // makes carry it, and CSE never sees them
  return cmp_long(static_cast<long>(a.dbginfo.size()), static_cast<long>(b.dbginfo.size()));
}

// Printing operations and addressing modes
void print_addressing(Formatter& ppf, const AddressingMode& addr, const Regs& arg, std::size_t off) {
  switch (addr.k) {
    case AddressingMode::K::Iindexed:
      reg(ppf, arg[off]);
      if (addr.displ != 0) fprintf(ppf, " + %i", addr.displ);
      return;
    case AddressingMode::K::Ibased:
      if (addr.displ == 0) fprintf(ppf, "\"%s\"", addr.sym);
      else fprintf(ppf, "\"%s\" + %i", addr.sym, addr.displ);
      return;
  }
}

void print_specific_operation(Formatter& ppf, const SpecificOperation& op, const Regs& arg) {
  using K = SpecificOperation::K;
  switch (op.k) {
    case K::Ipoll_far: fprintf(ppf, "(far) poll"); return;
    case K::Ialloc_far: fprintf(ppf, "(far) alloc %i", op.bytes); return;
    case K::Icheckbound_far: fprintf(ppf, "%a (far) check > %a", pr(reg, arg[0]), pr(reg, arg[1])); return;
    case K::Icheckbound_imm_far: fprintf(ppf, "%a (far) check > %i", pr(reg, arg[0]), op.n); return;
    case K::Ishiftarith: {
      const char* op_name = op.arith == ArithOperation::Ishiftadd ? "+" : "-";
      std::string shift_mark = op.n >= 0 ? "<< " + std::to_string(op.n) : ">> " + std::to_string(-op.n);
      fprintf(ppf, "%a %s %a %s", pr(reg, arg[0]), op_name, pr(reg, arg[1]), shift_mark);
      return;
    }
    case K::Ishiftcheckbound: fprintf(ppf, "check %a >> %i > %a", pr(reg, arg[0]), op.n, pr(reg, arg[1])); return;
    case K::Ishiftcheckbound_far:
      fprintf(ppf, "(far) check %a >> %i > %a", pr(reg, arg[0]), op.n, pr(reg, arg[1]));
      return;
    case K::Imuladd: fprintf(ppf, "(%a * %a) + %a", pr(reg, arg[0]), pr(reg, arg[1]), pr(reg, arg[2])); return;
    case K::Imulsub: fprintf(ppf, "-(%a * %a) + %a", pr(reg, arg[0]), pr(reg, arg[1]), pr(reg, arg[2])); return;
    case K::Inegmulf: fprintf(ppf, "-f (%a *f %a)", pr(reg, arg[0]), pr(reg, arg[1])); return;
    case K::Imuladdf: fprintf(ppf, "%a +f (%a *f %a)", pr(reg, arg[0]), pr(reg, arg[1]), pr(reg, arg[2])); return;
    case K::Inegmuladdf:
      fprintf(ppf, "(-f %a) -f (%a *f %a)", pr(reg, arg[0]), pr(reg, arg[1]), pr(reg, arg[2]));
      return;
    case K::Imulsubf: fprintf(ppf, "%a -f (%a *f %a)", pr(reg, arg[0]), pr(reg, arg[1]), pr(reg, arg[2])); return;
    case K::Inegmulsubf:
      fprintf(ppf, "(-f %a) +f (%a *f %a)", pr(reg, arg[0]), pr(reg, arg[1]), pr(reg, arg[2]));
      return;
    case K::Isqrtf: fprintf(ppf, "sqrtf %a", pr(reg, arg[0])); return;
    case K::Iclz: fprintf(ppf, "clz %a", pr(reg, arg[0])); return;
    case K::Ictz: fprintf(ppf, "ctz %a", pr(reg, arg[0])); return;
    case K::Iroundf: {
      const char* name = op.rounding == FloatRounding::Rnearest_away ? "roundf"
                         : op.rounding == FloatRounding::Rtoward_zero ? "truncf"
                         : op.rounding == FloatRounding::Rtoward_pos  ? "ceilf"
                                                                      : "floorf";
      fprintf(ppf, "%s %a", name, pr(reg, arg[0]));
      return;
    }
    case K::Ibswap: fprintf(ppf, "bswap%i %a", op.n, pr(reg, arg[0])); return;
    case K::Imove32: fprintf(ppf, "move32 %a", pr(reg, arg[0])); return;
    case K::Isignext: fprintf(ppf, "signext%i %a", op.n, pr(reg, arg[0])); return;
  }
}

}  // namespace cppcaml::typing::arch
