// Port of asmcomp/amd64/arch.ml (the operations on addressing modes and
// specific operations, their printing).  See amd64/arch.hpp.
#include "cppcaml/typing/mach.hpp"

#include "cppcaml/typing/arch_options.hpp"
#include "cppcaml/typing/clflags.hpp"

namespace cppcaml::typing::arch {

using format::Formatter;
using format::fprintf;
using format::pr;
using printmach::reg;
using reg::Regs;

AddressingMode offset_addressing(const AddressingMode& addr, long delta) {
  AddressingMode a = addr;
  a.displ = addr.displ + delta;
  return a;
}

bool operation_is_pure(const SpecificOperation& op) {
  using K = SpecificOperation::K;
  switch (op.k) {
    case K::Ilea:
    case K::Ibswap:
    case K::Isqrtf:
    case K::Isextend32:
    case K::Izextend32:
    case K::Ifloatarithmem:
    case K::Ifloatsqrtf: return true;
    default: return false;
  }
}

namespace {
int cmp_long(long a, long b) { return a < b ? -1 : a > b ? 1 : 0; }
}  // namespace

int compare_addressing(const AddressingMode& a, const AddressingMode& b) {
  if (int c = cmp_long(static_cast<long>(a.k), static_cast<long>(b.k))) return c;
  if (int c = a.sym.compare(b.sym)) return c;
  if (int c = cmp_long(a.scale, b.scale)) return c;
  return cmp_long(a.displ, b.displ);
}

int compare_specific_operation(const SpecificOperation& a, const SpecificOperation& b) {
  if (int c = cmp_long(static_cast<long>(a.k), static_cast<long>(b.k))) return c;
  if (int c = compare_addressing(a.addr, b.addr)) return c;
  if (int c = cmp_long(a.n, b.n)) return c;
  if (int c = cmp_long(a.is_assign, b.is_assign)) return c;
  return cmp_long(static_cast<long>(a.fop), static_cast<long>(b.fop));
}

// Arch.print_addressing
void print_addressing(Formatter& ppf, const AddressingMode& addr, const Regs& arg, std::size_t off) {
  using K = AddressingMode::K;
  auto idx = [&](long n) { return n != 0 ? " + " + std::to_string(n) : std::string(); };
  switch (addr.k) {
    case K::Ibased:
      if (addr.displ == 0) fprintf(ppf, "\"%s\"", addr.sym);
      else fprintf(ppf, "\"%s\" + %i", addr.sym, addr.displ);
      return;
    case K::Iindexed: fprintf(ppf, "%a%s", pr(reg, arg[off]), idx(addr.displ)); return;
    case K::Iindexed2: fprintf(ppf, "%a + %a%s", pr(reg, arg[off]), pr(reg, arg[off + 1]), idx(addr.displ)); return;
    case K::Iscaled: fprintf(ppf, "%a  * %i%s", pr(reg, arg[off]), addr.scale, idx(addr.displ)); return;
    case K::Iindexed2scaled:
      fprintf(ppf, "%a + %a * %i%s", pr(reg, arg[off]), pr(reg, arg[off + 1]), addr.scale, idx(addr.displ));
      return;
  }
}

// Arch.print_specific_operation
void print_specific_operation(Formatter& ppf, const SpecificOperation& op, const Regs& arg) {
  using K = SpecificOperation::K;
  switch (op.k) {
    case K::Ilea: print_addressing(ppf, op.addr, arg); return;
    case K::Istore_int:
      fprintf(ppf, "[%t] := %s %s", [&](Formatter& f) { print_addressing(f, op.addr, arg); }, std::to_string(op.n),
              op.is_assign ? "(assign)" : "(init)");
      return;
    case K::Ioffset_loc:
      fprintf(ppf, "[%t] +:= %i", [&](Formatter& f) { print_addressing(f, op.addr, arg); }, static_cast<long>(op.n));
      return;
    case K::Isqrtf: fprintf(ppf, "sqrtf %a", pr(reg, arg[0])); return;
    case K::Ifloatsqrtf:
      fprintf(ppf, "sqrtf float64[%t]", [&](Formatter& f) { print_addressing(f, op.addr, Regs{arg[0]}); });
      return;
    case K::Ifloatarithmem: {
      const char* name = op.fop == FloatOperation::Ifloatadd   ? "+f"
                         : op.fop == FloatOperation::Ifloatsub ? "-f"
                         : op.fop == FloatOperation::Ifloatmul ? "*f"
                                                                     : "/f";
      fprintf(ppf, "%a %s float64[%t]", pr(reg, arg[0]), name,
              [&](Formatter& f) { print_addressing(f, op.addr, arg, 1); });
      return;
    }
    case K::Ibswap: fprintf(ppf, "bswap_%i %a", static_cast<long>(op.n), pr(reg, arg[0])); return;
    case K::Isextend32: fprintf(ppf, "sextend32 %a", pr(reg, arg[0])); return;
    case K::Izextend32: fprintf(ppf, "zextend32 %a", pr(reg, arg[0])); return;
  }
}

// Machine-specific command-line options
std::vector<arg::Option> command_line_options() {
  arg::Option fpic{"-fPIC", arg::Spec{}, " Generate position-independent machine code (default)"};
  fpic.spec.k = arg::Spec::K::Unit;
  fpic.spec.unit = [] { clflags::pic_code = true; };
  arg::Option fnopic{"-fno-PIC", arg::Spec{}, " Generate position-dependent machine code"};
  fnopic.spec.k = arg::Spec::K::Unit;
  fnopic.spec.unit = [] { clflags::pic_code = false; };
  return {fpic, fnopic};
}

// amd64 on Linux (Unix, no frame pointers, CFI)
const std::vector<std::pair<const char*, std::vector<const char*>>>& supported_configuration() {
  static const std::vector<std::pair<const char*, std::vector<const char*>>> v = {
      {"architecture", {"amd64"}},
      {"system", {"linux"}},
      {"with_frame_pointers", {"false"}},
      {"asm_cfi_supported", {"true"}},
      {"tsan", {"false"}}};
  return v;
}

}  // namespace cppcaml::typing::arch
