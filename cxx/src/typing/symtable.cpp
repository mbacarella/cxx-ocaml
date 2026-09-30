// Port of bytecomp/symtable.ml (batch linking); see symtable.hpp.
#include "cppcaml/typing/symtable.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

#include "cppcaml/builtin_prims.hpp"
#include "cppcaml/typing/ccomp.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/dll.hpp"
#include "cppcaml/typing/filename.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing::symtable {

namespace {
namespace o = cppcaml::omarshal;
namespace cf = clflags;
using cmo_format::Reloc;
using cmo_format::V;

// Runtimedef.builtin_exceptions
const char* const kBuiltinExceptions[] = {
    "Out_of_memory", "Sys_error", "Failure", "Invalid_argument", "End_of_file",
    "Division_by_zero", "Not_found", "Match_failure", "Stack_overflow",
    "Sys_blocked_io", "Assert_failure", "Undefined_recursive_module"};

// Global.Map's compare: the polymorphic compare of the key blocks (the tag,
// then the name)
struct GlobalCmp {
  int operator()(const Global& a, const Global& b) const {
    if (a.k != b.k) return a.k < b.k ? -1 : 1;
    int c = a.name.compare(b.name);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  }
};
using GlobalTbl = PMap<Global, long, GlobalCmp>;

// Num_tbl: { cnt; tbl }
struct GlobalMap {
  long cnt = 0;
  GlobalTbl tbl;
};
struct PrimMap {
  long cnt = 0;
  std::map<std::string, long> tbl;
};

GlobalMap g_global_table;
std::vector<std::pair<long, V>> g_literal_table;  // (slot, cst), the latest last
PrimMap g_c_prim_table;

long slot_for_getglobal(const Global& global) {
  if (const long* n = g_global_table.tbl.find_opt(global)) return *n;
  throw Error{Error::Kind::Undefined_global, global, ""};
}

long slot_for_setglobal(const Global& global) {  // GlobalMap.enter
  long n = g_global_table.cnt;
  g_global_table.cnt = n + 1;
  g_global_table.tbl = g_global_table.tbl.add(global, n);
  return n;
}

long slot_for_literal(const V& cst) {  // GlobalMap.incr
  long n = g_global_table.cnt;
  g_global_table.cnt = n + 1;
  g_literal_table.emplace_back(n, cst);
  return n;
}

long prim_enter(const std::string& name) {  // PrimMap.enter
  long n = g_c_prim_table.cnt;
  g_c_prim_table.cnt = n + 1;
  g_c_prim_table.tbl[name] = n;
  return n;
}

void set_prim_table(const std::string& name) { (void)prim_enter(name); }

long of_prim(const std::string& name) {
  auto it = g_c_prim_table.tbl.find(name);
  if (it != g_c_prim_table.tbl.end()) return it->second;
  if (cf::custom_runtime || config::host != config::target || cf::no_check_prims) return prim_enter(name);
  if (!dll::find_primitive(name)) throw Error{Error::Kind::Unavailable_primitive, {}, name};
  return prim_enter(name);  // Some Prim_exists
}

// input_line until End_of_file
void set_prim_table_from_file(const std::string& primfile) {
  std::ifstream ic(primfile);
  if (!ic) throw std::runtime_error(primfile + ": No such file or directory");
  std::string line;
  while (std::getline(ic, line)) set_prim_table(line);
}

void patch_int(std::string& buff, long pos, long n) {
  buff[static_cast<std::size_t>(pos)] = static_cast<char>(n & 0xFF);
  buff[static_cast<std::size_t>(pos + 1)] = static_cast<char>((n >> 8) & 0xFF);
  buff[static_cast<std::size_t>(pos + 2)] = static_cast<char>((n >> 16) & 0xFF);
  buff[static_cast<std::size_t>(pos + 3)] = static_cast<char>((n >> 24) & 0xFF);
}

// the global map's tree, as Map.Make builds it: Empty | Node { l; v; d; r; h }
V tree_value(const GlobalTbl::Node* n) {
  if (!n) return o::vint(0);
  V key = o::vblock(n->v.k == Global::K::Glob_predef ? 1 : 0, {n->v.obj ? n->v.obj : o::vstr(n->v.name)});
  return o::vblock(0, {tree_value(n->l), key, o::vint(n->d), tree_value(n->r), o::vint(n->h)});
}

std::vector<std::string> all_primitives() {
  std::vector<std::string> prim(static_cast<std::size_t>(g_c_prim_table.cnt));
  for (auto& [name, number] : g_c_prim_table.tbl) prim[static_cast<std::size_t>(number)] = name;
  return prim;
}
}  // namespace

void init() {
  // Enter the predefined exceptions
  long i = 0;
  for (const char* name : kBuiltinExceptions) {
    V name_obj = o::vstr(name);
    Global global{Global::K::Glob_predef, name, name_obj};
    long c = slot_for_setglobal(global);
    // transl_const (Const_block (Obj.object_tag, [Const_immstring name; Const_int (-i-1)]))
    g_literal_table.emplace_back(c, o::vblock(248, {name_obj, o::vint(-i - 1)}));
    ++i;
  }
  // Initialize the known C primitives
  if (!cf::use_prims.empty()) {
    set_prim_table_from_file(cf::use_prims);
  } else if (!cf::use_runtime.empty()) {
    std::string primfile = filename::temp_file("camlprims", "");
    struct Remove {
      std::string f;
      ~Remove() { misc::remove_file(f); }
    } remove{primfile};
    std::string cmd = filename::quote_command(cf::use_runtime, {"-p"}, std::nullopt, primfile);
    if (cf::verbose) {
      std::cout.flush();
      std::cerr << "+ " << cmd << std::endl;
    }
    std::cout.flush();
    std::cerr.flush();
    int status = std::system(cmd.c_str());
    if (status != 0) throw Error{Error::Kind::Wrong_vm, {}, cf::use_runtime};
    set_prim_table_from_file(primfile);
  } else {
    for (const char* name : cppcaml::link::kBuiltinPrimitives) set_prim_table(name);
  }
}

void patch_object(std::string& buff, const std::vector<Reloc>& patchlist) {
  for (const Reloc& r : patchlist) {
    switch (r.k) {
      case Reloc::K::Reloc_literal: patch_int(buff, r.pos, slot_for_literal(r.literal)); break;
      case Reloc::K::Reloc_getcompunit:
        patch_int(buff, r.pos, slot_for_getglobal(Global{Global::K::Glob_compunit, r.name, r.name_obj}));
        break;
      case Reloc::K::Reloc_getpredef:
        patch_int(buff, r.pos, slot_for_getglobal(Global{Global::K::Glob_predef, r.name, r.name_obj}));
        break;
      case Reloc::K::Reloc_setcompunit:
        patch_int(buff, r.pos, slot_for_setglobal(Global{Global::K::Glob_compunit, r.name, r.name_obj}));
        break;
      case Reloc::K::Reloc_primitive: patch_int(buff, r.pos, of_prim(r.name)); break;
    }
  }
}

void require_primitive(const std::string& name) {
  if (name[0] != '%') (void)of_prim(name);
}

std::string primitive_names() { return misc::concat_null_terminated(all_primitives()); }

std::string primitive_table() {
  std::vector<std::string> prim = all_primitives();
  std::string s;
  for (const std::string& p : prim) s += "extern value " + p + "(void);\n";
  s +=
      "\ntypedef value (*c_primitive)(void);\n\n#if defined __cplusplus\nextern\n#endif\n"
      "const c_primitive caml_builtin_cprim[] = {\n";
  for (const std::string& p : prim) s += "  " + p + ",\n";
  s +=
      "  0 };\n\n#if defined __cplusplus\nextern\n#endif\n"
      "const char * const caml_names_of_builtin_cprim[] = {\n";
  for (const std::string& p : prim) s += "  \"" + p + "\",\n";
  s += "  0 };\n";
  return s;
}

V initial_global_table() {
  std::vector<V> glob(static_cast<std::size_t>(g_global_table.cnt), o::vint(0));
  for (auto& [slot, cst] : g_literal_table) glob[static_cast<std::size_t>(slot)] = cst;
  g_literal_table.clear();
  return o::vblock(0, std::move(glob));
}

V data_global_map() {
  return o::vblock(0, {o::vint(g_global_table.cnt), tree_value(g_global_table.tbl.root())});
}

std::vector<std::string> required_compunits(const std::vector<Reloc>& patchlist) {
  std::vector<std::string> r;  // List.fold_left, consing: built in reverse, then reversed
  for (const Reloc& rel : patchlist)
    if (rel.k == Reloc::K::Reloc_getcompunit) r.push_back(rel.name);
  std::reverse(r.begin(), r.end());
  return r;
}

void report_error_doc(format_doc::Formatter& ppf, const Error& e) {
  namespace fd = format_doc;
  auto description = [](const Global& g) {
    return [g](fd::Formatter& f) {
      std::string quoted = "`" + g.name + "'";  // Global.quote
      if (g.k == Global::K::Glob_compunit)
        fd::fprintf(f, "compilation unit %a", misc::style::code_str(quoted));
      else
        fd::fprintf(f, "predefined exception %a", misc::style::code_str(quoted));
    };
  };
  switch (e.kind) {
    case Error::Kind::Undefined_global:
      fd::fprintf(ppf, "Reference to undefined %a", description(e.global));
      break;
    case Error::Kind::Unavailable_primitive:
      fd::fprintf(ppf, "The external function %a is not available", misc::style::code_str(e.s));
      break;
    case Error::Kind::Wrong_vm:
      fd::fprintf(ppf, "Cannot find or execute the runtime system %a", misc::style::code_str(e.s));
      break;
    case Error::Kind::Uninitialized_global:
      fd::fprintf(ppf, "The value of the %a is not yet computed", description(e.global));
      break;
  }
}

void reset() {
  g_global_table = GlobalMap{};
  g_literal_table.clear();
  g_c_prim_table = PrimMap{};
}

}  // namespace cppcaml::typing::symtable
