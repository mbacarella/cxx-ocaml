// Port of asmcomp/arm64/emit.mlp: emission of ARM assembly code, 64-bit
// mode.  The quotations' pieces are appended in order; a piece with an
// effect (a new label, a recorded frame) is computed in its own statement,
// where OCaml evaluates it, the order of C++ arguments being unspecified.
#include "cppcaml/typing/emit.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

#include "../branch_relaxation.hpp"
#include "../emitaux.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/config.hpp"

namespace cppcaml::typing::emit {

namespace {

using namespace emitaux;
using linear::Instr;
using LK = linear::Instruction::K;
using MK = mach::Operation::K;
using IO = mach::IntegerOperation;
using SK = arch::SpecificOperation::K;
using AK = arch::AddressingMode::K;
using Chunk = cmm::MemoryChunk;
using LocK = reg::Location::K;
using reg::Reg;
using reg::Regs;
using Env = PerFunctionEnv;

[[noreturn]] void fatal(const std::string& s) { throw std::runtime_error(s); }

template <class... A>
void emit(const A&... a) {
  (output.append(std::string_view(a)), ...);
}
std::string I(long n) { return std::to_string(n); }

// Names for special regs
Reg* reg_domain_state_ptr() { return proc::phys_reg(25); }  // x28
Reg* reg_trap_ptr() { return proc::phys_reg(23); }          // x26
Reg* reg_alloc_ptr() { return proc::phys_reg(24); }         // x27
Reg* reg_tmp1() { return proc::phys_reg(26); }              // x16
Reg* reg_x8() { return proc::phys_reg(8); }                 // x8
Reg* reg_stack_arg_begin() { return proc::phys_reg(17); }   // x20
Reg* reg_stack_arg_end() { return proc::phys_reg(18); }     // x21

// Output a label
std::string L(long lbl) { return (arch::macosx() ? "L" : ".L") + std::to_string(lbl); }

// Object types
void emit_label_type(long lbl, std::string_view ty) {
  if (config::config_var("asm_size_type_directives") == std::optional<std::string>("true"))
    emit("\t.type\t", L(lbl), ", ", ty, "\n");
}

// Output a pseudo-register
std::string R(const Reg* r) {
  if (r->loc.k != LocK::Reg) fatal("Emit.reg_name");
  return std::string(proc::register_name(r->loc.n));
}

// Likewise, but with the 32-bit name of the register
const char* const int_reg_name_w[] = {"w0",  "w1",  "w2",  "w3",  "w4",  "w5",  "w6",  "w7",  "w8",  "w9",
                                      "w10", "w11", "w12", "w13", "w14", "w15", "w19", "w20", "w21", "w22",
                                      "w23", "w24", "w25", "w26", "w27", "w28", "w16", "w17"};
std::string W(const Reg* r) {
  if (r->loc.k != LocK::Reg) fatal("Emit.emit_wreg");
  return int_reg_name_w[r->loc.n];
}

constexpr bool fp = config::with_frame_pointers;

long align(long n, long a) { return n >= 0 ? (n + a - 1) & -a : n & -a; }

long initial_stack_offset(const linear::Fundecl& f) {
  return 8 * f.fun_num_stack_slots[0] +  // Local int variables
         8 * f.fun_num_stack_slots[1] +  // Local float variables
         (f.fun_frame_required ? 8 + (fp ? 8 : 0) : 0);  // Return address plus optional Frame Pointer
}

long frame_size(const Env& env) { return align(env.stack_offset + initial_stack_offset(*env.f), 16); }

long slot_offset(const Env& env, const reg::Location& loc, long cl) {
  switch (loc.k) {
    case LocK::Incoming:
      if (loc.n < 0) fatal("Emit.slot_offset");
      return frame_size(env) + loc.n;
    case LocK::Local:
      if (cl == 0) return env.stack_offset + loc.n * 8;
      return env.stack_offset + (env.f->fun_num_stack_slots[0] + loc.n) * 8;
    case LocK::Outgoing:
      if (loc.n < 0) fatal("Emit.slot_offset");
      return loc.n;
    default: fatal("Emit.slot_offset: not a stack slot");
  }
}

bool is_stack(const Reg* r) { return r->loc.k != LocK::Unknown && r->loc.k != LocK::Reg; }

// Output a stack reference
std::string emit_stack(const Env& env, const Reg* r) {
  if (r->loc.k == LocK::Domainstate) {
    long ofs = r->loc.n + domainstate::extra_params * 8;
    return "[" + R(reg_domain_state_ptr()) + ", #" + I(ofs) + "]";
  }
  if (is_stack(r)) return "[sp, #" + I(slot_offset(env, r->loc, proc::register_class(r))) + "]";
  fatal("Emit.emit_stack");
}

// Output an addressing mode
std::string emit_symbol_offset(std::string_view s, long ofs) {
  std::string r = emitaux::symbol(s);
  if (ofs > 0) r += "+" + I(ofs);
  else if (ofs < 0) r += "-" + I(-ofs);
  return r;
}

std::string emit_addressing(const arch::AddressingMode& addr, const Reg* r) {
  if (addr.k == AK::Iindexed) return "[" + R(r) + ", #" + I(addr.displ) + "]";
  if (clflags::dlcode) fatal("Emit.emit_addressing");  // see selection.ml
  return "[" + R(r) + ", #:lo12:" + emit_symbol_offset(addr.sym, addr.displ) + "]";
}

// Record live pointers at call points
long record_frame_label(const Env& env, const reg::Set& live, const FrameDebuginfo& dbg) {
  long lbl = cmm::new_label();
  std::vector<long> live_offset;
  for (Reg* r : live) {
    if (r->typ == cmm::MachtypeComponent::Val) {
      if (r->loc.k == LocK::Reg) live_offset.insert(live_offset.begin(), (r->loc.n << 1) + 1);
      else if (is_stack(r)) live_offset.insert(live_offset.begin(), slot_offset(env, r->loc, proc::register_class(r)));
    } else if (r->typ == cmm::MachtypeComponent::Addr) {
      fatal("bad GC root " + reg::name(r));
    }
  }
  record_frame_descr(lbl, frame_size(env), live_offset, dbg);
  return lbl;
}

void record_frame(const Env& env, const reg::Set& live, const FrameDebuginfo& dbg) {
  long lbl = record_frame_label(env, live, dbg);
  emit(L(lbl), ":");
}

void emit_call_gc(const GcCall& gc) {
  emit(L(gc.gc_lbl), ":\tbl\t", emitaux::symbol("caml_call_gc"), "\n");
  emit(L(gc.gc_frame_lbl), ":\tb\t", L(gc.gc_return_lbl), "\n");
}

long bound_error_label(Env& env, const debuginfo::t& dbg) {
  if (clflags::debug || env.bound_error_sites.empty()) {
    long lbl_bound_error = cmm::new_label();
    long lbl_frame = record_frame_label(env, reg::Set{}, dbg_other(dbg));
    env.bound_error_sites.insert(env.bound_error_sites.begin(), BoundErrorCall{lbl_bound_error, lbl_frame});
    return lbl_bound_error;
  }
  return env.bound_error_sites.front().bd_lbl;
}

void emit_call_bound_error(const BoundErrorCall& bd) {
  emit(L(bd.bd_lbl), ":\tbl\t", emitaux::symbol("caml_ml_array_bound_error"), "\n");
  emit(L(bd.bd_frame), ":\n");
}

// Names of various instructions
const char* name_for_comparison(const mach::IntegerComparison& c) {
  using C = lambda::IntegerComparison;
  switch (c.c) {
    case C::Ceq: return "eq";
    case C::Cne: return "ne";
    case C::Cle: return c.is_signed ? "le" : "ls";
    case C::Cge: return c.is_signed ? "ge" : "cs";
    case C::Clt: return c.is_signed ? "lt" : "cc";
    case C::Cgt: return c.is_signed ? "gt" : "hi";
  }
  return "";
}

const char* name_for_int_operation(IO op) {
  switch (op) {
    case IO::Iadd: return "add";
    case IO::Isub: return "sub";
    case IO::Imul: return "mul";
    case IO::Idiv: return "sdiv";
    case IO::Iand: return "and";
    case IO::Ior: return "orr";
    case IO::Ixor: return "eor";
    case IO::Ilsl: return "lsl";
    case IO::Ilsr: return "lsr";
    case IO::Iasr: return "asr";
    default: fatal("Emit.name_for_int_operation");
  }
}

// Decompose an integer constant into four 16-bit shifted fragments.  Omit
// the fragments that are equal to "default" (16 zeros or 16 ones).
std::vector<std::pair<std::int64_t, long>> decompose_int(std::int64_t dflt, std::int64_t n) {
  std::vector<std::pair<std::int64_t, long>> r;
  auto u = static_cast<std::uint64_t>(n);
  for (long pos = 0; pos < 64; pos += 16) {
    auto frag = static_cast<std::int64_t>(u & 0xFFFF);
    u >>= 16;
    if (frag != dflt) r.push_back({frag, pos});
  }
  return r;
}

// Load an integer constant into a register
void emit_movk(const Reg* dst, std::int64_t f, long p) { emit("\tmovk\t", R(dst), ", #", I(f), ", lsl #", I(p), "\n"); }

void emit_intconst(const Reg* dst, std::int64_t n) {
  if (arch::is_logical_immediate(n)) {
    emit("\torr\t", R(dst), ", xzr, #", I(n), "\n");
    return;
  }
  auto dz = decompose_int(0x0000, n);
  auto dn = decompose_int(0xFFFF, n);
  if (dz.size() <= dn.size()) {
    if (dz.empty()) {
      emit("\tmov\t", R(dst), ", xzr\n");
      return;
    }
    emit("\tmovz\t", R(dst), ", #", I(dz[0].first), ", lsl #", I(dz[0].second), "\n");
    for (std::size_t k = 1; k < dz.size(); ++k) emit_movk(dst, dz[k].first, dz[k].second);
  } else {
    if (dn.empty()) {
      emit("\tmovn\t", R(dst), ", #0\n");
      return;
    }
    std::int64_t nf = dn[0].first ^ 0xFFFF;
    emit("\tmovn\t", R(dst), ", #", I(nf), ", lsl #", I(dn[0].second), "\n");
    for (std::size_t k = 1; k < dn.size(); ++k) emit_movk(dst, dn[k].first, dn[k].second);
  }
}

long num_instructions_for_intconst(std::int64_t n) {
  if (arch::is_logical_immediate(n)) return 1;
  long dz = static_cast<long>(decompose_int(0x0000, n).size());
  long dn = static_cast<long>(decompose_int(0xFFFF, n).size());
  return std::max(1L, std::min(dz, dn));
}

// Recognize float constants appropriate for FMOV dst, #fpimm instruction: "a
// normalized binary floating point encoding with 1 sign bit, 4 bits of
// fraction and a 3-bit exponent"
bool is_immediate_float(std::int64_t bits) {
  long exp = static_cast<long>((static_cast<std::uint64_t>(bits) >> 52) & 0x7FF) - 1023;
  std::int64_t mant = bits & 0xF'FFFF'FFFF'FFFFLL;
  return exp >= -3 && exp <= 4 && (mant & 0xF'0000'0000'0000LL) == mant;
}

// Set [rd] to [rs + n].
void emit_addimm_gen(const std::string& rd, const std::string& rs, long n) {
  const char* insn = n >= 0 ? "add" : "sub";
  n = n >= 0 ? n : -n;
  if (n <= 0xFFF) {
    emit("\t", insn, "\t", rd, ", ", rs, ", #", I(n), "\n");
    return;
  }
  if (n > 0xFFF'FFF) fatal("Emit.emit_addimm_gen");
  long nl = n & 0xFFF, nh = n & 0xFFF000;
  emit("\t", insn, "\t", rd, ", ", rs, ", #", I(nh), "\n");
  if (nl != 0) emit("\t", insn, "\t", rd, ", ", rd, ", #", I(nl), "\n");
}

// Adjust sp (up or down) by the given byte amount
void emit_stack_adjustment(long n) {
  if (n != 0) {
    emit_addimm_gen("sp", "sp", n);
    cfi_adjust_cfa_offset(-n);
  }
}

// Deallocate the stack frame and reload the return address before a return
// or tail call
void output_epilogue(const Env& env, const std::function<void()>& f) {
  long n = frame_size(env);
  if (n > 0) emit_stack_adjustment(n);
  if (env.f->fun_frame_required) {
    if (fp) emit("\tldp\tx29, x30, [sp, #-16]\n");
    else emit("\tldr\tx30, [sp, #-8]\n");
  }
  f();
  // reset CFA back because function body may continue
  if (n > 0) cfi_adjust_cfa_offset(n);
}

// Output add-immediate / sub-immediate / cmp-immediate instructions
void emit_addimm(const Reg* rd, const Reg* rs, long n) { emit_addimm_gen(R(rd), R(rs), n); }
void emit_subimm(const Reg* rd, const Reg* rs, long n) { emit_addimm(rd, rs, -n); }
void emit_cmpimm(const Reg* rs, long n) {
  if (n >= 0) emit("\tcmp\t", R(rs), ", #", I(n), "\n");
  else emit("\tcmn\t", R(rs), ", #", I(-n), "\n");
}

// Label a floating-point literal
long float_literal(Env& env, std::int64_t fl) {
  for (const FloatLiteral& x : env.float_literals)
    if (x.fl == fl) return x.lbl;
  long lbl = cmm::new_label();
  env.float_literals.insert(env.float_literals.begin(), FloatLiteral{fl, lbl});
  return lbl;
}

// Emit all pending literals
void emit_literals(Env& env) {
  if (env.float_literals.empty()) return;
  if (arch::macosx()) emit("\t.section\t__TEXT,__literal8,8byte_literals\n");
  else emit("\t.section\t.rodata\n");
  emit("\t.align\t3\n");
  for (const FloatLiteral& x : env.float_literals) {
    emit(L(x.lbl), ":");
    emit_float64_directive(".quad", x.fl);
  }
  env.float_literals.clear();
}

// Emit code to load the address of a symbol
void emit_load_symbol_addr(const Reg* dst, std::string_view s) {
  if (arch::macosx()) {
    emit("\tadrp\t", R(dst), ", ", emitaux::symbol(s), "@GOTPAGE\n");
    emit("\tldr\t", R(dst), ", [", R(dst), ", ", emitaux::symbol(s), "@GOTPAGEOFF]\n");
  } else if (!clflags::dlcode) {
    emit("\tadrp\t", R(dst), ", ", emitaux::symbol(s), "\n");
    emit("\tadd\t", R(dst), ", ", R(dst), ", #:lo12:", emitaux::symbol(s), "\n");
  } else {
    emit("\tadrp\t", R(dst), ", :got:", emitaux::symbol(s), "\n");
    emit("\tldr\t", R(dst), ", [", R(dst), ", #:got_lo12:", emitaux::symbol(s), "]\n");
  }
}

// The sizes of the call GC and bounds check points emitted out-of-line from
// the function body.  See branch_relaxation.mli.
std::pair<long, long> num_call_gc_and_check_bound_points(const Env& env) {
  long call_gc = 0, check_bound = 0;
  for (Instr i = env.f->fun_body; i->desc != LK::Lend; i = i->next) {
    if (i->desc != LK::Lop) continue;
    const mach::Operation& op = *i->op;
    bool checkbound = ((op.k == MK::Iintop || op.k == MK::Iintop_imm) && op.intop.op == IO::Icheckbound) ||
                      (op.k == MK::Ispecific && op.spec.k == SK::Ishiftcheckbound);
    if (op.k == MK::Ialloc && env.f->fun_fast) ++call_gc;
    else if (op.k == MK::Ipoll) ++call_gc;
    else if (checkbound)
      // When not in debug mode, there is at most one check-bound point.
      check_bound = !clflags::debug ? 1 : check_bound + 1;
    else if (op.k == MK::Ispecific &&
             (op.spec.k == SK::Ialloc_far || op.spec.k == SK::Ipoll_far || op.spec.k == SK::Icheckbound_far ||
              op.spec.k == SK::Icheckbound_imm_far || op.spec.k == SK::Ishiftcheckbound_far))
      // never seen: this function is run before branch relaxation
      fatal("Emit.num_call_gc_and_check_bound_points");
  }
  return {call_gc, check_bound};
}

long max_out_of_line_code_offset(long num_call_gc, long num_check_bound) {
  if (num_call_gc < 1 && num_check_bound < 1) return 0;
  long size_of_call_gc = 2, size_of_check_bound = 1;
  // Call-GC points come before check-bound points.
  long size_of_last_thing = num_check_bound >= 1 ? size_of_check_bound : size_of_call_gc;
  long total_size = size_of_call_gc * num_call_gc + size_of_check_bound * num_check_bound;
  long max_offset = total_size - size_of_last_thing;
  if (max_offset < 0) fatal("Emit.max_out_of_line_code_offset");
  return max_offset;
}

// B and BL have +/- 128Mb ranges; for the moment we assume we will never
// exceed this.  AArch64 instructions are 32 bits wide, so [distance] in this
// module means units of 32-bit words.
struct Size {
  enum class CondBranch { TB, CB, Bcc };
  static constexpr CondBranch all[] = {CondBranch::TB, CondBranch::CB, CondBranch::Bcc};

  static long max_displacement(CondBranch b) {
    return b == CondBranch::TB ? 32 * 1024 / 4  // +/- 32Kb
                               : 1 * 1024 * 1024 / 4;  // +/- 1Mb
  }

  static std::optional<CondBranch> classify_instr(const linear::Instruction& i) {
    using TK = mach::Test::K;
    switch (i.desc) {
      case LK::Lop: {
        const mach::Operation& op = *i.op;
        if (op.k == MK::Ialloc || op.k == MK::Ipoll ||
            ((op.k == MK::Iintop || op.k == MK::Iintop_imm) && op.intop.op == IO::Icheckbound) ||
            (op.k == MK::Ispecific && op.spec.k == SK::Ishiftcheckbound))
          return CondBranch::Bcc;
        // The various "far" variants in [specific_operation] don't need to
        // return [Some] here, since their code sequences never contain any
        // conditional branches that might need relaxing.
        return std::nullopt;
      }
      case LK::Lcondbranch:
        switch (i.test.k) {
          case TK::Itruetest:
          case TK::Ifalsetest: return CondBranch::CB;
          case TK::Iinttest:
          case TK::Iinttest_imm:
          case TK::Ifloattest: return CondBranch::Bcc;
          case TK::Ioddtest:
          case TK::Ieventest: return CondBranch::TB;
        }
        return std::nullopt;
      case LK::Lcondbranch3: return CondBranch::Bcc;
      default: return std::nullopt;
    }
  }

  static constexpr long offset_pc_at_branch = 0;

  static long addsub_size(long n) {
    long m = n >= 0 ? n : -n;
    if (m >= 0x1'000'000) fatal("Emit.Size.addsub_size");
    return m <= 0xFFF ? 1 : (m & 0xFFF) == 0 ? 1 : 2;
  }
  static long stack_adj_size(long n) { return addsub_size(n); }  // see emit_stack_adjustment

  static long prologue_size(const linear::Fundecl& f) {
    long stk = initial_stack_offset(f);
    return (stk > 0 ? stack_adj_size(-stk) : 0) + (f.fun_frame_required ? (fp ? 1 + addsub_size(stk - 16) : 1) : 0);
  }
  static long epilogue_size(const linear::Fundecl& f) {
    long stk = initial_stack_offset(f);
    return (stk > 0 ? stack_adj_size(stk) : 0) + (f.fun_frame_required ? 1 : 0) + 1;
  }

  static long alloc_size(long num_bytes) {
    if (num_bytes == 16 || num_bytes == 24 || num_bytes == 32) return 2;
    return 2 + num_instructions_for_intconst(num_bytes);
  }

  static long store_pre_size(const mach::Operation& op) {
    if (!((op.chunk == Chunk::Word_int || op.chunk == Chunk::Word_val) && op.is_assign)) return 0;
    if (!arch::macosx()) return 1;  // Barrier instruction
    if (op.addr.k != AK::Iindexed) return 0;
    return op.addr.displ == 0 ? 0 : addsub_size(op.addr.displ);  // Compute dest address
  }

  static long instr_size(const linear::Fundecl& f, const linear::Instruction& i) {
    using TK = mach::Test::K;
    switch (i.desc) {
      case LK::Lend: return 0;
      case LK::Lprologue: return prologue_size(f);
      case LK::Lop: break;
      case LK::Lreloadretaddr: return 0;
      case LK::Lreturn: return epilogue_size(f) + (arch::top_bits_ignore() ? 0 : 1);
      case LK::Llabel: return 0;
      case LK::Lbranch: return 1;
      case LK::Lcondbranch:
        switch (i.test.k) {
          case TK::Itruetest:
          case TK::Ifalsetest: return 1;
          case TK::Iinttest:
          case TK::Iinttest_imm:
          case TK::Ifloattest: return 2;
          case TK::Ioddtest:
          case TK::Ieventest: return 1;
        }
        return 0;
      case LK::Lcondbranch3: return 1 + (i.lbl0 ? 1 : 0) + (i.lbl1 ? 1 : 0) + (i.lbl2 ? 1 : 0);
      case LK::Lswitch: return 3 + static_cast<long>(i.lbls.size());
      case LK::Lentertrap: return fp ? 2 : 0;  // over-approximation
      case LK::Ladjust_trap_depth: return 0;
      case LK::Lpushtrap: return 3;
      case LK::Lpoptrap: return 1;
      case LK::Lraise: return i.raise == lambda::RaiseKind::Raise_notrace ? 3 : 1;
    }
    const mach::Operation& op = *i.op;
    switch (op.k) {
      case MK::Imove:
      case MK::Ispill:
      case MK::Ireload: return 1;
      case MK::Iconst_int: return num_instructions_for_intconst(op.n);
      case MK::Iconst_float: return op.n == 0 || is_immediate_float(op.n) ? 1 : 2;
      case MK::Iconst_symbol: return 2;
      case MK::Icall_ind: return 1;
      case MK::Icall_imm: return 1;
      case MK::Itailcall_ind: return epilogue_size(f);
      case MK::Itailcall_imm: return op.func == f.fun_name ? 1 : epilogue_size(f);
      case MK::Iextcall: return op.stack_ofs > 0 ? 5 : op.alloc ? 3 : 5;
      case MK::Istackoffset: return stack_adj_size(-op.n);
      case MK::Iload: {
        long based = op.addr.k == AK::Iindexed ? 0 : 1;
        long barrier = op.is_atomic ? 1 : 0;
        long single = op.chunk == Chunk::Single ? 2 : 1;
        return based + barrier + single;
      }
      case MK::Istore: {
        long based = op.addr.k == AK::Iindexed ? 0 : 1;
        long store = op.chunk == Chunk::Single ? 2 : 1;
        return based + store_pre_size(op) + store;
      }
      case MK::Ialloc: return f.fun_fast ? 5 : alloc_size(op.n);
      case MK::Ipoll: return op.return_label ? 4 : 3;
      case MK::Iintop:
        switch (op.intop.op) {
          case IO::Icomp: return 2;
          case IO::Icheckbound: return 2;
          case IO::Imod: return 2;
          case IO::Imulh: return 1;
          default: return 1;
        }
      case MK::Iintop_imm:
        switch (op.intop.op) {
          case IO::Icomp: return 2;
          case IO::Icheckbound: return 2;
          case IO::Iadd:
          case IO::Isub: return addsub_size(op.n);
          default: return 1;
        }
      case MK::Icompf: return 2;
      case MK::Ifloatofint:
      case MK::Iintoffloat:
      case MK::Iabsf:
      case MK::Inegf: return 1;
      case MK::Iaddf:
      case MK::Isubf:
      case MK::Imulf:
      case MK::Idivf: return 1;
      case MK::Iopaque: return 0;
      case MK::Idls_get: return 1;
      case MK::Ireturn_addr: return 1;
      case MK::Ispecific:
        switch (op.spec.k) {
          case SK::Ialloc_far: return f.fun_fast ? 6 : alloc_size(op.spec.bytes);
          case SK::Ipoll_far: return op.spec.return_label ? 5 : 4;
          case SK::Icheckbound_far: return 3;
          case SK::Icheckbound_imm_far: return 3;
          case SK::Ishiftcheckbound: return 2;
          case SK::Ishiftcheckbound_far: return 3;
          case SK::Isqrtf: return 1;
          case SK::Inegmulf: return 1;
          case SK::Imuladdf:
          case SK::Inegmuladdf:
          case SK::Imulsubf:
          case SK::Inegmulsubf: return 1;
          case SK::Ishiftarith: return 1;
          case SK::Imuladd:
          case SK::Imulsub: return 1;
          case SK::Ibswap: return op.spec.n == 16 ? 2 : 1;
          case SK::Imove32: return 1;
          case SK::Isignext: return 1;
        }
        return 0;
      default: fatal("Emit.Size.instr_size");
    }
  }

  static mach::Operation specific(SK k) {
    mach::Operation o{MK::Ispecific};
    o.spec.k = k;
    return o;
  }
  static mach::Operation relax_poll(const std::optional<long>& return_label) {
    mach::Operation o = specific(SK::Ipoll_far);
    o.spec.return_label = return_label;
    return o;
  }
  static mach::Operation relax_allocation(long num_bytes, const std::vector<mach::AllocDbginfo>& dbginfo) {
    mach::Operation o = specific(SK::Ialloc_far);
    o.spec.bytes = num_bytes;
    o.spec.dbginfo = dbginfo;
    return o;
  }
  static mach::Operation relax_intop_checkbound() { return specific(SK::Icheckbound_far); }
  static mach::Operation relax_intop_imm_checkbound(long bound) {
    mach::Operation o = specific(SK::Icheckbound_imm_far);
    o.spec.n = bound;
    return o;
  }
  static mach::Operation relax_specific_op(const arch::SpecificOperation& s) {
    if (s.k != SK::Ishiftcheckbound) fatal("Emit.Size.relax_specific_op");
    mach::Operation o = specific(SK::Ishiftcheckbound_far);
    o.spec.n = s.n;
    return o;
  }
};
using BR = branch_relaxation::Make<Size>;

// Output the assembly code for allocation.
void assembly_code_for_allocation(Env& env, const linear::Instruction& i, long n, bool far,
                                  const std::vector<mach::AllocDbginfo>& dbginfo) {
  long lbl_frame = record_frame_label(env, i.live, dbg_alloc(dbginfo));
  if (env.f->fun_fast) {
    long lbl_after_alloc = cmm::new_label();
    long lbl_call_gc = cmm::new_label();
    // n is at most Max_young_whsize * 8, i.e. currently 0x808, so it is
    // reasonable to assume n < 0x1_000.  This makes the generated code
    // simpler.
    if (!(16 <= n && n < 0x1000 && (n & 0x7) == 0)) fatal("Emit.assembly_code_for_allocation");
    long offset = domainstate::young_limit * 8;
    emit("\tldr\t", R(reg_tmp1()), ", [", R(reg_domain_state_ptr()), ", #", I(offset), "]\n");
    emit("\tsub\t", R(reg_alloc_ptr()), ", ", R(reg_alloc_ptr()), ", #", I(n), "\n");
    emit("\tcmp\t", R(reg_alloc_ptr()), ", ", R(reg_tmp1()), "\n");
    if (!far) {
      emit("\tb.lo\t", L(lbl_call_gc), "\n");
    } else {
      long lbl = cmm::new_label();
      emit("\tb.cs\t", L(lbl), "\n");
      emit("\tb\t", L(lbl_call_gc), "\n");
      emit(L(lbl), ":\n");
    }
    emit(L(lbl_after_alloc), ":");
    emit("\tadd\t", R(i.res[0]), ", ", R(reg_alloc_ptr()), ", #8\n");
    env.call_gc_sites.insert(env.call_gc_sites.begin(), GcCall{lbl_call_gc, lbl_after_alloc, lbl_frame});
  } else {
    switch (n) {
      case 16: emit("\tbl\t", emitaux::symbol("caml_alloc1"), "\n"); break;
      case 24: emit("\tbl\t", emitaux::symbol("caml_alloc2"), "\n"); break;
      case 32: emit("\tbl\t", emitaux::symbol("caml_alloc3"), "\n"); break;
      default:
        emit_intconst(reg_x8(), n);
        emit("\tbl\t", emitaux::symbol("caml_allocN"), "\n");
    }
    emit(L(lbl_frame), ":\tadd\t", R(i.res[0]), ", ", R(reg_alloc_ptr()), ", #8\n");
  }
}

void assembly_code_for_poll(Env& env, const linear::Instruction& i, bool far, const std::optional<long>& return_label) {
  long lbl_frame = record_frame_label(env, i.live, dbg_alloc({}));
  long lbl_call_gc = cmm::new_label();
  long lbl_after_poll = return_label ? *return_label : cmm::new_label();
  long offset = domainstate::young_limit * 8;
  emit("\tldr\t", R(reg_tmp1()), ", [", R(reg_domain_state_ptr()), ", #", I(offset), "]\n");
  emit("\tcmp\t", R(reg_alloc_ptr()), ", ", R(reg_tmp1()), "\n");
  if (!far) {
    if (!return_label) {
      emit("\tb.ls\t", L(lbl_call_gc), "\n");
      emit(L(lbl_after_poll), ":\n");
    } else {
      emit("\tb.hi\t", L(*return_label), "\n");
      emit("\tb\t", L(lbl_call_gc), "\n");
    }
  } else {
    if (!return_label) {
      emit("\tb.hi\t", L(lbl_after_poll), "\n");
      emit("\tb\t", L(lbl_call_gc), "\n");
      emit(L(lbl_after_poll), ":\n");
    } else {
      long lbl = cmm::new_label();
      emit("\tb.ls\t", L(lbl), "\n");
      emit("\tb\t", L(*return_label), "\n");
      emit(L(lbl), ":\tb\t", L(lbl_call_gc), "\n");
    }
  }
  env.call_gc_sites.insert(env.call_gc_sites.begin(), GcCall{lbl_call_gc, lbl_after_poll, lbl_frame});
}

void emit_named_text_section(std::string_view func_name) { emitaux::emit_named_text_section(func_name, '%'); }

// Emit code to load an emitted literal
void emit_load_literal(const Reg* dst, long lbl) {
  if (arch::macosx()) {
    emit("\tadrp\t", R(reg_tmp1()), ", ", L(lbl), "@PAGE\n");
    emit("\tldr\t", R(dst), ", [", R(reg_tmp1()), ", ", L(lbl), "@PAGEOFF]\n");
  } else {
    emit("\tadrp\t", R(reg_tmp1()), ", ", L(lbl), "\n");
    emit("\tldr\t", R(dst), ", [", R(reg_tmp1()), ", #:lo12:", L(lbl), "]\n");
  }
}

const char* name_for_float_comparison(lambda::FloatComparison c) {
  using C = lambda::FloatComparison;
  switch (c) {
    case C::CFeq: return "eq";
    case C::CFneq: return "ne";
    case C::CFlt: return "cc";
    case C::CFnlt: return "cs";
    case C::CFle: return "ls";
    case C::CFnle: return "hi";
    case C::CFgt: return "gt";
    case C::CFngt: return "le";
    case C::CFge: return "ge";
    case C::CFnge: return "lt";
  }
  return "";
}

// Output a release store [stlr] of register [src] at address [base + addr].
// Since [stlr] does not support addressing modes, we compute [base + addr]
// explicitly.
void emit_stlr(const Reg* src, const Reg* base, const arch::AddressingMode& addr) {
  if (!arch::macosx()) fatal("Emit.emit_stlr");
  // Ibased is not emitted under macOS
  if (addr.k != AK::Iindexed) fatal("Emit.emit_stlr");
  const Reg* dest_reg = base;
  if (addr.displ != 0) {
    emit_addimm(reg_tmp1(), base, addr.displ);
    dest_reg = reg_tmp1();
  }
  emit("\tstlr\t", R(src), ", [", R(dest_reg), "]\n");
}

bool same_loc(const Reg* a, const Reg* b) { return a->loc.k == b->loc.k && a->loc.n == b->loc.n; }

// Output the assembly code for an instruction
void emit_instr(Env& env, const linear::Instruction& i) {
  emit_debug_info(i.dbg);
  auto res0 = [&] { return R(i.res[0]); };
  auto arg = [&](std::size_t k) { return R(i.arg[k]); };
  switch (i.desc) {
    case LK::Lend: return;
    case LK::Lprologue: {
      long n = frame_size(env);
      if (env.f->fun_frame_required) {
        if (fp) {
          emit("\tstp\tx29, x30, [sp, #-16]\n");
          cfi_offset(29 /* frame pointer */, -16);
          cfi_offset(30 /* return address */, -8);
        } else {
          emit("\tstr\tx30, [sp, #-8]\n");
          cfi_offset(30 /* return address */, -8);
        }
      }
      if (n > 0) {
        emit_stack_adjustment(-n);
        if (env.f->fun_frame_required && fp) emit_addimm_gen("x29", "sp", n - 16);
      }
      return;
    }
    case LK::Lop: break;
    case LK::Lreloadretaddr: return;
    case LK::Lreturn:
      output_epilogue(env, [] {
        // mask the top 8 bits ourselves
        if (!arch::top_bits_ignore()) emit("\tand\tx30, x30, #0x00FFFFFFFFFFFFFF\n");
        emit("\tret\n");
      });
      return;
    case LK::Llabel: emit(L(i.lbl), ":\n"); return;
    case LK::Lbranch: emit("\tb\t", L(i.lbl), "\n"); return;
    case LK::Lcondbranch: {
      using TK = mach::Test::K;
      switch (i.test.k) {
        case TK::Itruetest: emit("\tcbnz\t", arg(0), ", ", L(i.lbl), "\n"); return;
        case TK::Ifalsetest: emit("\tcbz\t", arg(0), ", ", L(i.lbl), "\n"); return;
        case TK::Iinttest:
          emit("\tcmp\t", arg(0), ", ", arg(1), "\n");
          emit("\tb.", name_for_comparison(i.test.icmp), "\t", L(i.lbl), "\n");
          return;
        case TK::Iinttest_imm:
          emit_cmpimm(i.arg[0], i.test.n);
          emit("\tb.", name_for_comparison(i.test.icmp), "\t", L(i.lbl), "\n");
          return;
        case TK::Ifloattest:
          emit("\tfcmp\t", arg(0), ", ", arg(1), "\n");
          emit("\tb.", name_for_float_comparison(i.test.fcmp), "\t", L(i.lbl), "\n");
          return;
        case TK::Ioddtest: emit("\ttbnz\t", arg(0), ", #0, ", L(i.lbl), "\n"); return;
        case TK::Ieventest: emit("\ttbz\t", arg(0), ", #0, ", L(i.lbl), "\n"); return;
      }
      return;
    }
    case LK::Lcondbranch3:
      emit("\tcmp\t", arg(0), ", #1\n");
      if (i.lbl0) emit("\tb.lt\t", L(*i.lbl0), "\n");
      if (i.lbl1) emit("\tb.eq\t", L(*i.lbl1), "\n");
      if (i.lbl2) emit("\tb.gt\t", L(*i.lbl2), "\n");
      return;
    case LK::Lswitch: {
      long lbltbl = cmm::new_label();
      emit("\tadr\t", R(reg_tmp1()), ", ", L(lbltbl), "\n");
      emit("\tadd\t", R(reg_tmp1()), ", ", R(reg_tmp1()), ", ", arg(0), ", lsl #2\n");
      emit("\tbr\t", R(reg_tmp1()), "\n");
      emit(L(lbltbl), ":");
      for (long l : i.lbls) emit("\tb\t", L(l), "\n");
      return;
    }
    case LK::Lentertrap:
      if (fp) {
        long delta = frame_size(env) - 16;  // return address + frame pointer
        emit_addimm_gen("x29", "sp", delta);
      }
      return;
    case LK::Ladjust_trap_depth: {
      // each trap occupies 16 bytes on the stack
      long delta = 16 * i.delta_traps;
      cfi_adjust_cfa_offset(delta);
      env.stack_offset += delta;
      return;
    }
    case LK::Lpushtrap:
      emit("\tadr\t", R(reg_tmp1()), ", ", L(i.lbl), "\n");
      env.stack_offset += 16;
      emit("\tstp\t", R(reg_trap_ptr()), ", ", R(reg_tmp1()), ", [sp, -16]!\n");
      cfi_adjust_cfa_offset(16);
      emit("\tmov\t", R(reg_trap_ptr()), ", sp\n");
      return;
    case LK::Lpoptrap:
      emit("\tldr\t", R(reg_trap_ptr()), ", [sp], 16\n");
      cfi_adjust_cfa_offset(-16);
      env.stack_offset -= 16;
      return;
    case LK::Lraise:
      switch (i.raise) {
        case lambda::RaiseKind::Raise_regular:
          emit("\tbl\t", emitaux::symbol("caml_raise_exn"), "\n");
          record_frame(env, reg::Set{}, dbg_raise(i.dbg));
          emit("\n");
          return;
        case lambda::RaiseKind::Raise_reraise:
          emit("\tbl\t", emitaux::symbol("caml_reraise_exn"), "\n");
          record_frame(env, reg::Set{}, dbg_raise(i.dbg));
          emit("\n");
          return;
        case lambda::RaiseKind::Raise_notrace:
          emit("\tmov\tsp, ", R(reg_trap_ptr()), "\n");
          emit("\tldp\t", R(reg_trap_ptr()), ", ", R(reg_tmp1()), ", [sp], 16\n");
          emit("\tbr\t", R(reg_tmp1()), "\n");
          return;
      }
      return;
  }
  const mach::Operation& op = *i.op;
  switch (op.k) {
    case MK::Imove:
    case MK::Ispill:
    case MK::Ireload: {
      const Reg* src = i.arg[0];
      const Reg* dst = i.res[0];
      if (same_loc(src, dst)) return;
      if (src->loc.k == LocK::Reg && src->typ == cmm::MachtypeComponent::Float && dst->loc.k == LocK::Reg)
        emit("\tfmov\t", R(dst), ", ", R(src), "\n");
      else if (src->loc.k == LocK::Reg && dst->loc.k == LocK::Reg) emit("\tmov\t", R(dst), ", ", R(src), "\n");
      else if (src->loc.k == LocK::Reg && is_stack(dst)) emit("\tstr\t", R(src), ", ", emit_stack(env, dst), "\n");
      else if (is_stack(src) && dst->loc.k == LocK::Reg) emit("\tldr\t", R(dst), ", ", emit_stack(env, src), "\n");
      else fatal("Emit.emit_instr: move");
      return;
    }
    case MK::Iconst_int: emit_intconst(i.res[0], op.n); return;
    case MK::Iconst_float: {
      std::int64_t f = op.n;
      if (f == 0) {
        emit("\tfmov\t", res0(), ", xzr\n");
      } else if (is_immediate_float(f)) {
        double d;
        std::memcpy(&d, &f, sizeof d);
        char b[64];
        std::snprintf(b, sizeof b, "%.7f", d);
        emit("\tfmov\t", res0(), ", #", b, "\n");
      } else {
        long lbl = float_literal(env, f);
        emit_load_literal(i.res[0], lbl);
      }
      return;
    }
    case MK::Iconst_symbol: emit_load_symbol_addr(i.res[0], op.func); return;
    case MK::Icall_ind:
      emit("\tblr\t", arg(0), "\n");
      record_frame(env, i.live, dbg_other(i.dbg));
      emit("\n");
      return;
    case MK::Icall_imm:
      emit("\tbl\t", emitaux::symbol(op.func), "\n");
      record_frame(env, i.live, dbg_other(i.dbg));
      emit("\n");
      return;
    case MK::Itailcall_ind: output_epilogue(env, [&] { emit("\tbr\t", arg(0), "\n"); }); return;
    case MK::Itailcall_imm:
      if (op.func == env.f->fun_name) emit("\tb\t", L(env.f->fun_tailrec_entry_point_label), "\n");
      else output_epilogue(env, [&] { emit("\tb\t", emitaux::symbol(op.func), "\n"); });
      return;
    case MK::Iextcall:
      if (op.stack_ofs > 0) {
        emit("\tmov\t", R(reg_stack_arg_begin()), ", sp\n");
        emit("\tadd\t", R(reg_stack_arg_end()), ", sp, #", I(align(op.stack_ofs, 16)), "\n");
        emit_load_symbol_addr(reg_x8(), op.func);
        emit("\tbl\t", emitaux::symbol("caml_c_call_stack_args"), "\n");
        record_frame(env, i.live, dbg_other(i.dbg));
        emit("\n");
      } else if (op.alloc) {
        emit_load_symbol_addr(reg_x8(), op.func);
        emit("\tbl\t", emitaux::symbol("caml_c_call"), "\n");
        record_frame(env, i.live, dbg_other(i.dbg));
        emit("\n");
      } else {
        // Store OCaml stack in x19 register and restore later.
        emit("\tmov\tx19, sp\n");
        cfi_remember_state();
        cfi_def_cfa_register(19);
        long offset = domainstate::c_stack * 8;
        emit("\tldr\t", R(reg_tmp1()), ", [", R(reg_domain_state_ptr()), ", ", I(offset), "]\n");
        emit("\tmov\tsp, ", R(reg_tmp1()), "\n");
        emit("\tbl\t", emitaux::symbol(op.func), "\n");
        emit("\tmov\tsp, x19\n");
        cfi_restore_state();
      }
      return;
    case MK::Istackoffset:
      if (op.n % 16 != 0) fatal("Emit.emit_instr: Istackoffset");
      emit_stack_adjustment(-op.n);
      env.stack_offset += op.n;
      return;
    case MK::Iload: {
      if (!(op.chunk == Chunk::Word_int || op.chunk == Chunk::Word_val || !op.is_atomic)) fatal("Emit: atomic load");
      const Reg* dst = i.res[0];
      const Reg* base = i.arg.empty() ? nullptr : i.arg[0];
      if (op.addr.k == AK::Ibased) {
        if (clflags::dlcode) fatal("Emit: Ibased");  // see selection.ml
        emit("\tadrp\t", R(reg_tmp1()), ", ", emit_symbol_offset(op.addr.sym, op.addr.displ), "\n");
        base = reg_tmp1();
      }
      switch (op.chunk) {
        case Chunk::Byte_unsigned: emit("\tldrb\t", W(dst), ", ", emit_addressing(op.addr, base), "\n"); return;
        case Chunk::Byte_signed: emit("\tldrsb\t", R(dst), ", ", emit_addressing(op.addr, base), "\n"); return;
        case Chunk::Sixteen_unsigned: emit("\tldrh\t", W(dst), ", ", emit_addressing(op.addr, base), "\n"); return;
        case Chunk::Sixteen_signed: emit("\tldrsh\t", R(dst), ", ", emit_addressing(op.addr, base), "\n"); return;
        case Chunk::Thirtytwo_unsigned: emit("\tldr\t", W(dst), ", ", emit_addressing(op.addr, base), "\n"); return;
        case Chunk::Thirtytwo_signed: emit("\tldrsw\t", R(dst), ", ", emit_addressing(op.addr, base), "\n"); return;
        case Chunk::Single:
          emit("\tldr\ts7, ", emit_addressing(op.addr, base), "\n");
          emit("\tfcvt\t", R(dst), ", s7\n");
          return;
        case Chunk::Sixtyfour:
        case Chunk::Word_int:
        case Chunk::Word_val:
          if (op.is_atomic) {
            if (!(op.addr.k == AK::Iindexed && op.addr.displ == 0) || op.chunk == Chunk::Sixtyfour)
              fatal("Emit: atomic load addressing");
            emit("\tdmb\tishld\n");
            emit("\tldar\t", R(dst), ", [", arg(0), "]\n");
          } else {
            emit("\tldr\t", R(dst), ", ", emit_addressing(op.addr, base), "\n");
          }
          return;
        case Chunk::Double: emit("\tldr\t", R(dst), ", ", emit_addressing(op.addr, base), "\n"); return;
      }
      return;
    }
    case MK::Istore: {
      // NB: assignments other than Word_int and Word_val do not follow the
      // Multicore OCaml memory model and so do not emit a barrier
      const Reg* src = i.arg[0];
      const Reg* base = i.arg.size() > 1 ? i.arg[1] : nullptr;
      if (op.addr.k == AK::Ibased) {
        if (clflags::dlcode) fatal("Emit: Ibased");
        emit("\tadrp\t", R(reg_tmp1()), ", ", emit_symbol_offset(op.addr.sym, op.addr.displ), "\n");
        base = reg_tmp1();
      }
      switch (op.chunk) {
        case Chunk::Byte_unsigned:
        case Chunk::Byte_signed: emit("\tstrb\t", W(src), ", ", emit_addressing(op.addr, base), "\n"); return;
        case Chunk::Sixteen_unsigned:
        case Chunk::Sixteen_signed: emit("\tstrh\t", W(src), ", ", emit_addressing(op.addr, base), "\n"); return;
        case Chunk::Thirtytwo_unsigned:
        case Chunk::Thirtytwo_signed: emit("\tstr\t", W(src), ", ", emit_addressing(op.addr, base), "\n"); return;
        case Chunk::Single:
          emit("\tfcvt\ts7, ", R(src), "\n");
          emit("\tstr\ts7, ", emit_addressing(op.addr, base), "\n");
          return;
        case Chunk::Sixtyfour: emit("\tstr\t", R(src), ", ", emit_addressing(op.addr, base), "\n"); return;
        case Chunk::Word_int:
        case Chunk::Word_val:
          if (op.is_assign && arch::macosx()) {
            // Release store for assignments on macOS. See
            // https://github.com/ocaml/ocaml/issues/13262.
            emit_stlr(src, base, op.addr);
          } else if (op.is_assign) {
            // Memory model barrier for assignments.
            emit("\tdmb\tishld\n");
            emit("\tstr\t", R(src), ", ", emit_addressing(op.addr, base), "\n");
          } else {
            // Initializing store
            emit("\tstr\t", R(src), ", ", emit_addressing(op.addr, base), "\n");
          }
          return;
        case Chunk::Double: emit("\tstr\t", R(src), ", ", emit_addressing(op.addr, base), "\n"); return;
      }
      return;
    }
    case MK::Ialloc: assembly_code_for_allocation(env, i, op.n, false, op.dbginfo); return;
    case MK::Ipoll: assembly_code_for_poll(env, i, false, op.return_label); return;
    case MK::Iintop:
      switch (op.intop.op) {
        case IO::Icomp:
          emit("\tcmp\t", arg(0), ", ", arg(1), "\n");
          emit("\tcset\t", res0(), ", ", name_for_comparison(op.intop.cmp), "\n");
          return;
        case IO::Icheckbound: {
          long lbl = bound_error_label(env, i.dbg);
          emit("\tcmp\t", arg(0), ", ", arg(1), "\n");
          emit("\tb.ls\t", L(lbl), "\n");
          return;
        }
        case IO::Imod:
          emit("\tsdiv\t", R(reg_tmp1()), ", ", arg(0), ", ", arg(1), "\n");
          emit("\tmsub\t", res0(), ", ", R(reg_tmp1()), ", ", arg(1), ", ", arg(0), "\n");
          return;
        case IO::Imulh: emit("\tsmulh\t", res0(), ", ", arg(0), ", ", arg(1), "\n"); return;
        default:
          emit("\t", name_for_int_operation(op.intop.op), "\t", res0(), ", ", arg(0), ", ", arg(1), "\n");
          return;
      }
    case MK::Iintop_imm:
      switch (op.intop.op) {
        case IO::Iadd: emit_addimm(i.res[0], i.arg[0], op.n); return;
        case IO::Isub: emit_subimm(i.res[0], i.arg[0], op.n); return;
        case IO::Icomp:
          emit_cmpimm(i.arg[0], op.n);
          emit("\tcset\t", res0(), ", ", name_for_comparison(op.intop.cmp), "\n");
          return;
        case IO::Icheckbound: {
          long lbl = bound_error_label(env, i.dbg);
          emit_cmpimm(i.arg[0], op.n);
          emit("\tb.ls\t", L(lbl), "\n");
          return;
        }
        default:
          emit("\t", name_for_int_operation(op.intop.op), "\t", res0(), ", ", arg(0), ", #", I(op.n), "\n");
          return;
      }
    case MK::Icompf:
      emit("\tfcmp\t", arg(0), ", ", arg(1), "\n");
      emit("\tcset\t", res0(), ", ", name_for_float_comparison(op.fcmp), "\n");
      return;
    case MK::Ifloatofint: emit("\tscvtf\t", res0(), ", ", arg(0), "\n"); return;
    case MK::Iintoffloat: emit("\tfcvtzs\t", res0(), ", ", arg(0), "\n"); return;
    case MK::Iabsf: emit("\tfabs\t", res0(), ", ", arg(0), "\n"); return;
    case MK::Inegf: emit("\tfneg\t", res0(), ", ", arg(0), "\n"); return;
    case MK::Iaddf: emit("\tfadd\t", res0(), ", ", arg(0), ", ", arg(1), "\n"); return;
    case MK::Isubf: emit("\tfsub\t", res0(), ", ", arg(0), ", ", arg(1), "\n"); return;
    case MK::Imulf: emit("\tfmul\t", res0(), ", ", arg(0), ", ", arg(1), "\n"); return;
    case MK::Idivf: emit("\tfdiv\t", res0(), ", ", arg(0), ", ", arg(1), "\n"); return;
    case MK::Iopaque:
      if (!same_loc(i.arg[0], i.res[0])) fatal("Emit: Iopaque");
      return;
    case MK::Idls_get: {
      long offset = domainstate::dls_root * 8;
      emit("\tldr\t", res0(), ", [", R(reg_domain_state_ptr()), ", ", I(offset), "]\n");
      return;
    }
    case MK::Ireturn_addr: {
      long n = frame_size(env);
      if (env.f->fun_frame_required) emit("\tldr\t", res0(), ", [sp, #", I(n - 8), "]\n");
      else emit("\tmov\t", res0(), ", x30\n");
      return;
    }
    case MK::Ispecific: break;
    default: fatal("Emit.emit_instr: unexpected operation");
  }
  const arch::SpecificOperation& s = op.spec;
  switch (s.k) {
    case SK::Imove32: {
      const Reg* src = i.arg[0];
      const Reg* dst = i.res[0];
      if (same_loc(src, dst)) return;
      if (src->loc.k == LocK::Reg && dst->loc.k == LocK::Reg) emit("\tmov\t", W(dst), ", ", W(src), "\n");
      else if (src->loc.k == LocK::Reg && is_stack(dst)) emit("\tstr\t", W(src), ", ", emit_stack(env, dst), "\n");
      else if (is_stack(src) && dst->loc.k == LocK::Reg) emit("\tldr\t", W(dst), ", ", emit_stack(env, src), "\n");
      else fatal("Emit.emit_instr: move32");
      return;
    }
    case SK::Ialloc_far: assembly_code_for_allocation(env, i, s.bytes, true, s.dbginfo); return;
    case SK::Ipoll_far: assembly_code_for_poll(env, i, true, s.return_label); return;
    case SK::Icheckbound_far: {
      long lbl = bound_error_label(env, i.dbg);
      long lbl2 = cmm::new_label();
      emit("\tcmp\t", arg(0), ", ", arg(1), "\n");
      emit("\tb.hi\t", L(lbl2), "\n");
      emit("\tb\t", L(lbl), "\n");
      emit(L(lbl2), ":\n");
      return;
    }
    case SK::Icheckbound_imm_far: {
      long lbl = bound_error_label(env, i.dbg);
      long lbl2 = cmm::new_label();
      emit("\tcmp\t", arg(0), ", #", I(s.n), "\n");
      emit("\tb.hi\t", L(lbl2), "\n");
      emit("\tb\t", L(lbl), "\n");
      emit(L(lbl2), ":\n");
      return;
    }
    case SK::Ishiftcheckbound: {
      long lbl = bound_error_label(env, i.dbg);
      emit("\tcmp\t", arg(1), ", ", arg(0), ", lsr #", I(s.n), "\n");
      emit("\tb.cs\t", L(lbl), "\n");
      return;
    }
    case SK::Ishiftcheckbound_far: {
      long lbl = bound_error_label(env, i.dbg);
      long lbl2 = cmm::new_label();
      emit("\tcmp\t", arg(1), ", ", arg(0), ", lsr #", I(s.n), "\n");
      emit("\tb.lo\t", L(lbl2), "\n");
      emit("\tb\t", L(lbl), "\n");
      emit(L(lbl2), ":\n");
      return;
    }
    case SK::Isqrtf: emit("\tfsqrt\t", res0(), ", ", arg(0), "\n"); return;
    case SK::Inegmulf: emit("\tfnmul\t", res0(), ", ", arg(0), ", ", arg(1), "\n"); return;
    case SK::Imuladdf:
    case SK::Inegmuladdf:
    case SK::Imulsubf:
    case SK::Inegmulsubf: {
      const char* instr = s.k == SK::Imuladdf      ? "fmadd"
                          : s.k == SK::Inegmuladdf ? "fnmadd"
                          : s.k == SK::Imulsubf    ? "fmsub"
                                                   : "fnmsub";
      emit("\t", instr, "\t", res0(), ", ", arg(1), ", ", arg(2), ", ", arg(0), "\n");
      return;
    }
    case SK::Ishiftarith:
      emit("\t", s.arith == arch::ArithOperation::Ishiftadd ? "add" : "sub", "\t", res0(), ", ", arg(0), ", ",
           arg(1));
      if (s.n >= 0) emit(", lsl #", I(s.n), "\n");
      else emit(", asr #", I(-s.n), "\n");
      return;
    case SK::Imuladd:
    case SK::Imulsub:
      emit("\t", s.k == SK::Imuladd ? "madd" : "msub", "\t", res0(), ", ", arg(0), ", ", arg(1), ", ", arg(2), "\n");
      return;
    case SK::Ibswap:
      switch (s.n) {
        case 16:
          emit("\trev16\t", W(i.res[0]), ", ", W(i.arg[0]), "\n");
          emit("\tubfm\t", res0(), ", ", res0(), ", #0, #15\n");
          return;
        case 32: emit("\trev\t", W(i.res[0]), ", ", W(i.arg[0]), "\n"); return;
        case 64: emit("\trev\t", res0(), ", ", arg(0), "\n"); return;
        default: fatal("Emit: Ibswap");
      }
    case SK::Isignext: emit("\tsbfm\t", res0(), ", ", arg(0), ", #0, #", I(s.n - 1), "\n"); return;
  }
}

// Emission of an instruction sequence (for debugging instr_size errors)
void emit_instr_debug(Env& env, const linear::Instruction& i) {
  long lbl = cmm::new_label();
  emit(L(lbl), ":\n");
  emit_instr(env, i);
  long sz = Size::instr_size(*env.f, i) * 4;
  emit("\t.ifgt (. - ", L(lbl), ") - ", I(sz), "\n");
  emit("\t.error \"Emit.instr_size: instruction length mismatch\"\n");
  emit("\t.endif\n");
}

void emit_all(Env& env, Instr body) {
  long lbl_start = cmm::new_label();
  emit(L(lbl_start), ":\n");
  constexpr bool debug = config::with_codegen_invariants;
  long acc = 0;  // in units of 32-bit instructions
  for (Instr i = body;; i = i->next) {
    if (i->desc == LK::Lend) {
      if (debug) {
        emit("\t.ifgt (. - ", L(lbl_start), ") - ", I(acc * 4), "\n");
        emit("\t.error \"Emit.instr_size: instruction length mismatch\"\n");
        emit("\t.endif\n");
      }
      return;
    }
    if (debug) emit_instr_debug(env, *i);
    else emit_instr(env, *i);
    acc += Size::instr_size(*env.f, *i);
  }
}

// Emission of data
long log2(long n) {
  long r = 0;
  while ((1L << (r + 1)) <= n) ++r;
  return r;
}

void emit_item(const cmm::DataItem& d) {
  using DK = cmm::DataItem::K;
  switch (d.kind) {
    case DK::Cglobal_symbol: emit("\t.globl\t", emitaux::symbol(d.s), "\n"); return;
    case DK::Cdefine_symbol:
      // GOT relocations against non-global symbols don't seem to work
      // properly: force all symbols to be global.
      if (clflags::dlcode) emit("\t.globl\t", emitaux::symbol(d.s), "\n");
      emit(emitaux::symbol(d.s), ":\n");
      return;
    case DK::Cint8: emit("\t.byte\t", I(d.n), "\n"); return;
    case DK::Cint16: emit("\t.short\t", I(d.n), "\n"); return;
    case DK::Cint32: emit("\t.long\t", I(d.n), "\n"); return;
    case DK::Cint: emit("\t.quad\t", I(d.n), "\n"); return;
    case DK::Csingle: {
      float f = static_cast<float>(d.f);
      std::int32_t bits;
      std::memcpy(&bits, &f, sizeof bits);
      emit_float32_directive(".long", bits);
      return;
    }
    case DK::Cdouble: {
      std::int64_t bits;
      std::memcpy(&bits, &d.f, sizeof bits);
      emit_float64_directive(".quad", bits);
      return;
    }
    case DK::Csymbol_address: emit("\t.quad\t", emitaux::symbol(d.s), "\n"); return;
    case DK::Cstring: emit_string_directive("\t.ascii  ", d.s); return;
    case DK::Cskip:
      if (d.n > 0) emit("\t.space\t", I(d.n), "\n");
      return;
    case DK::Calign: emit("\t.align\t", I(log2(d.n)), "\n"); return;
  }
}

}  // namespace

// Emission of a function declaration
void fundecl(const linear::Fundecl& f) {
  Env env = mk_env(f);
  emit_named_text_section(f.fun_name);
  emit("\t.align\t3\n");
  emit("\t.globl\t", emitaux::symbol(f.fun_name), "\n");
  emit_type_directive(f.fun_name, "%function");
  // Dynamic stack checking
  long stack_threshold_size = stack_threshold * 8;  // bytes
  long max_frame_size = frame_size(env) + f.fun_extra_stack_used;
  std::optional<long> handle_overflow;
  long stack_check_size = 0;
  if (f.fun_contains_nontail_calls || max_frame_size >= stack_threshold_size) {
    long overflow = cmm::new_label();
    emit(L(overflow), ":\n");
    // Pass the desired frame size on the stack, since all of the
    // argument-passing registers may be in use.
    long s = stack_threshold + max_frame_size / 8;
    emit("\tmov\t", R(reg_tmp1()), ", #", I(s), "\n");
    emit("\tstp\t", R(reg_tmp1()), ", x30, [sp, #-16]!\n");
    emit("\tbl\t", emitaux::symbol("caml_call_realloc_stack"), "\n");
    emit("\tldp\t", R(reg_tmp1()), ", x30, [sp], #16\n");
    // fall through function entry point
    handle_overflow = overflow;
    stack_check_size = 5;
  }
  emit(emitaux::symbol(f.fun_name), ":\n");
  emit_debug_info(f.fun_dbg);
  cfi_startproc();
  if (handle_overflow) {
    long threshold_offset = domainstate::stack_ctx_words * 8 + stack_threshold_size;
    long fs = max_frame_size + threshold_offset;
    long offset = domainstate::current_stack * 8;
    emit("\tldr\t", R(reg_tmp1()), ", [", R(reg_domain_state_ptr()), ", #", I(offset), "]\n");
    emit_addimm(reg_tmp1(), reg_tmp1(), fs);
    emit("\tcmp\tsp, ", R(reg_tmp1()), "\n");
    emit("\tbcc\t", L(*handle_overflow), "\n");
  }
  auto [num_call_gc, num_check_bound] = num_call_gc_and_check_bound_points(env);
  long max_ool = stack_check_size + max_out_of_line_code_offset(num_call_gc, num_check_bound);
  BR::relax(f, max_ool);
  emit_all(env, f.fun_body);
  for (const GcCall& gc : env.call_gc_sites) emit_call_gc(gc);
  for (const BoundErrorCall& bd : env.bound_error_sites) emit_call_bound_error(bd);
  if (static_cast<long>(env.call_gc_sites.size()) != num_call_gc ||
      static_cast<long>(env.bound_error_sites.size()) != num_check_bound)
    fatal("Emit.fundecl: out-of-line code count");
  cfi_endproc();
  emit_type_directive(f.fun_name, "%function");
  emit_size_directive(f.fun_name);
  emit_literals(env);
}

void data(const std::vector<cmm::DataItem>& l) {
  emit("\t.data\n");
  emit("\t.align\t3\n");
  for (const cmm::DataItem& d : l) emit_item(d);
}

// Beginning / end of an assembly file
void begin_assembly() {
  output.clear();
  reset_debug_info();
  emit("\t.file\t\"\"\n");  // PR#7037
  std::string_view lbl_begin = compilenv::make_symbol(std::string_view("data_begin"));
  emit("\t.data\n");
  emit("\t.globl\t", emitaux::symbol(lbl_begin), "\n");
  emit(emitaux::symbol(lbl_begin), ":\n");
  lbl_begin = compilenv::make_symbol(std::string_view("code_begin"));
  emit_named_text_section(lbl_begin);
  emit("\t.globl\t", emitaux::symbol(lbl_begin), "\n");
  emit(emitaux::symbol(lbl_begin), ":\n");
  // we need to pad here to avoid collision for the unwind test between the
  // code_begin symbol and the first function.  (See also #4690) Alignment is
  // needed to avoid linker warnings for shared_startup__code_{begin,end}.
  if (arch::macosx()) {
    emit("\tnop\n");
    emit("\t.align\t3\n");
  }
}

std::string end_assembly() {
  std::string_view lbl_end = compilenv::make_symbol(std::string_view("code_end"));
  emit_named_text_section(lbl_end);
  emit("\t.globl\t", emitaux::symbol(lbl_end), "\n");
  emit(emitaux::symbol(lbl_end), ":\n");
  lbl_end = compilenv::make_symbol(std::string_view("data_end"));
  emit("\t.data\n");
  emit("\t.quad\t0\n");  // PR#6329
  emit("\t.globl\t", emitaux::symbol(lbl_end), "\n");
  emit(emitaux::symbol(lbl_end), ":\n");
  emit("\t.quad\t0\n");
  emit("\t.align\t3\n");  // #7887
  std::string_view lbl = compilenv::make_symbol(std::string_view("frametable"));
  emit("\t.globl\t", emitaux::symbol(lbl), "\n");
  emit(emitaux::symbol(lbl), ":\n");
  emit_frames(EmitFrameActions{
      [](long l) {
        emit_label_type(l, "%function");
        emit("\t.quad\t", L(l), "\n");
      },
      [](long l) {
        emit_label_type(l, "%object");
        emit("\t.quad\t", L(l), "\n");
      },
      [](long n) { emit("\t.byte\t", I(n), "\n"); },
      [](long n) { emit("\t.short\t", I(n), "\n"); },
      [](std::int32_t n) {
        emit("\t.long\t");
        emit_int32(n);
        emit("\n");
      },
      [](long n) { emit("\t.quad\t", I(n), "\n"); },
      [](long n) { emit("\t.align\t", I(log2(n)), "\n"); },
      [](long l, std::int32_t ofs) {
        emit("\t.long\t", L(l), " - . + ");
        emit_int32(ofs);
        emit("\n");
      },
      [](long l) { emit(L(l), ":\n"); },
      [](const std::string& s) { emit_string_directive("\t.asciz\t", s); }});
  emit_type_directive(lbl, "%object");
  emit_size_directive(lbl);
  emit_nonexecstack_note();
  std::string r = std::move(output);
  output.clear();
  return r;
}

}  // namespace cppcaml::typing::emit
