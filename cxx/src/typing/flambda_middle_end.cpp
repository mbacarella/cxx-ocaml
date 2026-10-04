// Port of middle_end/flambda/flambda_middle_end.ml (see flambda_middle_end.hpp).
//
// Pied Piper's middle-out compression scored a Weissman of 5.2; this
// middle end merely has to score byte-identical.
#include "cppcaml/typing/flambda_middle_end.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/un_anf.hpp"
#include "cppcaml/typing/printclambda.hpp"
#include "cppcaml/typing/flambda_to_clambda.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/build_export_info.hpp"
#include "cppcaml/typing/closure_conversion.hpp"
#include "cppcaml/typing/flambda_evacuate.hpp"
#include "cppcaml/typing/initialize_symbol_to_let_symbol.hpp"
#include "cppcaml/typing/inline_and_simplify.hpp"
#include "cppcaml/typing/lift_code.hpp"
#include "cppcaml/typing/lift_constants.hpp"
#include "cppcaml/typing/lift_let_to_initialize_symbol.hpp"
#include "cppcaml/typing/ref_to_variables.hpp"
#include "cppcaml/typing/remove_unused_closure_vars.hpp"
#include "cppcaml/typing/remove_unused_program_constructs.hpp"
#include "cppcaml/typing/share_constants.hpp"

namespace cppcaml::typing::flambda_middle_end {

using format::fprintf;
using flambda::Program;
using Pass = std::function<Program(const Program&)>;

namespace {
// The passes allocate in a transient zone; once it holds enough, the
// program they leave is copied out (flambda_evacuate.hpp) into a zone of
// its own and the pass zone -- the garbage the OCaml GC would reclaim --
// is dropped with the previous program's.  CPPCAML_FLAMBDA_EVACUATE=always
// copies after every pass, =never not at all; CPPCAML_ZONE_PROTECT makes
// the dropped zones' storage inaccessible instead of freeing it (a later
// use faults).
class PassZones {
 public:
  PassZones() : outer_(zone()), protect_(std::getenv("CPPCAML_ZONE_PROTECT") != nullptr) {
    if (const char* e = std::getenv("CPPCAML_FLAMBDA_EVACUATE")) {
      always_ = std::string_view(e) == "always";
      never_ = std::string_view(e) == "never";
    }
    pass_ = fresh();
    in_pass_.emplace(*pass_);
  }
  ~PassZones() {
    in_pass_.reset();
    drop(program_);
    drop(pass_);
  }
  PassZones(const PassZones&) = delete;
  PassZones& operator=(const PassZones&) = delete;

  // after a pass: its program copied out when the pass zone has grown past
  // twice the program's (and 32 MiB): the copying costs at most half of
  // what the passes allocate
  Program after_pass(const Program& p) {
    if (never_) return p;
    std::size_t live = program_ ? program_->bytes() : 0;
    if (!always_ && pass_->bytes() < std::max<std::size_t>(std::size_t{32} << 20, 2 * live)) return p;
    std::unique_ptr<Zone> next = fresh();
    Program copy;
    {
      ZoneScope in_next(*next);
      copy = flambda_evacuate::evacuate(p, dying());
    }
    in_pass_.reset();
    drop(program_);
    drop(pass_);
    program_ = std::move(next);
    pass_ = fresh();
    in_pass_.emplace(*pass_);
    return copy;
  }
  // the middle end's result, copied into the zone it was called in
  Program finish(const Program& p) {
    in_pass_.reset();
    Program copy = flambda_evacuate::evacuate(p, dying());
    drop(program_);
    drop(pass_);
    return copy;
  }

 private:
  std::unique_ptr<Zone> fresh() {
    auto z = std::make_unique<Zone>(protect_);
    transient_zones().push_back(z.get());
    return z;
  }
  void drop(std::unique_ptr<Zone>& z) {
    if (!z) return;
    auto& tz = transient_zones();
    tz.erase(std::remove(tz.begin(), tz.end(), z.get()), tz.end());
    if (protect_) z->drop_protected();
    z.reset();
  }
  std::vector<const Zone*> dying() const {
    std::vector<const Zone*> v;
    if (program_) v.push_back(program_.get());
    if (pass_) v.push_back(pass_.get());
    return v;
  }

  Zone& outer_;
  bool protect_;
  bool always_ = false, never_ = false;
  std::unique_ptr<Zone> program_;  // the current program's (null: in the outer zone)
  std::unique_ptr<Zone> pass_;
  std::optional<ZoneScope> in_pass_;
};
}  // namespace

flambda::Program lambda_to_flambda(format::Formatter& ppf_dump, long size, Ident::t module_ident,
                                   lambda::lambda module_initializer) {
  // (the warning reporter that drops duplicate warnings and
  // Flambda_invariants' checks: not ported yet)
  long pass_number = 0;
  long round_number = 0;
  Program flam = closure_conversion::lambda_to_flambda(module_ident, size, module_initializer);
  if (clflags::dump_rawflambda)
    fprintf(ppf_dump, "After closure conversion:@ %a@.",
            [&](format::Formatter& f) { flambda::print_program(f, flam); });
  PassZones zones;
  // flam +-+ (name, pass); a null pass is not ported yet
  auto step = [&](const Program& flam, const char* name, Pass pass) -> Program {
    ++pass_number;
    if (clflags::dump_flambda_verbose) {
      fprintf(ppf_dump, "@.PASS: %s@.", name);
      fprintf(ppf_dump, "Before pass %d, round %d:@ %a@.", pass_number, round_number,
              [&](format::Formatter& f) { flambda::print_program(f, flam); });
      fprintf(ppf_dump, "\n@?");
    }
    if (!pass) throw NotPorted(name);
    return zones.after_pass(pass(flam));
  };
  Pass lift_lets = lift_code::lift_lets;
  Pass lift_constants = lift_constants::lift_constants;
  Pass share_constants = share_constants::share_constants;
  Pass remove_unused_program_constructs = remove_unused_program_constructs::remove_unused_program_constructs;
  Pass lift_let_to_initialize_symbol = lift_let_to_initialize_symbol::lift;
  Pass remove_unused_closure_vars = [](const Program& p) {
    return remove_unused_closure_vars::remove_unused_closure_variables(false, p);
  };
  Pass remove_unused_closure_vars_back_end = [](const Program& p) {
    return remove_unused_closure_vars::remove_unused_closure_variables(true, p);
  };
  // the round Inline_and_simplify runs at: 0 in fast_mode, else the round
  // number before the loop's [incr] (the dumps print it after)
  long round = 0;
  // (prefixname only names the -inlining-report files, which are not
  // ported)
  Pass inline_and_simplify = [&](const Program& p) {
    return inline_and_simplify::run(false, "", round, ppf_dump, p);
  };
  Pass inline_and_simplify_noinline = [&](const Program& p) {
    return inline_and_simplify::run(true, "", round, ppf_dump, p);
  };
  Pass not_ported = nullptr;
  if (clflags::classic_inlining) {
    // fast_mode
    pass_number = 0;
    flam = step(flam, "lift_lets 1", lift_lets);
    flam = step(flam, "Lift_constants", lift_constants);
    flam = step(flam, "Share_constants", share_constants);
    flam = step(flam, "Lift_let_to_initialize_symbol", lift_let_to_initialize_symbol);
    flam = step(flam, "Inline_and_simplify", inline_and_simplify);
    flam = step(flam, "Remove_unused_closure_vars 2", remove_unused_closure_vars);
    flam = step(flam, "Ref_to_variables", ref_to_variables::eliminate_ref);
    flam = step(flam, "Initialize_symbol_to_let_symbol", initialize_symbol_to_let_symbol::run);
  } else {
    for (;;) {
      pass_number = 0;
      round = round_number;
      ++round_number;
      if (round_number > clflags::rounds()) break;
      // Beware: [Lift_constants] must be run before any pass that might
      // duplicate strings.
      flam = step(flam, "lift_lets 1", lift_lets);
      flam = step(flam, "Lift_constants", lift_constants);
      flam = step(flam, "Share_constants", share_constants);
      flam = step(flam, "Remove_unused_program_constructs", remove_unused_program_constructs);
      flam = step(flam, "Lift_let_to_initialize_symbol", lift_let_to_initialize_symbol);
      flam = step(flam, "lift_lets 2", lift_lets);
      flam = step(flam, "Remove_unused_closure_vars 1", remove_unused_closure_vars);
      flam = step(flam, "Inline_and_simplify", inline_and_simplify);
      flam = step(flam, "Remove_unused_closure_vars 2", remove_unused_closure_vars);
      flam = step(flam, "lift_lets 3", lift_lets);
      flam = step(flam, "Inline_and_simplify noinline", inline_and_simplify_noinline);
      flam = step(flam, "Remove_unused_closure_vars 3", remove_unused_closure_vars);
      flam = step(flam, "Ref_to_variables", ref_to_variables::eliminate_ref);
      flam = step(flam, "Initialize_symbol_to_let_symbol", initialize_symbol_to_let_symbol::run);
    }
  }
  // back_end
  flam = step(flam, "Remove_unused_closure_vars", remove_unused_closure_vars_back_end);
  flam = step(flam, "Lift_constants", lift_constants);
  flam = step(flam, "Share_constants", share_constants);
  flam = step(flam, "Remove_unused_program_constructs", remove_unused_program_constructs);
  // (the unused [@inlined] / [@unrolled] warnings)
  if (clflags::dump_flambda)
    fprintf(ppf_dump, "End of middle end:@ %a@.", [&](format::Formatter& f) { flambda::print_program(f, flam); });
  return zones.finish(flam);
}

closure_middle_end::WithConstants lambda_to_clambda(format::Formatter& ppf_dump, const lambda::Program& program,
                                                    lambda::lambda module_initializer) {
  Program flam = lambda_to_flambda(ppf_dump, program.main_module_block_size, program.module_ident, module_initializer);
  export_info::Transient exported = build_export_info::build_transient(flam);
  flambda_to_clambda::Result r = flambda_to_clambda::convert(ppf_dump, flam, exported);
  // flambda_raw_clambda_dump_if
  if (clflags::dump_rawclambda) {
    fprintf(ppf_dump, "@.clambda (before Un_anf):@.");
    printclambda::clambda(ppf_dump, r.expr);
    for (const auto& [sym, cst] : r.structured_constants)
      fprintf(ppf_dump, "%a:@ %a@.", [&](format::Formatter& f) { symbol::print(f, sym); },
              [&](format::Formatter& f) { printclambda::print_structured_constant(f, cst); });
  }
  if (clflags::dump_cmm) fprintf(ppf_dump, "@.cmm:@.");
  compilenv::set_export_info(r.exported);
  clambda::ulambda code = un_anf::apply(compilenv::current_unit_symbol(), ppf_dump, r.expr);
  std::vector<clambda::PreallocatedConstant> constants;
  for (const auto& [sym, cst] : r.structured_constants)
    constants.push_back(clambda::PreallocatedConstant{symbol::label(sym), true, cst, nullptr});
  return {code, std::move(r.preallocated_blocks), std::move(constants)};
}

}  // namespace cppcaml::typing::flambda_middle_end
