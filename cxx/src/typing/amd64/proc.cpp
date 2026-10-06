// Port of asmcomp/amd64/proc.ml (Unix, no frame pointers, PLT): the amd64
// registers and calling conventions.  See proc in mach.hpp.
#include "cppcaml/typing/mach.hpp"

#include <stdexcept>


namespace cppcaml::typing::proc {

const bool rotate_registers = false;
const long max_arguments_for_tailcalls = 10 /* in regs */ + 64 /* in domain state */;

using reg::Location;
using MC = cmm::MachtypeComponent;

namespace {
const char* const int_reg_name[] = {"%rax", "%rbx", "%rdi", "%rsi", "%rdx", "%rcx", "%r8",
                                    "%r9",  "%r12", "%r13", "%r10", "%r11", "%rbp"};
const char* const float_reg_name[] = {"%xmm0", "%xmm1", "%xmm2",  "%xmm3",  "%xmm4",  "%xmm5",  "%xmm6",  "%xmm7",
                                      "%xmm8", "%xmm9", "%xmm10", "%xmm11", "%xmm12", "%xmm13", "%xmm14", "%xmm15"};
std::vector<Reg*> hard_int_reg, hard_float_reg;
constexpr long size_int = 8, size_float = 8;
constexpr long size_domainstate_args = 64 * size_int;

Reg* stack_slot(Location slot, MC ty) { return reg::at_location(ty, slot); }

long align(long n, long a) { return n >= 0 ? (n + a - 1) & -a : n & -a; }

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

std::pair<Regs, long> calling_conventions(long first_int, long last_int, long first_float, long last_float,
                                          MakeStack make_stack, long first_stack,
                                          const std::vector<MC>& arg) {
  Regs loc(arg.size(), nullptr);
  long i_ = first_int, f_ = first_float, ofs = first_stack;
  for (std::size_t i = 0; i < arg.size(); ++i) {
    if (arg[i] != MC::Float) {
      if (i_ <= last_int) loc[i] = phys_reg(i_++);
      else {
        loc[i] = stack_slot(make_stack(ofs), arg[i]);
        ofs += size_int;
      }
    } else {
      if (f_ <= last_float) loc[i] = phys_reg(f_++);
      else {
        loc[i] = stack_slot(make_stack(ofs), MC::Float);
        ofs += size_float;
      }
    }
  }
  return {loc, align(std::max(0L, ofs), 16)};  // keep stack 16-aligned
}
}  // namespace

void init_hard_regs() {
  if (!hard_int_reg.empty()) return;
  for (long i = 0; i <= 12; ++i) hard_int_reg.push_back(reg::at_location(MC::Int, {Location::K::Reg, i}));
  for (long i = 0; i <= 15; ++i) hard_float_reg.push_back(reg::at_location(MC::Float, {Location::K::Reg, 100 + i}));
}

std::string_view register_name(long r) { return r < 100 ? int_reg_name[r] : float_reg_name[r - 100]; }

Reg* phys_reg(long n) {
  init_hard_regs();
  return n < 100 ? hard_int_reg[n] : hard_float_reg[n - 100];
}

std::pair<Regs, long> loc_arguments(const std::vector<MC>& arg) {
  return calling_conventions(0, 9, 100, 109, outgoing, -size_domainstate_args, arg);
}
Regs loc_parameters(const std::vector<MC>& arg) {
  return calling_conventions(0, 9, 100, 109, incoming, -size_domainstate_args, arg).first;
}
Regs loc_results(const std::vector<MC>& res) {
  return calling_conventions(0, 0, 100, 100, not_supported, 0, res).first;
}
Regs loc_external_results(const std::vector<MC>& res) {
  return calling_conventions(0, 0, 100, 100, not_supported, 0, res).first;
}
std::pair<std::vector<Regs>, long> loc_external_arguments(const std::vector<cmm::Exttype>& ty_args) {
  std::vector<MC> arg;
  for (cmm::Exttype t : ty_args) arg.push_back(t == cmm::Exttype::XFloat ? MC::Float : MC::Int);
  auto [loc, stack_ofs] = calling_conventions(2, 7, 100, 107, outgoing, 0, arg);
  std::vector<Regs> r;
  for (Reg* x : loc) r.push_back({x});
  return {r, stack_ofs};
}
Reg* loc_exn_bucket() { return phys_reg(0); }

long num_available_registers[num_register_classes] = {13, 16};
const long first_available_register[num_register_classes] = {0, 100};

// Config.with_frame_pointers = false: init keeps 13 integer registers
void init() { num_available_registers[0] = 13; }

long register_class(const Reg* r) { return r->typ == MC::Float ? 1 : 0; }

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
// X86_proc.use_plt: evaluated at module initialization, while
// Clflags.dlcode still has its default (true)
Regs destroyed_at_alloc_or_poll() { return regs_of({10, 11}); }
Regs destroyed_at_c_call() {
  // Unix: r12-r15 preserved
  return regs_of({0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112,
                  113, 114, 115});
}
}  // namespace

Regs destroyed_at_oper(const mach::Instruction& i) {
  using K = mach::Operation::K;
  using IO = mach::IntegerOperation;
  if (i.desc == mach::Instruction::K::Iswitch) return regs_of({0, 4});
  if (i.desc == mach::Instruction::K::Itrywith) return regs_of({11});
  if (i.desc != mach::Instruction::K::Iop) return {};
  const mach::Operation& op = *i.op;
  switch (op.k) {
    case K::Icall_ind:
    case K::Icall_imm: return all_phys_regs();
    case K::Iextcall:
      if (op.alloc || op.stack_ofs > 0) return all_phys_regs();
      return destroyed_at_c_call();
    case K::Iintop:
    case K::Iintop_imm:
      if (op.intop.op == IO::Idiv || op.intop.op == IO::Imod) return regs_of({0, 4});
      if (op.intop.op == IO::Icomp || (op.k == K::Iintop && op.intop.op == IO::Imulh)) return regs_of({0});
      return {};
    case K::Istore:
      if (op.chunk == cmm::MemoryChunk::Single) return regs_of({115});
      return {};
    case K::Ialloc:
    case K::Ipoll: return destroyed_at_alloc_or_poll();
    default: return {};
  }
}

Regs destroyed_at_raise() { return all_phys_regs(); }

// Maximal register pressure (no frame pointers, Unix)
std::vector<long> max_register_pressure(const mach::Operation& op) {
  using K = mach::Operation::K;
  using IO = mach::IntegerOperation;
  auto consumes = [](long i, long f) { return std::vector<long>{13 - i, 16 - f}; };
  switch (op.k) {
    case K::Iextcall: return consumes(9, 16);
    case K::Iintop:
    case K::Iintop_imm:
      if (op.intop.op == IO::Idiv || op.intop.op == IO::Imod) return consumes(2, 0);
      if (op.intop.op == IO::Icomp) return consumes(1, 0);
      return consumes(0, 0);
    case K::Ialloc:
    case K::Ipoll: return consumes(1 + 2, 0);
    case K::Istore:
      if (op.chunk == cmm::MemoryChunk::Single) return consumes(0, 1);
      return consumes(0, 0);
    case K::Icompf: return consumes(0, 1);
    default: return consumes(0, 0);
  }
}

long safe_register_pressure(const mach::Operation& op) {
  return op.k == mach::Operation::K::Iextcall ? 0 : 11;
}

}  // namespace cppcaml::typing::proc
