// Port of middle_end/flambda/freshening.ml (see freshening.hpp).
#include "cppcaml/typing/freshening.hpp"

#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::freshening {

using format::Formatter;
using format::fprintf;

namespace {
const Tbl* empty_tbl() {
  static const Tbl* e = make<Tbl>();
  return e;
}
T active(const Tbl& tbl) { return T{make<Tbl>(tbl)}; }

// Slice cons: x :: l
Slice<variable::t> cons(variable::t x, Slice<variable::t> l) {
  std::vector<variable::t> v;
  v.reserve(l.size() + 1);
  v.push_back(x);
  v.insert(v.end(), l.begin(), l.end());
  return slice(v);
}

Tbl add_sb_var(Tbl sb, variable::t id, variable::t id2) {
  sb.sb_var = sb.sb_var.add(id, id2);
  if (const Slice<variable::t>* pre_vars = sb.back_var.find_opt(id)) {
    Slice<variable::t> pv = *pre_vars;  // List.fold_left
    for (variable::t pre_id : pv) sb = add_sb_var(sb, pre_id, id2);
  }
  const Slice<variable::t>* l = sb.back_var.find_opt(id2);
  sb.back_var = sb.back_var.add(id2, cons(id, l ? *l : Slice<variable::t>{}));
  return sb;
}

Tbl add_sb_mutable_var(Tbl sb, variable::t id, variable::t id2) {
  sb.sb_mutable_var = sb.sb_mutable_var.add(id, id2);
  if (const Slice<variable::t>* pre_vars = sb.back_mutable_var.find_opt(id)) {
    Slice<variable::t> pv = *pre_vars;
    for (variable::t pre_id : pv) sb = add_sb_mutable_var(sb, pre_id, id2);
  }
  const Slice<variable::t>* l = sb.back_mutable_var.find_opt(id2);
  sb.back_mutable_var = sb.back_mutable_var.add(id2, cons(id, l ? *l : Slice<variable::t>{}));
  return sb;
}

std::pair<variable::t, Tbl> active_add_variable(const Tbl& t, variable::t id) {
  variable::t id2 = variable::rename(id);
  return {id2, add_sb_var(t, id, id2)};
}

std::pair<Parameter, Tbl> active_add_parameter(const Tbl& t, const Parameter& param) {
  Parameter param2 = parameter::wrap(variable::rename(param.var));
  return {param2, add_sb_var(t, param.var, param2.var)};
}

// List.fold_right: the last parameter first
std::pair<std::vector<Parameter>, Tbl> active_add_parameters_prime(Tbl t, Slice<Parameter> params) {
  std::vector<Parameter> out(params.size());
  for (std::size_t k = params.size(); k-- > 0;) {
    auto [p2, t2] = active_add_parameter(t, params[k]);
    out[k] = p2;
    t = t2;
  }
  return {out, t};
}

variable::t active_find_var_exn(const Tbl& t, variable::t id) {
  if (const variable::t* v = t.sb_var.find_opt(id)) return *v;
  Formatter f;
  fprintf(f, "find_var: can't find %a@.", pr(variable::print, id));
  misc::fatal_error(f.contents());
}
}  // namespace

T empty_preserving_activation_state(const T& t) { return t.active ? T{empty_tbl()} : T{}; }
T activate(const T& t) { return t.active ? t : T{empty_tbl()}; }

void print(Formatter& ppf, const T& t) {
  if (!t.active) {
    fprintf(ppf, "Inactive");
    return;
  }
  const Tbl& tbl = *t.active;
  fprintf(ppf, "Active:@ ");
  tbl.sb_var.iter([&](variable::t v1, variable::t v2) {
    fprintf(ppf, "%a -> %a@ ", pr(variable::print, v1), pr(variable::print, v2));
  });
  tbl.sb_mutable_var.iter([&](variable::t v1, variable::t v2) {
    fprintf(ppf, "(mutable) %a -> %a@ ", pr(variable::print, v1), pr(variable::print, v2));
  });
  auto set_of = [](Slice<variable::t> l) {
    return variable::Set::of_list(std::vector<variable::t>(l.begin(), l.end()));
  };
  tbl.back_var.iter([&](variable::t v, Slice<variable::t> vars) {
    fprintf(ppf, "%a -> %a@ ", pr(variable::print, v), [&](Formatter& f) { variable::print_set(f, set_of(vars)); });
  });
  tbl.back_mutable_var.iter([&](variable::t v, Slice<variable::t> vars) {
    fprintf(ppf, "(mutable) %a -> %a@ ", pr(variable::print, v),
            [&](Formatter& f) { variable::print_set(f, set_of(vars)); });
  });
}

static_exception::t apply_static_exception(const T& t, static_exception::t i) {
  if (!t.active) return i;
  const static_exception::t* r = t.active->sb_exn.find_opt(i);
  return r ? *r : i;
}

std::pair<static_exception::t, T> add_static_exception(const T& t, static_exception::t i) {
  if (!t.active) return {i, t};
  static_exception::t i2 = static_exception::create();
  Tbl tbl = *t.active;
  tbl.sb_exn = tbl.sb_exn.add(i, i2);
  return {i2, active(tbl)};
}

std::pair<variable::t, T> add_variable(const T& t, variable::t id) {
  if (!t.active) return {id, t};
  auto [id2, tbl] = active_add_variable(*t.active, id);
  return {id2, active(tbl)};
}

std::pair<std::vector<variable::t>, T> add_variables_prime(const T& t0, Slice<variable::t> ids) {
  std::vector<variable::t> out(ids.size());
  T t = t0;
  for (std::size_t k = ids.size(); k-- > 0;) {
    auto [id2, t2] = add_variable(t, ids[k]);
    out[k] = id2;
    t = t2;
  }
  return {out, t};
}

std::pair<variable::t, T> add_mutable_variable(const T& t, variable::t id) {
  if (!t.active) return {id, t};
  variable::t id2 = variable::rename(id);  // Mutable_variable.rename
  return {id2, active(add_sb_mutable_var(*t.active, id, id2))};
}

variable::t apply_variable(const T& t, variable::t var) {
  if (!t.active) return var;
  const variable::t* r = t.active->sb_var.find_opt(var);
  return r ? *r : var;
}

variable::t apply_mutable_variable(const T& t, variable::t mut_var) {
  if (!t.active) return mut_var;
  const variable::t* r = t.active->sb_mutable_var.find_opt(mut_var);
  return r ? *r : mut_var;
}

const flambda::FunctionDeclarations* rewrite_recursive_calls_with_symbols(
    const T& t, const flambda::FunctionDeclarations* function_declarations,
    FnRef<symbol::t(variable::t)> make_closure_symbol) {
  if (!t.active) return function_declarations;
  symbol::Set all_free_symbols = function_declarations->funs.fold(
      [](variable::t, const flambda::FunctionDeclaration* d, symbol::Set syms) {
        return symbol::Set::union_(syms, d->free_symbols);
      },
      symbol::Set{});
  bool closure_symbols_used = false;
  symbol::Map<variable::t> closure_symbols = function_declarations->funs.fold(
      [&](variable::t var, const flambda::FunctionDeclaration*, symbol::Map<variable::t> map) {
        symbol::t sym = make_closure_symbol(var);
        if (all_free_symbols.mem(sym)) {
          closure_symbols_used = true;
          return map.add(sym, var);
        }
        return map;
      },
      symbol::Map<variable::t>{});
  // Don't waste time rewriting the function declaration(s) if there are no
  // occurrences of any of the closure symbols.
  if (!closure_symbols_used) return function_declarations;
  variable::Map<const flambda::FunctionDeclaration*> funs =
      function_declarations->funs.map([&](const flambda::FunctionDeclaration* ffun) {
        // CR-someday pchambart: This may be worth deep substituting below
        // the closures, but that means that we need to take care of
        // functions' free variables.
        flambda::t body = flambda_iterators::map_toplevel_named(
            [&](flambda::named n) -> flambda::named {
              if (auto* s = flambda::as<flambda::NSymbol>(n))
                if (const variable::t* v = closure_symbols.find_opt(s->sym)) return flambda::n_expr(flambda::var(*v));
              return n;
            },
            ffun->body);
        return flambda::update_body_of_function_declaration(ffun, body);
      });
  return flambda::update_function_declarations(function_declarations, funs);
}

bool does_not_freshen(const T& t, Slice<variable::t> vars) {
  if (!t.active) return true;
  for (variable::t v : vars)
    if (t.active->sb_var.mem(v)) return false;
  return true;
}

namespace project_var {
namespace {
// Variable.Map.print Variable.print (Identifiable.Make_map.print)
void print_map(Formatter& ppf, const variable::Map<variable::t>& m) {
  auto elts = [&](Formatter& f) {
    m.iter([&](variable::t id, variable::t v) {
      fprintf(f, "@ (@[%a@ %a@])", pr(variable::print, id), pr(variable::print, v));
    });
  };
  fprintf(ppf, "@[<1>{@[%a@ @]}@]", elts);
}

// Compose (T).compose ~earlier ~later
variable::Map<variable::t> compose_map(const variable::Map<variable::t>& earlier,
                                       const variable::Map<variable::t>& later) {
  if (variable::Map<variable::t>::equal([](variable::t a, variable::t b) { return variable::equal(a, b); }, earlier,
                                        later) ||
      later.is_empty())
    return earlier;
  return earlier.mapi([&](variable::t src_var, variable::t var) -> variable::t {
    if (later.mem(src_var)) {
      Formatter f;
      fprintf(f, "Freshening.Project_var.compose: domains of substitutions must be disjoint.  earlier=%a later=%a",
              pr(print_map, earlier), pr(print_map, later));
      misc::fatal_error(f.contents());
    }
    if (const variable::t* v = later.find_opt(var)) return *v;
    return var;
  });
}
}  // namespace

void print(Formatter& ppf, const T& t) {
  fprintf(ppf, "{ vars_within_closure %a, closure_id %a }", pr(print_map, t.vars_within_closure),
          pr(print_map, t.closure_id));
}

variable::t apply_closure_id(const T& t, variable::t closure_id) {
  if (const variable::t* v = t.closure_id.find_opt(closure_id)) return *v;
  return closure_id;
}

variable::t apply_var_within_closure(const T& t, variable::t var_in_closure) {
  if (const variable::t* v = t.vars_within_closure.find_opt(var_in_closure)) return *v;
  return var_in_closure;
}

T compose(const T& earlier, const T& later) {
  // (the record's fields right to left: closure_id first)
  variable::Map<variable::t> closure_id = compose_map(earlier.closure_id, later.closure_id);
  variable::Map<variable::t> vars = compose_map(earlier.vars_within_closure, later.vars_within_closure);
  return {vars, closure_id};
}
}  // namespace project_var

AppliedFunctionDecls apply_function_decls_and_free_vars(const T& t0, const variable::Map<SpecApprox>& fv0,
                                                        const flambda::FunctionDeclarations* func_decls,
                                                        bool only_freshen_parameters) {
  namespace F = flambda;
  // I.subst_free_vars fv t ~only_freshen_parameters
  variable::Map<SpecApprox> fv;
  T subst = t0;
  project_var::T pt;
  fv0.iter([&](variable::t id, const SpecApprox& lam) {
    variable::t id2 = id;
    if (!only_freshen_parameters && subst.active) {
      // new_subst_fv t id subst
      auto [nid, tbl] = active_add_variable(*subst.active, id);
      id2 = nid;
      subst = active(tbl);
      pt.vars_within_closure = pt.vars_within_closure.add(id, id2);
    }
    fv = fv.add(id2, lam);
  });
  // I.func_decls_subst of_closures t func_decls ~only_freshen_parameters
  if (!subst.active) return {fv, func_decls, subst, pt};
  Tbl sb = *subst.active;
  if (!only_freshen_parameters)
    func_decls->funs.iter([&](variable::t orig_id, const F::FunctionDeclaration*) {
      // new_subst_fun t orig_id subst
      variable::t id2 = variable::rename(orig_id);
      sb = add_sb_var(sb, orig_id, id2);
      pt.closure_id = pt.closure_id.add(orig_id, id2);
    });
  variable::Map<const F::FunctionDeclaration*> funs;
  func_decls->funs.iter([&](variable::t orig_id, const F::FunctionDeclaration* func_decl) {
    auto [params, sb2] = active_add_parameters_prime(sb, func_decl->params);
    sb = sb2;
    // Since all parameters are distinct, even between functions, we can
    // just use a single substitution.
    F::t body = flambda_utils::toplevel_substitution(sb.sb_var, func_decl->body);
    const F::FunctionDeclaration* function_decl = F::create_function_declaration(
        slice(params), body, func_decl->stub, func_decl->dbg, func_decl->inline_, func_decl->specialise,
        func_decl->is_a_functor, func_decl->closure_origin, func_decl->poll);
    variable::t id = only_freshen_parameters ? orig_id : active_find_var_exn(sb, orig_id);
    funs = funs.add(id, function_decl);
  });
  const F::FunctionDeclarations* function_decls = F::update_function_declarations(func_decls, funs);
  return {fv, function_decls, active(sb), pt};
}

projection::t freshen_projection(projection::t p, const T& freshening, const project_var::T& closure_freshening) {
  projection::T r = *p;
  switch (p->kind) {
    case projection::T::Kind::Project_var:
      // (the record's fields right to left: pure)
      r.project_var.closure = apply_variable(freshening, p->project_var.closure);
      r.project_var.closure_id = project_var::apply_closure_id(closure_freshening, p->project_var.closure_id);
      r.project_var.var = project_var::apply_var_within_closure(closure_freshening, p->project_var.var);
      break;
    case projection::T::Kind::Project_closure:
      r.project_closure.set_of_closures = apply_variable(freshening, p->project_closure.set_of_closures);
      r.project_closure.closure_id = project_var::apply_closure_id(closure_freshening, p->project_closure.closure_id);
      break;
    case projection::T::Kind::Move_within_set_of_closures:
      r.move.closure = apply_variable(freshening, p->move.closure);
      r.move.start_from = project_var::apply_closure_id(closure_freshening, p->move.start_from);
      r.move.move_to = project_var::apply_closure_id(closure_freshening, p->move.move_to);
      break;
    case projection::T::Kind::Field: r.field_var = apply_variable(freshening, p->field_var); break;
  }
  return make<projection::T>(r);
}

variable::Map<flambda::SpecialisedTo> freshen_projection_relation(const variable::Map<flambda::SpecialisedTo>& relation,
                                                                  const T& freshening,
                                                                  const project_var::T& closure_freshening) {
  return relation.map([&](const flambda::SpecialisedTo& s) {
    flambda::SpecialisedTo r = s;
    if (s.projection) r.projection = freshen_projection(s.projection, freshening, closure_freshening);
    return r;
  });
}

variable::Map<SpecApprox> freshen_projection_relation_prime(const variable::Map<SpecApprox>& relation,
                                                            const T& freshening,
                                                            const project_var::T& closure_freshening) {
  return relation.map([&](const SpecApprox& s) {
    SpecApprox r = s;
    if (s.spec.projection) r.spec.projection = freshen_projection(s.spec.projection, freshening, closure_freshening);
    return r;
  });
}

}  // namespace cppcaml::typing::freshening
