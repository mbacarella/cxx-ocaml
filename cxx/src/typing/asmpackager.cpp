// Port of asmcomp/asmpackager.ml; see asmpackager.hpp.
#include "cppcaml/typing/asmpackager.hpp"
#include "cppcaml/typing/flambda_middle_end.hpp"
#include "cppcaml/typing/export_info_for_pack.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <optional>

#include "cppcaml/typing/asmgen.hpp"
#include "cppcaml/typing/asmlink.hpp"
#include "cppcaml/typing/ccomp.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/closure.hpp"
#include "cppcaml/typing/cmx_format.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/filename.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/persistent_env.hpp"
#include "cppcaml/typing/simplif.hpp"
#include "cppcaml/typing/translmod.hpp"
#include "cppcaml/typing/typemod.hpp"

namespace cppcaml::typing::asmpackager {

namespace {
namespace cf = clflags;
using cmx_format::UnitInfos;

[[noreturn]] void fail(Error::Kind k, const std::string& a, const std::string& b = {}, const std::string& c = {}) {
  Error e{};
  e.kind = k;
  e.a = a;
  e.b = b;
  e.c = c;
  throw e;
}

// Unit_info.lax_modname_from_source
std::string modname_from_source(const std::string& f) {
  std::string base = filename::basename(f);
  std::size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  if (!base.empty() && base[0] >= 'a' && base[0] <= 'z') base[0] = static_cast<char>(base[0] - 'a' + 'A');
  return base;
}

// Read the unit information from a .cmx file
struct PackMember {
  std::string pm_file;
  std::string pm_name;
  const UnitInfos* pm_impl = nullptr;  // PM_intf | PM_impl of unit_infos
};

PackMember read_member_info(const std::string& pack_path, const std::string& file) {
  PackMember m;
  m.pm_file = file;
  m.pm_name = modname_from_source(file);
  if (filename::check_suffix(file, ".cmi")) return m;
  auto [info, crc] = cmx_format::read_unit_info(file);
  if (info->ui_name != m.pm_name) fail(Error::Kind::Illegal_renaming, m.pm_name, file, std::string(info->ui_name));
  std::string expected_symbol = std::string(compilenv::current_unit().ui_symbol) + compilenv::symbol_separator() + std::string(info->ui_name);
  if (info->ui_symbol != expected_symbol) fail(Error::Kind::Wrong_for_pack, file, pack_path);
  asmlink::check_consistency(file, *info, crc);
  compilenv::cache_unit_info(info);
  m.pm_impl = info;
  return m;
}

// Check absence of forward references
void check_units(const std::vector<PackMember>& members) {
  std::vector<std::string> forbidden;
  for (const PackMember& mb : members) forbidden.push_back(mb.pm_name);
  for (const PackMember& mb : members) {
    if (mb.pm_impl)
      for (auto& [unit, _] : mb.pm_impl->ui_imports_cmx)
        if (std::find(forbidden.begin(), forbidden.end(), unit) != forbidden.end())
          fail(Error::Kind::Forward_reference, mb.pm_file, std::string(unit));
    // list_remove: the first occurrence
    if (auto it = std::find(forbidden.begin(), forbidden.end(), mb.pm_name); it != forbidden.end()) forbidden.erase(it);
  }
}

// Make the .o file for the package
void make_package_object(std::ostream& ppf_dump, const std::vector<PackMember>& members, const std::string& target,
                         const typedtree::ModuleCoercion* coercion) {
  std::string objtemp = cf::keep_asm_file
                            ? filename::remove_extension(target) + ".pack" + config::ext_obj
                            // Put the full name of the module in the temporary file name to avoid
                            // collisions with MSVC's link /lib in case of successive packs
                            : filename::temp_file(std::string(compilenv::make_symbol(std::string_view(""))),
                                                  config::ext_obj);
  std::vector<Ident::t> components;
  for (const PackMember& m : members)
    components.push_back(m.pm_impl ? Ident::create_persistent(zstr(m.pm_name)) : nullptr);
  // (Unit_info.Artifact.modname target: the string Compilenv.reset was
  // given, the unit's ui_name)
  Ident::t module_ident = Ident::create_persistent(compilenv::current_unit().ui_name);
  std::string prefixname = filename::remove_extension(objtemp);
  auto [size, code0] = config::flambda ? translmod::transl_package_flambda(slice(components), coercion)
                                        : translmod::transl_store_package(slice(components), module_ident, coercion);
  lambda::lambda code = simplif::simplify_lambda(code0);
  lambda::Program program;
  program.code = code;
  program.main_module_block_size = size;
  program.module_ident = module_ident;
  // Asmgen.compile_implementation (no required globals)
  format::Formatter dump;
  try {
    asmgen::compile_unit(asmgen::asm_filename(prefixname), cf::keep_asm_file, prefixname + config::ext_obj, [&] {
      closure_middle_end::WithConstants clambda = [&] {
        if constexpr (config::flambda) return flambda_middle_end::lambda_to_clambda(dump, prefixname, program, code);
        else return closure_middle_end::lambda_to_clambda(dump, program, code);
      }();
      return asmgen::end_gen_implementation(dump, clambda);
    });
  } catch (...) {
    ppf_dump << dump.contents();
    throw;
  }
  ppf_dump << dump.contents();
  ppf_dump.flush();
  std::vector<std::string> files{objtemp};
  for (const PackMember& m : members)
    if (m.pm_impl) files.push_back(filename::remove_extension(m.pm_file) + config::ext_obj);
  int exitcode = ccomp::call_linker(ccomp::LinkMode::Partial, target, files, "");
  std::remove(objtemp.c_str());
  if (exitcode != 0) fail(Error::Kind::Linking_error, "");
}

// Make the .cmx file for the package
void build_package_cmx(const std::vector<PackMember>& members, const std::string& cmxfile) {
  auto is_member = [&](std::string_view name) {
    for (const PackMember& m : members)
      if (m.pm_name == name) return true;
    return false;
  };
  auto filter = [&](const cmx_format::Crcs& l) {
    cmx_format::Crcs r;
    for (auto& e : l)
      if (!is_member(e.first)) r.push_back(e);
    return r;
  };
  std::vector<const UnitInfos*> units;
  for (const PackMember& m : members)
    if (m.pm_impl) units.push_back(m.pm_impl);
  // flambda: the members' export info renamed into the pack, then merged
  // with the pack's own (renamed last)
  const export_info::T* flambda_export_info = nullptr;
  if constexpr (config::flambda) {
    OSet<compilation_unit::t, compilation_unit::Cmp> pack_units;
    for (const UnitInfos* u : units) {
      Ident::t unit_id = Ident::create_persistent(u->ui_name);  // Compilenv.unit_id_from_name
      pack_units = pack_units.add(compilenv::unit_for_global(unit_id));
    }
    compilation_unit::t pack = compilenv::current_compilation_unit();
    std::vector<const UnitInfos*> imported;
    for (const UnitInfos* u : units) {  // List.map: in order
      auto* c = make<UnitInfos>(*u);
      c->ui_flambda_export_info = export_info_for_pack::import_for_pack(pack_units, pack, u->ui_flambda_export_info);
      imported.push_back(c);
    }
    units = imported;
    flambda_export_info = export_info_for_pack::import_for_pack(pack_units, pack, compilenv::current_unit().ui_flambda_export_info);
    for (const UnitInfos* u : units) flambda_export_info = export_info::merge(flambda_export_info, u->ui_flambda_export_info);
    export_info_for_pack::clear_import_state();
  }
  // union: List.fold_left (List.fold_left (fun accu n -> if mem then accu else n :: accu)) []
  auto union_ = [&](std::vector<long> UnitInfos::*field) {
    std::vector<long> accu;
    for (const UnitInfos* u : units)
      for (long n : u->*field)
        if (std::find(accu.begin(), accu.end(), n) == accu.end()) accu.insert(accu.begin(), n);
    return accu;
  };
  UnitInfos& ui = compilenv::current_unit();
  UnitInfos pkg;
  pkg.ui_name = ui.ui_name;
  pkg.ui_symbol = ui.ui_symbol;
  for (const UnitInfos* u : units) pkg.ui_defines.insert(pkg.ui_defines.end(), u->ui_defines.begin(), u->ui_defines.end());
  pkg.ui_defines.push_back(ui.ui_symbol);
  pkg.ui_imports_cmi.push_back({ui.ui_name, env::crc_of_unit(std::string(ui.ui_name))});
  for (auto& e : filter(asmlink::extract_crc_interfaces())) pkg.ui_imports_cmi.push_back(e);
  pkg.ui_imports_cmx = filter(asmlink::extract_crc_implementations());
  pkg.ui_curry_fun = union_(&UnitInfos::ui_curry_fun);
  pkg.ui_apply_fun = union_(&UnitInfos::ui_apply_fun);
  pkg.ui_send_fun = union_(&UnitInfos::ui_send_fun);
  pkg.ui_force_link = false;
  pkg.ui_need_stdlib = false;
  for (const UnitInfos* u : units) {
    pkg.ui_force_link = pkg.ui_force_link || u->ui_force_link;
    pkg.ui_need_stdlib = pkg.ui_need_stdlib || u->ui_need_stdlib;
  }
  pkg.ui_export_info = ui.ui_export_info;
  pkg.ui_flambda_export_info = flambda_export_info;
  pkg.ui_for_pack = std::nullopt;
  std::string bytes = cmx_format::write_unit_info(pkg);
  std::ofstream os(cmxfile, std::ios::binary);
  os << bytes;
}
}  // namespace

// The entry point
void package_files(std::ostream& ppf_dump, env::t initial_env, const std::vector<std::string>& files0,
                   const std::string& targetcmx) {
  std::vector<std::string> files;
  for (const std::string& f : files0) {
    try {
      files.push_back(load_path::find(f));
    } catch (const load_path::NotFound&) {
      fail(Error::Kind::File_not_found, f);
    }
  }
  std::string prefix = filename::remove_extension(targetcmx);
  std::string cmi = prefix + ".cmi";
  std::string obj = prefix + config::ext_obj;
  std::string modname = modname_from_source(cmi);
  // Set the name of the current "input"
  location::input_name = targetcmx;
  // Set the name of the current compunit
  compilenv::reset(cf::for_package, zstr(modname));
  try {
    const typedtree::ModuleCoercion* coercion = typemod::package_units(initial_env, files, modname, cmi);
    // package_object_files
    std::string pack_path = cf::for_package ? *cf::for_package + "." + modname_from_source(obj) : modname_from_source(obj);
    std::vector<PackMember> members;
    for (const std::string& f : files) members.push_back(read_member_info(pack_path, f));
    check_units(members);
    make_package_object(ppf_dump, members, obj, coercion);
    build_package_cmx(members, targetcmx);
  } catch (...) {
    std::remove(targetcmx.c_str());
    std::remove(obj.c_str());
    throw;
  }
}

void report_error_doc(format_doc::Formatter& ppf, const Error& e) {
  namespace fd = format_doc;
  auto qf = [](const std::string& f) { return [f](fd::Formatter& ff) { location::doc::quoted_filename(ff, f); }; };
  using misc::style::code_str;
  switch (e.kind) {
    case Error::Kind::Illegal_renaming:
      fd::fprintf(ppf, "Wrong file naming: %a@ contains the code for@ %a when %a was expected", qf(e.b), code_str(e.a),
                  code_str(e.c));
      break;
    case Error::Kind::Forward_reference:
      fd::fprintf(ppf, "Forward reference to %a in file %a", code_str(e.b), qf(e.a));
      break;
    case Error::Kind::Wrong_for_pack:
      fd::fprintf(ppf, "File %a@ was not compiled with the %a option", qf(e.a), code_str("-for-pack " + e.b));
      break;
    case Error::Kind::File_not_found: fd::fprintf(ppf, "File %a not found", code_str(e.a)); break;
    case Error::Kind::Assembler_error: fd::fprintf(ppf, "Error while assembling %a", code_str(e.a)); break;
    case Error::Kind::Linking_error: fd::fprintf(ppf, "Error during partial linking"); break;
  }
}

}  // namespace cppcaml::typing::asmpackager
