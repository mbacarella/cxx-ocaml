// Port of middle_end/flambda/flambda.ml, allocated_const.ml, parameter.ml and
// projection.ml (see flambda.hpp).
#include "cppcaml/typing/flambda.hpp"

#include <map>
#include <string>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/printclambda.hpp"
#include "cppcaml/typing/printlambda.hpp"

namespace cppcaml::typing {

using format::Formatter;
using format::fprintf;

namespace {
// Printf's "%f" (OCaml formats a float conversion with the C library's)
std::string percent_f(double f) {
  char buf[512];
  std::snprintf(buf, sizeof buf, "%f", f);
  return buf;
}
}  // namespace

// ---- Allocated_const --------------------------------------------------------
namespace allocated_const {
namespace {
t mk(AllocatedConst c) { return make<AllocatedConst>(c); }
// compare_floats: the bit patterns (Int64.bits_of_float)
int compare_floats(double x, double y) {
  std::int64_t a, b;
  std::memcpy(&a, &x, sizeof a);
  std::memcpy(&b, &y, sizeof b);
  return a < b ? -1 : a > b ? 1 : 0;
}
int compare_float_lists(Slice<double> l1, Slice<double> l2) {
  for (std::size_t k = 0;; ++k) {
    if (k == l1.size()) return k == l2.size() ? 0 : -1;
    if (k == l2.size()) return 1;
    if (int c = compare_floats(l1[k], l2[k]); c != 0) return c;
  }
}
int compare_i64(std::int64_t a, std::int64_t b) { return a < b ? -1 : a > b ? 1 : 0; }
}  // namespace
t float_(double f) { return mk({AllocatedConst::Kind::Float, f}); }
t int32(std::int32_t n) { return mk({AllocatedConst::Kind::Int32, 0, n}); }
t int64(std::int64_t n) { return mk({AllocatedConst::Kind::Int64, 0, n}); }
t nativeint(std::int64_t n) { return mk({AllocatedConst::Kind::Nativeint, 0, n}); }
t immutable_float_array(Slice<double> fl) {
  AllocatedConst c{AllocatedConst::Kind::Immutable_float_array};
  c.floats = fl;
  return mk(c);
}
t immutable_string(std::string_view s) {
  AllocatedConst c{AllocatedConst::Kind::Immutable_string};
  c.s = s;
  return mk(c);
}
int compare(t x, t y) {
  using K = AllocatedConst::Kind;
  if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1;  // (constructor order)
  switch (x->kind) {
    case K::Float: return compare_floats(x->f, y->f);
    case K::Int32: case K::Int64: case K::Nativeint: return compare_i64(x->i, y->i);
    case K::Float_array: case K::Immutable_float_array: return compare_float_lists(x->floats, y->floats);
    case K::String: case K::Immutable_string: {
      int c = x->s.compare(y->s);
      return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
  }
  return 0;
}
void print(Formatter& ppf, t c) {
  auto floats = [&](Formatter& f, Slice<double> fl, std::size_t from) {
    for (std::size_t k = from; k < fl.size(); ++k) fprintf(f, "@ %s", percent_f(fl[k]));
  };
  using K = AllocatedConst::Kind;
  switch (c->kind) {
    case K::String: fprintf(ppf, "%S", c->s); break;
    case K::Immutable_string: fprintf(ppf, "#%S", c->s); break;
    case K::Int32: fprintf(ppf, "%lil", static_cast<std::int32_t>(c->i)); break;
    case K::Int64: fprintf(ppf, "%LiL", c->i); break;
    case K::Nativeint: fprintf(ppf, "%nin", c->i); break;
    case K::Float: fprintf(ppf, "%s", percent_f(c->f)); break;
    case K::Float_array:
      if (c->floats.empty()) fprintf(ppf, "[| |]");
      else
        fprintf(ppf, "@[<1>[|@[%s%a@]|]@]", percent_f(c->floats[0]),
                [&](Formatter& f) { floats(f, c->floats, 1); });
      break;
    case K::Immutable_float_array:
      if (c->floats.empty()) fprintf(ppf, "[|# |]");
      else
        fprintf(ppf, "@[<1>[|# @[%s%a@]|]@]", percent_f(c->floats[0]),
                [&](Formatter& f) { floats(f, c->floats, 1); });
      break;
  }
}
}  // namespace allocated_const

// ---- Parameter ----------------------------------------------------------------
// Variable.Set.of_list (List.map var l)
variable::Set parameter::set_vars(Slice<Parameter> ps) {
  std::vector<variable::t> l;
  l.reserve(ps.size());
  for (const Parameter& p : ps) l.push_back(p.var);
  return variable::Set::of_list(l);
}

// ---- Projection -----------------------------------------------------------------
namespace projection {
void print_project_closure(Formatter& ppf, const ProjectClosure& p) {
  fprintf(ppf, "@[<2>(project_closure@ %a@ from@ %a)@]", pr(variable::print, p.closure_id),
          pr(variable::print, p.set_of_closures));
}
void print_move_within_set_of_closures(Formatter& ppf, const MoveWithinSetOfClosures& m) {
  fprintf(ppf, "@[<2>(move_within_set_of_closures@ %a <-- %a@ (closure = %a))@]", pr(variable::print, m.move_to),
          pr(variable::print, m.start_from), pr(variable::print, m.closure));
}
void print_project_var(Formatter& ppf, const ProjectVar& p) {
  fprintf(ppf, "@[<2>(project_var@ %a@ from %a=%a)@]", pr(variable::print, p.var), pr(variable::print, p.closure_id),
          pr(variable::print, p.closure));
}
int compare_project_var(const ProjectVar& a, const ProjectVar& b) {
  if (int c = variable::compare(a.closure, b.closure); c != 0) return c;
  if (int c = variable::compare(a.closure_id, b.closure_id); c != 0) return c;
  return variable::compare(a.var, b.var);
}
int compare_move_within_set_of_closures(const MoveWithinSetOfClosures& a, const MoveWithinSetOfClosures& b) {
  if (int c = variable::compare(a.closure, b.closure); c != 0) return c;
  if (int c = variable::compare(a.start_from, b.start_from); c != 0) return c;
  return variable::compare(a.move_to, b.move_to);
}
int compare_project_closure(const ProjectClosure& a, const ProjectClosure& b) {
  if (int c = variable::compare(a.set_of_closures, b.set_of_closures); c != 0) return c;
  return variable::compare(a.closure_id, b.closure_id);
}
int compare(t a, t b) {
  if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;  // (constructor order)
  switch (a->kind) {
    case T::Kind::Project_var: return compare_project_var(a->project_var, b->project_var);
    case T::Kind::Project_closure: return compare_project_closure(a->project_closure, b->project_closure);
    case T::Kind::Move_within_set_of_closures: return compare_move_within_set_of_closures(a->move, b->move);
    case T::Kind::Field:
      if (a->field_index != b->field_index) return a->field_index < b->field_index ? -1 : 1;
      return variable::compare(a->field_var, b->field_var);
  }
  return 0;
}
variable::t projecting_from(t p) {
  switch (p->kind) {
    case T::Kind::Project_var: return p->project_var.closure;
    case T::Kind::Project_closure: return p->project_closure.set_of_closures;
    case T::Kind::Move_within_set_of_closures: return p->move.closure;
    case T::Kind::Field: return p->field_var;
  }
  return nullptr;
}
t map_projecting_from(t p, FnRef<variable::t(variable::t)> f) {
  T r = *p;
  switch (p->kind) {
    case T::Kind::Project_var: r.project_var.closure = f(p->project_var.closure); break;
    case T::Kind::Project_closure: r.project_closure.set_of_closures = f(p->project_closure.set_of_closures); break;
    case T::Kind::Move_within_set_of_closures: r.move.closure = f(p->move.closure); break;
    case T::Kind::Field: r.field_var = f(p->field_var); break;
  }
  return make<T>(r);
}
void print(Formatter& ppf, t p) {
  switch (p->kind) {
    case T::Kind::Project_closure: print_project_closure(ppf, p->project_closure); break;
    case T::Kind::Project_var: print_project_var(ppf, p->project_var); break;
    case T::Kind::Move_within_set_of_closures: print_move_within_set_of_closures(ppf, p->move); break;
    case T::Kind::Field: fprintf(ppf, "Field %d of %a", p->field_index, pr(variable::print, p->field_var)); break;
  }
}
}  // namespace projection

namespace flambda {

named n_const_literal(const char* unit, long n) {
  static std::map<std::pair<std::string, long>, named> literals;
  auto [it, fresh] = literals.try_emplace({unit, n}, nullptr);
  if (fresh) {
    ZoneScope in_permanent(permanent_zone());
    it->second = n_const(const_int_literal(unit, n));
  }
  return it->second;
}
named n_allocated_const_literal(const char* unit, allocated_const::t (*make)(), const char* key) {
  static std::map<std::pair<std::string, std::string>, named> literals;
  auto [it, fresh] = literals.try_emplace({unit, key}, nullptr);
  if (fresh) {
    ZoneScope in_permanent(permanent_zone());
    it->second = n_allocated_const(make());
  }
  return it->second;
}

Const const_int_literal(const char* unit, long n) {
  static std::map<std::pair<std::string, long>, std::uint64_t> literals;
  auto [it, fresh] = literals.try_emplace({unit, n}, 0);
  if (fresh) it->second = next_value_identity();
  Const c{Const::Kind::Int, n};
  c.obj = it->second;
  return c;
}

// ---- constructors -----------------------------------------------------------------
t var(variable::t v) { return make<Var>(Var{{EK::Var}, v}); }
t let_mutable(variable::t v, variable::t initial_value, lambda::ValueKind contents_kind, t body) {
  return make<Let_mutable>(Let_mutable{{EK::Let_mutable}, v, initial_value, contents_kind, body});
}
t apply(variable::t func, Slice<variable::t> args, CallKind kind, debuginfo::t dbg, lambda::InlineAttribute inline_,
        lambda::SpecialiseAttribute specialise) {
  return make<Apply>(Apply{{EK::Apply}, func, args, kind, dbg, inline_, specialise});
}
t send(lambda::MethKind kind, variable::t meth, variable::t obj, Slice<variable::t> args, debuginfo::t dbg) {
  return make<Send>(Send{{EK::Send}, kind, meth, obj, args, dbg});
}
t assign(variable::t being_assigned, variable::t new_value) {
  return make<Assign>(Assign{{EK::Assign}, being_assigned, new_value});
}
t if_then_else(variable::t cond, t ifso, t ifnot) {
  return make<If_then_else>(If_then_else{{EK::If_then_else}, cond, ifso, ifnot});
}
t switch_(variable::t scrutinee, IntSet numconsts, Slice<SwitchCase> consts, IntSet numblocks,
          Slice<SwitchCase> blocks, t failaction) {
  return make<Switch>(Switch{{EK::Switch}, scrutinee, numconsts, consts, numblocks, blocks, failaction});
}
t string_switch(variable::t scrutinee, Slice<StringCase> cases, t def) {
  return make<String_switch>(String_switch{{EK::String_switch}, scrutinee, cases, def});
}
t static_raise(static_exception::t exn, Slice<variable::t> args) {
  return make<Static_raise>(Static_raise{{EK::Static_raise}, exn, args});
}
t static_catch(static_exception::t exn, Slice<CatchVar> vars, t body, t handler) {
  return make<Static_catch>(Static_catch{{EK::Static_catch}, exn, vars, body, handler});
}
t try_with(t body, variable::t v, t handler) { return make<Try_with>(Try_with{{EK::Try_with}, body, v, handler}); }
t while_(t cond, t body) { return make<While>(While{{EK::While}, cond, body}); }
t for_(variable::t bound_var, variable::t from_value, variable::t to_value, parsetree::DirectionFlag direction,
       t body) {
  return make<For>(For{{EK::For}, bound_var, from_value, to_value, direction, body});
}
t proved_unreachable() { return make<Proved_unreachable>(Proved_unreachable{{EK::Proved_unreachable}}); }

named n_symbol(symbol::t s) { return make<NSymbol>(NSymbol{{NK::Symbol}, s}); }
named n_const(Const c) { return make<NConst>(NConst{{NK::Const}, c}); }
named n_allocated_const(allocated_const::t c) {
  return make<NAllocated_const>(NAllocated_const{{NK::Allocated_const}, c});
}
named n_read_mutable(variable::t v) { return make<NRead_mutable>(NRead_mutable{{NK::Read_mutable}, v}); }
named n_read_symbol_field(symbol::t s, long field) {
  return make<NRead_symbol_field>(NRead_symbol_field{{NK::Read_symbol_field}, s, field});
}
named n_set_of_closures(const SetOfClosures* set) {
  return make<NSet_of_closures>(NSet_of_closures{{NK::Set_of_closures}, set});
}
named n_project_closure(const projection::ProjectClosure& p) {
  return make<NProject_closure>(NProject_closure{{NK::Project_closure}, p});
}
named n_move_within_set_of_closures(const projection::MoveWithinSetOfClosures& m) {
  return make<NMove_within_set_of_closures>(NMove_within_set_of_closures{{NK::Move_within_set_of_closures}, m});
}
named n_project_var(const projection::ProjectVar& p) {
  return make<NProject_var>(NProject_var{{NK::Project_var}, p});
}
named n_prim(const clambda::Primitive& prim, Slice<variable::t> args, debuginfo::t dbg) {
  return make<NPrim>(NPrim{{NK::Prim}, make<clambda::Primitive>(prim), args, dbg});
}
named n_expr(t e) { return make<NExpr>(NExpr{{NK::Expr}, e}); }

namespace {
program_body mk_body(ProgramBody b) { return make<ProgramBody>(b); }
}  // namespace
program_body let_symbol(symbol::t s, constant_defining_value def, program_body body) {
  ProgramBody b{ProgramBody::Kind::Let_symbol};
  b.sym = s;
  b.def = def;
  b.body = body;
  return mk_body(b);
}
program_body let_rec_symbol(Slice<SymbolBinding> defs, program_body body) {
  ProgramBody b{ProgramBody::Kind::Let_rec_symbol};
  b.defs = defs;
  b.body = body;
  return mk_body(b);
}
program_body initialize_symbol(symbol::t s, tag::t tag, Slice<t> fields, program_body body) {
  ProgramBody b{ProgramBody::Kind::Initialize_symbol};
  b.sym = s;
  b.tag = tag;
  b.fields = fields;
  b.body = body;
  return mk_body(b);
}
program_body effect(t e, program_body body) {
  ProgramBody b{ProgramBody::Kind::Effect};
  b.expr = e;
  b.body = body;
  return mk_body(b);
}
program_body end(symbol::t root) {
  ProgramBody b{ProgramBody::Kind::End};
  b.sym = root;
  return mk_body(b);
}

// ---- printers --------------------------------------------------------------------
namespace {
// Variable.print_list: "@ %a" each
void print_var_list(Formatter& ppf, Slice<variable::t> l) {
  for (variable::t v : l) fprintf(ppf, "@ %a", pr(variable::print, v));
}
void print_kind(Formatter& ppf, const lambda::ValueKind& kind) {
  if (kind.kind == lambda::ValueKind::Kind::Pgenval) return;
  fprintf(ppf, " %a", [&](Formatter& f) { printlambda::value_kind(f, kind); });
}
// Format.pp_print_list (pp_sep = pp_print_cut)
template <class T, class P>
void pp_print_list(Formatter& ppf, Slice<T> l, P pp_v) {
  for (std::size_t k = 0; k < l.size(); ++k) {
    if (k > 0) ppf.print_cut();
    pp_v(ppf, l[k]);
  }
}
void print_function_declaration(Formatter& ppf, variable::t var, const FunctionDeclaration* f);
}  // namespace

void print_specialised_to(Formatter& ppf, const SpecialisedTo& spec_to) {
  if (!spec_to.projection) fprintf(ppf, "%a", pr(variable::print, spec_to.var));
  else
    fprintf(ppf, "%a(= %a)", pr(variable::print, spec_to.var), pr(projection::print, spec_to.projection));
}

void print_const(Formatter& ppf, const Const& c) {
  if (c.kind == Const::Kind::Int) fprintf(ppf, "%i", c.n);
  else fprintf(ppf, "%C", static_cast<char>(c.n));
}

// lam
void print_expr(Formatter& ppf, t flam) {
  switch (flam->kind) {
    case EK::Var: variable::print(ppf, static_cast<const Var*>(flam)->var); break;
    case EK::Apply: {
      auto* a = static_cast<const Apply*>(flam);
      auto direct = [&](Formatter& f) {
        if (a->call_kind.direct) fprintf(f, "*[%a]", pr(variable::print, a->call_kind.direct));
      };
      auto inline_ = [&](Formatter& f) {
        using IK = lambda::InlineAttribute::Kind;
        switch (a->inline_.kind) {
          case IK::Always_inline: fprintf(f, "<always>"); break;
          case IK::Never_inline: fprintf(f, "<never>"); break;
          case IK::Hint_inline: fprintf(f, "<hint>"); break;
          case IK::Unroll: fprintf(f, "<unroll %i>", a->inline_.unroll); break;
          case IK::Default_inline: break;
        }
      };
      fprintf(ppf, "@[<2>(apply%a%a<%s>@ %a%a)@]", direct, inline_, debuginfo::to_string(a->dbg),
              pr(variable::print, a->func), [&](Formatter& f) { print_var_list(f, a->args); });
      break;
    }
    case EK::Assign: {
      auto* a = static_cast<const Assign*>(flam);
      fprintf(ppf, "@[<2>(assign@ %a@ %a)@]", pr(variable::print, a->being_assigned),
              pr(variable::print, a->new_value));
      break;
    }
    case EK::Send: {
      auto* s = static_cast<const Send*>(flam);
      const char* kind = s->meth_kind == lambda::MethKind::Self     ? "self"
                         : s->meth_kind == lambda::MethKind::Public ? "public"
                                                                    : "cached";
      fprintf(ppf, "@[<2>(send%s@ %a@ %a%a)@]", kind, pr(variable::print, s->obj), pr(variable::print, s->meth),
              [&](Formatter& f) { print_var_list(f, s->args); });
      break;
    }
    case EK::Proved_unreachable: fprintf(ppf, "unreachable"); break;
    case EK::Let: {
      auto* l = static_cast<const Let*>(flam);
      fprintf(ppf, "@[<2>(let@ @[<hv 1>(@[<2>%a@ %a@]", pr(variable::print, l->var),
              pr(print_named, l->defining_expr));
      t ul = l->body;
      while (auto* l2 = as<Let>(ul)) {
        fprintf(ppf, "@ @[<2>%a@ %a@]", pr(variable::print, l2->var), pr(print_named, l2->defining_expr));
        ul = l2->body;
      }
      fprintf(ppf, ")@]@ %a)@]", pr(print_expr, ul));
      break;
    }
    case EK::Let_mutable: {
      auto* l = static_cast<const Let_mutable*>(flam);
      fprintf(ppf, "@[<2>(let_mutable%a@ @[<2>%a@ %a@]@ %a)@]", [&](Formatter& f) { print_kind(f, l->contents_kind); },
              pr(variable::print, l->var), pr(variable::print, l->initial_value), pr(print_expr, l->body));
      break;
    }
    case EK::Switch: {
      auto* s = static_cast<const Switch*>(flam);
      auto sw = [&](Formatter& f) {
        bool spc = false;
        for (const SwitchCase& c : s->consts) {
          if (spc) fprintf(f, "@ ");
          else spc = true;
          fprintf(f, "@[<hv 1>case int %i:@ %a@]", c.key, pr(print_expr, c.action));
        }
        for (const SwitchCase& c : s->blocks) {
          if (spc) fprintf(f, "@ ");
          else spc = true;
          fprintf(f, "@[<hv 1>case tag %i:@ %a@]", c.key, pr(print_expr, c.action));
        }
        if (s->failaction) {
          if (spc) fprintf(f, "@ ");
          else spc = true;
          fprintf(f, "@[<hv 1>default:@ %a@]", pr(print_expr, s->failaction));
        }
      };
      fprintf(ppf, "@[<1>(%s(%i,%i) %a@ @[<v 0>%a@])@]", s->failaction ? "switch" : "switch*",
              s->numconsts.cardinal(), s->numblocks.cardinal(), pr(variable::print, s->scrutinee), sw);
      break;
    }
    case EK::String_switch: {
      auto* s = static_cast<const String_switch*>(flam);
      auto sw = [&](Formatter& f) {
        bool spc = false;
        for (const StringCase& c : s->cases) {
          if (spc) fprintf(f, "@ ");
          else spc = true;
          fprintf(f, "@[<hv 1>case \"%s\":@ %a@]", format::string_escaped(c.s), pr(print_expr, c.action));
        }
        if (s->def) {
          if (spc) fprintf(f, "@ ");
          else spc = true;
          fprintf(f, "@[<hv 1>default:@ %a@]", pr(print_expr, s->def));
        }
      };
      fprintf(ppf, "@[<1>(stringswitch %a@ @[<v 0>%a@])@]", pr(variable::print, s->scrutinee), sw);
      break;
    }
    case EK::Static_raise: {
      auto* r = static_cast<const Static_raise*>(flam);
      fprintf(ppf, "@[<2>(exit@ %d%a)@]", r->exn, [&](Formatter& f) { print_var_list(f, r->args); });
      break;
    }
    case EK::Static_catch: {
      auto* c = static_cast<const Static_catch*>(flam);
      auto vars = [&](Formatter& f) {
        for (const CatchVar& v : c->vars)
          fprintf(f, " %a%a", pr(variable::print, v.var), [&](Formatter& g) { print_kind(g, v.kind); });
      };
      fprintf(ppf, "@[<2>(catch@ %a@;<1 -1>with (%d%a)@ %a)@]", pr(print_expr, c->body), c->exn, vars,
              pr(print_expr, c->handler));
      break;
    }
    case EK::Try_with: {
      auto* tw = static_cast<const Try_with*>(flam);
      fprintf(ppf, "@[<2>(try@ %a@;<1 -1>with %a@ %a)@]", pr(print_expr, tw->body), pr(variable::print, tw->var),
              pr(print_expr, tw->handler));
      break;
    }
    case EK::If_then_else: {
      auto* i = static_cast<const If_then_else*>(flam);
      fprintf(ppf, "@[<2>(if@ %a@ then begin@ %a@ end else begin@ %a@ end)@]", pr(variable::print, i->cond),
              pr(print_expr, i->ifso), pr(print_expr, i->ifnot));
      break;
    }
    case EK::While: {
      auto* w = static_cast<const While*>(flam);
      fprintf(ppf, "@[<2>(while@ %a@ %a)@]", pr(print_expr, w->cond), pr(print_expr, w->body));
      break;
    }
    case EK::For: {
      auto* f = static_cast<const For*>(flam);
      fprintf(ppf, "@[<2>(for %a@ %a@ %s@ %a@ %a)@]", pr(variable::print, f->bound_var),
              pr(variable::print, f->from_value), f->direction == parsetree::DirectionFlag::Upto ? "to" : "downto",
              pr(variable::print, f->to_value), pr(print_expr, f->body));
      break;
    }
  }
}

void print_named(Formatter& ppf, named n) {
  switch (n->kind) {
    case NK::Symbol: symbol::print(ppf, static_cast<const NSymbol*>(n)->sym); break;
    case NK::Const: fprintf(ppf, "Const(%a)", pr(print_const, static_cast<const NConst*>(n)->c)); break;
    case NK::Allocated_const:
      fprintf(ppf, "Aconst(%a)", pr(allocated_const::print, static_cast<const NAllocated_const*>(n)->c));
      break;
    case NK::Read_mutable:
      fprintf(ppf, "Read_mut(%a)", pr(variable::print, static_cast<const NRead_mutable*>(n)->var));
      break;
    case NK::Read_symbol_field: {
      auto* r = static_cast<const NRead_symbol_field*>(n);
      fprintf(ppf, "%a.(%d)", pr(symbol::print, r->sym), r->field);
      break;
    }
    case NK::Project_closure:
      projection::print_project_closure(ppf, static_cast<const NProject_closure*>(n)->p);
      break;
    case NK::Project_var: projection::print_project_var(ppf, static_cast<const NProject_var*>(n)->p); break;
    case NK::Move_within_set_of_closures:
      projection::print_move_within_set_of_closures(ppf, static_cast<const NMove_within_set_of_closures*>(n)->m);
      break;
    case NK::Set_of_closures: print_set_of_closures(ppf, static_cast<const NSet_of_closures*>(n)->set); break;
    case NK::Prim: {
      auto* p = static_cast<const NPrim*>(n);
      fprintf(ppf, "@[<2>(%a<%s>%a)@]", [&](Formatter& f) { printclambda::primitive(f, *p->prim); },
              debuginfo::to_string(p->dbg), [&](Formatter& f) { print_var_list(f, p->args); });
      break;
    }
    case NK::Expr: fprintf(ppf, "*%a", pr(print_expr, static_cast<const NExpr*>(n)->expr)); break;
  }
}

namespace {
void print_function_declaration(Formatter& ppf, variable::t var, const FunctionDeclaration* f) {
  auto params = [&](Formatter& g) {
    for (const Parameter& p : f->params) fprintf(g, "@ %a", pr(variable::print, p.var));
  };
  const char* stub = f->stub ? " *stub*" : "";
  const char* is_a_functor = f->is_a_functor ? " *functor*" : "";
  const char* inline_ = "";
  switch (f->inline_.kind) {
    case lambda::InlineAttribute::Kind::Always_inline:
    case lambda::InlineAttribute::Kind::Hint_inline: inline_ = " *inline*"; break;
    case lambda::InlineAttribute::Kind::Never_inline: inline_ = " *never_inline*"; break;
    case lambda::InlineAttribute::Kind::Unroll: inline_ = " *unroll*"; break;
    case lambda::InlineAttribute::Kind::Default_inline: break;
  }
  const char* specialise = f->specialise == lambda::SpecialiseAttribute::Always_specialise  ? " *specialise*"
                           : f->specialise == lambda::SpecialiseAttribute::Never_specialise ? " *never_specialise*"
                                                                                            : "";
  fprintf(ppf, "@[<2>(%a%s%s%s%s@ =@ fun@[<2>%a@] ->@ @[<2>%a@])@]@ ", pr(variable::print, var), stub, is_a_functor,
          inline_, specialise, params, pr(print_expr, f->body));
}
}  // namespace

void print_set_of_closures(Formatter& ppf, const SetOfClosures* set) {
  auto funs = [&](Formatter& f) {
    set->function_decls->funs.iter(
        [&](variable::t var, const FunctionDeclaration* d) { print_function_declaration(f, var, d); });
  };
  auto vars = [&](Formatter& f) {
    set->free_vars.iter([&](variable::t id, const SpecialisedTo& v) {
      fprintf(f, "@ %a -rename-> %a", pr(variable::print, id), pr(print_specialised_to, v));
    });
  };
  auto spec = [&](Formatter& f) {
    if (!set->specialised_args.is_empty()) {
      fprintf(f, "@ ");
      set->specialised_args.iter([&](variable::t id, const SpecialisedTo& s) {
        fprintf(f, "@ %a := %a", pr(variable::print, id), pr(print_specialised_to, s));
      });
    }
  };
  // Variable.Map.print Variable.print
  auto surrogates = [&](Formatter& f) {
    auto elts = [&](Formatter& g) {
      set->direct_call_surrogates.iter([&](variable::t id, variable::t v) {
        fprintf(g, "@ (@[%a@ %a@])", pr(variable::print, id), pr(variable::print, v));
      });
    };
    fprintf(f, "@[<1>{@[%a@ @]}@]", elts);
  };
  fprintf(ppf,
          "@[<2>(set_of_closures id=%a@ %a@ @[<2>free_vars={%a@ }@]@ @[<2>specialised_args={%a})@]@ "
          "@[<2>direct_call_surrogates=%a@]@ @[<2>set_of_closures_origin=%a@]@]]",
          pr(unit_id::print, set->function_decls->set_of_closures_id), funs, vars, spec, surrogates,
          pr(unit_id::print, set->function_decls->set_of_closures_origin));
}

void print_function_declarations(Formatter& ppf, const FunctionDeclarations* fd) {
  auto funs = [&](Formatter& f) {
    fd->funs.iter([&](variable::t var, const FunctionDeclaration* d) { print_function_declaration(f, var, d); });
  };
  fprintf(ppf, "@[<2>(%a)(origin = %a)@]", funs, pr(unit_id::print, fd->set_of_closures_origin));
}

void print(Formatter& ppf, t flam) { fprintf(ppf, "%a@.", pr(print_expr, flam)); }

void print_constant_defining_value(Formatter& ppf, constant_defining_value c) {
  using K = ConstantDefiningValue::Kind;
  switch (c->kind) {
    case K::Allocated_const: fprintf(ppf, "(Allocated_const %a)", pr(allocated_const::print, c->c)); break;
    case K::Block:
      if (c->fields.empty()) fprintf(ppf, "(Atom (tag %d))", c->tag);
      else {
        auto fields = [&](Formatter& f) {
          for (const BlockField& field : c->fields) {
            if (field.sym) fprintf(f, "@ %a", pr(symbol::print, field.sym));
            else fprintf(f, "@ %a", pr(print_const, field.c));
          }
        };
        fprintf(ppf, "(Block (tag %d, %a))", c->tag, fields);
      }
      break;
    case K::Set_of_closures:
      fprintf(ppf, "@[<2>(Set_of_closures (@ %a))@]", pr(print_set_of_closures, c->set));
      break;
    case K::Project_closure:
      fprintf(ppf, "(Project_closure (%a, %a))", pr(symbol::print, c->sym), pr(variable::print, c->closure_id));
      break;
  }
}

namespace {
void print_program_body(Formatter& ppf, program_body program) {
  auto symbol_binding = [](Formatter& f, const SymbolBinding& b) {
    fprintf(f, "@[<2>(%a@ %a)@]", pr(symbol::print, b.sym), pr(print_constant_defining_value, b.def));
  };
  for (;;) {
    using K = ProgramBody::Kind;
    switch (program->kind) {
      case K::Let_symbol: {
        std::vector<SymbolBinding> defs;
        while (program->kind == K::Let_symbol) {
          defs.push_back({program->sym, program->def});
          program = program->body;
        }
        Slice<SymbolBinding> d{defs.data(), defs.size()};
        fprintf(ppf, "@[<2>let_symbol@ @[%a@]@]@.", [&](Formatter& f) { pp_print_list(f, d, symbol_binding); });
        break;
      }
      case K::Let_rec_symbol:
        fprintf(ppf, "@[<2>let_rec_symbol@ @[%a@]@]@.",
                [&](Formatter& f) { pp_print_list(f, program->defs, symbol_binding); });
        program = program->body;
        break;
      case K::Initialize_symbol:
        fprintf(ppf, "@[<2>initialize_symbol@ (@[<2>%a@ %d@ %a@])@]@.", pr(symbol::print, program->sym),
                program->tag, [&](Formatter& f) {
                  pp_print_list(f, program->fields, [](Formatter& g, t e) { print_expr(g, e); });
                });
        program = program->body;
        break;
      case K::Effect:
        fprintf(ppf, "@[<2>effect@ %a@]@.", pr(print_expr, program->expr));
        program = program->body;
        break;
      case K::End: fprintf(ppf, "End %a", pr(symbol::print, program->sym)); return;
    }
  }
}
}  // namespace

void print_program(Formatter& ppf, const Program& program) {
  program.imported_symbols.iter(
      [&](symbol::t s) { fprintf(ppf, "@[import_symbol@ %a@]@.", pr(symbol::print, s)); });
  print_program_body(ppf, program.program_body);
}

// ---- free variables ------------------------------------------------------------
namespace {
struct Usage {
  FvOpts o;
  bool all_used_variables;
  variable::Set free;
  variable::Set bound;
  // free := Variable.Set.union ids !free
  void free_variables(const variable::Set& ids) { free = variable::Set::union_(ids, free); }
  void free_variable(variable::t fv) { free = free.add(fv); }
  void bound_variable(variable::t id) { bound = bound.add(id); }
  void aux(t flam);
};
variable::Set variables_usage_named(named n, FvOpts o, bool all_used_variables);

// variables_usage ?ignore_uses_as_callee ?ignore_uses_as_argument
//   ?ignore_uses_in_project_var ~all_used_variables tree
variable::Set variables_usage(t tree, FvOpts o, bool all_used_variables) {
  if (auto* v = as<Var>(tree)) return variable::Set::singleton(v->var);
  Usage u{o, all_used_variables, {}, {}};
  u.aux(tree);
  if (all_used_variables) return u.free;
  return variable::Set::diff(u.free, u.bound);
}

// N.B. This function assumes that all bound identifiers are distinct.
void Usage::aux(t flam) {
  for (;;) {
    switch (flam->kind) {
      case EK::Var: free_variable(static_cast<const Var*>(flam)->var); return;
      case EK::Apply: {
        auto* a = static_cast<const Apply*>(flam);
        if (!o.ignore_uses_as_callee) free_variable(a->func);
        if (!o.ignore_uses_as_argument)
          for (variable::t v : a->args) free_variable(v);
        return;
      }
      case EK::Let: {
        auto* l = static_cast<const Let*>(flam);
        bound_variable(l->var);
        if (all_used_variables || o.ignore_uses_as_callee || o.ignore_uses_as_argument ||
            o.ignore_uses_in_project_var) {
          // In these cases we can't benefit from the pre-computed free
          // variable sets.
          free_variables(variables_usage_named(l->defining_expr, o, all_used_variables));
          flam = l->body;
          continue;
        }
        free_variables(l->free_vars_of_defining_expr);
        free_variables(l->free_vars_of_body);
        return;
      }
      case EK::Let_mutable: {
        auto* l = static_cast<const Let_mutable*>(flam);
        free_variable(l->initial_value);
        flam = l->body;
        continue;
      }
      case EK::Switch: {
        auto* s = static_cast<const Switch*>(flam);
        free_variable(s->scrutinee);
        for (const SwitchCase& c : s->consts) aux(c.action);
        for (const SwitchCase& c : s->blocks) aux(c.action);
        if (s->failaction) aux(s->failaction);
        return;
      }
      case EK::String_switch: {
        auto* s = static_cast<const String_switch*>(flam);
        free_variable(s->scrutinee);
        for (const StringCase& c : s->cases) aux(c.action);
        if (s->def) aux(s->def);
        return;
      }
      case EK::Static_raise:
        for (variable::t v : static_cast<const Static_raise*>(flam)->args) free_variable(v);
        return;
      case EK::Static_catch: {
        auto* c = static_cast<const Static_catch*>(flam);
        for (const CatchVar& v : c->vars) bound_variable(v.var);
        aux(c->body);
        flam = c->handler;
        continue;
      }
      case EK::Try_with: {
        auto* tw = static_cast<const Try_with*>(flam);
        aux(tw->body);
        bound_variable(tw->var);
        flam = tw->handler;
        continue;
      }
      case EK::If_then_else: {
        auto* i = static_cast<const If_then_else*>(flam);
        free_variable(i->cond);
        aux(i->ifso);
        flam = i->ifnot;
        continue;
      }
      case EK::While: {
        auto* w = static_cast<const While*>(flam);
        aux(w->cond);
        flam = w->body;
        continue;
      }
      case EK::For: {
        auto* f = static_cast<const For*>(flam);
        bound_variable(f->bound_var);
        free_variable(f->from_value);
        free_variable(f->to_value);
        flam = f->body;
        continue;
      }
      case EK::Assign: free_variable(static_cast<const Assign*>(flam)->new_value); return;
      case EK::Send: {
        auto* s = static_cast<const Send*>(flam);
        free_variable(s->meth);
        free_variable(s->obj);
        for (variable::t v : s->args) free_variable(v);
        return;
      }
      case EK::Proved_unreachable: return;
    }
  }
}

variable::Set variables_usage_named(named n, FvOpts o, bool all_used_variables) {
  variable::Set free;
  auto free_variable = [&](variable::t fv) { free = free.add(fv); };
  switch (n->kind) {
    case NK::Symbol: case NK::Const: case NK::Allocated_const: case NK::Read_mutable: case NK::Read_symbol_field:
      break;
    case NK::Set_of_closures: {
      // Sets of closures are, well, closed---except for the free variable
      // and specialised argument lists, which may identify variables
      // currently in scope outside of the closure.
      auto* s = static_cast<const NSet_of_closures*>(n)->set;
      s->free_vars.iter([&](variable::t, const SpecialisedTo& renamed_to) { free_variable(renamed_to.var); });
      s->specialised_args.iter([&](variable::t, const SpecialisedTo& spec_to) { free_variable(spec_to.var); });
      break;
    }
    case NK::Project_closure: free_variable(static_cast<const NProject_closure*>(n)->p.set_of_closures); break;
    case NK::Project_var:
      if (!o.ignore_uses_in_project_var) free_variable(static_cast<const NProject_var*>(n)->p.closure);
      break;
    case NK::Move_within_set_of_closures:
      free_variable(static_cast<const NMove_within_set_of_closures*>(n)->m.closure);
      break;
    case NK::Prim:
      for (variable::t v : static_cast<const NPrim*>(n)->args) free_variable(v);
      break;
    case NK::Expr:
      free = variable::Set::union_(variables_usage(static_cast<const NExpr*>(n)->expr, o, all_used_variables), free);
      break;
  }
  return free;
}
}  // namespace

variable::Set free_variables(t tree, FvOpts o) { return variables_usage(tree, o, false); }
variable::Set free_variables_named(named n, bool ignore_uses_in_project_var) {
  FvOpts o;
  o.ignore_uses_in_project_var = ignore_uses_in_project_var;
  return variables_usage_named(n, o, false);
}
variable::Set used_variables(t tree, FvOpts o) { return variables_usage(tree, o, true); }
variable::Set used_variables_named(named n, bool ignore_uses_in_project_var) {
  FvOpts o;
  o.ignore_uses_in_project_var = ignore_uses_in_project_var;
  return variables_usage_named(n, o, true);
}

t create_let(variable::t var, named defining_expr, t body) {
  // (-dflambda-let: the creation's backtrace is not ported)
  variable::Set free_vars_of_defining_expr;
  const Let* inner = nullptr;
  if (auto* e = as<NExpr>(defining_expr))
    if (auto* l = as<Let>(e->expr))
      if (auto* v = as<Var>(l->body); v && variable::equal(l->var, v->var)) inner = l;
  if (inner) {
    defining_expr = inner->defining_expr;
    free_vars_of_defining_expr = inner->free_vars_of_defining_expr;
  } else {
    free_vars_of_defining_expr = free_variables_named(defining_expr);
  }
  // (the record's fields right to left: free_vars_of_body first -- pure)
  variable::Set free_vars_of_body = free_variables(body);
  return make<Let>(Let{{EK::Let}, var, defining_expr, body, free_vars_of_defining_expr, free_vars_of_body});
}

namespace with_free_variables {
t create_let_reusing_defining_expr(variable::t var, const WithFvNamed& def, t body) {
  return make<Let>(Let{{EK::Let}, var, def.n, body, def.free_vars, free_variables(body)});
}
t create_let_reusing_body(variable::t var, named defining_expr, const WithFvExpr& body) {
  return make<Let>(Let{{EK::Let}, var, defining_expr, body.expr, free_variables_named(defining_expr), body.free_vars});
}
t create_let_reusing_both(variable::t var, const WithFvNamed& def, const WithFvExpr& body) {
  return make<Let>(Let{{EK::Let}, var, def.n, body.expr, def.free_vars, body.free_vars});
}
}  // namespace with_free_variables

// ---- iter_general, iter_lets, map_lets -------------------------------------------
namespace {
struct IterGeneral {
  bool toplevel;
  FnRef<void(t)> f;
  FnRef<void(named)> f_named;
  void aux(t e) {
    for (;;) {
      if (auto* l = as<Let>(e)) {
        // iter_lets t ~for_defining_expr:(fun _ n -> aux_named n)
        //   ~for_last_body:aux ~for_each_let:f
        f(e);
        aux_named(l->defining_expr);
        e = l->body;
        continue;
      }
      f(e);
      switch (e->kind) {
        case EK::Var: case EK::Apply: case EK::Assign: case EK::Send: case EK::Proved_unreachable:
        case EK::Static_raise: case EK::Let: return;
        case EK::Let_mutable: e = static_cast<const Let_mutable*>(e)->body; continue;
        case EK::Try_with: {
          auto* tw = static_cast<const Try_with*>(e);
          aux(tw->body);
          e = tw->handler;
          continue;
        }
        case EK::While: {
          auto* w = static_cast<const While*>(e);
          aux(w->cond);
          e = w->body;
          continue;
        }
        case EK::Static_catch: {
          auto* c = static_cast<const Static_catch*>(e);
          aux(c->body);
          e = c->handler;
          continue;
        }
        case EK::For: e = static_cast<const For*>(e)->body; continue;
        case EK::If_then_else: {
          auto* i = static_cast<const If_then_else*>(e);
          aux(i->ifso);
          e = i->ifnot;
          continue;
        }
        case EK::Switch: {
          auto* s = static_cast<const Switch*>(e);
          for (const SwitchCase& c : s->consts) aux(c.action);
          for (const SwitchCase& c : s->blocks) aux(c.action);
          if (!s->failaction) return;
          e = s->failaction;
          continue;
        }
        case EK::String_switch: {
          auto* s = static_cast<const String_switch*>(e);
          for (const StringCase& c : s->cases) aux(c.action);
          if (!s->def) return;
          e = s->def;
          continue;
        }
      }
    }
  }
  void aux_named(named n) {
    f_named(n);
    if (auto* s = as<NSet_of_closures>(n)) {
      if (!toplevel)
        s->set->function_decls->funs.iter([&](variable::t, const FunctionDeclaration* d) { aux(d->body); });
    } else if (auto* e = as<NExpr>(n)) {
      aux(e->expr);
    }
  }
};
}  // namespace

void iter_general(bool toplevel, FnRef<void(t)> f, FnRef<void(named)> f_named, t expr) {
  IterGeneral{toplevel, f, f_named}.aux(expr);
}
void iter_general_named(bool toplevel, FnRef<void(t)> f, FnRef<void(named)> f_named, named n) {
  IterGeneral{toplevel, f, f_named}.aux_named(n);
}

void iter_lets(t e, FnRef<void(variable::t, named)> for_defining_expr, FnRef<void(t)> for_last_body,
               FnRef<void(t)> for_each_let) {
  while (auto* l = as<Let>(e)) {
    for_each_let(e);
    for_defining_expr(l->var, l->defining_expr);
    e = l->body;
  }
  for_last_body(e);
}

t map_lets(t e, FnRef<named(variable::t, named)> for_defining_expr, FnRef<t(t)> for_last_body,
           FnRef<t(t)> after_rebuild) {
  struct Rev {
    variable::t var;
    named defining_expr;
    t original;  // null: None
  };
  std::vector<Rev> rev_lets;  // oldest first
  while (auto* l = as<Let>(e)) {
    named new_defining_expr = for_defining_expr(l->var, l->defining_expr);
    t original = new_defining_expr == l->defining_expr ? e : nullptr;
    rev_lets.push_back({l->var, new_defining_expr, original});
    e = l->body;
  }
  t last_body = for_last_body(e);
  // As soon as we see a change, we have to rebuild that [Let] and every
  // outer one.
  bool seen_change = last_body != e;
  t acc = last_body;
  for (std::size_t k = rev_lets.size(); k-- > 0;) {
    const Rev& r = rev_lets[k];
    t let_expr;
    if (r.original && !seen_change) let_expr = r.original;
    else {
      seen_change = true;
      let_expr = create_let(r.var, r.defining_expr, acc);
    }
    t new_let = after_rebuild(let_expr);
    if (new_let != let_expr) seen_change = true;
    acc = new_let;
  }
  return acc;
}

t map_defining_expr_of_let(const Let* let_expr, FnRef<named(named)> f) {
  named defining_expr = f(let_expr->defining_expr);
  if (defining_expr == let_expr->defining_expr) return let_expr;
  variable::Set fv = free_variables_named(defining_expr);
  return make<Let>(Let{{EK::Let}, let_expr->var, defining_expr, let_expr->body, fv, let_expr->free_vars_of_body});
}

// ---- free symbols ----------------------------------------------------------------
namespace {
void free_symbols_helper(symbol::Set& symbols, named n) {
  if (auto* s = as<NSymbol>(n)) symbols = symbols.add(s->sym);
  else if (auto* r = as<NRead_symbol_field>(n)) symbols = symbols.add(r->sym);
  else if (auto* soc = as<NSet_of_closures>(n))
    soc->set->function_decls->funs.iter([&](variable::t, const FunctionDeclaration* d) {
      symbols = symbol::Set::union_(d->free_symbols, symbols);
    });
}
}  // namespace

symbol::Set free_symbols(t expr) {
  symbol::Set symbols;
  iter_general(true, [](t) {}, [&](named n) { free_symbols_helper(symbols, n); }, expr);
  return symbols;
}
symbol::Set free_symbols_named(named n) {
  symbol::Set symbols;
  iter_general_named(true, [](t) {}, [&](named m) { free_symbols_helper(symbols, m); }, n);
  return symbols;
}

namespace {
void free_symbols_allocated_constant_helper(symbol::Set& symbols, constant_defining_value c) {
  using K = ConstantDefiningValue::Kind;
  switch (c->kind) {
    case K::Allocated_const: break;
    case K::Block:
      for (const BlockField& f : c->fields)
        if (f.sym) symbols = symbols.add(f.sym);
      break;
    case K::Set_of_closures:
      symbols = symbol::Set::union_(symbols, free_symbols_named(n_set_of_closures(c->set)));
      break;
    case K::Project_closure: symbols = symbols.add(c->sym); break;
  }
}
}  // namespace

symbol::Set free_symbols_program(const Program& program) {
  symbol::Set symbols;
  for (program_body p = program.program_body;; p = p->body) {
    using K = ProgramBody::Kind;
    switch (p->kind) {
      case K::Let_symbol: free_symbols_allocated_constant_helper(symbols, p->def); break;
      case K::Let_rec_symbol:
        for (const SymbolBinding& b : p->defs) free_symbols_allocated_constant_helper(symbols, b.def);
        break;
      case K::Initialize_symbol:
        for (t field : p->fields) symbols = symbol::Set::union_(symbols, free_symbols(field));
        break;
      case K::Effect: symbols = symbol::Set::union_(symbols, free_symbols(p->expr)); break;
      case K::End:
        // Note that there is no need to count the [imported_symbols].
        return symbols.add(p->sym);
    }
  }
}

// ---- function declarations, sets of closures ------------------------------------
const FunctionDeclaration* create_function_declaration(Slice<Parameter> params, t body, bool stub, debuginfo::t dbg,
                                                       lambda::InlineAttribute inline_,
                                                       lambda::SpecialiseAttribute specialise, bool is_a_functor,
                                                       variable::t closure_origin, lambda::PollAttribute poll) {
  using IK = lambda::InlineAttribute::Kind;
  if (stub && (inline_.kind == IK::Always_inline || inline_.kind == IK::Hint_inline || inline_.kind == IK::Unroll)) {
    Formatter f;
    print(f, body);
    misc::fatal_error("Stubs may not be annotated as [Always_inline], [Hint_inline] or [Unroll]: " + f.contents());
  }
  if (stub && specialise == lambda::SpecialiseAttribute::Always_specialise) {
    Formatter f;
    print(f, body);
    misc::fatal_error("Stubs may not be annotated as [Always_specialise]: " + f.contents());
  }
  symbol::Set free_symbols_ = free_symbols(body);
  variable::Set free_variables_ = free_variables(body);
  return make<FunctionDeclaration>(FunctionDeclaration{closure_origin, params, body, free_variables_, free_symbols_,
                                                       stub, dbg, inline_, specialise, is_a_functor, poll});
}

const FunctionDeclarations* create_function_declarations(bool is_classic_mode,
                                                         variable::Map<const FunctionDeclaration*> funs) {
  compilation_unit::t cu = compilation_unit::get_current_exn();
  set_of_closures_id::t id = set_of_closures_id::create(cu);
  // Set_of_closures_origin.create: the id itself
  return make<FunctionDeclarations>(FunctionDeclarations{is_classic_mode, id, id, funs});
}

const SetOfClosures* create_set_of_closures(const FunctionDeclarations* function_decls,
                                            variable::Map<SpecialisedTo> free_vars,
                                            variable::Map<SpecialisedTo> specialised_args,
                                            variable::Map<variable::t> direct_call_surrogates) {
  if (clflags::flambda_invariant_checks) {
    variable::Set all_fun_vars = function_decls->funs.keys();
    variable::Set expected_free_vars = function_decls->funs.fold(
        [&](variable::t, const FunctionDeclaration* d, variable::Set acc) {
          variable::Set fv = variable::Set::diff(
              d->free_variables, variable::Set::union_(parameter::set_vars(d->params), all_fun_vars));
          return variable::Set::union_(fv, acc);
        },
        variable::Set{});
    // CR-soon pchambart: We do not seem to be able to maintain the
    // invariant that if a variable is not used inside the closure, it is
    // not used outside either.  (flambda.ml)
    variable::Set free_vars_domain = free_vars.keys();
    if (!variable::Set::subset(expected_free_vars, free_vars_domain)) {
      Formatter f;
      fprintf(f,
              "create_set_of_closures: [free_vars] mapping of variables bound by the closure(s) is wrong.  (Must map "
              "at least %a but only maps %a.)@ \nfunction_decls:@ %a",
              [&](Formatter& g) { variable::print_set(g, expected_free_vars); },
              [&](Formatter& g) { variable::print_set(g, free_vars_domain); },
              pr(print_function_declarations, function_decls));
      misc::fatal_error(f.contents());
    }
    variable::Set all_params = function_decls->funs.fold(
        [&](variable::t, const FunctionDeclaration* d, variable::Set acc) {
          return variable::Set::union_(parameter::set_vars(d->params), acc);
        },
        variable::Set{});
    variable::Set spec_args_domain = specialised_args.keys();
    if (!variable::Set::subset(spec_args_domain, all_params)) {
      Formatter f;
      fprintf(f,
              "create_set_of_closures: [specialised_args] maps variable(s) that are not parameters of the given "
              "function declarations.  specialised_args domain=%a all_params=%a \nfunction_decls:@ %a",
              [&](Formatter& g) { variable::print_set(g, spec_args_domain); },
              [&](Formatter& g) { variable::print_set(g, all_params); },
              pr(print_function_declarations, function_decls));
      misc::fatal_error(f.contents());
    }
  }
  return make<SetOfClosures>(SetOfClosures{function_decls, free_vars, specialised_args, direct_call_surrogates});
}

const FunctionDeclaration* update_body_of_function_declaration(const FunctionDeclaration* d, t body) {
  return update_function_decl_params_and_body(d, d->params, body);
}

const FunctionDeclaration* update_function_decl_params_and_body(const FunctionDeclaration* d, Slice<Parameter> params,
                                                                t body) {
  symbol::Set fs = free_symbols(body);
  variable::Set fv = free_variables(body);
  FunctionDeclaration r = *d;
  r.params = params;
  r.body = body;
  r.free_variables = fv;
  r.free_symbols = fs;
  return make<FunctionDeclaration>(r);
}

const FunctionDeclaration* update_function_declaration(const FunctionDeclaration* d, Slice<Parameter> params, t body) {
  return update_function_decl_params_and_body(d, params, body);
}

const FunctionDeclarations* create_function_declarations_with_origin(bool is_classic_mode,
                                                                     variable::Map<const FunctionDeclaration*> funs,
                                                                     set_of_closures_id::t set_of_closures_origin) {
  set_of_closures_id::t id = set_of_closures_id::create(compilation_unit::get_current_exn());
  return make<FunctionDeclarations>(FunctionDeclarations{is_classic_mode, id, set_of_closures_origin, funs});
}

const FunctionDeclarations* update_function_declarations(const FunctionDeclarations* fds,
                                                         variable::Map<const FunctionDeclaration*> funs) {
  set_of_closures_id::t id = set_of_closures_id::create(compilation_unit::get_current_exn());
  return make<FunctionDeclarations>(FunctionDeclarations{fds->is_classic_mode, id, fds->set_of_closures_origin, funs});
}

const FunctionDeclarations* import_function_declarations_for_pack(
    const FunctionDeclarations* fds, FnRef<set_of_closures_id::t(set_of_closures_id::t)> import_set_of_closures_id,
    FnRef<set_of_closures_id::t(set_of_closures_id::t)> import_set_of_closures_origin) {
  set_of_closures_id::t id = import_set_of_closures_id(fds->set_of_closures_id);
  set_of_closures_id::t origin = import_set_of_closures_origin(fds->set_of_closures_origin);
  return make<FunctionDeclarations>(FunctionDeclarations{fds->is_classic_mode, id, origin, fds->funs});
}

variable::Set used_params(const FunctionDeclaration* d) {
  return parameter::set_vars(d->params).filter([&](variable::t p) { return d->free_variables.mem(p); });
}

int compare_const(const Const& a, const Const& b) {
  if (a.kind != b.kind) return a.kind == Const::Kind::Int ? -1 : 1;
  return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;  // (Char.compare: the codes)
}

int compare_block_field(const BlockField& a, const BlockField& b) {
  if (a.sym && b.sym) return symbol::compare(a.sym, b.sym);
  if (!a.sym && !b.sym) return compare_const(a.c, b.c);
  return a.sym ? -1 : 1;
}

int compare_constant_defining_value(constant_defining_value a, constant_defining_value b) {
  using K = ConstantDefiningValue::Kind;
  if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;  // (constructor order)
  switch (a->kind) {
    case K::Allocated_const: return allocated_const::compare(a->c, b->c);
    case K::Block: {
      if (a->tag != b->tag) return a->tag < b->tag ? -1 : 1;
      // Misc.Stdlib.List.compare
      for (std::size_t k = 0;; ++k) {
        if (k == a->fields.size()) return k == b->fields.size() ? 0 : -1;
        if (k == b->fields.size()) return 1;
        if (int c = compare_block_field(a->fields[k], b->fields[k]); c != 0) return c;
      }
    }
    case K::Set_of_closures:
      return unit_id::compare(a->set->function_decls->set_of_closures_id, b->set->function_decls->set_of_closures_id);
    case K::Project_closure: {
      if (int c = symbol::compare(a->sym, b->sym); c != 0) return c;
      return variable::compare(a->closure_id, b->closure_id);
    }
  }
  return 0;
}

bool equal_call_kind(const CallKind& a, const CallKind& b) {
  if (!a.direct || !b.direct) return !a.direct && !b.direct;
  return variable::equal(a.direct, b.direct);
}

bool equal_specialised_to(const SpecialisedTo& a, const SpecialisedTo& b) {
  if (!variable::equal(a.var, b.var)) return false;
  if (!a.projection || !b.projection) return !a.projection && !b.projection;
  return projection::equal(a.projection, b.projection);
}

void print_function_declaration_pub(Formatter& ppf, variable::t var, const FunctionDeclaration* f) {
  print_function_declaration(ppf, var, f);
}
}  // namespace flambda
}  // namespace cppcaml::typing
