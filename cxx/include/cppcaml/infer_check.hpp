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

#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cppcaml/ast.hpp"
#include "cppcaml/infer.hpp"
#include "cppcaml/cmi.hpp"

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

// Strict type-check (toward error-rejection parity): the definite type errors in
// a structure; empty => accepted.  Conservative (only certain errors), so the
// false-rejection rate over oracle-accepted files measures engine completeness.
std::vector<std::string> structure_typecheck(const ast::Structure& s);

// For the Lambda back end: inferred value kinds (Lambda's value_kind, as a
// short string "int"/"float"/"int32"/"int64"/"nativeint", or "" for generic),
// keyed by the let/parameter pattern, and by function node for its return kind.
struct ValueKinds {
  std::unordered_map<const ast::Pattern*, std::string> pat;
  std::unordered_map<const void*, std::string> fn_ret;  // keyed by Pexp_function*
  std::unordered_map<const void*, std::string> expr;    // keyed by Expression*
  // String-literal expressions inferred at a format type (Printf/Format/Scanf):
  // the Lambda back end lowers these to a CamlinternalFormatBasics format value.
  std::set<const ast::Expression*> format_lits;
  // For an array-typed expression, the kind_str of its ELEMENT type ("int"/
  // "float"/"addr"/"string"/""), so the back end can annotate Array.length /
  // empty `[||]` with the element kind (which the array's own kind can't give).
  std::unordered_map<const void*, std::string> array_elem;
  // For an expression whose type is a module-qualified constr ("Gc.stat"), the
  // full path -- the back end resolves unqualified record labels through the
  // base expression's inferred type (heap_stats.major_collections).
  std::unordered_map<const void*, std::string> expr_constr;
  // Expressions of type `?l:.. -> ..` used where a non-optional arrow is expected:
  // the back end eta-expands them, inserting None for each erased optional.  The
  // bool vector is the resulting application's argument slots (true = None for an
  // erased optional, false = an eta parameter).
  std::unordered_map<const ast::Expression*, std::vector<bool>> optional_erasures;
};
ValueKinds infer_value_kinds(const ast::Structure& s);

// Directory holding the compiled stdlib .cmi files the inferencer consults
// (default "stdlib", relative to the CWD -- callers that run from elsewhere,
// like c++ocamlc, must set the discovered absolute path first).
void set_infer_stdlib_dir(const std::string& dir);

// Extra -I dirs the inferencer searches for a separately-compiled local module's
// .cmi (so a dependent gets real types for `A.x`, not Any).
void set_infer_module_dirs(std::vector<std::string> dirs);

// A compilation unit's top-level value signature (in source order) as cmiw type
// descriptors, bridged from the inferencer's types -- the input to
// cmi::cmiw::write_cmi.  Only single-variable top-level `let` bindings are
// included (matching the .cmo's exported field order for the common case);
// types using an unsupported construct become a fresh type variable (opaque but
// valid).
std::vector<std::pair<std::string, cmi::cmiw::TyPtr>> infer_signature(
    const ast::Structure& s);

}  // namespace cppcaml
