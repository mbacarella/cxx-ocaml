// Port of middle_end/flambda/build_export_info.ml and
// traverse_for_exported_symbols.ml (see build_export_info.hpp).
//
// Export IDs are created in ocamlopt's order (they are in the .cmx).
#include "cppcaml/typing/build_export_info.hpp"

#include <deque>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/inline_and_simplify_aux.hpp"
#include "cppcaml/typing/invariant_params.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::build_export_info {

using namespace flambda;
namespace A = simple_value_approx;
using export_info::Approx;
using export_info::Descr;
using export_info::ValueSetOfClosures;
using DK = Descr::Kind;
using AK = Approx::Kind;

namespace {

Approx value_unknown() { return {}; }
Approx value_id(export_id::t id) { return {AK::Value_id, id, nullptr}; }
Approx value_symbol(symbol::t s) { return {AK::Value_symbol, nullptr, s}; }

const Descr* mk(const Descr& d) { return make<Descr>(d); }

[[noreturn]] void fatal(const std::function<void(format::Formatter&)>& msg) {
  format::Formatter f;
  msg(f);
  misc::fatal_error(f.contents());
}

// Env.Global: [ex_table] and [closure_table] are shared (OCaml's refs)
struct Tables {
  export_id::Map<const Descr*> ex_table;
  variable::Map<export_id::t> closure_table;  // Closure_id.Map
};
struct Global {
  symbol::Map<export_id::t> sym;
  Tables* tables;
};

export_id::t fresh_id() { return export_id::create(compilenv::current_compilation_unit()); }

Global add_symbol(const Global& t, symbol::t sym, export_id::t export_id) {
  if (t.sym.mem(sym))
    fatal([&](format::Formatter& f) {
      format::fprintf(f, "Build_export_info.Env.Global.add_symbol: cannot rebind symbol %a in environment",
                      pr(symbol::print, sym));
    });
  return {t.sym.add(sym, export_id), t.tables};
}
std::pair<export_id::t, Global> new_symbol(const Global& t, symbol::t sym) {
  export_id::t export_id = fresh_id();
  return {export_id, add_symbol(t, sym, export_id)};
}

struct Env {
  variable::Map<Approx> var;
  symbol::Map<export_id::t> sym;
  symbol::Set symbols_being_defined;
  Tables* tables;

  static Env empty_of_global(const symbol::Set& symbols_being_defined, const Global& env) {
    return {{}, env.sym, symbols_being_defined, env.tables};
  }

  static const Descr* extern_id_descr(export_id::t export_id) {
    return export_info::find_description(compilenv::approx_env(), export_id);
  }
  static const Descr* extern_symbol_descr(symbol::t sym) {
    if (compilenv::is_predefined_exception(sym)) return nullptr;
    const export_info::T* exp = compilenv::approx_for_global(symbol::compilation_unit(sym));
    if (!exp) return nullptr;
    const export_id::t* id = exp->symbol_id.find_opt(sym);
    if (!id) return nullptr;
    return export_info::find_description(exp, *id);
  }
  const Descr* get_id_descr(export_id::t export_id) const {
    if (const Descr* const* d = tables->ex_table.find_opt(export_id)) return *d;
    return extern_id_descr(export_id);
  }
  const Descr* get_symbol_descr(symbol::t sym) const {
    if (const export_id::t* id = this->sym.find_opt(sym))
      if (const Descr* const* d = tables->ex_table.find_opt(*id)) return *d;
    return extern_symbol_descr(sym);
  }
  const Descr* get_descr(const Approx& approx) const {
    switch (approx.kind) {
      case AK::Value_unknown: return nullptr;
      case AK::Value_id: return get_id_descr(approx.id);
      case AK::Value_symbol: return get_symbol_descr(approx.sym);
    }
    return nullptr;
  }
  void record_descr(export_id::t id, const Descr* descr) const {
    if (tables->ex_table.mem(id))
      fatal([&](format::Formatter& f) {
        format::fprintf(f, "Build_export_info.Env.record_descr: cannot rebind export ID %a in environment",
                        pr(unit_id::print, id));
      });
    tables->ex_table = tables->ex_table.add(id, descr);
  }
  export_id::t new_descr(const Descr* descr) const {
    export_id::t id = fresh_id();
    record_descr(id, descr);
    return id;
  }
  export_id::t new_value_closure_descr(variable::t closure_id, const ValueSetOfClosures* set_of_closures) const {
    if (const export_id::t* id = tables->closure_table.find_opt(closure_id)) return *id;
    Descr d{DK::Value_closure};
    d.closure_id = closure_id;
    d.set = set_of_closures;
    export_id::t export_id = new_descr(mk(d));
    tables->closure_table = tables->closure_table.add(closure_id, export_id);
    return export_id;
  }
  // (the literal [Value_int 0]: one block, which every unit description
  // shares)
  export_id::t new_unit_descr() const {
    static const Descr* unit_descr = [] {
      Descr d{DK::Value_int};
      d.n = 0;
      return permanent_zone().make<Descr>(d);
    }();
    return new_descr(unit_descr);
  }
  Env add_approx(variable::t v, const Approx& approx) const {
    if (var.mem(v))
      fatal([&](format::Formatter& f) {
        format::fprintf(f, "Build_export_info.Env.add_approx: cannot rebind variable %a in environment",
                        pr(variable::print, v));
      });
    Env e = *this;
    e.var = var.add(v, approx);
    return e;
  }
  Env add_approx_maps(const std::vector<variable::Map<Approx>>& maps) const {
    Env t = *this;
    for (const variable::Map<Approx>& m : maps) m.iter([&](variable::t v, const Approx& a) { t = t.add_approx(v, a); });
    return t;
  }
  Approx find_approx(variable::t v) const {
    const Approx* a = var.find_opt(v);
    return a ? *a : value_unknown();
  }
  bool is_symbol_being_defined(symbol::t sym) const { return symbols_being_defined.mem(sym); }
};

const Descr* descr_of_constant(const Const& c) {
  Descr d{c.kind == Const::Kind::Int ? DK::Value_int : DK::Value_char};
  d.n = c.n;
  return mk(d);
}

const Descr* descr_of_allocated_constant(allocated_const::t c) {
  using K = AllocatedConst::Kind;
  Descr d{DK::Value_unknown_descr};
  switch (c->kind) {
    case K::Float:
      d.kind = DK::Value_float;
      d.f = c->f;
      break;
    case K::Int32: case K::Int64: case K::Nativeint:
      d.kind = DK::Value_boxed_int;
      d.bi = c->kind == K::Int32 ? A::BoxedInt::Int32 : c->kind == K::Int64 ? A::BoxedInt::Int64 : A::BoxedInt::Nativeint;
      d.bival = c->i;
      break;
    case K::String:
      d.kind = DK::Value_string;
      d.str = {std::nullopt, static_cast<long>(c->s.size())};
      break;
    case K::Immutable_string:
      d.kind = DK::Value_string;
      d.str = {c->s, static_cast<long>(c->s.size())};
      break;
    case K::Immutable_float_array: {
      d.kind = DK::Value_float_array;
      std::vector<std::optional<double>> fs(c->floats.begin(), c->floats.end());
      d.float_array = {true, slice(fs), static_cast<long>(c->floats.size())};
      break;
    }
    case K::Float_array:
      d.kind = DK::Value_float_array;
      d.float_array = {false, {}, static_cast<long>(c->floats.size())};
      break;
  }
  return mk(d);
}

Approx descr_of_named(const Env& env, named n);
const ValueSetOfClosures* describe_set_of_closures(const Env& env, const SetOfClosures* set);

Approx approx_of_expr(Env env, t flam) {
  for (;;) {
    switch (flam->kind) {
      case EK::Var: return env.find_approx(as<Var>(flam)->var);
      case EK::Let: {
        auto* l = as<Let>(flam);
        Approx approx = descr_of_named(env, l->defining_expr);
        env = env.add_approx(l->var, approx);
        flam = l->body;
        continue;
      }
      case EK::Let_mutable: flam = as<Let_mutable>(flam)->body; continue;
      case EK::Apply: {
        auto* a = as<Apply>(flam);
        if (!a->call_kind.direct) return value_unknown();
        const Descr* d = env.get_descr(env.find_approx(a->func));
        if (d && d->kind == DK::Value_closure) {
          if (!variable::equal(d->closure_id, a->call_kind.direct) || !d->set->results.mem(d->closure_id))
            misc::fatal_error("Build_export_info.approx_of_expr");  // (assert)
          return *d->set->results.find_opt(d->closure_id);
        }
        return value_unknown();
      }
      case EK::Assign: case EK::For: case EK::While: return value_id(env.new_unit_descr());
      case EK::Static_raise: case EK::Static_catch: case EK::Try_with: case EK::If_then_else: case EK::Switch:
      case EK::String_switch: case EK::Send: case EK::Proved_unreachable:
        return value_unknown();
    }
    return value_unknown();
  }
}

Approx descr_of_named(const Env& env, named n) {
  using K = clambda::Primitive::K;
  switch (n->kind) {
    case NK::Expr: return approx_of_expr(env, as<NExpr>(n)->expr);
    case NK::Symbol: return value_symbol(as<NSymbol>(n)->sym);
    case NK::Read_mutable: return value_unknown();
    case NK::Read_symbol_field: {
      auto* r = as<NRead_symbol_field>(n);
      const Descr* d = env.get_symbol_descr(r->sym);
      if (d && d->kind == DK::Value_block && static_cast<long>(d->fields.size()) > r->field)
        return d->fields[static_cast<std::size_t>(r->field)];
      return value_unknown();
    }
    case NK::Const: return value_id(env.new_descr(descr_of_constant(as<NConst>(n)->c)));
    case NK::Allocated_const: return value_id(env.new_descr(descr_of_allocated_constant(as<NAllocated_const>(n)->c)));
    case NK::Prim: {
      auto* p = as<NPrim>(n);
      if (p->prim->kind == K::Pmakeblock && p->prim->mut == MutableFlag::Immutable) {
        std::vector<Approx> approxs;  // List.map
        for (variable::t a : p->args) approxs.push_back(env.find_approx(a));
        Descr d{DK::Value_block};
        d.tag = tag::create_exn(p->prim->n);
        d.fields = slice(approxs);
        return value_id(env.new_descr(mk(d)));
      }
      if (p->prim->kind == K::Pfield && p->args.size() == 1) {
        const Descr* d = env.get_descr(env.find_approx(p->args[0]));
        if (d && d->kind == DK::Value_block && static_cast<long>(d->fields.size()) > p->prim->n)
          return d->fields[static_cast<std::size_t>(p->prim->n)];
        return value_unknown();
      }
      return value_unknown();
    }
    case NK::Set_of_closures: {
      Descr d{DK::Value_set_of_closures};
      d.set = describe_set_of_closures(env, as<NSet_of_closures>(n)->set);
      return value_id(env.new_descr(mk(d)));
    }
    case NK::Project_closure: {
      const projection::ProjectClosure& pc = as<NProject_closure>(n)->p;
      const Descr* d = env.get_descr(env.find_approx(pc.set_of_closures));
      if (d && d->kind == DK::Value_set_of_closures) {
        if (!d->set->results.mem(pc.closure_id))
          fatal([&](format::Formatter& f) {
            format::fprintf(f,
                            "Could not build export description for [Project_closure]: closure ID %a not in set of "
                            "closures",
                            pr(variable::print, pc.closure_id));
          });
        return value_id(env.new_value_closure_descr(pc.closure_id, d->set));
      }
      // It would be nice if this were [assert false], but owing to the
      // fact that this pass may propagate less information than for
      // example [Inline_and_simplify], we might end up here.
      return value_unknown();
    }
    case NK::Move_within_set_of_closures: {
      const projection::MoveWithinSetOfClosures& m = as<NMove_within_set_of_closures>(n)->m;
      const Descr* d = env.get_descr(env.find_approx(m.closure));
      if (d && d->kind == DK::Value_closure) {
        if (!variable::equal(d->closure_id, m.start_from)) misc::fatal_error("Build_export_info.descr_of_named");
        return value_id(env.new_value_closure_descr(m.move_to, d->set));
      }
      return value_unknown();
    }
    case NK::Project_var: {
      const projection::ProjectVar& pv = as<NProject_var>(n)->p;
      const Descr* d = env.get_descr(env.find_approx(pv.closure));
      if (d && d->kind == DK::Value_closure) {
        if (!variable::equal(d->closure_id, pv.closure_id)) misc::fatal_error("Build_export_info.descr_of_named");
        const Approx* a = d->set->bound_vars.find_opt(pv.var);
        if (!a)
          fatal([&](format::Formatter& f) {
            format::fprintf(f, "Project_var from %a (closure ID %a) of variable %a that is not bound by the closure.",
                            pr(variable::print, pv.closure), pr(variable::print, d->closure_id),
                            pr(variable::print, pv.var));
          });
        return *a;
      }
      return value_unknown();
    }
  }
  return value_unknown();
}

const ValueSetOfClosures* describe_set_of_closures(const Env& env, const SetOfClosures* set) {
  variable::Map<Approx> bound_vars_approx = set->free_vars.map([&](const SpecialisedTo& s) { return env.find_approx(s.var); });
  variable::Map<Approx> specialised_args_approx =
      set->specialised_args.map([&](const SpecialisedTo& s) { return env.find_approx(s.var); });
  // To build an approximation of the results, we need an approximation of
  // the functions.  The first one we can build is one where every function
  // returns something unknown.
  const ValueSetOfClosures* initial_value_set_of_closures = make<ValueSetOfClosures>(ValueSetOfClosures{
      set->function_decls->set_of_closures_id, bound_vars_approx, set->free_vars,
      set->function_decls->funs.map([](const FunctionDeclaration*) { return value_unknown(); }), nullptr});
  variable::Map<Approx> closures_approx =
      set->function_decls->funs.mapi([&](variable::t fun_var, const FunctionDeclaration*) {
        return value_id(env.new_value_closure_descr(fun_var, initial_value_set_of_closures));
      });
  Env closure_env = env.add_approx_maps({closures_approx, bound_vars_approx, specialised_args_approx});
  variable::Map<Approx> results = set->function_decls->funs.mapi(
      [&](variable::t, const FunctionDeclaration* function_decl) { return approx_of_expr(closure_env, function_decl->body); });
  return make<ValueSetOfClosures>(ValueSetOfClosures{set->function_decls->set_of_closures_id, bound_vars_approx,
                                                     set->free_vars, results, nullptr});
}

Approx approx_of_constant_defining_value_block_field(const Env& env, const BlockField& c) {
  if (c.sym) return env.is_symbol_being_defined(c.sym) ? value_unknown() : value_symbol(c.sym);
  return value_id(env.new_descr(descr_of_constant(c.c)));
}

void describe_constant_defining_value(const Global& genv, export_id::t export_id, symbol::t symbol,
                                      const symbol::Set& symbols_being_defined, constant_defining_value c) {
  // Assignments of variables to export IDs are local to each constant
  // defining value.
  Env env = Env::empty_of_global(symbols_being_defined, genv);
  using K = ConstantDefiningValue::Kind;
  switch (c->kind) {
    case K::Allocated_const: env.record_descr(export_id, descr_of_allocated_constant(c->c)); break;
    case K::Block: {
      std::vector<Approx> approxs;  // List.map: in order
      for (const BlockField& f : c->fields) approxs.push_back(approx_of_constant_defining_value_block_field(env, f));
      Descr d{DK::Value_block};
      d.tag = c->tag;
      d.fields = slice(approxs);
      env.record_descr(export_id, mk(d));
      break;
    }
    case K::Set_of_closures: {
      ValueSetOfClosures s = *describe_set_of_closures(env, c->set);
      s.aliased_symbol = symbol;
      Descr d{DK::Value_set_of_closures};
      d.set = make<ValueSetOfClosures>(s);
      env.record_descr(export_id, mk(d));
      break;
    }
    case K::Project_closure: {
      const Descr* d = env.get_symbol_descr(c->sym);
      if (d && d->kind == DK::Value_set_of_closures) {
        if (!d->set->results.mem(c->closure_id))
          fatal([&](format::Formatter& f) {
            format::fprintf(f,
                            "Could not build export description for [Project_closure] constant defining value: "
                            "closure ID %a not in set of closures",
                            pr(variable::print, c->closure_id));
          });
        Descr dc{DK::Value_closure};
        dc.closure_id = c->closure_id;
        dc.set = d->set;
        env.record_descr(export_id, mk(dc));
        break;
      }
      fatal([&](format::Formatter& f) {
        const char* why = !d                              ? "No available export description@."
                          : d->kind == DK::Value_closure ? "The symbol is a closure instead of a set of closures.@."
                                                         : "The symbol is not a set of closures.@.";
        format::fprintf(f, "Cannot project symbol %a to closure_id %a.  %s", pr(symbol::print, c->sym),
                        pr(variable::print, c->closure_id), why);
      });
    }
  }
}

Global describe_program(Global env, const Program& program) {
  for (program_body p = program.program_body; p->kind != ProgramBody::Kind::End; p = p->body) {
    switch (p->kind) {
      case ProgramBody::Kind::Let_symbol: {
        auto [id, env2] = new_symbol(env, p->sym);
        env = env2;
        describe_constant_defining_value(env, id, p->sym, symbol::Set::singleton(p->sym), p->def);
        break;
      }
      case ProgramBody::Kind::Let_rec_symbol: {
        // (fold_left consing: the list reversed)
        struct D {
          export_id::t id;
          symbol::t sym;
          constant_defining_value def;
        };
        std::vector<D> defs;
        for (const SymbolBinding& b : p->defs) {
          auto [id, env2] = new_symbol(env, b.sym);
          env = env2;
          defs.insert(defs.begin(), D{id, b.sym, b.def});
        }
        // [Project_closure]s are separated to be handled last.  They are
        // the only values that need a description for their argument.
        std::vector<D> project_closures, other_constants;  // List.partition
        for (const D& d : defs)
          (d.def->kind == ConstantDefiningValue::Kind::Project_closure ? project_closures : other_constants).push_back(d);
        std::vector<symbol::t> syms;
        for (const D& d : defs) syms.push_back(d.sym);
        symbol::Set symbols_being_defined = symbol::Set::of_list(syms);
        for (const D& d : other_constants) describe_constant_defining_value(env, d.id, d.sym, symbols_being_defined, d.def);
        for (const D& d : project_closures)
          describe_constant_defining_value(env, d.id, d.sym, symbols_being_defined, d.def);
        break;
      }
      case ProgramBody::Kind::Initialize_symbol: {
        // Assignments of variables to export IDs are local to each
        // [Initialize_symbol] construction.
        Env lenv = Env::empty_of_global(symbol::Set::singleton(p->sym), env);
        std::vector<Approx> field_approxs;  // List.map: in order
        for (t f : p->fields) field_approxs.push_back(approx_of_expr(lenv, f));
        Descr d{DK::Value_block};
        d.tag = p->tag;
        d.fields = slice(field_approxs);
        export_id::t id = lenv.new_descr(mk(d));
        env = add_symbol(env, p->sym, id);
        break;
      }
      case ProgramBody::Kind::Effect: case ProgramBody::Kind::End: break;
    }
  }
  return env;
}

// ---- Traverse_for_exported_symbols ------------------------------------------
struct SymbolsToExport {
  symbol::Set symbols;
  export_id::Set export_ids;
  set_of_closures_id::Set set_of_closure_ids;
  set_of_closures_id::Set set_of_closure_ids_keep_declaration;
  variable::Set relevant_imported_closure_ids;
  variable::Set relevant_local_closure_ids;
  variable::Set relevant_imported_vars_within_closure;
  variable::Set relevant_local_vars_within_closure;
};

SymbolsToExport traverse(const set_of_closures_id::Map<const SetOfClosures*>& sets_of_closures_map,
                         const variable::Map<set_of_closures_id::t>& closure_id_to_set_of_closures_id,
                         const set_of_closures_id::Map<const A::FunctionDeclarations*>& function_declarations_map,
                         const export_id::Map<const Descr*>& values, const symbol::Map<export_id::t>& symbol_id,
                         symbol::t root_symbol) {
  SymbolsToExport r;
  r.symbols = symbol::Set::singleton(root_symbol);
  struct QueueElem {
    enum class Kind : unsigned char { Q_symbol, Q_set_of_closures_id, Q_export_id } kind;
    symbol::t sym = nullptr;
    unit_id::t id = nullptr;  // Q_set_of_closures_id, Q_export_id
  };
  std::deque<QueueElem> queue;
  auto conditionally_add_symbol = [&](symbol::t symbol) {
    if (!r.symbols.mem(symbol)) {
      r.symbols = r.symbols.add(symbol);
      queue.push_back({QueueElem::Kind::Q_symbol, symbol, nullptr});
    }
  };
  auto conditionally_add_set_of_closures_id = [&](set_of_closures_id::t id) {
    if (!r.set_of_closure_ids.mem(id)) {
      r.set_of_closure_ids = r.set_of_closure_ids.add(id);
      queue.push_back({QueueElem::Kind::Q_set_of_closures_id, nullptr, id});
    }
  };
  auto conditionally_add_export_id = [&](export_id::t id) {
    if (!r.export_ids.mem(id)) {
      r.export_ids = r.export_ids.add(id);
      queue.push_back({QueueElem::Kind::Q_export_id, nullptr, id});
    }
  };
  auto process_approx = [&](const Approx& approx) {
    if (approx.kind == AK::Value_id) conditionally_add_export_id(approx.id);
    else if (approx.kind == AK::Value_symbol) conditionally_add_symbol(approx.sym);
  };
  auto process_value_set_of_closures = [&](const ValueSetOfClosures* soc) {
    conditionally_add_set_of_closures_id(soc->set_of_closures_id);
    soc->bound_vars.iter([&](variable::t, const Approx& a) { process_approx(a); });
    soc->results.iter([&](variable::t, const Approx& a) { process_approx(a); });
    if (soc->aliased_symbol) conditionally_add_symbol(soc->aliased_symbol);
  };
  auto local_closure = [&](variable::t closure_id, bool keep_declaration) {
    const set_of_closures_id::t* id = closure_id_to_set_of_closures_id.find_opt(closure_id);
    if (!id) {
      r.relevant_imported_closure_ids = r.relevant_imported_closure_ids.add(closure_id);
      return false;
    }
    r.relevant_local_closure_ids = r.relevant_local_closure_ids.add(closure_id);
    if (keep_declaration) r.set_of_closure_ids_keep_declaration = r.set_of_closure_ids_keep_declaration.add(*id);
    else conditionally_add_set_of_closures_id(*id);
    return true;
  };
  auto process_function_body = [&](const A::FunctionBody* function_body) {
    auto f = [&](t term) {
      if (auto* a = as<Apply>(term); a && a->call_kind.direct) local_closure(a->call_kind.direct, false);
    };
    auto f_named = [&](named n) {
      switch (n->kind) {
        case NK::Symbol: conditionally_add_symbol(as<NSymbol>(n)->sym); break;
        case NK::Read_symbol_field: conditionally_add_symbol(as<NRead_symbol_field>(n)->sym); break;
        case NK::Set_of_closures:
          conditionally_add_set_of_closures_id(as<NSet_of_closures>(n)->set->function_decls->set_of_closures_id);
          break;
        case NK::Project_closure: local_closure(as<NProject_closure>(n)->p.closure_id, true); break;
        case NK::Move_within_set_of_closures: {
          const projection::MoveWithinSetOfClosures& m = as<NMove_within_set_of_closures>(n)->m;
          local_closure(m.start_from, true);
          local_closure(m.move_to, true);
          break;
        }
        case NK::Project_var: {
          const projection::ProjectVar& pv = as<NProject_var>(n)->p;
          if (local_closure(pv.closure_id, true))
            r.relevant_local_vars_within_closure = r.relevant_local_vars_within_closure.add(pv.var);
          else
            r.relevant_imported_vars_within_closure = r.relevant_imported_vars_within_closure.add(pv.var);
          break;
        }
        case NK::Prim: case NK::Expr: case NK::Const: case NK::Allocated_const: case NK::Read_mutable: break;
      }
    };
    flambda_iterators::iter(f, f_named, function_body->body);
  };
  queue.push_back({QueueElem::Kind::Q_symbol, root_symbol, nullptr});
  while (!queue.empty()) {
    QueueElem e = queue.front();
    queue.pop_front();
    switch (e.kind) {
      case QueueElem::Kind::Q_export_id: {
        const Descr* const* d = values.find_opt(e.id);
        if (!d) break;
        if ((*d)->kind == DK::Value_block)
          for (const Approx& a : (*d)->fields) process_approx(a);
        else if ((*d)->kind == DK::Value_closure || (*d)->kind == DK::Value_set_of_closures)
          process_value_set_of_closures((*d)->set);
        break;
      }
      case QueueElem::Kind::Q_symbol: {
        compilation_unit::t cu = symbol::compilation_unit(e.sym);
        if (compilation_unit::is_current(cu)) {
          const export_id::t* id = symbol_id.find_opt(e.sym);
          if (!id)
            fatal([&](format::Formatter& f) {
              format::fprintf(f, "cannot find symbol's export id %a\n", pr(symbol::print, e.sym));
            });
          conditionally_add_export_id(*id);
        }
        break;
      }
      case QueueElem::Kind::Q_set_of_closures_id: {
        const A::FunctionDeclarations* const* fds = function_declarations_map.find_opt(e.id);
        if (!fds) break;
        (*fds)->funs.iter([&](variable::t, const A::FunctionDeclaration* fun_decl) {
          if (fun_decl->function_body) process_function_body(fun_decl->function_body);
        });
        break;
      }
    }
  }
  closure_id_to_set_of_closures_id.iter([&](variable::t closure_id, set_of_closures_id::t id) {
    if (r.set_of_closure_ids.mem(id)) r.relevant_local_closure_ids = r.relevant_local_closure_ids.add(closure_id);
  });
  r.set_of_closure_ids.iter([&](set_of_closures_id::t id) {
    const SetOfClosures* const* set = sets_of_closures_map.find_opt(id);
    if (!set) return;
    (*set)->free_vars.iter([&](variable::t var, const SpecialisedTo&) {
      r.relevant_local_vars_within_closure = r.relevant_local_vars_within_closure.add(var);
    });
  });
  return r;
}

// the invariant parameters' / recursive functions' map: the local sets'
// (Map.map), then those of the imported sets the descriptions mention
template <class V, class Local, class Imported>
set_of_closures_id::Map<V> with_imported(const set_of_closures_id::Map<const SetOfClosures*>& sets, Local local,
                                         const export_id::Map<const Descr*>& unnested_values, Imported imported) {
  set_of_closures_id::Map<V> m = sets.map([&](const SetOfClosures* s) { return local(s->function_decls); });
  const export_info::T* exp = compilenv::approx_env();
  return unnested_values.fold(
      [&](export_id::t, const Descr* descr, set_of_closures_id::Map<V> acc) {
        if (descr->kind != DK::Value_closure && descr->kind != DK::Value_set_of_closures) return acc;
        set_of_closures_id::t id = descr->set->set_of_closures_id;
        const V* found = imported(exp).find_opt(id);
        return found ? acc.add(id, *found) : acc;
      },
      m);
}

}  // namespace

export_info::Transient build_transient(const Program& program) {
  if (clflags::opaque)
    return export_info::opaque_transient(compilenv::current_compilation_unit(), compilenv::current_unit_symbol());
  Tables tables;
  Global env = describe_program(Global{{}, &tables}, program);
  set_of_closures_id::Map<const SetOfClosures*> sets_of_closures_map = flambda_utils::all_sets_of_closures_map(program);
  set_of_closures_id::Map<const A::FunctionDeclarations*> function_declarations_map =
      sets_of_closures_map.map([](const SetOfClosures* s) {
        const FunctionDeclarations* function_decls = s->function_decls;
        auto recursive = A::Lazy<variable::Set>::of_fun(
            [function_decls] { return find_recursive_functions::in_function_declarations(function_decls); });
        auto keep_body = inline_and_simplify_aux::keep_body_check(function_decls->is_classic_mode, recursive);
        return A::function_declarations_approx(keep_body, function_decls);
      });
  const export_id::Map<const Descr*>& unnested_values = tables.ex_table;
  set_of_closures_id::Map<variable::Map<variable::Set>> invariant_params =
      with_imported<variable::Map<variable::Set>>(
          flambda_utils::all_sets_of_closures_map(program),
          [](const FunctionDeclarations* fd) {
            return fd->is_classic_mode ? variable::Map<variable::Set>{}
                                       : invariant_params::invariant_params_in_recursion(fd);
          },
          unnested_values, [](const export_info::T* exp) -> const auto& { return exp->invariant_params; });
  set_of_closures_id::Map<variable::Set> recursive = with_imported<variable::Set>(
      flambda_utils::all_sets_of_closures_map(program),
      [](const FunctionDeclarations* fd) {
        return fd->is_classic_mode ? variable::Set{} : find_recursive_functions::in_function_declarations(fd);
      },
      unnested_values, [](const export_info::T* exp) -> const auto& { return exp->recursive; });
  export_info::CUMap<export_id::Map<const Descr*>> values = export_info::nest_eid_map(unnested_values);
  symbol::Map<export_id::t> symbol_id = env.sym;
  variable::Map<set_of_closures_id::t> closure_id_to_set_of_closures_id = function_declarations_map.fold(
      [](set_of_closures_id::t id, const A::FunctionDeclarations* fds, variable::Map<set_of_closures_id::t> acc) {
        return fds->funs.fold(
            [&](variable::t fun_var, const A::FunctionDeclaration*, variable::Map<set_of_closures_id::t> a) {
              return a.add(fun_var, id);
            },
            acc);
      },
      variable::Map<set_of_closures_id::t>{});
  const export_id::Map<const Descr*>* current = values.find_opt(compilenv::current_compilation_unit());
  if (!current) misc::fatal_error("Build_export_info.build_transient");  // (Not_found)
  SymbolsToExport rel = traverse(sets_of_closures_map, closure_id_to_set_of_closures_id, function_declarations_map,
                                 *current, symbol_id, compilenv::current_unit_symbol());
  set_of_closures_id::Map<const A::FunctionDeclarations*> sets_of_closures = function_declarations_map.filter_map(
      [&](set_of_closures_id::t key, const A::FunctionDeclarations* fun_decls) -> std::optional<const A::FunctionDeclarations*> {
        if (rel.set_of_closure_ids.mem(key)) return fun_decls;
        if (rel.set_of_closure_ids_keep_declaration.mem(key))
          return fun_decls->is_classic_mode ? A::clear_function_bodies(fun_decls) : fun_decls;
        return std::nullopt;
      });
  values = values.map([&](const export_id::Map<const Descr*>& m) {
    return m.filter([&](export_id::t key, const Descr*) { return rel.export_ids.mem(key); });
  });
  symbol_id = symbol_id.filter([&](symbol::t key, export_id::t) { return rel.symbols.mem(key); });
  export_info::Transient tr;
  tr.sets_of_closures = sets_of_closures;
  tr.values = values;
  tr.symbol_id = symbol_id;
  tr.invariant_params = invariant_params;
  tr.recursive = recursive;
  tr.relevant_local_closure_ids = rel.relevant_local_closure_ids;
  tr.relevant_imported_closure_ids = rel.relevant_imported_closure_ids;
  tr.relevant_local_vars_within_closure = rel.relevant_local_vars_within_closure;
  tr.relevant_imported_vars_within_closure = rel.relevant_imported_vars_within_closure;
  return tr;
}

}  // namespace cppcaml::typing::build_export_info
