// Port of bytecomp/bytelibrarian.ml; see bytelibrarian.hpp.
#include "cppcaml/typing/bytelibrarian.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "cppcaml/omarshal.hpp"
#include "cppcaml/typing/arg.hpp"
#include "cppcaml/typing/bytelink.hpp"
#include "cppcaml/typing/bytesections.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/cmo_format.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/persistent_env.hpp"

namespace cppcaml::typing::bytelibrarian {

namespace {
namespace o = cppcaml::omarshal;
namespace cf = clflags;
using cmo_format::CompUnit;
using cmo_format::ObjFile;
using cmo_format::V;

[[noreturn]] void fail(Error::Kind k, const std::string& name) {
  Error e{};
  e.kind = k;
  e.name = name;
  throw e;
}

// Copy a compilation unit from a .cmo or .cma into the archive
void copy_compunit(const ObjFile& ic, bytesections::OutChannel& oc, CompUnit& cu) {
  long pos = cu.int_field(cmo_format::cu_pos);
  cu.set_int_field(cmo_format::cu_pos, oc.pos());
  cu.set_int_field(cmo_format::cu_force_link, cu.force_link() || cf::link_everything ? 1 : 0);
  // copy_file_chunk ic oc cu_codesize (End_of_file when short)
  auto copy = [&](long from, long len) {
    if (from < 0 || len < 0 || from + len > ic.size()) throw cmo_format::EndOfFile{};
    oc.output_data(ic.data() + from, static_cast<std::size_t>(len));
    ic.done_with(from, len);
  };
  copy(pos, cu.int_field(cmo_format::cu_codesize));
  long debug = cu.int_field(cmo_format::cu_debug);
  if (debug > 0) {
    cu.set_int_field(cmo_format::cu_debug, oc.pos());
    copy(debug, cu.int_field(cmo_format::cu_debugsize));
  }
}

// Add C objects and options and "custom" info from a library descriptor
// (scanned left to right: appended)
std::vector<V> g_lib_ccobjs, g_lib_ccopts, g_lib_dllibs;

void add_ccobjs(const V& l) {
  if (cf::no_auto_link) return;
  if (l->fields[cmo_format::lib_custom].int_value() != 0) cf::custom_runtime = true;
  for (const V& x : cmo_format::list_elems(l->fields[cmo_format::lib_ccobjs])) g_lib_ccobjs.push_back(x);
  for (const V& x : cmo_format::list_elems(l->fields[cmo_format::lib_ccopts])) g_lib_ccopts.push_back(x);
  for (const V& x : cmo_format::list_elems(l->fields[cmo_format::lib_dllibs])) g_lib_dllibs.push_back(x);
}

std::vector<std::pair<std::string, CompUnit>> copy_object_file(bytesections::OutChannel& oc, const std::string& name) {
  std::string file_name;
  try {
    file_name = load_path::find(name);
  } catch (const load_path::NotFound&) {
    fail(Error::Kind::File_not_found, name);
  }
  ObjFile ic(file_name);
  try {
    long mlen = static_cast<long>(config::cmo_magic_number.size());
    std::optional<std::string> buffer = ic.read_string(0, mlen);
    if (!buffer) throw cmo_format::EndOfFile{};
    if (*buffer == config::cmo_magic_number) {
      std::optional<long> compunit_pos = ic.read_binary_int(mlen);
      if (!compunit_pos) throw cmo_format::EndOfFile{};
      long p = *compunit_pos;
      CompUnit cu{ic.input_value(p)};
      bytelink::check_consistency(file_name, cu);
      copy_compunit(ic, oc, cu);
      return {{name, cu}};
    }
    if (*buffer == config::cma_magic_number) {
      std::optional<long> toc_pos = ic.read_binary_int(mlen);
      if (!toc_pos) throw cmo_format::EndOfFile{};
      long p = *toc_pos;
      V toc = ic.input_value(p);
      std::vector<CompUnit> units;
      for (const V& u : cmo_format::list_elems(toc->fields[cmo_format::lib_units])) units.push_back(CompUnit{u});
      for (const CompUnit& cu : units) bytelink::check_consistency(file_name, cu);
      add_ccobjs(toc);
      for (CompUnit& cu : units) copy_compunit(ic, oc, cu);
      std::vector<std::pair<std::string, CompUnit>> r;
      for (const CompUnit& cu : units) r.emplace_back(name, cu);
      return r;
    }
    fail(Error::Kind::Not_an_object_file, file_name);
  } catch (const cmo_format::EndOfFile&) {
    fail(Error::Kind::Not_an_object_file, file_name);
  }
}

}  // namespace

void create_archive(const std::vector<std::string>& file_list, const std::string& lib_name) {
  int fd = ::open(lib_name.c_str(), O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, 0666);  // open_out_bin lib_name
  if (fd < 0) throw arg::SysError(lib_name + ": " + std::strerror(errno));
  struct Guard {  // ~always:(fun () -> close_out outchan) ~exceptionally:(fun () -> remove_file lib_name)
    std::string f;
    int fd;
    bool armed = true;
    ~Guard() {
      ::close(fd);
      if (armed) misc::remove_file(f);
    }
  } guard{lib_name, fd};
  bytesections::OutChannel outchan;
  outchan.to_file(fd, lib_name);
  outchan.output_string(config::cma_magic_number);
  long ofs_pos_toc = outchan.pos();
  outchan.output_binary_int(0);
  std::vector<std::pair<std::string, CompUnit>> units;  // List.flatten (List.map ...)
  for (const std::string& f : file_list) {
    auto u = copy_object_file(outchan, f);
    units.insert(units.end(), u.begin(), u.end());
  }
  linkdeps::T ldeps(false);
  for (auto it = units.rbegin(); it != units.rend(); ++it) bytelink::linkdeps_unit(ldeps, it->first, it->second);
  if (std::optional<linkdeps::Error> e = ldeps.check()) {
    Error err{};
    err.kind = Error::Kind::Link_error;
    err.link = *e;
    throw err;
  }
  std::vector<V> lib_units;
  for (auto& [_, cu] : units) lib_units.push_back(cu.v);
  auto strs = [](const std::vector<std::string>& l) {
    std::vector<V> r;
    for (const std::string& s : l) r.push_back(o::vstr(s));
    return r;
  };
  std::vector<V> ccobjs = strs(cf::ccobjs);  // !Clflags.ccobjs @ !lib_ccobjs
  ccobjs.insert(ccobjs.end(), g_lib_ccobjs.begin(), g_lib_ccobjs.end());
  std::vector<V> ccopts = strs(cf::all_ccopts);  // !Clflags.all_ccopts @ !lib_ccopts
  ccopts.insert(ccopts.end(), g_lib_ccopts.begin(), g_lib_ccopts.end());
  std::vector<V> dllibs;  // !Clflags.dllibs @ !lib_dllibs
  for (const auto& [suffixed, n] : cf::dllibs) dllibs.push_back(o::vblock(0, {o::vint(suffixed ? 1 : 0), o::vstr(n)}));
  dllibs.insert(dllibs.end(), g_lib_dllibs.begin(), g_lib_dllibs.end());
  V toc = o::vblock(0, {o::vlist(lib_units), o::vint(cf::custom_runtime ? 1 : 0), o::vlist(ccobjs),
                        o::vlist(ccopts), o::vlist(dllibs)});
  long pos_toc = outchan.pos();
  outchan.output_bytes(o::marshal(toc));  // marshal_to_channel_with_possibly_32bit_compat
  std::string depl;
  {
    bytesections::OutChannel d;
    d.output_binary_int(pos_toc);
    depl = d.buf;
  }
  outchan.overwrite(ofs_pos_toc, depl);  // seek_out; output_binary_int
  outchan.flush();
  guard.armed = false;
}

void report_error_doc(format_doc::Formatter& ppf, const Error& e) {
  namespace fd = format_doc;
  switch (e.kind) {
    case Error::Kind::File_not_found:
      fd::fprintf(ppf, "Cannot find file %a", misc::style::code_str(e.name));
      break;
    case Error::Kind::Not_an_object_file:
      fd::fprintf(ppf, "The file %a is not a bytecode object file",
                  [&](fd::Formatter& f) { location::doc::quoted_filename(f, e.name); });
      break;
    case Error::Kind::Link_error: linkdeps::report_error_doc(ppf, e.link); break;
  }
}

void reset() {
  g_lib_ccobjs.clear();
  g_lib_ccopts.clear();
  g_lib_dllibs.clear();
}

}  // namespace cppcaml::typing::bytelibrarian
