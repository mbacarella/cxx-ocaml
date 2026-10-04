// Port of middle_end/compilenv.ml (the Closure half).  See compilenv.hpp.
#include "cppcaml/typing/compilenv.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/export_info.hpp"
#include "cppcaml/typing/import_approx.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/persistent_env.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::compilenv {

using namespace clambda;
using cmx_format::UnitInfos;

namespace {

// global_infos_table: (string, unit_infos option) Hashtbl.t
std::unordered_map<std::string, const UnitInfos*>& global_infos_table() {
  static std::unordered_map<std::string, const UnitInfos*> t;
  return t;
}

struct CstLess {
  bool operator()(const UStructuredConstant* a, const UStructuredConstant* b) const {
    return compare_structured_constants(a, b) < 0;
  }
};
// structured_constants: {strcst_shared: string CstMap.t; strcst_all:
// ustructured_constant SymMap.t}, with the insertions logged so that
// backtrack can undo those made after a snapshot
struct Constants {
  std::map<const UStructuredConstant*, std::string_view, CstLess> shared;
  std::map<std::string_view, const UStructuredConstant*> all;
  struct Insert {
    std::string_view sym;
    const UStructuredConstant* shared_key;  // nullptr: not added to strcst_shared
  };
  std::vector<Insert> log;
};
Constants& constants() {
  static Constants c;
  return c;
}
std::set<std::string_view>& exported_constants() {
  static std::set<std::string_view> s;
  return s;
}

// linuxlike_mangling

std::string symbolname_for_pack(const std::optional<std::string>& pack, std::string_view name) {
  if (!pack) return std::string(name);
  return concat_symbol(*pack, name);
}

// Referring to a packed unit is only allowed from a unit that will
// ultimately end up in the same pack, including through nested packs.
bool is_import_from_same_pack(std::string_view imported, std::string_view current) {
  std::string prefix = concat_symbol(imported, "");
  return imported == current || current.substr(0, prefix.size()) == prefix;
}

long const_label = 0;

// the flambda half's tables
struct FlambdaState {
  std::unordered_map<std::string, const export_info::T*> export_infos_table;
  const export_info::T* merged_environment = nullptr;  // (null: Export_info.empty)
};
FlambdaState& flambda_state() {
  static FlambdaState s;
  return s;
}

}  // namespace

UnitInfos& current_unit() {
  static UnitInfos ui{};
  return ui;
}

std::string concat_symbol(std::string_view unitname, std::string_view id) {
  std::string s(unitname);
  s += symbol_separator;
  s += id;
  return s;
}

std::string_view make_symbol_in(std::string_view unitname, std::optional<std::string_view> idopt) {
  std::string prefix = "caml" + std::string(unitname);
  if (!idopt) return zstr(prefix);
  return zstr(concat_symbol(prefix, *idopt));
}
std::string_view make_symbol(std::optional<std::string_view> idopt) {
  return make_symbol_in(current_unit().ui_symbol, idopt);
}

void reset(const std::optional<std::string>& packname0, std::string_view name) {
  std::optional<std::string> packname;
  if (packname0) {
    std::string p = *packname0;
    std::replace(p.begin(), p.end(), '.', symbol_separator);
    packname = p;
  }
  global_infos_table().clear();
  UnitInfos& cu = current_unit();
  cu.ui_name = uid::unit_name_string(name);  // Unit_info.modname: the one string (__MODULE__ shares it)
  // symbolname_for_pack None name = name: the one string
  std::string_view symbol = packname ? zstr(symbolname_for_pack(packname, name)) : cu.ui_name;
  cu.ui_symbol = symbol;
  cu.ui_defines = {symbol};
  cu.ui_imports_cmi.clear();
  cu.ui_imports_cmx.clear();
  cu.ui_curry_fun.clear();
  cu.ui_apply_fun.clear();
  cu.ui_send_fun.clear();
  cu.ui_force_link = clflags::link_everything;
  cu.ui_for_pack = packname ? std::optional<std::string_view>(zstr(*packname)) : std::nullopt;
  cu.ui_need_stdlib = false;
  exported_constants().clear();
  constants() = Constants{};
  cu.ui_export_info = value_unknown();
  if constexpr (config::flambda) {
    // merged_environment := Export_info.empty; Hashtbl.clear
    // export_infos_table (and imported_sets_of_closures_table)
    flambda_state() = FlambdaState{};
    // (default_ui_export_info: Flambda Export_info.empty)
    cu.ui_flambda_export_info = export_info::empty();
    import_approx::clear_imported_sets_of_closures_table();
    // (the unit outlives the typing zone current here: in the permanent
    // zone, its ident and linkage name with it)
    ZoneScope in_permanent(permanent_zone());
    compilation_unit::t c =
        compilation_unit::create(Ident::create_persistent(name), current_unit_linkage_name());
    compilation_unit::set_current(c);
  }
}

std::string_view current_unit_name() { return current_unit().ui_name; }

// Read and cache info on global identifiers
const UnitInfos* get_global_info(Ident::t global_ident) {
  std::string modname(ident::name(global_ident));
  UnitInfos& cu = current_unit();
  if (modname == cu.ui_name) return &cu;
  auto& table = global_infos_table();
  if (auto it = table.find(modname); it != table.end()) return it->second;
  // (cached for the whole compilation: in the permanent zone)
  ZoneScope in_permanent(permanent_zone());
  const UnitInfos* infos = nullptr;
  std::optional<std::string> crc;
  if (!env::is_imported_opaque(modname)) {
    std::string filename;
    bool found = true;
    try {
      filename = load_path::find_normalized(modname + ".cmx");
    } catch (const load_path::NotFound&) {
      found = false;
    }
    if (found) {
      std::pair<UnitInfos*, std::string> r;
      try {
        r = cmx_format::read_unit_info(filename);
      } catch (const cmx_format::Error& e) {
        Error err(e.kind == cmx_format::Error::Kind::Not_a_unit_info ? Error::Kind::Not_a_unit_info
                                                                      : Error::Kind::Corrupted_unit_info);
        err.filename = e.filename;
        throw err;
      }
      UnitInfos* ui = r.first;
      if (ui->ui_name != modname) {
        Error err(Error::Kind::Illegal_renaming);
        err.name = modname;
        err.modname = std::string(ui->ui_name);
        err.filename = filename;
        throw err;
      }
      // Linking to a compilation unit expected to go into a pack
      // (ui_for_pack = Some ...) is possible only from inside the same
      // pack, but it is perfectly ok to link to an unit outside of the pack.
      if (ui->ui_for_pack &&
          !(cu.ui_for_pack && is_import_from_same_pack(*ui->ui_for_pack, *cu.ui_for_pack))) {
        Error err(Error::Kind::Mismatching_for_pack);
        err.filename = filename;
        err.pack_1 = std::string(*ui->ui_for_pack);
        err.current_unit = std::string(cu.ui_name);
        if (cu.ui_for_pack) err.pack_2 = std::string(*cu.ui_for_pack);
        throw err;
      }
      infos = ui;
      crc = r.second;
    } else {
      location::prerr_warning(location::none(),
                              warnings::Warning::with_s(warnings::Warning::K::No_cmx_file, modname));
    }
  }
  // (Ident.name global_ident, crc): the ident's own name string
  cu.ui_imports_cmx.insert(cu.ui_imports_cmx.begin(), {ident::name(global_ident), crc});
  table.emplace(modname, infos);
  return infos;
}

void require_global(Ident::t global_ident) {
  if (!ident::is_predef(global_ident)) get_global_info(global_ident);
}

// Return the approximation of a global identifier
const ValueApproximation* global_approx(Ident::t id) {
  if (ident::is_predef(id)) return value_unknown();
  const UnitInfos* ui = get_global_info(id);
  return ui ? ui->ui_export_info : value_unknown();
}

// The name of the symbol defined globally for %standard_library_default
Ident::t stdlib_symbol_name() {
  // (process-long: in the permanent zone, whichever zone is current)
  static Ident::t id = [] {
    ZoneScope in_permanent(permanent_zone());
    return Ident::create_persistent("caml_standard_library_nat");
  }();
  return id;
}

// Return the symbol used to refer to a global identifier
std::string_view symbol_for_global(Ident::t id) {
  if (ident::is_predef(id)) return zstr("caml_exn_" + std::string(ident::name(id)));
  if (ident::same(stdlib_symbol_name(), id)) return ident::name(id);
  const UnitInfos* ui = get_global_info(id);
  if (!ui) return make_symbol_in(ident::name(id), std::nullopt);
  return make_symbol_in(ui->ui_symbol, std::nullopt);
}

// unit_for_global id
compilation_unit::t unit_for_global(Ident::t id) {
  linkage_name::t sym_label = symbol_for_global(id);
  return compilation_unit::create(id, sym_label);
}

compilation_unit::t predefined_exception_compilation_unit() {
  static compilation_unit::t c = [] {
    ZoneScope in_permanent(permanent_zone());
    return compilation_unit::create(Ident::create_persistent("__dummy__"), std::string_view("__dummy__"));
  }();
  return c;
}

bool is_predefined_exception(symbol::t sym) {
  return compilation_unit::equal(predefined_exception_compilation_unit(), symbol::compilation_unit(sym));
}

// symbol_for_global' id
symbol::t symbol_for_global_prime(Ident::t id) {
  linkage_name::t sym_label = symbol_for_global(id);
  if (ident::is_predef(id) || ident::same(stdlib_symbol_name(), id))
    return symbol::of_global_linkage(predefined_exception_compilation_unit(), sym_label);
  return symbol::of_global_linkage(unit_for_global(id), sym_label);
}

linkage_name::t current_unit_linkage_name() { return make_symbol_in(current_unit().ui_symbol, std::nullopt); }

compilation_unit::t current_compilation_unit() {
  compilation_unit::t c = compilation_unit::get_current();
  if (!c) misc::fatal_error("Compilenv.current_unit");
  return c;
}

symbol::t current_unit_symbol() {
  return symbol::of_global_linkage(current_compilation_unit(), current_unit_linkage_name());
}

const export_info::T* approx_for_global(compilation_unit::t comp_unit) {
  Ident::t id = comp_unit->id;
  if (compilation_unit::equal(predefined_exception_compilation_unit(), comp_unit) || ident::is_predef(id) ||
      !ident::global(id))
    misc::fatal_error("approx_for_global " + ident::unique_name(id));  // (invalid_arg)
  std::string modname(ident::name(id));
  FlambdaState& st = flambda_state();
  if (auto it = st.export_infos_table.find(modname); it != st.export_infos_table.end()) return it->second;
  ZoneScope in_permanent(permanent_zone());
  const UnitInfos* ui = get_global_info(id);
  if (!ui) return nullptr;
  const export_info::T* exported = ui->ui_flambda_export_info;
  if (!exported) misc::fatal_error("Compilenv.get_flambda_export_info");
  st.export_infos_table.emplace(modname, exported);
  st.merged_environment = export_info::merge(approx_env(), exported);
  return exported;
}

const export_info::T* approx_env() {
  FlambdaState& st = flambda_state();
  return st.merged_environment ? st.merged_environment : export_info::empty();
}

void set_export_info(const export_info::T* ei) { current_unit().ui_flambda_export_info = ei; }

symbol::t closure_symbol(variable::t fv) {
  compilation_unit::t cu = variable::get_compilation_unit(fv);
  std::string linkage_name = concat_symbol(cu->linkage_name, variable::unique_name(fv) + "_closure");
  return symbol::of_global_linkage(cu, permanent_zone().str(linkage_name));
}

std::string_view function_label(variable::t fv) {
  compilation_unit::t cu = variable::get_compilation_unit(fv);
  return permanent_zone().str(concat_symbol(cu->linkage_name, variable::unique_name(fv)));
}

void set_global_approx(const ValueApproximation* approx) { current_unit().ui_export_info = approx; }

// Record that a currying function or application function is needed
namespace {
void need(std::vector<long>& funs, long n) {
  // ui_*_fun <- n :: ui_*_fun
  if (std::find(funs.begin(), funs.end(), n) == funs.end()) funs.insert(funs.begin(), n);
}
}  // namespace
void need_curry_fun(long n) { need(current_unit().ui_curry_fun, n); }
void need_apply_fun(long n) { need(current_unit().ui_apply_fun, n); }
void need_send_fun(long n) { need(current_unit().ui_send_fun, n); }
void need_stdlib_location() { current_unit().ui_need_stdlib = true; }

std::string_view new_const_symbol() {
  ++const_label;
  return make_symbol(std::string_view(std::to_string(const_label)));
}

Snapshot snapshot() { return {constants().log.size()}; }
void backtrack(Snapshot s) {
  Constants& c = constants();
  while (c.log.size() > s.log_size) {
    const Constants::Insert& ins = c.log.back();
    c.all.erase(ins.sym);
    if (ins.shared_key) c.shared.erase(ins.shared_key);
    c.log.pop_back();
  }
}

std::string_view new_structured_constant(const UStructuredConstant* cst, bool shared) {
  Constants& c = constants();
  if (shared) {
    if (auto it = c.shared.find(cst); it != c.shared.end()) return it->second;
    std::string_view lbl = new_const_symbol();
    c.shared.emplace(cst, lbl);
    c.all[lbl] = cst;
    c.log.push_back({lbl, cst});
    return lbl;
  }
  std::string_view lbl = new_const_symbol();
  c.all[lbl] = cst;
  c.log.push_back({lbl, nullptr});
  return lbl;
}

void add_exported_constant(std::string_view s) { exported_constants().insert(s); }

void clear_structured_constants() { constants() = Constants{}; }

const UStructuredConstant* structured_constant_of_symbol(std::string_view s) {
  auto& all = constants().all;
  auto it = all.find(s);
  return it == all.end() ? nullptr : it->second;
}

std::vector<PreallocatedConstant> structured_constants() {
  auto* provenance = make<USymbolProvenance>(
      USymbolProvenance{{}, Path::pident(Ident::create_persistent(current_unit_name()))});
  std::vector<PreallocatedConstant> r;
  for (auto& [symbol, definition] : constants().all)
    r.push_back({symbol, exported_constants().count(symbol) > 0, definition, provenance});
  return r;
}

std::string error_message(const Error& e) {
  auto q = [](const std::string& s) { return "\"" + s + "\""; };
  switch (e.kind) {
    case Error::Kind::Not_a_unit_info: return q(e.filename) + " is not a compilation unit description.";
    case Error::Kind::Corrupted_unit_info: return "Corrupted compilation unit description " + q(e.filename);
    case Error::Kind::Illegal_renaming:
      return q(e.filename) + " contains the description for unit \"" + e.name + "\" when \"" + e.modname +
             "\" was expected";
    case Error::Kind::Mismatching_for_pack:
      if (!e.pack_2)
        return q(e.filename) + " was built with \"-for-pack " + e.pack_1 + "\", but the current unit \"" +
               e.current_unit + "\" is not";
      return q(e.filename) + " was built with \"-for-pack " + e.pack_1 + "\", but the current unit \"" +
             e.current_unit + "\" is built with \"-for-pack " + *e.pack_2 + "\"";
  }
  return {};
}

void cache_unit_info(const UnitInfos* ui) { global_infos_table()[std::string(ui->ui_name)] = ui; }

void report_error_doc(format_doc::Formatter& ppf, const Error& e) {
  namespace fd = format_doc;
  auto qf = [](const std::string& f) { return [f](fd::Formatter& ff) { location::doc::quoted_filename(ff, f); }; };
  using misc::style::code_str;
  switch (e.kind) {
    case Error::Kind::Not_a_unit_info: fd::fprintf(ppf, "%a@ is not a compilation unit description.", qf(e.filename)); break;
    case Error::Kind::Corrupted_unit_info:
      fd::fprintf(ppf, "Corrupted compilation unit description@ %a", qf(e.filename));
      break;
    case Error::Kind::Illegal_renaming:
      fd::fprintf(ppf, "%a@ contains the description for unit@ %a when %a was expected", qf(e.filename),
                  code_str(e.name), code_str(e.modname));
      break;
    case Error::Kind::Mismatching_for_pack:
      if (!e.pack_2)
        fd::fprintf(ppf, "%a@ was built with %a, but the @ current unit %a is not", qf(e.filename),
                    code_str("-for-pack " + e.pack_1), code_str(e.current_unit));
      else
        fd::fprintf(ppf, "%a@ was built with %a, but the @ current unit %a is built with %a", qf(e.filename),
                    code_str("-for-pack " + e.pack_1), code_str(e.current_unit), code_str("-for-pack " + *e.pack_2));
      break;
  }
}

}  // namespace cppcaml::typing::compilenv
