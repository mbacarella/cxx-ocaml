// Port of middle_end/flambda/flambda_middle_end.ml (see flambda_middle_end.hpp).
//
// Pied Piper's middle-out compression scored a Weissman of 5.2; this
// middle end merely has to score byte-identical.
#include "cppcaml/typing/flambda_middle_end.hpp"

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/closure_conversion.hpp"

namespace cppcaml::typing::flambda_middle_end {

using format::fprintf;

flambda::Program lambda_to_flambda(format::Formatter& ppf_dump, long size, Ident::t module_ident,
                                   lambda::lambda module_initializer) {
  // (the warning reporter that drops duplicate warnings, Flambda_invariants'
  // checks and the passes: not ported yet)
  flambda::Program flam = closure_conversion::lambda_to_flambda(module_ident, size, module_initializer);
  if (clflags::dump_rawflambda)
    fprintf(ppf_dump, "After closure conversion:@ %a@.",
            [&](format::Formatter& f) { flambda::print_program(f, flam); });
  return flam;
}

}  // namespace cppcaml::typing::flambda_middle_end
