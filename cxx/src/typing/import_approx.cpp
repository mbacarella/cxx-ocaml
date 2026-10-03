// Port of middle_end/flambda/import_approx.ml (see import_approx.hpp).
#include "cppcaml/typing/import_approx.hpp"

#include <map>

#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/export_info.hpp"
#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::import_approx {

using format::Formatter;
using format::fprintf;
namespace F = flambda;

namespace {
struct UnitIdLess {
  bool operator()(set_of_closures_id::t a, set_of_closures_id::t b) const { return unit_id::compare(a, b) < 0; }
};
// Set_of_closures_id.Tbl (memoize): only looked up
std::map<set_of_closures_id::t, const A::FunctionDeclarations*, UnitIdLess>& imported_sets_of_closures_table() {
  static std::map<set_of_closures_id::t, const A::FunctionDeclarations*, UnitIdLess> t;
  return t;
}

const A::FunctionDeclarations* import_function_declarations(const A::FunctionDeclarations* clos) {
  // CR-soon mshinwell for pchambart: Do we still need to do this rewriting?
  symbol::Map<variable::t> sym_map = clos->funs.fold(
      [](variable::t fun_var, const A::FunctionDeclaration*, symbol::Map<variable::t> acc) {
        symbol::t sym = compilenv::closure_symbol(fun_var);
        return acc.add(sym, fun_var);
      },
      symbol::Map<variable::t>{});
  auto f_named = [&](F::named n) -> F::named {
    if (auto* s = F::as<F::NSymbol>(n))
      if (const variable::t* v = sym_map.find_opt(s->sym)) return F::n_expr(F::var(*v));
    return n;
  };
  variable::Map<const A::FunctionDeclaration*> funs = clos->funs.map([&](const A::FunctionDeclaration* d) {
    return A::update_function_declaration_body(d, [&](F::t body) {
      return flambda_iterators::map_toplevel_named(f_named, body);
    });
  });
  return A::update_function_declarations(clos, funs);
}

// import_set_of_closures (memoized; null: None)
const A::FunctionDeclarations* import_set_of_closures(set_of_closures_id::t set_of_closures_id) {
  auto& table = imported_sets_of_closures_table();
  if (auto it = table.find(set_of_closures_id); it != table.end()) return it->second;
  const A::FunctionDeclarations* r = nullptr;
  if (const export_info::T* ex_info = compilenv::approx_for_global(set_of_closures_id->unit)) {
    const auto* fd = ex_info->sets_of_closures.find_opt(set_of_closures_id);
    if (!fd) misc::fatal_error("Cannot find set of closures");
    r = import_function_declarations(*fd);
  }
  table.emplace(set_of_closures_id, r);
  return r;
}

A::t import_approx(const export_info::Approx& ap);

const A::ValueSetOfClosures* import_value_set_of_closures(set_of_closures_id::t set_of_closures_id,
                                                          const variable::Map<export_info::Approx>& bound_vars,
                                                          const variable::Map<F::SpecialisedTo>& free_vars,
                                                          const export_info::T* ex_info, export_id::t ex,
                                                          const std::function<std::string()>& what) {
  variable::Map<A::t> bvs = bound_vars.map([](const export_info::Approx& a) { return import_approx(a); });
  const A::FunctionDeclarations* function_decls = import_set_of_closures(set_of_closures_id);
  if (!function_decls) return nullptr;
  // CR-someday xclerc: add a test to the test suite to ensure that classic
  // mode behaves as expected.
  bool is_classic_mode = function_decls->is_classic_mode;
  variable::Map<variable::Set> invariant_params;
  if (const auto* found = ex_info->invariant_params.find_opt(set_of_closures_id)) invariant_params = *found;
  else if (!is_classic_mode) {
    Formatter f;
    fprintf(f, "Set of closures ID %a not found in invariant_params (when importing [%a: %s])",
            pr(unit_id::print, set_of_closures_id), pr(unit_id::print, ex), what());
    misc::fatal_error(f.contents());
  }
  variable::Set recursive;
  if (const auto* found = ex_info->recursive.find_opt(set_of_closures_id)) recursive = *found;
  else if (!is_classic_mode) {
    Formatter f;
    fprintf(f, "Set of closures ID %a not found in recursive (when importing [%a: %s])",
            pr(unit_id::print, set_of_closures_id), pr(unit_id::print, ex), what());
    misc::fatal_error(f.contents());
  }
  return A::create_value_set_of_closures(function_decls, bvs, free_vars,
                                         A::Lazy<variable::Map<variable::Set>>::from_val(invariant_params),
                                         A::Lazy<variable::Set>::from_val(recursive), {},
                                         freshening::project_var::empty(), {});
}

A::t import_ex(export_id::t ex) {
  compilation_unit::t compilation_unit = ex->unit;
  const export_info::T* ex_info = compilenv::approx_for_global(compilation_unit);
  if (!ex_info) return A::value_unknown(A::other());
  const export_info::Descr* d = export_info::find_description(ex_info, ex);
  if (!d) {
    Formatter f;
    fprintf(f, "Cannot find export id %a", pr(unit_id::print, ex));
    misc::fatal_error(f.contents());
  }
  using K = export_info::Descr::Kind;
  switch (d->kind) {
    case K::Value_unknown_descr: return A::value_unknown(A::other());
    case K::Value_int: return A::value_int(d->n);
    case K::Value_char: return A::value_char(d->n);
    case K::Value_float: return A::value_float(d->f);
    case K::Value_float_array: {
      if (!d->float_array.contents_known) return A::value_mutable_float_array(d->float_array.size);
      std::vector<A::t> c;  // Array.map: in order
      for (const std::optional<double>& x : d->float_array.contents)
        c.push_back(x ? A::value_float(*x) : A::value_any_float());
      return A::value_immutable_float_array(slice(c));
    }
    case K::Value_boxed_int: return A::value_boxed_int(d->bi, d->bival);
    case K::Value_string: return A::value_string(d->str.size, d->str.contents);
    case K::Value_mutable_block: return A::value_unknown(A::other());
    case K::Value_block: {
      std::vector<A::t> fields;  // Array.map: in order
      for (const export_info::Approx& a : d->fields) fields.push_back(import_approx(a));
      return A::value_block(d->tag, slice(fields));
    }
    case K::Value_closure: {
      const export_info::ValueSetOfClosures* s = d->set;
      variable::t closure_id = d->closure_id;
      const A::ValueSetOfClosures* vsoc = import_value_set_of_closures(
          s->set_of_closures_id, s->bound_vars, s->free_vars, ex_info, ex, [&] {
            Formatter f;
            fprintf(f, "Value_closure %a", pr(variable::print, closure_id));
            return f.contents();
          });
      if (!vsoc) return A::value_unresolved(A::UnresolvedValue{s->set_of_closures_id, nullptr});
      return A::value_closure(vsoc, closure_id, nullptr, nullptr, s->aliased_symbol);
    }
    case K::Value_set_of_closures: {
      const export_info::ValueSetOfClosures* s = d->set;
      const A::ValueSetOfClosures* vsoc = import_value_set_of_closures(
          s->set_of_closures_id, s->bound_vars, s->free_vars, ex_info, ex, [] { return std::string("Value_set_of_closures"); });
      if (!vsoc) return A::value_unresolved(A::UnresolvedValue{s->set_of_closures_id, nullptr});
      A::t approx = A::value_set_of_closures(vsoc);
      if (!s->aliased_symbol) return approx;
      return A::augment_with_symbol(approx, s->aliased_symbol);
    }
  }
  return A::value_unknown(A::other());
}

A::t import_approx(const export_info::Approx& ap) {
  switch (ap.kind) {
    case export_info::Approx::Kind::Value_unknown: return A::value_unknown(A::other());
    case export_info::Approx::Kind::Value_id: return A::value_extern(ap.id);
    case export_info::Approx::Kind::Value_symbol: return A::value_symbol(ap.sym);
  }
  return A::value_unknown(A::other());
}
}  // namespace

void clear_imported_sets_of_closures_table() { imported_sets_of_closures_table().clear(); }

A::t import_symbol(symbol::t sym) {
  if (compilenv::is_predefined_exception(sym)) return A::value_unknown(A::other());
  compilation_unit::t compilation_unit = symbol::compilation_unit(sym);
  const export_info::T* export_info = compilenv::approx_for_global(compilation_unit);
  if (!export_info) return A::value_unresolved(A::UnresolvedValue{nullptr, sym});
  const export_id::t* approx = export_info->symbol_id.find_opt(sym);
  if (!approx) {
    Formatter f;
    fprintf(f, "Compilation unit = %a Cannot find symbol %a", pr(compilation_unit::print, compilation_unit),
            pr(symbol::print, sym));
    misc::fatal_error(f.contents());
  }
  return A::augment_with_symbol(import_ex(*approx), sym);
}

// Note for code reviewers: Observe that [really_import] iterates until the
// approximation description is fully resolved (or a necessary .cmx file is
// missing).
A::Descr really_import(const A::Descr& approx) {
  A::Descr d = approx;
  for (;;) {
    if (d.kind == A::DK::Value_extern) d = import_ex(d.ex)->descr;
    else if (d.kind == A::DK::Value_symbol) d = import_symbol(d.sym)->descr;
    else return d;
  }
}

A::t really_import_approx(A::t approx) { return A::replace_description(approx, really_import(approx->descr)); }

}  // namespace cppcaml::typing::import_approx
