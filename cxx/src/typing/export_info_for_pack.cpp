// Port of middle_end/flambda/export_info_for_pack.ml (see
// export_info_for_pack.hpp).
//
// The renamed ids are created in ocamlopt's order (they are in the .cmx):
// Export_info.create's labelled arguments are evaluated right to left in
// its parameter order (recursive, invariant_params, ..., symbol_id,
// values), and a record's fields right to left in its type's order.
#include "cppcaml/typing/export_info_for_pack.hpp"

#include <map>

#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::export_info_for_pack {

namespace A = simple_value_approx;
using export_info::Approx;
using export_info::Descr;
using export_info::ValueSetOfClosures;
using Units = OSet<compilation_unit::t, compilation_unit::Cmp>;

namespace {

struct UnitIdLess {
  bool operator()(unit_id::t a, unit_id::t b) const { return unit_id::compare(a, b) < 0; }
};
// (the Tbls are only searched and added to: maps keyed by the ids' value)
std::map<export_id::t, export_id::t, UnitIdLess>& rename_id_state() {
  static std::map<export_id::t, export_id::t, UnitIdLess> t;
  return t;
}
std::map<set_of_closures_id::t, set_of_closures_id::t, UnitIdLess>& rename_set_of_closures_id_state() {
  static std::map<set_of_closures_id::t, set_of_closures_id::t, UnitIdLess> t;
  return t;
}
std::map<set_of_closures_id::t, const A::FunctionDeclarations*, UnitIdLess>& imported_function_declarations_table() {
  static std::map<set_of_closures_id::t, const A::FunctionDeclarations*, UnitIdLess> t;
  return t;
}

// Rename export identifiers' compilation units to denote that they now
// live within a pack.
export_id::t import_eid_for_pack(const Units& units, compilation_unit::t pack, export_id::t id) {
  auto& tbl = rename_id_state();
  if (auto it = tbl.find(id); it != tbl.end()) return it->second;
  export_id::t id2 = units.mem(id->unit) ? export_id::create(pack, id->name) : id;
  tbl.emplace(id, id2);
  return id2;
}

// Similar to [import_eid_for_pack], but for symbols.
symbol::t import_symbol_for_pack(const Units& units, compilation_unit::t pack, symbol::t sym) {
  return units.mem(symbol::compilation_unit(sym)) ? symbol::import_for_pack(pack, sym) : sym;
}

Approx import_approx_for_pack(const Units& units, compilation_unit::t pack, const Approx& approx) {
  switch (approx.kind) {
    case Approx::Kind::Value_symbol: return Approx{Approx::Kind::Value_symbol, nullptr, import_symbol_for_pack(units, pack, approx.sym)};
    case Approx::Kind::Value_id: return Approx{Approx::Kind::Value_id, import_eid_for_pack(units, pack, approx.id), nullptr};
    case Approx::Kind::Value_unknown: return Approx{};
  }
  return Approx{};
}

set_of_closures_id::t import_set_of_closures_id_for_pack(const Units& units, compilation_unit::t pack,
                                                         set_of_closures_id::t id) {
  if (!units.mem(id->unit)) return id;
  auto& tbl = rename_set_of_closures_id_state();
  if (auto it = tbl.find(id); it != tbl.end()) return it->second;
  set_of_closures_id::t id2 = set_of_closures_id::create(pack, id->name);
  tbl.emplace(id, id2);
  return id2;
}

const ValueSetOfClosures* import_set_of_closures(const Units& units, compilation_unit::t pack,
                                                 const ValueSetOfClosures* s) {
  auto imp = [&](const Approx& a) { return import_approx_for_pack(units, pack, a); };
  // (the record's fields right to left)
  symbol::t aliased_symbol = s->aliased_symbol ? import_symbol_for_pack(units, pack, s->aliased_symbol) : nullptr;
  variable::Map<Approx> results = s->results.map(imp);
  variable::Map<Approx> bound_vars = s->bound_vars.map(imp);
  set_of_closures_id::t id = import_set_of_closures_id_for_pack(units, pack, s->set_of_closures_id);
  return make<ValueSetOfClosures>(ValueSetOfClosures{id, bound_vars, s->free_vars, results, aliased_symbol});
}

const Descr* import_descr_for_pack(const Units& units, compilation_unit::t pack, const Descr* descr) {
  using K = Descr::Kind;
  switch (descr->kind) {
    case K::Value_block: {
      std::vector<Approx> fields;  // Array.map: in order
      for (const Approx& a : descr->fields) fields.push_back(import_approx_for_pack(units, pack, a));
      Descr d = *descr;
      d.fields = slice(fields);
      return make<Descr>(d);
    }
    case K::Value_closure: case K::Value_set_of_closures: {
      Descr d = *descr;
      d.set = import_set_of_closures(units, pack, descr->set);
      return make<Descr>(d);
    }
    default: return descr;
  }
}

flambda::t import_code_for_pack(const Units& units, compilation_unit::t pack, flambda::t expr);

const flambda::FunctionDeclarations* import_flambda_function_declarations(const Units& units, compilation_unit::t pack,
                                                                         const flambda::FunctionDeclarations* fd) {
  // Flambda.import_function_declarations_for_pack: the id, then the origin
  set_of_closures_id::t id = import_set_of_closures_id_for_pack(units, pack, fd->set_of_closures_id);
  set_of_closures_id::t origin = import_set_of_closures_id_for_pack(units, pack, fd->set_of_closures_origin);
  return make<flambda::FunctionDeclarations>(flambda::FunctionDeclarations{fd->is_classic_mode, id, origin, fd->funs});
}

flambda::t import_code_for_pack(const Units& units, compilation_unit::t pack, flambda::t expr) {
  auto f = [&](flambda::named n) -> flambda::named {
    using flambda::NK;
    switch (n->kind) {
      case NK::Symbol: return flambda::n_symbol(import_symbol_for_pack(units, pack, flambda::as<flambda::NSymbol>(n)->sym));
      case NK::Read_symbol_field: {
        auto* r = flambda::as<flambda::NRead_symbol_field>(n);
        return flambda::n_read_symbol_field(import_symbol_for_pack(units, pack, r->sym), r->field);
      }
      case NK::Set_of_closures: {
        const flambda::SetOfClosures* s = flambda::as<flambda::NSet_of_closures>(n)->set;
        const flambda::FunctionDeclarations* fd = import_flambda_function_declarations(units, pack, s->function_decls);
        return flambda::n_set_of_closures(
            flambda::create_set_of_closures(fd, s->free_vars, s->specialised_args, s->direct_call_surrogates));
      }
      default: return n;
    }
  };
  return flambda_iterators::map_named(f, expr);
}

const A::FunctionDeclarations* import_function_declarations_approx_for_pack(const Units& units, compilation_unit::t pack,
                                                                           const A::FunctionDeclarations* function_decls) {
  auto& tbl = imported_function_declarations_table();
  set_of_closures_id::t original = function_decls->set_of_closures_id;
  if (auto it = tbl.find(original); it != tbl.end()) return it->second;
  // import_function_declarations_for_pack_aux
  variable::Map<const A::FunctionDeclaration*> funs = function_decls->funs.map([&](const A::FunctionDeclaration* d) {
    return A::update_function_declaration_body(d, [&](flambda::t body) { return import_code_for_pack(units, pack, body); });
  });
  const A::FunctionDeclarations* updated = A::update_function_declarations(function_decls, funs);
  // A.import_function_declarations_for_pack: the record's fields right to
  // left (funs, set_of_closures_origin, set_of_closures_id, is_classic_mode)
  set_of_closures_id::t origin = import_set_of_closures_id_for_pack(units, pack, updated->set_of_closures_origin);
  set_of_closures_id::t id = import_set_of_closures_id_for_pack(units, pack, updated->set_of_closures_id);
  const A::FunctionDeclarations* r =
      make<A::FunctionDeclarations>(A::FunctionDeclarations{updated->is_classic_mode, id, origin, updated->funs});
  tbl.emplace(original, r);
  return r;
}

}  // namespace

const export_info::T* import_for_pack(const Units& pack_units, compilation_unit::t pack, const export_info::T* exp) {
  auto import_soc_id = [&](set_of_closures_id::t id) { return import_set_of_closures_id_for_pack(pack_units, pack, id); };
  auto import_eid = [&](export_id::t id) { return import_eid_for_pack(pack_units, pack, id); };
  set_of_closures_id::Map<const A::FunctionDeclarations*> sets_of_closures =
      exp->sets_of_closures
          .map([&](const A::FunctionDeclarations* fd) {
            return import_function_declarations_approx_for_pack(pack_units, pack, fd);
          })
          .map_keys(import_soc_id);
  // Export_info.create's arguments, right to left in its parameter order
  set_of_closures_id::Map<variable::Set> recursive = exp->recursive.map_keys(import_soc_id);
  set_of_closures_id::Map<variable::Map<variable::Set>> invariant_params = exp->invariant_params.map_keys(import_soc_id);
  symbol::Map<export_id::t> symbol_id =
      exp->symbol_id.map(import_eid).map_keys([&](symbol::t s) { return import_symbol_for_pack(pack_units, pack, s); });
  // import_eidmap import_descr exp.values
  export_info::CUMap<export_id::Map<const Descr*>> mapped = exp->values.map([&](const export_id::Map<const Descr*>& m) {
    return m.map([&](const Descr* d) { return import_descr_for_pack(pack_units, pack, d); }).map_keys(import_eid);
  });
  export_id::Map<const Descr*> flat = mapped.fold(
      [](compilation_unit::t, const export_id::Map<const Descr*>& m, export_id::Map<const Descr*> acc) {
        // Export_id.Map.disjoint_union map acc
        return export_id::Map<const Descr*>::union_(
            [](export_id::t, const Descr*, const Descr*) -> std::optional<const Descr*> {
              misc::fatal_error("Export_info_for_pack: Map.disjoint_union");
            },
            m, acc);
      },
      export_id::Map<const Descr*>{});
  export_info::CUMap<export_id::Map<const Descr*>> values = export_info::nest_eid_map(flat);
  return make<export_info::T>(export_info::T{sets_of_closures, values, symbol_id, exp->offset_fun, exp->offset_fv,
                                             exp->constant_closures, invariant_params, recursive});
}

void clear_import_state() {
  imported_function_declarations_table().clear();
  rename_set_of_closures_id_state().clear();
  rename_id_state().clear();
}

}  // namespace cppcaml::typing::export_info_for_pack
