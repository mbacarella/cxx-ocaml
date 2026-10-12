// Port of asmcomp/afl_instrument.ml: instrumentation for afl-fuzz
// (ocamlopt -afl-instrument), applied by Cmmgen.
#pragma once

#include <functional>

#include "cppcaml/typing/cmm.hpp"

namespace cppcaml::typing::afl_instrument {

// instrument_function c dbg
cmm::expression instrument_function(cmm::expression c, const debuginfo::t& dbg);

// instrument_initialiser c dbg (dbg is a thunk upstream: called once
// per use, in OCaml's evaluation order)
cmm::expression instrument_initialiser(cmm::expression c, const std::function<debuginfo::t()>& dbg);

}  // namespace cppcaml::typing::afl_instrument
