// Port of middle_end/flambda/inconstant_idents.ml (see
// inconstant_idents.hpp).  The tables are only queried for membership: the
// result does not depend on their iteration orders, so they are plain hash
// maps here.
#include "cppcaml/typing/inconstant_idents.hpp"

#include <deque>
#include <unordered_map>
#include <variant>
#include <vector>

#include "cppcaml/typing/import_approx.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::inconstant_idents {

using namespace flambda;

namespace {
// dep = Closure of Set_of_closures_id.t | Var of Variable.t | Symbol of
// Symbol.t | Symbol_field of Symbol_field.t
struct Dep {
  enum class Kind : unsigned char { Closure, Var, Symbol, Symbol_field } kind;
  set_of_closures_id::t closure = nullptr;
  variable::t var = nullptr;
  symbol::t sym = nullptr;
  long field = 0;
};
using Deps = std::vector<Dep>;
Dep dep_var(variable::t v) { return {Dep::Kind::Var, nullptr, v}; }
Dep dep_closure(set_of_closures_id::t c) { return {Dep::Kind::Closure, c}; }

// state = Not_constant | Implication of dep list
struct State {
  bool not_constant;
  Deps deps;
};

struct VarHash {
  std::size_t operator()(variable::t v) const { return static_cast<std::size_t>(variable::hash(v)); }
};
struct VarEq {
  bool operator()(variable::t a, variable::t b) const { return variable::equal(a, b); }
};
struct IdHash {
  std::size_t operator()(set_of_closures_id::t v) const { return static_cast<std::size_t>(v->id); }
};
struct IdEq {
  bool operator()(set_of_closures_id::t a, set_of_closures_id::t b) const { return unit_id::compare(a, b) == 0; }
};
struct SymHash {
  std::size_t operator()(symbol::t s) const { return static_cast<std::size_t>(symbol::hash(s)); }
};
struct SymEq {
  bool operator()(symbol::t a, symbol::t b) const { return symbol::equal(a, b); }
};
struct SFHash {
  std::size_t operator()(const std::pair<symbol::t, long>& p) const {
    return static_cast<std::size_t>(symbol::hash(p.first)) * 31u + static_cast<std::size_t>(p.second);
  }
};
struct SFEq {
  bool operator()(const std::pair<symbol::t, long>& a, const std::pair<symbol::t, long>& b) const {
    return symbol::equal(a.first, b.first) && a.second == b.second;
  }
};
}  // namespace

struct Result {
  std::unordered_map<variable::t, State, VarHash, VarEq> id;
  std::unordered_map<set_of_closures_id::t, State, IdHash, IdEq> closure;
};

namespace {
struct Inconstants {
  const Program& program;
  compilation_unit::t compilation_unit;
  symbol::Set imported_symbols;
  Result& res;  // variables, closures
  std::unordered_map<symbol::t, State, SymHash, SymEq> symbols;
  std::unordered_map<std::pair<symbol::t, long>, State, SFHash, SFEq> symbol_fields;
  std::deque<Deps> mark_queue;

  template <class Tbl, class K>
  void mark_in(Tbl& tbl, const K& k) {
    auto it = tbl.find(k);
    if (it == tbl.end()) {
      tbl.emplace(k, State{true, {}});
      return;
    }
    if (it->second.not_constant) return;
    Deps deps = std::move(it->second.deps);
    it->second = State{true, {}};
    mark_queue.push_back(std::move(deps));
  }
  // adds 'dep in NC'
  void mark_dep(const Dep& d) {
    switch (d.kind) {
      case Dep::Kind::Var: mark_in(res.id, d.var); break;
      case Dep::Kind::Closure: mark_in(res.closure, d.closure); break;
      case Dep::Kind::Symbol: mark_in(symbols, d.sym); break;
      case Dep::Kind::Symbol_field: mark_in(symbol_fields, std::make_pair(d.sym, d.field)); break;
    }
  }
  void mark_deps(const Deps& deps) {
    for (const Dep& d : deps) mark_dep(d);
  }
  void complete_marking() {
    while (!mark_queue.empty()) {
      Deps deps = std::move(mark_queue.front());
      mark_queue.pop_front();
      mark_deps(deps);
    }
  }
  // adds 'curr in NC'
  void mark_curr(const Deps& curr) {
    mark_deps(curr);
    complete_marking();
  }
  template <class Tbl, class K>
  void implication_in(Tbl& tbl, const K& k, const Deps& curr) {
    auto it = tbl.find(k);
    if (it == tbl.end()) {
      tbl.emplace(k, State{false, curr});
      return;
    }
    if (it->second.not_constant) {
      mark_deps(curr);
      complete_marking();
      return;
    }
    // List.rev_append curr deps
    Deps deps(curr.rbegin(), curr.rend());
    deps.insert(deps.end(), it->second.deps.begin(), it->second.deps.end());
    it->second.deps = std::move(deps);
  }
  // adds in the tables 'dep in NC => curr in NC'
  void register_implication(const Dep& in_nc, const Deps& implies_in_nc) {
    switch (in_nc.kind) {
      case Dep::Kind::Var: implication_in(res.id, in_nc.var, implies_in_nc); break;
      case Dep::Kind::Closure: implication_in(res.closure, in_nc.closure, implies_in_nc); break;
      case Dep::Kind::Symbol: implication_in(symbols, in_nc.sym, implies_in_nc); break;
      case Dep::Kind::Symbol_field: {
        auto key = std::make_pair(in_nc.sym, in_nc.field);
        if (symbol_fields.find(key) == symbol_fields.end() && imported_symbols.mem(in_nc.sym)) {
          // There is no information available about the contents of
          // imported symbols, so we must consider all their fields as
          // inconstant.
          symbol_fields.emplace(key, State{true, {}});
          mark_deps(implies_in_nc);
          complete_marking();
        } else {
          implication_in(symbol_fields, key, implies_in_nc);
        }
        break;
      }
    }
  }

  void mark_var(variable::t var, const Deps& curr) { register_implication(dep_var(var), curr); }
  void mark_vars(Slice<variable::t> vars, const Deps& curr) {
    for (variable::t v : vars) mark_var(v, curr);
  }

  // First loop: iterates on the tree to mark dependencies.
  void mark_loop(bool toplevel, const Deps& curr, t flam) {
    switch (flam->kind) {
      case EK::Let: {
        auto* l = static_cast<const Let*>(flam);
        mark_named(toplevel, Deps{dep_var(l->var)}, l->defining_expr);
        // adds 'var in NC => curr in NC'
        mark_var(l->var, curr);
        mark_loop(toplevel, curr, l->body);
        return;
      }
      case EK::Let_mutable: {
        auto* l = static_cast<const Let_mutable*>(flam);
        mark_var(l->initial_value, curr);
        mark_loop(toplevel, curr, l->body);
        return;
      }
      case EK::Var: mark_var(static_cast<const Var*>(flam)->var, curr); return;
      // Not constant cases: we mark directly 'curr in NC' and mark bound
      // variables as in NC also
      case EK::Assign: mark_curr(curr); return;
      case EK::Try_with: {
        auto* tw = static_cast<const Try_with*>(flam);
        mark_curr(Deps{dep_var(tw->var)});
        mark_curr(curr);
        mark_loop(toplevel, {}, tw->body);
        mark_loop(toplevel, {}, tw->handler);
        return;
      }
      case EK::Static_catch: {
        auto* c = static_cast<const Static_catch*>(flam);
        for (const CatchVar& v : c->vars) mark_curr(Deps{dep_var(v.var)});
        mark_curr(curr);
        mark_loop(toplevel, {}, c->body);
        mark_loop(toplevel, {}, c->handler);
        return;
      }
      case EK::For: {
        auto* f = static_cast<const For*>(flam);
        mark_curr(Deps{dep_var(f->bound_var)});
        mark_var(f->from_value, curr);
        mark_var(f->to_value, curr);
        mark_curr(curr);
        mark_loop(false, {}, f->body);
        return;
      }
      case EK::While: {
        auto* w = static_cast<const While*>(flam);
        mark_curr(curr);
        mark_loop(toplevel, {}, w->cond);
        mark_loop(false, {}, w->body);
        return;
      }
      case EK::If_then_else: {
        auto* i = static_cast<const If_then_else*>(flam);
        mark_curr(curr);
        mark_var(i->cond, curr);
        mark_loop(toplevel, {}, i->ifso);
        mark_loop(toplevel, {}, i->ifnot);
        return;
      }
      case EK::Static_raise:
        mark_curr(curr);
        for (variable::t v : static_cast<const Static_raise*>(flam)->args) mark_var(v, curr);
        return;
      case EK::Apply: {
        auto* a = static_cast<const Apply*>(flam);
        mark_curr(curr);
        mark_var(a->func, curr);
        mark_vars(a->args, curr);
        return;
      }
      case EK::Switch: {
        auto* s = static_cast<const Switch*>(flam);
        mark_curr(curr);
        mark_var(s->scrutinee, curr);
        for (const SwitchCase& c : s->consts) mark_loop(toplevel, {}, c.action);
        for (const SwitchCase& c : s->blocks) mark_loop(toplevel, {}, c.action);
        if (s->failaction) mark_loop(toplevel, {}, s->failaction);
        return;
      }
      case EK::String_switch: {
        auto* s = static_cast<const String_switch*>(flam);
        mark_curr(curr);
        mark_var(s->scrutinee, curr);
        for (const StringCase& c : s->cases) mark_loop(toplevel, {}, c.action);
        if (s->def) mark_loop(toplevel, {}, s->def);
        return;
      }
      case EK::Send: {
        auto* s = static_cast<const Send*>(flam);
        mark_curr(curr);
        mark_var(s->meth, curr);
        mark_var(s->obj, curr);
        for (variable::t a : s->args) mark_var(a, curr);
        return;
      }
      case EK::Proved_unreachable: mark_curr(curr); return;
    }
  }

  void mark_named(bool toplevel, const Deps& curr, named n) {
    using PK = clambda::Primitive::K;
    switch (n->kind) {
      case NK::Set_of_closures: mark_loop_set_of_closures(curr, static_cast<const NSet_of_closures*>(n)->set); return;
      case NK::Const: case NK::Allocated_const: return;
      case NK::Read_mutable: mark_curr(curr); return;
      case NK::Symbol: {
        symbol::t sym = static_cast<const NSymbol*>(n)->sym;
        compilation_unit::t current_unit = compilation_unit::get_current_exn();
        if (compilation_unit::equal(current_unit, symbol::compilation_unit(sym))) return;
        // Constant when 'for_clambda' means: can be a symbol (which is
        // obviously the case here) with a known approximation.  If this
        // condition is not satisfied we mark as inconstant to reflect the
        // fact that the symbol's contents are unknown and thus prevent
        // attempts to examine it.  (This is a bit of a hack.)
        if (import_approx::import_symbol(sym)->descr.kind == simple_value_approx::DK::Value_unresolved)
          mark_curr(curr);
        return;
      }
      case NK::Read_symbol_field: {
        auto* r = static_cast<const NRead_symbol_field*>(n);
        register_implication(Dep{Dep::Kind::Symbol_field, nullptr, nullptr, r->sym, r->field}, curr);
        return;
      }
      case NK::Project_closure: {
        const auto& p = static_cast<const NProject_closure*>(n)->p;
        if (variable::in_compilation_unit(p.closure_id, compilation_unit)) mark_var(p.set_of_closures, curr);
        else mark_curr(curr);
        return;
      }
      case NK::Move_within_set_of_closures: {
        const auto& m = static_cast<const NMove_within_set_of_closures*>(n)->m;
        // CR-someday mshinwell: We should be able to deem these
        // projections (same for the cases below) as constant when from
        // another compilation unit, but there isn't code to handle this yet.
        if (variable::in_compilation_unit(m.start_from, compilation_unit)) {
          if (!variable::in_compilation_unit(m.move_to, compilation_unit))
            misc::fatal_error("Inconstant_idents: Move_within_set_of_closures");
          mark_var(m.closure, curr);
        } else {
          mark_curr(curr);
        }
        return;
      }
      case NK::Project_var: {
        const auto& p = static_cast<const NProject_var*>(n)->p;
        if (variable::in_compilation_unit(p.closure_id, compilation_unit)) mark_var(p.closure, curr);
        else mark_curr(curr);
        return;
      }
      case NK::Prim: {
        auto* p = static_cast<const NPrim*>(n);
        const clambda::Primitive& prim = *p->prim;
        bool floatarray = prim.array == lambda::ArrayKind::Pfloatarray;
        bool immutable = prim.mut == MutableFlag::Immutable;
        // Constant constructors: those expressions are constant if all
        // their parameters are (inconstant_idents.ml)
        if (prim.kind == PK::Pmakeblock && immutable) {
          mark_vars(p->args, curr);
        } else if (prim.kind == PK::Pmakearray && floatarray) {
          if (immutable || toplevel) mark_vars(p->args, curr);
          else mark_curr(curr);
        } else if (prim.kind == PK::Pduparray && floatarray && p->args.size() == 1) {
          if (immutable || toplevel) mark_var(p->args[0], curr);
          else mark_curr(curr);
        } else if (prim.kind == PK::Pduparray) {
          // See Lift_constants
          mark_curr(curr);
        } else if (prim.kind == PK::Pfield && p->args.size() == 1) {
          mark_curr(curr);
          mark_var(p->args[0], curr);
        } else {
          mark_curr(curr);
          mark_vars(p->args, curr);
        }
        return;
      }
      case NK::Expr: mark_loop(toplevel, curr, static_cast<const NExpr*>(n)->expr); return;
    }
  }

  void mark_loop_set_of_closures(const Deps& curr, const SetOfClosures* s) {
    set_of_closures_id::t id = s->function_decls->set_of_closures_id;
    // If a function in the set of closures is specialised, do not consider
    // it constant, unless all specialised args are also constant.
    s->specialised_args.iter(
        [&](variable::t, const SpecialisedTo& spec_arg) { register_implication(dep_var(spec_arg.var), Deps{dep_closure(id)}); });
    // adds 'function_decls in NC => curr in NC'
    register_implication(dep_closure(id), curr);
    // a closure is constant if its free variables are constants.
    s->free_vars.iter([&](variable::t inner_id, const SpecialisedTo& var) {
      register_implication(dep_var(var.var), Deps{dep_var(inner_id), dep_closure(id)});
    });
    s->function_decls->funs.iter([&](variable::t fun_id, const FunctionDeclaration* ffunc) {
      // for each function f in a closure c 'c in NC => f'
      register_implication(dep_closure(id), Deps{dep_var(fun_id)});
      // function parameters are in NC unless specialised
      for (const Parameter& param : ffunc->params) {
        if (const SpecialisedTo* outer = s->specialised_args.find_opt(param.var))
          register_implication(dep_var(outer->var), Deps{dep_var(param.var)});
        else
          mark_curr(Deps{dep_var(param.var)});
      }
      mark_loop(false, {}, ffunc->body);
    });
  }

  void mark_constant_defining_value(constant_defining_value c) {
    if (c->kind == ConstantDefiningValue::Kind::Set_of_closures) mark_loop_set_of_closures({}, c->set);
  }

  void mark_program() {
    for (program_body p = program.program_body;; p = p->body) {
      using K = ProgramBody::Kind;
      switch (p->kind) {
        case K::End: return;
        case K::Initialize_symbol:
          for (std::size_t i = 0; i < p->fields.size(); ++i)
            mark_loop(true, Deps{Dep{Dep::Kind::Symbol, nullptr, nullptr, p->sym},
                                 Dep{Dep::Kind::Symbol_field, nullptr, nullptr, p->sym, static_cast<long>(i)}},
                      p->fields[i]);
          break;
        case K::Effect: mark_loop(true, {}, p->expr); break;
        case K::Let_symbol: mark_constant_defining_value(p->def); break;
        case K::Let_rec_symbol:
          for (const SymbolBinding& b : p->defs) mark_constant_defining_value(b.def);
          break;
      }
    }
  }
};
}  // namespace

std::shared_ptr<const Result> inconstants_on_program(compilation_unit::t compilation_unit, const Program& program) {
  auto res = std::make_shared<Result>();
  Inconstants i{program, compilation_unit, program.imported_symbols, *res, {}, {}, {}};
  i.mark_program();
  return res;
}

bool variable(variable::t var, const Result& r) {
  auto it = r.id.find(var);
  return it != r.id.end() && it->second.not_constant;
}

bool closure(set_of_closures_id::t cl, const Result& r) {
  auto it = r.closure.find(cl);
  return it != r.closure.end() && it->second.not_constant;
}

}  // namespace cppcaml::typing::inconstant_idents
