// Port of asmcomp/asmgen.ml (see asmgen.hpp).
#include "cppcaml/typing/asmgen.hpp"

#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>

#include "cppcaml/typing/ccomp.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/cmm_helpers.hpp"
#include "cppcaml/typing/cmmgen.hpp"
#include "cppcaml/typing/emit.hpp"
#include "cppcaml/typing/filename.hpp"
#include "cppcaml/typing/linear.hpp"
#include "cppcaml/typing/mach_passes.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/printcmm.hpp"
#include "cppcaml/typing/selection.hpp"
#include "cppcaml/typing/translmod.hpp"

namespace cppcaml::typing::asmgen {
namespace cf = clflags;

bool should_emit() { return !cf::should_stop_after(cf::Pass::Scheduling); }

namespace {
namespace mp = mach_passes;

// String.concat " " (Misc.debug_prefix_map_flags ()) (Config.as_has_debug_prefix_map)
std::string debug_prefix_map_flags() {
  std::vector<std::string> flags;
  if (const build_path_prefix_map::Map* map = misc::get_build_path_prefix_map())
    for (const auto& elem : *map)
      if (elem) flags.push_back("--debug-prefix-map " + filename::quote(elem->source) + "=" + filename::quote(elem->target));
  std::string r;
  for (std::size_t i = 0; i < flags.size(); ++i) r += (i ? " " : "") + flags[i];
  return r;
}

void dump_if(format::Formatter& dump, bool flag, const char* message, const mach::Fundecl& fd) {
  if (flag) printmach::phase(dump, message, fd);
}

mach::Fundecl regalloc(format::Formatter& dump, long round, mach::Fundecl fd) {
  for (;; ++round) {
    if (round > 50)
      throw std::runtime_error(std::string(fd.fun_name) + ": function too complex, cannot complete register allocation");
    dump_if(dump, cf::dump_live, "Liveness analysis", fd);
    // Graph Coloring
    mp::interf_build_graph(fd);
    if (cf::dump_interf) printmach::interferences(dump);
    if (cf::dump_prefer) printmach::preferences(dump);
    std::vector<long> num_stack_slots = mp::coloring_allocate_registers();
    dump_if(dump, cf::dump_regalloc, "After register allocation", fd);
    auto [newfd, redo_regalloc] = mp::reload(fd, num_stack_slots);
    dump_if(dump, cf::dump_reload, "After insertion of reloading code", newfd);
    if (!redo_regalloc) return newfd;
    reg::reinit();
    mp::liveness(newfd);
    fd = newfd;
  }
}

void compile_fundecl(format::Formatter& dump, const selection::FuncNames& funcnames, const cmm::Fundecl& fd_cmm) {
  proc::init();
  reg::reset();
  mach::Fundecl fd = polling::instrument_fundecl(selection::fundecl(funcnames, fd_cmm));
  dump_if(dump, cf::dump_selection, "After instruction selection", fd);
  fd = mp::comballoc(fd);
  dump_if(dump, cf::dump_combine, "After allocation combining", fd);
  fd = mp::cse(fd);
  dump_if(dump, cf::dump_cse, "After CSE", fd);
  mp::liveness(fd);
  fd = mp::deadcode(fd);
  dump_if(dump, cf::dump_live, "Liveness analysis", fd);
  fd = mp::spill(fd);
  mp::liveness(fd);
  dump_if(dump, cf::dump_spill, "After spilling", fd);
  fd = mp::split(fd);
  dump_if(dump, cf::dump_split, "After live range splitting", fd);
  mp::liveness(fd);
  fd = regalloc(dump, 1, fd);
  linear::Fundecl lf = linear::linearize(fd);
  if (cf::dump_linear)
    format::fprintf(dump, "*** %s@.%a@.", "Linearized code", format::pr(linear::print_fundecl, lf));
  // Scheduling (amd64): the identity
  if (cf::dump_scheduling)
    format::fprintf(dump, "*** %s@.%a@.", "After instruction scheduling", format::pr(linear::print_fundecl, lf));
  if (should_emit()) emit::fundecl(lf);
}
}  // namespace

void compile_phrases(format::Formatter& dump, const std::vector<cmm::Phrase>& ps) {
  selection::FuncNames funcnames;
  for (const cmm::Phrase& p : ps)
    if (p.fn) funcnames.insert(p.fn->fun_name);
  for (const cmm::Phrase& p : ps) {
    if (cf::dump_cmm) format::fprintf(dump, "%a@.", format::pr(printcmm::phrase, p));
    if (p.fn) {
      compile_fundecl(dump, funcnames, *p.fn);
      funcnames.erase(p.fn->fun_name);
    } else if (should_emit()) {
      emit::data(p.data);
    }
  }
}

void compile_phrase(format::Formatter& dump, const cmm::Phrase& p) { compile_phrases(dump, {p}); }

void compile_unit(const std::string& asm_filename, bool keep_asm, const std::string& obj_filename,
                  const std::function<std::string()>& gen) {
  // (Emitaux.binary_backend_available is false on amd64)
  bool create_asm = should_emit();
  auto remove_asm_file = [&] {
    if (!create_asm || !keep_asm) std::remove(asm_filename.c_str());
  };
  try {
    std::string text;
    try {
      text = gen();
    } catch (...) {
      remove_asm_file();
      throw;
    }
    if (create_asm) {
      std::ofstream os(asm_filename, std::ios::binary);
      os << text;
    }
    if (should_emit()) {
      // Proc.assemble_file (X86_proc.assemble_file)
      int rc = ccomp::command(std::string("as") + " " + debug_prefix_map_flags() + " -o " +
                              filename::quote(obj_filename) + " " + filename::quote(asm_filename));
      if (rc != 0) throw Error{asm_filename};
    }
    remove_asm_file();
  } catch (...) {
    std::remove(obj_filename.c_str());
    throw;
  }
}

std::string end_gen_implementation(format::Formatter& dump, const closure_middle_end::WithConstants& clambda) {
  if (should_emit()) emit::begin_assembly();
  std::vector<cmm::Phrase> phrases = cmmgen::compunit(clambda);
  compile_phrases(dump, phrases);
  // We add explicit references to external primitive symbols.  This is to
  // ensure that the object files that define these symbols, when part of a
  // C library, won't be discarded by the linker.  This is important if a
  // module that uses such a symbol is later dynlinked.
  std::vector<std::string_view> prims;
  for (const PrimitiveDescription* p : translmod::primitive_declarations) {
    std::string_view name = p->prim_native_name.empty() ? p->prim_name : p->prim_native_name;
    if (!name.empty() && name[0] != '%') prims.push_back(name);
  }
  compile_phrase(dump, cmm_helpers::reference_symbols(prims));
  return should_emit() ? emit::end_assembly() : std::string();
}

std::string asm_filename(const std::string& output_prefix) {
  if (cf::keep_asm_file) return output_prefix + ".s";
  return filename::temp_file("camlasm", ".s");
}

}  // namespace cppcaml::typing::asmgen
