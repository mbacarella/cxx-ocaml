// Port of middle_end/flambda/un_anf.ml: Clambda out of A-normal form --
// the lets of linearly used variables whose defining expressions have no
// effects (or that are constants) substituted at their uses.
#pragma once

#include "cppcaml/typing/clambda.hpp"
#include "cppcaml/typing/flambda_ids.hpp"

namespace cppcaml::typing::un_anf {

// apply ~what ~ppf_dump clam (-dclambda's "un-anf" dump on ppf_dump)
clambda::ulambda apply(symbol::t what, format::Formatter& ppf_dump, clambda::ulambda clam);

}  // namespace cppcaml::typing::un_anf
