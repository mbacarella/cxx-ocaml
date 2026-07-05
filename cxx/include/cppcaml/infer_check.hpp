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
#include <unordered_set>
#include <utility>
#include <vector>

#include "cppcaml/ast.hpp"
#include "cppcaml/infer.hpp"
#include "cppcaml/cmi.hpp"
#include "cppcaml/apply_match.hpp"

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

// Slice 3 dump side-tables computed in ONE inference pass (so the transcriber
// doesn't pay for inference twice): per `match` node, whether it is certainly
// non-exhaustive; and per `Pexp_apply` node, the reconstructed argument slots
// (callee-parameter order, omitted optionals filled with None, labelled args
// reordered) -- present only when the call's args don't already match a plain
// positional pass-through.
struct DumpAux {
  std::unordered_map<const ast::Expression*, bool> match_partial;
  // Partiality of a bare `function .. | ..` (Tfunction_cases), keyed by the
  // Pfunction_cases node pointer (its cases match the single parameter).
  std::unordered_map<const void*, bool> function_cases_partial;
  // Param-pattern node -> is-partial (Param_pat (Partial)), against its type.
  std::unordered_map<const void*, bool> param_partial;
  std::unordered_map<const ast::Expression*, std::vector<applymatch::Slot>> apply_plans;
  // Construct nodes (Pexp_construct / Ppat_construct) whose argument tuple the
  // dump flattens because the resolved constructor has arity>1 (incl. cmi ctors).
  std::unordered_set<const void*> flatten_construct;
  // Functional record-update nodes whose base is an EXTERNAL record type -> its
  // full ordered field list (for the dump's <kept> fields).
  std::unordered_map<const ast::Expression*, std::vector<std::string>> record_fields;
  // String-literal expressions inferred at a printf-family format type (`printf
  // "%d"`, `let f : _ format = "%d"`, ...): the dump desugars them to the
  // CamlinternalFormatBasics.Format(...) tree, exactly as OCaml's type_format.
  std::set<const ast::Expression*> format_lits;
};
DumpAux infer_dump_aux(const ast::Structure& s);

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
  // For a constructor PATTERN whose constructor name is unqualified/unknown but
  // whose inferred (scrutinee-column) type is a module-qualified variant
  // ("Load_path.visibility"): the full path.  The back end registers that type's
  // constructors so an unqualified pattern (`Visible`/`Hidden` matched without an
  // `open Load_path`) resolves -- type-directed constructor disambiguation.
  std::unordered_map<const void*, std::string> pat_constr;
  // A record pattern whose matched value's type resolved to a (possibly
  // qualified) record type, so the back end disambiguates an AMBIGUOUS field
  // (`{ args = .. }` on a `pattern_matching` -- args@1 -- vs `division` -- args@0)
  // by the matched value's type instead of a single-label guess.
  std::unordered_map<const void*, std::string> pat_record_type;
  // Expressions of type `?l:.. -> ..` used where a non-optional arrow is expected:
  // the back end eta-expands them, inserting None for each erased optional.  The
  // bool vector is the resulting application's argument slots (true = None for an
  // erased optional, false = an eta parameter).
  std::unordered_map<const ast::Expression*, std::vector<bool>> optional_erasures;
  // For a record-field PROJECTION (`d.untypables`) whose label is AMBIGUOUS
  // across record types, the field resolved through the base's inferred type
  // IDENTITY (its decl stamp) -- which the back end's by-name `find_field`
  // cannot disambiguate (Sign_diff.t.untypables@4 vs signature_symptom.untypables@8).
  struct FieldResolved { int index; bool mut; std::string kind; };
  std::unordered_map<const ast::Expression*, FieldResolved> field_resolved;
};
ValueKinds infer_value_kinds(const ast::Structure& s);

// Directory holding the compiled stdlib .cmi files the inferencer consults
// (default "stdlib", relative to the CWD -- callers that run from elsewhere,
// like c++ocamlc, must set the discovered absolute path first).
void set_infer_stdlib_dir(const std::string& dir);

// Extra -I dirs the inferencer searches for a separately-compiled local module's
// .cmi (so a dependent gets real types for `A.x`, not Any).
void set_infer_module_dirs(std::vector<std::string> dirs);

// A compilation unit's top-level signature (in source order) as cmiw items --
// the input to cmi::cmiw::write_cmi.  Single-var `let` bindings -> Sig_value
// (matching the .cmo's exported field order); `type` declarations -> Sig_type
// (abstract / manifest; they take no runtime field).  A type using an
// unsupported construct becomes a fresh type variable (opaque but valid).
std::vector<cmi::cmiw::SigItem> infer_signature(const ast::Structure& s);

// Build the .cmi signature from a hand-written interface (.mli).  Unlike
// infer_signature this reads types verbatim from the declarations (no
// inference); it is the path used when an interface file exists.
std::vector<cmi::cmiw::SigItem> signature_to_cmi(
    const ast::Signature& s,
    const std::unordered_map<std::string, const ast::Signature*>* outer = nullptr,
    const std::unordered_map<std::string, const ast::Signature*>* outer_mods = nullptr);

}  // namespace cppcaml
