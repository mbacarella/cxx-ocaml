// Port of middle_end/flambda/alias_analysis.ml (see alias_analysis.hpp).
#include "cppcaml/typing/alias_analysis.hpp"

#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::alias_analysis {

using format::Formatter;
using format::fprintf;

void print_constant_defining_value(Formatter& ppf, constant_defining_value d) {
  using K = ConstantDefiningValue::Kind;
  auto vars = [](Slice<variable::t> l) {
    return [l](Formatter& f) {
      for (std::size_t k = 0; k < l.size(); ++k) {
        if (k > 0) f.print_cut();
        variable::print(f, l[k]);
      }
    };
  };
  switch (d->kind) {
    case K::Allocated_const:
      switch (d->allocated.kind) {
        case AllocatedConst::Kind::Normal: allocated_const::print(ppf, d->allocated.c); break;
        case AllocatedConst::Kind::Array: fprintf(ppf, "[| %a |]", vars(d->allocated.vars)); break;
        case AllocatedConst::Kind::Duplicate_array:
          fprintf(ppf, "dup_array(%a)", pr(variable::print, d->allocated.var));
          break;
      }
      break;
    case K::Block: fprintf(ppf, "[|%d: %a|]", d->tag, vars(d->fields)); break;
    case K::Set_of_closures: flambda::print_set_of_closures(ppf, d->set); break;
    case K::Project_closure: projection::print_project_closure(ppf, d->project_closure); break;
    case K::Move_within_set_of_closures: projection::print_move_within_set_of_closures(ppf, d->move); break;
    case K::Project_var: projection::print_project_var(ppf, d->project_var); break;
    case K::Field: fprintf(ppf, "%a.(%d)", pr(variable::print, d->var), d->field); break;
    case K::Symbol_field: fprintf(ppf, "%a.(%d)", pr(symbol::print, d->sym), d->field); break;
    case K::Const: flambda::print_const(ppf, d->c); break;
    case K::Symbol: symbol::print(ppf, d->sym); break;
    case K::Variable: variable::print(ppf, d->var); break;
  }
}

namespace {
struct Definitions {
  const variable::Tbl<constant_defining_value>& variable;
  const symbol::Tbl<std::vector<variable::t>>& initialize_symbol;
  const symbol::Tbl<flambda::constant_defining_value>& symbol;
  symbol::t the_dead_constant;

  AllocationPoint resolve_definition(variable::t var, constant_defining_value def) {
    using K = ConstantDefiningValue::Kind;
    switch (def->kind) {
      case K::Allocated_const: case K::Block: case K::Set_of_closures: case K::Project_closure: case K::Const:
      case K::Move_within_set_of_closures: return {nullptr, var};
      case K::Project_var: return fetch_variable(def->project_var.var);
      case K::Variable: return fetch_variable(def->var);
      case K::Symbol: return {def->sym, nullptr};
      case K::Field: {
        AllocationPoint p = fetch_variable(def->var);
        if (p.sym) return fetch_symbol_field(p.sym, def->field);
        return fetch_variable_field(p.var, def->field);
      }
      case K::Symbol_field: return fetch_symbol_field(def->sym, def->field);
    }
    return {};
  }
  AllocationPoint fetch_variable(variable::t var) {
    const constant_defining_value* d = variable.find_opt(var);
    if (!d) return {nullptr, var};
    return resolve_definition(var, *d);
  }
  // List.nth: Failure "nth" past the end (not the Not_found the code
  // expects), Invalid_argument below 0
  [[noreturn]] static void nth_failure(long n) {
    misc::fatal_error(n < 0 ? "Invalid_argument(\"List.nth\")" : "Failure(\"nth\")");
  }
  AllocationPoint fetch_variable_field(variable::t var, long field) {
    const constant_defining_value* d = variable.find_opt(var);
    if (!d) {
      Formatter f;
      fprintf(f, "No definition for field access to %a", pr(variable::print, var));
      misc::fatal_error(f.contents());
    }
    using K = ConstantDefiningValue::Kind;
    switch ((*d)->kind) {
      case K::Block: {
        Slice<variable::t> fields = (*d)->fields;
        if (field < 0 || field >= static_cast<long>(fields.size())) nth_failure(field);
        return fetch_variable(fields[static_cast<std::size_t>(field)]);
      }
      case K::Symbol: case K::Variable: case K::Project_var: case K::Field: case K::Symbol_field:
        // Must have been resolved
        misc::fatal_error("Alias_analysis.fetch_variable_field");
      default: return {the_dead_constant, nullptr};
    }
  }
  AllocationPoint fetch_symbol_field(symbol::t sym, long field) {
    if (const flambda::constant_defining_value* d = symbol.find_opt(sym)) {
      if ((*d)->kind == flambda::ConstantDefiningValue::Kind::Block) {
        Slice<flambda::BlockField> fields = (*d)->fields;
        if (field < 0 || field >= static_cast<long>(fields.size())) nth_failure(field);
        const flambda::BlockField& b = fields[static_cast<std::size_t>(field)];
        if (b.sym) return {b.sym, nullptr};
        return {sym, nullptr};
      }
      return {the_dead_constant, nullptr};
    }
    const std::vector<variable::t>* fields = initialize_symbol.find_opt(sym);
    if (!fields) {
      Formatter f;
      fprintf(f, "No definition for field access to %a", pr(symbol::print, sym));
      misc::fatal_error(f.contents());
    }
    if (field < 0 || field >= static_cast<long>(fields->size())) nth_failure(field);
    variable::t v = (*fields)[static_cast<std::size_t>(field)];
    if (!v) {
      Formatter f;
      fprintf(f, "Constant field access to an inconstant %a", pr(symbol::print, sym));
      misc::fatal_error(f.contents());
    }
    return fetch_variable(v);
  }
};
}  // namespace

variable::Map<AllocationPoint> run(const variable::Tbl<constant_defining_value>& variable,
                                   const symbol::Tbl<std::vector<variable::t>>& initialize_symbol,
                                   const symbol::Tbl<flambda::constant_defining_value>& symbol,
                                   symbol::t the_dead_constant) {
  Definitions d{variable, initialize_symbol, symbol, the_dead_constant};
  return variable.fold(
      [&](variable::t var, constant_defining_value def, variable::Map<AllocationPoint> result) {
        AllocationPoint p = d.resolve_definition(var, def);
        return result.add(var, p);
      },
      variable::Map<AllocationPoint>{});
}

}  // namespace cppcaml::typing::alias_analysis
