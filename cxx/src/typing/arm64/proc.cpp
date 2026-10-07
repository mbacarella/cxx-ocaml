// Port of asmcomp/arm64/proc.ml: the registers and calling conventions of
// the ARM processor in 64-bit mode.  See proc in mach.hpp.
//
// Integer register map:
//   x0 - x15    general purpose (caller-save)
//   x16, x17    temporaries (used by call veeners)
//   x18         platform register (reserved)
//   x19 - x25   general purpose (callee-save)
//   x26         trap pointer
//   x27         alloc pointer
//   x28         domain state pointer
//   x29         frame pointer
//   x30         return address
//   sp / xzr    stack pointer / zero register
// Floating-point register map:
//   d0 - d7     general purpose (caller-save)
//   d8 - d15    general purpose (callee-save)
//   d16 - d31   general purpose (caller-save)
#include "cppcaml/typing/mach.hpp"

#include <stdexcept>

namespace cppcaml::typing::proc {

const bool rotate_registers = true;
const long max_arguments_for_tailcalls = 16 /* in regs */ + 64 /* in domain state */;

using reg::Location;
using MC = cmm::MachtypeComponent;
using cmm::Exttype;

namespace {
const char* const int_reg_name[] = {"x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  // 0 - 7
                                    "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15",  // 8 - 15
                                    "x19", "x20", "x21", "x22", "x23", "x24", "x25",         // 16 - 22
                                    "x26", "x27", "x28",                                     // 23 - 25
                                    "x16", "x17"};                                           // 26 - 27
const char* const float_reg_name[] = {"d0",  "d1",  "d2",  "d3",  "d4",  "d5",  "d6",  "d7",
                                      "d8",  "d9",  "d10", "d11", "d12", "d13", "d14", "d15",
                                      "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
                                      "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31"};
std::vector<Reg*> hard_int_reg, hard_float_reg;
constexpr long size_int = 8, size_float = 8;
constexpr long size_domainstate_args = 64 * size_int;

Reg* stack_slot(Location slot, MC ty) { return reg::at_location(ty, slot); }

long align(long n, long a) { return n >= 0 ? (n + a - 1) & -a : n & -a; }

// Calling conventions
using MakeStack = Location (*)(long);
Location incoming(long ofs) {
  if (ofs >= 0) return {Location::K::Incoming, ofs};
  return {Location::K::Domainstate, ofs + size_domainstate_args};
}
Location outgoing(long ofs) {
  if (ofs >= 0) return {Location::K::Outgoing, ofs};
  return {Location::K::Domainstate, ofs + size_domainstate_args};
}
Location not_supported(long) { throw std::runtime_error("Proc.loc_results: cannot call"); }

Reg* loc_int(long last_int, MakeStack make_stack, long& int_, long& ofs) {
  if (int_ <= last_int) return phys_reg(int_++);
  ofs = align(ofs, size_int);
  Reg* l = stack_slot(make_stack(ofs), MC::Int);
  ofs += size_int;
  return l;
}
Reg* loc_float(long last_float, MakeStack make_stack, long& float_, long& ofs) {
  if (float_ <= last_float) return phys_reg(float_++);
  ofs = align(ofs, size_float);
  Reg* l = stack_slot(make_stack(ofs), MC::Float);
  ofs += size_float;
  return l;
}
// macOS/iOS peculiarity: int32 arguments passed on stack occupy 4 bytes,
// while the AAPCS64 says 8 bytes
Reg* loc_int32(long last_int, MakeStack make_stack, long& int_, long& ofs) {
  if (int_ <= last_int) return phys_reg(int_++);
  Reg* l = stack_slot(make_stack(ofs), MC::Int);
  ofs += arch::macosx() ? 4 : 8;
  return l;
}

std::pair<Regs, long> calling_conventions(long first_int, long last_int, long first_float, long last_float,
                                          MakeStack make_stack, long first_stack, const std::vector<MC>& arg) {
  Regs loc(arg.size(), nullptr);
  long int_ = first_int, float_ = first_float, ofs = first_stack;
  for (std::size_t i = 0; i < arg.size(); ++i) {
    if (arg[i] != MC::Float) loc[i] = loc_int(last_int, make_stack, int_, ofs);
    else loc[i] = loc_float(last_float, make_stack, float_, ofs);
  }
  return {loc, align(std::max(0L, ofs), 16)};  // keep stack 16-aligned
}

// OCaml calling convention: first integer args in r0...r15 (r7 on macOS),
// first float args in d0...d15, remaining args in domain state area, then
// on stack.  Return values in r0...r15 or d0...d15.
long last_int_register() { return arch::macosx() ? 7 : 15; }

// C calling convention: first integer args in r0...r7, first float args in
// d0...d7, remaining args on stack.  Return values in r0...r1 or d0.
std::pair<std::vector<Regs>, long> external_calling_conventions(long first_int, long last_int, long first_float,
                                                                long last_float, MakeStack make_stack,
                                                                const std::vector<Exttype>& ty_args) {
  std::vector<Regs> loc(ty_args.size());
  long int_ = first_int, float_ = first_float, ofs = 0;
  for (std::size_t i = 0; i < ty_args.size(); ++i) {
    switch (ty_args[i]) {
      case Exttype::XInt:
      case Exttype::XInt64: loc[i] = {loc_int(last_int, make_stack, int_, ofs)}; break;
      case Exttype::XInt32: loc[i] = {loc_int32(last_int, make_stack, int_, ofs)}; break;
      case Exttype::XFloat: loc[i] = {loc_float(last_float, make_stack, float_, ofs)}; break;
    }
  }
  return {loc, align(ofs, 16)};  // keep stack 16-aligned
}
}  // namespace

// Representation of hard registers by pseudo-registers
void init_hard_regs() {
  if (!hard_int_reg.empty()) return;
  for (long i = 0; i <= 27; ++i) hard_int_reg.push_back(reg::at_location(MC::Int, {Location::K::Reg, i}));
  for (long i = 0; i <= 31; ++i) hard_float_reg.push_back(reg::at_location(MC::Float, {Location::K::Reg, 100 + i}));
}

std::string_view register_name(long r) { return r < 100 ? int_reg_name[r] : float_reg_name[r - 100]; }

Reg* phys_reg(long n) {
  init_hard_regs();
  return n < 100 ? hard_int_reg[n] : hard_float_reg[n - 100];
}

std::pair<Regs, long> loc_arguments(const std::vector<MC>& arg) {
  return calling_conventions(0, last_int_register(), 100, 115, outgoing, -size_domainstate_args, arg);
}
Regs loc_parameters(const std::vector<MC>& arg) {
  return calling_conventions(0, last_int_register(), 100, 115, incoming, -size_domainstate_args, arg).first;
}
Regs loc_results(const std::vector<MC>& res) {
  return calling_conventions(0, last_int_register(), 100, 115, not_supported, 0, res).first;
}
std::pair<std::vector<Regs>, long> loc_external_arguments(const std::vector<Exttype>& ty_args) {
  return external_calling_conventions(0, 7, 100, 107, outgoing, ty_args);
}
Regs loc_external_results(const std::vector<MC>& res) {
  return calling_conventions(0, 1, 100, 100, not_supported, 0, res).first;
}
Reg* loc_exn_bucket() { return phys_reg(0); }

// first 23 int regs allocatable; all float regs allocatable
long num_available_registers[num_register_classes] = {23, 32};
const long first_available_register[num_register_classes] = {0, 100};

void init() {}

long register_class(const Reg* r) { return r->typ == MC::Float ? 1 : 0; }

// Registers destroyed by operations
namespace {
Regs regs_of(std::initializer_list<long> ns) {
  Regs r;
  for (long n : ns) r.push_back(phys_reg(n));
  return r;
}
Regs all_phys_regs() {
  init_hard_regs();
  Regs r = hard_int_reg;
  r.insert(r.end(), hard_float_reg.begin(), hard_float_reg.end());
  return r;
}
// x20-x28, d8-d15 preserved
Regs destroyed_at_c_noalloc_call() {
  return regs_of({0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,
                  100, 101, 102, 103, 104, 105, 106, 107, 116, 117, 118, 119, 120, 121, 122, 123,
                  124, 125, 126, 127, 128, 129, 130, 131});
}
}  // namespace

Regs destroyed_at_oper(const mach::Instruction& i) {
  using K = mach::Operation::K;
  if (i.desc != mach::Instruction::K::Iop) return {};
  const mach::Operation& op = *i.op;
  switch (op.k) {
    case K::Icall_ind:
    case K::Icall_imm: return all_phys_regs();
    case K::Iextcall:
      if (op.stack_ofs < 0) throw std::runtime_error("Proc.destroyed_at_oper: negative stack_ofs");
      if (op.alloc || op.stack_ofs > 0) return all_phys_regs();
      return destroyed_at_c_noalloc_call();
    case K::Ialloc:
    case K::Ipoll: return regs_of({8});
    case K::Iintoffloat:
    case K::Ifloatofint: return regs_of({107});  // d7 / s7 destroyed
    case K::Iload:
    case K::Istore:
      if (op.chunk == cmm::MemoryChunk::Single) return regs_of({107});
      return {};
    default: return {};
  }
}

Regs destroyed_at_raise() { return all_phys_regs(); }

// Maximal register pressure
long safe_register_pressure(const mach::Operation& op) {
  using K = mach::Operation::K;
  switch (op.k) {
    case K::Iextcall: return 7;
    case K::Ialloc:
    case K::Ipoll: return 22;
    default: return 23;
  }
}

std::vector<long> max_register_pressure(const mach::Operation& op) {
  using K = mach::Operation::K;
  switch (op.k) {
    case K::Iextcall: return {7, 8};  // 7 integer callee-saves, 8 FP callee-saves
    case K::Ialloc:
    case K::Ipoll: return {22, 32};
    case K::Iintoffloat:
    case K::Ifloatofint: return {23, 31};
    case K::Iload:
    case K::Istore:
      if (op.chunk == cmm::MemoryChunk::Single) return {23, 31};
      return {23, 32};
    default: return {23, 32};
  }
}

}  // namespace cppcaml::typing::proc
