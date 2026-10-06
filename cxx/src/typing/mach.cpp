// Ports of asmcomp/reg.ml, mach.ml and printmach.ml.  See mach.hpp.
#include "cppcaml/typing/mach.hpp"

#include "cppcaml/flat_map.hpp"

#include <functional>
#include <unordered_map>

#include <cstring>
#include <stdexcept>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/printclambda.hpp"

namespace cppcaml::typing {

namespace proc {
void init_hard_regs();
}

// ---- Reg ---------------------------------------------------------------------------------------
namespace reg {

namespace {
long currstamp = 0;
std::vector<Reg*> reg_list;     // the oldest first (Reg.reg_list reversed)
std::vector<Reg*> hw_reg_list;
long visit_generation = 1;
constexpr long unvisited = 0;
long first_virtual_reg_stamp = -1;
}  // namespace

Reg* create(cmm::MachtypeComponent ty) {
  auto* r = make<Reg>();
  r->stamp = currstamp;
  r->typ = ty;
  r->visited = unvisited;
  reg_list.push_back(r);
  ++currstamp;
  return r;
}

Regs createv(cmm::Machtype tyv) {
  Regs rv;
  for (auto t : tyv) rv.push_back(create(t));
  return rv;
}
Regs createv(const std::vector<cmm::MachtypeComponent>& tyv) {
  Regs rv;
  for (auto t : tyv) rv.push_back(create(t));
  return rv;
}
Regs createv_like(const Regs& rv) {
  Regs r;
  for (Reg* x : rv) r.push_back(create(x->typ));
  return r;
}
Reg* clone(Reg* r) {
  Reg* nr = create(r->typ);
  nr->raw_name = r->raw_name;
  return nr;
}

Reg* at_location(cmm::MachtypeComponent ty, Location loc) {
  // (hw_reg_list keeps it for good -- every reset visits it -- while a
  // function's other registers live in its scratch zone: Asmgen)
  ZoneScope perm(permanent_zone());
  auto* r = make<Reg>();
  r->raw_name.k = RawName::K::R;
  r->stamp = currstamp;
  r->typ = ty;
  r->loc = loc;
  r->visited = unvisited;
  hw_reg_list.push_back(r);
  ++currstamp;
  return r;
}

std::vector<cmm::MachtypeComponent> typv(const Regs& rv) {
  std::vector<cmm::MachtypeComponent> t;
  for (Reg* r : rv) t.push_back(r->typ);
  return t;
}

namespace {
std::optional<std::string> raw_name_to_string(const RawName& n) {
  switch (n.k) {
    case RawName::K::Anon: return std::nullopt;
    case RawName::K::R: return std::string("R");
    case RawName::K::Var: {
      std::string_view name = ident::name(n.var);
      if (name.empty()) return std::nullopt;
      return std::string(name);
    }
  }
  return std::nullopt;
}
}  // namespace

bool anonymous(const Reg* r) { return !raw_name_to_string(r->raw_name); }

std::string name(const Reg* r) {
  std::optional<std::string> raw = raw_name_to_string(r->raw_name);
  if (!raw) return "";
  std::string with_spilled = r->spill ? "spilled-" + *raw : *raw;
  if (!r->part) return with_spilled;
  return with_spilled + "#" + std::to_string(*r->part);
}

void reset() {
  // When reset() is called for the first time, the current stamp reflects
  // all hard pseudo-registers that have been allocated by Proc, so remember
  // it and use it as the base stamp for allocating soft pseudo-registers
  proc::init_hard_regs();
  if (first_virtual_reg_stamp == -1) first_virtual_reg_stamp = currstamp;
  currstamp = first_virtual_reg_stamp;
  reg_list.clear();
  visit_generation = 1;
  for (Reg* r : hw_reg_list) r->visited = unvisited;
}

void reinit() {
  for (Reg* r : reg_list) {
    r->loc = Location{};
    r->interf.clear();
    r->prefer.clear();
    r->degree = 0;
    // Preserve the very high spill costs introduced by the reloading pass
    r->spill_cost = r->spill_cost >= 100000 ? 100000 : 0;
  }
}

void mark_visited(Reg* r) { r->visited = visit_generation; }
bool is_visited(const Reg* r) { return r->visited == visit_generation; }
void clear_visited_marks() { ++visit_generation; }

std::vector<Reg*> all_registers() { return std::vector<Reg*>(reg_list.rbegin(), reg_list.rend()); }
long num_registers() { return currstamp; }

}  // namespace reg

// ---- Mach ------------------------------------------------------------------------------------
namespace mach {

Instr dummy_instr() {
  static Instr d = [] {
    auto* i = new Instruction{Instruction::K::Iend};
    i->next = i;
    return i;
  }();
  return d;
}

// The zone Mach instructions are allocated in (Asmgen's generations: a
// pass's input is dropped once its output is compacted); null = zone()
static Zone* g_instr_zone = nullptr;
void set_instr_zone(Zone* z) { g_instr_zone = z; }
static Instruction* new_instr(const Instruction& d) {
  return g_instr_zone ? g_instr_zone->make<Instruction>(d) : make<Instruction>(d);
}

Instr compact_instrs(Instr root, Zone& into) {
  FlatMap<Instr, Instr> memo;  // (open addressing: an unordered_map node per instruction was ~2%)
  // a chain along `next` copied iteratively (a function body is long), the
  // branches recursively (as deep as the code nests); shared nodes stay
  // shared, the static dummy_instr is kept
  std::function<Instr(Instr)> chain = [&](Instr i) -> Instr {
    Instr head = nullptr;
    Instr* slot = &head;
    while (i) {
      if (i == dummy_instr()) {
        *slot = i;
        break;
      }
      if (auto it = memo.find(i)) {
        *slot = it->second;
        break;
      }
      Instruction* c = into.make<Instruction>(*i);
      memo.try_emplace(i, c);
      *slot = c;
      if (c->ifso) c->ifso = chain(c->ifso);
      if (c->ifnot) c->ifnot = chain(c->ifnot);
      if (c->body) c->body = chain(c->body);
      for (Handler& h : c->handlers) h.body = chain(h.body);
      for (Instr& x : c->cases) x = chain(x);
      slot = &c->next;
      i = i->next;
    }
    return head;
  };
  return chain(root);
}

Instr end_instr() {
  auto* i = new_instr(Instruction{Instruction::K::Iend});
  i->next = dummy_instr();
  return i;
}

const Operation* op_ref(const Operation& op) { return make<Operation>(op); }
const Operation* imove_op() {
  static const Operation* m = permanent_zone().make<Operation>(Operation{Operation::K::Imove});
  return m;
}

Instruction iop(const Operation& op) {
  Instruction d{Instruction::K::Iop};
  d.op = op_ref(op);
  return d;
}
Instruction idesc(Instruction::K k) { return Instruction{k}; }

Instr instr_cons(const Instruction& d, const Regs& a, const Regs& r, Instr n) {
  return instr_cons_debug(d, a, r, debuginfo::none(), n);
}
Instr instr_cons_debug(const Instruction& d, const Regs& a, const Regs& r, const debuginfo::t& dbg, Instr n) {
  auto* i = new_instr(d);
  i->next = n;
  i->arg = a;
  i->res = r;
  i->dbg = dbg;
  i->live.clear();
  return i;
}
Instr copy(Instr i) { return new_instr(*i); }

void instr_iter(const std::function<void(Instr)>& f, Instr i) {
  using K = Instruction::K;
  while (i->desc != K::Iend) {
    f(i);
    switch (i->desc) {
      case K::Ireturn: return;
      case K::Iop:
        if (i->op->k == Operation::K::Itailcall_ind || i->op->k == Operation::K::Itailcall_imm) return;
        break;
      case K::Iifthenelse:
        instr_iter(f, i->ifso);
        instr_iter(f, i->ifnot);
        break;
      case K::Iswitch:
        for (Instr c : i->cases) instr_iter(f, c);
        break;
      case K::Icatch:
        instr_iter(f, i->body);
        for (auto& h : i->handlers) instr_iter(f, h.body);
        break;
      case K::Iexit: return;
      case K::Itrywith:
        instr_iter(f, i->ifso);
        instr_iter(f, i->ifnot);
        break;
      case K::Iraise: return;
      default: break;
    }
    i = i->next;
  }
}

bool operation_is_pure(const Operation& op) {
  using K = Operation::K;
  switch (op.k) {
    case K::Icall_ind:
    case K::Icall_imm:
    case K::Itailcall_ind:
    case K::Itailcall_imm:
    case K::Iextcall:
    case K::Istackoffset:
    case K::Istore:
    case K::Iatomic_fetch_add:
    case K::Ialloc:
    case K::Ipoll:
    case K::Idls_get:
    case K::Iopaque: return false;
    case K::Iintop:
    case K::Iintop_imm: return op.intop.op != IntegerOperation::Icheckbound;
    case K::Iload: return !op.is_atomic;
    case K::Ispecific: return arch::operation_is_pure(op.spec);
    default: return true;
  }
}

bool operation_can_raise(const Operation& op) {
  using K = Operation::K;
  switch (op.k) {
    case K::Icall_ind:
    case K::Icall_imm:
    case K::Iextcall:
    case K::Ialloc:
    case K::Ipoll: return true;
    case K::Iintop:
    case K::Iintop_imm: return op.intop.op == IntegerOperation::Icheckbound;
    case K::Ispecific: return arch::operation_can_raise(op.spec);
    default: return false;
  }
}

}  // namespace mach

// ---- Printmach ---------------------------------------------------------------------------------
namespace printmach {

using format::Formatter;
using format::fprintf;
using format::pr;
using namespace mach;

void reg(Formatter& ppf, const reg::Reg* r) {
  using MC = cmm::MachtypeComponent;
  if (!reg::anonymous(r)) fprintf(ppf, "%s", reg::name(r));
  else
    fprintf(ppf, "%s", r->typ == MC::Val ? "V" : r->typ == MC::Addr ? "A" : r->typ == MC::Int ? "I" : "F");
  fprintf(ppf, "/%i", r->stamp);
  using LK = reg::Location::K;
  switch (r->loc.k) {
    case LK::Unknown: break;
    case LK::Reg: fprintf(ppf, "[%s]", proc::register_name(r->loc.n)); break;
    case LK::Local: fprintf(ppf, "[s%i]", r->loc.n); break;
    case LK::Incoming: fprintf(ppf, "[si%i]", r->loc.n); break;
    case LK::Outgoing: fprintf(ppf, "[so%i]", r->loc.n); break;
    case LK::Domainstate: fprintf(ppf, "[ds%i]", r->loc.n); break;
  }
}

namespace {

void regs(Formatter& ppf, const Regs& v) {
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) fprintf(ppf, " ");
    reg(ppf, v[i]);
  }
}

void regsetaddr(Formatter& ppf, const reg::Set& s) {
  using MC = cmm::MachtypeComponent;
  bool first = true;
  for (reg::Reg* r : s) {
    if (first) {
      first = false;
      fprintf(ppf, "%a", pr(reg, r));
    } else
      fprintf(ppf, "@ %a", pr(reg, r));
    if (r->typ == MC::Val) fprintf(ppf, "*");
    else if (r->typ == MC::Addr) fprintf(ppf, "!");
  }
}

const char* integer_comparison(lambda::IntegerComparison c) {
  using C = lambda::IntegerComparison;
  switch (c) {
    case C::Ceq: return "==";
    case C::Cne: return "!=";
    case C::Clt: return "<";
    case C::Cle: return "<=";
    case C::Cgt: return ">";
    case C::Cge: return ">=";
  }
  return "";
}
const char* float_comparison(lambda::FloatComparison c) {
  using C = lambda::FloatComparison;
  switch (c) {
    case C::CFeq: return "==";
    case C::CFneq: return "!=";
    case C::CFlt: return "<";
    case C::CFnlt: return "!<";
    case C::CFle: return "<=";
    case C::CFnle: return "!<=";
    case C::CFgt: return ">";
    case C::CFngt: return "!>";
    case C::CFge: return ">=";
    case C::CFnge: return "!>=";
  }
  return "";
}
const char* chunk(cmm::MemoryChunk c) {
  using MC = cmm::MemoryChunk;
  switch (c) {
    case MC::Byte_unsigned: return "unsigned int8";
    case MC::Byte_signed: return "signed int8";
    case MC::Sixteen_unsigned: return "unsigned int16";
    case MC::Sixteen_signed: return "signed int16";
    case MC::Thirtytwo_unsigned: return "unsigned int32";
    case MC::Thirtytwo_signed: return "signed int32";
    case MC::Sixtyfour: return "int64";
    case MC::Word_int: return "int";
    case MC::Word_val: return "val";
    case MC::Single: return "float32";
    case MC::Double: return "float64";
  }
  return "";
}

std::string intcomp(const IntegerComparison& c) {
  return std::string(" ") + integer_comparison(c.c) + (c.is_signed ? "s " : "u ");
}
std::string floatcomp(lambda::FloatComparison c) { return std::string(" ") + float_comparison(c) + "f "; }

std::string intop(const IntOp& op) {
  using IO = IntegerOperation;
  switch (op.op) {
    case IO::Iadd: return " + ";
    case IO::Isub: return " - ";
    case IO::Imul: return " * ";
    case IO::Imulh: return " *h ";
    case IO::Idiv: return " div ";
    case IO::Imod: return " mod ";
    case IO::Iand: return " & ";
    case IO::Ior: return " | ";
    case IO::Ixor: return " ^ ";
    case IO::Ilsl: return " << ";
    case IO::Ilsr: return " >>u ";
    case IO::Iasr: return " >>s ";
    case IO::Icomp: return intcomp(op.cmp);
    case IO::Icheckbound: return "check > ";
  }
  return "";
}

void test(Formatter& ppf, const Test& tst, const Regs& arg) {
  using K = Test::K;
  switch (tst.k) {
    case K::Itruetest: reg(ppf, arg[0]); return;
    case K::Ifalsetest: fprintf(ppf, "not %a", pr(reg, arg[0])); return;
    case K::Iinttest: fprintf(ppf, "%a%s%a", pr(reg, arg[0]), intcomp(tst.icmp), pr(reg, arg[1])); return;
    case K::Iinttest_imm: fprintf(ppf, "%a%s%i", pr(reg, arg[0]), intcomp(tst.icmp), tst.n); return;
    case K::Ifloattest: fprintf(ppf, "%a%s%a", pr(reg, arg[0]), floatcomp(tst.fcmp), pr(reg, arg[1])); return;
    case K::Ieventest: fprintf(ppf, "%a & 1 == 0", pr(reg, arg[0])); return;
    case K::Ioddtest: fprintf(ppf, "%a & 1 == 1", pr(reg, arg[0])); return;
  }
}

void operation(Formatter& ppf, const Operation& op, const Regs& arg, const Regs& res) {
  using K = Operation::K;
  if (!res.empty()) fprintf(ppf, "%a := ", pr(regs, res));
  switch (op.k) {
    case K::Imove: regs(ppf, arg); return;
    case K::Ispill: fprintf(ppf, "%a (spill)", pr(regs, arg)); return;
    case K::Ireload: fprintf(ppf, "%a (reload)", pr(regs, arg)); return;
    case K::Iconst_int: fprintf(ppf, "%s", std::to_string(op.n)); return;
    case K::Iconst_float: {
      double d;
      std::uint64_t bits = static_cast<std::uint64_t>(op.n);
      std::memcpy(&d, &bits, sizeof d);
      fprintf(ppf, "%s", printclambda::float_F(d));
      return;
    }
    case K::Iconst_symbol: fprintf(ppf, "\"%s\"", op.func); return;
    case K::Icall_ind: fprintf(ppf, "call %a", pr(regs, arg)); return;
    case K::Icall_imm: fprintf(ppf, "call \"%s\" %a", op.func, pr(regs, arg)); return;
    case K::Itailcall_ind: fprintf(ppf, "tailcall %a", pr(regs, arg)); return;
    case K::Itailcall_imm: fprintf(ppf, "tailcall \"%s\" %a", op.func, pr(regs, arg)); return;
    case K::Iextcall:
      fprintf(ppf, "extcall \"%s\" %a%s", op.func, pr(regs, arg), op.alloc ? "" : " (noalloc)");
      return;
    case K::Istackoffset: fprintf(ppf, "offset stack %i", static_cast<long>(op.n)); return;
    case K::Iload:
      fprintf(ppf, op.mut == MutableFlag::Immutable ? "%s %s[%t]" : "%s %s mut[%t]", chunk(op.chunk),
              op.is_atomic ? "atomic" : "", [&](Formatter& f) { arch::print_addressing(f, op.addr, arg); });
      return;
    case K::Istore: {
      Regs rest(arg.begin() + 1, arg.end());
      fprintf(ppf, "%s[%t] := %a %s", chunk(op.chunk), [&](Formatter& f) { arch::print_addressing(f, op.addr, rest); },
              pr(reg, arg[0]), op.is_assign ? "(assign)" : "(init)");
      return;
    }
    case K::Iatomic_fetch_add: fprintf(ppf, "atomic_fetch_add [%a] %a", pr(reg, arg[0]), pr(reg, arg[1])); return;
    case K::Ialloc: fprintf(ppf, "alloc %i", static_cast<long>(op.n)); return;
    case K::Iintop: fprintf(ppf, "%a%s%a", pr(reg, arg[0]), intop(op.intop), pr(reg, arg[1])); return;
    case K::Iintop_imm: fprintf(ppf, "%a%s%i", pr(reg, arg[0]), intop(op.intop), static_cast<long>(op.n)); return;
    case K::Icompf: fprintf(ppf, "%a%s%a", pr(reg, arg[0]), floatcomp(op.fcmp), pr(reg, arg[1])); return;
    case K::Inegf: fprintf(ppf, "-f %a", pr(reg, arg[0])); return;
    case K::Iabsf: fprintf(ppf, "absf %a", pr(reg, arg[0])); return;
    case K::Iaddf: fprintf(ppf, "%a +f %a", pr(reg, arg[0]), pr(reg, arg[1])); return;
    case K::Isubf: fprintf(ppf, "%a -f %a", pr(reg, arg[0]), pr(reg, arg[1])); return;
    case K::Imulf: fprintf(ppf, "%a *f %a", pr(reg, arg[0]), pr(reg, arg[1])); return;
    case K::Idivf: fprintf(ppf, "%a /f %a", pr(reg, arg[0]), pr(reg, arg[1])); return;
    case K::Ifloatofint: fprintf(ppf, "floatofint %a", pr(reg, arg[0])); return;
    case K::Iintoffloat: fprintf(ppf, "intoffloat %a", pr(reg, arg[0])); return;
    case K::Iopaque: fprintf(ppf, "opaque %a", pr(reg, arg[0])); return;
    case K::Ispecific: arch::print_specific_operation(ppf, op.spec, arg); return;
    case K::Idls_get: fprintf(ppf, "dls_get"); return;
    case K::Ireturn_addr: fprintf(ppf, "return_addr"); return;
    case K::Ipoll:
      fprintf(ppf, "poll call");
      if (op.return_label) fprintf(ppf, " returning to L%i", *op.return_label);
      return;
  }
}

void instr(Formatter& ppf, Instr i) {
  using K = Instruction::K;
  if (clflags::dump_live) {
    fprintf(ppf, "@[<1>{%t", [&](Formatter& f) { regsetaddr(f, i->live); });
    if (!i->arg.empty()) fprintf(ppf, "@ +@ %a", pr(regs, i->arg));
    fprintf(ppf, "}@]@,");
  }
  switch (i->desc) {
    case K::Iend: break;
    case K::Iop: operation(ppf, *i->op, i->arg, i->res); break;
    case K::Ireturn: fprintf(ppf, "return %a", pr(regs, i->arg)); break;
    case K::Iifthenelse:
      fprintf(ppf, "@[<v 2>if %t then@,%a", [&](Formatter& f) { test(f, i->test, i->arg); }, pr(instr, i->ifso));
      if (i->ifnot->desc != K::Iend) fprintf(ppf, "@;<0 -2>else@,%a", pr(instr, i->ifnot));
      fprintf(ppf, "@;<0 -2>endif@]");
      break;
    case K::Iswitch:
      fprintf(ppf, "switch %a", pr(reg, i->arg[0]));
      for (std::size_t c = 0; c < i->cases.size(); ++c) {
        fprintf(ppf, "@,@[<v 2>@[");
        for (std::size_t j = 0; j < i->index.size(); ++j)
          if (i->index[j] == static_cast<long>(c)) fprintf(ppf, "case %i:@,", static_cast<long>(j));
        fprintf(ppf, "@]@,%a@]", pr(instr, i->cases[c]));
      }
      fprintf(ppf, "@,endswitch");
      break;
    case K::Icatch: {
      fprintf(ppf, "@[<v 2>catch%s@,%a@;<0 -2>with", i->rec == cmm::RecFlag::Recursive ? " rec" : "",
              pr(instr, i->body));
      for (std::size_t k = 0; k < i->handlers.size(); ++k) {
        fprintf(ppf, "(%i)@,%a@;", i->handlers[k].n, pr(instr, i->handlers[k].body));
        if (k + 1 < i->handlers.size()) fprintf(ppf, "@ and");
      }
      fprintf(ppf, "@;<0 -2>endcatch@]");
      break;
    }
    case K::Iexit: fprintf(ppf, "exit(%i)", i->nfail); break;
    case K::Itrywith:
      fprintf(ppf, "@[<v 2>try@,%a@;<0 -2>with@,%a@;<0 -2>endtry@]", pr(instr, i->ifso), pr(instr, i->ifnot));
      break;
    case K::Iraise: fprintf(ppf, "%s %a", lambda::raise_kind(i->raise), pr(reg, i->arg[0])); break;
  }
  if (!debuginfo::is_none(i->dbg) && clflags::locations) fprintf(ppf, "%s", debuginfo::to_string(i->dbg));
  if (i->next->desc != K::Iend) fprintf(ppf, "@,%a", pr(instr, i->next));
}

}  // namespace

void fundecl(Formatter& ppf, const Fundecl& f) {
  std::string dbg =
      debuginfo::is_none(f.fun_dbg) || !clflags::locations ? "" : " " + debuginfo::to_string(f.fun_dbg);
  fprintf(ppf, "@[<v 2>%s(%a)%s@,%a@]", f.fun_name, pr(regs, f.fun_args), dbg, pr(instr, f.fun_body));
}

void phase(Formatter& ppf, const std::string& msg, const Fundecl& f) {
  fprintf(ppf, "*** %s@.%a@.", msg, pr(fundecl, f));
}

void print_regs(Formatter& ppf, const Regs& v) { regs(ppf, v); }
void print_regsetaddr(Formatter& ppf, const reg::Set& s) { regsetaddr(ppf, s); }
void print_test(Formatter& ppf, const Test& t, const Regs& arg) { test(ppf, t, arg); }
void print_operation(Formatter& ppf, const Operation& op, const Regs& arg, const Regs& res) {
  operation(ppf, op, arg, res);
}

void interferences(Formatter& ppf) {
  fprintf(ppf, "*** Interferences@.");
  for (reg::Reg* r : reg::all_registers()) {
    auto interf = [r](Formatter& f) {
      for (reg::Reg* x : r->interf) fprintf(f, "@ %a", pr(reg, x));
    };
    fprintf(ppf, "@[<2>%a:%t@]@.", pr(reg, r), interf);
  }
}

void intervals(Formatter& ppf, const interval::Result& r) {
  auto one = [&](const interval::Interval* i) {
    auto interv = [i](Formatter& f) {
      for (std::size_t k = i->first; k < i->ranges.size(); ++k)
        fprintf(f, "@ [%d;%d]", i->ranges[k].rbegin, i->ranges[k].rend);
    };
    fprintf(ppf, "@[<2>%a:%t@]@.", pr(reg, i->reg), interv);
  };
  fprintf(ppf, "*** Intervals@.");
  for (const interval::Interval* i : r.fixed_intervals) one(i);
  for (const interval::Interval* i : r.intervals) one(i);
}

void preferences(Formatter& ppf) {
  fprintf(ppf, "*** Preferences@.");
  for (reg::Reg* r : reg::all_registers()) {
    auto prefs = [r](Formatter& f) {
      for (auto& [x, w] : r->prefer) fprintf(f, "@ %a weight %i", pr(reg, x), w);
    };
    fprintf(ppf, "@[<2>%a: %t@]@.", pr(reg, r), prefs);
  }
}

}  // namespace printmach

}  // namespace cppcaml::typing
