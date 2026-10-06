// Ports of asmcomp/reg.ml, asmcomp/mach.ml and the interface of the
// target's Proc (asmcomp/proc.mli): pseudo-registers, the Mach
// pseudo-instructions Selection produces, and the register and calling
// conventions (src/typing/<arch>/proc.cpp).
#pragma once

#include <algorithm>
#include <iterator>

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cppcaml/typing/cmm.hpp"
#include "cppcaml/typing/format.hpp"

namespace cppcaml::typing {

// ---- Reg ---------------------------------------------------------------------------------------
namespace reg {

struct RawName {  // Anon | R | Var of V.t
  enum class K : std::uint8_t { Anon, R, Var } k = K::Anon;
  Ident::t var = nullptr;
};

struct Location {  // Unknown | Reg of int | Stack of stack_location
  enum class K : std::uint8_t { Unknown, Reg, Local, Incoming, Outgoing, Domainstate } k = K::Unknown;
  long n = 0;  // the register number or the stack slot
};

struct Reg;
struct RegLess {
  bool operator()(const Reg* a, const Reg* b) const;  // RegOrder: by stamp
};
// Reg.Set: the registers ordered by stamp.  A sorted vector (a liveness set
// is stored in every instruction: a node per element, as std::set has,
// costs five times the memory), with the std::set operations the passes use.
class Set {
 public:
  using value_type = Reg*;
  using const_iterator = Reg* const*;
  using iterator = const_iterator;
  Set() = default;
  template <class It>
  Set(It first, It last) {
    insert(first, last);
  }
  Set(std::initializer_list<Reg*> l) { insert(l.begin(), l.end()); }
  std::pair<iterator, bool> insert(Reg* r) {
    auto it = std::lower_bound(v_.begin(), v_.end(), r, RegLess{});
    if (it != v_.end() && !RegLess{}(r, *it)) return {it, false};
    it = v_.insert(it, r);
    return {it, true};
  }
  template <class It>
  void insert(It first, It last) {
    for (; first != last; ++first) insert(*first);
  }
  std::size_t erase(Reg* r) {
    auto it = std::lower_bound(v_.begin(), v_.end(), r, RegLess{});
    if (it == v_.end() || RegLess{}(r, *it)) return 0;
    v_.erase(it);
    return 1;
  }
  iterator find(Reg* r) const {
    auto it = std::lower_bound(v_.begin(), v_.end(), r, RegLess{});
    return it != v_.end() && !RegLess{}(r, *it) ? it : v_.end();
  }
  std::size_t count(Reg* r) const { return find(r) != v_.end() ? 1 : 0; }
  bool contains(Reg* r) const { return count(r) != 0; }
  iterator begin() const { return v_.begin(); }
  iterator end() const { return v_.end(); }
  std::size_t size() const { return v_.size(); }
  bool empty() const { return v_.empty(); }
  void clear() { v_.clear(); }
  // union / difference / intersection by merging (linear)
  static Set set_union(const Set& a, const Set& b) {
    Set r;
    r.v_.reserve(a.size() + b.size());
    std::set_union(a.v_.begin(), a.v_.end(), b.v_.begin(), b.v_.end(), std::back_inserter(r.v_), RegLess{});
    return r;
  }
  static Set set_difference(const Set& a, const Set& b) {
    Set r;
    std::set_difference(a.v_.begin(), a.v_.end(), b.v_.begin(), b.v_.end(), std::back_inserter(r.v_), RegLess{});
    return r;
  }
  static bool includes(const Set& a, const Set& b) {  // b is a subset of a
    return std::includes(a.v_.begin(), a.v_.end(), b.v_.begin(), b.v_.end(), RegLess{});
  }
  friend bool operator==(const Set& a, const Set& b) { return a.v_ == b.v_; }
  friend bool operator!=(const Set& a, const Set& b) { return a.v_ != b.v_; }

 private:
  SmallVec<Reg*, 4> v_;
};

using ::cppcaml::typing::NewestFirst;

struct Reg {
  RawName raw_name;
  long stamp;
  cmm::MachtypeComponent typ;
  Location loc;
  bool spill = false;
  std::optional<long> part;
  NewestFirst<Reg*> interf;                  // list order: the newest first
  NewestFirst<std::pair<Reg*, long>> prefer;  // list order: the newest first
  long degree = 0;
  long spill_cost = 0;
  long visited = 0;
};
inline bool RegLess::operator()(const Reg* a, const Reg* b) const { return a->stamp < b->stamp; }

using Regs = SmallVec<Reg*, 2>;  // Reg.t array (mostly 0 to 2 registers: inline)

Reg* create(cmm::MachtypeComponent ty);
Regs createv(cmm::Machtype tyv);
Regs createv(const std::vector<cmm::MachtypeComponent>& tyv);
Regs createv_like(const Regs& rv);
Reg* clone(Reg* r);
Reg* at_location(cmm::MachtypeComponent ty, Location loc);
std::vector<cmm::MachtypeComponent> typv(const Regs& rv);
bool anonymous(const Reg* r);
std::string name(const Reg* r);
void reset();
void reinit();
void mark_visited(Reg* r);
bool is_visited(const Reg* r);
void clear_visited_marks();
std::vector<Reg*> all_registers();  // Reg.all_registers: the newest first
long num_registers();

}  // namespace reg

}  // namespace cppcaml::typing

#include "cppcaml/typing/arch.hpp"

namespace cppcaml::typing {

// ---- Mach --------------------------------------------------------------------------------------
namespace mach {

using reg::Reg;
using reg::Regs;

struct IntegerComparison {  // Isigned of Cmm.integer_comparison | Iunsigned of ..
  bool is_signed;
  lambda::IntegerComparison c;
};

enum class IntegerOperation : std::uint8_t {
  Iadd, Isub, Imul, Imulh, Idiv, Imod, Iand, Ior, Ixor, Ilsl, Ilsr, Iasr, Icomp, Icheckbound
};
struct IntOp {
  IntegerOperation op;
  IntegerComparison cmp{true, lambda::IntegerComparison::Ceq};  // Icomp
};

struct Test {
  enum class K : std::uint8_t { Itruetest, Ifalsetest, Iinttest, Iinttest_imm, Ifloattest, Ioddtest, Ieventest } k;
  IntegerComparison icmp{true, lambda::IntegerComparison::Ceq};
  long n = 0;  // Iinttest_imm
  lambda::FloatComparison fcmp = lambda::FloatComparison::CFeq;
};

struct AllocDbginfo {
  long alloc_words;
  debuginfo::t alloc_dbg;
};

struct Operation {
  enum class K : std::uint8_t {
    Imove, Ispill, Ireload, Iconst_int, Iconst_float, Iconst_symbol, Icall_ind, Icall_imm, Itailcall_ind,
    Itailcall_imm, Iextcall, Istackoffset, Iload, Istore, Ialloc, Iintop, Iintop_imm, Icompf,
    Inegf, Iabsf, Iaddf, Isubf, Imulf, Idivf, Ifloatofint, Iintoffloat, Iopaque, Ispecific, Ipoll, Idls_get,
    Ireturn_addr
  } k;
  std::int64_t n = 0;             // Iconst_int; Iconst_float's bits; Istackoffset; Ialloc's bytes; Iintop_imm's
  std::string_view func;          // Iconst_symbol, Icall_imm, Itailcall_imm, Iextcall
  cmm::Machtype ty_res;           // Iextcall
  Slice<cmm::Exttype> ty_args;    // Iextcall
  bool alloc = false;             // Iextcall
  long stack_ofs = 0;             // Iextcall
  cmm::MemoryChunk chunk = cmm::MemoryChunk::Word_int;  // Iload / Istore
  arch::AddressingMode addr{};                           // Iload / Istore
  MutableFlag mut = MutableFlag::Immutable;              // Iload
  bool is_atomic = false;                                // Iload
  bool is_assign = false;                                // Istore
  std::vector<AllocDbginfo> dbginfo;                     // Ialloc
  IntOp intop{IntegerOperation::Iadd};                   // Iintop / Iintop_imm
  lambda::FloatComparison fcmp = lambda::FloatComparison::CFeq;  // Icompf
  arch::SpecificOperation spec{arch::SpecificOperation::K::Ilea};  // Ispecific
  std::optional<long> return_label;                                // Ipoll
};
inline Operation mop(Operation::K k) { return Operation{k}; }
// An instruction's operation, shared by its copies as ocamlc's instructions
// share their immutable desc: a copy of [op] in the current zone (the
// function's scratch zone while it compiles, which outlives its
// instructions' generations), or the shared Imove.
const Operation* op_ref(const Operation& op);
const Operation* imove_op();

struct Instruction;
using Instr = Instruction*;
struct Handler {
  long n;
  Instr body;
};
struct Instruction {
  enum class K : std::uint8_t { Iend, Iop, Ireturn, Iifthenelse, Iswitch, Icatch, Iexit, Itrywith, Iraise } desc;
  const Operation* op = imove_op();  // Iop (op_ref)
  Test test{Test::K::Itruetest};      // Iifthenelse
  Instr ifso = nullptr, ifnot = nullptr;  // Iifthenelse; Itrywith's body / handler
  Slice<long> index;                      // Iswitch
  std::vector<Instr> cases;               // Iswitch
  cmm::RecFlag rec = cmm::RecFlag::Nonrecursive;  // Icatch
  std::vector<Handler> handlers;                  // Icatch
  Instr body = nullptr;                           // Icatch
  long nfail = 0;                                 // Iexit
  lambda::RaiseKind raise = lambda::RaiseKind::Raise_regular;  // Iraise
  Instr next = nullptr;
  Regs arg;
  Regs res;
  debuginfo::t dbg;
  reg::Set live;
};

Instr dummy_instr();
Instr end_instr();
Instr instr_cons(const Instruction& d, const Regs& a, const Regs& r, Instr n);
Instr instr_cons_debug(const Instruction& d, const Regs& a, const Regs& r, const debuginfo::t& dbg, Instr n);
Instr copy(Instr i);  // { i with .. }
// Asmgen's instruction generations: the zone new instructions go to (null:
// the current zone), and a body deep-copied into [into] (sharing kept)
void set_instr_zone(Zone* z);
Instr compact_instrs(Instr root, Zone& into);
Instruction iop(const Operation& op);
Instruction idesc(Instruction::K k);
void instr_iter(const std::function<void(Instr)>& f, Instr i);
bool operation_is_pure(const Operation& op);
bool operation_can_raise(const Operation& op);

struct Fundecl {
  std::string_view fun_name;
  Regs fun_args;
  Instr fun_body;
  Slice<cmm::CodegenOption> fun_codegen_options;
  debuginfo::t fun_dbg;
  lambda::PollAttribute fun_poll;
  std::vector<long> fun_num_stack_slots;
};

}  // namespace mach

// ---- Proc (amd64) ------------------------------------------------------------------------------
namespace proc {
using reg::Reg;
using reg::Regs;
constexpr long num_register_classes = 2;
std::string_view register_name(long r);
Reg* phys_reg(long n);
std::pair<Regs, long> loc_arguments(const std::vector<cmm::MachtypeComponent>& arg);
Regs loc_parameters(const std::vector<cmm::MachtypeComponent>& arg);
Regs loc_results(const std::vector<cmm::MachtypeComponent>& res);
Regs loc_external_results(const std::vector<cmm::MachtypeComponent>& res);
std::pair<std::vector<Regs>, long> loc_external_arguments(const std::vector<cmm::Exttype>& ty_args);
Reg* loc_exn_bucket();
void init();
long register_class(const Reg* r);
extern long num_available_registers[num_register_classes];
extern const long first_available_register[num_register_classes];
extern const bool rotate_registers;
extern const long max_arguments_for_tailcalls;
Regs destroyed_at_oper(const mach::Instruction& i);
Regs destroyed_at_raise();
std::vector<long> max_register_pressure(const mach::Operation& op);
long safe_register_pressure(const mach::Operation& op);
}  // namespace proc

// ---- Interval: live intervals for the linear scan register allocator ----------------------------
namespace interval {
struct Range {
  long rbegin, rend;
};
struct Interval {
  reg::Reg* reg = nullptr;  // Reg.dummy until the register is seen
  long ibegin = 0, iend = 0;
  // the range list: ranges[first..] (while built, newest last)
  std::vector<Range> ranges;
  std::size_t first = 0;
};
struct Result {
  std::vector<Interval*> intervals;  // sorted by start position
  std::vector<Interval*> fixed_intervals;
};
}  // namespace interval

// ---- Printmach ---------------------------------------------------------------------------------
namespace printmach {
void intervals(format::Formatter& ppf, const interval::Result& r);
void reg(format::Formatter& ppf, const reg::Reg* r);
void fundecl(format::Formatter& ppf, const mach::Fundecl& f);
void phase(format::Formatter& ppf, const std::string& msg, const mach::Fundecl& f);
void interferences(format::Formatter& ppf);
void print_regs(format::Formatter& ppf, const reg::Regs& v);
void print_regsetaddr(format::Formatter& ppf, const reg::Set& s);
void print_test(format::Formatter& ppf, const mach::Test& t, const reg::Regs& arg);
void print_operation(format::Formatter& ppf, const mach::Operation& op, const reg::Regs& arg, const reg::Regs& res);
void preferences(format::Formatter& ppf);
}  // namespace printmach

}  // namespace cppcaml::typing
