// Port of asmcomp/asmlink.ml; see asmlink.hpp.
#include "cppcaml/typing/asmlink.hpp"

#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <map>
#include <optional>

#include "cppcaml/omarshal.hpp"
#include "cppcaml/typing/asmgen.hpp"
#include "cppcaml/typing/ccomp.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/cmm_helpers.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/emit.hpp"
#include "cppcaml/typing/filename.hpp"
#include "cppcaml/typing/hashtbl.hpp"
#include "cppcaml/typing/list_sort.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/persistent_env.hpp"

namespace cppcaml::typing::asmlink {

namespace {
namespace o = cppcaml::omarshal;
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

bool ends_with(const std::string& s, const std::string& suff) {
  return s.size() >= suff.size() && s.compare(s.size() - suff.size(), suff.size(), suff) == 0;
}

// Consistency check between interfaces and implementations.  The tables
// keep the first crc checked; interfaces / implementations are the names'
// string objects (from the .cmx files), the latest first.
Consistbl g_crc_interfaces;
std::vector<std::string_view> g_interfaces;
std::map<std::string, std::string, std::less<>> g_crc_interface_objs;
Consistbl g_crc_implementations;
std::vector<std::string_view> g_implementations;
std::map<std::string, std::string, std::less<>> g_crc_implementation_objs;
std::vector<std::string> g_cmx_required;

// Consistbl.extract l tbl: the names sorted (List.sort_uniq's choice among
// equal names is the object the result carries), then consed
cmx_format::Crcs extract(const std::vector<std::string_view>& names,
                         const std::map<std::string, std::string, std::less<>>& crcs) {
  std::vector<std::string_view> l = list_sort::sort_uniq(names, [](std::string_view a, std::string_view b) {
    int c = a.compare(b);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  });
  cmx_format::Crcs assc;
  for (std::string_view name : l) {
    auto it = crcs.find(name);
    assc.insert(assc.begin(), {name, it == crcs.end() ? std::nullopt : std::optional<std::string>(it->second)});
  }
  return assc;
}

// Add C objects and options and "custom" info from a library descriptor
// (OCaml lists, the head first)
std::vector<std::string> g_lib_ccobjs, g_lib_ccopts;

void add_ccobjs(const std::string& origin, const cmx_format::LibraryInfos& l) {
  if (cf::no_auto_link) return;
  std::vector<std::string> objs = l.lib_ccobjs;
  objs.insert(objs.end(), g_lib_ccobjs.begin(), g_lib_ccobjs.end());
  g_lib_ccobjs = std::move(objs);
  std::vector<std::string> opts;
  for (const std::string& s : l.lib_ccopts) opts.push_back(misc::replace_substring("$CAMLORIGIN", origin, s));
  opts.insert(opts.end(), g_lib_ccopts.begin(), g_lib_ccopts.end());
  g_lib_ccopts = std::move(opts);
}

std::vector<std::string> runtime_lib() {
  if (cf::runtime_variant == "_shared") {
    if (config::suffixing) return {misc::shared_runtime_native()};
    return {"-lasmrun_shared"};
  }
  std::string libname = "libasmrun" + cf::runtime_variant + config::ext_lib;
  if (cf::nopervasives || !cf::with_runtime) return {};
  try {
    return {load_path::find(libname)};
  } catch (const load_path::NotFound&) {
    fail(Error::Kind::File_not_found, libname);
  }
}

// First pass: determine which units are needed
struct File {  // Unit of string * unit_infos * crc | Library of string * library_infos
  bool is_library;
  std::string name;
  UnitInfos* info = nullptr;
  std::string crc;
  cmx_format::LibraryInfos lib;
};

std::optional<std::string> object_file_name_of_file(const File& f) {
  if (!f.is_library) return filename::chop_suffix(f.name, ".cmx") + config::ext_obj;
  std::string obj_file = filename::chop_suffix(f.name, ".cmxa") + config::ext_lib;
  // MSVC doesn't support empty .lib files, and macOS struggles to make
  // them (#6550), so there shouldn't be one if the .cmxa contains no units
  if (f.lib.lib_units.empty() && ::access(obj_file.c_str(), F_OK) != 0) return std::nullopt;
  return obj_file;
}

File read_file(const std::string& obj_name) {
  std::string file_name;
  try {
    file_name = load_path::find(obj_name);
  } catch (const load_path::NotFound&) {
    fail(Error::Kind::File_not_found, obj_name);
  }
  File f;
  f.name = file_name;
  if (filename::check_suffix(file_name, ".cmx")) {
    // This is a .cmx file. It must be linked in any case.  Read the infos
    // to see which modules it requires.
    auto [info, crc] = cmx_format::read_unit_info(file_name);
    f.is_library = false;
    f.info = info;
    f.crc = crc;
  } else if (filename::check_suffix(file_name, ".cmxa")) {
    f.is_library = true;
    try {
      f.lib = cmx_format::read_library_info(file_name);
    } catch (const cmx_format::Error& e) {
      if (e.kind == cmx_format::Error::Kind::Not_a_unit_info) fail(Error::Kind::Not_an_object_file, file_name);
      throw;
    }
  } else {
    fail(Error::Kind::Not_an_object_file, file_name);
  }
  return f;
}

struct ToLink {
  UnitInfos* info;
  std::string file_name;
  std::string crc;
};

std::vector<std::string> requires_of(const UnitInfos& ui) {
  std::vector<std::string> r;
  for (auto& [name, _] : ui.ui_imports_cmx) r.emplace_back(name);
  return r;
}

// scan_file ldeps file tolink (List.fold_right: the files last first)
void scan_file(linkdeps::T& ldeps, const File& file, std::vector<ToLink>& tolink) {
  if (!file.is_library) {
    // This is a .cmx file. It must be linked in any case.
    ldeps.add(file.name, std::string(file.info->ui_name), {std::string(file.info->ui_name)}, requires_of(*file.info));
    tolink.insert(tolink.begin(), ToLink{file.info, file.name, file.crc});
    return;
  }
  // This is an archive file. Each unit contained in it will be linked in
  // only if needed.
  add_ccobjs(filename::dirname(file.name), file.lib);
  std::vector<ToLink> reqd;
  for (auto it = file.lib.lib_units.rbegin(); it != file.lib.lib_units.rend(); ++it) {
    UnitInfos* info = it->first;
    if (info->ui_force_link || cf::link_everything || ldeps.required(std::string(info->ui_name))) {
      ldeps.add(file.name, std::string(info->ui_name), {std::string(info->ui_name)}, requires_of(*info));
      reqd.insert(reqd.begin(), ToLink{info, file.name, it->second});
    }
  }
  tolink.insert(tolink.begin(), reqd.begin(), reqd.end());
}

// Second pass: generate the startup file and link it with everything else

void compile_phrase(format::Formatter& dump, const cmm::Phrase& p) { asmgen::compile_phrase(dump, p); }

void force_linking_of_startup(format::Formatter& dump) {
  cmm::Phrase p;
  p.data.push_back(cmm::data_sym(cmm::DataItem::K::Csymbol_address, "caml_startup"));
  compile_phrase(dump, p);
}

// make_globals_map units_list ~crc_interfaces, marshaled
// (Marshal.to_string: the names shared as the .cmx files share them)
std::string globals_map(const std::vector<ToLink>& units_list,
                        const std::vector<std::pair<std::string_view, std::optional<std::string>>>& crc_interfaces) {
  struct Hash {
    long operator()(std::string_view s) const { return hashtbl::hash_string(std::string(s)); }
  };
  using Crco = std::optional<std::string>;
  hashtbl::Hashtbl<std::string_view, Crco, Hash> tbl(16);  // String.Tbl.of_seq
  for (auto& [name, crc] : crc_interfaces) tbl.replace(name, crc);
  std::map<std::pair<const char*, std::size_t>, o::ValPtr> strs;
  auto str = [&](std::string_view s) {
    if (!s.data()) return o::vstr(std::string(s));
    auto [it, fresh] = strs.try_emplace({s.data(), s.size()}, nullptr);
    if (fresh) it->second = o::vstr(std::string(s));
    return it->second;
  };
  auto opt = [](const Crco& c) { return c ? o::vblock(0, {o::vstr(*c)}) : o::vint(0); };
  std::vector<o::ValPtr> defined;
  for (const ToLink& u : units_list) {
    const Crco* intf_crc = tbl.find_opt(u.info->ui_name);
    if (!intf_crc) throw std::out_of_range("Not_found");
    Crco intf = *intf_crc;
    tbl.remove(u.info->ui_name);
    std::vector<o::ValPtr> defines;
    for (std::string_view d : u.info->ui_defines) defines.push_back(str(d));
    defined.push_back(o::vblock(0, {str(u.info->ui_name), opt(intf), opt(u.crc), o::vlist(defines)}));
  }
  // String.Tbl.fold (fun name intf acc -> (name, intf, None, []) :: acc) tbl defined
  std::vector<o::ValPtr> all;
  std::vector<std::pair<std::string_view, Crco>> seq = tbl.to_seq();
  for (auto it = seq.rbegin(); it != seq.rend(); ++it)
    all.push_back(o::vblock(0, {str(it->first), opt(it->second), o::vint(0), o::vint(0)}));
  all.insert(all.end(), defined.begin(), defined.end());
  std::vector<std::uint8_t> bytes = o::marshal(o::vlist(all));
  return std::string(bytes.begin(), bytes.end());
}

std::string make_startup_file(format::Formatter& dump, const std::vector<ToLink>& units_list,
                              const std::vector<std::pair<std::string_view, std::optional<std::string>>>& crc_interfaces) {
  bool need_stdlib = false;
  for (const ToLink& u : units_list) need_stdlib = need_stdlib || u.info->ui_need_stdlib;
  location::input_name = "caml_startup";  // set name of "current" input
  compilenv::reset(std::nullopt, "_startup");  // set the name of the "current" compunit
  bool emit = asmgen::should_emit();
  if (emit) emit::begin_assembly();
  std::vector<std::string_view> name_list;
  for (const ToLink& u : units_list) name_list.insert(name_list.end(), u.info->ui_defines.begin(), u.info->ui_defines.end());
  // (Config.tsan is false: no wrap_tsan)
  for (const cmm::Phrase& p : cmm_helpers::entry_point(name_list)) compile_phrase(dump, p);
  std::vector<const UnitInfos*> units;
  for (const ToLink& u : units_list) units.push_back(u.info);
  for (const cmm::Phrase& p :
       cmm_helpers::emit_preallocated_blocks({}, cmm_helpers::generic_functions(false, units)))
    compile_phrase(dump, p);
  for (std::size_t i = 0; i < cmm_helpers::builtin_exceptions.size(); ++i)
    compile_phrase(dump, cmm_helpers::predef_exception(static_cast<long>(i), cmm_helpers::builtin_exceptions[i]));
  if (need_stdlib) {
    std::string standard_library_default =
        cf::standard_library_default_override ? *cf::standard_library_default_override : config::standard_library_default;
    compile_phrase(dump, cmm_helpers::emit_global_string_constant("caml_standard_library_nat",
                                                                   zstr(standard_library_default)));
  }
  compile_phrase(dump, cmm_helpers::global_table(name_list));
  std::string gmap = globals_map(units_list, crc_interfaces);
  compile_phrase(dump, cmm_helpers::global_data("caml_globals_map", zstr(gmap)));
  std::vector<std::string_view> startup_names{"_startup"};
  startup_names.insert(startup_names.end(), name_list.begin(), name_list.end());
  compile_phrase(dump, cmm_helpers::data_segment_table(startup_names));
  compile_phrase(dump, cmm_helpers::code_segment_table(startup_names));
  std::vector<std::string_view> all_names{"_startup", "_system"};
  all_names.insert(all_names.end(), name_list.begin(), name_list.end());
  compile_phrase(dump, cmm_helpers::frame_table(all_names));
  if (cf::output_complete_object) force_linking_of_startup(dump);
  return emit ? emit::end_assembly() : std::string();
}

// Cmm_helpers.plugin_header units: the marshaled Cmxs_format.dynheader
std::string plugin_header(const std::vector<std::pair<const UnitInfos*, std::string>>& units) {
  std::map<std::pair<const char*, std::size_t>, o::ValPtr> strs;
  auto str = [&](std::string_view s) {
    if (!s.data()) return o::vstr(std::string(s));
    auto [it, fresh] = strs.try_emplace({s.data(), s.size()}, nullptr);
    if (fresh) it->second = o::vstr(std::string(s));
    return it->second;
  };
  auto crcs = [&](const cmx_format::Crcs& l) {
    std::vector<o::ValPtr> v;
    for (auto& [name, crc] : l) v.push_back(o::vblock(0, {str(name), crc ? o::vblock(0, {o::vstr(*crc)}) : o::vint(0)}));
    return o::vlist(v);
  };
  std::vector<o::ValPtr> dynunits;
  for (auto& [ui, crc] : units) {
    std::vector<o::ValPtr> defines;
    for (std::string_view d : ui->ui_defines) defines.push_back(str(d));
    dynunits.push_back(o::vblock(
        0, {str(ui->ui_name), o::vstr(crc), crcs(ui->ui_imports_cmi), crcs(ui->ui_imports_cmx), o::vlist(defines)}));
  }
  o::ValPtr header = o::vblock(0, {o::vstr(*config::config_var("cmxs_magic_number")), o::vlist(dynunits)});
  std::vector<std::uint8_t> bytes = o::marshal(header);
  return std::string(bytes.begin(), bytes.end());
}

std::string make_shared_startup_file(format::Formatter& dump,
                                     const std::vector<std::pair<const UnitInfos*, std::string>>& units) {
  location::input_name = "caml_startup";
  compilenv::reset(std::nullopt, "_shared_startup");
  bool emit = asmgen::should_emit();
  if (emit) emit::begin_assembly();
  std::vector<const UnitInfos*> uis;
  for (auto& [ui, _] : units) uis.push_back(ui);
  for (const cmm::Phrase& p : cmm_helpers::emit_preallocated_blocks({}, cmm_helpers::generic_functions(true, uis)))
    compile_phrase(dump, p);
  compile_phrase(dump, cmm_helpers::global_data("caml_plugin_header", zstr(plugin_header(units))));
  std::vector<std::string_view> symbols;
  for (auto& [ui, _] : units) symbols.push_back(ui->ui_symbol);
  compile_phrase(dump, cmm_helpers::global_table(symbols));
  if (cf::output_complete_object) force_linking_of_startup(dump);
  // this is to force a reference to all units, otherwise the linker might
  // drop some of them (in case of libraries)
  return emit ? emit::end_assembly() : std::string();
}

void call_linker(const std::vector<std::string>& file_list, const std::string& startup_file,
                 const std::string& output_name) {
  bool main_dll = cf::output_c_object && ends_with(output_name, config::ext_dll);
  bool main_obj_runtime = cf::output_complete_object;
  std::vector<std::string> files{startup_file};
  files.insert(files.end(), file_list.rbegin(), file_list.rend());
  std::string ldflags;
  if (!cf::output_c_object || main_dll || main_obj_runtime) {
    files.insert(files.end(), cf::ccobjs.rbegin(), cf::ccobjs.rend());
    std::vector<std::string> rt = runtime_lib();
    files.insert(files.end(), rt.begin(), rt.end());
    ldflags = *config::config_var("native_ldflags") + " " +
              (cf::nopervasives || (main_obj_runtime && !main_dll) ? std::string()
                                                                   : *config::config_var("native_c_libraries"));
  }
  ccomp::LinkMode mode = main_dll                ? ccomp::LinkMode::MainDll
                         : cf::output_c_object ? ccomp::LinkMode::Partial
                                               : ccomp::LinkMode::Exe;
  int exitcode = ccomp::call_linker(mode, output_name, files, ldflags);
  if (exitcode != 0) {
    Error e{};
    e.kind = Error::Kind::Linking_error;
    e.exitcode = exitcode;
    throw e;
  }
}
}  // namespace

void check_consistency(const std::string& file_name, const UnitInfos& unit, const std::string& crc) {
  try {
    for (auto& [name, crco] : unit.ui_imports_cmi) {
      g_interfaces.insert(g_interfaces.begin(), name);
      if (crco) {
        g_crc_interfaces.check(std::string(name), *crco, file_name);
        g_crc_interface_objs.emplace(std::string(name), *crco);  // (Consistbl.check adds the first)
      }
    }
  } catch (const Consistbl::Inconsistency& e) {
    fail(Error::Kind::Inconsistent_interface, e.unit_name, e.inconsistent_source, e.original_source);
  }
  try {
    for (auto& [name, crco] : unit.ui_imports_cmx) {
      g_implementations.insert(g_implementations.begin(), name);
      if (!crco) {
        for (const std::string& r : g_cmx_required)
          if (r == name) fail(Error::Kind::Missing_cmx, file_name, std::string(name));
      } else {
        g_crc_implementations.check(std::string(name), *crco, file_name);
        g_crc_implementation_objs.emplace(std::string(name), *crco);
      }
    }
  } catch (const Consistbl::Inconsistency& e) {
    fail(Error::Kind::Inconsistent_implementation, e.unit_name, e.inconsistent_source, e.original_source);
  }
  g_implementations.insert(g_implementations.begin(), unit.ui_name);
  g_crc_implementations.check(std::string(unit.ui_name), crc, file_name);
  g_crc_implementation_objs.emplace(std::string(unit.ui_name), crc);
  if (unit.ui_symbol != unit.ui_name) g_cmx_required.insert(g_cmx_required.begin(), std::string(unit.ui_name));
}

// Main entry point
void link(std::ostream& ppf_dump, const std::vector<std::string>& objfiles0, const std::string& output_name) {
  std::vector<std::string> objfiles;
  if (cf::nopervasives) {
    objfiles = objfiles0;
  } else if (cf::output_c_object) {
    objfiles.push_back("stdlib.cmxa");
    objfiles.insert(objfiles.end(), objfiles0.begin(), objfiles0.end());
  } else {
    objfiles.push_back("stdlib.cmxa");
    objfiles.insert(objfiles.end(), objfiles0.begin(), objfiles0.end());
    objfiles.push_back("std_exit.cmx");
  }
  std::vector<File> obj_infos;
  for (const std::string& f : objfiles) obj_infos.push_back(read_file(f));
  linkdeps::T ldeps(true);
  std::vector<ToLink> units_tolink;
  for (auto it = obj_infos.rbegin(); it != obj_infos.rend(); ++it) scan_file(ldeps, *it, units_tolink);
  if (std::optional<linkdeps::Error> e = ldeps.check()) {
    Error err{};
    err.kind = Error::Kind::Link_error;
    err.link = *e;
    throw err;
  }
  for (const ToLink& u : units_tolink) check_consistency(u.file_name, *u.info, u.crc);
  cmx_format::Crcs crc_interfaces = extract_crc_interfaces();
  cf::ccobjs.insert(cf::ccobjs.end(), g_lib_ccobjs.begin(), g_lib_ccobjs.end());
  std::vector<std::string> opts = g_lib_ccopts;  // put user's opts first
  opts.insert(opts.end(), cf::all_ccopts.begin(), cf::all_ccopts.end());
  cf::all_ccopts = std::move(opts);
  std::string startup = cf::keep_startup_file ? output_name + ".startup" + ".s" : filename::temp_file("camlstartup", ".s");
  std::string startup_obj = filename::temp_file("camlstartup", config::ext_obj);
  format::Formatter dump;
  try {
    asmgen::compile_unit(startup, cf::keep_startup_file, startup_obj,
                         [&] { return make_startup_file(dump, units_tolink, crc_interfaces); });
  } catch (...) {
    ppf_dump << dump.contents();
    throw;
  }
  ppf_dump << dump.contents();
  ppf_dump.flush();
  std::vector<std::string> objs;
  for (const File& f : obj_infos)
    if (auto o = object_file_name_of_file(f)) objs.push_back(*o);
  try {
    call_linker(objs, startup_obj, output_name);
  } catch (...) {
    std::remove(startup_obj.c_str());
    throw;
  }
  std::remove(startup_obj.c_str());
}

void link_shared(std::ostream& ppf_dump, const std::vector<std::string>& objfiles, const std::string& output_name) {
  std::vector<File> obj_infos;
  for (const std::string& f : objfiles) obj_infos.push_back(read_file(f));
  linkdeps::T ldeps(false);
  std::vector<ToLink> units_tolink;
  for (auto it = obj_infos.rbegin(); it != obj_infos.rend(); ++it) scan_file(ldeps, *it, units_tolink);
  if (std::optional<linkdeps::Error> e = ldeps.check()) {
    Error err{};
    err.kind = Error::Kind::Link_error;
    err.link = *e;
    throw err;
  }
  for (const ToLink& u : units_tolink) check_consistency(u.file_name, *u.info, u.crc);
  cf::ccobjs.insert(cf::ccobjs.end(), g_lib_ccobjs.begin(), g_lib_ccobjs.end());
  std::vector<std::string> opts = g_lib_ccopts;
  opts.insert(opts.end(), cf::all_ccopts.begin(), cf::all_ccopts.end());
  cf::all_ccopts = std::move(opts);
  // List.rev (List.filter_map object_file_name_of_file obj_infos) @ List.rev !Clflags.ccobjs
  std::vector<std::string> objs;
  for (auto it = obj_infos.rbegin(); it != obj_infos.rend(); ++it)
    if (auto o = object_file_name_of_file(*it)) objs.push_back(*o);
  objs.insert(objs.end(), cf::ccobjs.rbegin(), cf::ccobjs.rend());
  std::string startup = cf::keep_startup_file ? output_name + ".startup" + ".s" : filename::temp_file("camlstartup", ".s");
  std::string startup_obj = output_name + ".startup" + config::ext_obj;
  std::vector<std::pair<const UnitInfos*, std::string>> units;
  for (const ToLink& u : units_tolink) units.emplace_back(u.info, u.crc);
  format::Formatter dump;
  try {
    asmgen::compile_unit(startup, cf::keep_startup_file, startup_obj,
                         [&] { return make_shared_startup_file(dump, units); });
  } catch (...) {
    ppf_dump << dump.contents();
    throw;
  }
  ppf_dump << dump.contents();
  ppf_dump.flush();
  std::vector<std::string> files{startup_obj};
  files.insert(files.end(), objs.begin(), objs.end());
  int exitcode = ccomp::call_linker(ccomp::LinkMode::Dll, output_name, files, "");
  if (exitcode != 0) {
    Error e{};
    e.kind = Error::Kind::Linking_error;
    e.exitcode = exitcode;
    throw e;
  }
  std::remove(startup_obj.c_str());
}

cmx_format::Crcs extract_crc_interfaces() { return extract(g_interfaces, g_crc_interface_objs); }
cmx_format::Crcs extract_crc_implementations() { return extract(g_implementations, g_crc_implementation_objs); }

void report_error_doc(format_doc::Formatter& ppf, const Error& e) {
  namespace fd = format_doc;
  auto qf = [](const std::string& f) { return [f](fd::Formatter& ff) { location::doc::quoted_filename(ff, f); }; };
  using misc::style::code_str;
  switch (e.kind) {
    case Error::Kind::File_not_found: fd::fprintf(ppf, "Cannot find file %a", code_str(e.a)); break;
    case Error::Kind::Not_an_object_file:
      fd::fprintf(ppf, "The file %a is not a compilation unit description", qf(e.a));
      break;
    case Error::Kind::Inconsistent_interface:
      fd::fprintf(ppf, "@[<hov>Files %a@ and %a@ make inconsistent assumptions over interface %a@]", qf(e.b), qf(e.c),
                  code_str(e.a));
      break;
    case Error::Kind::Inconsistent_implementation:
      fd::fprintf(ppf, "@[<hov>Files %a@ and %a@ make inconsistent assumptions over implementation %a@]", qf(e.b),
                  qf(e.c), code_str(e.a));
      break;
    case Error::Kind::Assembler_error: fd::fprintf(ppf, "Error while assembling %a", qf(e.a)); break;
    case Error::Kind::Linking_error: fd::fprintf(ppf, "Error during linking (exit code %d)", e.exitcode); break;
    case Error::Kind::Missing_cmx:
      fd::fprintf(ppf,
                  "@[<hov>File %a@ was compiled without access@ to the %a file@ for module %a,@ which was produced "
                  "by %a.@ Please recompile %a@ with the correct %a option@ so that %a@ is found.@]",
                  qf(e.a), code_str(".cmx"), code_str(e.b), code_str("ocamlopt -for-pack"), qf(e.a), code_str("-I"),
                  code_str(e.b + ".cmx"));
      break;
    case Error::Kind::Link_error: linkdeps::report_error_doc(ppf, e.link); break;
  }
}

void reset() {
  g_crc_interfaces.clear();
  g_crc_interface_objs.clear();
  g_crc_implementations.clear();
  g_crc_implementation_objs.clear();
  g_cmx_required.clear();
  g_interfaces.clear();
  g_implementations.clear();
  g_lib_ccobjs.clear();
  g_lib_ccopts.clear();
}

}  // namespace cppcaml::typing::asmlink
