// Port of middle_end/flambda/flambda_utils.ml (see flambda_utils.hpp).
#include "cppcaml/typing/flambda_utils.hpp"

#include <deque>

#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::flambda_utils {

using namespace flambda;
namespace Names = internal_variable_names;

t name_expr(named n, Names::t name) {
  variable::t var = variable::create(name, compilation_unit::get_current_exn());
  return create_let(var, n, flambda::var(var));
}

t name_expr_from_var(named n, variable::t v) {
  variable::t var = variable::rename(v, compilation_unit::get_current_exn());
  return create_let(var, n, flambda::var(var));
}

const FunctionDeclaration* find_declaration(variable::t cf, const FunctionDeclarations* d) {
  const FunctionDeclaration* const* r = d->funs.find_opt(cf);
  return r ? *r : nullptr;
}

variable::t find_declaration_variable(variable::t cf, const FunctionDeclarations* d) {
  return d->funs.mem(cf) ? cf : nullptr;
}

variable::t find_free_variable(variable::t cv, const SetOfClosures* set) {
  const SpecialisedTo* s = set->free_vars.find_opt(cv);
  return s ? s->var : nullptr;
}

variable::Set variables_bound_by_the_closure(variable::t cf, const FunctionDeclarations* decls) {
  const FunctionDeclaration* func = find_declaration(cf, decls);
  if (!func) misc::fatal_error("Flambda_utils.variables_bound_by_the_closure");  // (Not_found)
  variable::Set params = parameter::set_vars(func->params);
  variable::Set functions = decls->funs.keys();
  return variable::Set::diff(variable::Set::diff(func->free_variables, params), functions);
}

std::string description_of_toplevel_node(t expr) {
  auto var_string = [](const char* prefix, variable::t v) {
    format::Formatter f;
    format::fprintf(f, "%s %a", prefix, pr(variable::print, v));
    f.print_flush();
    return f.contents();
  };
  switch (expr->kind) {
    case EK::Var: return var_string("var", static_cast<const Var*>(expr)->var);
    case EK::Apply: return "apply";
    case EK::Assign: return "assign";
    case EK::Send: return "send";
    case EK::Proved_unreachable: return "unreachable";
    case EK::Let: return var_string("let", static_cast<const Let*>(expr)->var);
    case EK::Let_mutable: return "let_mutable";
    case EK::If_then_else: return "if";
    case EK::Switch: return "switch";
    case EK::String_switch: return "stringswitch";
    case EK::Static_raise: return "staticraise";
    case EK::Static_catch: return "catch";
    case EK::Try_with: return "trywith";
    case EK::While: return "while";
    case EK::For: return "for";
  }
  return "";
}

namespace {
bool same_vars(Slice<variable::t> a, Slice<variable::t> b) {
  if (a.size() != b.size()) return false;
  for (std::size_t k = 0; k < a.size(); ++k)
    if (!variable::equal(a[k], b[k])) return false;
  return true;
}
bool same_opt(t a, t b) {
  if (!a || !b) return !a && !b;
  return same(a, b);
}
bool sameclosure(const FunctionDeclaration* c1, const FunctionDeclaration* c2) {
  if (c1->params.size() != c2->params.size()) return false;
  for (std::size_t k = 0; k < c1->params.size(); ++k)
    if (!variable::equal(c1->params[k].var, c2->params[k].var)) return false;
  return same(c1->body, c2->body);
}
bool same_set_of_closures(const SetOfClosures* c1, const SetOfClosures* c2) {
  return variable::Map<const FunctionDeclaration*>::equal(sameclosure, c1->function_decls->funs,
                                                          c2->function_decls->funs) &&
         variable::Map<SpecialisedTo>::equal(equal_specialised_to, c1->free_vars, c2->free_vars) &&
         variable::Map<SpecialisedTo>::equal(equal_specialised_to, c1->specialised_args, c2->specialised_args);
}
bool samecases(Slice<SwitchCase> a, Slice<SwitchCase> b) {
  if (a.size() != b.size()) return false;
  for (std::size_t k = 0; k < a.size(); ++k)
    if (a[k].key != b[k].key || !same(a[k].action, b[k].action)) return false;
  return true;
}
}  // namespace

bool same(t l1, t l2) {
  // it is ok for the string case: if they are physically the same, it is
  // the same original branch
  if (l1 == l2) return true;
  if (l1->kind != l2->kind) return false;
  switch (l1->kind) {
    case EK::Var: return variable::equal(static_cast<const Var*>(l1)->var, static_cast<const Var*>(l2)->var);
    case EK::Apply: {
      auto *a1 = static_cast<const Apply*>(l1), *a2 = static_cast<const Apply*>(l2);
      return equal_call_kind(a1->call_kind, a2->call_kind) && variable::equal(a1->func, a2->func) &&
             same_vars(a1->args, a2->args);
    }
    case EK::Let: {
      auto *a = static_cast<const Let*>(l1), *b = static_cast<const Let*>(l2);
      return variable::equal(a->var, b->var) && same_named(a->defining_expr, b->defining_expr) && same(a->body, b->body);
    }
    case EK::Let_mutable: {
      auto *a = static_cast<const Let_mutable*>(l1), *b = static_cast<const Let_mutable*>(l2);
      return variable::equal(a->var, b->var) && variable::equal(a->initial_value, b->initial_value) &&
             lambda::equal_value_kind(a->contents_kind, b->contents_kind) && same(a->body, b->body);
    }
    case EK::Switch: {
      auto *a = static_cast<const Switch*>(l1), *b = static_cast<const Switch*>(l2);
      return variable::equal(a->scrutinee, b->scrutinee) && IntSet::equal(a->numconsts, b->numconsts) &&
             IntSet::equal(a->numblocks, b->numblocks) && samecases(a->consts, b->consts) &&
             samecases(a->blocks, b->blocks) && same_opt(a->failaction, b->failaction);
    }
    case EK::String_switch: {
      auto *a = static_cast<const String_switch*>(l1), *b = static_cast<const String_switch*>(l2);
      if (!variable::equal(a->scrutinee, b->scrutinee) || a->cases.size() != b->cases.size()) return false;
      for (std::size_t k = 0; k < a->cases.size(); ++k)
        if (a->cases[k].s != b->cases[k].s || !same(a->cases[k].action, b->cases[k].action)) return false;
      return same_opt(a->def, b->def);
    }
    case EK::Static_raise: {
      auto *a = static_cast<const Static_raise*>(l1), *b = static_cast<const Static_raise*>(l2);
      return a->exn == b->exn && same_vars(a->args, b->args);
    }
    case EK::Static_catch: {
      auto *a = static_cast<const Static_catch*>(l1), *b = static_cast<const Static_catch*>(l2);
      if (a->exn != b->exn || a->vars.size() != b->vars.size()) return false;
      for (std::size_t k = 0; k < a->vars.size(); ++k)
        if (!variable::equal(a->vars[k].var, b->vars[k].var) ||
            !lambda::equal_value_kind(a->vars[k].kind, b->vars[k].kind))
          return false;
      return same(a->body, b->body) && same(a->handler, b->handler);
    }
    case EK::Try_with: {
      auto *a = static_cast<const Try_with*>(l1), *b = static_cast<const Try_with*>(l2);
      return same(a->body, b->body) && variable::equal(a->var, b->var) && same(a->handler, b->handler);
    }
    case EK::If_then_else: {
      auto *a = static_cast<const If_then_else*>(l1), *b = static_cast<const If_then_else*>(l2);
      return variable::equal(a->cond, b->cond) && same(a->ifso, b->ifso) && same(a->ifnot, b->ifnot);
    }
    case EK::While: {
      auto *a = static_cast<const While*>(l1), *b = static_cast<const While*>(l2);
      return same(a->cond, b->cond) && same(a->body, b->body);
    }
    case EK::For: {
      auto *a = static_cast<const For*>(l1), *b = static_cast<const For*>(l2);
      return variable::equal(a->bound_var, b->bound_var) && variable::equal(a->from_value, b->from_value) &&
             variable::equal(a->to_value, b->to_value) && a->direction == b->direction && same(a->body, b->body);
    }
    case EK::Assign: {
      auto *a = static_cast<const Assign*>(l1), *b = static_cast<const Assign*>(l2);
      return variable::equal(a->being_assigned, b->being_assigned) && variable::equal(a->new_value, b->new_value);
    }
    case EK::Send: {
      auto *a = static_cast<const Send*>(l1), *b = static_cast<const Send*>(l2);
      return a->meth_kind == b->meth_kind && variable::equal(a->meth, b->meth) && variable::equal(a->obj, b->obj) &&
             same_vars(a->args, b->args);
    }
    case EK::Proved_unreachable: return true;
  }
  return false;
}

bool same_named(named n1, named n2) {
  if (n1->kind != n2->kind) return false;
  switch (n1->kind) {
    case NK::Symbol: return symbol::equal(as<NSymbol>(n1)->sym, as<NSymbol>(n2)->sym);
    case NK::Const: return compare_const(as<NConst>(n1)->c, as<NConst>(n2)->c) == 0;
    case NK::Allocated_const:
      return allocated_const::compare(as<NAllocated_const>(n1)->c, as<NAllocated_const>(n2)->c) == 0;
    case NK::Read_mutable: return variable::equal(as<NRead_mutable>(n1)->var, as<NRead_mutable>(n2)->var);
    case NK::Read_symbol_field: {
      auto *a = as<NRead_symbol_field>(n1), *b = as<NRead_symbol_field>(n2);
      return symbol::equal(a->sym, b->sym) && a->field == b->field;
    }
    case NK::Set_of_closures:
      return same_set_of_closures(as<NSet_of_closures>(n1)->set, as<NSet_of_closures>(n2)->set);
    case NK::Project_closure: {
      auto &a = as<NProject_closure>(n1)->p, &b = as<NProject_closure>(n2)->p;
      return variable::equal(a.set_of_closures, b.set_of_closures) && variable::equal(a.closure_id, b.closure_id);
    }
    case NK::Project_var: {
      auto &a = as<NProject_var>(n1)->p, &b = as<NProject_var>(n2)->p;
      return variable::equal(a.closure, b.closure) && variable::equal(a.closure_id, b.closure_id) &&
             variable::equal(a.var, b.var);
    }
    case NK::Move_within_set_of_closures: {
      auto &a = as<NMove_within_set_of_closures>(n1)->m, &b = as<NMove_within_set_of_closures>(n2)->m;
      return variable::equal(a.closure, b.closure) && variable::equal(a.start_from, b.start_from) &&
             variable::equal(a.move_to, b.move_to);
    }
    case NK::Prim: {
      auto *a = as<NPrim>(n1), *b = as<NPrim>(n2);
      return clambda::equal_primitive(*a->prim, *b->prim) && same_vars(a->args, b->args);
    }
    case NK::Expr: return same(as<NExpr>(n1)->expr, as<NExpr>(n2)->expr);
  }
  return false;
}

namespace {
Slice<variable::t> map_vars(Slice<variable::t> l, FnRef<variable::t(variable::t)> sb) {
  std::vector<variable::t> out;  // List.map: in order
  out.reserve(l.size());
  for (variable::t v : l) out.push_back(sb(v));
  return slice(out);
}
variable::Map<SpecialisedTo> map_spec(const variable::Map<SpecialisedTo>& m, FnRef<variable::t(variable::t)> sb) {
  return m.map([&](const SpecialisedTo& s) { return SpecialisedTo{sb(s.var), s.projection}; });
}
// the named part of toplevel_substitution / substitute_read_symbol_field
named subst_named(named n, FnRef<variable::t(variable::t)> sb) {
  switch (n->kind) {
    case NK::Symbol: case NK::Const: case NK::Expr: case NK::Allocated_const: case NK::Read_mutable:
    case NK::Read_symbol_field: return n;
    case NK::Set_of_closures: {
      const SetOfClosures* s = as<NSet_of_closures>(n)->set;
      // (the arguments right to left: specialised_args first)
      variable::Map<SpecialisedTo> specialised_args = map_spec(s->specialised_args, sb);
      variable::Map<SpecialisedTo> free_vars = map_spec(s->free_vars, sb);
      return n_set_of_closures(
          create_set_of_closures(s->function_decls, free_vars, specialised_args, s->direct_call_surrogates));
    }
    case NK::Project_closure: {
      projection::ProjectClosure p = as<NProject_closure>(n)->p;
      p.set_of_closures = sb(p.set_of_closures);
      return n_project_closure(p);
    }
    case NK::Move_within_set_of_closures: {
      projection::MoveWithinSetOfClosures m = as<NMove_within_set_of_closures>(n)->m;
      m.closure = sb(m.closure);
      return n_move_within_set_of_closures(m);
    }
    case NK::Project_var: {
      projection::ProjectVar p = as<NProject_var>(n)->p;
      p.closure = sb(p.closure);
      return n_project_var(p);
    }
    case NK::Prim: {
      auto* p = as<NPrim>(n);
      return make<NPrim>(NPrim{{NK::Prim}, p->prim, map_vars(p->args, sb), p->dbg});
    }
  }
  return n;
}
}  // namespace

// CR-soon mshinwell: this should use the explicit ignore functions
t toplevel_substitution(const variable::Map<variable::t>& sb_map, t tree) {
  auto sb = [&](variable::t v) -> variable::t {
    const variable::t* r = sb_map.find_opt(v);
    return r ? *r : v;
  };
  auto aux = [&](t flam) -> t {
    switch (flam->kind) {
      case EK::Var: return var(sb(static_cast<const Var*>(flam)->var));
      case EK::Let_mutable: {
        auto* l = static_cast<const Let_mutable*>(flam);
        return let_mutable(l->var, sb(l->initial_value), l->contents_kind, l->body);
      }
      case EK::Assign: {
        auto* a = static_cast<const Assign*>(flam);
        return assign(a->being_assigned, sb(a->new_value));
      }
      case EK::Apply: {
        auto* a = static_cast<const Apply*>(flam);
        variable::t func = sb(a->func);
        Slice<variable::t> args = map_vars(a->args, sb);
        return apply(func, args, a->call_kind, a->dbg, a->inline_, a->specialise);
      }
      case EK::If_then_else: {
        auto* i = static_cast<const If_then_else*>(flam);
        return if_then_else(sb(i->cond), i->ifso, i->ifnot);
      }
      case EK::Switch: {
        auto* s = static_cast<const Switch*>(flam);
        return switch_(sb(s->scrutinee), s->numconsts, s->consts, s->numblocks, s->blocks, s->failaction);
      }
      case EK::String_switch: {
        auto* s = static_cast<const String_switch*>(flam);
        return string_switch(sb(s->scrutinee), s->cases, s->def);
      }
      case EK::Send: {
        auto* s = static_cast<const Send*>(flam);
        variable::t meth = sb(s->meth);
        variable::t obj = sb(s->obj);
        Slice<variable::t> args = map_vars(s->args, sb);
        return send(s->meth_kind, meth, obj, args, s->dbg);
      }
      case EK::For: {
        auto* f = static_cast<const For*>(flam);
        variable::t from_value = sb(f->from_value);
        variable::t to_value = sb(f->to_value);
        return for_(f->bound_var, from_value, to_value, f->direction, f->body);
      }
      case EK::Static_raise: {
        auto* r = static_cast<const Static_raise*>(flam);
        return static_raise(r->exn, map_vars(r->args, sb));
      }
      case EK::Static_catch: case EK::Try_with: case EK::While: case EK::Let: case EK::Proved_unreachable:
        return flam;
    }
    return flam;
  };
  if (sb_map.is_empty()) return tree;
  return flambda_iterators::map_toplevel(aux, [&](named n) { return subst_named(n, sb); }, tree);
}

// CR-someday mshinwell: Fix [Flambda_iterators] so this can be implemented
// properly.
named toplevel_substitution_named(const variable::Map<variable::t>& sb, named n) {
  t expr = name_expr(n, Names::toplevel_substitution_named);
  t r = toplevel_substitution(sb, expr);
  auto* l = as<Let>(r);
  if (!l) misc::fatal_error("Flambda_utils.toplevel_substitution_named");
  return l->defining_expr;
}

t make_closure_declaration(bool is_classic_mode, variable::t id, t body, Slice<Parameter> params) {
  variable::Set free_variables = flambda::free_variables(body);
  variable::Set param_set = parameter::set_vars(params);
  if (!variable::Set::subset(param_set, free_variables)) misc::fatal_error("Flambda_utils.make_closure_declaration");
  variable::Map<variable::t> sb = free_variables.fold(
      [](variable::t v, variable::Map<variable::t> acc) { return acc.add(v, variable::rename(v)); },
      variable::Map<variable::t>{});
  // CR-soon mshinwell: try to eliminate this [toplevel_substitution].
  body = toplevel_substitution(sb, body);
  auto subst = [&](variable::t v) { return *sb.find_opt(v); };
  std::vector<Parameter> new_params;  // List.map: in order
  for (const Parameter& p : params) new_params.push_back(parameter::wrap(subst(p.var)));
  const FunctionDeclaration* function_declaration = create_function_declaration(
      slice(new_params), body, true, debuginfo::none(), lambda::InlineAttribute{},
      lambda::SpecialiseAttribute::Default_specialise, false, id, lambda::PollAttribute::Default_poll);
  variable::Map<SpecialisedTo> free_vars =
      sb.filter([&](variable::t v, variable::t) { return !param_set.mem(v); })
          .fold([](variable::t v, variable::t v2,
                   variable::Map<SpecialisedTo> acc) { return acc.add(v2, SpecialisedTo{v, nullptr}); },
                variable::Map<SpecialisedTo>{});
  compilation_unit::t cu = compilation_unit::get_current_exn();
  variable::t set_of_closures_var = variable::create(Names::set_of_closures, cu);
  const FunctionDeclarations* function_decls = create_function_declarations(
      is_classic_mode, variable::Map<const FunctionDeclaration*>::singleton(id, function_declaration));
  const SetOfClosures* set = create_set_of_closures(function_decls, free_vars, {}, {});
  named project_closure = n_project_closure(projection::ProjectClosure{set_of_closures_var, id});
  variable::t project_closure_var = variable::create(Names::project_closure, cu);
  return create_let(set_of_closures_var, n_set_of_closures(set),
                    create_let(project_closure_var, project_closure, var(project_closure_var)));
}

t bind(const std::vector<std::pair<variable::t, named>>& bindings, t body) {
  for (const auto& [v, d] : bindings) body = create_let(v, d, body);
  return body;
}

std::vector<SymbolBinding> all_lifted_constants(const Program& program) {
  // loop: Let_symbol conses before the rest's list; Let_rec_symbol folds
  // its declarations onto the rest's list (reversed)
  std::vector<std::vector<SymbolBinding>> parts;
  for (program_body p = program.program_body; p->kind != ProgramBody::Kind::End; p = p->body) {
    if (p->kind == ProgramBody::Kind::Let_symbol) parts.push_back({{p->sym, p->def}});
    else if (p->kind == ProgramBody::Kind::Let_rec_symbol) {
      std::vector<SymbolBinding> d(p->defs.begin(), p->defs.end());
      parts.push_back(std::vector<SymbolBinding>(d.rbegin(), d.rend()));
    }
  }
  std::vector<SymbolBinding> out;
  for (auto& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

symbol::Map<constant_defining_value> all_lifted_constants_as_map(const Program& program) {
  symbol::Map<constant_defining_value> m;  // Make_map.of_list: List.fold_left add
  for (const SymbolBinding& b : all_lifted_constants(program)) m = m.add(b.sym, b.def);
  return m;
}

std::vector<InitializeSymbol> initialize_symbols(const Program& program) {
  std::vector<InitializeSymbol> out;
  for (program_body p = program.program_body; p->kind != ProgramBody::Kind::End; p = p->body)
    if (p->kind == ProgramBody::Kind::Initialize_symbol) out.push_back({p->sym, p->tag, p->fields});
  return out;
}

symbol::Set needed_import_symbols(const Program& program) {
  symbol::Set dependencies = free_symbols_program(program);
  std::vector<symbol::t> lifted, inits;
  for (const SymbolBinding& b : all_lifted_constants(program)) lifted.push_back(b.sym);
  for (const InitializeSymbol& i : initialize_symbols(program)) inits.push_back(i.sym);
  // (the arguments right to left: the initialize_symbols set first)
  symbol::Set s_inits = symbol::Set::of_list(inits);
  symbol::Set s_lifted = symbol::Set::of_list(lifted);
  symbol::Set defined_symbol = symbol::Set::union_(s_lifted, s_inits);
  return symbol::Set::diff(dependencies, defined_symbol);
}

Program introduce_needed_import_symbols(const Program& program) {
  return {needed_import_symbols(program), program.program_body};
}

symbol::t root_symbol(const Program& program) {
  program_body p = program.program_body;
  while (p->kind != ProgramBody::Kind::End) p = p->body;
  return p->sym;
}

bool might_raise_static_exn(named flam, static_exception::t stexn) {
  bool found = false;
  flambda_iterators::iter_on_named(
      [&](t e) {
        if (auto* r = as<Static_raise>(e); r && r->exn == stexn) found = true;
      },
      [](named) {}, flam);
  return found;
}

variable::Map<set_of_closures_id::t> make_closure_map(const Program& program) {
  variable::Map<set_of_closures_id::t> map;
  flambda_iterators::iter_on_set_of_closures_of_program(program, [&](bool, const SetOfClosures* s) {
    s->function_decls->funs.iter([&](variable::t var, const FunctionDeclaration*) {
      map = map.add(var, s->function_decls->set_of_closures_id);
    });
  });
  return map;
}

variable::Set all_lifted_constant_closures(const Program& program) {
  variable::Set acc;
  for (const SymbolBinding& b : all_lifted_constants(program))
    if (b.def->kind == ConstantDefiningValue::Kind::Set_of_closures)
      b.def->set->function_decls->funs.iter([&](variable::t key, const FunctionDeclaration*) { acc = acc.add(key); });
  return acc;
}

set_of_closures_id::Set all_lifted_constant_sets_of_closures(const Program& program) {
  set_of_closures_id::Set set;
  for (const SymbolBinding& b : all_lifted_constants(program))
    if (b.def->kind == ConstantDefiningValue::Kind::Set_of_closures)
      set = set.add(b.def->set->function_decls->set_of_closures_id);
  return set;
}

std::vector<const SetOfClosures*> all_sets_of_closures(const Program& program) {
  std::vector<const SetOfClosures*> list;  // (consed: the last first)
  flambda_iterators::iter_on_set_of_closures_of_program(program,
                                                        [&](bool, const SetOfClosures* s) { list.push_back(s); });
  return std::vector<const SetOfClosures*>(list.rbegin(), list.rend());
}

set_of_closures_id::Map<const SetOfClosures*> all_sets_of_closures_map(const Program& program) {
  set_of_closures_id::Map<const SetOfClosures*> r;
  flambda_iterators::iter_on_set_of_closures_of_program(
      program, [&](bool, const SetOfClosures* s) { r = r.add(s->function_decls->set_of_closures_id, s); });
  return r;
}

t substitute_read_symbol_field_for_variables(const variable::Map<const SymbolPath*>& substitution, t expr) {
  auto bind_ = [&](variable::t v, variable::t fresh_var, t e) -> t {
    const SymbolPath* sp = *substitution.find_opt(v);
    symbol::t sym = sp->sym;
    std::function<named(std::size_t)> make_named = [&](std::size_t from) -> named {
      std::size_t n = sp->path.size() - from;
      if (n == 0) return n_symbol(sym);
      if (n == 1) return n_read_symbol_field(sym, sp->path[from]);
      long h = sp->path[from];
      variable::t block = variable::create(Names::symbol_field_block);
      variable::t field = variable::create(Names::get_symbol_field);
      clambda::Primitive p = clambda::prim(clambda::Primitive::K::Pfield);
      p.n = h;
      p.ptr = lambda::ImmediateOrPointer::Pointer;
      p.mut = MutableFlag::Mutable;
      // create_let block (make_named t) (create_let field (Prim ...) (Var
      // field)): the inner let first, then the rest of the path
      t inner = create_let(field, n_prim(p, slice(std::vector<variable::t>{block}), debuginfo::none()), var(field));
      named rest = make_named(from + 1);
      return n_expr(create_let(block, rest, inner));
    };
    return create_let(fresh_var, make_named(0), e);
  };
  auto make_var_subst = [&](variable::t v) -> std::pair<variable::t, bool> {
    if (substitution.mem(v)) return {variable::rename(v), true};
    return {v, false};
  };
  auto f = [&](t e) -> t {
    switch (e->kind) {
      case EK::Var: {
        variable::t v = static_cast<const Var*>(e)->var;
        if (!substitution.mem(v)) return e;
        variable::t fresh = variable::rename(v);
        return bind_(v, fresh, var(fresh));
      }
      case EK::Let: {
        auto* l = static_cast<const Let*>(e);
        variable::Set to_substitute =
            free_variables_named(l->defining_expr).filter([&](variable::t v) { return substitution.mem(v); });
        if (to_substitute.is_empty()) return e;
        // Variable.Map.of_set: in increasing order
        variable::Map<variable::t> bindings = to_substitute.fold(
            [](variable::t v, variable::Map<variable::t> acc) { return acc.add(v, variable::rename(v)); },
            variable::Map<variable::t>{});
        named nd = subst_named(l->defining_expr, [&](variable::t v) {
          const variable::t* r = bindings.find_opt(v);
          return r ? *r : v;
        });
        t r = with_free_variables::create_let_reusing_body(l->var, nd, with_free_variables::of_body_of_let(l));
        return bindings.fold([&](variable::t ts, variable::t fresh, t acc) { return bind_(ts, fresh, acc); }, r);
      }
      case EK::Let_mutable: {
        auto* l = static_cast<const Let_mutable*>(e);
        if (!substitution.mem(l->initial_value)) return e;
        variable::t fresh = variable::rename(l->initial_value);
        return bind_(l->initial_value, fresh, let_mutable(l->var, fresh, l->contents_kind, l->body));
      }
      case EK::If_then_else: {
        auto* i = static_cast<const If_then_else*>(e);
        if (!substitution.mem(i->cond)) return e;
        variable::t fresh = variable::rename(i->cond);
        return bind_(i->cond, fresh, if_then_else(fresh, i->ifso, i->ifnot));
      }
      case EK::Switch: {
        auto* s = static_cast<const Switch*>(e);
        if (!substitution.mem(s->scrutinee)) return e;
        variable::t fresh = variable::rename(s->scrutinee);
        return bind_(s->scrutinee, fresh, switch_(fresh, s->numconsts, s->consts, s->numblocks, s->blocks, s->failaction));
      }
      case EK::String_switch: {
        auto* s = static_cast<const String_switch*>(e);
        if (!substitution.mem(s->scrutinee)) return e;
        variable::t fresh = variable::rename(s->scrutinee);
        return bind_(s->scrutinee, fresh, string_switch(fresh, s->cases, s->def));
      }
      case EK::Assign: {
        auto* a = static_cast<const Assign*>(e);
        if (!substitution.mem(a->new_value)) return e;
        variable::t fresh = variable::rename(a->new_value);
        return bind_(a->new_value, fresh, assign(a->being_assigned, fresh));
      }
      case EK::Static_raise: {
        auto* r = static_cast<const Static_raise*>(e);
        std::vector<variable::t> args;
        std::vector<std::pair<variable::t, bool>> subs;  // List.map: in order
        for (variable::t v : r->args) subs.push_back(make_var_subst(v));
        for (std::size_t k = 0; k < subs.size(); ++k) args.push_back(subs[k].first);
        t body = static_raise(r->exn, slice(args));
        // List.fold_right (fun f expr -> f expr) bind_args: the last first
        for (std::size_t k = subs.size(); k-- > 0;)
          if (subs[k].second) body = bind_(r->args[k], subs[k].first, body);
        return body;
      }
      case EK::For: {
        auto* fo = static_cast<const For*>(e);
        auto from = make_var_subst(fo->from_value);
        auto to = make_var_subst(fo->to_value);
        t body = for_(fo->bound_var, from.first, to.first, fo->direction, fo->body);
        if (to.second) body = bind_(fo->to_value, to.first, body);
        if (from.second) body = bind_(fo->from_value, from.first, body);
        return body;
      }
      case EK::Apply: {
        auto* a = static_cast<const Apply*>(e);
        auto func = make_var_subst(a->func);
        std::vector<std::pair<variable::t, bool>> subs;
        for (variable::t v : a->args) subs.push_back(make_var_subst(v));
        std::vector<variable::t> args;
        for (auto& s : subs) args.push_back(s.first);
        t body = apply(func.first, slice(args), a->call_kind, a->dbg, a->inline_, a->specialise);
        for (std::size_t k = subs.size(); k-- > 0;)
          if (subs[k].second) body = bind_(a->args[k], subs[k].first, body);
        if (func.second) body = bind_(a->func, func.first, body);
        return body;
      }
      case EK::Send: {
        auto* s = static_cast<const Send*>(e);
        auto meth = make_var_subst(s->meth);
        auto obj = make_var_subst(s->obj);
        std::vector<std::pair<variable::t, bool>> subs;
        for (variable::t v : s->args) subs.push_back(make_var_subst(v));
        std::vector<variable::t> args;
        for (auto& x : subs) args.push_back(x.first);
        t body = send(s->meth_kind, meth.first, obj.first, slice(args), s->dbg);
        for (std::size_t k = subs.size(); k-- > 0;)
          if (subs[k].second) body = bind_(s->args[k], subs[k].first, body);
        if (obj.second) body = bind_(s->obj, obj.first, body);
        if (meth.second) body = bind_(s->meth, meth.first, body);
        return body;
      }
      case EK::Proved_unreachable: case EK::While: case EK::Try_with: case EK::Static_catch:
        // No variables directly used in those expressions
        return e;
    }
    return e;
  };
  return flambda_iterators::map_toplevel(f, [](named n) { return n; }, expr);
}

variable::Map<variable::Set> fun_vars_referenced_in_decls(const FunctionDeclarations* function_decls,
                                                          FnRef<symbol::t(variable::t)> closure_symbol) {
  variable::Set fun_vars = function_decls->funs.keys();
  symbol::Map<variable::t> symbols_to_fun_vars = fun_vars.fold(
      [&](variable::t fun_var, symbol::Map<variable::t> acc) { return acc.add(closure_symbol(fun_var), fun_var); },
      symbol::Map<variable::t>{});
  return function_decls->funs.map([&](const FunctionDeclaration* d) {
    variable::Set from_symbols = d->free_symbols.fold(
        [&](symbol::t s, variable::Set acc) {
          const variable::t* fv = symbols_to_fun_vars.find_opt(s);
          return fv ? acc.add(*fv) : acc;
        },
        variable::Set{});
    variable::Set from_variables = variable::Set::inter(d->free_variables, fun_vars);
    return variable::Set::union_(from_symbols, from_variables);
  });
}

variable::Set closures_required_by_entry_point(variable::t entry_point, FnRef<symbol::t(variable::t)> closure_symbol,
                                               const FunctionDeclarations* function_decls) {
  variable::Map<variable::Set> dependencies = fun_vars_referenced_in_decls(function_decls, closure_symbol);
  variable::Set set;
  std::deque<variable::t> queue;
  auto add = [&](variable::t v) {
    if (!set.mem(v)) {
      set = set.add(v);
      queue.push_back(v);
    }
  };
  add(entry_point);
  while (!queue.empty()) {
    variable::t fun_var = queue.front();
    queue.pop_front();
    if (const variable::Set* deps = dependencies.find_opt(fun_var))
      deps->iter([&](variable::t dep) {
        if (function_decls->funs.mem(dep)) add(dep);
      });
  }
  return set;
}

variable::Set all_functions_parameters(const FunctionDeclarations* function_decls) {
  return function_decls->funs.fold(
      [](variable::t, const FunctionDeclaration* d, variable::Set acc) {
        return variable::Set::union_(acc, parameter::set_vars(d->params));
      },
      variable::Set{});
}

symbol::Set all_free_symbols(const FunctionDeclarations* function_decls) {
  return function_decls->funs.fold(
      [](variable::t, const FunctionDeclaration* d, symbol::Set acc) { return symbol::Set::union_(acc, d->free_symbols); },
      symbol::Set{});
}

bool contains_stub(const FunctionDeclarations* fun_decls) {
  return !fun_decls->funs.filter([](variable::t, const FunctionDeclaration* d) { return d->stub; }).is_empty();
}

variable::Map<SpecialisedTo> clean_projections(const variable::Map<SpecialisedTo>& which_variables) {
  return which_variables.map([&](const SpecialisedTo& s) -> SpecialisedTo {
    if (!s.projection) return s;
    variable::t from = projection::projecting_from(s.projection);
    if (which_variables.mem(from)) return s;
    return SpecialisedTo{s.var, nullptr};
  });
}

named projection_to_named(projection::t p) {
  switch (p->kind) {
    case projection::T::Kind::Project_var: return n_project_var(p->project_var);
    case projection::T::Kind::Project_closure: return n_project_closure(p->project_closure);
    case projection::T::Kind::Move_within_set_of_closures: return n_move_within_set_of_closures(p->move);
    case projection::T::Kind::Field: {
      clambda::Primitive pf = clambda::prim(clambda::Primitive::K::Pfield);
      pf.n = p->field_index;
      pf.ptr = lambda::ImmediateOrPointer::Pointer;
      pf.mut = MutableFlag::Mutable;
      return n_prim(pf, slice(std::vector<variable::t>{p->field_var}), debuginfo::none());
    }
  }
  return nullptr;
}

variable::Map<std::vector<SpecialisedToSameAs>> parameters_specialised_to_the_same_variable(
    const FunctionDeclarations* function_decls, const variable::Map<SpecialisedTo>& specialised_args) {
  // For each external variable involved in a specialisation, which
  // internal variable(s) it maps to via that specialisation.
  // (Variable.Map.transpose_keys_and_data_set)
  variable::Map<variable::Set> specialised_arg_aliasing = specialised_args.fold(
      [](variable::t k, const SpecialisedTo& s, variable::Map<variable::Set> m) {
        const variable::Set* set = m.find_opt(s.var);
        return m.add(s.var, set ? set->add(k) : variable::Set::singleton(k));
      },
      variable::Map<variable::Set>{});
  return function_decls->funs.map([&](const FunctionDeclaration* d) {
    std::vector<SpecialisedToSameAs> out;
    for (const Parameter& p : d->params) {
      const SpecialisedTo* s = specialised_args.find_opt(p.var);
      if (!s) out.push_back({});
      else out.push_back({true, *specialised_arg_aliasing.find_opt(s->var)});
    }
    return out;
  });
}

// ---- Switch_storer ------------------------------------------------------------
namespace {
bool comparable_named(named n);
bool comparable_expr(t e) {
  for (;;) {
    if (as<Var>(e) || as<Static_raise>(e)) return true;
    auto* l = as<Let>(e);
    if (!l || !comparable_named(l->defining_expr)) return false;
    e = l->body;
  }
}
bool comparable_named(named n) {
  if (as<NSymbol>(n) || as<NConst>(n) || as<NPrim>(n)) return true;
  if (auto* x = as<NExpr>(n)) return comparable_expr(x->expr);
  return false;
}
// compare_key's equality: the environment maps the variables bound in [e2]
// to the corresponding ones of [e1]
bool same_var(const variable::Map<variable::t>& env, variable::t v1, variable::t v2) {
  const variable::t* bound = env.find_opt(v2);
  return variable::compare(v1, bound ? *bound : v2) == 0;
}
bool same_vars(const variable::Map<variable::t>& env, Slice<variable::t> a1, Slice<variable::t> a2) {
  if (a1.size() != a2.size()) return false;
  for (std::size_t k = 0; k < a1.size(); ++k)
    if (!same_var(env, a1[k], a2[k])) return false;
  return true;
}
bool same_expr(variable::Map<variable::t> env, t e1, t e2);
bool same_named(const variable::Map<variable::t>& env, named n1, named n2) {
  if (n1->kind != n2->kind) return false;
  switch (n1->kind) {
    case NK::Symbol: return symbol::compare(as<NSymbol>(n1)->sym, as<NSymbol>(n2)->sym) == 0;
    case NK::Const: return compare_const(as<NConst>(n1)->c, as<NConst>(n2)->c) == 0;
    case NK::Expr: return same_expr(env, as<NExpr>(n1)->expr, as<NExpr>(n2)->expr);
    case NK::Prim: {
      auto* p1 = as<NPrim>(n1);
      auto* p2 = as<NPrim>(n2);
      return clambda::equal_primitive(*p1->prim, *p2->prim) && same_vars(env, p1->args, p2->args);
    }
    default: return false;
  }
}
bool same_expr(variable::Map<variable::t> env, t e1, t e2) {
  for (;;) {
    if (e1->kind != e2->kind) return false;
    switch (e1->kind) {
      case EK::Var: return same_var(env, as<Var>(e1)->var, as<Var>(e2)->var);
      case EK::Static_raise: {
        auto* r1 = as<Static_raise>(e1);
        auto* r2 = as<Static_raise>(e2);
        return r1->exn == r2->exn && same_vars(env, r1->args, r2->args);
      }
      case EK::Let: {
        auto* l1 = as<Let>(e1);
        auto* l2 = as<Let>(e2);
        if (!same_named(env, l1->defining_expr, l2->defining_expr)) return false;
        env = env.add(l2->var, l1->var);
        e1 = l1->body;
        e2 = l2->body;
        continue;
      }
      default: return false;
    }
  }
}
}  // namespace

std::optional<SwitchStorerPolicy::key> SwitchStorerPolicy::make_key(const t& expr) {
  if (!comparable_expr(expr)) return std::nullopt;
  return expr;
}
bool SwitchStorerPolicy::same_key(const key& a, const key& b) { return same_expr({}, a, b); }

}  // namespace cppcaml::typing::flambda_utils
