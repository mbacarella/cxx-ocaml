// Port of middle_end/flambda/lift_constants.ml (see lift_constants.hpp).
//
// The Variable.Tbl / Symbol.Tbl tables are hashtbl.hpp's OCaml Hashtbl
// (Hashtbl.Make with the identifiers' hashes): their iteration orders
// decide the order in which sets of closures are rebuilt (fresh ids) and
// variables renamed (fresh stamps), so they must be ocamlopt's.
#include "cppcaml/typing/lift_constants.hpp"

#include <tuple>
#include <vector>

#include "cppcaml/typing/alias_analysis.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/import_approx.hpp"
#include "cppcaml/typing/inconstant_idents.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/strongly_connected_components.hpp"

namespace cppcaml::typing::lift_constants {

using namespace flambda;
namespace AA = alias_analysis;
namespace A = simple_value_approx;
namespace Names = internal_variable_names;
using format::Formatter;
using format::fprintf;

namespace {

using Aliases = variable::Map<AA::AllocationPoint>;
using VarToSymbol = variable::Tbl<symbol::t>;
using VarToDefinition = variable::Tbl<AA::constant_defining_value>;
using VarToBlockField = variable::Tbl<BlockField>;
struct InitSym {
  tag::t tag;
  Slice<t> fields;
  symbol::t previous;  // option
};
struct EffectDef {
  t expr;
  symbol::t previous;  // option
};
using InitializeSymbolTbl = symbol::Tbl<InitSym>;
using EffectTbl = symbol::Tbl<EffectDef>;

[[noreturn]] void fatal(const std::function<void(Formatter&)>& msg) {
  Formatter f;
  msg(f);
  misc::fatal_error(f.contents());
}

// CR-someday mshinwell: move to Flambda_utils
variable::t tail_variable(t e) {
  for (;;) {
    if (auto* v = as<Var>(e)) return v->var;
    if (auto* l = as<Let_mutable>(e)) e = l->body;
    else if (auto* l = as<Let>(e)) e = l->body;
    else return nullptr;
  }
}

symbol::t closure_symbol(variable::t closure_id) { return compilenv::closure_symbol(closure_id); }

AA::constant_defining_value mk(AA::ConstantDefiningValue d) { return make<AA::ConstantDefiningValue>(d); }
AA::ConstantDefiningValue cdv(AA::ConstantDefiningValue::Kind k) {
  AA::ConstantDefiningValue d;
  d.kind = k;
  return d;
}
constant_defining_value fl_cdv(ConstantDefiningValue v) { return make<ConstantDefiningValue>(v); }
constant_defining_value fl_set(const SetOfClosures* s) {
  ConstantDefiningValue v{ConstantDefiningValue::Kind::Set_of_closures};
  v.set = s;
  return fl_cdv(v);
}
constant_defining_value fl_project_closure(symbol::t s, variable::t closure_id) {
  ConstantDefiningValue v{ConstantDefiningValue::Kind::Project_closure};
  v.sym = s;
  v.closure_id = closure_id;
  return fl_cdv(v);
}
constant_defining_value fl_allocated(allocated_const::t c) {
  ConstantDefiningValue v{ConstantDefiningValue::Kind::Allocated_const};
  v.c = c;
  return fl_cdv(v);
}

// Traverse the given expression assigning symbols to [let]- and [let
// rec]-bound constant variables.  At the same time collect the definitions
// of such variables.
struct Assigned {
  VarToSymbol var_to_symbol_tbl{42};
  VarToDefinition var_to_definition_tbl{42};
  symbol::Tbl<constant_defining_value> let_symbol_to_definition_tbl{42};
  symbol::Tbl<std::vector<variable::t>> initialize_symbol_to_definition_tbl{42};
};

void assign_symbols_and_collect_constant_definitions(const Program& program,
                                                     const inconstant_idents::Result& inconstants, Assigned& r) {
  using K = AA::ConstantDefiningValue::Kind;
  auto assign_symbol = [&](variable::t var, named n) {
    if (inconstant_idents::variable(var, inconstants)) return;
    auto assign_fresh_symbol = [&]() {
      symbol::t sym = symbol::of_variable(variable::rename(var));
      r.var_to_symbol_tbl.add(var, sym);
    };
    auto assign_existing_symbol = [&](symbol::t s) { r.var_to_symbol_tbl.add(var, s); };
    auto record_definition = [&](AA::ConstantDefiningValue d) { r.var_to_definition_tbl.add(var, mk(d)); };
    switch (n->kind) {
      case NK::Symbol: {
        symbol::t s = as<NSymbol>(n)->sym;
        assign_existing_symbol(s);
        AA::ConstantDefiningValue d = cdv(K::Symbol);
        d.sym = s;
        record_definition(d);
        return;
      }
      case NK::Const: {
        AA::ConstantDefiningValue d = cdv(K::Const);
        d.c = as<NConst>(n)->c;
        record_definition(d);
        return;
      }
      case NK::Allocated_const: {
        assign_fresh_symbol();
        AA::ConstantDefiningValue d = cdv(K::Allocated_const);
        d.allocated.kind = AA::AllocatedConst::Kind::Normal;
        d.allocated.c = as<NAllocated_const>(n)->c;
        record_definition(d);
        return;
      }
      case NK::Read_mutable:
        // [Inconstant_idents] always marks these expressions as inconstant,
        // so we should never get here.
        misc::fatal_error("Lift_constants: Read_mutable");
      case NK::Read_symbol_field: {
        auto* rs = as<NRead_symbol_field>(n);
        AA::ConstantDefiningValue d = cdv(K::Symbol_field);
        d.sym = rs->sym;
        d.field = rs->field;
        record_definition(d);
        return;
      }
      case NK::Set_of_closures: {
        const SetOfClosures* set = as<NSet_of_closures>(n)->set;
        if (inconstant_idents::closure(set->function_decls->set_of_closures_id, inconstants))
          misc::fatal_error("Lift_constants: inconstant set of closures");
        assign_fresh_symbol();
        AA::ConstantDefiningValue d = cdv(K::Set_of_closures);
        d.set = set;
        record_definition(d);
        set->function_decls->funs.iter([&](variable::t fun_var, const FunctionDeclaration*) {
          symbol::t cs = closure_symbol(fun_var);
          r.var_to_symbol_tbl.add(fun_var, cs);
          AA::ConstantDefiningValue pc = cdv(K::Project_closure);
          pc.project_closure = projection::ProjectClosure{var, fun_var};
          r.var_to_definition_tbl.add(fun_var, mk(pc));
        });
        return;
      }
      case NK::Move_within_set_of_closures: {
        const auto& m = as<NMove_within_set_of_closures>(n)->m;
        assign_existing_symbol(closure_symbol(m.move_to));
        AA::ConstantDefiningValue d = cdv(K::Move_within_set_of_closures);
        d.move = m;
        record_definition(d);
        return;
      }
      case NK::Project_closure: {
        const auto& p = as<NProject_closure>(n)->p;
        assign_existing_symbol(closure_symbol(p.closure_id));
        AA::ConstantDefiningValue d = cdv(K::Project_closure);
        d.project_closure = p;
        record_definition(d);
        return;
      }
      case NK::Prim: {
        auto* p = as<NPrim>(n);
        using PK = clambda::Primitive::K;
        const clambda::Primitive& prim = *p->prim;
        if (prim.kind == PK::Pmakeblock) {
          assign_fresh_symbol();
          AA::ConstantDefiningValue d = cdv(K::Block);
          d.tag = tag::create_exn(prim.n);
          d.fields = p->args;
          record_definition(d);
          return;
        }
        if (prim.kind == PK::Pfield) {
          if (p->args.size() != 1)
            fatal([&](Formatter& f) { fprintf(f, "[Pfield] with the wrong number of arguments"); });
          AA::ConstantDefiningValue d = cdv(K::Field);
          d.var = p->args[0];
          d.field = prim.n;
          record_definition(d);
          return;
        }
        if (prim.kind == PK::Pmakearray && prim.array == lambda::ArrayKind::Pfloatarray) {
          assign_fresh_symbol();
          AA::ConstantDefiningValue d = cdv(K::Allocated_const);
          d.allocated = AA::AllocatedConst{AA::AllocatedConst::Kind::Array, nullptr, prim.array, prim.mut, p->args};
          record_definition(d);
          return;
        }
        if (prim.kind == PK::Pduparray && p->args.size() == 1) {
          assign_fresh_symbol();
          AA::ConstantDefiningValue d = cdv(K::Allocated_const);
          d.allocated = AA::AllocatedConst{AA::AllocatedConst::Kind::Duplicate_array, nullptr, prim.array, prim.mut,
                                           {}, p->args[0]};
          record_definition(d);
          return;
        }
        fatal([&](Formatter& f) {
          fprintf(f, "Primitive not expected to be constant: @.%a@.", pr(print_named, n));
        });
      }
      case NK::Project_var: {
        AA::ConstantDefiningValue d = cdv(K::Project_var);
        d.project_var = as<NProject_var>(n)->p;
        record_definition(d);
        return;
      }
      case NK::Expr: {
        variable::t v = tail_variable(as<NExpr>(n)->expr);
        if (!v) misc::fatal_error("Lift_constants: Expr without a tail variable");  // See [Inconstant_idents].
        AA::ConstantDefiningValue d = cdv(K::Variable);
        d.var = v;
        record_definition(d);
        return;
      }
    }
  };
  flambda_iterators::iter_exprs_at_toplevel_of_program(
      program, [&](t expr) { flambda_iterators::iter_all_immutable_let_bindings(expr, assign_symbol); });
  // collect_let_and_initialize_symbols: an Initialize_symbol's entry after
  // the rest's (the recursion first)
  std::vector<program_body> inits;
  for (program_body p = program.program_body; p->kind != ProgramBody::Kind::End; p = p->body) {
    if (p->kind == ProgramBody::Kind::Let_symbol) r.let_symbol_to_definition_tbl.add(p->sym, p->def);
    else if (p->kind == ProgramBody::Kind::Let_rec_symbol)
      for (const SymbolBinding& b : p->defs) r.let_symbol_to_definition_tbl.add(b.sym, b.def);
    else if (p->kind == ProgramBody::Kind::Initialize_symbol) inits.push_back(p);
  }
  for (std::size_t k = inits.size(); k-- > 0;) {
    std::vector<variable::t> fields;  // List.map tail_variable
    for (t field : inits[k]->fields) fields.push_back(tail_variable(field));
    r.initialize_symbol_to_definition_tbl.add(inits[k]->sym, fields);
  }
  auto record_set_of_closure_equalities = [&](const SetOfClosures* set) {
    set->free_vars.iter([&](variable::t arg, const SpecialisedTo& var) {
      if (!inconstant_idents::variable(arg, inconstants)) {
        AA::ConstantDefiningValue d = cdv(K::Variable);
        d.var = var.var;
        r.var_to_definition_tbl.add(arg, mk(d));
      }
    });
    set->specialised_args.iter([&](variable::t arg, const SpecialisedTo& spec_to) {
      if (!inconstant_idents::variable(arg, inconstants)) {
        AA::ConstantDefiningValue d = cdv(K::Variable);
        d.var = spec_to.var;
        r.var_to_definition_tbl.add(arg, mk(d));
      }
    });
  };
  flambda_iterators::iter_on_set_of_closures_of_program(program, [&](bool constant, const SetOfClosures* set) {
    record_set_of_closure_equalities(set);
    if (constant)
      set->function_decls->funs.iter([&](variable::t fun_var, const FunctionDeclaration*) {
        symbol::t cs = closure_symbol(fun_var);
        AA::ConstantDefiningValue d = cdv(K::Symbol);
        d.sym = cs;
        r.var_to_definition_tbl.add(fun_var, mk(d));
        r.var_to_symbol_tbl.add(fun_var, cs);
      });
  });
}

BlockField variable_field_definition(const VarToSymbol& var_to_symbol_tbl, const VarToDefinition& var_to_definition_tbl,
                                     variable::t var) {
  if (const symbol::t* s = var_to_symbol_tbl.find_opt(var)) {
    BlockField b;
    b.sym = *s;
    return b;
  }
  const AA::constant_defining_value* d = var_to_definition_tbl.find_opt(var);
  if (!d) fatal([&](Formatter& f) { fprintf(f, "No associated symbol for the constant %a", pr(variable::print, var)); });
  if ((*d)->kind == AA::ConstantDefiningValue::Kind::Const) {
    BlockField b;
    b.c = (*d)->c;
    return b;
  }
  fatal([&](Formatter& f) {
    fprintf(f, "Unexpected pattern for a constant: %a: %a", pr(variable::print, var),
            pr(AA::print_constant_defining_value, *d));
  });
}

BlockField resolve_variable(const Aliases& aliases, const VarToSymbol& var_to_symbol_tbl,
                            const VarToDefinition& var_to_definition_tbl, variable::t var) {
  const AA::AllocationPoint* p = aliases.find_opt(var);
  if (!p) return variable_field_definition(var_to_symbol_tbl, var_to_definition_tbl, var);
  if (p->sym) {
    BlockField b;
    b.sym = p->sym;
    return b;
  }
  return variable_field_definition(var_to_symbol_tbl, var_to_definition_tbl, p->var);
}

named block_field_named(const BlockField& b) { return b.sym ? n_symbol(b.sym) : n_const(b.c); }

const SetOfClosures* translate_set_of_closures(const inconstant_idents::Result& inconstants, const Aliases& aliases,
                                               const VarToSymbol& var_to_symbol_tbl,
                                               const VarToDefinition& var_to_definition_tbl, const SetOfClosures* set) {
  auto f = [&](variable::t var, named n) -> named {
    if (inconstant_idents::variable(var, inconstants)) return n;
    return block_field_named(resolve_variable(aliases, var_to_symbol_tbl, var_to_definition_tbl, var));
  };
  return flambda_iterators::map_function_bodies(
      set, [&](t body) { return flambda_iterators::map_all_immutable_let_and_let_rec_bindings(body, f); });
}

symbol::Map<constant_defining_value> translate_constant_set_of_closures(
    const inconstant_idents::Result& inconstants, const Aliases& aliases, const VarToSymbol& var_to_symbol_tbl,
    const VarToDefinition& var_to_definition_tbl, const symbol::Map<constant_defining_value>& constant_defining_values) {
  return constant_defining_values.map([&](constant_defining_value c) -> constant_defining_value {
    if (c->kind != ConstantDefiningValue::Kind::Set_of_closures) return c;
    return fl_set(translate_set_of_closures(inconstants, aliases, var_to_symbol_tbl, var_to_definition_tbl, c->set));
  });
}

symbol::t find_original_set_of_closure(const Aliases& aliases, const VarToSymbol& var_to_symbol_tbl,
                                       const VarToDefinition& var_to_definition_tbl,
                                       const symbol::Map<symbol::t>& project_closure_map, variable::t var) {
  using K = AA::ConstantDefiningValue::Kind;
  for (;;) {
    const AA::AllocationPoint* p = aliases.find_opt(var);
    if (!p) misc::fatal_error("Lift_constants.find_original_set_of_closure");  // (Not_found)
    if (p->sym) {
      const symbol::t* s = project_closure_map.find_opt(p->sym);
      if (!s)
        fatal([&](Formatter& f) {
          fprintf(f, "find_original_set_of_closure: cannot find symbol %a in the project-closure map",
                  pr(symbol::print, p->sym));
        });
      return *s;
    }
    var = p->var;
    const AA::constant_defining_value* d = var_to_definition_tbl.find_opt(var);
    if (!d) misc::fatal_error("Lift_constants.find_original_set_of_closure");  // (Not_found)
    switch ((*d)->kind) {
      case K::Project_closure: var = (*d)->project_closure.set_of_closures; continue;
      case K::Move_within_set_of_closures: var = (*d)->move.closure; continue;
      case K::Set_of_closures: {
        const symbol::t* s = var_to_symbol_tbl.find_opt(var);
        if (!s) misc::fatal_error("Lift_constants.find_original_set_of_closure");  // (assert false)
        return *s;
      }
      default: misc::fatal_error("Lift_constants.find_original_set_of_closure");
    }
  }
}

// translate_definition_and_resolve_alias (null: None)
constant_defining_value translate_definition_and_resolve_alias(
    const inconstant_idents::Result& inconstants, const Aliases& aliases, const VarToSymbol& var_to_symbol_tbl,
    const VarToDefinition& var_to_definition_tbl, const symbol::Map<constant_defining_value>& symbol_definition_map,
    const symbol::Map<symbol::t>& project_closure_map, AA::constant_defining_value definition) {
  using K = AA::ConstantDefiningValue::Kind;
  using AK = AA::AllocatedConst::Kind;
  auto resolve_float_array_involving_variables = [&](MutableFlag mutability, Slice<variable::t> vars) {
    // Resolve an [Allocated_const] of the form: [Array (Pfloatarray, _, _)]
    // (which references its contents via variables; it does not contain
    // manifest floats).
    auto find_float_var_definition = [&](variable::t var) -> double {
      const AA::constant_defining_value* d = var_to_definition_tbl.find_opt(var);
      if (!d) misc::fatal_error("Lift_constants: float array member");  // (Not_found)
      if ((*d)->kind == K::Allocated_const && (*d)->allocated.kind == AK::Normal &&
          (*d)->allocated.c->kind == AllocatedConst::Kind::Float)
        return (*d)->allocated.c->f;
      fatal([&](Formatter& f) {
        fprintf(f, "Bad definition for float array member %a: %a", pr(variable::print, var),
                pr(AA::print_constant_defining_value, *d));
      });
    };
    auto find_float_symbol_definition = [&](symbol::t sym) -> double {
      const constant_defining_value* d = symbol_definition_map.find_opt(sym);
      if (!d) misc::fatal_error("Lift_constants: float array member");  // (Not_found)
      if ((*d)->kind == ConstantDefiningValue::Kind::Allocated_const &&
          (*d)->c->kind == AllocatedConst::Kind::Float)
        return (*d)->c->f;
      fatal([&](Formatter& f) {
        fprintf(f, "Bad definition for float array member %a: %a", pr(symbol::print, sym),
                pr(print_constant_defining_value, *d));
      });
    };
    std::vector<double> floats;  // List.map: in order
    for (variable::t var : vars) {
      const AA::AllocationPoint* p = aliases.find_opt(var);
      if (!p) floats.push_back(find_float_var_definition(var));
      else if (p->var) floats.push_back(find_float_var_definition(p->var));
      else floats.push_back(find_float_symbol_definition(p->sym));
    }
    AllocatedConst c{mutability == MutableFlag::Immutable ? AllocatedConst::Kind::Immutable_float_array
                                                          : AllocatedConst::Kind::Float_array};
    c.floats = slice(floats);
    return fl_allocated(make<AllocatedConst>(c));
  };
  switch (definition->kind) {
    case K::Block: {
      std::vector<BlockField> fields;  // List.map: in order
      for (variable::t v : definition->fields)
        fields.push_back(resolve_variable(aliases, var_to_symbol_tbl, var_to_definition_tbl, v));
      ConstantDefiningValue b{ConstantDefiningValue::Kind::Block};
      b.tag = definition->tag;
      b.fields = slice(fields);
      return fl_cdv(b);
    }
    case K::Allocated_const: {
      const AA::AllocatedConst& ac = definition->allocated;
      if (ac.kind == AK::Normal) return fl_allocated(ac.c);
      if (ac.kind == AK::Duplicate_array) {
        if (ac.array_kind != lambda::ArrayKind::Pfloatarray)
          fatal([&](Formatter& f) {
            fprintf(f,
                    "Lift_constants.translate_definition_and_resolve_alias: Duplicate_array with non-Pfloatarray "
                    "kind: %a",
                    pr(AA::print_constant_defining_value, definition));
          });
        // CR-someday mshinwell: This next section could do with cleanup.
        // (lift_constants.ml)
        variable::t var = ac.var;
        MutableFlag mutability = ac.mut;
        AA::constant_defining_value dup_def = nullptr;
        const AA::AllocationPoint* p = aliases.find_opt(var);
        if (!p || p->var) {
          variable::t v = p ? p->var : var;
          const AA::constant_defining_value* d = var_to_definition_tbl.find_opt(v);
          if (!d) misc::fatal_error("Lift_constants: Duplicate_array");  // (Not_found)
          dup_def = *d;
        } else {
          symbol::t sym = p->sym;
          if (const constant_defining_value* sd = symbol_definition_map.find_opt(sym)) {
            if ((*sd)->kind == ConstantDefiningValue::Kind::Allocated_const &&
                (*sd)->c->kind == AllocatedConst::Kind::Immutable_float_array) {
              AA::ConstantDefiningValue d = cdv(K::Allocated_const);
              d.allocated.kind = AK::Normal;
              d.allocated.c = (*sd)->c;
              dup_def = mk(d);
            } else {
              fatal([&](Formatter& f) {
                fprintf(f,
                        "Lift_constants.translate_definition_and_resolve_alias: Duplicate Pfloatarray %a with symbol "
                        "%a mapping to wrong constant defining value %a",
                        pr(variable::print, var), pr(AA::print_constant_defining_value, definition),
                        pr(print_constant_defining_value, *sd));
              });
            }
          } else {
            A::t approx = import_approx::import_symbol(sym);
            const A::Descr& descr = approx->descr;
            if (descr.kind == A::DK::Value_unresolved)
              fatal([&](Formatter& f) {
                fprintf(f,
                        "Lift_constants.translate_definition_and_resolve_alias: Duplicate Pfloatarray %a with unknown "
                        "symbol: %a",
                        pr(variable::print, var), pr(AA::print_constant_defining_value, definition));
              });
            if (descr.kind != A::DK::Value_float_array)
              // CR-someday mshinwell: we might hit this if we ever
              // duplicate a mutable array across compilation units (e.g.
              // "snapshotting" an array).  We do not currently generate
              // such code.
              fatal([&](Formatter& f) {
                fprintf(f,
                        "Lift_constants.translate_definition_and_resolve_alias: Duplicate Pfloatarray %a with symbol "
                        "%a that does not have an export description of an immutable array",
                        pr(variable::print, var), pr(AA::print_constant_defining_value, definition));
              });
            std::optional<std::vector<double>> contents = A::float_array_as_constant(descr.float_array);
            if (!contents)
              fatal([&](Formatter& f) {
                fprintf(f,
                        "Lift_constants.translate_definition_and_resolve_alias: Duplicate Pfloatarray %a with not "
                        "completely known float array from symbol: %a",
                        pr(variable::print, var), pr(AA::print_constant_defining_value, definition));
              });
            AllocatedConst c{AllocatedConst::Kind::Immutable_float_array};
            c.floats = slice(*contents);
            AA::ConstantDefiningValue d = cdv(K::Allocated_const);
            d.allocated.kind = AK::Normal;
            d.allocated.c = make<AllocatedConst>(c);
            dup_def = mk(d);
          }
        }
        if (dup_def->kind == K::Allocated_const) {
          const AA::AllocatedConst& v = dup_def->allocated;
          if (v.kind == AK::Normal && v.c->kind == AllocatedConst::Kind::Float_array)
            // This example from pchambart illustrates why we do not allow
            // the duplication of mutable arrays (lift_constants.ml)
            misc::fatal_error("Pduparray is not allowed on mutable arrays");
          if (v.kind == AK::Normal && v.c->kind == AllocatedConst::Kind::Immutable_float_array) {
            AllocatedConst c{mutability == MutableFlag::Immutable ? AllocatedConst::Kind::Immutable_float_array
                                                                  : AllocatedConst::Kind::Float_array};
            c.floats = v.c->floats;
            return fl_allocated(make<AllocatedConst>(c));
          }
          if (v.kind == AK::Array && v.array_kind == lambda::ArrayKind::Pfloatarray)
            // Important: [mutability] is from the [Duplicate_array]
            // construction above.
            return resolve_float_array_involving_variables(mutability, v.vars);
        }
        fatal([&](Formatter& f) {
          fprintf(f,
                  "Lift_constants.translate_definition_and_resolve_alias: Duplicate Pfloatarray %a with wrong "
                  "argument: %a",
                  pr(variable::print, var), pr(AA::print_constant_defining_value, dup_def));
        });
      }
      // Array
      if (ac.array_kind != lambda::ArrayKind::Pfloatarray)
        fatal([&](Formatter& f) {
          fprintf(f, "Lift_constants.translate_definition_and_resolve_alias: Array with non-Pfloatarray kind: %a",
                  pr(AA::print_constant_defining_value, definition));
        });
      return resolve_float_array_involving_variables(ac.mut, ac.vars);
    }
    case K::Project_closure: {
      const projection::ProjectClosure& pc = definition->project_closure;
      const AA::AllocationPoint* p = aliases.find_opt(pc.set_of_closures);
      // If a closure projection is a constant, the set of closures must be
      // assigned to a symbol.
      if (!p) misc::fatal_error("Lift_constants: Project_closure");  // (assert false)
      if (p->sym) return fl_project_closure(p->sym, pc.closure_id);
      const symbol::t* s = var_to_symbol_tbl.find_opt(p->var);
      if (!s) misc::fatal_error("Lift_constants: Project_closure");  // (assert false)
      return fl_project_closure(*s, pc.closure_id);
    }
    case K::Move_within_set_of_closures: {
      symbol::t set_of_closure_symbol = find_original_set_of_closure(
          aliases, var_to_symbol_tbl, var_to_definition_tbl, project_closure_map, definition->move.closure);
      return fl_project_closure(set_of_closure_symbol, definition->move.move_to);
    }
    case K::Set_of_closures:
      return fl_set(
          translate_set_of_closures(inconstants, aliases, var_to_symbol_tbl, var_to_definition_tbl, definition->set));
    case K::Project_var: case K::Field: case K::Symbol_field: case K::Const: case K::Symbol: case K::Variable:
      return nullptr;
  }
  return nullptr;
}

symbol::Map<constant_defining_value> translate_definitions_and_resolve_alias(
    const inconstant_idents::Result& inconstants, const Aliases& aliases, const VarToSymbol& var_to_symbol_tbl,
    const VarToDefinition& var_to_definition_tbl, const symbol::Map<constant_defining_value>& symbol_definition_map,
    const symbol::Map<symbol::t>& project_closure_map) {
  return var_to_definition_tbl.fold(
      [&](variable::t var, AA::constant_defining_value def, symbol::Map<constant_defining_value> map) {
        constant_defining_value d =
            translate_definition_and_resolve_alias(inconstants, aliases, var_to_symbol_tbl, var_to_definition_tbl,
                                                   symbol_definition_map, project_closure_map, def);
        if (!d) return map;
        const symbol::t* sym = var_to_symbol_tbl.find_opt(var);
        if (!sym) misc::fatal_error("Lift_constants.translate_definitions_and_resolve_alias");  // (Not_found)
        return map.add(*sym, d);
      },
      symbol::Map<constant_defining_value>{});
}

// Resorting of graph including Initialize_symbol
symbol::Set constant_dependencies(constant_defining_value c) {
  switch (c->kind) {
    case ConstantDefiningValue::Kind::Allocated_const: return {};
    case ConstantDefiningValue::Kind::Block: {
      std::vector<symbol::t> symbol_fields;  // List.filter_map
      for (const BlockField& f : c->fields)
        if (f.sym) symbol_fields.push_back(f.sym);
      return symbol::Set::of_list(symbol_fields);
    }
    case ConstantDefiningValue::Kind::Set_of_closures: return free_symbols_named(n_set_of_closures(c->set));
    case ConstantDefiningValue::Kind::Project_closure: return symbol::Set::singleton(c->sym);
  }
  return {};
}

using SymbolSCC = strongly_connected_components::Component<symbol::t>;

std::vector<SymbolSCC> program_graph(const symbol::Set& imported_symbols,
                                     const symbol::Map<constant_defining_value>& symbol_to_constant,
                                     const InitializeSymbolTbl& initialize_symbol_tbl, const EffectTbl& effect_tbl) {
  using Graph = symbol::Map<symbol::Set>;
  Graph graph_with_only_constant_parts = symbol_to_constant.map([&](constant_defining_value c) {
    return symbol::Set::diff(constant_dependencies(c), imported_symbols);
  });
  Graph graph_with_initialisation = initialize_symbol_tbl.fold(
      [&](symbol::t sym, const InitSym& init, Graph g) {
        symbol::Set order_dep = init.previous ? symbol::Set::singleton(init.previous) : symbol::Set{};
        symbol::Set deps = order_dep;
        for (t field : init.fields) deps = symbol::Set::union_(free_symbols(field), deps);
        deps = symbol::Set::diff(deps, imported_symbols);
        return g.add(sym, deps);
      },
      graph_with_only_constant_parts);
  Graph graph = effect_tbl.fold(
      [&](symbol::t sym, const EffectDef& eff, Graph g) {
        symbol::Set order_dep = eff.previous ? symbol::Set::singleton(eff.previous) : symbol::Set{};
        symbol::Set deps = symbol::Set::union_(free_symbols(eff.expr), order_dep);
        deps = symbol::Set::diff(deps, imported_symbols);
        return g.add(sym, deps);
      },
      graph_with_initialisation);
  return strongly_connected_components::connected_components_sorted_from_roots_to_leaf(graph);
}

// rebuilding the program
program_body add_definition_of_symbol(const symbol::Map<constant_defining_value>& constant_definitions,
                                      const InitializeSymbolTbl& initialize_symbol_tbl, const EffectTbl& effect_tbl,
                                      program_body program, const SymbolSCC& component) {
  auto symbol_declaration = [&](symbol::t sym) {
    // A symbol declared through an Initialize_symbol construct cannot be
    // recursive, this is not allowed in the construction.
    if (initialize_symbol_tbl.mem(sym)) misc::fatal_error("Lift_constants.add_definition_of_symbol");
    const constant_defining_value* d = constant_definitions.find_opt(sym);
    if (!d) misc::fatal_error("Lift_constants.add_definition_of_symbol");  // (Not_found)
    return SymbolBinding{sym, *d};
  };
  if (component.has_loop) {
    std::vector<SymbolBinding> l;  // List.map: in order
    for (symbol::t s : component.ids) l.push_back(symbol_declaration(s));
    return let_rec_symbol(slice(l), program);
  }
  symbol::t sym = component.ids[0];
  if (const InitSym* init = initialize_symbol_tbl.find_opt(sym))
    return initialize_symbol(sym, init->tag, init->fields, program);
  if (const EffectDef* eff = effect_tbl.find_opt(sym)) return effect(eff->expr, program);
  const constant_defining_value* d = constant_definitions.find_opt(sym);
  if (!d) misc::fatal_error("Lift_constants.add_definition_of_symbol");  // (Not_found)
  return let_symbol(sym, *d, program);
}

const SetOfClosures* introduce_free_variables_in_set_of_closures(const VarToBlockField& var_to_block_field_tbl,
                                                                 const SetOfClosures* set) {
  const FunctionDeclarations* fds = set->function_decls;
  using Acc = std::pair<t, variable::Map<variable::t>>;
  auto add_definition_and_make_substitution = [&](variable::t var, Acc acc) -> Acc {
    variable::t searched_var = var;
    // specialised arguments bound to constant can be rewritten
    if (const SpecialisedTo* external_var = set->specialised_args.find_opt(var)) searched_var = external_var->var;
    const BlockField* def = var_to_block_field_tbl.find_opt(searched_var);
    // The variable is bound by the closure or the arguments or not
    // constant.  In either case it does not need to be bound
    if (!def) return acc;
    variable::t fresh = variable::rename(var);
    named n = block_field_named(*def);
    return {create_let(fresh, n, acc.first), acc.second.add(var, fresh)};
  };
  bool done_something = false;
  variable::Set fun_keys = fds->funs.keys();
  variable::Map<const FunctionDeclaration*> funs = fds->funs.map([&](const FunctionDeclaration* d) {
    // Closures from the same set must not be bound.
    variable::Set variables_to_bind = variable::Set::diff(d->free_variables, fun_keys);
    Acc acc = variables_to_bind.fold(add_definition_and_make_substitution, Acc{d->body, {}});
    if (acc.second.is_empty()) return d;
    done_something = true;
    t body = flambda_utils::toplevel_substitution(acc.second, acc.first);
    return update_body_of_function_declaration(d, body);
  });
  const FunctionDeclarations* function_decls = update_function_declarations(fds, funs);
  // Keep only those that are not rewritten to constants.
  variable::Map<SpecialisedTo> free_vars = set->free_vars.filter([&](variable::t v, const SpecialisedTo&) {
    bool keep = !var_to_block_field_tbl.mem(v);
    if (!keep) done_something = true;
    return keep;
  });
  free_vars = flambda_utils::clean_projections(free_vars);
  variable::Map<SpecialisedTo> specialised_args =
      set->specialised_args.filter([&](variable::t, const SpecialisedTo& spec_to) {
        bool keep = !var_to_block_field_tbl.mem(spec_to.var);
        if (!keep) done_something = true;
        return keep;
      });
  specialised_args = flambda_utils::clean_projections(specialised_args);
  if (!done_something) return set;
  return create_set_of_closures(function_decls, free_vars, specialised_args, set->direct_call_surrogates);
}

named rewrite_project_var(const VarToBlockField& var_to_block_field_tbl, const projection::ProjectVar& project_var,
                          named original) {
  const BlockField* b = var_to_block_field_tbl.find_opt(project_var.var);
  if (!b) return original;
  return block_field_named(*b);
}

symbol::Map<constant_defining_value> introduce_free_variables_in_sets_of_closures(
    const VarToBlockField& var_to_block_field_tbl, const symbol::Map<constant_defining_value>& translate_definition) {
  return translate_definition.map([&](constant_defining_value def) -> constant_defining_value {
    if (def->kind != ConstantDefiningValue::Kind::Set_of_closures) return def;
    return fl_set(introduce_free_variables_in_set_of_closures(var_to_block_field_tbl, def->set));
  });
}

void var_to_block_field(const Aliases& aliases, const VarToSymbol& var_to_symbol_tbl,
                        const VarToDefinition& var_to_definition_tbl, VarToBlockField& var_to_block_field_tbl) {
  var_to_definition_tbl.iter([&](variable::t var, AA::constant_defining_value) {
    BlockField def = resolve_variable(aliases, var_to_symbol_tbl, var_to_definition_tbl, var);
    var_to_block_field_tbl.add(var, def);
  });
}

void program_symbols(const Program& program, InitializeSymbolTbl& initialize_symbol_tbl,
                     symbol::Tbl<constant_defining_value>& symbol_definition_tbl, EffectTbl& effect_tbl) {
  auto add_project_closure_definitions = [&](symbol::t def_symbol, constant_defining_value c) {
    if (c->kind != ConstantDefiningValue::Kind::Set_of_closures) return;
    c->set->function_decls->funs.iter([&](variable::t fun_var, const FunctionDeclaration*) {
      symbol::t cs = closure_symbol(fun_var);
      symbol_definition_tbl.add(cs, fl_project_closure(def_symbol, fun_var));
    });
  };
  symbol::t previous_effect = nullptr;
  for (program_body p = program.program_body; p->kind != ProgramBody::Kind::End; p = p->body) {
    switch (p->kind) {
      case ProgramBody::Kind::Let_symbol:
        add_project_closure_definitions(p->sym, p->def);
        symbol_definition_tbl.add(p->sym, p->def);
        break;
      case ProgramBody::Kind::Let_rec_symbol:
        for (const SymbolBinding& b : p->defs) {
          add_project_closure_definitions(b.sym, b.def);
          symbol_definition_tbl.add(b.sym, b.def);
        }
        break;
      case ProgramBody::Kind::Initialize_symbol:
        // previous_effect is used to keep the order of initialize and
        // effect values.  Their effects order must be kept ordered.  It is
        // used as an extra dependency when sorting the symbols.
        initialize_symbol_tbl.add(p->sym, InitSym{p->tag, p->fields, previous_effect});
        previous_effect = p->sym;
        break;
      case ProgramBody::Kind::Effect: {
        // Used to ensure that effects are correctly ordered
        symbol::t fake_effect_symbol = symbol::of_variable(variable::create(Names::fake_effect_symbol));
        effect_tbl.add(fake_effect_symbol, EffectDef{p->expr, previous_effect});
        previous_effect = fake_effect_symbol;
        break;
      }
      case ProgramBody::Kind::End: break;
    }
  }
}

void replace_definitions_in_initialize_symbol_and_effects(const inconstant_idents::Result& inconstants,
                                                          const Aliases& aliases, const VarToSymbol& var_to_symbol_tbl,
                                                          const VarToDefinition& var_to_definition_tbl,
                                                          InitializeSymbolTbl& initialize_symbol_tbl,
                                                          EffectTbl& effect_tbl) {
  auto rewrite_expr = [&](t expr) {
    return flambda_iterators::map_all_immutable_let_and_let_rec_bindings(expr, [&](variable::t var, named n) -> named {
      if (inconstant_idents::variable(var, inconstants)) return n;
      BlockField resolved = resolve_variable(aliases, var_to_symbol_tbl, var_to_definition_tbl, var);
      if (resolved.sym) {
        if (auto* s = as<NSymbol>(n)) {
          if (s->sym != resolved.sym) misc::fatal_error("Lift_constants: symbol mismatch");  // (assert s1 == s2)
          return n;
        }
        return n_symbol(resolved.sym);
      }
      if (auto* c = as<NConst>(n)) {
        // (assert (c1 == c2): physical equality on immediate constants)
        (void)c;
        return n;
      }
      return n_const(resolved.c);
    });
  };
  // This is safe because we only [replace] the current key during iteration
  // (cf. https://github.com/ocaml/ocaml/pull/337)
  initialize_symbol_tbl.iter([&](symbol::t sym, const InitSym& init) {
    std::vector<t> fields;  // List.map: in order
    for (t f : init.fields) fields.push_back(rewrite_expr(f));
    initialize_symbol_tbl.replace(sym, InitSym{init.tag, slice(fields), init.previous});
  });
  effect_tbl.iter([&](symbol::t sym, const EffectDef& eff) {
    t e = rewrite_expr(eff.expr);
    effect_tbl.replace(sym, EffectDef{e, eff.previous});
  });
}

// CR-soon mshinwell: Update the name of [project_closure_map].
symbol::Map<symbol::t> project_closure_map(const symbol::Map<constant_defining_value>& symbol_definition_map) {
  return symbol_definition_map.fold(
      [](symbol::t sym, constant_defining_value c, symbol::Map<symbol::t> acc) {
        if (c->kind == ConstantDefiningValue::Kind::Project_closure) return acc.add(sym, c->sym);
        if (c->kind == ConstantDefiningValue::Kind::Set_of_closures) return acc.add(sym, sym);
        return acc;
      },
      symbol::Map<symbol::t>{});
}

// Symbol.Tbl.map t f = of_map (Map.map f (to_map t)): a fresh table
template <class V, class F>
std::unique_ptr<symbol::Tbl<V>> tbl_map(const symbol::Tbl<V>& t, F&& f) {
  symbol::Map<V> m = tbl_to_map<symbol::Tbl<V>, symbol::Map<V>>(t).map(f);
  auto r = std::make_unique<symbol::Tbl<V>>(m.cardinal());
  tbl_of_map(*r, m);
  return r;
}

}  // namespace

Program lift_constants(const Program& program0) {
  symbol::t the_dead_constant = symbol::of_variable(variable::create(Names::the_dead_constant));
  Program program{program0.imported_symbols,
                  let_symbol(the_dead_constant, fl_allocated(allocated_const::nativeint(0)), program0.program_body)};
  std::shared_ptr<const inconstant_idents::Result> inconstants =
      inconstant_idents::inconstants_on_program(compilation_unit::get_current_exn(), program);
  InitializeSymbolTbl initialize_symbol_tbl0(42);
  symbol::Tbl<constant_defining_value> symbol_definition_tbl(42);
  EffectTbl effect_tbl0(42);
  program_symbols(program, initialize_symbol_tbl0, symbol_definition_tbl, effect_tbl0);
  Assigned assigned;
  assign_symbols_and_collect_constant_definitions(program, *inconstants, assigned);
  const VarToSymbol& var_to_symbol_tbl = assigned.var_to_symbol_tbl;
  const VarToDefinition& var_to_definition_tbl = assigned.var_to_definition_tbl;
  Aliases aliases = AA::run(var_to_definition_tbl, assigned.initialize_symbol_to_definition_tbl,
                            assigned.let_symbol_to_definition_tbl, the_dead_constant);
  replace_definitions_in_initialize_symbol_and_effects(*inconstants, aliases, var_to_symbol_tbl, var_to_definition_tbl,
                                                       initialize_symbol_tbl0, effect_tbl0);
  symbol::Map<constant_defining_value> symbol_definition_map = translate_constant_set_of_closures(
      *inconstants, aliases, var_to_symbol_tbl, var_to_definition_tbl,
      tbl_to_map<symbol::Tbl<constant_defining_value>, symbol::Map<constant_defining_value>>(symbol_definition_tbl));
  symbol::Map<symbol::t> pc_map = project_closure_map(symbol_definition_map);
  symbol::Map<constant_defining_value> translated_definitions = translate_definitions_and_resolve_alias(
      *inconstants, aliases, var_to_symbol_tbl, var_to_definition_tbl, symbol_definition_map, pc_map);
  VarToBlockField var_to_block_field_tbl(42);
  var_to_block_field(aliases, var_to_symbol_tbl, var_to_definition_tbl, var_to_block_field_tbl);
  translated_definitions = introduce_free_variables_in_sets_of_closures(var_to_block_field_tbl, translated_definitions);
  // Add previous Let_symbol to the newly discovered ones
  symbol::Map<constant_defining_value> constant_definitions = symbol::Map<constant_defining_value>::union_(
      [](symbol::t, constant_defining_value c1, constant_defining_value c2) -> std::optional<constant_defining_value> {
        using K = ConstantDefiningValue::Kind;
        if (c1->kind == K::Project_closure && c2->kind == K::Project_closure) {
          if (symbol::equal(c1->sym, c2->sym) && variable::equal(c1->closure_id, c2->closure_id)) return c1;
          misc::fatal_error("Lift_constants: not equal project closure");
        }
        misc::fatal_error("Lift_constants.lift_constants: union");
      },
      symbol_definition_map, translated_definitions);
  // Upon the [Initialize_symbol]s, the [Effect]s and the constant
  // definitions, do the following:
  // 1. Introduce [Let]s to bind variables that are going to be replaced by
  // constants.
  // 2. If a variable bound by a closure gets replaced by a symbol and thus
  // eliminated from the [free_vars] set of the closure, we need to rewrite
  // any subsequent [Project_var] expressions that project that variable.
  auto rewrite_expr = [&](t expr) {
    return flambda_iterators::map_named(
        [&](named n) -> named {
          if (auto* s = as<NSet_of_closures>(n)) {
            const SetOfClosures* ns = introduce_free_variables_in_set_of_closures(var_to_block_field_tbl, s->set);
            return ns == s->set ? n : n_set_of_closures(ns);
          }
          if (auto* pv = as<NProject_var>(n)) return rewrite_project_var(var_to_block_field_tbl, pv->p, n);
          return n;
        },
        expr);
  };
  constant_definitions = constant_definitions.map([&](constant_defining_value c) -> constant_defining_value {
    if (c->kind != ConstantDefiningValue::Kind::Set_of_closures) return c;
    const SetOfClosures* set = flambda_iterators::map_function_bodies(c->set, rewrite_expr);
    return fl_set(introduce_free_variables_in_set_of_closures(var_to_block_field_tbl, set));
  });
  std::unique_ptr<EffectTbl> effect_tbl = tbl_map(effect_tbl0, [&](const EffectDef& e) {
    t r = rewrite_expr(e.expr);
    return EffectDef{r, e.previous};
  });
  std::unique_ptr<InitializeSymbolTbl> initialize_symbol_tbl = tbl_map(initialize_symbol_tbl0, [&](const InitSym& i) {
    std::vector<t> fields;  // List.map: in order
    for (t f : i.fields) fields.push_back(rewrite_expr(f));
    return InitSym{i.tag, slice(fields), i.previous};
  });
  symbol::Set imported_symbols = flambda_utils::imported_symbols(program);
  std::vector<SymbolSCC> components =
      program_graph(imported_symbols, constant_definitions, *initialize_symbol_tbl, *effect_tbl);
  program_body body = end(flambda_utils::root_symbol(program));
  for (const SymbolSCC& c : components)
    body = add_definition_of_symbol(constant_definitions, *initialize_symbol_tbl, *effect_tbl, body, c);
  return flambda_utils::introduce_needed_import_symbols(Program{program.imported_symbols, body});
}

}  // namespace cppcaml::typing::lift_constants
