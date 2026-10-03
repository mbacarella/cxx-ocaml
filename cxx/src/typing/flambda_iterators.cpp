// Port of middle_end/flambda/flambda_iterators.ml (see flambda_iterators.hpp).
#include "cppcaml/typing/flambda_iterators.hpp"

#include <vector>

namespace cppcaml::typing::flambda_iterators {

using namespace flambda;

void apply_on_subexpressions(FnRef<void(t)> f, FnRef<void(named)> f_named, t flam) {
  switch (flam->kind) {
    case EK::Var: case EK::Apply: case EK::Assign: case EK::Send: case EK::Proved_unreachable:
    case EK::Static_raise: return;
    case EK::Let: {
      auto* l = static_cast<const Let*>(flam);
      f_named(l->defining_expr);
      f(l->body);
      return;
    }
    case EK::Let_mutable: f(static_cast<const Let_mutable*>(flam)->body); return;
    case EK::Switch: {
      auto* s = static_cast<const Switch*>(flam);
      for (const SwitchCase& c : s->consts) f(c.action);
      for (const SwitchCase& c : s->blocks) f(c.action);
      if (s->failaction) f(s->failaction);
      return;
    }
    case EK::String_switch: {
      auto* s = static_cast<const String_switch*>(flam);
      for (const StringCase& c : s->cases) f(c.action);
      if (s->def) f(s->def);
      return;
    }
    case EK::Static_catch: {
      auto* c = static_cast<const Static_catch*>(flam);
      f(c->body);
      f(c->handler);
      return;
    }
    case EK::Try_with: {
      auto* tw = static_cast<const Try_with*>(flam);
      f(tw->body);
      f(tw->handler);
      return;
    }
    case EK::If_then_else: {
      auto* i = static_cast<const If_then_else*>(flam);
      f(i->ifso);
      f(i->ifnot);
      return;
    }
    case EK::While: {
      auto* w = static_cast<const While*>(flam);
      f(w->cond);
      f(w->body);
      return;
    }
    case EK::For: f(static_cast<const For*>(flam)->body); return;
  }
}

namespace {
// list_map_sharing (map_snd_sharing ...): the tail first (the last case
// first); the same list when no case changed
template <class C>
Slice<C> cases_map_sharing(Slice<C> l, FnRef<t(t)> f) {
  std::vector<C> out(l.begin(), l.end());
  bool changed = false;
  for (std::size_t k = l.size(); k-- > 0;) {
    t n = f(l[k].action);
    if (n != l[k].action) {
      out[k].action = n;
      changed = true;
    }
  }
  return changed ? slice(out) : l;
}
}  // namespace

t map_subexpressions(FnRef<t(t)> f, FnRef<named(variable::t, named)> f_named, t tree) {
  switch (tree->kind) {
    case EK::Var: case EK::Apply: case EK::Assign: case EK::Send: case EK::Proved_unreachable:
    case EK::Static_raise: return tree;
    case EK::Let: {
      auto* l = static_cast<const Let*>(tree);
      named new_named = f_named(l->var, l->defining_expr);
      t new_body = f(l->body);
      if (new_named == l->defining_expr && new_body == l->body) return tree;
      return create_let(l->var, new_named, new_body);
    }
    case EK::Let_mutable: {
      auto* l = static_cast<const Let_mutable*>(tree);
      t new_body = f(l->body);
      if (new_body == l->body) return tree;
      return let_mutable(l->var, l->initial_value, l->contents_kind, new_body);
    }
    case EK::Switch: {
      auto* s = static_cast<const Switch*>(tree);
      Slice<SwitchCase> new_consts = cases_map_sharing(s->consts, f);
      Slice<SwitchCase> new_blocks = cases_map_sharing(s->blocks, f);
      t new_failaction = s->failaction ? f(s->failaction) : nullptr;
      if (s->failaction == new_failaction && new_consts.p == s->consts.p && new_blocks.p == s->blocks.p) return tree;
      return switch_(s->scrutinee, s->numconsts, new_consts, s->numblocks, new_blocks, new_failaction);
    }
    case EK::String_switch: {
      auto* s = static_cast<const String_switch*>(tree);
      Slice<StringCase> new_sw = cases_map_sharing(s->cases, f);
      t new_def = s->def ? f(s->def) : nullptr;
      if (new_sw.p == s->cases.p && new_def == s->def) return tree;
      return string_switch(s->scrutinee, new_sw, new_def);
    }
    case EK::Static_catch: {
      auto* c = static_cast<const Static_catch*>(tree);
      t new_body = f(c->body);
      t new_handler = f(c->handler);
      if (new_body == c->body && new_handler == c->handler) return tree;
      return static_catch(c->exn, c->vars, new_body, new_handler);
    }
    case EK::Try_with: {
      auto* tw = static_cast<const Try_with*>(tree);
      t new_body = f(tw->body);
      t new_handler = f(tw->handler);
      if (new_body == tw->body && new_handler == tw->handler) return tree;
      return try_with(new_body, tw->var, new_handler);
    }
    case EK::If_then_else: {
      auto* i = static_cast<const If_then_else*>(tree);
      t new_ifso = f(i->ifso);
      t new_ifnot = f(i->ifnot);
      if (new_ifso == i->ifso && new_ifnot == i->ifnot) return tree;
      return if_then_else(i->cond, new_ifso, new_ifnot);
    }
    case EK::While: {
      auto* w = static_cast<const While*>(tree);
      t new_cond = f(w->cond);
      t new_body = f(w->body);
      if (new_cond == w->cond && new_body == w->body) return tree;
      return while_(new_cond, new_body);
    }
    case EK::For: {
      auto* fo = static_cast<const For*>(tree);
      t new_body = f(fo->body);
      if (new_body == fo->body) return tree;
      return for_(fo->bound_var, fo->from_value, fo->to_value, fo->direction, new_body);
    }
  }
  return tree;
}

void iter(FnRef<void(t)> f, FnRef<void(named)> f_named, t e) { iter_general(false, f, f_named, e); }
void iter_expr(FnRef<void(t)> f, t e) { iter(f, [](named) {}, e); }
void iter_on_named(FnRef<void(t)> f, FnRef<void(named)> f_named, named n) { iter_general_named(false, f, f_named, n); }
void iter_named(FnRef<void(named)> f_named, t e) { iter([](t) {}, f_named, e); }
void iter_named_on_named(FnRef<void(named)> f_named, named n) { iter_general_named(false, [](t) {}, f_named, n); }
void iter_toplevel(FnRef<void(t)> f, FnRef<void(named)> f_named, t e) { iter_general(true, f, f_named, e); }
void iter_named_toplevel(FnRef<void(t)> f, FnRef<void(named)> f_named, named n) {
  iter_general_named(true, f, f_named, n);
}
void iter_all_immutable_let_bindings(t e, FnRef<void(variable::t, named)> f) {
  iter_expr(
      [&](t x) {
        if (auto* l = as<Let>(x)) f(l->var, l->defining_expr);
      },
      e);
}
void iter_all_toplevel_immutable_let_bindings(t e, FnRef<void(variable::t, named)> f) {
  iter_general(
      true,
      [&](t x) {
        if (auto* l = as<Let>(x)) f(l->var, l->defining_expr);
      },
      [](named) {}, e);
}
void iter_on_sets_of_closures(FnRef<void(const SetOfClosures*)> f, t e) {
  iter_named(
      [&](named n) {
        if (auto* s = as<NSet_of_closures>(n)) f(s->set);
      },
      e);
}

void iter_exprs_at_toplevel_of_program(const Program& program, FnRef<void(t)> f) {
  auto funs = [&](const SetOfClosures* set) {
    set->function_decls->funs.iter([&](variable::t, const FunctionDeclaration* d) { f(d->body); });
  };
  for (program_body p = program.program_body;; p = p->body) {
    using K = ProgramBody::Kind;
    switch (p->kind) {
      case K::Let_symbol:
        if (p->def->kind == ConstantDefiningValue::Kind::Set_of_closures) funs(p->def->set);
        break;
      case K::Let_rec_symbol:
        for (const SymbolBinding& b : p->defs)
          if (b.def->kind == ConstantDefiningValue::Kind::Set_of_closures) funs(b.def->set);
        break;
      case K::Initialize_symbol:
        for (t field : p->fields) f(field);
        break;
      case K::Effect: f(p->expr); break;
      case K::End: return;
    }
  }
}

void iter_named_of_program(const Program& program, FnRef<void(named)> f) {
  iter_exprs_at_toplevel_of_program(program, [&](t e) { iter_named(f, e); });
}

void iter_on_set_of_closures_of_program(const Program& program, FnRef<void(bool, const SetOfClosures*)> f) {
  auto nonconst = [&](const SetOfClosures* s) { f(false, s); };
  auto constant_set = [&](const SetOfClosures* set) {
    f(true, set);
    set->function_decls->funs.iter(
        [&](variable::t, const FunctionDeclaration* d) { iter_on_sets_of_closures(nonconst, d->body); });
  };
  for (program_body p = program.program_body;; p = p->body) {
    using K = ProgramBody::Kind;
    switch (p->kind) {
      case K::Let_symbol:
        if (p->def->kind == ConstantDefiningValue::Kind::Set_of_closures) constant_set(p->def->set);
        break;
      case K::Let_rec_symbol:
        for (const SymbolBinding& b : p->defs)
          if (b.def->kind == ConstantDefiningValue::Kind::Set_of_closures) constant_set(b.def->set);
        break;
      case K::Initialize_symbol:
        for (t field : p->fields) iter_on_sets_of_closures(nonconst, field);
        break;
      case K::Effect: iter_on_sets_of_closures(nonconst, p->expr); break;
      case K::End: return;
    }
  }
}

void iter_constant_defining_values_on_program(const Program& program, FnRef<void(constant_defining_value)> f) {
  for (program_body p = program.program_body;; p = p->body) {
    using K = ProgramBody::Kind;
    switch (p->kind) {
      case K::Let_symbol: f(p->def); break;
      case K::Let_rec_symbol:
        for (const SymbolBinding& b : p->defs) f(b.def);
        break;
      case K::Initialize_symbol: case K::Effect: break;
      case K::End: return;
    }
  }
}

void iter_apply_on_program(const Program& program, FnRef<void(const Apply*)> f) {
  iter_exprs_at_toplevel_of_program(program, [&](t expr) {
    iter(
        [&](t e) {
          if (auto* a = as<Apply>(e)) f(a);
        },
        [](named) {}, expr);
  });
}

// ---- map_general ----------------------------------------------------------------
namespace {
struct MapGeneral {
  bool toplevel;
  FnRef<t(t)> f;
  FnRef<named(variable::t, named)> f_named;

  t aux(t tree) {
    if (tree->kind == EK::Let)
      return map_lets(
          tree, [this](variable::t id, named n) { return aux_named(id, n); }, [this](t e) { return aux(e); }, f);
    t exp = tree;
    switch (tree->kind) {
      case EK::Var: case EK::Apply: case EK::Assign: case EK::Send: case EK::Proved_unreachable:
      case EK::Static_raise: case EK::Let: break;
      case EK::Let_mutable: {
        auto* l = static_cast<const Let_mutable*>(tree);
        t new_body = aux(l->body);
        if (new_body != l->body) exp = let_mutable(l->var, l->initial_value, l->contents_kind, new_body);
        break;
      }
      case EK::Switch: {
        auto* s = static_cast<const Switch*>(tree);
        bool done_something = false;
        // { sw with failaction; consts; blocks }: the fields right to left
        t failaction = s->failaction ? aux_done_something(s->failaction, done_something) : nullptr;
        std::vector<SwitchCase> blocks;  // List.map: left to right
        for (const SwitchCase& c : s->blocks) blocks.push_back({c.key, aux_done_something(c.action, done_something)});
        std::vector<SwitchCase> consts;
        for (const SwitchCase& c : s->consts) consts.push_back({c.key, aux_done_something(c.action, done_something)});
        if (done_something)
          exp = switch_(s->scrutinee, s->numconsts, slice(consts), s->numblocks, slice(blocks), failaction);
        break;
      }
      case EK::String_switch: {
        auto* s = static_cast<const String_switch*>(tree);
        bool done_something = false;
        std::vector<StringCase> sw;
        for (const StringCase& c : s->cases) sw.push_back({c.s, aux_done_something(c.action, done_something)});
        t def = s->def ? aux_done_something(s->def, done_something) : nullptr;
        if (done_something) exp = string_switch(s->scrutinee, slice(sw), def);
        break;
      }
      case EK::Static_catch: {
        auto* c = static_cast<const Static_catch*>(tree);
        t new_body = aux(c->body);
        t new_handler = aux(c->handler);
        if (new_body != c->body || new_handler != c->handler) exp = static_catch(c->exn, c->vars, new_body, new_handler);
        break;
      }
      case EK::Try_with: {
        auto* tw = static_cast<const Try_with*>(tree);
        t new_body = aux(tw->body);
        t new_handler = aux(tw->handler);
        if (new_body != tw->body || new_handler != tw->handler) exp = try_with(new_body, tw->var, new_handler);
        break;
      }
      case EK::If_then_else: {
        auto* i = static_cast<const If_then_else*>(tree);
        t new_ifso = aux(i->ifso);
        t new_ifnot = aux(i->ifnot);
        if (new_ifso != i->ifso || new_ifnot != i->ifnot) exp = if_then_else(i->cond, new_ifso, new_ifnot);
        break;
      }
      case EK::While: {
        auto* w = static_cast<const While*>(tree);
        t new_cond = aux(w->cond);
        t new_body = aux(w->body);
        if (new_cond != w->cond || new_body != w->body) exp = while_(new_cond, new_body);
        break;
      }
      case EK::For: {
        auto* fo = static_cast<const For*>(tree);
        t new_body = aux(fo->body);
        if (new_body != fo->body) exp = for_(fo->bound_var, fo->from_value, fo->to_value, fo->direction, new_body);
        break;
      }
    }
    return f(exp);
  }
  t aux_done_something(t expr, bool& done_something) {
    t new_expr = aux(expr);
    if (new_expr != expr) done_something = true;
    return new_expr;
  }
  named aux_named(variable::t id, named n) {
    if (auto* s = as<NSet_of_closures>(n)) {
      if (!toplevel) {
        const SetOfClosures* set = s->set;
        bool done_something = false;
        variable::Map<const FunctionDeclaration*> funs =
            set->function_decls->funs.map([&](const FunctionDeclaration* d) -> const FunctionDeclaration* {
              t new_body = aux(d->body);
              if (new_body == d->body) return d;
              done_something = true;
              return update_function_declaration(d, d->params, new_body);
            });
        if (done_something) {
          const FunctionDeclarations* function_decls = update_function_declarations(set->function_decls, funs);
          n = n_set_of_closures(create_set_of_closures(function_decls, set->free_vars, set->specialised_args,
                                                       set->direct_call_surrogates));
        }
      }
    } else if (auto* e = as<NExpr>(n)) {
      t new_expr = aux(e->expr);
      if (new_expr != e->expr) n = n_expr(new_expr);
    }
    return f_named(id, n);
  }
};
}  // namespace

t map_general(bool toplevel, FnRef<t(t)> f, FnRef<named(variable::t, named)> f_named, t tree) {
  MapGeneral m{toplevel, f, f_named};
  return m.aux(tree);
}
t map(FnRef<t(t)> f, FnRef<named(named)> f_named, t tree) {
  return map_general(false, f, [&](variable::t, named n) { return f_named(n); }, tree);
}
t map_expr(FnRef<t(t)> f, t tree) { return map(f, [](named n) { return n; }, tree); }
t map_named(FnRef<named(named)> f_named, t tree) { return map([](t e) { return e; }, f_named, tree); }
t map_named_with_id(FnRef<named(variable::t, named)> f_named, t tree) {
  return map_general(false, [](t e) { return e; }, f_named, tree);
}
t map_toplevel(FnRef<t(t)> f, FnRef<named(named)> f_named, t tree) {
  return map_general(true, f, [&](variable::t, named n) { return f_named(n); }, tree);
}
t map_toplevel_expr(FnRef<t(t)> f, t tree) { return map_toplevel(f, [](named n) { return n; }, tree); }
t map_toplevel_named(FnRef<named(named)> f_named, t tree) { return map_toplevel([](t e) { return e; }, f_named, tree); }

t map_symbols(t tree, FnRef<symbol::t(symbol::t)> f) {
  return map_named(
      [&](named n) -> named {
        if (auto* s = as<NSymbol>(n)) {
          symbol::t new_sym = f(s->sym);
          return new_sym == s->sym ? n : n_symbol(new_sym);
        }
        if (auto* r = as<NRead_symbol_field>(n)) {
          symbol::t new_sym = f(r->sym);
          return new_sym == r->sym ? n : n_read_symbol_field(new_sym, r->field);
        }
        return n;
      },
      tree);
}

const SetOfClosures* map_symbols_on_set_of_closures(const SetOfClosures* set, FnRef<symbol::t(symbol::t)> f) {
  bool done_something = false;
  variable::Map<const FunctionDeclaration*> funs =
      set->function_decls->funs.map([&](const FunctionDeclaration* d) -> const FunctionDeclaration* {
        t body = map_symbols(d->body, f);
        if (body != d->body) done_something = true;
        return update_function_declaration(d, d->params, body);
      });
  if (!done_something) return set;
  const FunctionDeclarations* function_decls = update_function_declarations(set->function_decls, funs);
  return create_set_of_closures(function_decls, set->free_vars, set->specialised_args, set->direct_call_surrogates);
}

t map_toplevel_sets_of_closures(t tree, FnRef<const SetOfClosures*(const SetOfClosures*)> f) {
  return map_toplevel_named(
      [&](named n) -> named {
        if (auto* s = as<NSet_of_closures>(n)) {
          const SetOfClosures* ns = f(s->set);
          return ns == s->set ? n : n_set_of_closures(ns);
        }
        return n;
      },
      tree);
}

t map_apply(t tree, FnRef<const Apply*(const Apply*)> f) {
  return map(
      [&](t e) -> t {
        if (auto* a = as<Apply>(e)) return f(a);
        return e;
      },
      [](named n) { return n; }, tree);
}

t map_sets_of_closures(t tree, FnRef<const SetOfClosures*(const SetOfClosures*)> f) {
  return map_named(
      [&](named n) -> named {
        if (auto* s = as<NSet_of_closures>(n)) {
          const SetOfClosures* ns = f(s->set);
          return ns == s->set ? n : n_set_of_closures(ns);
        }
        return n;
      },
      tree);
}

t map_project_var_to_expr_opt(t tree, FnRef<t(const projection::ProjectVar&)> f) {
  return map_named(
      [&](named n) -> named {
        if (auto* p = as<NProject_var>(n)) {
          t e = f(p->p);
          return e ? n_expr(e) : n;
        }
        return n;
      },
      tree);
}

t map_project_var_to_named_opt(t tree, FnRef<named(const projection::ProjectVar&)> f) {
  return map_named(
      [&](named n) -> named {
        if (auto* p = as<NProject_var>(n)) {
          named r = f(p->p);
          return r ? r : n;
        }
        return n;
      },
      tree);
}

const SetOfClosures* map_function_bodies(const SetOfClosures* set, FnRef<t(t)> f) {
  bool done_something = false;
  variable::Map<const FunctionDeclaration*> funs =
      set->function_decls->funs.map([&](const FunctionDeclaration* d) -> const FunctionDeclaration* {
        t new_body = f(d->body);
        if (new_body == d->body) return d;
        done_something = true;
        return update_function_declaration(d, d->params, new_body);
      });
  if (!done_something) return set;
  const FunctionDeclarations* function_decls = update_function_declarations(set->function_decls, funs);
  return create_set_of_closures(function_decls, set->free_vars, set->specialised_args, set->direct_call_surrogates);
}

namespace {
constant_defining_value cdv_set_of_closures(const SetOfClosures* set) {
  ConstantDefiningValue v{ConstantDefiningValue::Kind::Set_of_closures};
  v.set = set;
  return make<ConstantDefiningValue>(v);
}

// map_sets_of_closures_of_program's loop (recursive, as OCaml's: its
// Let_rec_symbol case runs the loop on the rest twice when it rebuilds)
struct MapSetsOfClosuresOfProgram {
  FnRef<const SetOfClosures*(const SetOfClosures*)> f;

  const SetOfClosures* map_constant_set_of_closures(const SetOfClosures* set) {
    bool done_something = false;
    variable::Map<const FunctionDeclaration*> funs =
        set->function_decls->funs.map([&](const FunctionDeclaration* d) -> const FunctionDeclaration* {
          t body = map_sets_of_closures(d->body, f);
          if (body == d->body) return d;
          done_something = true;
          return update_function_declaration(d, d->params, body);
        });
    const FunctionDeclarations* function_decls =
        done_something ? update_function_declarations(set->function_decls, funs) : set->function_decls;
    const SetOfClosures* new_set = f(set);
    if (new_set == set) return set;
    return create_set_of_closures(function_decls, set->free_vars, set->specialised_args, set->direct_call_surrogates);
  }

  program_body loop(program_body program) {
    using K = ProgramBody::Kind;
    switch (program->kind) {
      case K::Let_symbol: {
        if (program->def->kind == ConstantDefiningValue::Kind::Set_of_closures) {
          const SetOfClosures* ns = map_constant_set_of_closures(program->def->set);
          program_body np = loop(program->body);
          if (ns == program->def->set && np == program->body) return program;
          return let_symbol(program->sym, cdv_set_of_closures(ns), np);
        }
        program_body np = loop(program->body);
        if (np == program->body) return program;
        return let_symbol(program->sym, program->def, np);
      }
      case K::Let_rec_symbol: {
        bool done_something = false;
        std::vector<SymbolBinding> defs;
        for (const SymbolBinding& b : program->defs) {
          if (b.def->kind == ConstantDefiningValue::Kind::Set_of_closures) {
            const SetOfClosures* ns = map_constant_set_of_closures(b.def->set);
            if (ns != b.def->set) done_something = true;
            defs.push_back({b.sym, cdv_set_of_closures(ns)});
          } else {
            defs.push_back(b);
          }
        }
        program_body np = loop(program->body);
        if (np == program->body && !done_something) return program;
        return let_rec_symbol(slice(defs), loop(program->body));
      }
      case K::Initialize_symbol: {
        bool done_something = false;
        std::vector<t> fields;
        for (t field : program->fields) {
          t nf = map_sets_of_closures(field, f);
          if (nf != field) done_something = true;
          fields.push_back(nf);
        }
        program_body np = loop(program->body);
        if (np == program->body && !done_something) return program;
        return initialize_symbol(program->sym, program->tag, slice(fields), np);
      }
      case K::Effect: {
        t ne = map_sets_of_closures(program->expr, f);
        program_body np = loop(program->body);
        if (ne == program->expr && np == program->body) return program;
        return effect(ne, np);
      }
      case K::End: return program;
    }
    return program;
  }
};
}  // namespace

Program map_sets_of_closures_of_program(const Program& program,
                                        FnRef<const SetOfClosures*(const SetOfClosures*)> f) {
  MapSetsOfClosuresOfProgram m{f};
  return {program.imported_symbols, m.loop(program.program_body)};
}

Program map_exprs_at_toplevel_of_program(const Program& program, FnRef<t(t)> f) {
  auto map_constant_set_of_closures = [&](const SetOfClosures* set) -> const SetOfClosures* {
    bool done_something = false;
    variable::Map<const FunctionDeclaration*> funs =
        set->function_decls->funs.map([&](const FunctionDeclaration* d) -> const FunctionDeclaration* {
          t body = f(d->body);
          if (body == d->body) return d;
          done_something = true;
          return update_function_declaration(d, d->params, body);
        });
    if (!done_something) return set;
    const FunctionDeclarations* function_decls = update_function_declarations(set->function_decls, funs);
    return create_set_of_closures(function_decls, set->free_vars, set->specialised_args, set->direct_call_surrogates);
  };
  // The loop's work top-down (each node's before the rest's, as OCaml's
  // recursion does), then the rebuild bottom-up.
  using K = ProgramBody::Kind;
  struct Step {
    program_body node;
    bool changed;
    const SetOfClosures* set;         // Let_symbol of a set of closures
    std::vector<SymbolBinding> defs;  // Let_rec_symbol
    std::vector<t> fields;            // Initialize_symbol
    t expr;                           // Effect
  };
  std::vector<Step> steps;
  program_body p = program.program_body;
  for (; p->kind != K::End; p = p->body) {
    Step s{p, false, nullptr, {}, {}, nullptr};
    switch (p->kind) {
      case K::Let_symbol:
        if (p->def->kind == ConstantDefiningValue::Kind::Set_of_closures) {
          s.set = map_constant_set_of_closures(p->def->set);
          s.changed = s.set != p->def->set;
        }
        break;
      case K::Let_rec_symbol:
        for (const SymbolBinding& b : p->defs) {
          if (b.def->kind == ConstantDefiningValue::Kind::Set_of_closures) {
            const SetOfClosures* ns = map_constant_set_of_closures(b.def->set);
            if (ns != b.def->set) s.changed = true;
            s.defs.push_back({b.sym, cdv_set_of_closures(ns)});
          } else {
            s.defs.push_back(b);
          }
        }
        break;
      case K::Initialize_symbol:
        for (t field : p->fields) {
          t nf = f(field);
          if (nf != field) s.changed = true;
          s.fields.push_back(nf);
        }
        break;
      case K::Effect:
        s.expr = f(p->expr);
        s.changed = s.expr != p->expr;
        break;
      case K::End: break;
    }
    steps.push_back(std::move(s));
  }
  program_body rest = p;  // End: the same
  for (std::size_t k = steps.size(); k-- > 0;) {
    Step& s = steps[k];
    program_body node = s.node;
    if (!s.changed && rest == node->body) {
      rest = node;
      continue;
    }
    switch (node->kind) {
      case K::Let_symbol:
        rest = let_symbol(node->sym, s.set && s.set != node->def->set ? cdv_set_of_closures(s.set) : node->def, rest);
        break;
      case K::Let_rec_symbol: rest = let_rec_symbol(slice(s.defs), rest); break;
      case K::Initialize_symbol: rest = initialize_symbol(node->sym, node->tag, slice(s.fields), rest); break;
      case K::Effect: rest = effect(s.expr, rest); break;
      case K::End: break;
    }
  }
  return {program.imported_symbols, rest};
}

Program map_named_of_program(const Program& program, FnRef<named(variable::t, named)> f) {
  return map_exprs_at_toplevel_of_program(program, [&](t expr) { return map_named_with_id(f, expr); });
}

t map_all_immutable_let_and_let_rec_bindings(t expr, FnRef<named(variable::t, named)> f) {
  return map_named_with_id(f, expr);
}

}  // namespace cppcaml::typing::flambda_iterators
