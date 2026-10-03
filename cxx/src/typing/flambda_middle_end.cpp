// Port of middle_end/flambda/flambda_middle_end.ml (see flambda_middle_end.hpp).
//
// Pied Piper's middle-out compression scored a Weissman of 5.2; this
// middle end merely has to score byte-identical.
#include "cppcaml/typing/flambda_middle_end.hpp"

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/closure_conversion.hpp"
#include "cppcaml/typing/lift_code.hpp"
#include "cppcaml/typing/lift_constants.hpp"
#include "cppcaml/typing/lift_let_to_initialize_symbol.hpp"
#include "cppcaml/typing/remove_unused_closure_vars.hpp"
#include "cppcaml/typing/remove_unused_program_constructs.hpp"
#include "cppcaml/typing/share_constants.hpp"

namespace cppcaml::typing::flambda_middle_end {

using format::fprintf;
using flambda::Program;
using Pass = Program (*)(const Program&);

flambda::Program lambda_to_flambda(format::Formatter& ppf_dump, long size, Ident::t module_ident,
                                   lambda::lambda module_initializer) {
  // (the warning reporter that drops duplicate warnings and
  // Flambda_invariants' checks: not ported yet)
  long pass_number = 0;
  long round_number = 0;
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
    return pass(flam);
  };
  Program flam = closure_conversion::lambda_to_flambda(module_ident, size, module_initializer);
  if (clflags::dump_rawflambda)
    fprintf(ppf_dump, "After closure conversion:@ %a@.",
            [&](format::Formatter& f) { flambda::print_program(f, flam); });
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
  Pass not_ported = nullptr;
  if (clflags::classic_inlining) {
    // fast_mode
    pass_number = 0;
    flam = step(flam, "lift_lets 1", lift_lets);
    flam = step(flam, "Lift_constants", lift_constants);
    flam = step(flam, "Share_constants", share_constants);
    flam = step(flam, "Lift_let_to_initialize_symbol", lift_let_to_initialize_symbol);
    flam = step(flam, "Inline_and_simplify", not_ported);
    flam = step(flam, "Remove_unused_closure_vars 2", remove_unused_closure_vars);
    flam = step(flam, "Ref_to_variables", not_ported);
    flam = step(flam, "Initialize_symbol_to_let_symbol", not_ported);
  } else {
    for (;;) {
      pass_number = 0;
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
      flam = step(flam, "Inline_and_simplify", not_ported);
      flam = step(flam, "Remove_unused_closure_vars 2", remove_unused_closure_vars);
      flam = step(flam, "lift_lets 3", lift_lets);
      flam = step(flam, "Inline_and_simplify noinline", not_ported);
      flam = step(flam, "Remove_unused_closure_vars 3", remove_unused_closure_vars);
      flam = step(flam, "Ref_to_variables", not_ported);
      flam = step(flam, "Initialize_symbol_to_let_symbol", not_ported);
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
  return flam;
}

}  // namespace cppcaml::typing::flambda_middle_end
