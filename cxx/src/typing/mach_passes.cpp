// Ports of the Mach passes between selection and linearization.  See
// mach_passes.hpp.
//
// Register stamps are observable (the -d dumps, and register allocation
// breaks ties by stamp), so every Reg.create / Reg.clone happens in
// OCaml's order: a record update [{i with desc = ..; next = ..}] and a
// constructor evaluate their fields right to left.
#include "cppcaml/typing/mach_passes.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#include "cppcaml/typing/clflags.hpp"

namespace cppcaml::typing::mach_passes {

using namespace mach;
using reg::Reg;
using reg::Regs;
using IK = Instruction::K;
using MK = mach::Operation::K;
using IO = IntegerOperation;
using LK = reg::Location::K;
using MC = cmm::MachtypeComponent;

namespace {

[[noreturn]] void fatal(const std::string& s) { throw std::runtime_error(s); }

struct RegCmp {
  int operator()(const Reg* a, const Reg* b) const {
    return a->stamp < b->stamp ? -1 : a->stamp > b->stamp ? 1 : 0;
  }
};
template <class V>
using RegMap = PMap<Reg*, V, RegCmp>;
using RegSet = reg::Set;

RegSet set_of_array(const Regs& v) { return RegSet(v.begin(), v.end()); }
RegSet add_set_array(const RegSet& s, const Regs& v) { return RegSet::set_union(s, set_of_array(v)); }
RegSet diff_set_array(const RegSet& s, const Regs& v) { return RegSet::set_difference(s, set_of_array(v)); }
RegSet inter_set_array(const RegSet& s, const Regs& v) {
  RegSet r;
  for (Reg* x : v)
    if (s.count(x)) r.insert(x);
  return r;
}
bool disjoint_set_array(const RegSet& s, const Regs& v) {
  for (Reg* x : v)
    if (s.count(x)) return false;
  return true;
}
RegSet set_union(const RegSet& a, const RegSet& b) { return RegSet::set_union(a, b); }
RegSet set_diff(const RegSet& a, const RegSet& b) { return RegSet::set_difference(a, b); }
bool set_subset(const RegSet& a, const RegSet& b) { return RegSet::includes(b, a); }

Instr cons(const Instruction& d, const Regs& a, const Regs& r, Instr n) { return instr_cons(d, a, r, n); }

}  // namespace

// ---- Comballoc: combine heap allocations occurring in the same basic block ----
namespace {
struct PendingAlloc {
  Reg* reg;
  std::vector<AllocDbginfo> dbginfos;
  long totalsz;
};
using AllocState = std::optional<PendingAlloc>;  // No_alloc | Pending_alloc

Instr combine_restart(Instr i);

std::pair<Instr, AllocState> combine(Instr i, const AllocState& allocstate) {
  switch (i->desc) {
    case IK::Iend:
    case IK::Ireturn:
    case IK::Iexit:
    case IK::Iraise: return {i, allocstate};
    case IK::Iop: {
      const mach::Operation& op = i->op;
      if (op.k == MK::Ialloc) {
        long sz = op.n;
        const std::vector<AllocDbginfo>& dbginfo = op.dbginfo;
        if (allocstate && allocstate->totalsz + sz <= (256 + 1) * 8) {
          std::vector<AllocDbginfo> d = dbginfo;  // dbginfo @ dbginfos
          d.insert(d.end(), allocstate->dbginfos.begin(), allocstate->dbginfos.end());
          auto [next, state] = combine(i->next, PendingAlloc{i->res[0], d, allocstate->totalsz + sz});
          mach::Operation add{MK::Iintop_imm};
          add.intop = {IO::Iadd};
          add.n = -sz;
          return {instr_cons_debug(iop(add), {allocstate->reg}, i->res, i->dbg, next), state};
        }
        auto [next, state] = combine(i->next, PendingAlloc{i->res[0], dbginfo, sz});
        if (!state) fatal("Comballoc.combine");
        long totalsz = state->totalsz;
        std::vector<AllocDbginfo> dbgs = state->dbginfos;
        long offset = totalsz - sz;
        if (offset != 0) {
          mach::Operation add{MK::Iintop_imm};
          add.intop = {IO::Iadd};
          add.n = offset;
          next = instr_cons_debug(iop(add), i->res, i->res, i->dbg, next);
        }
        mach::Operation alloc{MK::Ialloc};
        alloc.n = totalsz;
        alloc.dbginfo = dbgs;
        return {instr_cons_debug(iop(alloc), i->arg, i->res, i->dbg, next), allocstate};
      }
      if (op.k == MK::Icall_ind || op.k == MK::Icall_imm || op.k == MK::Iextcall || op.k == MK::Itailcall_ind ||
          op.k == MK::Itailcall_imm || op.k == MK::Ipoll) {
        Instr newnext = combine_restart(i->next);
        return {instr_cons_debug(*i, i->arg, i->res, i->dbg, newnext), allocstate};
      }
      auto [newnext, s2] = combine(i->next, allocstate);
      return {instr_cons_debug(*i, i->arg, i->res, i->dbg, newnext), s2};
    }
    case IK::Iifthenelse: {
      Instr newifso = combine_restart(i->ifso);
      Instr newifnot = combine_restart(i->ifnot);
      Instr newnext = combine_restart(i->next);
      Instruction d = idesc(IK::Iifthenelse);
      d.test = i->test;
      d.ifso = newifso;
      d.ifnot = newifnot;
      return {cons(d, i->arg, i->res, newnext), allocstate};
    }
    case IK::Iswitch: {
      Instruction d = idesc(IK::Iswitch);
      d.index = i->index;
      for (Instr c : i->cases) d.cases.push_back(combine_restart(c));
      Instr newnext = combine_restart(i->next);
      return {cons(d, i->arg, i->res, newnext), allocstate};
    }
    case IK::Icatch: {
      auto [newbody, s2] = combine(i->body, allocstate);
      Instruction d = idesc(IK::Icatch);
      d.rec = i->rec;
      for (auto& h : i->handlers) d.handlers.push_back({h.n, combine_restart(h.body)});
      d.body = newbody;
      Instr newnext = combine_restart(i->next);
      return {cons(d, i->arg, i->res, newnext), s2};
    }
    case IK::Itrywith: {
      auto [newbody, s2] = combine(i->ifso, allocstate);
      Instr newhandler = combine_restart(i->ifnot);
      Instr newnext = combine_restart(i->next);
      Instruction d = idesc(IK::Itrywith);
      d.ifso = newbody;
      d.ifnot = newhandler;
      return {cons(d, i->arg, i->res, newnext), s2};
    }
  }
  return {i, allocstate};
}

Instr combine_restart(Instr i) { return combine(i, std::nullopt).first; }
}  // namespace

mach::Fundecl comballoc(const mach::Fundecl& f) {
  mach::Fundecl r = f;
  r.fun_body = combine_restart(f.fun_body);
  return r;
}

// ---- CSE: common subexpression elimination by value numbering over extended basic blocks ----
namespace {

enum class OpClass { Op_pure, Op_checkbound, Op_load_immutable, Op_load_mutable, Op_store_init, Op_store_assign, Op_other };

// A total order on operations consistent with OCaml's structural equality
// (Rhs_map's Stdlib.compare: only equal keys matter to lookups)
int cmp_long(long a, long b) { return a < b ? -1 : a > b ? 1 : 0; }
int compare_addr(const arch::AddressingMode& a, const arch::AddressingMode& b) {
  if (int c = cmp_long(static_cast<long>(a.k), static_cast<long>(b.k))) return c;
  if (int c = a.sym.compare(b.sym)) return c;
  if (int c = cmp_long(a.scale, b.scale)) return c;
  return cmp_long(a.displ, b.displ);
}
int compare_operation(const mach::Operation& a, const mach::Operation& b) {
  if (int c = cmp_long(static_cast<long>(a.k), static_cast<long>(b.k))) return c;
  if (int c = cmp_long(a.n, b.n)) return c;
  if (int c = a.func.compare(b.func)) return c;
  if (int c = cmp_long(static_cast<long>(a.chunk), static_cast<long>(b.chunk))) return c;
  if (int c = compare_addr(a.addr, b.addr)) return c;
  if (int c = cmp_long(static_cast<long>(a.mut), static_cast<long>(b.mut))) return c;
  if (int c = cmp_long(a.is_atomic, b.is_atomic)) return c;
  if (int c = cmp_long(a.is_assign, b.is_assign)) return c;
  if (int c = cmp_long(static_cast<long>(a.intop.op), static_cast<long>(b.intop.op))) return c;
  if (int c = cmp_long(a.intop.cmp.is_signed, b.intop.cmp.is_signed)) return c;
  if (int c = cmp_long(static_cast<long>(a.intop.cmp.c), static_cast<long>(b.intop.cmp.c))) return c;
  if (int c = cmp_long(static_cast<long>(a.fcmp), static_cast<long>(b.fcmp))) return c;
  if (int c = cmp_long(static_cast<long>(a.spec.k), static_cast<long>(b.spec.k))) return c;
  if (int c = compare_addr(a.spec.addr, b.spec.addr)) return c;
  if (int c = cmp_long(a.spec.n, b.spec.n)) return c;
  if (int c = cmp_long(a.spec.is_assign, b.spec.is_assign)) return c;
  return cmp_long(static_cast<long>(a.spec.fop), static_cast<long>(b.spec.fop));
}
using Rhs = std::pair<mach::Operation, std::vector<long>>;
struct RhsCmp {
  int operator()(const Rhs& a, const Rhs& b) const {
    if (int c = compare_operation(a.first, b.first)) return c;
    return std::lexicographical_compare_three_way(a.second.begin(), a.second.end(), b.second.begin(), b.second.end()) <
                   0
               ? -1
               : (a.second == b.second ? 0 : 1);
  }
};
using RhsMap = PMap<Rhs, std::vector<long>, RhsCmp>;

struct Equations {
  RhsMap mutable_load_equations;
  RhsMap other_equations;
};

struct Numbering {
  long num_next = 0;          // next fresh value number
  Equations num_eqs;          // mapping rhs -> valnums
  RegMap<long> num_reg;       // mapping register -> valnum
};

std::pair<Numbering, long> fresh_valnum_reg(const Numbering& n, Reg* r) {
  long v = n.num_next;
  Numbering n2 = n;
  n2.num_next = v + 1;
  n2.num_reg = n.num_reg.add(r, v);
  return {n2, v};
}
std::pair<Numbering, std::vector<long>> fresh_valnum_regs(Numbering n, const Regs& rs) {
  std::vector<long> vs;
  for (Reg* r : rs) {
    auto [n2, v] = fresh_valnum_reg(n, r);
    n = n2;
    vs.push_back(v);
  }
  return {n, vs};
}
std::pair<Numbering, long> valnum_reg(const Numbering& n, Reg* r) {
  if (const long* v = n.num_reg.find_opt(r)) return {n, *v};
  return fresh_valnum_reg(n, r);
}
std::pair<Numbering, std::vector<long>> valnum_regs(Numbering n, const Regs& rs) {
  std::vector<long> vs;
  for (Reg* r : rs) {
    auto [n2, v] = valnum_reg(n, r);
    n = n2;
    vs.push_back(v);
  }
  return {n, vs};
}
const std::vector<long>* find_equation(OpClass c, const Numbering& n, const Rhs& rhs) {
  return (c == OpClass::Op_load_mutable ? n.num_eqs.mutable_load_equations : n.num_eqs.other_equations).find_opt(rhs);
}
// Find a register containing the given value number (Reg.Map.fold: the last
// in stamp order)
Reg* find_reg_containing(const Numbering& n, long v) {
  Reg* res = nullptr;
  n.num_reg.iter([&](Reg* const& r, const long& v2) {
    if (v2 == v) res = r;
  });
  return res;
}
std::optional<Regs> find_regs_containing(const Numbering& n, const std::vector<long>& vs) {
  Regs rs;
  for (long v : vs) {
    Reg* r = find_reg_containing(n, v);
    if (!r) return std::nullopt;
    rs.push_back(r);
  }
  return rs;
}
Numbering set_known_regs(Numbering n, const Regs& rs, const std::vector<long>& vs) {
  for (std::size_t k = 0; k < rs.size(); ++k) n.num_reg = n.num_reg.add(rs[k], vs[k]);
  return n;
}
Numbering set_move(const Numbering& n, Reg* src, Reg* dst) {
  auto [n1, v] = valnum_reg(n, src);
  n1.num_reg = n1.num_reg.add(dst, v);
  return n1;
}
Numbering set_fresh_regs(const Numbering& n, const Regs& rs, const Rhs& rhs, OpClass c) {
  auto [n1, vs] = fresh_valnum_regs(n, rs);
  // { n1 with num_eqs = Equations.add op_class rhs vs n.num_eqs }
  Equations eqs = n.num_eqs;
  if (c == OpClass::Op_load_mutable) eqs.mutable_load_equations = eqs.mutable_load_equations.add(rhs, vs);
  else eqs.other_equations = eqs.other_equations.add(rhs, vs);
  n1.num_eqs = eqs;
  return n1;
}
Numbering set_unknown_regs(Numbering n, const Regs& rs) {
  // Array.fold_right Reg.Map.remove rs
  for (std::size_t k = rs.size(); k-- > 0;) n.num_reg = n.num_reg.remove(rs[k]);
  return n;
}
Numbering remove_mutable_load_numbering(Numbering n) {
  n.num_eqs.mutable_load_equations = RhsMap{};
  return n;
}
// Forget everything we know about registers of type [Addr].
Numbering kill_addr_regs(Numbering n) {
  RegMap<long> m;
  n.num_reg.iter([&](Reg* const& r, const long& v) {
    if (r->typ != MC::Addr) m = m.add(r, v);
  });
  n.num_reg = m;
  return n;
}

// Prepend a set of moves before [i] to assign [srcs] to [dsts].
Instr insert_single_move(Instr i, Reg* src, Reg* dst) { return cons(iop(mop(MK::Imove)), {src}, {dst}, i); }
Instr insert_move(const Regs& srcs, const Regs& dsts, Instr i) {
  if (srcs.empty()) return i;
  if (srcs.size() == 1) return cons(iop(mop(MK::Imove)), srcs, dsts, i);
  // Parallel move: first copy srcs into tmps one by one, then copy tmps into
  // dsts one by one
  Regs tmps = reg::createv_like(srcs);
  Instr i1 = i;
  for (std::size_t k = 0; k < tmps.size(); ++k) i1 = insert_single_move(i1, tmps[k], dsts[k]);
  for (std::size_t k = 0; k < srcs.size(); ++k) i1 = insert_single_move(i1, srcs[k], tmps[k]);
  return i1;
}

OpClass class_of_operation(const mach::Operation& op) {
  using SK = arch::SpecificOperation::K;
  switch (op.k) {
    case MK::Iconst_int:
    case MK::Iconst_float:
    case MK::Iconst_symbol: return OpClass::Op_pure;
    case MK::Istackoffset: return OpClass::Op_other;
    case MK::Iload:
      if (op.is_atomic) return OpClass::Op_store_assign;
      return op.mut == MutableFlag::Mutable ? OpClass::Op_load_mutable : OpClass::Op_load_immutable;
    case MK::Istore: return op.is_assign ? OpClass::Op_store_assign : OpClass::Op_store_init;
    case MK::Iintop:
    case MK::Iintop_imm: return op.intop.op == IO::Icheckbound ? OpClass::Op_checkbound : OpClass::Op_pure;
    case MK::Icompf:
    case MK::Inegf:
    case MK::Iabsf:
    case MK::Iaddf:
    case MK::Isubf:
    case MK::Imulf:
    case MK::Idivf:
    case MK::Ifloatofint:
    case MK::Iintoffloat: return OpClass::Op_pure;
    case MK::Ispecific:
      // (amd64)
      switch (op.spec.k) {
        case SK::Ilea:
        case SK::Isextend32:
        case SK::Izextend32: return OpClass::Op_pure;
        case SK::Istore_int: return op.spec.is_assign ? OpClass::Op_store_assign : OpClass::Op_store_init;
        case SK::Ioffset_loc: return OpClass::Op_store_assign;
        case SK::Ifloatarithmem:
        case SK::Ifloatsqrtf: return OpClass::Op_load_mutable;
        default: return OpClass::Op_other;  // Ibswap, Isqrtf: super -> Op_other
      }
    case MK::Idls_get: return OpClass::Op_load_mutable;
    case MK::Ireturn_addr: return OpClass::Op_load_immutable;
    default: fatal("CSEgen.class_of_operation");
  }
}

bool is_cheap_operation(const mach::Operation& op) { return op.k == MK::Iconst_int; }

Instr cse_i(const Numbering& n, Instr i);

// { i with next = cse n i.next }
Instr with_next(Instr i, Instr next) {
  Instr c = copy(i);
  c->next = next;
  return c;
}

Instr cse_i(const Numbering& n, Instr i) {
  switch (i->desc) {
    case IK::Iend:
    case IK::Ireturn:
    case IK::Iexit:
    case IK::Iraise: return i;
    case IK::Iop: {
      const mach::Operation& op = i->op;
      switch (op.k) {
        case MK::Itailcall_ind:
        case MK::Itailcall_imm: return i;
        case MK::Imove:
        case MK::Ispill:
        case MK::Ireload: {
          // For moves, we associate the same value number to the result reg
          // as to the argument reg.
          Numbering n1 = set_move(n, i->arg[0], i->res[0]);
          return with_next(i, cse_i(n1, i->next));
        }
        case MK::Icall_ind:
        case MK::Icall_imm:
        case MK::Iextcall:
        case MK::Iopaque: return with_next(i, cse_i(Numbering{}, i->next));
        case MK::Ialloc:
        case MK::Ipoll: {
          // Allocations and polls: forget the derived heap pointers and the
          // mutable loads
          Numbering n1 = kill_addr_regs(remove_mutable_load_numbering(n));
          Numbering n2 = set_unknown_regs(n1, i->res);
          return with_next(i, cse_i(n2, i->next));
        }
        default: break;
      }
      OpClass c = class_of_operation(op);
      if (c == OpClass::Op_pure || c == OpClass::Op_checkbound || c == OpClass::Op_load_immutable ||
          c == OpClass::Op_load_mutable) {
        auto [n1, varg] = valnum_regs(n, i->arg);
        Numbering n2 = set_unknown_regs(n1, proc::destroyed_at_oper(*i));
        Rhs rhs{op, varg};
        if (const std::vector<long>* vres = find_equation(c, n1, rhs)) {
          // This operation was computed earlier.  Are there registers that
          // hold the results computed earlier?
          std::optional<Regs> res = find_regs_containing(n1, *vres);
          if (res && !is_cheap_operation(op)) {
            // We can replace res <- op args with r <- move res, provided res
            // are stable (non-volatile) registers.
            Numbering n3 = set_known_regs(n1, i->res, *vres);
            // insert_move res i.res (cse n3 i.next): right to left
            Instr next = cse_i(n3, i->next);
            return insert_move(*res, i->res, next);
          }
          // We already computed the operation but lost its results.
          Numbering n3 = set_known_regs(n2, i->res, *vres);
          return with_next(i, cse_i(n3, i->next));
        }
        // This operation produces a result we haven't seen earlier.
        Numbering n3 = set_fresh_regs(n2, i->res, rhs, c);
        return with_next(i, cse_i(n3, i->next));
      }
      if (c == OpClass::Op_store_init || c == OpClass::Op_other) {
        Numbering n1 = set_unknown_regs(n, proc::destroyed_at_oper(*i));
        Numbering n2 = set_unknown_regs(n1, i->res);
        return with_next(i, cse_i(n2, i->next));
      }
      // Op_store true: a non-initializing store can invalidate anything we
      // know about prior mutable loads.
      Numbering n1 = set_unknown_regs(n, proc::destroyed_at_oper(*i));
      Numbering n2 = set_unknown_regs(n1, i->res);
      Numbering n3 = remove_mutable_load_numbering(n2);
      return with_next(i, cse_i(n3, i->next));
    }
    // For control structures, we set the numbering to empty at every join
    // point, but propagate the current numbering across fork points.
    // (Record updates: [next] first, then the fields of [desc] right to
    // left.)
    case IK::Iifthenelse: {
      Numbering n1 = set_unknown_regs(n, proc::destroyed_at_oper(*i));
      Instr next = cse_i(Numbering{}, i->next);
      Instr ifnot = cse_i(n1, i->ifnot);
      Instr ifso = cse_i(n1, i->ifso);
      Instr c = copy(i);
      c->ifso = ifso;
      c->ifnot = ifnot;
      c->next = next;
      return c;
    }
    case IK::Iswitch: {
      Numbering n1 = set_unknown_regs(n, proc::destroyed_at_oper(*i));
      Instr next = cse_i(Numbering{}, i->next);
      std::vector<Instr> cases;
      for (Instr k : i->cases) cases.push_back(cse_i(n1, k));
      Instr c = copy(i);
      c->cases = cases;
      c->next = next;
      return c;
    }
    case IK::Icatch: {
      Instr next = cse_i(Numbering{}, i->next);
      Instr body = cse_i(n, i->body);
      std::vector<Handler> hs;
      for (auto& h : i->handlers) hs.push_back({h.n, cse_i(Numbering{}, h.body)});
      Instr c = copy(i);
      c->handlers = hs;
      c->body = body;
      c->next = next;
      return c;
    }
    case IK::Itrywith: {
      Instr next = cse_i(Numbering{}, i->next);
      Instr handler = cse_i(Numbering{}, i->ifnot);
      Instr body = cse_i(n, i->ifso);
      Instr c = copy(i);
      c->ifso = body;
      c->ifnot = handler;
      c->next = next;
      return c;
    }
  }
  return i;
}

}  // namespace

mach::Fundecl cse(const mach::Fundecl& f) {
  // CSE can trigger bad register allocation behaviors, see MPR#7630
  for (auto o : f.fun_codegen_options)
    if (o == cmm::CodegenOption::No_CSE) return f;
  mach::Fundecl r = f;
  r.fun_body = cse_i(Numbering{}, f.fun_body);
  return r;
}

// ---- Dataflow.Backward over register sets; Liveness ------------------------------------------
namespace {
struct SetDomain {
  static RegSet bot() { return {}; }
};
using SetTransfer = std::function<RegSet(Instr, const RegSet& next, const RegSet& exn)>;

RegSet analyze_sets(const std::function<RegSet(const RegSet&)>& exnhandler, const SetTransfer& transfer, Instr instr) {
  std::map<long, RegSet> lbls;
  auto get_lbl = [&](long n) {
    auto it = lbls.find(n);
    return it == lbls.end() ? RegSet{} : it->second;
  };
  std::function<RegSet(const RegSet&, const RegSet&, Instr)> before = [&](const RegSet& end_, const RegSet& exn,
                                                                          Instr i) -> RegSet {
    switch (i->desc) {
      case IK::Iend: return transfer(i, end_, exn);
      case IK::Ireturn: return transfer(i, {}, {});
      case IK::Iop: {
        if (i->op.k == MK::Itailcall_ind || i->op.k == MK::Itailcall_imm) return transfer(i, {}, {});
        RegSet bx = before(end_, exn, i->next);
        return transfer(i, bx, exn);
      }
      case IK::Iifthenelse: {
        RegSet bx = before(end_, exn, i->next);
        RegSet b1 = before(bx, exn, i->ifso);
        RegSet b0 = before(bx, exn, i->ifnot);
        return transfer(i, set_union(b1, b0), exn);
      }
      case IK::Iswitch: {
        RegSet bx = before(end_, exn, i->next);
        RegSet b1;
        for (Instr c : i->cases) b1 = set_union(b1, before(bx, exn, c));
        return transfer(i, b1, exn);
      }
      case IK::Icatch: {
        RegSet bx = before(end_, exn, i->next);
        if (i->rec == cmm::RecFlag::Nonrecursive) {
          for (auto& h : i->handlers) lbls[h.n] = before(bx, exn, h.body);
        } else {
          for (;;) {
            bool changed = false;
            for (auto& h : i->handlers) {
              RegSet b0 = get_lbl(h.n);
              RegSet b1 = before(bx, exn, h.body);
              if (!set_subset(b1, b0)) {
                lbls[h.n] = b1;
                changed = true;
              }
            }
            if (!changed) break;
          }
        }
        RegSet b = before(bx, exn, i->body);
        return transfer(i, b, exn);
      }
      case IK::Iexit: return transfer(i, get_lbl(i->nfail), exn);
      case IK::Itrywith: {
        RegSet bx = before(end_, exn, i->next);
        RegSet bh = exnhandler(before(bx, exn, i->ifnot));
        RegSet bb = before(bx, bh, i->ifso);
        return transfer(i, bb, exn);
      }
      case IK::Iraise: return transfer(i, {}, exn);
    }
    return {};
  };
  return before({}, {}, instr);
}
}  // namespace

void liveness(const mach::Fundecl& f) {
  SetTransfer transfer = [](Instr i, const RegSet& next, const RegSet& exn) -> RegSet {
    switch (i->desc) {
      case IK::Ireturn:
        i->live.clear();  // no regs are live across
        return set_of_array(i->arg);
      case IK::Iop: {
        const mach::Operation& op = i->op;
        if (op.k == MK::Itailcall_ind || op.k == MK::Itailcall_imm) {
          i->live.clear();
          return set_of_array(i->arg);
        }
        if (operation_is_pure(op) && disjoint_set_array(next, i->res)) {
          // This operation is dead code.  Ignore its arguments.
          i->live = next;
          return next;
        }
        RegSet across1 = diff_set_array(next, i->res);
        // Operations that can raise an exception (function calls, bounds
        // checks, allocations) can branch to the nearest enclosing try ...
        // with.
        RegSet across = operation_can_raise(op) ? set_union(across1, exn) : across1;
        i->live = across;
        return add_set_array(across, i->arg);
      }
      case IK::Iifthenelse:
      case IK::Iswitch:
        i->live = next;
        return add_set_array(next, i->arg);
      case IK::Iraise:
        i->live = exn;
        return add_set_array(exn, i->arg);
      default:  // Iend, Icatch, Iexit, Itrywith
        i->live = next;
        return next;
    }
  };
  auto exnhandler = [](const RegSet& before_handler) {
    RegSet s = before_handler;
    s.erase(proc::loc_exn_bucket());
    return s;
  };
  RegSet initially_live = analyze_sets(exnhandler, transfer, f.fun_body);
  // Sanity check: only function parameters can be live at entrypoint
  RegSet wrong_live = set_diff(initially_live, set_of_array(f.fun_args));
  if (!wrong_live.empty()) fatal("Liveness.fundecl");
}

// ---- Deadcode: remove pure instructions whose results are not used ----------------------------
namespace {
struct D {
  Instr i;                // optimized instruction
  RegSet regs;            // a set of registers live "before" instruction [i]
  std::set<long> exits;   // indexes of Iexit instructions "live before" [i]
};

Instr append_i(Instr a, Instr b) {
  if (b->desc == IK::Iend) return a;
  std::function<Instr(Instr)> app = [&](Instr x) -> Instr {
    if (x->desc == IK::Iend) return b;
    Instr c = copy(x);
    c->next = app(x->next);
    return c;
  };
  return app(a);
}

D deadcode_i(Instr i) {
  switch (i->desc) {
    case IK::Iend:
    case IK::Ireturn:
    case IK::Iraise: return {i, add_set_array(i->live, i->arg), {}};
    case IK::Iop: {
      if (i->op.k == MK::Itailcall_ind || i->op.k == MK::Itailcall_imm)
        return {i, add_set_array(i->live, i->arg), {}};
      D s = deadcode_i(i->next);
      if (operation_is_pure(i->op) && disjoint_set_array(s.regs, i->res)) return s;
      Instr c = copy(i);
      c->next = s.i;
      return {c, add_set_array(i->live, i->arg), s.exits};
    }
    case IK::Iifthenelse: {
      D ifso = deadcode_i(i->ifso);
      D ifnot = deadcode_i(i->ifnot);
      D s = deadcode_i(i->next);
      Instr c = copy(i);
      c->ifso = ifso.i;
      c->ifnot = ifnot.i;
      c->next = s.i;
      std::set<long> exits = s.exits;
      exits.insert(ifso.exits.begin(), ifso.exits.end());
      exits.insert(ifnot.exits.begin(), ifnot.exits.end());
      return {c, add_set_array(i->live, i->arg), exits};
    }
    case IK::Iswitch: {
      std::vector<D> dc;
      for (Instr k : i->cases) dc.push_back(deadcode_i(k));
      D s = deadcode_i(i->next);
      Instr c = copy(i);
      c->cases.clear();
      std::set<long> exits = s.exits;
      for (D& d : dc) {
        c->cases.push_back(d.i);
        exits.insert(d.exits.begin(), d.exits.end());
      }
      c->next = s.i;
      return {c, add_set_array(i->live, i->arg), exits};
    }
    case IK::Icatch: {
      D body = deadcode_i(i->body);
      D s = deadcode_i(i->next);
      std::map<long, D> handlers;  // Int.Map.map deadcode (Int.Map.of_list handlers)
      for (auto& h : i->handlers) handlers.insert_or_assign(h.n, D{});
      for (auto& [n, d] : handlers)
        for (auto& h : i->handlers)
          if (h.n == n) d = deadcode_i(h.body);
      std::set<long> live_exits;
      std::vector<std::pair<long, const D*>> used_handlers;  // list order: the newest first
      std::function<void(long)> add_live = [&](long nfail) {
        if (live_exits.count(nfail)) return;
        live_exits.insert(nfail);
        auto it = handlers.find(nfail);
        if (it == handlers.end()) return;
        used_handlers.insert(used_handlers.begin(), {nfail, &it->second});
        if (i->rec == cmm::RecFlag::Recursive)
          for (long e : std::set<long>(it->second.exits)) add_live(e);
      };
      for (long e : body.exits) add_live(e);
      // Remove exits that are going out of scope.
      for (auto& [n, _] : used_handlers) live_exits.erase(n);
      // For non-recursive catch, live exits referenced in handlers are free.
      if (i->rec == cmm::RecFlag::Nonrecursive)
        for (auto& [_, h] : used_handlers) live_exits.insert(h->exits.begin(), h->exits.end());
      std::set<long> exits = s.exits;
      exits.insert(live_exits.begin(), live_exits.end());
      if (used_handlers.empty())  // Simplify catch without handlers
        return {append_i(body.i, s.i), body.regs, exits};
      Instr c = copy(i);
      c->handlers.clear();
      for (auto& [n, h] : used_handlers) c->handlers.push_back({n, h->i});
      c->body = body.i;
      c->next = s.i;
      return {c, i->live, exits};
    }
    case IK::Iexit: return {i, i->live, {i->nfail}};
    case IK::Itrywith: {
      D body = deadcode_i(i->ifso);
      D handler = deadcode_i(i->ifnot);
      D s = deadcode_i(i->next);
      Instr c = copy(i);
      c->ifso = body.i;
      c->ifnot = handler.i;
      c->next = s.i;
      std::set<long> exits = s.exits;
      exits.insert(body.exits.begin(), body.exits.end());
      exits.insert(handler.exits.begin(), handler.exits.end());
      return {c, i->live, exits};
    }
  }
  return {i, {}, {}};
}
}  // namespace

mach::Fundecl deadcode(const mach::Fundecl& f) {
  mach::Fundecl r = f;
  r.fun_body = deadcode_i(f.fun_body).i;
  return r;
}

// ---- Spill: insertion of moves to suggest possible spilling / reloading points ----------------
namespace {

struct SpillEnv {
  std::map<Reg*, Reg*, reg::RegLess> m;
};

struct ReloadData {
  SpillEnv* spill_env;
  RegMap<long> use_date;  // Record the position of last use of registers
  long current_date = 0;
  std::vector<std::pair<Instr, RegSet>> destroyed_at_fork;  // list order: the newest first
  std::map<long, RegSet> reload_at_exit;
};

struct SpillData {
  SpillEnv* spill_env;
  std::vector<std::pair<Instr, RegSet>> destroyed_at_fork;
  RegSet spill_at_raise;
  bool inside_arm = false;
  bool inside_catch = false;
  std::map<long, RegSet> spill_at_exit;
};

Reg* spill_reg(SpillEnv* env, Reg* r) {
  auto it = env->m.find(r);
  if (it != env->m.end()) return it->second;
  Reg* spill_r = reg::create(r->typ);
  spill_r->spill = true;
  if (!reg::anonymous(r)) spill_r->raw_name = r->raw_name;
  env->m[r] = spill_r;
  return spill_r;
}

void record_use(ReloadData& t, const Regs& regv) {
  for (Reg* r : regv) {
    const long* p = t.use_date.find_opt(r);
    long prev_date = p ? *p : 0;
    if (t.current_date > prev_date) t.use_date = t.use_date.add(r, t.current_date);
  }
}

// Check if the register pressure overflows the maximum pressure allowed at
// that point. If so, spill enough registers to lower the pressure.
RegSet add_superpressure_regs(ReloadData& t, const mach::Operation& op, const RegSet& live_regs, const Regs& res_regs,
                              RegSet spilled) {
  std::vector<long> max_pressure = proc::max_register_pressure(op);
  RegSet regs = add_set_array(live_regs, res_regs);
  // Compute the pressure in each register class
  std::vector<long> pressure(proc::num_register_classes, 0);
  for (Reg* r : regs) {
    if (spilled.count(r)) continue;
    if (r->loc.k == LK::Local || r->loc.k == LK::Incoming || r->loc.k == LK::Outgoing ||
        r->loc.k == LK::Domainstate)
      continue;
    pressure[proc::register_class(r)] += 1;
  }
  // Check if pressure is exceeded for each class.
  long cl = 0;
  while (cl < proc::num_register_classes) {
    if (pressure[cl] <= max_pressure[cl]) {
      ++cl;
      continue;
    }
    // Find the least recently used, unspilled, unallocated, live register
    // in the class
    long lru_date = 1000000;
    Reg* lru_reg = nullptr;
    for (Reg* r : live_regs) {
      if (proc::register_class(r) == cl && !spilled.count(r) && r->loc.k == LK::Unknown) {
        if (const long* d = t.use_date.find_opt(r))
          if (*d < lru_date) {
            lru_date = *d;
            lru_reg = r;
          }
      }
    }
    if (lru_reg) {
      pressure[cl] -= 1;
      spilled.insert(lru_reg);
    } else {
      // Couldn't find any spillable register, give up for this class
      ++cl;
    }
  }
  return spilled;
}

// First pass: insert reload instructions based on an approximation of what
// is destroyed at pressure points.
Instr add_reloads(SpillEnv* env, const RegSet& regset, Instr i) {
  // Reg.Set.fold: in increasing stamp order, each consed in front
  for (Reg* r : regset) i = cons(iop(mop(MK::Ireload)), {spill_reg(env, r)}, {r}, i);
  return i;
}

RegSet get_at(const std::map<long, RegSet>& m, long k) {
  auto it = m.find(k);
  return it == m.end() ? RegSet{} : it->second;
}

std::pair<Instr, RegSet> reload_i(ReloadData& t, Instr i, const RegSet& before) {
  t.current_date += 1;
  record_use(t, i->arg);
  record_use(t, i->res);
  switch (i->desc) {
    case IK::Iend: return {i, before};
    case IK::Ireturn: return {add_reloads(t.spill_env, inter_set_array(before, i->arg), i), {}};
    case IK::Iop: {
      const mach::Operation& op = i->op;
      if (op.k == MK::Itailcall_ind || op.k == MK::Itailcall_imm)
        return {add_reloads(t.spill_env, inter_set_array(before, i->arg), i), {}};
      if (op.k == MK::Icall_ind || op.k == MK::Icall_imm || (op.k == MK::Iextcall && op.alloc)) {
        // All regs live across must be spilled
        auto [new_next, finally] = reload_i(t, i->next, i->live);
        return {add_reloads(t.spill_env, inter_set_array(before, i->arg),
                            instr_cons_debug(*i, i->arg, i->res, i->dbg, new_next)),
                finally};
      }
      RegSet new_before;
      // Quick check to see if the register pressure is below the maximum
      if (clflags::use_linscan ||
          static_cast<long>(i->live.size() + i->res.size()) <= proc::safe_register_pressure(op))
        new_before = before;
      else
        new_before = add_superpressure_regs(t, op, i->live, i->res, before);
      RegSet after = diff_set_array(diff_set_array(new_before, i->arg), i->res);
      auto [new_next, finally] = reload_i(t, i->next, after);
      return {add_reloads(t.spill_env, inter_set_array(new_before, i->arg),
                          instr_cons_debug(*i, i->arg, i->res, i->dbg, new_next)),
              finally};
    }
    case IK::Iifthenelse: {
      RegSet at_fork = diff_set_array(before, i->arg);
      long date_fork = t.current_date;
      auto [new_ifso, after_ifso] = reload_i(t, i->ifso, at_fork);
      long date_ifso = t.current_date;
      t.current_date = date_fork;
      auto [new_ifnot, after_ifnot] = reload_i(t, i->ifnot, at_fork);
      t.current_date = std::max(date_ifso, t.current_date);
      auto [new_next, finally] = reload_i(t, i->next, set_union(after_ifso, after_ifnot));
      Instruction d = idesc(IK::Iifthenelse);
      d.test = i->test;
      d.ifso = new_ifso;
      d.ifnot = new_ifnot;
      Instr new_i = cons(d, i->arg, i->res, new_next);
      t.destroyed_at_fork.insert(t.destroyed_at_fork.begin(), {new_i, at_fork});
      return {add_reloads(t.spill_env, inter_set_array(before, i->arg), new_i), finally};
    }
    case IK::Iswitch: {
      RegSet at_fork = diff_set_array(before, i->arg);
      long date_fork = t.current_date;
      long date_join = 0;
      RegSet after_cases;
      Instruction d = idesc(IK::Iswitch);
      d.index = i->index;
      for (Instr c : i->cases) {
        t.current_date = date_fork;
        auto [new_c, after_c] = reload_i(t, c, at_fork);
        after_cases = set_union(after_cases, after_c);
        date_join = std::max(date_join, t.current_date);
        d.cases.push_back(new_c);
      }
      t.current_date = date_join;
      auto [new_next, finally] = reload_i(t, i->next, after_cases);
      return {add_reloads(t.spill_env, inter_set_array(before, i->arg), cons(d, i->arg, i->res, new_next)), finally};
    }
    case IK::Icatch: {
      auto [new_body, after_body] = reload_i(t, i->body, before);
      std::vector<std::pair<Instr, RegSet>> res;
      for (;;) {
        std::vector<RegSet> at_exits;
        for (auto& h : i->handlers) at_exits.push_back(get_at(t.reload_at_exit, h.n));
        res.clear();
        for (std::size_t k = 0; k < i->handlers.size(); ++k) res.push_back(reload_i(t, i->handlers[k].body, at_exits[k]));
        if (i->rec == cmm::RecFlag::Nonrecursive) break;
        bool equal = true;
        for (std::size_t k = 0; k < i->handlers.size(); ++k)
          if (at_exits[k] != get_at(t.reload_at_exit, i->handlers[k].n)) equal = false;
        if (equal) break;
      }
      RegSet un = after_body;
      for (auto& [_, a] : res) un = set_union(un, a);
      auto [new_next, finally] = reload_i(t, i->next, un);
      Instruction d = idesc(IK::Icatch);
      d.rec = i->rec;
      for (std::size_t k = 0; k < i->handlers.size(); ++k) d.handlers.push_back({i->handlers[k].n, res[k].first});
      d.body = new_body;
      return {cons(d, i->arg, i->res, new_next), finally};
    }
    case IK::Iexit:
      t.reload_at_exit[i->nfail] = set_union(get_at(t.reload_at_exit, i->nfail), before);
      return {i, {}};
    case IK::Itrywith: {
      auto [new_body, after_body] = reload_i(t, i->ifso, before);
      // All registers live at the beginning of the handler are destroyed,
      // except the exception bucket
      RegSet before_handler = add_set_array(i->ifnot->live, i->ifnot->arg);
      before_handler.erase(proc::loc_exn_bucket());
      auto [new_handler, after_handler] = reload_i(t, i->ifnot, before_handler);
      auto [new_next, finally] = reload_i(t, i->next, set_union(after_body, after_handler));
      Instruction d = idesc(IK::Itrywith);
      d.ifso = new_body;
      d.ifnot = new_handler;
      return {cons(d, i->arg, i->res, new_next), finally};
    }
    case IK::Iraise: return {add_reloads(t.spill_env, inter_set_array(before, i->arg), i), {}};
  }
  return {i, before};
}

// Second pass: add spill instructions based on what we've decided to
// reload.  That is, any register that may be reloaded in the future must
// be spilled just after its definition.
Instr add_spills(SpillEnv* env, const RegSet& regset, Instr i) {
  for (Reg* r : regset) i = cons(iop(mop(MK::Ispill)), {r}, {spill_reg(env, r)}, i);
  return i;
}

std::pair<Instr, RegSet> spill_i(SpillData& t, Instr i, const RegSet& finally) {
  switch (i->desc) {
    case IK::Iend: return {i, finally};
    case IK::Ireturn: return {i, {}};
    case IK::Iop: {
      const mach::Operation& op = i->op;
      if (op.k == MK::Itailcall_ind || op.k == MK::Itailcall_imm) return {i, {}};
      if (op.k == MK::Ireload) {
        auto [new_next, after] = spill_i(t, i->next, finally);
        RegSet before1 = diff_set_array(after, i->res);
        return {cons(*i, i->arg, i->res, new_next), add_set_array(before1, i->res)};
      }
      auto [new_next, after] = spill_i(t, i->next, finally);
      RegSet before1 = diff_set_array(after, i->res);
      RegSet before = operation_can_raise(op) ? set_union(before1, t.spill_at_raise) : before1;
      return {instr_cons_debug(*i, i->arg, i->res, i->dbg, add_spills(t.spill_env, inter_set_array(after, i->res), new_next)),
              before};
    }
    case IK::Iifthenelse: {
      auto [new_next, at_join] = spill_i(t, i->next, finally);
      auto [new_ifso, before_ifso] = spill_i(t, i->ifso, at_join);
      auto [new_ifnot, before_ifnot] = spill_i(t, i->ifnot, at_join);
      Instruction d = idesc(IK::Iifthenelse);
      d.test = i->test;
      if (t.inside_arm || t.inside_catch) {
        d.ifso = new_ifso;
        d.ifnot = new_ifnot;
        return {cons(d, i->arg, i->res, new_next), set_union(before_ifso, before_ifnot)};
      }
      const RegSet* destroyed = nullptr;
      for (auto& [ins, s] : t.destroyed_at_fork)
        if (ins == i) {
          destroyed = &s;
          break;
        }
      if (!destroyed) fatal("Spill: destroyed_at_fork");
      RegSet spill_ifso_branch = set_diff(set_diff(before_ifso, before_ifnot), *destroyed);
      RegSet spill_ifnot_branch = set_diff(set_diff(before_ifnot, before_ifso), *destroyed);
      // Iifthenelse(test, add_spills .. new_ifso, add_spills .. new_ifnot):
      // right to left
      d.ifnot = add_spills(t.spill_env, spill_ifnot_branch, new_ifnot);
      d.ifso = add_spills(t.spill_env, spill_ifso_branch, new_ifso);
      return {cons(d, i->arg, i->res, new_next),
              set_diff(set_diff(set_union(before_ifso, before_ifnot), spill_ifso_branch), spill_ifnot_branch)};
    }
    case IK::Iswitch: {
      auto [new_next, at_join] = spill_i(t, i->next, finally);
      bool saved_inside_arm = t.inside_arm;
      t.inside_arm = true;
      RegSet before;
      Instruction d = idesc(IK::Iswitch);
      d.index = i->index;
      for (Instr c : i->cases) {
        auto [new_c, before_c] = spill_i(t, c, at_join);
        before = set_union(before, before_c);
        d.cases.push_back(new_c);
      }
      t.inside_arm = saved_inside_arm;
      return {cons(d, i->arg, i->res, new_next), before};
    }
    case IK::Icatch: {
      auto [new_next, at_join] = spill_i(t, i->next, finally);
      bool saved_inside_catch = t.inside_catch;
      t.inside_catch = true;
      std::vector<std::pair<Instr, RegSet>> res;
      for (;;) {
        res.clear();
        for (auto& h : i->handlers) res.push_back(spill_i(t, h.body, at_join));
        bool changed = false;
        for (std::size_t k = 0; k < i->handlers.size(); ++k) {
          long n = i->handlers[k].n;
          if (res[k].second != get_at(t.spill_at_exit, n)) {
            t.spill_at_exit[n] = res[k].second;
            changed = true;
          }
        }
        if (!(i->rec == cmm::RecFlag::Recursive && changed)) break;
      }
      t.inside_catch = saved_inside_catch;
      auto [new_body, before] = spill_i(t, i->body, at_join);
      Instruction d = idesc(IK::Icatch);
      d.rec = i->rec;
      for (std::size_t k = 0; k < i->handlers.size(); ++k) d.handlers.push_back({i->handlers[k].n, res[k].first});
      d.body = new_body;
      return {cons(d, i->arg, i->res, new_next), before};
    }
    case IK::Iexit: return {i, get_at(t.spill_at_exit, i->nfail)};
    case IK::Itrywith: {
      auto [new_next, at_join] = spill_i(t, i->next, finally);
      auto [new_handler, before_handler] = spill_i(t, i->ifnot, at_join);
      RegSet saved_spill_at_raise = t.spill_at_raise;
      t.spill_at_raise = before_handler;
      auto [new_body, before_body] = spill_i(t, i->ifso, at_join);
      t.spill_at_raise = saved_spill_at_raise;
      Instruction d = idesc(IK::Itrywith);
      d.ifso = new_body;
      d.ifnot = new_handler;
      return {cons(d, i->arg, i->res, new_next), before_body};
    }
    case IK::Iraise: return {i, t.spill_at_raise};
  }
  return {i, finally};
}

}  // namespace

mach::Fundecl spill(const mach::Fundecl& f) {
  SpillEnv env;
  ReloadData reload_data{&env};
  Instr body1 = reload_i(reload_data, f.fun_body, {}).first;
  SpillData spill_data{&env, reload_data.destroyed_at_fork};
  auto [body2, tospill_at_entry] = spill_i(spill_data, body1, {});
  Instr new_body = add_spills(&env, inter_set_array(tospill_at_entry, f.fun_args), body2);
  mach::Fundecl r = f;
  r.fun_body = new_body;
  return r;
}

// ---- Split: renaming of registers at reload points to split live ranges ---------------------------
namespace {

using Subst = std::optional<RegMap<Reg*>>;  // Reg.t Reg.Map.t option

Reg* subst_reg(Reg* r, const RegMap<Reg*>& sub) {
  if (Reg* const* x = sub.find_opt(r)) return *x;
  return r;
}
Regs subst_regs(const Regs& rv, const Subst& sub) {
  if (!sub) return rv;
  Regs nv;
  for (Reg* r : rv) nv.push_back(subst_reg(r, *sub));
  return nv;
}

// We maintain equivalence classes of registers using a standard union-find
// algorithm
std::map<Reg*, Reg*, reg::RegLess> equiv_classes;

Reg* repres_reg(Reg* r) {
  for (;;) {
    auto it = equiv_classes.find(r);
    if (it == equiv_classes.end()) return r;
    r = it->second;
  }
}
void repres_regs(Regs& rv) {
  for (Reg*& r : rv) r = repres_reg(r);
}

// Identify two registers.  The second register is chosen as canonical
// representative.
void identify(Reg* r1, Reg* r2) {
  Reg* repres1 = repres_reg(r1);
  Reg* repres2 = repres_reg(r2);
  if (repres1->stamp != repres2->stamp) equiv_classes[repres1] = repres2;
}

// Identify the image of a register by two substitutions.
void identify_sub(const RegMap<Reg*>& sub1, const RegMap<Reg*>& sub2, Reg* r) {
  if (Reg* const* r1 = sub1.find_opt(r)) {
    if (Reg* const* r2 = sub2.find_opt(r)) identify(*r1, *r2);
    else identify(*r1, r);
    return;
  }
  if (Reg* const* r2 = sub2.find_opt(r)) identify(*r2, r);
}

// Identify registers so that the two substitutions agree on the registers
// live before the given instruction.
Subst merge_substs(const Subst& sub1, const Subst& sub2, Instr i) {
  if (!sub1 && !sub2) return std::nullopt;
  if (sub1 && !sub2) return sub1;
  if (!sub1 && sub2) return sub2;
  for (Reg* r : add_set_array(i->live, i->arg)) identify_sub(*sub1, *sub2, r);
  return sub1;
}

// Same, for N substitutions
Subst merge_subst_array(const std::vector<Subst>& subv, Instr instr) {
  for (std::size_t i = 0; i < subv.size(); ++i) {
    if (!subv[i]) continue;
    for (std::size_t j = i + 1; j < subv.size(); ++j) {
      if (!subv[j]) continue;
      for (Reg* r : add_set_array(instr->live, instr->arg)) identify_sub(*subv[i], *subv[j], r);
    }
    return subv[i];
  }
  return std::nullopt;
}

// First pass: rename registers at reload points
std::vector<std::pair<long, std::shared_ptr<Subst>>> exit_subst;  // list order: the newest first

std::pair<Instr, Subst> rename(Instr i, const Subst& sub) {
  switch (i->desc) {
    case IK::Iend: return {i, sub};
    case IK::Ireturn: return {instr_cons_debug(*i, subst_regs(i->arg, sub), {}, i->dbg, i->next), std::nullopt};
    case IK::Iop: {
      if (i->op.k == MK::Itailcall_ind || i->op.k == MK::Itailcall_imm)
        return {instr_cons_debug(*i, subst_regs(i->arg, sub), {}, i->dbg, i->next), std::nullopt};
      if (i->op.k == MK::Ireload && i->res[0]->loc.k == LK::Unknown) {
        if (!sub) return rename(i->next, sub);
        Reg* oldr = i->res[0];
        Reg* newr = reg::clone(i->res[0]);
        auto [new_next, sub_next] = rename(i->next, Subst(sub->add(oldr, newr)));
        return {cons(*i, i->arg, {newr}, new_next), sub_next};
      }
      auto [new_next, sub_next] = rename(i->next, sub);
      return {instr_cons_debug(*i, subst_regs(i->arg, sub), subst_regs(i->res, sub), i->dbg, new_next), sub_next};
    }
    case IK::Iifthenelse: {
      auto [new_ifso, sub_ifso] = rename(i->ifso, sub);
      auto [new_ifnot, sub_ifnot] = rename(i->ifnot, sub);
      auto [new_next, sub_next] = rename(i->next, merge_substs(sub_ifso, sub_ifnot, i->next));
      Instruction d = idesc(IK::Iifthenelse);
      d.test = i->test;
      d.ifso = new_ifso;
      d.ifnot = new_ifnot;
      return {cons(d, subst_regs(i->arg, sub), {}, new_next), sub_next};
    }
    case IK::Iswitch: {
      std::vector<std::pair<Instr, Subst>> new_sub_cases;
      for (Instr c : i->cases) new_sub_cases.push_back(rename(c, sub));
      std::vector<Subst> subs;
      for (auto& [_, s] : new_sub_cases) subs.push_back(s);
      Subst sub_merge = merge_subst_array(subs, i->next);
      auto [new_next, sub_next] = rename(i->next, sub_merge);
      Instruction d = idesc(IK::Iswitch);
      d.index = i->index;
      for (auto& [n, _] : new_sub_cases) d.cases.push_back(n);
      return {cons(d, subst_regs(i->arg, sub), {}, new_next), sub_next};
    }
    case IK::Icatch: {
      std::vector<std::pair<long, std::shared_ptr<Subst>>> new_subst;
      for (auto& h : i->handlers) new_subst.push_back({h.n, std::make_shared<Subst>()});
      auto previous_exit_subst = exit_subst;
      exit_subst.insert(exit_subst.begin(), new_subst.begin(), new_subst.end());
      auto [new_body, sub_body] = rename(i->body, sub);
      std::vector<std::pair<Instr, Subst>> res;
      for (std::size_t k = 0; k < i->handlers.size(); ++k)
        res.push_back(rename(i->handlers[k].body, *new_subst[k].second));
      exit_subst = previous_exit_subst;
      Subst merged_subst = sub_body;
      for (auto& [_, s] : res) merged_subst = merge_substs(merged_subst, s, i->next);
      auto [new_next, sub_next] = rename(i->next, merged_subst);
      Instruction d = idesc(IK::Icatch);
      d.rec = i->rec;
      for (std::size_t k = 0; k < i->handlers.size(); ++k) d.handlers.push_back({i->handlers[k].n, res[k].first});
      d.body = new_body;
      return {cons(d, {}, {}, new_next), sub_next};
    }
    case IK::Iexit: {
      std::shared_ptr<Subst> r;
      for (auto& [k, s] : exit_subst)
        if (k == i->nfail) {
          r = s;
          break;
        }
      if (!r) fatal("Split.find_exit_subst");
      *r = merge_substs(*r, sub, i);
      return {i, std::nullopt};
    }
    case IK::Itrywith: {
      auto [new_body, sub_body] = rename(i->ifso, sub);
      auto [new_handler, sub_handler] = rename(i->ifnot, sub);
      auto [new_next, sub_next] = rename(i->next, merge_substs(sub_body, sub_handler, i->next));
      Instruction d = idesc(IK::Itrywith);
      d.ifso = new_body;
      d.ifnot = new_handler;
      return {cons(d, {}, {}, new_next), sub_next};
    }
    case IK::Iraise:
      return {instr_cons_debug(*i, subst_regs(i->arg, sub), {}, i->dbg, i->next), std::nullopt};
  }
  return {i, sub};
}

}  // namespace

mach::Fundecl split(const mach::Fundecl& f) {
  equiv_classes.clear();
  exit_subst.clear();
  Regs new_args = f.fun_args;
  Instr new_body = rename(f.fun_body, Subst(RegMap<Reg*>{})).first;
  repres_regs(new_args);
  // Second pass: replace registers by their final representatives
  instr_iter(
      [](Instr i) {
        repres_regs(i->arg);
        repres_regs(i->res);
      },
      new_body);
  equiv_classes.clear();
  mach::Fundecl r = f;
  r.fun_args = new_args;
  r.fun_body = new_body;
  return r;
}

// ---- Interf: construction of the interference graph ------------------------------------------
void interf_build_graph(const mach::Fundecl& fundecl) {
  // The interference graph is represented in two ways: by adjacency lists
  // for each register, and by a sparse bit matrix (a set of pairs of
  // register stamps)
  std::set<std::pair<long, long>> mat;

  // Record an interference between two registers
  auto add_interf = [&](Reg* ri, Reg* rj) {
    if (proc::register_class(ri) != proc::register_class(rj)) return;
    long i = ri->stamp, j = rj->stamp;
    if (i == j) return;
    std::pair<long, long> p = i < j ? std::pair{i, j} : std::pair{j, i};
    if (mat.count(p)) return;
    mat.insert(p);
    if (ri->loc.k == LK::Unknown) {
      ri->interf.push_front(rj);
      if (!rj->spill) ri->degree += 1;
    }
    if (rj->loc.k == LK::Unknown) {
      rj->interf.push_front(ri);
      if (!ri->spill) rj->degree += 1;
    }
  };
  // Record interferences between a register array and a set of registers
  auto add_interf_set = [&](const Regs& v, const RegSet& s) {
    for (Reg* r1 : v)
      for (Reg* r : s) add_interf(r1, r);
  };
  // Record interferences between elements of an array
  auto add_interf_self = [&](const Regs& v) {
    for (std::size_t i = 0; i + 1 < v.size(); ++i)
      for (std::size_t j = i + 1; j < v.size(); ++j) add_interf(v[i], v[j]);
  };
  // Record interferences between the destination of a move and a set of
  // live registers. Since the destination is equal to the source, do not
  // add an interference between them if the source is still live
  // afterwards.
  auto add_interf_move = [&](Reg* src, Reg* dst, const RegSet& s) {
    for (Reg* r : s)
      if (r->stamp != src->stamp) add_interf(dst, r);
  };

  // Compute interferences
  std::function<void(Instr)> interf = [&](Instr i) {
    for (;;) {
      Regs destroyed = proc::destroyed_at_oper(*i);
      if (!destroyed.empty()) add_interf_set(destroyed, i->live);
      switch (i->desc) {
        case IK::Iend:
        case IK::Ireturn:
        case IK::Iexit:
        case IK::Iraise: return;
        case IK::Iop:
          switch (i->op.k) {
            case MK::Imove:
            case MK::Ispill:
            case MK::Ireload: add_interf_move(i->arg[0], i->res[0], i->live); break;
            case MK::Itailcall_ind:
            case MK::Itailcall_imm: return;
            default:
              add_interf_set(i->res, i->live);
              add_interf_self(i->res);
              break;
          }
          break;
        case IK::Iifthenelse:
          interf(i->ifso);
          interf(i->ifnot);
          break;
        case IK::Iswitch:
          for (Instr c : i->cases) interf(c);
          break;
        case IK::Icatch:
          interf(i->body);
          for (auto& h : i->handlers) interf(h.body);
          break;
        case IK::Itrywith:
          add_interf_set(proc::destroyed_at_raise(), i->ifnot->live);
          interf(i->ifso);
          interf(i->ifnot);
          break;
      }
      i = i->next;
    }
  };

  // Add a preference from one reg to another.  Do not add anything if the
  // two registers conflict, or if the source register already has a
  // location, or if the two registers belong to different classes.
  auto add_pref = [&](long weight, Reg* r1, Reg* r2) {
    long i = r1->stamp, j = r2->stamp;
    if (i != j && r1->loc.k == LK::Unknown && proc::register_class(r1) == proc::register_class(r2)) {
      std::pair<long, long> p = i < j ? std::pair{i, j} : std::pair{j, i};
      if (!mat.count(p)) r1->prefer.push_front({r2, weight});
    }
  };
  // Add a mutual preference between two regs
  auto add_mutual_pref = [&](long weight, Reg* r1, Reg* r2) {
    add_pref(weight, r1, r2);
    add_pref(weight, r2, r1);
  };
  // Update the spill cost of the registers involved in an operation
  auto add_spill_cost = [](long cost, const Regs& arg) {
    for (Reg* r : arg) r->spill_cost += cost;
  };

  // Compute preferences and spill costs
  std::function<void(long, Instr)> prefer = [&](long weight, Instr i) {
    for (;;) {
      add_spill_cost(weight, i->arg);
      add_spill_cost(weight, i->res);
      switch (i->desc) {
        case IK::Iend:
        case IK::Ireturn:
        case IK::Iexit:
        case IK::Iraise: return;
        case IK::Iop:
          switch (i->op.k) {
            case MK::Imove: add_mutual_pref(weight, i->arg[0], i->res[0]); break;
            case MK::Ispill: add_pref(weight / 4, i->arg[0], i->res[0]); break;
            case MK::Ireload: add_pref(weight / 4, i->res[0], i->arg[0]); break;
            case MK::Itailcall_ind:
            case MK::Itailcall_imm: return;
            default: break;
          }
          break;
        case IK::Iifthenelse:
          prefer(weight, i->ifso);
          prefer(weight, i->ifnot);
          break;
        case IK::Iswitch:
          for (Instr c : i->cases) prefer(weight, c);
          break;
        case IK::Icatch: {
          prefer(weight, i->body);
          long weight_h = weight;
          // Avoid overflow of weight and spill_cost
          if (i->rec == cmm::RecFlag::Recursive && weight < 1000) weight_h = 8 * weight;
          for (auto& h : i->handlers) prefer(weight_h, h.body);
          break;
        }
        case IK::Itrywith:
          prefer(weight, i->ifso);
          prefer(weight, i->ifnot);
          break;
      }
      i = i->next;
    }
  };

  interf(fundecl.fun_body);
  prefer(8, fundecl.fun_body);
}

// ---- Coloring: register allocation by coloring of the interference graph ----------------------
std::vector<long> coloring_allocate_registers() {
  // OrderedRegSet: constrained regs sorted by spill cost (highest first)
  struct OrderedLess {
    bool operator()(const Reg* r1, const Reg* r2) const {
      long c1 = r1->spill_cost, d1 = r1->degree;
      long c2 = r2->spill_cost, d2 = r2->degree;
      long n = c2 * d1 - c1 * d2;
      if (n != 0) return n < 0;
      n = c2 - c1;
      if (n != 0) return n < 0;
      n = d1 - d2;
      if (n != 0) return n < 0;
      return r1->stamp < r2->stamp;
    }
  };
  std::set<Reg*, OrderedLess> constrained;
  std::vector<Reg*> unconstrained;  // list order: the newest first
  std::vector<long> num_stack_slots(proc::num_register_classes, 0);

  // Preallocate the spilled registers in the stack.  Split the remaining
  // registers into constrained and unconstrained.
  auto remove_reg = [&](Reg* reg) {
    long cl = proc::register_class(reg);
    if (reg->spill) {
      // Preallocate the registers in the stack
      long nslots = num_stack_slots[cl];
      std::vector<bool> conflict(nslots, false);
      for (Reg* r : reg->interf)
        if (r->loc.k == LK::Local && proc::register_class(r) == cl) conflict[r->loc.n] = true;
      long slot = 0;
      while (slot < nslots && conflict[slot]) ++slot;
      reg->loc = {LK::Local, slot};
      if (slot >= nslots) num_stack_slots[cl] = slot + 1;
    } else if (reg->degree < proc::num_available_registers[cl]) {
      unconstrained.insert(unconstrained.begin(), reg);
    } else {
      constrained.insert(reg);
    }
  };

  // Iterate over all registers preferred by the given register (transitive)
  auto iter_preferred = [](const std::function<void(Reg*, long)>& f, Reg* reg) {
    std::function<void(Reg*, long)> walk = [&](Reg* r, long w) {
      if (reg::is_visited(r)) return;
      reg::mark_visited(r);
      f(r, w);
      for (auto& [r1, w1] : r->prefer) walk(r1, std::min(w, w1));
    };
    for (auto& [r, w] : reg->prefer) walk(r, w);
    reg::clear_visited_marks();
  };

  // Where to start the search for a suitable register.
  std::vector<long> start_register(proc::num_register_classes, 0);

  // Assign a location to a register, the best we can.
  auto assign_location = [&](Reg* reg) {
    long cl = proc::register_class(reg);
    long first_reg = proc::first_available_register[cl];
    long num_regs = proc::num_available_registers[cl];
    std::vector<long> score(num_regs, 0);
    long best_score = -1000000, best_reg = -1;
    long start = start_register[cl];
    if (num_regs != 0) {
      // Favor the registers that have been assigned to pseudoregs for which
      // we have a preference. If these pseudoregs have not been assigned
      // already, avoid the registers with which they conflict.
      iter_preferred(
          [&](Reg* r, long w) {
            if (r->loc.k == LK::Reg) {
              long n = r->loc.n - first_reg;
              if (n < num_regs) score[n] += w;
            } else if (r->loc.k == LK::Unknown) {
              for (Reg* neighbour : r->interf)
                if (neighbour->loc.k == LK::Reg) {
                  long n = neighbour->loc.n - first_reg;
                  if (n < num_regs) score[n] -= w;
                }
            }
          },
          reg);
      for (Reg* neighbour : reg->interf) {
        // Prohibit the registers that have been assigned to our neighbours
        if (neighbour->loc.k == LK::Reg) {
          long n = neighbour->loc.n - first_reg;
          if (n < num_regs) score[n] = -1000000;
        }
        // Avoid the registers that have been assigned to pseudoregs for
        // which our neighbours have a preference
        iter_preferred(
            [&](Reg* r, long w) {
              if (r->loc.k == LK::Reg) {
                long n = r->loc.n - first_reg;
                // w-1 to break the symmetry when two conflicting regs have
                // the same preference for a third reg.
                if (n < num_regs) score[n] -= w - 1;
              }
            },
            neighbour);
      }
      // Pick the register with the best score
      for (long n = start; n < num_regs; ++n)
        if (score[n] > best_score) {
          best_score = score[n];
          best_reg = n;
        }
      for (long n = 0; n < start; ++n)
        if (score[n] > best_score) {
          best_score = score[n];
          best_reg = n;
        }
    }
    // Found a register?
    if (best_reg >= 0) {
      reg->loc = {LK::Reg, first_reg + best_reg};
    } else {
      // Sorry, we must put the pseudoreg in a stack location
      long nslots = num_stack_slots[cl];
      std::vector<long> sscore(nslots, 0);
      // Compute the scores as for registers
      for (auto& [r, w] : reg->prefer) {
        if (r->loc.k == LK::Local) sscore[r->loc.n] += w;
        else if (r->loc.k == LK::Unknown)
          for (Reg* neighbour : r->interf)
            if (neighbour->loc.k == LK::Local) sscore[neighbour->loc.n] -= w;
      }
      for (Reg* neighbour : reg->interf) {
        if (neighbour->loc.k == LK::Local) sscore[neighbour->loc.n] = -1000000;
        for (auto& [r, w] : neighbour->prefer)
          if (r->loc.k == LK::Local) sscore[r->loc.n] -= w;
      }
      // Pick the location with the best score
      long bs = -1000000, best_slot = -1;
      for (long n = 0; n < nslots; ++n)
        if (sscore[n] > bs) {
          bs = sscore[n];
          best_slot = n;
        }
      // Mark this register as spilled so that we don't waste time trying to
      // put in in a register if we have to redo regalloc due to Reload
      reg->spill = true;
      if (best_slot >= 0) reg->loc = {LK::Local, best_slot};
      else {
        // Allocate a new stack slot
        reg->loc = {LK::Local, nslots};
        num_stack_slots[cl] = nslots + 1;
      }
    }
    // Cancel the preferences of this register so that they don't influence
    // transitively the allocation of registers that prefer this reg.
    reg->prefer.clear();
  };

  // First pass: preallocate spill registers and split remaining regs.
  // Second pass: assign locations to constrained regs.  Third pass: assign
  // locations to unconstrained regs.
  for (Reg* r : reg::all_registers()) remove_reg(r);
  for (Reg* r : constrained) assign_location(r);
  for (Reg* r : unconstrained) assign_location(r);
  return num_stack_slots;
}

// ---- Interval: live intervals for the linear scan register allocator ---------------------------
namespace {
using interval::Interval;
using interval::Range;

// Check if two intervals overlap
bool overlap(const Interval* i0, const Interval* i1) {
  std::size_t k0 = i0->first, k1 = i1->first;
  while (k0 < i0->ranges.size() && k1 < i1->ranges.size()) {
    const Range& r0 = i0->ranges[k0];
    const Range& r1 = i1->ranges[k1];
    if (r0.rend >= r1.rbegin && r1.rend >= r0.rbegin) return true;
    if (r0.rend < r1.rend) ++k0;
    else if (r0.rend > r1.rend) ++k1;
    else {
      ++k0;
      ++k1;
    }
  }
  return false;
}

bool is_live(const Interval* i, long pos) {
  for (std::size_t k = i->first; k < i->ranges.size(); ++k) {
    if (pos < i->ranges[k].rbegin) return false;
    if (pos <= i->ranges[k].rend) return true;
  }
  return false;
}

void remove_expired_ranges(Interval* i, long pos) {
  while (i->first < i->ranges.size() && !(pos < i->ranges[i->first].rend)) ++i->first;
}

enum class Kind { Result, Argument, Live };

std::vector<Interval> g_intervals;
interval::Result g_interval_result;

void update_interval_position(long pos, Kind kind, Reg* reg) {
  Interval& i = g_intervals[reg->stamp];
  long on = pos << 1;
  long off = on + 1;
  long rbegin = kind == Kind::Result ? off : on;
  long rend = kind == Kind::Argument ? on : off;
  if (i.iend == 0) {
    i.ibegin = rbegin;
    i.reg = reg;
    i.ranges = {{rbegin, rend}};
  } else {
    // the list's head: the newest range
    Range& r = i.ranges.back();
    long ridx = r.rend >> 1;
    if (pos - ridx <= 1) r.rend = rend;
    else i.ranges.push_back({rbegin, rend});
  }
  i.iend = rend;
}

void update_interval_position_by_array(const Regs& regs, long pos, Kind kind) {
  for (Reg* r : regs) update_interval_position(pos, kind, r);
}

void update_interval_position_by_instr(Instr i, long pos) {
  update_interval_position_by_array(i->arg, pos, Kind::Argument);
  update_interval_position_by_array(i->res, pos, Kind::Result);
  for (Reg* r : i->live) update_interval_position(pos, Kind::Live, r);
}

void insert_destroyed_at_oper(Instr i, long pos) {
  Regs destroyed = proc::destroyed_at_oper(*i);
  if (!destroyed.empty()) update_interval_position_by_array(destroyed, pos, Kind::Result);
}

void insert_destroyed_at_raise(long pos) {
  Regs destroyed = proc::destroyed_at_raise();
  if (!destroyed.empty()) update_interval_position_by_array(destroyed, pos, Kind::Result);
}
}  // namespace

// Build all intervals.  The intervals will be expanded by one step at the
// start and end of a basic block.
const interval::Result& build_intervals(const Fundecl& fd) {
  g_intervals.assign(static_cast<std::size_t>(reg::num_registers()), Interval{});
  long pos = 0;
  std::function<void(Instr)> walk_instruction = [&](Instr i) {
    for (;;) {
      ++pos;
      update_interval_position_by_instr(i, pos);
      switch (i->desc) {
        case IK::Iend: return;
        case IK::Iop:
          if (!(i->op.k == MK::Icall_ind || i->op.k == MK::Icall_imm || (i->op.k == MK::Iextcall && i->op.alloc) ||
                i->op.k == MK::Itailcall_ind || i->op.k == MK::Itailcall_imm))
            insert_destroyed_at_oper(i, pos);
          break;
        case IK::Ireturn: insert_destroyed_at_oper(i, pos); break;
        case IK::Iifthenelse:
          insert_destroyed_at_oper(i, pos);
          walk_instruction(i->ifso);
          walk_instruction(i->ifnot);
          break;
        case IK::Iswitch:
          insert_destroyed_at_oper(i, pos);
          for (Instr c : i->cases) walk_instruction(c);
          break;
        case IK::Icatch:
          insert_destroyed_at_oper(i, pos);
          for (auto& h : i->handlers) walk_instruction(h.body);
          walk_instruction(i->body);
          break;
        case IK::Iexit: insert_destroyed_at_oper(i, pos); break;
        case IK::Itrywith:
          insert_destroyed_at_oper(i, pos);
          walk_instruction(i->ifso);
          insert_destroyed_at_raise(pos);
          walk_instruction(i->ifnot);
          break;
        case IK::Iraise: break;
      }
      i = i->next;
    }
  };
  walk_instruction(fd.fun_body);
  // Generate the interval and fixed interval lists (built by consing, so
  // the highest stamp first; the ranges are already oldest first)
  interval::Result& r = g_interval_result;
  r.intervals.clear();
  r.fixed_intervals.clear();
  for (std::size_t k = g_intervals.size(); k-- > 0;) {
    Interval* i = &g_intervals[k];
    if (i->iend != 0) {
      if (i->reg->loc.k == LK::Reg) r.fixed_intervals.push_back(i);
      else r.intervals.push_back(i);
    }
  }
  // Sort the intervals according to their start position (List.sort: stable)
  std::stable_sort(r.intervals.begin(), r.intervals.end(),
                   [](const Interval* a, const Interval* b) { return a->ibegin < b->ibegin; });
  return r;
}

// ---- Linscan: linear scan register allocation ------------------------------------------------
namespace {
struct IntervalLess {
  bool operator()(const Interval* i, const Interval* j) const {
    if (i->iend != j->iend) return i->iend < j->iend;
    return i->reg->stamp < j->reg->stamp;
  }
};
using IntervalSet = std::set<Interval*, IntervalLess>;

// Live intervals per register class
struct ClassIntervals {
  IntervalSet ci_fixed;
  IntervalSet ci_active;
  IntervalSet ci_inactive;
  IntervalSet ci_spilled;  // spilled stack slots (reg.loc = Stack (Local n)) still in use
  std::set<long> ci_free_slots;  // expired stack slots available for reuse
};

// split_by_pos: the intervals ending before [pos] leave [s] (returned)
IntervalSet take_expired(IntervalSet& s, long pos) {
  IntervalSet expired;
  while (!s.empty() && (*s.begin())->iend < pos) expired.insert(s.extract(s.begin()));
  return expired;
}

void remove_expired_ranges(const IntervalSet& s, long pos) {
  for (Interval* i : s) remove_expired_ranges(i, pos);
}

bool same_loc(const reg::Location& a, const reg::Location& b);  // Reload's

struct Linscan {
  ClassIntervals active[proc::num_register_classes];
  std::vector<long> num_stack_slots = std::vector<long>(proc::num_register_classes, 0);

  void release_expired_spilled(ClassIntervals& ci, long pos) {
    for (Interval* i : take_expired(ci.ci_spilled, pos)) ci.ci_free_slots.insert(i->reg->loc.n);
  }
  void release_expired_fixed(ClassIntervals& ci, long pos) {
    take_expired(ci.ci_fixed, pos);
    remove_expired_ranges(ci.ci_fixed, pos);
  }
  void release_expired_active(ClassIntervals& ci, long pos) {
    take_expired(ci.ci_active, pos);
    remove_expired_ranges(ci.ci_active, pos);
    for (auto it = ci.ci_active.begin(); it != ci.ci_active.end();) {
      if (is_live(*it, pos)) ++it;
      else ci.ci_inactive.insert(ci.ci_active.extract(it++));
    }
  }
  void release_expired_inactive(ClassIntervals& ci, long pos) {
    take_expired(ci.ci_inactive, pos);
    remove_expired_ranges(ci.ci_inactive, pos);
    for (auto it = ci.ci_inactive.begin(); it != ci.ci_inactive.end();) {
      if (is_live(*it, pos)) ci.ci_active.insert(ci.ci_inactive.extract(it++));
      else ++it;
    }
  }

  // Allocate a stack slot to the interval.  [existing] indicates whether we
  // are spilling an already allocated interval (which requires a new stack
  // slot) or spilling a new interval, in which case we may reuse an expired
  // stack slot.
  void allocate_stack_slot(bool existing, Interval* i) {
    long cl = proc::register_class(i->reg);
    ClassIntervals& ci = active[cl];
    long ss;
    if (!existing && !ci.ci_free_slots.empty()) {
      ss = *ci.ci_free_slots.begin();
      ci.ci_free_slots.erase(ci.ci_free_slots.begin());
    } else {
      ss = num_stack_slots[cl];
      num_stack_slots[cl] = ss + 1;
    }
    i->reg->loc = {LK::Local, ss};
    i->reg->spill = true;
    ci.ci_spilled.insert(i);
  }

  // Find a register for the given interval and assigns this register.  The
  // interval is added to active.  false (Not_found) if no free registers left.
  bool allocate_free_register(Interval* i) {
    if (i->reg->loc.k != LK::Unknown) return true;
    if (i->reg->spill) {
      // Allocate a stack slot for the already spilled interval
      allocate_stack_slot(false, i);
      return true;
    }
    // We need to allocate a register to this interval somehow
    long cl = proc::register_class(i->reg);
    long rn = proc::num_available_registers[cl];
    if (rn == 0) return false;  // There are no registers available for this class
    ClassIntervals& ci = active[cl];
    long r0 = proc::first_available_register[cl];
    // Create register mask for this class (with frame pointers, some
    // registers may have indexes that are off-bounds)
    std::vector<bool> regmask(rn, true);
    // Remove all assigned registers from the register mask
    for (Interval* j : ci.ci_active)
      if (j->reg->loc.k == LK::Reg) {
        long r = j->reg->loc.n;
        if (r - r0 < rn) regmask.at(r - r0) = false;
      }
    // Remove all overlapping registers from the register mask
    auto remove_bound_overlapping = [&](Interval* j) {
      if (j->reg->loc.k == LK::Reg) {
        long r = j->reg->loc.n;
        if (r - r0 < rn && regmask.at(r - r0) && overlap(j, i)) regmask[r - r0] = false;
      }
    };
    for (Interval* j : ci.ci_inactive) remove_bound_overlapping(j);
    for (Interval* j : ci.ci_fixed) remove_bound_overlapping(j);
    // Assign the first free register (if any)
    for (long r = 0; r < rn; ++r)
      if (regmask[r]) {
        // Assign the free register and insert the current interval into
        // the active list
        i->reg->loc = {LK::Reg, r0 + r};
        i->reg->spill = false;
        ci.ci_active.insert(i);
        return true;
      }
    return false;
  }

  void allocate_blocked_register(Interval* i) {
    long cl = proc::register_class(i->reg);
    ClassIntervals& ci = active[cl];
    if (!ci.ci_active.empty()) {
      Interval* ilast = *ci.ci_active.rbegin();
      auto chk = [&](Interval* r) { return same_loc(r->reg->loc, ilast->reg->loc) && overlap(r, i); };
      // Last interval in active is the last interval, so spill it -- but
      // only if its physical register is admissible for the current interval
      if (ilast->iend > i->iend && !(std::any_of(ci.ci_fixed.begin(), ci.ci_fixed.end(), chk) ||
                                     std::any_of(ci.ci_inactive.begin(), ci.ci_inactive.end(), chk))) {
        ci.ci_active.erase(ilast);
        if (ilast->reg->loc.k != LK::Reg) throw std::logic_error("Linscan.allocate_blocked_register");
        // Use register from last interval for current interval
        i->reg->loc = ilast->reg->loc;
        // Remove the last interval from active and insert the current
        ci.ci_active.insert(i);
        // Now get a new stack slot for the spilled register
        allocate_stack_slot(true, ilast);
        return;
      }
    }
    // Either the current interval is last and we have to spill it, or there
    // are no registers at all in the register class
    allocate_stack_slot(false, i);
  }

  void walk_interval(Interval* i) {
    long pos = i->ibegin & ~0x01L;
    // Release all intervals that have been expired at the current position
    for (ClassIntervals& ci : active) {
      release_expired_fixed(ci, pos);
      release_expired_active(ci, pos);
      release_expired_inactive(ci, pos);
      release_expired_spilled(ci, pos);
    }
    // Allocate free register (if any); else decide which interval to spill
    if (!allocate_free_register(i)) allocate_blocked_register(i);
  }
};
}  // namespace

std::vector<long> linscan_allocate_registers(const interval::Result& intervals) {
  Linscan ls;
  // Add all fixed intervals (sorted by end position)
  for (Interval* i : intervals.fixed_intervals) ls.active[proc::register_class(i->reg)].ci_fixed.insert(i);
  // Walk all the intervals within the list
  for (Interval* i : intervals.intervals) ls.walk_interval(i);
  return ls.num_stack_slots;
}

// ---- Reload: insert load/stores for pseudoregs that got assigned to stack locations ----------
namespace {

bool stackp(const Reg* r) {
  return r->loc.k == LK::Local || r->loc.k == LK::Incoming || r->loc.k == LK::Outgoing || r->loc.k == LK::Domainstate;
}
bool same_loc(const reg::Location& a, const reg::Location& b) { return a.k == b.k && a.n == b.n; }

Instr insert_move_r(Reg* src, Reg* dst, Instr next) {
  if (same_loc(src->loc, dst->loc)) return next;
  return cons(iop(mop(MK::Imove)), {src}, {dst}, next);
}
Instr insert_moves_r(const Regs& src, const Regs& dst, Instr next) {
  // insmoves i = insert_move src.(i) dst.(i) (insmoves (i+1))
  for (std::size_t k = src.size(); k-- > 0;) next = insert_move_r(src[k], dst[k], next);
  return next;
}

struct Reloader {
  bool redo_regalloc = false;

  Reg* makereg(Reg* r) {
    if (r->loc.k == LK::Unknown) fatal("Reload.makereg");
    if (r->loc.k == LK::Reg) return r;
    redo_regalloc = true;
    Reg* newr = reg::clone(r);
    // Strongly discourage spilling this register
    newr->spill_cost = 100000;
    return newr;
  }
  Regs makeregs(const Regs& rv) {
    Regs nv;
    for (Reg* r : rv) nv.push_back(makereg(r));
    return nv;
  }
  Regs makereg1(const Regs& rv) {
    Regs nv = rv;
    nv[0] = makereg(rv[0]);
    return nv;
  }

  // reload_generic: all arguments and results in hardware registers,
  // except for moves
  std::pair<Regs, Regs> reload_operation_generic(const mach::Operation& op, const Regs& arg, const Regs& res) {
    if (op.k == MK::Imove || op.k == MK::Ireload || op.k == MK::Ispill) {
      if (stackp(arg[0]) && stackp(res[0]) && !same_loc(arg[0]->loc, res[0]->loc)) return {{makereg(arg[0])}, res};
      return {arg, res};
    }
    if (op.k == MK::Iopaque) return {arg, res};  // arg = result, can be on stack or register
    // (makeregs arg, makeregs res): right to left
    Regs r = makeregs(res);
    Regs a = makeregs(arg);
    return {a, r};
  }

  // amd64's reload_operation
  std::pair<Regs, Regs> reload_operation(const mach::Operation& op, const Regs& arg, const Regs& res) {
    switch (op.k) {
      case MK::Iintop:
        switch (op.intop.op) {
          case IO::Iadd:
          case IO::Isub:
          case IO::Iand:
          case IO::Ior:
          case IO::Ixor:
          case IO::Icheckbound:
            // One of the two arguments can reside in the stack, but not both
            if (stackp(arg[0]) && stackp(arg[1])) return {{arg[0], makereg(arg[1])}, res};
            return {arg, res};
          case IO::Icomp: {
            // The result must be a register (PR#11803)
            Regs res2 = makeregs(res);
            if (stackp(arg[0]) && stackp(arg[1])) return {{arg[0], makereg(arg[1])}, res2};
            return {arg, res2};
          }
          case IO::Imul:
            // First argument (= result) must be in register, second arg can
            // reside in the stack
            if (stackp(arg[0])) {
              Reg* r = makereg(arg[0]);
              return {{r, arg[1]}, {r}};
            }
            return {arg, res};
          default:  // Imulh, Idiv, Imod, Ilsl, Ilsr, Iasr
            return {arg, res};
        }
      case MK::Iintop_imm:
        if (op.intop.op == IO::Iadd && !same_loc(arg[0]->loc, res[0]->loc))
          // This add will be turned into a lea; args and results must be in
          // registers
          return reload_operation_generic(op, arg, res);
        if (op.intop.op == IO::Imul) {
          // The result (= the argument) must be a register (#10626)
          if (stackp(arg[0])) {
            Reg* r = makereg(arg[0]);
            return {{r}, {r}};
          }
          return {arg, res};
        }
        if (op.intop.op == IO::Icomp) return {arg, makeregs(res)};  // The result must be in a register (PR#11803)
        return {arg, res};
      case MK::Iaddf:
      case MK::Isubf:
      case MK::Imulf:
      case MK::Idivf:
        if (stackp(arg[0])) {
          Reg* r = makereg(arg[0]);
          return {{r, arg[1]}, {r}};
        }
        return {arg, res};
      case MK::Ifloatofint:
      case MK::Iintoffloat:
        // Result must be in register, but argument can be on stack
        return {arg, stackp(res[0]) ? Regs{makereg(res[0])} : res};
      case MK::Iconst_int:
        if (op.n <= 0x7FFFFFFFLL && op.n >= -0x80000000LL) return {arg, res};
        return reload_operation_generic(op, arg, res);
      case MK::Iconst_symbol:
        if (clflags::pic_code || clflags::dlcode) return reload_operation_generic(op, arg, res);
        return {arg, res};
      default:  // Other operations: all args and results in registers
        return reload_operation_generic(op, arg, res);
    }
  }

  Regs reload_test(const Test& tst, const Regs& arg) {
    using FC = lambda::FloatComparison;
    switch (tst.k) {
      case Test::K::Iinttest:
        // One of the two arguments can reside on stack
        if (stackp(arg[0]) && stackp(arg[1])) return {makereg(arg[0]), arg[1]};
        return arg;
      case Test::K::Ifloattest:
        if (tst.fcmp == FC::CFlt || tst.fcmp == FC::CFnlt || tst.fcmp == FC::CFle || tst.fcmp == FC::CFnle) {
          // Cf. emit.mlp: we swap arguments in this case.  First argument
          // can be on stack, second must be in register
          if (stackp(arg[1])) return {arg[0], makereg(arg[1])};
          return arg;
        }
        // Second argument can be on stack, first must be in register
        if (stackp(arg[0])) return {makereg(arg[0]), arg[1]};
        return arg;
      default: return arg;  // The argument(s) can be either in register or on stack
    }
  }

  Instr reload(Instr i) {
    switch (i->desc) {
      case IK::Iend:
      case IK::Ireturn:
      case IK::Iraise: return i;
      case IK::Iop: {
        const mach::Operation& op = i->op;
        if (op.k == MK::Itailcall_imm) return i;
        if (op.k == MK::Itailcall_ind) {
          Regs newarg = makereg1(i->arg);
          Instr c = copy(i);
          c->arg = newarg;
          return insert_moves_r(i->arg, newarg, c);
        }
        if (op.k == MK::Icall_imm || op.k == MK::Iextcall) {
          Instr next = reload(i->next);
          Instr c = copy(i);
          c->next = next;
          return c;
        }
        if (op.k == MK::Icall_ind) {
          Regs newarg = makereg1(i->arg);
          Instr next = reload(i->next);
          Instr c = copy(i);
          c->arg = newarg;
          c->next = next;
          return insert_moves_r(i->arg, newarg, c);
        }
        auto [newarg, newres] = reload_operation(op, i->arg, i->res);
        // {i with arg = newarg; res = newres; next = insert_moves newres
        // i.res (reload i.next)}: the next first
        Instr next = insert_moves_r(newres, i->res, reload(i->next));
        Instr c = copy(i);
        c->arg = newarg;
        c->res = newres;
        c->next = next;
        return insert_moves_r(i->arg, newarg, c);
      }
      case IK::Iifthenelse: {
        Regs newarg = reload_test(i->test, i->arg);
        // instr_cons (Iifthenelse(tst, reload ifso, reload ifnot)) newarg
        // [||] (reload i.next): right to left
        Instr next = reload(i->next);
        Instr ifnot = reload(i->ifnot);
        Instr ifso = reload(i->ifso);
        Instruction d = idesc(IK::Iifthenelse);
        d.test = i->test;
        d.ifso = ifso;
        d.ifnot = ifnot;
        return insert_moves_r(i->arg, newarg, cons(d, newarg, {}, next));
      }
      case IK::Iswitch: {
        Regs newarg = makeregs(i->arg);
        Instr next = reload(i->next);
        Instruction d = idesc(IK::Iswitch);
        d.index = i->index;
        for (Instr c : i->cases) d.cases.push_back(reload(c));
        return insert_moves_r(i->arg, newarg, cons(d, newarg, {}, next));
      }
      case IK::Icatch: {
        Instruction d = idesc(IK::Icatch);
        d.rec = i->rec;
        for (auto& h : i->handlers) d.handlers.push_back({h.n, reload(h.body)});
        Instr next = reload(i->next);
        d.body = reload(i->body);
        return cons(d, {}, {}, next);
      }
      case IK::Iexit: {
        Instruction d = idesc(IK::Iexit);
        d.nfail = i->nfail;
        return cons(d, {}, {}, dummy_instr());
      }
      case IK::Itrywith: {
        Instr next = reload(i->next);
        Instr handler = reload(i->ifnot);
        Instr body = reload(i->ifso);
        Instruction d = idesc(IK::Itrywith);
        d.ifso = body;
        d.ifnot = handler;
        return cons(d, {}, {}, next);
      }
    }
    return i;
  }
};

}  // namespace

std::pair<mach::Fundecl, bool> reload(const mach::Fundecl& f, const std::vector<long>& num_stack_slots) {
  Reloader r;
  Instr new_body = r.reload(f.fun_body);
  mach::Fundecl nf = f;
  nf.fun_body = new_body;
  nf.fun_num_stack_slots = num_stack_slots;
  return {nf, r.redo_regalloc};
}

}  // namespace cppcaml::typing::mach_passes
