// Port of asmcomp/asmlibrarian.ml; see asmlibrarian.hpp.
#include "cppcaml/typing/asmlibrarian.hpp"

#include <cstdio>
#include <fstream>

#include "cppcaml/typing/asmlink.hpp"
#include "cppcaml/typing/ccomp.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/cmx_format.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/filename.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/persistent_env.hpp"

namespace cppcaml::typing::asmlibrarian {

namespace {
namespace cf = clflags;

[[noreturn]] void fail(Error::Kind k, const std::string& name) {
  Error e{};
  e.kind = k;
  e.name = name;
  throw e;
}

// read_info name: the approximation is not kept in the .cmxa (the linker,
// its only reader, does not need it): write_library_info writes
// default_ui_export_info
std::pair<std::string, std::pair<cmx_format::UnitInfos*, std::string>> read_info(const std::string& name) {
  std::string filename;
  try {
    filename = load_path::find(name);
  } catch (const load_path::NotFound&) {
    fail(Error::Kind::File_not_found, name);
  }
  auto [info, crc] = cmx_format::read_unit_info(filename);
  info->ui_force_link = info->ui_force_link || cf::link_everything;
  return {filename, {info, crc}};
}
}  // namespace

void create_archive(const std::vector<std::string>& file_list, const std::string& lib_name) {
  std::string archive_name = filename::remove_extension(lib_name) + config::ext_lib;
  std::ofstream outchan(lib_name, std::ios::binary);
  try {
    std::vector<std::pair<std::string, std::pair<cmx_format::UnitInfos*, std::string>>> units;
    for (const std::string& f : file_list) units.push_back(read_info(f));
    std::vector<std::string> objfiles;
    for (auto& [filename, _] : units) objfiles.push_back(filename::chop_suffix(filename, ".cmx") + config::ext_obj);
    for (auto& [file_name, u] : units) asmlink::check_consistency(file_name, *u.first, u.second);
    linkdeps::T ldeps(false);
    for (auto it = units.rbegin(); it != units.rend(); ++it) {
      std::vector<std::string> requires_;
      for (auto& [name, _] : it->second.first->ui_imports_cmx) requires_.emplace_back(name);
      std::string unit_name(it->second.first->ui_name);
      ldeps.add(it->first, unit_name, {unit_name}, requires_);
    }
    if (std::optional<linkdeps::Error> e = ldeps.check()) {
      Error err{};
      err.kind = Error::Kind::Link_error;
      err.link = *e;
      throw err;
    }
    cmx_format::LibraryInfos infos;
    for (auto& [_, u] : units) infos.lib_units.push_back(u);
    infos.lib_ccobjs = cf::ccobjs;
    infos.lib_ccopts = cf::all_ccopts;
    std::string bytes = cmx_format::write_library_info(infos);
    outchan << bytes;
    outchan.close();
    if (ccomp::create_archive(archive_name, objfiles) != 0) fail(Error::Kind::Archiver_error, archive_name);
  } catch (...) {
    outchan.close();
    std::remove(lib_name.c_str());
    std::remove(archive_name.c_str());
    throw;
  }
}

void report_error_doc(format_doc::Formatter& ppf, const Error& e) {
  namespace fd = format_doc;
  using misc::style::code_str;
  switch (e.kind) {
    case Error::Kind::File_not_found: fd::fprintf(ppf, "Cannot find file %a", code_str(e.name)); break;
    case Error::Kind::Archiver_error:
      fd::fprintf(ppf, "Error while creating the library %a", code_str(e.name));
      break;
    case Error::Kind::Link_error: linkdeps::report_error_doc(ppf, e.link); break;
  }
}

}  // namespace cppcaml::typing::asmlibrarian
