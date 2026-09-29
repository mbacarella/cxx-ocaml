// Port of bytecomp/bytelink.ml; see bytelink.hpp.
#include "cppcaml/typing/bytelink.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>

#include "cppcaml/omarshal.hpp"
#include "cppcaml/typing/arg.hpp"
#include "cppcaml/typing/bytesections.hpp"
#include "cppcaml/typing/ccomp.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/dll.hpp"
#include "cppcaml/typing/filename.hpp"
#include "cppcaml/typing/list_sort.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/persistent_env.hpp"

namespace cppcaml::typing::bytelink {

namespace {
namespace o = cppcaml::omarshal;
namespace cf = clflags;
namespace bs = bytesections;
using cmo_format::CompUnit;
using cmo_format::ObjFile;
using cmo_format::V;

constexpr int opSTOP = 143;  // runtime/caml/instruct.h (Opcodes.opSTOP)

[[noreturn]] void fail(Error::Kind k, const std::string& a = "", const std::string& b = "",
                       const std::string& c = "") {
  Error e{};
  e.kind = k;
  e.a = a;
  e.b = b;
  e.c = c;
  throw e;
}

// type link_action = Link_object of string * compilation_unit
//                  | Link_archive of string * compilation_unit list
struct LinkAction {
  bool archive;
  std::string file_name;
  std::vector<CompUnit> units;  // Link_object: one
};

// lib_ccobjs / lib_ccopts / lib_dllibs: OCaml lists, the head first
std::vector<std::string> g_lib_ccobjs, g_lib_ccopts;
std::vector<std::pair<bool, std::string>> g_lib_dllibs;

std::vector<std::string> strings_of(const V& l) {
  std::vector<std::string> r;
  for (const V& e : cmo_format::list_elems(l)) r.push_back(e->str());
  return r;
}
std::vector<std::pair<bool, std::string>> dllibs_of(const V& l) {
  std::vector<std::pair<bool, std::string>> r;  // (~suffixed:bool * string)
  for (const V& e : cmo_format::list_elems(l)) r.emplace_back(e->fields[0].int_value() != 0, e->fields[1]->str());
  return r;
}
template <class T>
std::vector<T> append(std::vector<T> a, const std::vector<T>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// add_ccobjs obj_name origin l
void add_ccobjs(const std::string& obj_name, const std::string& origin, const V& l) {
  if (cf::no_auto_link) return;
  bool lib_custom = l->fields[cmo_format::lib_custom].int_value() != 0;
  if (cf::use_runtime.empty() && cf::use_prims.empty()) {
    if (lib_custom) cf::custom_runtime = true;
    g_lib_ccobjs = append(strings_of(l->fields[cmo_format::lib_ccobjs]), g_lib_ccobjs);
    std::vector<std::string> opts;
    for (const std::string& s : strings_of(l->fields[cmo_format::lib_ccopts]))
      opts.push_back(misc::replace_substring("$CAMLORIGIN", origin, s));
    g_lib_ccopts = append(opts, g_lib_ccopts);
  } else if (lib_custom) {
    fail(Error::Kind::Needs_custom_runtime, obj_name);
  }
  g_lib_dllibs = append(dllibs_of(l->fields[cmo_format::lib_dllibs]), g_lib_dllibs);
}

// First pass: determine which units are needed

std::vector<std::string> required(const CompUnit& cu) {
  return append(symtable::required_compunits(cu.relocs()), cu.required_compunits());
}

std::vector<std::string> provided(const CompUnit& cu) {
  std::vector<std::string> r;
  for (const cmo_format::Reloc& rel : cu.relocs())
    if (rel.k == cmo_format::Reloc::K::Reloc_setcompunit) r.push_back(rel.name);
  return r;
}

// scan_file ldeps obj_name tolink
void scan_file(linkdeps::T& ldeps, const std::string& obj_name, std::vector<LinkAction>& tolink) {
  std::string file_name;
  try {
    file_name = load_path::find(obj_name);
  } catch (const load_path::NotFound&) {
    fail(Error::Kind::File_not_found, obj_name);
  }
  ObjFile ic(file_name);
  try {
    std::optional<std::string> buffer = ic.read_string(0, static_cast<long>(config::cmo_magic_number.size()));
    if (!buffer) throw cmo_format::EndOfFile{};
    long pos = static_cast<long>(config::cmo_magic_number.size());
    if (*buffer == config::cmo_magic_number) {
      // This is a .cmo file. It must be linked in any case.
      std::optional<long> compunit_pos = ic.read_binary_int(pos);
      if (!compunit_pos) throw cmo_format::EndOfFile{};
      long p = *compunit_pos;
      CompUnit cu{ic.input_value(p)};
      linkdeps_unit(ldeps, obj_name, cu);
      tolink.insert(tolink.begin(), LinkAction{false, file_name, {cu}});
    } else if (*buffer == config::cma_magic_number) {
      // An archive file: each unit in it is linked only if needed.
      std::optional<long> pos_toc = ic.read_binary_int(pos);
      if (!pos_toc) throw cmo_format::EndOfFile{};
      long p = *pos_toc;
      V toc = ic.input_value(p);
      add_ccobjs(obj_name, filename::dirname(file_name), toc);
      std::vector<V> units = cmo_format::list_elems(toc->fields[cmo_format::lib_units]);
      std::vector<CompUnit> reqd;  // List.fold_right
      for (auto it = units.rbegin(); it != units.rend(); ++it) {
        CompUnit cu{*it};
        if (cu.force_link() || cf::link_everything || ldeps.required(cu.name())) {
          linkdeps_unit(ldeps, obj_name, cu);
          reqd.insert(reqd.begin(), cu);
        }
      }
      tolink.insert(tolink.begin(), LinkAction{true, file_name, std::move(reqd)});
    } else {
      fail(Error::Kind::Not_an_object_file, file_name);
    }
  } catch (const cmo_format::EndOfFile&) {
    fail(Error::Kind::Not_an_object_file, file_name);
  }
}

// Second pass: link in the required units

// Consistency check between interfaces
Consistbl g_crc_interfaces;
// interfaces: the names' string objects, the latest first; and each name's
// crc object as the table keeps it (the first one checked)
std::vector<V> g_interfaces;
std::map<std::string, V> g_crc_objs;

// extract_crc_interfaces (): Consistbl.extract !interfaces crc_interfaces --
// List.sort_uniq on the names (its choice among equal names is the object
// the result carries), then consed
std::vector<std::pair<V, V>> extract_crc_interfaces() {
  std::vector<V> l = list_sort::sort_uniq(g_interfaces, [](const V& a, const V& b) {
    int c = a->str().compare(b->str());
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  });
  std::vector<std::pair<V, V>> assc;
  for (const V& name : l) {
    auto it = g_crc_objs.find(name->str());
    assc.insert(assc.begin(), {name, it == g_crc_objs.end() ? nullptr : it->second});
  }
  return assc;
}

void clear_crc_interfaces() {
  g_crc_interfaces.clear();
  g_interfaces.clear();
  g_crc_objs.clear();
}

// Record compilation events: (ofs, evl, debug_dirs) / (ofs, hints), the latest first
struct DebugInfo {
  long ofs;
  V evl, dirs;
};
std::vector<DebugInfo> g_debug_info;
std::vector<std::pair<long, V>> g_hint_info;

using OutputFun = std::function<void(const std::string&)>;
using CurrposFun = std::function<long()>;

// link_compunit accu output_fun currpos_fun inchan file_name compunit
bool link_compunit(bool accu, const OutputFun& output_fun, const CurrposFun& currpos_fun, const ObjFile& inchan,
                   const std::string& file_name, const CompUnit& cu) {
  check_consistency(file_name, cu);
  long cu_pos = cu.int_field(cmo_format::cu_pos), codesize = cu.int_field(cmo_format::cu_codesize);
  std::optional<std::string> code_block = inchan.read_string(cu_pos, codesize);
  if (!code_block) throw std::runtime_error("End_of_file");
  symtable::patch_object(*code_block, cu.relocs());
  long cu_debug = cu.int_field(cmo_format::cu_debug);
  if (cf::debug && cu_debug > 0) {
    long p = cu_debug;
    V debug_event_list = inchan.input_value(p);
    V debug_dirs = inchan.input_value(p);
    std::string file_path = filename::dirname(location::absolute_path(file_name));
    bool mem = false;
    for (const V& d : cmo_format::list_elems(debug_dirs)) mem = mem || d->str() == file_path;
    if (!mem) debug_dirs = o::vblock(0, {o::vstr(file_path), debug_dirs});
    g_debug_info.insert(g_debug_info.begin(), DebugInfo{currpos_fun(), debug_event_list, debug_dirs});
  }
  long cu_hint = cu.int_field(cmo_format::cu_hint);
  if (cf::bytecode_hints && cu_hint > 0) {
    long p = cu_hint;
    V hint_list = inchan.input_value(p);
    g_hint_info.insert(g_hint_info.begin(), {currpos_fun(), hint_list});
  }
  output_fun(*code_block);
  bool needs_stdlib = accu;
  for (const std::string& name : cu.primitives()) {  // fold_primitive
    if (cf::link_everything) symtable::require_primitive(name);
    needs_stdlib = needs_stdlib || name == "%standard_library_default";
  }
  return needs_stdlib;
}

// link_object / link_archive / link_file / link_files
bool link_files(const OutputFun& output_fun, const CurrposFun& currpos_fun, const std::vector<LinkAction>& tolink) {
  bool accu = false;
  for (const LinkAction& a : tolink) {
    ObjFile inchan(a.file_name);
    if (!a.archive) {
      try {
        accu = link_compunit(accu, output_fun, currpos_fun, inchan, a.file_name, a.units.front());
      } catch (const symtable::Error& msg) {
        Error e{};
        e.kind = Error::Kind::Symbol_error;
        e.a = a.file_name;
        e.symbol = msg;
        throw e;
      }
    } else {
      for (const CompUnit& cu : a.units) {
        std::string name = a.file_name + "(" + cu.name() + ")";
        try {
          accu = link_compunit(accu, output_fun, currpos_fun, inchan, name, cu);
        } catch (const symtable::Error& msg) {
          Error e{};
          e.kind = Error::Kind::Symbol_error;
          e.a = name;
          e.symbol = msg;
          throw e;
        }
      }
    }
  }
  return accu;
}

// Output the debugging information:
//   <int32> number of event lists, then per list <int32> offset,
//   <output_value> event list, <output_value> debug directories
void output_debug_info(bs::OutChannel& oc) {
  oc.output_binary_int(static_cast<long>(g_debug_info.size()));
  for (const DebugInfo& d : g_debug_info) {
    oc.output_binary_int(d.ofs);
    oc.output_bytes(o::marshal(d.evl));
    oc.output_bytes(o::marshal(d.dirs));
  }
  g_debug_info.clear();
}

void output_hint_info(bs::OutChannel& oc) {
  oc.output_binary_int(static_cast<long>(g_hint_info.size()));
  for (auto& [ofs, hints] : g_hint_info) {
    oc.output_binary_int(ofs);
    oc.output_bytes(o::marshal(hints));
  }
  g_hint_info.clear();
}

// output_value (extract_crc_interfaces ())
V crcs_value() {
  std::vector<V> l;
  for (auto& [name, crc] : extract_crc_interfaces())
    l.push_back(o::vblock(0, {name, crc ? o::vblock(0, {crc}) : o::vint(0)}));
  return o::vlist(l);
}

// ---- the launcher ----

enum class LaunchMethod { Shebang_bin_sh, Shebang_runtime, Executable };

// invalid_for_shebang_line path
bool invalid_for_shebang_line(const std::string& path) {
  if (path.size() > 125) return true;
  for (char c : path)
    if (c == ' ' || c == '\t' || c == '\n') return true;
  return false;
}

std::string find_bin_sh() {
  std::string output_file = filename::temp_file("caml_bin_sh", "");
  std::string result;
  auto run = [&](const std::string& command, const std::vector<std::string>& args) {
    std::string cmd = filename::quote_command(command, args, std::nullopt, output_file);
    if (cf::verbose) {
      std::cout.flush();
      std::cerr << "+ " << cmd << "\n";
    }
    std::cout.flush();
    std::cerr.flush();
    int status = std::system(cmd.c_str());
    return status == 0;
  };
  // (command -p -v is Posix Issue 7, which Solaris does not support)
  if (run("command", {"-p", "-v", "sh"}) || run("sh", {"-c", "PATH=\"`getconf PATH`\" command -v sh"})) {
    std::ifstream ic(output_file);
    if (!std::getline(ic, result)) result = "";  // End_of_file
  }
  misc::remove_file(output_file);
  return result;
}

// String.trim: ' ', '\012', '\n', '\r', '\t'
std::string trim(const std::string& s) {
  auto sp = [](char c) { return c == ' ' || c == '\012' || c == '\n' || c == '\r' || c == '\t'; };
  std::size_t b = 0, e = s.size();
  while (b < e && sp(s[b])) ++b;
  while (e > b && sp(s[e - 1])) --e;
  return s.substr(b, e - b);
}

// write_sh_launcher outchan bin_sh bindir search runtime
void write_sh_launcher(bs::OutChannel& outchan, const std::string& bin_sh, const std::string& bindir,
                       config::SearchMethod search, const std::string& runtime0) {
  enum Tag { DFE, F, FE };
  auto l = [&](Tag tag, const std::string& s) {
    bool out = tag == DFE || (tag == F && search == config::SearchMethod::Fallback) ||
               (tag == FE && (search == config::SearchMethod::Fallback || search == config::SearchMethod::Enable));
    if (out) {
      outchan.output_string(trim(s));
      outchan.output_char('\n');
    }
  };
  std::string runtime = filename::quote(runtime0);
  std::string bin = filename::quote(filename::concat(bindir, ""));
  std::string exec = search == config::SearchMethod::Disable ? runtime : "\"$c\"";
  std::string release = std::to_string(config::ocaml_release_major) + "." + std::to_string(config::ocaml_release_minor);
  l(DFE, "#!" + bin_sh);
  l(FE, "r=" + runtime);
  l(F, "c=" + bin + "\"$r\"");
  l(F, "if ! test -f \"$c\"; then");
  l(FE, "  d=\"$(dirname \"$0\" 2>/dev/null)\"");
  l(FE, "  test -z \"$d\" || d=\"${d%/}/\"");
  l(FE, "  c=\"$(command -v \"$d$r\")\"");
  l(FE, "  test -n \"$c\" || c=\"$(command -v \"$r\")\"");
  l(F, "fi");
  l(FE, "if test -z \"$c\"; then");
  l(FE, "  echo 'This program requires an OCaml " + release + " interpreter'>&2");
  l(FE, "  echo \"$r not found either alongside $0 or in \\$PATH\">&2");
  l(FE, "else");
  l(DFE, "  exec " + exec + " \"$0\" \"$@\"");
  l(FE, "fi");
  l(FE, "exit 126");
}

// write_header outchan: the executable header, and the RNTM section when
// needed; returns the toc_writer
bs::TocWriter write_header(bs::OutChannel& outchan) {
  const std::string header = "runtime-launch-info";
  std::string header_path;
  try {
    header_path = load_path::find(header);
  } catch (const load_path::NotFound&) {
    fail(Error::Kind::File_not_found, header);
  }
  std::string data;
  {
    std::ifstream in(header_path, std::ios::binary);
    if (!in) fail(Error::Kind::Camlheader, header_path + ": " + std::strerror(errno), header_path);
    std::ostringstream ss;
    ss << in.rdbuf();
    data = ss.str();
  }
  std::optional<misc::RuntimeID> zinc_runtime_id;
  std::size_t offset;
  if (data.size() < 2) {
    fail(Error::Kind::Camlheader, "corrupt header", header_path);
  } else if (data[0] == '\0') {
    offset = 1;
  } else {
    std::optional<misc::RuntimeID> zinc = misc::RuntimeID::of_string(data.substr(0, 4));
    if (!zinc || !zinc->is_zinc()) fail(Error::Kind::Camlheader, "corrupt header", header_path);
    zinc_runtime_id = zinc;
    offset = 4;
  }
  std::string runtime;
  config::SearchMethod search;
  if (!cf::use_runtime.empty()) {
    // Do not use BUILD_PATH_PREFIX_MAP mapping for this.
    runtime = cf::use_runtime;
    if (filename::is_relative(runtime)) runtime = filename::concat(std::filesystem::current_path().string(), runtime);
    search = config::SearchMethod::Disable;
  } else {
    runtime = zinc_runtime_id ? zinc_runtime_id->ocamlrun(cf::runtime_variant) : "ocamlrun" + cf::runtime_variant;
    search = cf::search_method.value_or(config::search_method());
    if (search == config::SearchMethod::Disable)
      runtime = filename::concat(cf::target_bindir.value_or(config::target_bindir()), runtime);
  }
  // Determine which method will be used for launching the executable
  LaunchMethod launcher;
  std::string bin_sh;
  config::LaunchMethod lm = cf::launch_method.value_or(config::launch_method());
  if (lm.k == config::LaunchMethod::K::Executable) {
    launcher = LaunchMethod::Executable;
  } else if (search != config::SearchMethod::Disable || invalid_for_shebang_line(runtime)) {
    std::string sh = lm.sh ? *lm.sh : find_bin_sh();
    if (sh.empty() || invalid_for_shebang_line(sh)) {
      launcher = LaunchMethod::Executable;
    } else {
      launcher = LaunchMethod::Shebang_bin_sh;
      bin_sh = sh;
    }
  } else {
    launcher = LaunchMethod::Shebang_runtime;
  }
  switch (launcher) {
    case LaunchMethod::Shebang_runtime:
      outchan.output_string("#!" + runtime + "\n");  // Use the runtime directly
      return bs::init_record(outchan);
    case LaunchMethod::Shebang_bin_sh:
      write_sh_launcher(outchan, bin_sh, config::resolved_bindir(), search, runtime);
      return bs::init_record(outchan);
    case LaunchMethod::Executable: {
      // Use the executable stub launcher
      outchan.output_string(data.substr(offset));
      // The runtime name needs recording in RNTM
      bs::TocWriter toc_writer = bs::init_record(outchan);
      if (search == config::SearchMethod::Disable) {
        outchan.output_string(runtime);
      } else {
        if (search == config::SearchMethod::Fallback)
          // Ensure bindir does _not_ end up with a separator
          outchan.output_string(filename::dirname(filename::concat(config::resolved_bindir(), filename::current_dir_name)));
        outchan.output_char('\0');
        outchan.output_string(runtime);
      }
      bs::record(toc_writer, bs::Name::RNTM);
      return toc_writer;
    }
  }
  return bs::init_record(outchan);
}

// Misc.try_finally ~exceptionally: the output file removed on failure
struct RemoveOnFailure {
  std::string file;
  bool armed = true;
  ~RemoveOnFailure() {
    if (armed) misc::remove_file(file);
  }
};

void write_file(const std::string& path, const std::string& data, int perm) {
  int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CREAT, perm);
  if (fd < 0) throw arg::SysError(path + ": " + std::strerror(errno));
  std::size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n <= 0) {
      ::close(fd);
      throw arg::SysError(path + ": " + std::strerror(errno));
    }
    off += static_cast<std::size_t>(n);
  }
  ::close(fd);
}

// link_bytecode ?final_name tolink exec_name standalone
void link_bytecode(const std::vector<LinkAction>& tolink, const std::string& exec_name, bool standalone) {
  // Avoid the case where the specified exec output file is the same as
  // one of the objects to be linked
  for (const LinkAction& a : tolink)
    if (!a.archive && a.file_name == exec_name) fail(Error::Kind::Wrong_object_name, exec_name);
  // Remove the output file if it exists (PR#8354), but not a special file (PR#11302)
  misc::remove_file(exec_name);
  int outperm = cf::with_runtime ? 0777 : 0666;
  write_file(exec_name, "", outperm);  // open_out_gen [Open_wronly; Open_trunc; Open_creat; Open_binary]
  RemoveOnFailure guard{exec_name};
  bs::OutChannel outchan;
  // Write the header and set the path to the bytecode interpreter
  bs::TocWriter toc_writer =
      standalone && cf::with_runtime ? write_header(outchan) : bs::init_record(outchan);
  // The bytecode
  long start_code = outchan.pos();
  symtable::init();
  clear_crc_interfaces();
  std::vector<std::string> tocheck;
  std::vector<std::pair<bool, std::string>> sharedobjs;
  for (auto it = cf::dllibs.rbegin(); it != cf::dllibs.rend(); ++it) {  // List.fold_right process_dllib
    const auto& [suffixed, name] = *it;
    std::string resolved_name = dll::extract_dll_name(*it);
    std::pair<bool, std::string> partial_name;
    if (suffixed)
      partial_name = name.rfind("-l", 0) == 0 ? std::make_pair(true, "dll" + name.substr(2)) : *it;
    else
      partial_name = {false, resolved_name};
    tocheck.insert(tocheck.begin(), resolved_name);
    sharedobjs.insert(sharedobjs.begin(), partial_name);
  }
  bool check_dlls = standalone && config::target == config::host;
  if (check_dlls) {
    // Initialize the DLL machinery
    dll::init_compile(cf::no_std_include);
    dll::add_path(load_path::get_path_list());
    try {
      dll::open_dlls_for_checking(tocheck);
    } catch (const dll::Failure& reason) {
      fail(Error::Kind::Cannot_open_dll, reason.what());
    }
  }
  OutputFun output_fun = [&](const std::string& buf) { outchan.output_string(buf); };
  CurrposFun currpos_fun = [&] { return outchan.pos() - start_code; };
  bool needs_stdlib = link_files(output_fun, currpos_fun, tolink);
  if (check_dlls) dll::close_all_dlls();
  // The final STOP instruction
  outchan.output_byte(opSTOP);
  outchan.output_byte(0);
  outchan.output_byte(0);
  outchan.output_byte(0);
  bs::record(toc_writer, bs::Name::CODE);
  // DLL stuff
  if (standalone) {
    // The extra search path for DLLs
    if (!cf::dllpaths.empty()) {
      outchan.output_string(misc::concat_null_terminated(cf::dllpaths));
      bs::record(toc_writer, bs::Name::DLPT);
    }
    // The names of the DLLs
    if (!sharedobjs.empty()) {
      for (const auto& [suffixed, name] : sharedobjs) {
        outchan.output_char(suffixed ? '-' : ':');
        outchan.output_string(name);
        outchan.output_byte(0);
      }
      bs::record(toc_writer, bs::Name::DLLS);
    }
  }
  // The names of all primitives
  outchan.output_string(symtable::primitive_names());
  bs::record(toc_writer, bs::Name::PRIM);
  // The table of global data (Emitcode.marshal_to_channel_with_possibly_32bit_compat)
  outchan.output_bytes(o::marshal(symtable::initial_global_table()));
  bs::record(toc_writer, bs::Name::DATA);
  // -custom executables don't need OSLD sections
  if (standalone && needs_stdlib) {
    // OCaml Standard Library Default location
    outchan.output_string(cf::standard_library_default_override.value_or(config::standard_library_default));
    bs::record(toc_writer, bs::Name::OSLD);
  }
  // The map of global identifiers
  outchan.output_bytes(o::marshal(symtable::data_global_map()));
  bs::record(toc_writer, bs::Name::SYMB);
  // CRCs for modules
  outchan.output_bytes(o::marshal(crcs_value()));
  bs::record(toc_writer, bs::Name::CRCS);
  // Debug info
  if (cf::debug) {
    output_debug_info(outchan);
    bs::record(toc_writer, bs::Name::DBUG);
  }
  if (cf::bytecode_hints) {
    output_hint_info(outchan);
    bs::record(toc_writer, bs::Name::HINT);
  }
  // The table of contents and the trailer
  bs::write_toc_and_trailer(toc_writer);
  write_file(exec_name, outchan.buf, outperm);
  guard.armed = false;
}

// ---- output as C ----

long g_output_code_string_counter = 0;

// Output a string as a C array of unsigned ints
void output_code_string(std::string& out, const std::string& code) {
  char b[32];
  for (std::size_t pos = 0; pos < code.size(); pos += 4) {
    auto c = [&](std::size_t k) { return static_cast<unsigned>(static_cast<unsigned char>(code[pos + k])); };
    std::snprintf(b, sizeof b, "0x%02x%02x%02x%02x, ", c(3), c(2), c(1), c(0));
    out += b;
    if (++g_output_code_string_counter >= 6) {
      out += '\n';
      g_output_code_string_counter = 0;
    }
  }
}

// Output a string as a C string
void output_data_string(std::string& out, const std::string& data) {
  int counter = 0;
  for (unsigned char ch : data) {
    out += std::to_string(static_cast<int>(ch)) + ", ";
    if (++counter >= 12) {
      out += "\n";
      counter = 0;
    }
  }
}

// Output a debug stub
void output_cds_file(const std::string& outfile) {
  misc::remove_file(outfile);
  write_file(outfile, "", 0777);
  RemoveOnFailure guard{outfile};
  bs::OutChannel outchan;
  bs::TocWriter toc_writer = bs::init_record(outchan);
  // The map of global identifiers
  outchan.output_bytes(o::marshal(symtable::data_global_map()));
  bs::record(toc_writer, bs::Name::SYMB);
  // Debug info
  output_debug_info(outchan);
  bs::record(toc_writer, bs::Name::DBUG);
  bs::write_toc_and_trailer(toc_writer);
  write_file(outfile, outchan.buf, 0777);
  guard.armed = false;
}

// String.to_utf_8_seq: the characters of s, an invalid sequence decoded as
// U+FFFD (String.get_utf_8_uchar's maximal invalid subpart)
std::vector<std::pair<std::uint32_t, std::string>> utf_8_seq(const std::string& s) {
  std::vector<std::pair<std::uint32_t, std::string>> r;
  const std::string rep = "\xEF\xBF\xBD";
  std::size_t i = 0, n = s.size();
  auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
  auto cont = [&](std::size_t k, unsigned lo = 0x80, unsigned hi = 0xBF) {
    return k < n && byte(k) >= lo && byte(k) <= hi;
  };
  while (i < n) {
    unsigned c = byte(i);
    if (c < 0x80) {
      r.emplace_back(c, s.substr(i, 1));
      ++i;
      continue;
    }
    std::size_t len = 0;
    unsigned lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      if (c == 0xE0) lo = 0xA0;
      if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      if (c == 0xF0) lo = 0x90;
      if (c == 0xF4) hi = 0x8F;
    }
    if (len == 0) {
      r.emplace_back(0xFFFD, rep);
      ++i;
      continue;
    }
    std::size_t k = 1;
    if (!cont(i + 1, lo, hi)) {
      r.emplace_back(0xFFFD, rep);
      ++i;
      continue;
    }
    for (k = 2; k < len && cont(i + k); ++k) {
    }
    if (k < len) {
      r.emplace_back(0xFFFD, rep);
      i += k;
      continue;
    }
    std::uint32_t u = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
    for (std::size_t j = 1; j < len; ++j) u = (u << 6) | (byte(i + j) & 0x3F);
    r.emplace_back(u, s.substr(i, len));
    i += len;
  }
  return r;
}

// c_string_literal_of_string s (not target_win32: UTF-8 kept, C escapes)
std::string c_string_literal_of_string(const std::string& s) {
  std::string b;
  b += '"';
  for (auto& [u, bytes] : utf_8_seq(s)) {
    switch (u) {
      case 0: b += "\\000"; break;
      case 9: b += "\\t"; break;
      case 10: b += "\\n"; break;
      case 13: b += "\\r"; break;
      case 34: b += "\\\""; break;
      case 92: b += "\\\\"; break;
      default: b += bytes; break;
    }
  }
  b += '"';
  return b;
}

std::string emit_runtime_standard_library_default() {
  std::string stdlib = cf::standard_library_default_override.value_or(config::standard_library_default);
  return "const char_os * caml_runtime_standard_library_default = " + c_string_literal_of_string(stdlib) + ";\n";
}

// Output a bytecode executable as a C file
void link_bytecode_as_c(const std::vector<LinkAction>& tolink, const std::string& outfile, bool with_main) {
  RemoveOnFailure guard{outfile};
  std::string out;
  // The bytecode
  out +=
      "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n#define CAML_INTERNALS\n#define CAMLDLLIMPORT\n"
      "#define CAML_INTERNALS_NO_PRIM_DECLARATIONS\n\n#include <caml/mlvalues.h>\n#include <caml/startup.h>\n"
      "#include <caml/sys.h>\n#include <caml/misc.h>\n\n"
      "const enum caml_byte_program_mode caml_byte_program_mode = EMBEDDED;\n\nstatic int caml_code[] = {\n";
  symtable::init();
  clear_crc_interfaces();
  long currpos = 0;
  OutputFun output_fun = [&](const std::string& code) {
    output_code_string(out, code);
    currpos += static_cast<long>(code.size());
  };
  CurrposFun currpos_fun = [&] { return currpos; };
  (void)link_files(output_fun, currpos_fun, tolink);
  // The final STOP instruction
  char stop[32];
  std::snprintf(stop, sizeof stop, "\n0x%x};\n", opSTOP);
  out += stop;
  // The table of global data
  out += "\nstatic char caml_data[] = {\n";
  std::vector<std::uint8_t> data = o::marshal(symtable::initial_global_table());
  output_data_string(out, std::string(data.begin(), data.end()));
  out += "\n};\n";
  // The sections: [| "SYMB", data_global_map (); "CRCS", extract_crc_interfaces () |]
  V sections = o::vblock(0, {o::vblock(0, {o::vstr("SYMB"), symtable::data_global_map()}),
                             o::vblock(0, {o::vstr("CRCS"), crcs_value()})});
  out += "\nstatic char caml_sections[] = {\n";
  std::vector<std::uint8_t> sect = o::marshal(sections);
  output_data_string(out, std::string(sect.begin(), sect.end()));
  out += "\n};\n\n";
  out += emit_runtime_standard_library_default();
  // The table of primitives
  out += symtable::primitive_table();
  // The entry point
  if (with_main) {
    out +=
        "\nint main_os(int argc, char_os **argv)\n{\n"
        "  caml_startup_code(caml_code, sizeof(caml_code),\n"
        "                    caml_data, sizeof(caml_data),\n"
        "                    caml_sections, sizeof(caml_sections),\n"
        "                    /* pooling */ 0,\n"
        "                    argv);\n"
        "  caml_do_exit(0);\n"
        "  return 0; /* not reached */\n}\n";
  } else {
    out +=
        "\nvoid caml_startup(char_os ** argv)\n{\n"
        "  caml_startup_code(caml_code, sizeof(caml_code),\n"
        "                    caml_data, sizeof(caml_data),\n"
        "                    caml_sections, sizeof(caml_sections),\n"
        "                    /* pooling */ 0,\n"
        "                    argv);\n}\n\n"
        "value caml_startup_exn(char_os ** argv)\n{\n"
        "  return caml_startup_code_exn(caml_code, sizeof(caml_code),\n"
        "                               caml_data, sizeof(caml_data),\n"
        "                               caml_sections, sizeof(caml_sections),\n"
        "                               /* pooling */ 0,\n"
        "                               argv);\n}\n\n"
        "void caml_startup_pooled(char_os ** argv)\n{\n"
        "  caml_startup_code(caml_code, sizeof(caml_code),\n"
        "                    caml_data, sizeof(caml_data),\n"
        "                    caml_sections, sizeof(caml_sections),\n"
        "                    /* pooling */ 1,\n"
        "                    argv);\n}\n\n"
        "value caml_startup_pooled_exn(char_os ** argv)\n{\n"
        "  return caml_startup_code_exn(caml_code, sizeof(caml_code),\n"
        "                               caml_data, sizeof(caml_data),\n"
        "                               caml_sections, sizeof(caml_sections),\n"
        "                               /* pooling */ 1,\n"
        "                               argv);\n}\n";
  }
  out += "\n#ifdef __cplusplus\n}\n#endif\n";
  write_file(outfile, out, 0666);  // open_out
  guard.armed = false;
  if (!with_main && cf::debug) output_cds_file(filename::chop_extension(outfile) + ".cds");
}

// runtime_library_name runtime_variant
std::string runtime_library_name(const std::string& runtime_variant) {
  if (runtime_variant == "_shared" && config::suffixing) return misc::shared_runtime_bytecode();
  return "-lcamlrun" + runtime_variant;
}

// List.rev !Clflags.ccobjs
std::vector<std::string> rev_ccobjs() { return std::vector<std::string>(cf::ccobjs.rbegin(), cf::ccobjs.rend()); }

// Build a custom runtime
bool build_custom_runtime(const std::string& prim_name, const std::string& exec_name) {
  std::string runtime_lib = cf::with_runtime ? runtime_library_name(cf::runtime_variant) : "";
  std::optional<std::string> stable_name;
  if (!cf::keep_camlprimc_file) stable_name = "camlprim.c";
  std::string prims_obj = filename::temp_file("camlprim", config::ext_obj);
  std::vector<std::string> files{prims_obj};
  std::vector<std::string> rc = rev_ccobjs();
  files.insert(files.end(), rc.begin(), rc.end());
  files.push_back(runtime_lib);
  bool result = ccomp::compile_file(prim_name, prims_obj, "", stable_name) == 0 &&
                ccomp::call_linker(ccomp::LinkMode::Exe, exec_name, files,
                                   cf::std_include_flag("-I") + " " + config::bytecomp_c_libraries) == 0;
  misc::remove_file(prims_obj);
  return result;
}

void append_bytecode(const std::string& bytecode_name, const std::string& exec_name) {
  std::ifstream ic(bytecode_name, std::ios::binary);
  std::ofstream oc(exec_name, std::ios::binary | std::ios::app);
  if (!ic || !oc) throw arg::SysError(exec_name + ": " + std::strerror(errno));
  oc << ic.rdbuf();
}

// fix_exec_name: the name the C compiler gives (Unix: unchanged)
std::string fix_exec_name(const std::string& name) { return name; }
}  // namespace

void check_consistency(const std::string& file_name, const CompUnit& cu) {
  try {
    for (auto& [name, crco] : cu.import_objs()) {
      g_interfaces.insert(g_interfaces.begin(), name);
      if (crco) {
        g_crc_interfaces.check(name->str(), crco->str(), file_name);
        g_crc_objs.emplace(name->str(), crco);  // (Consistbl.check adds the first)
      }
    }
  } catch (const Consistbl::Inconsistency& e) {
    fail(Error::Kind::Inconsistent_import, e.unit_name, e.inconsistent_source, e.original_source);
  }
}

void linkdeps_unit(linkdeps::T& ldeps, const std::string& filename, const CompUnit& cu) {
  std::vector<std::string> requires_ = required(cu);  // (contains pack submodules)
  std::vector<std::string> provides = provided(cu);
  ldeps.add(filename, cu.name(), provides, requires_);
}

void link(const std::vector<std::string>& objfiles0, const std::string& output_name) {
  std::vector<std::string> objfiles;
  if (cf::nopervasives) {
    objfiles = objfiles0;
  } else if (cf::output_c_object && !cf::output_complete_executable) {
    objfiles.push_back("stdlib.cma");
    objfiles.insert(objfiles.end(), objfiles0.begin(), objfiles0.end());
  } else {
    objfiles.push_back("stdlib.cma");
    objfiles.insert(objfiles.end(), objfiles0.begin(), objfiles0.end());
    objfiles.push_back("std_exit.cmo");
  }
  linkdeps::T ldeps(true);
  std::vector<LinkAction> tolink;  // List.fold_right (scan_file ldeps) objfiles []
  for (auto it = objfiles.rbegin(); it != objfiles.rend(); ++it) scan_file(ldeps, *it, tolink);
  if (std::optional<linkdeps::Error> e = ldeps.check()) {
    Error err{};
    err.kind = Error::Kind::Link_error;
    err.link = *e;
    throw err;
  }
  cf::ccobjs = append(cf::ccobjs, g_lib_ccobjs);           // put user's libs last
  cf::all_ccopts = append(g_lib_ccopts, cf::all_ccopts);   // put user's opts first
  cf::dllibs = append(g_lib_dllibs, cf::dllibs);           // put user's DLLs first
  if (!cf::custom_runtime) {
    link_bytecode(tolink, output_name, true);
  } else if (!cf::output_c_object) {
    std::string bytecode_name = filename::temp_file("camlcode", "");
    std::string prim_name =
        cf::keep_camlprimc_file ? output_name + ".camlprim.c" : filename::temp_file("camlprim", ".c");
    struct Always {
      std::string bytecode_name, prim_name;
      ~Always() {
        misc::remove_file(bytecode_name);
        if (!cf::keep_camlprimc_file) misc::remove_file(prim_name);
      }
    } always{bytecode_name, prim_name};
    link_bytecode(tolink, bytecode_name, false);
    // (builds will not be reproducible if the C code contains macros such as __FILE__)
    std::string poc =
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n#define CAML_INTERNALS\n"
        "#define CAML_INTERNALS_NO_PRIM_DECLARATIONS\n\n#include <caml/mlvalues.h>\n#include <caml/startup.h>\n\n"
        "const enum caml_byte_program_mode caml_byte_program_mode = APPENDED;\n\n";
    poc += symtable::primitive_table();
    poc += emit_runtime_standard_library_default();
    poc += "\n#ifdef __cplusplus\n}\n#endif\n";
    write_file(prim_name, poc, 0666);
    std::string exec_name = fix_exec_name(output_name);
    if (!build_custom_runtime(prim_name, exec_name)) fail(Error::Kind::Custom_runtime);
    if (!cf::make_runtime) append_bytecode(bytecode_name, exec_name);
  } else {
    std::string basename = filename::remove_extension(output_name);
    std::string c_file;
    std::optional<std::string> stable_name;
    if (cf::output_complete_object && !filename::check_suffix(output_name, ".c")) {
      c_file = filename::temp_file("camlobj", ".c");
      stable_name = "camlobj.c";
    } else {
      c_file = basename + ".c";
      std::error_code ec;
      if (std::filesystem::exists(c_file, ec)) fail(Error::Kind::File_exists, c_file);
    }
    std::string obj_file =
        cf::output_complete_object ? filename::chop_extension(c_file) + config::ext_obj : basename + config::ext_obj;
    struct Temps {
      std::vector<std::string> l;
      ~Temps() {
        for (const std::string& f : l) misc::remove_file(f);
      }
    } temps;
    link_bytecode_as_c(tolink, c_file, cf::output_complete_executable);
    if (cf::output_complete_executable) {
      temps.l.insert(temps.l.begin(), c_file);
      if (!build_custom_runtime(c_file, output_name)) fail(Error::Kind::Custom_runtime);
    } else if (!filename::check_suffix(output_name, ".c")) {
      temps.l.insert(temps.l.begin(), c_file);
      if (ccomp::compile_file(c_file, obj_file, "", stable_name) != 0) fail(Error::Kind::Custom_runtime);
      if (!filename::check_suffix(output_name, config::ext_obj) || cf::output_complete_object) {
        temps.l.insert(temps.l.begin(), obj_file);
        bool partial = filename::check_suffix(output_name, config::ext_obj);
        std::string runtime_lib = cf::with_runtime ? runtime_library_name(cf::runtime_variant) : "";
        std::vector<std::string> files{obj_file};
        std::vector<std::string> rc = rev_ccobjs();
        files.insert(files.end(), rc.begin(), rc.end());
        files.push_back(runtime_lib);
        if (ccomp::call_linker(partial ? ccomp::LinkMode::Partial : ccomp::LinkMode::MainDll, output_name, files,
                               partial ? "" : config::bytecomp_c_libraries) != 0)
          fail(Error::Kind::Custom_runtime);
      }
    }
  }
}

void report_error_doc(format_doc::Formatter& ppf, const Error& e) {
  namespace fd = format_doc;
  auto qf = [](const std::string& f) { return [f](fd::Formatter& ff) { location::doc::quoted_filename(ff, f); }; };
  using misc::style::code_str;
  switch (e.kind) {
    case Error::Kind::File_not_found: fd::fprintf(ppf, "Cannot find file %a", qf(e.a)); break;
    case Error::Kind::Not_an_object_file:
      fd::fprintf(ppf, "The file %a is not a bytecode object file", qf(e.a));
      break;
    case Error::Kind::Wrong_object_name:
      fd::fprintf(ppf,
                  "The output file %a has the wrong name. The extension implies an object file but the link step "
                  "was requested",
                  code_str(e.a));
      break;
    case Error::Kind::Symbol_error:
      fd::fprintf(ppf, "Error while linking %a:@ %a", qf(e.a),
                  [&](fd::Formatter& f) { symtable::report_error_doc(f, e.symbol); });
      break;
    case Error::Kind::Inconsistent_import:
      fd::fprintf(ppf, "@[<hov>Files %a@ and %a@ make inconsistent assumptions over interface %a@]", qf(e.b), qf(e.c),
                  code_str(e.a));
      break;
    case Error::Kind::Custom_runtime: fd::fprintf(ppf, "Error while building custom runtime system"); break;
    case Error::Kind::File_exists: fd::fprintf(ppf, "Cannot overwrite existing file %a", qf(e.a)); break;
    case Error::Kind::Cannot_open_dll:
      fd::fprintf(ppf, "Error on dynamically loaded library: %a",
                  [&](fd::Formatter& f) { location::doc::filename(f, e.a); });
      break;
    case Error::Kind::Camlheader:
      fd::fprintf(ppf, "System error while copying file %a: %a", code_str(e.b), code_str(e.a));
      break;
    case Error::Kind::Link_error: linkdeps::report_error_doc(ppf, e.link); break;
    case Error::Kind::Needs_custom_runtime:
      fd::fprintf(ppf,
                  "%s links with C code, so cannot be linked with -use-prims or -use-runtime unless -noautolink is "
                  "specified",
                  e.a);
      break;
  }
}

void reset() {
  g_lib_ccobjs.clear();
  g_lib_ccopts.clear();
  g_lib_dllibs.clear();
  g_crc_interfaces.clear();
  g_debug_info.clear();
  g_hint_info.clear();
  g_output_code_string_counter = 0;
}

}  // namespace cppcaml::typing::bytelink
