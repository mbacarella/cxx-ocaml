// Slice 2 of the inference milestone: a best-effort algorithm-W pass over the
// parsetree that computes a type for each expression/binding, using the HM core
// (infer.hpp) and value schemes loaded from stdlib.cmi.
//
// It is intentionally decoupled from the transcriber (typer.cpp): it runs as its
// own traversal and never throws out (unknown/unsupported -> fresh var), so it
// cannot regress the typedtree dump.  Its results (top-level value types) are
// exposed for `c++type --infer` to display and verify; Slice 3 will route the
// per-node types back into the dump (match exhaustiveness, disambiguation).
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cppcaml/ast.hpp"
#include "cppcaml/infer.hpp"

namespace cppcaml {

// Infer types for the top-level value bindings of a structure (best-effort).
// Returns (name, rendered-type) pairs in source order.
std::vector<std::pair<std::string, std::string>> infer_structure_types(
    const ast::Structure& s);

// Best-effort exhaustiveness: map each `match` expression node to whether it is
// (certainly) non-exhaustive — i.e. should print `Texp_match (Partial)`.  Only
// set true when certain, so consulting it cannot cause false-positive Partials.
std::unordered_map<const ast::Expression*, bool> infer_match_partiality(
    const ast::Structure& s);

}  // namespace cppcaml
