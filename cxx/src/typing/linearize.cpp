// Ports of asmcomp/linear.ml, linearize.ml, printlinear.ml and
// stackframegen.ml + amd64/stackframe.ml.  See linear.hpp.
#include "cppcaml/typing/linear.hpp"

#include <functional>
#include <stdexcept>

#include "cppcaml/typing/clflags.hpp"

namespace cppcaml::typing::linear {

using K = Instruction::K;
using MK = mach::Operation::K;
using IK = mach::Instruction::K;
using mach::Test;

namespace {
[[noreturn]] void fatal(const std::string& s) { throw std::runtime_error(s); }
}  // namespace

bool has_fallthrough(const Instruction& i) {
  switch (i.desc) {
    case K::Lreturn:
    case K::Lbranch:
    case K::Lswitch:
    case K::Lraise: return false;
    case K::Lop: return !(i.op->k == MK::Itailcall_ind || i.op->k == MK::Itailcall_imm);
    default: return true;
  }
}

// Invert a test
Test invert_test(const Test& t) {
  Test r = t;
  switch (t.k) {
    case Test::K::Itruetest: r.k = Test::K::Ifalsetest; break;
    case Test::K::Ifalsetest: r.k = Test::K::Itruetest; break;
    case Test::K::Iinttest:
    case Test::K::Iinttest_imm: r.icmp.c = lambda::negate_integer_comparison(t.icmp.c); break;
    case Test::K::Ifloattest: r.fcmp = lambda::negate_float_comparison(t.fcmp); break;
    case Test::K::Ieventest: r.k = Test::K::Ioddtest; break;
    case Test::K::Ioddtest: r.k = Test::K::Ieventest; break;
  }
  return r;
}

// The "end" instruction
Instr end_instr() {
  static Instr e = [] {
    auto* i = new Instruction{K::Lend};
    i->next = i;
    return i;
  }();
  return e;
}

namespace {

Instruction desc(K k) { return Instruction{k}; }

// Cons a simple instruction (arg, res, live empty)
Instr cons_instr(const Instruction& d, Instr n) {
  auto* i = make<Instruction>(d);
  i->next = n;
  return i;
}

// Build an instruction with arg, res, dbg, live taken from the given
// Mach.instruction
Instr copy_instr(const Instruction& d, mach::Instr i, Instr n) {
  auto* x = make<Instruction>(d);
  x->next = n;
  x->arg = i->arg;
  x->res = i->res;
  x->dbg = i->dbg;
  x->live = i->live;
  return x;
}

Instruction label_d(Label l) {
  Instruction d = desc(K::Llabel);
  d.lbl = l;
  return d;
}
Instruction branch_d(Label l) {
  Instruction d = desc(K::Lbranch);
  d.lbl = l;
  return d;
}
Instruction condbranch_d(const Test& t, Label l) {
  Instruction d = desc(K::Lcondbranch);
  d.test = t;
  d.lbl = l;
  return d;
}
Instruction op_d(const mach::Operation* op) {
  Instruction d = desc(K::Lop);
  d.op = op;
  return d;
}

// Label the beginning of the given instruction sequence.  If the sequence
// starts with a branch, jump over it.  If the sequence is the end, (tail
// call position), just do nothing
std::pair<Label, Instr> get_label(Instr n) {
  switch (n->desc) {
    case K::Lbranch:
    case K::Llabel: return {n->lbl, n};
    case K::Lend: return {-1, n};
    default: {
      Label lbl = cmm::new_label();
      return {lbl, cons_instr(label_d(lbl), n)};
    }
  }
}

// Check the fallthrough label
Label check_label(Instr n) { return n->desc == K::Lbranch || n->desc == K::Llabel ? n->lbl : -1; }

// Add pseudo-instruction Ladjust_trap_depth in front of a continuation
Instr adjust_trap_depth(long delta_traps, Instr next) {
  while (next->desc == K::Ladjust_trap_depth) {
    delta_traps += next->delta_traps;
    next = next->next;
  }
  if (delta_traps == 0) return next;
  Instruction d = desc(K::Ladjust_trap_depth);
  d.delta_traps = delta_traps;
  return cons_instr(d, next);
}

// Discard all instructions up to the next label.
Instr discard_dead_code(Instr n) {
  auto adjust = [&](long trap_depth) { return adjust_trap_depth(trap_depth, discard_dead_code(n->next)); };
  switch (n->desc) {
    case K::Lend:
    case K::Llabel: return n;
    // Do not discard Lpoptrap/Lpushtrap/Ladjust_trap_depth or Istackoffset
    // instructions, as this may cause a stack imbalance later during
    // assembler generation.
    case K::Lpoptrap: return adjust(-1);
    case K::Lpushtrap: return adjust(+1);
    case K::Ladjust_trap_depth: return adjust(n->delta_traps);
    case K::Lop:
      if (n->op->k == MK::Istackoffset) {
        Instr c = make<Instruction>(*n);
        c->next = discard_dead_code(n->next);
        return c;
      }
      return discard_dead_code(n->next);
    default: return discard_dead_code(n->next);
  }
}

// Add a branch in front of a continuation.  Discard dead code in the
// continuation.  Does not insert anything if we're just falling through or
// if we jump to dead code after the end of function (lbl=-1)
Instr add_branch(Label lbl, Instr n) {
  if (lbl >= 0) {
    Instr n1 = discard_dead_code(n);
    if (n1->desc == K::Llabel && n1->lbl == lbl) return n1;
    return cons_instr(branch_d(lbl), n1);
  }
  return discard_dead_code(n);
}

struct ExitInfo {
  long try_depth = 0;
  // Association list: exit handler -> (handler label, try-nesting factor)
  std::vector<std::pair<long, std::pair<Label, long>>> exit_label;  // list order: the newest first
};

std::pair<Label, long> find_exit_label_try_depth(const ExitInfo& e, long k) {
  for (auto& [n, v] : e.exit_label)
    if (n == k) return v;
  fatal("Linearize.find_exit_label");
}
Label find_exit_label(const ExitInfo& e, long k) {
  auto [label, t] = find_exit_label_try_depth(e, k);
  if (t != e.try_depth) fatal("Linearize.find_exit_label");
  return label;
}
bool is_next_catch(const ExitInfo& e, long n) {
  return !e.exit_label.empty() && e.exit_label[0].first == n && e.exit_label[0].second.second == e.try_depth;
}
bool local_exit(const ExitInfo& e, long k) { return find_exit_label_try_depth(e, k).second == e.try_depth; }

bool same_loc(const reg::Location& a, const reg::Location& b) { return a.k == b.k && a.n == b.n; }

// Linearize an instruction [i]: add it in front of the continuation [n]
Instr linear(const ExitInfo& exit_info, mach::Instr i, Instr n, bool contains_calls) {
  auto lin = [&](const ExitInfo& e, mach::Instr x, Instr m) { return linear(e, x, m, contains_calls); };
  switch (i->desc) {
    case IK::Iend: return n;
    case IK::Iop: {
      const mach::Operation& op = *i->op;
      if (op.k == MK::Itailcall_ind || op.k == MK::Itailcall_imm) return copy_instr(op_d(i->op), i, discard_dead_code(n));
      if ((op.k == MK::Imove || op.k == MK::Ireload || op.k == MK::Ispill) && same_loc(i->arg[0]->loc, i->res[0]->loc))
        return lin(exit_info, i->next, n);
      if (op.k == MK::Ipoll && !op.return_label) {
        // If the poll call does not already specify where to jump to after
        // the poll, absorb any branch after the poll call into the poll call
        // itself.
        Instr n2 = lin(exit_info, i->next, n);
        mach::Operation op2 = op;
        if (n2->desc == K::Lbranch) {
          op2.return_label = n2->lbl;
          n2 = n2->next;
        }
        return copy_instr(op_d(mach::op_ref(op2)), i, n2);
      }
      return copy_instr(op_d(i->op), i, lin(exit_info, i->next, n));
    }
    case IK::Ireturn: {
      Instr n1 = copy_instr(desc(K::Lreturn), i, discard_dead_code(n));
      if (contains_calls) return cons_instr(desc(K::Lreloadretaddr), n1);
      return n1;
    }
    case IK::Iifthenelse: {
      const Test& test = i->test;
      mach::Instr ifso = i->ifso, ifnot = i->ifnot;
      Instr n1 = lin(exit_info, i->next, n);
      if (ifso->desc == IK::Iend && n1->desc == K::Lbranch)
        return copy_instr(condbranch_d(test, n1->lbl), i, lin(exit_info, ifnot, n1));
      if (ifnot->desc == IK::Iend && n1->desc == K::Lbranch)
        return copy_instr(condbranch_d(invert_test(test), n1->lbl), i, lin(exit_info, ifso, n1));
      if (ifso->desc == IK::Iexit && ifnot->desc == IK::Iexit && is_next_catch(exit_info, ifso->nfail) &&
          local_exit(exit_info, ifnot->nfail)) {
        Label lbl2 = find_exit_label(exit_info, ifnot->nfail);
        return copy_instr(condbranch_d(invert_test(test), lbl2), i, lin(exit_info, ifso, n1));
      }
      if (ifso->desc == IK::Iexit && local_exit(exit_info, ifso->nfail)) {
        Instr n2 = lin(exit_info, ifnot, n1);
        Label lbl = find_exit_label(exit_info, ifso->nfail);
        return copy_instr(condbranch_d(test, lbl), i, n2);
      }
      if (ifnot->desc == IK::Iexit && local_exit(exit_info, ifnot->nfail)) {
        Instr n2 = lin(exit_info, ifso, n1);
        Label lbl = find_exit_label(exit_info, ifnot->nfail);
        return copy_instr(condbranch_d(invert_test(test), lbl), i, n2);
      }
      if (ifso->desc == IK::Iend) {
        auto [lbl_end, n2] = get_label(n1);
        return copy_instr(condbranch_d(test, lbl_end), i, lin(exit_info, ifnot, n2));
      }
      if (ifnot->desc == IK::Iend) {
        auto [lbl_end, n2] = get_label(n1);
        return copy_instr(condbranch_d(invert_test(test), lbl_end), i, lin(exit_info, ifso, n2));
      }
      // Should attempt branch prediction here
      auto [lbl_end, n2] = get_label(n1);
      auto [lbl_else, nelse] = get_label(lin(exit_info, ifnot, n2));
      return copy_instr(condbranch_d(invert_test(test), lbl_else), i, lin(exit_info, ifso, add_branch(lbl_end, nelse)));
    }
    case IK::Iswitch: {
      std::vector<Label> lbl_cases(i->cases.size(), 0);
      auto [lbl_end, n1] = get_label(lin(exit_info, i->next, n));
      Instr n2 = discard_dead_code(n1);
      for (std::size_t k = i->cases.size(); k-- > 0;) {
        Instr case_linear = lin(exit_info, i->cases[k], add_branch(lbl_end, n2));
        auto [lbl_case, ncase] = get_label(case_linear);
        lbl_cases[k] = lbl_case;
        n2 = discard_dead_code(ncase);
      }
      // Switches with 1 and 2 branches have been eliminated earlier.  Here,
      // we do something for switches with 3 branches.
      if (i->index.size() == 3) {
        Label fallthrough_lbl = check_label(n2);
        auto find_label = [&](std::size_t k) -> std::optional<Label> {
          Label lbl = lbl_cases[i->index[k]];
          if (lbl == fallthrough_lbl) return std::nullopt;
          return lbl;
        };
        Instruction d = desc(K::Lcondbranch3);
        d.lbl0 = find_label(0);
        d.lbl1 = find_label(1);
        d.lbl2 = find_label(2);
        return copy_instr(d, i, n2);
      }
      Instruction d = desc(K::Lswitch);
      for (long x : i->index) d.lbls.push_back(lbl_cases[x]);
      return copy_instr(d, i, n2);
    }
    case IK::Icatch: {
      auto [lbl_end, n1] = get_label(lin(exit_info, i->next, n));
      std::vector<Label> labels_at_entry_to_handlers;
      for (auto& h : i->handlers)
        labels_at_entry_to_handlers.push_back(h.body->desc == IK::Iend ? lbl_end : cmm::new_label());
      ExitInfo e2 = exit_info;
      std::vector<std::pair<long, std::pair<Label, long>>> add;
      for (std::size_t k = 0; k < i->handlers.size(); ++k)
        add.push_back({i->handlers[k].n, {labels_at_entry_to_handlers[k], exit_info.try_depth}});
      e2.exit_label.insert(e2.exit_label.begin(), add.begin(), add.end());
      Instr n2 = n1;
      for (std::size_t k = 0; k < i->handlers.size(); ++k) {
        mach::Instr handler = i->handlers[k].body;
        if (handler->desc == IK::Iend) continue;
        n2 = cons_instr(label_d(labels_at_entry_to_handlers[k]), lin(e2, handler, add_branch(lbl_end, n2)));
      }
      return lin(e2, i->body, add_branch(lbl_end, n2));
    }
    case IK::Iexit: {
      auto [lbl, t] = find_exit_label_try_depth(exit_info, i->nfail);
      long delta_traps = exit_info.try_depth - t;
      Instr n1 = adjust_trap_depth(delta_traps, n);
      Instr r = add_branch(lbl, n1);
      for (long tt = exit_info.try_depth; tt != t; --tt) r = cons_instr(desc(K::Lpoptrap), r);
      return r;
    }
    case IK::Itrywith: {
      auto [lbl_join, n1] = get_label(lin(exit_info, i->next, n));
      auto [lbl_handler, n2] = get_label(cons_instr(desc(K::Lentertrap), lin(exit_info, i->ifnot, n1)));
      ExitInfo e2 = exit_info;
      e2.try_depth = exit_info.try_depth + 1;
      Instruction push = desc(K::Lpushtrap);
      push.lbl = lbl_handler;
      return cons_instr(push, lin(e2, i->ifso, cons_instr(desc(K::Lpoptrap), add_branch(lbl_join, n2))));
    }
    case IK::Iraise: {
      Instruction d = desc(K::Lraise);
      d.raise = i->raise;
      return copy_instr(d, i, discard_dead_code(n));
    }
  }
  return n;
}

// ---- Stackframe (amd64) ----
struct Analysis {
  bool contains_nontail_calls;
  bool frame_required;
  long extra_stack_used;
};

bool is_call(const mach::Instruction& i) {
  using IO = mach::IntegerOperation;
  switch (i.desc) {
    case IK::Iop:
      switch (i.op->k) {
        case MK::Icall_ind:
        case MK::Icall_imm:
        case MK::Iextcall:
        case MK::Ialloc:
        case MK::Ipoll: return true;
        // (amd64) caml_ml_array_bound_error
        case MK::Iintop:
        case MK::Iintop_imm: return i.op->intop.op == IO::Icheckbound;
        default: return false;
      }
    case IK::Iraise: return i.raise != lambda::RaiseKind::Raise_notrace;
    case IK::Itrywith: return true;
    default: return false;
  }
}

Analysis analyze(const mach::Fundecl& f) {
  constexpr long trap_handler_size = 16;
  bool contains_nontail_calls = false, contains_calls = false;
  long extra_space = 0;
  std::function<void(long, mach::Instr)> an = [&](long sp, mach::Instr i) {
    for (;;) {
      if (sp > extra_space) extra_space = sp;
      contains_calls = contains_calls || is_call(*i);
      switch (i->desc) {
        case IK::Iend:
        case IK::Ireturn:
        case IK::Iexit:
        case IK::Iraise: return;
        case IK::Iop:
          if (i->op->k == MK::Istackoffset) sp += i->op->n;
          else if (i->op->k == MK::Itailcall_ind || i->op->k == MK::Itailcall_imm) return;
          else if (i->op->k == MK::Icall_ind || i->op->k == MK::Icall_imm) contains_nontail_calls = true;
          break;
        case IK::Iifthenelse:
          an(sp, i->ifso);
          an(sp, i->ifnot);
          break;
        case IK::Iswitch:
          for (mach::Instr c : i->cases) an(sp, c);
          break;
        case IK::Icatch:
          for (auto& h : i->handlers) an(sp, h.body);
          an(sp, i->body);
          break;
        case IK::Itrywith:
          an(sp + trap_handler_size, i->ifso);
          an(sp, i->ifnot);
          break;
      }
      i = i->next;
    }
  };
  an(0, f.fun_body);
  // Config.with_frame_pointers = false
  bool frame_required = contains_calls || f.fun_num_stack_slots[0] > 0 || f.fun_num_stack_slots[1] > 0;
  return {contains_nontail_calls, frame_required, extra_space};
}

}  // namespace

Fundecl linearize(const mach::Fundecl& f) {
  Analysis fa = analyze(f);
  Instr first_insn = linear(ExitInfo{}, f.fun_body, end_instr(), fa.frame_required);
  // add_prologue
  Label tailrec_entry_point_label = cmm::new_label();
  Instr tailrec_entry_point = cons_instr(label_d(tailrec_entry_point_label), first_insn);
  tailrec_entry_point->dbg = first_insn->dbg;
  tailrec_entry_point->live = first_insn->live;
  Instr body = tailrec_entry_point;
  if (fa.frame_required) {
    body = cons_instr(desc(K::Lprologue), tailrec_entry_point);
    body->dbg = tailrec_entry_point->dbg;
  }
  Fundecl r;
  r.fun_name = f.fun_name;
  r.fun_args = reg::Set(f.fun_args.begin(), f.fun_args.end());
  r.fun_body = body;
  r.fun_fast = true;
  for (auto o : f.fun_codegen_options)
    if (o == cmm::CodegenOption::Reduce_code_size) r.fun_fast = false;
  r.fun_dbg = f.fun_dbg;
  r.fun_tailrec_entry_point_label = tailrec_entry_point_label;
  r.fun_contains_nontail_calls = fa.contains_nontail_calls;
  r.fun_num_stack_slots = f.fun_num_stack_slots;
  r.fun_frame_required = fa.frame_required;
  r.fun_extra_stack_used = fa.extra_stack_used;
  return r;
}

// ---- Printlinear ----
namespace {
using format::Formatter;
using format::fprintf;
using format::pr;

void label(Formatter& ppf, Label l) { fprintf(ppf, "L%i", l); }

void instr(Formatter& ppf, Instr i) {
  switch (i->desc) {
    case K::Lend: break;
    case K::Lprologue: fprintf(ppf, "prologue"); break;
    case K::Lop:
      switch (i->op->k) {
        case MK::Ialloc:
        case MK::Ipoll:
        case MK::Icall_ind:
        case MK::Icall_imm:
        case MK::Iextcall:
          fprintf(ppf, "@[<1>{%t}@]@,", [i](Formatter& f) { printmach::print_regsetaddr(f, i->live); });
          break;
        default: break;
      }
      printmach::print_operation(ppf, *i->op, i->arg, i->res);
      break;
    case K::Lreloadretaddr: fprintf(ppf, "reload retaddr"); break;
    case K::Lreturn: fprintf(ppf, "return %t", [i](Formatter& f) { printmach::print_regs(f, i->arg); }); break;
    case K::Llabel: fprintf(ppf, "%a:", pr(label, i->lbl)); break;
    case K::Lbranch: fprintf(ppf, "goto %a", pr(label, i->lbl)); break;
    case K::Lcondbranch:
      fprintf(ppf, "if %t goto %a", [i](Formatter& f) { printmach::print_test(f, i->test, i->arg); },
              pr(label, i->lbl));
      break;
    case K::Lcondbranch3: {
      fprintf(ppf, "switch3 %a", pr(printmach::reg, i->arg[0]));
      auto c = [&](long n, const std::optional<Label>& l) {
        if (l) fprintf(ppf, "@,case %i: goto %a", n, pr(label, *l));
      };
      c(0, i->lbl0);
      c(1, i->lbl1);
      c(2, i->lbl2);
      fprintf(ppf, "@,endswitch");
      break;
    }
    case K::Lswitch:
      fprintf(ppf, "switch %a", pr(printmach::reg, i->arg[0]));
      for (std::size_t k = 0; k < i->lbls.size(); ++k)
        fprintf(ppf, "case %i: goto %a", static_cast<long>(k), pr(label, i->lbls[k]));
      fprintf(ppf, "@,endswitch");
      break;
    case K::Lentertrap: fprintf(ppf, "enter trap"); break;
    case K::Ladjust_trap_depth: fprintf(ppf, "adjust trap depth by %i traps", i->delta_traps); break;
    case K::Lpushtrap: fprintf(ppf, "push trap %a", pr(label, i->lbl)); break;
    case K::Lpoptrap: fprintf(ppf, "pop trap"); break;
    case K::Lraise: fprintf(ppf, "%s %a", lambda::raise_kind(i->raise), pr(printmach::reg, i->arg[0])); break;
  }
  if (!debuginfo::is_none(i->dbg) && clflags::locations) fprintf(ppf, " %s", debuginfo::to_string(i->dbg));
}

void all_instr(Formatter& ppf, Instr i) {
  // fprintf ppf "%a@,%a" instr i all_instr i.next
  while (i->desc != K::Lend) {
    fprintf(ppf, "%a@,", pr(instr, i));
    i = i->next;
  }
}
}  // namespace

void print_fundecl(Formatter& ppf, const Fundecl& f) {
  std::string dbg = debuginfo::is_none(f.fun_dbg) || !clflags::locations ? "" : " " + debuginfo::to_string(f.fun_dbg);
  fprintf(ppf, "@[<v 2>%s:%s@,%a@]", f.fun_name, dbg, pr(all_instr, f.fun_body));
}

}  // namespace cppcaml::typing::linear
