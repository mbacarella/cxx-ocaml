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
// One parameter position of an optional-erasure eta-expansion (`?x:.. -> y -> ..`
// used where `y -> ..` is expected): erased => filled with None, else an eta
// parameter passed through.  `label` is 0/1/2 (Nolabel/Labelled/Optional).
struct EtaSlot { bool erased; int label; std::string name; };
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
  // `C _` pattern nodes where C resolved to arity N>1 (incl. cmi ctors): the
  // lone `_` fills every slot (N Tpat_any) in the dump.
  std::unordered_map<const void*, int> construct_any_arity;
  // `_ M.t` core-type nodes where M.t is a cmi type of arity N>1: the lone `_`
  // fills every parameter slot (N Ttyp_any) in the dump.
  std::unordered_map<const void*, int> type_any_arity;
  // Functional record-update nodes whose base is an EXTERNAL record type -> its
  // full ordered field list (for the dump's <kept> fields).
  std::unordered_map<const ast::Expression*, std::vector<std::string>> record_fields;
  // Record-construction/update nodes whose type is an EXTERNAL record with a
  // non-default representation (currently: all-`float` fields -> Record_float).
  // Local records carry their repr in the transcriber's own field registry.
  std::unordered_map<const ast::Expression*, std::string> record_reprs;
  // String-literal expressions inferred at a printf-family format type (`printf
  // "%d"`, `let f : _ format = "%d"`, ...): the dump desugars them to the
  // CamlinternalFormatBasics.Format(...) tree, exactly as OCaml's type_format.
  std::set<const ast::Expression*> format_lits;
  // Array literals `[| .. |]` whose expected/inferred type is `iarray` (not
  // `array`): the dump prints them as `Texp_array Immutable`.  Type-directed --
  // `[||]` is polymorphic between array and iarray, resolved by expected type.
  std::set<const ast::Expression*> iarray_lits;
  // Argument expressions of type `?l:.. -> ..` used where a NON-optional arrow is
  // expected: OCaml eta-expands them (`let arg = e in fun eta -> arg ?l:None
  // eta`).  The slot list drives the ghost desugaring in the dump.
  std::unordered_map<const ast::Expression*, std::vector<EtaSlot>> eta_erasures;
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
  // Expressions whose type is an ABSTRACT type constructor (`Id.t`) -- boxed, so
  // kind_str spells them "addr", yet Typeopt.classify treats them as `Any`, a
  // GENERIC array element.  When such an expression is the RESULT of `a.(i)` (or
  // the value stored by `a.(i) <- v`), the back end must emit `array.get[gen]`,
  // not `[addr]`.  Consumed ONLY by array_elem_kind, so it cannot perturb any
  // other value_kind (e.g. the lazy-forward classification).
  std::set<const void*> abstract_elem;
  // For an expression whose type is a module-qualified constr ("Gc.stat"), the
  // full path -- the back end resolves unqualified record labels through the
  // base expression's inferred type (heap_stats.major_collections).
  std::unordered_map<const void*, std::string> expr_constr;
  // Expression nodes the checker typed CONCRETELY as exn.  A match scrutinee in
  // here licenses the back end's nearest-exception reading of a bare ctor name
  // a variant also declares (lambda.cpp's exn_decl_nearest); an annotated or
  // otherwise variant-typed scrutinee never enters, so type-directed
  // disambiguation keeps outranking declaration order.  (The per-PATTERN
  // pat_constr also says "exn" for those -- the pattern's own best-effort
  // resolution -- which is why the scrutinee's type is recorded instead.)
  std::set<const void*> expr_exn;
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
  // A record LITERAL (no `with` base) whose EXPECTED type names a record the
  // literal's label set fills exactly but which is NOT the record its labels
  // resolve to by scope: the expected record's fields in DECLARATION order.
  // Two records sharing a label set (`type p = {a;b}` / `type q = {b;a}`) leave
  // the back end's by-name lookup on the last-declared one, so
  // `rp { a = 1; b = 2 }` built at q's layout.  The ordered labels, not
  // the type name, identify the layout the back end must build at.
  std::unordered_map<const void*, std::vector<std::string>> expr_record_labels;
  // For a polymorphic-variant PATTERN whose unified row type is CLOSED (`[<`
  // upper bound or exact `[ .. ]`): the row's tag-universe size.  The back
  // end's combine_variant port needs it for sig_complete -- a column listing
  // every tag of its row drops the fail action even with default rows pending
  // (typemod's `Ok/`Contains_apply columns).  An open `[>` row can never be
  // complete, so no entry is recorded for it.
  std::unordered_map<const void*, int> pat_pvuniv;
  // Expressions of type `?l:.. -> ..` used where a non-optional arrow is expected:
  // the back end eta-expands them, inserting None for each erased optional.  The
  // bool vector is the resulting application's argument slots (true = None for an
  // erased optional, false = an eta parameter).
  std::unordered_map<const ast::Expression*, std::vector<bool>> optional_erasures;
  // For a record-field PROJECTION (`d.untypables`) whose label is AMBIGUOUS
  // across record types, the field resolved through the base's inferred type
  // IDENTITY (its decl stamp) -- which the back end's by-name `find_field`
  // cannot disambiguate (Sign_diff.t.untypables@4 vs signature_symptom.untypables@8).
  // `unboxed`: the owning record is `[@@unboxed]`, so the projection is the
  // identity -- there is no block to read a field from.
  struct FieldResolved { int index; bool mut; std::string kind; bool unboxed = false; };
  std::unordered_map<const ast::Expression*, FieldResolved> field_resolved;
  // For a comparison primitive used as a first-class VALUE (`let (=) : int -> int
  // -> bool = Stdlib.(=)`), the kind_str of the instantiated FIRST-parameter type
  // ("int"/"float"/"string"/"int32"/"int64"/"nativeint").  The eta-stub then
  // lowers the generic caml_compare/caml_equal/... to the type-specialized
  // opcode (Translprim.specialize_comparison), matching official ocamlc's .cmo.
  std::unordered_map<const void*, std::string> cmp_operand;
  // Match expressions the typer proved NON-exhaustive (Partial).  A match ABSENT
  // from this map (and present in the program) is Total -- the back end may then
  // omit the impossible `raise Match_failure` default (and its final test), exactly
  // as ocamlc does when Translcore passes it `Total`.  Keyed by the Pexp_match /
  // `function` Expression*.
  std::unordered_map<const ast::Expression*, bool> match_partial;
  // Same, for a bare `function ..` (Pfunction_cases), keyed by the node address.
  std::unordered_map<const void*, bool> function_cases_partial;
  // match / function-cases nodes whose Total verdict came from a COMPLETED GADT
  // refutation of every uncovered constructor.  A Total that is merely the
  // conservative default (absent / unknown-scrutinee) is NOT here, and the back
  // end must not route uncovered constructor tags through switch holes on its
  // strength -- only a proven Total carries that license.
  std::set<const void*> total_proven;
};
// `iface_cmi_path`, when non-empty, names this unit's own compiled interface
// (.mli -> .cmi): value-restriction weak vars in exported bindings are pinned
// against it (Includemod's moregeneral), so comparisons over them specialize.
ValueKinds infer_value_kinds(const ast::Structure& s,
                             const std::string& iface_cmi_path = "");

// Directory holding the compiled stdlib .cmi files the inferencer consults
// (default "stdlib", relative to the CWD -- callers that run from elsewhere,
// like c++ocamlc, must set the discovered absolute path first).
void set_infer_stdlib_dir(const std::string& dir);

// Extra -I dirs the inferencer searches for a separately-compiled local module's
// .cmi (so a dependent gets real types for `A.x`, not Any).
void set_infer_module_dirs(std::vector<std::string> dirs);
const std::vector<std::string>& infer_module_dirs();

// Drop the module-name -> .cmi-path memo.  Must run before each compiled unit:
// a unit compiled earlier in the same invocation writes a .cmi that a later
// unit's lookups must see.
void clear_head_cmi_cache();

// A compilation unit's top-level signature (in source order) as cmiw items --
// the input to cmi::cmiw::write_cmi.  Single-var `let` bindings -> Sig_value
// (matching the .cmo's exported field order); `type` declarations -> Sig_type
// (abstract / manifest; they take no runtime field).  A type using an
// unsupported construct becomes a fresh type variable (opaque but valid).
// `fparams` (optional): enclosing functor parameters as (name, param modtype)
// -- bound into the checker so a functor BODY's exports type against them
// (`module F (H : S) = struct let f x = H.g x end` keeps H.g's real type).
std::vector<cmi::cmiw::SigItem> infer_signature(
    const ast::Structure& s,
    const std::vector<std::pair<std::string, const ast::ModuleType*>>* fparams = nullptr);

// Build the .cmi signature from a hand-written interface (.mli).  Unlike
// infer_signature this reads types verbatim from the declarations (no
// inference); it is the path used when an interface file exists.
std::vector<cmi::cmiw::SigItem> signature_to_cmi(
    const ast::Signature& s,
    const std::unordered_map<std::string, const ast::Signature*>* outer = nullptr,
    const std::unordered_map<std::string, const ast::Signature*>* outer_mods = nullptr);

}  // namespace cppcaml
