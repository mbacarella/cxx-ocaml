#include "cppcaml/infer_check.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <unordered_map>

#include "cppcaml/cmi.hpp"

namespace cppcaml {

// Where the stdlib .cmi files live; see set_infer_stdlib_dir below.
static std::string g_stdlib_dir = "stdlib";
static std::vector<std::string> g_infer_module_dirs;  // extra -I dirs for local .cmi
void set_infer_stdlib_dir(const std::string& dir) { g_stdlib_dir = dir; }
void set_infer_module_dirs(std::vector<std::string> dirs) { g_infer_module_dirs = std::move(dirs); }
const std::vector<std::string>& infer_module_dirs() { return g_infer_module_dirs; }

namespace {

namespace I = infer;
using namespace ast;

// Path of a .cmi in the configured stdlib directory.
std::string stdpath(const std::string& file) { return g_stdlib_dir + "/" + file; }
// The .cmi of a head module: the stdlib naming pattern, else (for a separately
// compiled local module like `A`) the first <head>.cmi found in the -I dirs.
std::string head_cmi(const std::string& head) {
  std::string sp = stdpath(head == "Stdlib" ? "stdlib.cmi" : "stdlib__" + head + ".cmi");
  if (std::filesystem::exists(sp)) return sp;
  std::string low = (char)std::tolower((unsigned char)head[0]) + head.substr(1);
  // Some stdlib units (CamlinternalLazy, ...) ship as an unprefixed lower-case cmi.
  std::string lowsp = stdpath(low + ".cmi");
  if (std::filesystem::exists(lowsp)) return lowsp;
  for (const std::string& d : g_infer_module_dirs) {
    if (std::filesystem::exists(d + "/" + low + ".cmi")) return d + "/" + low + ".cmi";
    if (std::filesystem::exists(d + "/" + head + ".cmi")) return d + "/" + head + ".cmi";
  }
  return sp;
}
using I::TypePtr;

std::string lid_last(const Longident& x) {
  if (auto* p = std::get_if<Lident>(&x.v)) return p->name;
  if (auto* p = std::get_if<Ldot>(&x.v)) return p->name;
  return "?";
}
std::string lid_full(const Longident& x) {
  if (auto* p = std::get_if<Lident>(&x.v)) return p->name;
  if (auto* p = std::get_if<Ldot>(&x.v)) return lid_full(*p->prefix) + "." + p->name;
  auto& a = std::get<Lapply>(x.v);
  return lid_full(*a.f) + "(" + lid_full(*a.x) + ")";
}

// Module names removed by a DESTRUCTIVE module substitution in a `<modtype> with
// module X := P` (and `with module type X := ..`): the substituted module loses
// its runtime field.  identifiable.mli's `module Map : Map with module T := T`
// drops the leading `module T`, so the runtime block (Make_map, which has no T)
// and the .cmi agree -- without this they differ by one field and every
// `Numbers.Int.Map.x` reads one slot off.
static std::set<std::string> with_modsubst_names(const ast::ModuleType& mt) {
  std::set<std::string> r;
  const ast::ModuleType* m = &mt;
  while (auto* pw = std::get_if<Pmty_with>(&m->desc)) {
    for (auto& c : pw->constraints)
      if (auto* ms = std::get_if<Pwith_modsubst>(&c)) {
        if (auto* l = std::get_if<Lident>(&ms->lid1.txt.v)) r.insert(l->name);
      } else if (auto* mts = std::get_if<Pwith_modtypesubst>(&c)) {
        if (auto* l = std::get_if<Lident>(&mts->lid.txt.v)) r.insert(l->name);
      }
    m = pw->mt.get();
  }
  return r;
}

// Drop the module/modtype SigItems named by a destructive substitution, so the
// emitted .cmi field layout matches the runtime block.
static void drop_modsubst(std::vector<cmi::cmiw::SigItem>& items,
                          const std::set<std::string>& removed) {
  if (removed.empty()) return;
  items.erase(std::remove_if(items.begin(), items.end(), [&](const cmi::cmiw::SigItem& si) {
    return (si.k == cmi::cmiw::SigItem::Module || si.k == cmi::cmiw::SigItem::Modtype) &&
           removed.count(si.name) > 0;
  }), items.end());
}

// (kind, name) for an AST argument label: 0 Nolabel, 1 Labelled, 2 Optional.
std::pair<int, std::string> arglabel(const ArgLabel& l) {
  if (auto* p = std::get_if<Labelled>(&l)) return {1, p->name};
  if (auto* p = std::get_if<Optional>(&l)) return {2, p->name};
  return {0, ""};
}

// printf-family format types (format / format4 / format6, possibly qualified)
// are mutually-aliased with different arities; normalize them all to a single
// canonical nullary `format6` so they never clash on name or arity.
bool is_format_base(const std::string& path) {
  auto dot = path.rfind('.');
  std::string b = dot == std::string::npos ? path : path.substr(dot + 1);
  return b == "format" || b == "format4" || b == "format6";
}

std::string cmi_path_str(const cmi::Path& p) {
  switch (p.kind) {
    case cmi::Path::Pident: return p.id.name;
    case cmi::Path::Pdot: return (p.a ? cmi_path_str(*p.a) : "?") + "." + p.s;
    case cmi::Path::Papply:
      return (p.a ? cmi_path_str(*p.a) : "?") + "(" +
             (p.b ? cmi_path_str(*p.b) : "?") + ")";
    case cmi::Path::Pextra_ty: return p.a ? cmi_path_str(*p.a) : "?";
  }
  return "?";
}

// The whole checker, holding the engine and environments.  Every inference step
// is best-effort: a TypeError (clash, unbound, unsupported) is caught and the
// node gets a fresh variable, so inference never aborts a file.
struct Checker {
  I::Engine eng;
  // value scopes: name -> scheme (a possibly-generalized type)
  std::vector<std::unordered_map<std::string, TypePtr>> venv{{}};
  // constructor schemes: ctor name -> a chain arg1->..->argN->result (generic)
  std::unordered_map<std::string, TypePtr> ctors;
  // finite variant types: type name -> its full constructor-name set
  std::unordered_map<std::string, std::vector<std::string>> type_ctors;
  // type path -> its ctors' generic schemes (arg1->..->argN->result), kept per
  // type so a later same-named ctor (in the flat `ctors` map, last-wins) can't
  // shadow it -- used to check polyvariant-argument exhaustiveness (compute_partial).
  std::unordered_map<std::string, std::vector<std::pair<std::string, TypePtr>>>
      type_ctor_schemes_;
  // type abbreviations: name -> (param var names, manifest core_type) so that a
  // `type ('a,..) t = <manifest>` can be expanded when t is used in annotations.
  struct Alias { std::vector<std::string> params; const CoreType* manifest;
                 const std::vector<ast::TypeConstraint>* constraints = nullptr;
                 // module-qualified display path for a module-nested alias
                 // (`Buffer.t`), used when the folded pass keeps the name
                 std::string display_path; };
  std::unordered_map<std::string, Alias> type_aliases;
  // stamped (opaque) module-nested decls: stamp -> qualified display path
  std::unordered_map<int, std::string> stamp_path_;
  // module prefixes ("A.") whose contents are re-exported bare by a top-level
  // `include A`: their type names display UNqualified (ocamlc shows the
  // included bare name -- includestruct's `val x : t`).
  std::set<std::string> included_module_prefixes_;
  // GADT type names (a constructor has an explicit result type): matching one
  // refines types per branch, so branch results must not be cross-unified.
  std::set<std::string> gadt_types;
  std::set<std::string> gadt_ctors;  // constructor names belonging to a GADT
  // GADT constructors that introduce an existential (a type var in the args that
  // is absent from the result).  A structure-level `let A x = ..` binding such a
  // constructor lets the existential escape, which OCaml rejects ("Existential
  // types are not allowed in toplevel bindings").
  std::set<std::string> existential_ctors_;
  // `private` types cannot be constructed/mutated through their own
  // constructors/fields (the whole point of a private row is read-only access).
  std::set<std::string> private_variant_ctors_;            // ctors of a private variant
  std::unordered_map<std::string, std::string> private_ctor_type_;   // ctor  -> type name
  std::set<std::string> private_record_fields_;            // fields of a private record
  std::unordered_map<std::string, std::string> private_field_type_;  // field -> type name
  std::set<std::string> nonprivate_record_fields_;         // public-record fields (collision guard)
  // Set while the strict checker visits a `module rec` body (where `strict` is off
  // because the recursion dummies make full inference unsound): re-enables the
  // purely-local record-completeness check, which stays sound under the recursion.
  bool recmod_body_ = false;
  std::unordered_map<std::string, int> type_arity;  // type name -> param count
  // Type identity: each opaque (non-alias) local type declaration gets a unique
  // stamp; tenv is the scoped type-name -> stamp environment (mirrors module
  // scopes), so a shadowed `type t` resolves to the right identity.
  int next_type_stamp_ = 1;
  std::unordered_map<const TypeDeclaration*, int> type_stamp_;
  // Reverse maps for the tuple-GADT exhaustiveness analysis: a stamped opaque
  // decl's AST (constructor/field lists for the mcomp-lite compatibility check)
  // and its key into type_ctor_schemes_ (mod_prefix-qualified name).
  std::unordered_map<int, const TypeDeclaration*> stamp_type_decl_;
  std::unordered_map<int, std::string> stamp_ctor_key_;
  // Qualified opaque-decl name -> stamp (registration-scoped resolution of the
  // bare names inside ctor SCHEMES, which are built before tenv exists), and
  // bare name -> stamp when the bare name is declared exactly once (-1 = dup).
  std::unordered_map<std::string, int> qual_type_stamp_;
  std::unordered_map<std::string, int> bare_unique_stamp_;
  std::vector<std::unordered_map<std::string, int>> tenv{{}};
  // The path prefix of the submodule currently being type-registered (e.g.
  // "Float_record."), so a record type defined there is named `Float_record.s`
  // -- its rendered path, NOT its identity (the stamp is unchanged).
  std::string mod_prefix_;
  // The binding name of a `module M = F(Arg)` currently being elaborated, used to
  // name F's abstract result types `M.t` (so `M.empty : M.t`).
  std::string func_bind_name_;
  // True while translating a functor RESULT signature: from_cmi then keeps the
  // result's abbreviation types abstract+qualified (`elt` -> `IntSet.elt`) rather
  // than expanding their `= Ord.t` manifest.
  bool func_result_mode_ = false;
  int tenv_lookup(const std::string& name) {
    for (auto it = tenv.rbegin(); it != tenv.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return f->second;
    }
    return 0;
  }
  // constructors defined in more than one type: ambiguous without type-directed
  // disambiguation, so treated as unknown (a flat last-wins map picks wrong).
  // (Only consulted as a fallback for ctors not in the scoped cenv below.)
  std::set<std::string> ambiguous_ctors_;
  // Predefined ctor names ([]/::/Some/..) redefined by a TOP-LEVEL decl (which
  // genuinely shadows the predef for the rest of the file).  A module-NESTED
  // redefinition doesn't touch the outer name (OCaml scoping), so a predef name
  // absent here stays resolvable in the non-strict passes even when marked
  // ambiguous (the strict pass keeps bailing to Any: inside the defining module
  // the pick would need type-directed disambiguation we don't have).
  std::set<std::string> predef_toplevel_redef_;
  // Scoped, ordered constructor resolution (mirrors tenv for types): a ctor name
  // resolves to the in-scope declaration, so a name reused across several local
  // types is disambiguated by position instead of collapsed to ambiguous.
  std::unordered_map<const ConstructorDecl*, TypePtr> ctor_scheme_;
  std::vector<std::unordered_map<std::string, TypePtr>> cenv{{}};
  // Module aliases `module MP = Gc.Memprof`: (target-path, alias-name).  ocamlc
  // keeps the alias in displayed type paths (`MP.t`, not `Gc.Memprof.t`), so the
  // signature emitter rewrites the target prefix back to the alias.  Display-only
  // (same last path component, so unification is untouched).
  std::vector<std::pair<std::string, std::string>> module_aliases_;
  // Expression-local modules (`let module N = Map.Make(S) in ..`) whose name
  // escapes into an emitted signature: the local name is out of scope there, so
  // ocamlc prints the module's DEFINITION path with arguments resolved through
  // local aliases (`int Map.Make(String).t`, pr6944).  name -> resolved path;
  // "" = unresolvable or conflictingly rebound (don't rewrite).  The signature
  // emitter skips names also bound by a top-level module (those stay in scope).
  std::unordered_map<std::string, std::string> local_module_paths_;
  // The definition path of a local module expr: an ident (head resolved through
  // earlier local bindings) or a functor application F(A) of such.
  std::string resolve_local_module_path(const ModuleExpr& me0) {
    const ModuleExpr* me = &me0;
    while (auto* mc = std::get_if<Pmod_constraint>(&me->desc)) me = mc->me.get();
    if (auto* pi = std::get_if<Pmod_ident>(&me->desc)) {
      std::string p = lid_full(pi->id.txt);
      size_t dot = p.find('.');
      std::string head = p.substr(0, dot == std::string::npos ? p.size() : dot);
      if (auto f = local_module_paths_.find(head); f != local_module_paths_.end())
        return f->second.empty()
                   ? ""
                   : f->second + (dot == std::string::npos ? "" : p.substr(dot));
      return p;
    }
    if (auto* ap = std::get_if<Pmod_apply>(&me->desc)) {
      std::string f = resolve_local_module_path(*ap->f);
      std::string a = resolve_local_module_path(*ap->arg);
      return (f.empty() || a.empty()) ? "" : f + "(" + a + ")";
    }
    return "";
  }
  // names that are also predefined or exception constructors: when one of these
  // is reused by a variant, OCaml disambiguates by expected type (which we lack),
  // so we keep them unknown rather than resolve to the wrong kind.
  std::set<std::string> predef_ctors_;
  std::set<std::string> exn_ctors_;
  std::set<const void*> ext_rebind_registered_;  // resolved Pext_rebind ctors
  // Exception/typext AST nodes already registered, so the flat `ctors` map isn't
  // re-populated (re-registration would spuriously mark the name ambiguous).  The
  // top-level register_types_rec pass and the per-item process_item path can both
  // reach the same node; register once.
  std::unordered_set<const void*> ext_ctor_registered_;
  // For the Lambda back end: record inferred types of let/param patterns and
  // function bodies, so value kinds can be read off after inference (additive;
  // off by default so the soundness/completeness passes are unaffected).
  bool record_kinds_ = false;
  // Record format-string literals into fmt_lits_ (for the dump).  Purely
  // additive -- gated separately from record_kinds_ so the dump pass can collect
  // formats without also flipping on the heavier kind-recording behaviour.
  bool record_fmt_lits_ = false;
  std::unordered_map<const Pattern*, TypePtr> rec_pat_;
  std::unordered_map<const void*, TypePtr> rec_ret_;
  std::unordered_map<const void*, TypePtr> rec_expr_;  // every expression's type
  // Record decl by identity stamp, for resolving an AMBIGUOUS field projection
  // through the base's inferred type identity (Sign_diff.t.untypables@4).
  std::unordered_map<int, const TypeDeclaration*> stamp_record_decl_;
  std::unordered_map<std::string, const TypeDeclaration*> name_record_decl_;
  std::set<std::string> ambiguous_record_names_;  // a record name declared by >1 decl
  // Field projections so resolved: node -> (index, mut, kind_str).
  std::unordered_map<const Expression*, std::tuple<int, bool, std::string>> field_resolved_;
  // Ambiguous field accesses (base expr type + label), resolved AFTER inference
  // reaches a fixpoint: an unannotated record param's type is often pinned only by a
  // LATER field read (`pool.level` typed before `pool.next` proves `pool` is the pool
  // record), so resolving at the read site sees a free var.  The base TypePtr is
  // mutable (union-find), so its repr at the end reflects all constraints.
  std::vector<std::tuple<const Expression*, TypePtr, std::string>> pending_field_;
  void resolve_pending_fields() {
    for (auto& [e, bt, lbl] : pending_field_) {
      if (field_resolved_.count(e)) continue;
      TypePtr rb = I::Engine::repr(bt);
      if (rb->kind != I::Type::Kind::Constr) continue;
      // A stamped record disambiguates exactly; a stamp-LESS record Constr (a
      // parametrized local record like `('a,'b) pattern_matching` whose head
      // resolved but never got a stamp) falls back to the unique by-name decl.
      const TypeDeclaration* decl = nullptr;
      if (rb->stamp) { auto it = stamp_record_decl_.find(rb->stamp);
                       if (it != stamp_record_decl_.end()) decl = it->second; }
      if (!decl && !ambiguous_record_names_.count(rb->path)) {
        auto it = name_record_decl_.find(rb->path);
        if (it != name_record_decl_.end()) decl = it->second;
      }
      if (!decl) continue;
      auto* rec = std::get_if<Ptype_record>(&decl->kind);
      if (!rec) continue;
      bool all_float = !rec->fields.empty();
      for (auto& f : rec->fields)
        if (ct_kind(*f.type) != "float") { all_float = false; break; }
      if (all_float) continue;
      for (int i = 0; i < (int)rec->fields.size(); ++i)
        if (rec->fields[i].name.txt == lbl) {
          field_resolved_[e] = {i, rec->fields[i].mut == MutableFlag::Mutable,
                                ct_kind(*rec->fields[i].type)};
          break;
        }
    }
  }
  std::set<const Expression*> fmt_lits_;  // string literals inferred at format type
  std::set<const Expression*> iarray_lits_;  // `[|..|]` expected at an iarray type
  // An array literal `[|..|]` (possibly under `: t` constraints) whose EXPECTED
  // type is `iarray` prints as `Texp_array Immutable`.  Dump-only recording.
  void mark_if_iarray(const Expression& e, const TypePtr& expected) {
    if (!record_kinds_ && !record_fmt_lits_) return;
    TypePtr er = I::Engine::repr(expected);
    if (er->kind != I::Type::Kind::Constr) return;
    auto d = er->path.rfind('.');
    std::string b = d == std::string::npos ? er->path : er->path.substr(d + 1);
    if (b != "iarray") return;
    const Expression* inner = &e;
    while (auto* c = std::get_if<Pexp_constraint>(&inner->desc)) inner = c->e.get();
    if (std::holds_alternative<Pexp_array>(inner->desc)) iarray_lits_.insert(inner);
  }
  // Optional-argument erasure: an expression of type `?l:.. -> ..` used where a
  // non-optional arrow is expected is eta-expanded with None for each erased
  // optional.  The bool vector is the application's argument slots in order
  // (true = a None for an erased optional, false = an eta-expansion parameter);
  // the Lambda back end builds `(let (arg = e) (function eta.. (apply arg ..)))`.
  std::unordered_map<const Expression*, std::vector<bool>> erasures_;
  // As erasures_ but with each slot's label/name, for the dump's eta-expansion.
  std::unordered_map<const Expression*, std::vector<EtaSlot>> eta_erasures_;
  // Local variant types whose constructors are all constant (nullary): these have
  // an immediate (int) runtime representation, so a value of such a type gets the
  // [int] value kind in the Lambda dump.
  std::set<std::string> immediate_types_;
  // record fields with a UNIQUE label across all record types: label -> generic
  // scheme arrow(recordType, fieldType).  Ambiguous labels are omitted (type-
  // directed disambiguation needed) and left to Any, so this can't pick wrong.
  std::unordered_map<std::string, TypePtr> fields_;
  // Record-field schemes loaded from OPENED external modules (e.g. `open
  // Effect.Deep` brings the `handler` record's retc/exnc/effc fields).  A pure
  // FALLBACK consulted only when fields_ misses, so it can't disturb local
  // record uniqueness; used (uniquely) per label.  label -> candidate schemes.
  std::unordered_map<std::string, std::vector<TypePtr>> ext_fields_;
  // The scheme for a record field: a local unique field, else a unique external
  // (opened-module) field; null if unknown/ambiguous.
  TypePtr field_scheme(const std::string& label) {
    auto it = fields_.find(label);
    if (it != fields_.end()) return it->second;
    auto e = ext_fields_.find(label);
    if (e != ext_fields_.end() && e->second.size() == 1) return e->second[0];
    return nullptr;
  }
  std::unordered_map<std::string, std::vector<TypePtr>> field_candidates_;
  // A polymorphic field (`{ pf : 'a. .. }`) gets no monomorphic value scheme (it
  // would clash across uses), so it never lands in `fields_`.  But a record
  // PATTERN `{pf}` still identifies its record TYPE by that label -- so record the
  // owning record type per poly-field label (unique labels only, in
  // finalize_fields), letting the pattern resolve to `pf` while its bound field
  // stays polymorphic (Any).  label -> record type candidates.
  struct PolyField { TypePtr recTy; const CoreType* ftype; };
  std::unordered_map<std::string, std::vector<PolyField>> poly_field_rec_candidates_;
  std::unordered_map<std::string, PolyField> poly_field_rec_;
  // locally-abstract types `(type a)`: bound to a fresh (flexible) var so that
  // annotations mentioning `a` unify rather than clashing as an opaque constr.
  std::unordered_map<std::string, TypePtr> newtype_vars;
  // The binding for one locally-abstract type.  The DISPLAY pass binds a RIGID
  // node (ocamlc's newtype model): a GADT arm's equation `a = int` is then a
  // lenient constr mismatch that never LEAKS into the signature, while the
  // arm's ordinary unifications ('b := int) persist -- no trail window needed.
  // show prints a rigid node as a type variable ('a), its generalized face.
  // The strict and value-kind passes keep the flexible var (the reject pass
  // needs the equations to type arm bodies; the kind pass needs their kinds).
  TypePtr newtype_binding() {
    if (fold_abbrevs_ && !strict) {
      TypePtr t = eng.constr("");
      t->rigid = true;
      // Creation level: when the binding that scopes this newtype generalizes
      // (level popped below this), the rigid node becomes a generic VAR.
      t->level = eng.level;
      return t;
    }
    return eng.fresh_var();
  }
  // Named type variables (`'a`, `'b`) are shared across ALL annotations of a
  // single binding: `let f (x : 'a) (y : 'a) : 'a list = ..` ties the two params
  // and the return to ONE `'a` (OCaml's structure-item variable scoping).  Set to
  // a fresh map per binding group (infer_bindings); nullptr otherwise (each
  // annotation then mints its own vars, e.g. a stray `(e : 'a)`).  When non-null,
  // Ppat_constraint / the function return-type / the binding constraint all route
  // their from_coretype through it.
  std::unordered_map<std::string, TypePtr>* annot_vars_ = nullptr;
  std::set<std::string> expanding_;  // guard against cyclic abbreviations
  // match-expression node -> is-partial (the result we route back to the dump)
  std::unordered_map<const Expression*, bool> match_partial;
  // Pfunction_cases node -> is-partial (bare `function ..`; for the dump)
  std::unordered_map<const void*, bool> function_cases_partial;
  // Param-pattern node -> is-partial (Param_pat (Partial)); against the type
  std::unordered_map<const void*, bool> param_partial;
  // Pexp_apply node -> reconstructed argument slots (callee-param order, omitted
  // optionals filled), for the dump.  Only stored when non-trivial (see infer_apply).
  std::unordered_map<const Expression*, std::vector<applymatch::Slot>> apply_plans;
  // Pexp_construct / Ppat_construct nodes whose resolved constructor has arity>1
  // and is applied to a matching tuple -> the dump flattens the tuple into the
  // constructor's arguments.  Covers cmi constructors the transcriber can't see.
  std::unordered_set<const void*> flatten_construct;
  std::unordered_map<const void*, int> construct_any_arity;  // `C _`, C arity N>1
  std::unordered_map<const void*, int> type_any_arity;       // `_ M.t`, t arity N>1
  // Functional record-update nodes (`{ ext_record with .. }`) whose base resolves
  // to an EXTERNAL record type -> its full ordered field list, so the dump can
  // emit the omitted fields as <kept> (the transcriber's registry has only local
  // records).  Keyed by the Pexp_record node.
  std::unordered_map<const Expression*, std::vector<std::string>> record_fields;
  // Parallel to record_fields: an EXTERNAL record's non-default representation
  // (Record_float for all-float fields); absent => Record_regular.
  std::unordered_map<const Expression*, std::string> record_reprs;
  // local module name -> its exported value schemes (so open/include/M.x resolve)
  std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>> modenv;
  // local functor name -> its body's exported value schemes (F(X) result)
  std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>> functor_env;
  // A LOCAL functor `module F (P : PS) : RS = ..` with an explicit result
  // signature RS: enough to INSTANTIATE the result on application `F(Arg)` by
  // substituting the parameter's types (`P.t`) with the argument's, instead of
  // collapsing the result to generic vars.  param = P, param_sig = PS, result =
  // RS.  (Non-strict passes only.)
  struct FunctorDef { std::string param; const ast::ModuleType* param_sig = nullptr;
                      const ast::ModuleType* result_sig = nullptr; };
  std::unordered_map<std::string, FunctorDef> functor_defs_;
  // A local functor's fully-unwrapped body expression (constraints and functor
  // params stripped), for resolving applications through the body's own head.
  std::unordered_map<std::string, const ModuleExpr*> functor_body_exprs_;
  // Ascription signature of a top-level `module M : sig .. end = ..`, so an
  // application of a functor DECLARED IN that signature (`Msg.Define(struct ..)`)
  // can be instantiated from its declared functor type.
  std::unordered_map<std::string, const ast::Signature*> module_sig_asts_;
  // Signature AST of a local `module type S = sig .. end`, so a first-class-
  // module param `(module M : S)` can bind M's values at M-qualified types.
  std::unordered_map<std::string, const ast::Signature*> modtype_sig_asts_;
  // Structure AST of a local `let module M = struct .. end`, so packing M
  // against a modtype can resolve the sig's abstract types from M's own
  // manifests (`type t1 = s1` -> the sig's t1 IS s1, not an opaque M.t1).
  std::unordered_map<std::string, const ast::Structure*> local_module_structs_;
  // Bind a signature's typext ctors (`type t += E [of u]`) into the innermost
  // cenv scope, translating under the current substitution context.  Scoped
  // (not the flat map): a second flat registration of an already-registered
  // name would only mark it ambiguous.
  void bind_sig_typext_ctors(const ast::Signature& items) {
    for (auto& it : items)
      if (auto* tx = std::get_if<Psig_typext>(&it.desc))
        for (auto& ec : tx->ext.ctors)
          if (auto* d = std::get_if<Pext_decl>(&ec.kind)) {
            std::unordered_map<std::string, TypePtr> vars;
            TypePtr result;
            if (d->res) result = from_coretype(**d->res, vars);
            else {
              std::vector<TypePtr> params;
              for (auto& p : tx->ext.params) params.push_back(from_coretype(*p, vars));
              result = eng.constr(lid_last(tx->ext.path.txt), params);
            }
            TypePtr scheme = result;
            if (auto* tup = std::get_if<Pcstr_tuple>(&d->args))
              for (auto it2 = tup->elems.rbegin(); it2 != tup->elems.rend(); ++it2)
                scheme = eng.arrow(from_coretype(**it2, vars), scheme);
            cenv.back()[ec.name.txt] = scheme;
          }
  }
  // Value schemes of modtype S's signature, with S's own type names qualified
  // as `M.<name>` (the unpack param's view of its abstract types).
  std::unordered_map<std::string, TypePtr> unpack_module_values(
      const std::string& mod_name, const ast::Signature& items,
      const std::unordered_map<std::string, TypePtr>* argtypes = nullptr) {
    std::unordered_map<std::string, TypePtr> out;
    auto saved = functor_result_abstract_;
    for (auto& it : items)
      if (auto* pt = std::get_if<Psig_type>(&it.desc))
        for (auto& d : pt->decls) {
          // a `with type t = s` constraint substitutes; other abstract types
          // are M-qualified
          const TypePtr* sub = nullptr;
          if (argtypes)
            if (auto a = argtypes->find(d.name.txt); a != argtypes->end())
              sub = &a->second;
          functor_result_abstract_[d.name.txt] =
              sub ? *sub : eng.constr(mod_name + "." + d.name.txt);
        }
    for (auto& it : items)
      if (auto* pv = std::get_if<Psig_value>(&it.desc)) {
        std::unordered_map<std::string, TypePtr> vars;
        out[pv->vd.name.txt] = from_coretype(*pv->vd.type, vars);
      }
    functor_result_abstract_ = std::move(saved);
    return out;
  }
  std::unordered_map<std::string, TypePtr> functor_param_subst_;     // "Elem.t" -> arg type
  std::unordered_map<std::string, TypePtr> functor_result_abstract_; // RS's bare "t" -> fresh var
  std::unordered_map<std::string, TypePtr> cmi_abstract_subst_;       // a cmi modtype's "t" -> arg type
  // A parameterless class's object type, so `new c` yields it (non-strict only).
  std::unordered_map<std::string, TypePtr> class_types_;
  // Class CONSTRUCTOR schemes (`new c` for a class with params): the arrow
  // over the constructor's value params to the class's object type (mixin2).
  std::unordered_map<std::string, TypePtr> class_ctor_types_;
  // Each class's instance-variable types (name -> type), so `inherit P` brings
  // P's vals into the subclass body (woodyatt: charlie inherits bravo's `y`).
  std::unordered_map<std::string, std::vector<std::pair<std::string, TypePtr>>>
      class_instvars_;
  // `class type ['a,'b] ops = object method m : T .. end`: params + the
  // signature AST, so a `(T1,T2) #ops` annotation can build the object row
  // with params substituted (mixin3's self coercions).
  struct ClassTypeInfo {
    std::vector<std::string> params;  // "" for a non-var param
    const ast::ClassSignature* sig = nullptr;
  };
  std::unordered_map<std::string, ClassTypeInfo> classtype_decls_;
  // True while from_coretype expands a local decl's MANIFEST (variant alias
  // fold / `#t` rows): its internal constrs are expansion nodes, not
  // source-written -- they stay adoptable (no family-head finalization).
  // Only honoured under adoptable_annot_ (a FUNCTION-constraint translation,
  // `: var -> _`): ocamlc adopts through those (subst_var's `Var slot takes
  // Subst.key) but keeps the decl face for a PARAM annotation (`(v : var)`
  // stays `Var of string).
  bool manifest_expansion_ = false;
  bool adoptable_annot_ = false;
  // local module-type name -> its signature's value names (first-class modules):
  // (val e : S) unpacks bring S's values into scope.
  std::unordered_map<std::string, std::vector<std::string>> modtype_env;
  // When loading a module's values from a cmi, its own type abbreviations so
  // from_cmi can expand them (e.g. Float.t = float, so `min : t -> t -> t`
  // becomes float -> float -> float instead of clashing t vs float).
  const std::vector<cmi::TypeDecl>* cmi_types_ctx_ = nullptr;
  std::set<std::string> cmi_expanding_;
  // The module path owning cmi_types_ctx_: a Pident type constr in a cmi names a
  // same-unit type, so qualify it ("stat" in gc.cmi -> "Gc.stat") -- unification
  // compares last components, and consumers (record-label resolution) need the
  // module.  Predefs aren't in the signature's type list and stay bare.
  std::string cmi_mod_prefix_;
  // Enclosing cmi module scopes (outermost..innermost) for qualifying a Pident
  // type that lives in a PARENT module: `Array1.create`'s `kind` is
  // `Bigarray.kind`, not `Bigarray.Array1.kind`.  Each entry is (types, prefix).
  // When set, the qualification step searches innermost->outermost; empty falls
  // back to the single (cmi_types_ctx_, cmi_mod_prefix_) above.
  std::vector<std::pair<const std::vector<cmi::TypeDecl>*, std::string>> cmi_scopes_;
  // Parallel to cmi_scopes_, the SUBMODULE lists per scope, for qualifying a Pdot
  // type whose head is a submodule of the value's owning module: a top-level
  // `Bigarray.reshape` returns `Genarray.t` (Ldot in the cmi) which must display
  // as `Bigarray.Genarray.t`.  Searched innermost->outermost; empty => no change.
  std::vector<std::pair<const std::vector<cmi::ModuleDecl>*, std::string>> cmi_mod_scopes_;
  // Stdlib value schemes (loaded once, lazily).
  bool stdlib_ready_ = false;
  std::unordered_map<std::string, TypePtr> stdlib_;
  // Strict mode: record definite type errors instead of swallowing them.
  bool strict = false;
  // --infer signature DISPLAY pass: keep type abbreviations FOLDED (don't expand
  // `Float.t`/`String.t`/`int Seq.t` to their manifest), matching ocamlc's
  // printed signatures.  Safe only because this pass runs with lenient unify (a
  // `Float.t` vs `float` clash from `+.` is swallowed, the annotation keeps its
  // name).  NOT set in value-kinds (which needs the expanded arrow to apply a
  // `Seq.t` as a function, the float kind, etc.) or strict.
  bool fold_abbrevs_ = false;
  // The .cmi VERBATIM path (signature_to_cmi): keep a same-signature
  // abbreviation as its written name (`val make : float -> t` stores the
  // local `t`, like ocamlc) instead of expanding its manifest.
  bool keep_local_abbrevs_ = false;
  std::vector<std::string> errors;
  int cur_line_ = 0;  // line of the expression currently being inferred (for diagnostics)
  void note_error(const std::string& m) {
    if (errors.size() < 100)
      errors.push_back(cur_line_ ? ("L" + std::to_string(cur_line_) + ": " + m) : m);
  }

  TypePtr generic_var() {
    auto v = eng.fresh_var();
    v->level = I::GENERIC_LEVEL;
    return v;
  }

  // cmi type graph -> infer scheme (cmi vars become generic).
  TypePtr from_cmi(const cmi::TypePtr& t0,
                   std::unordered_map<cmi::TypeExpr*, TypePtr>& memo) {
    const cmi::TypeExpr* n = t0.get();
    // follow links
    while (n && (n->kind == cmi::TypeExpr::Tlink || n->kind == cmi::TypeExpr::Tsubst))
      n = n->link.get();
    if (!n) return generic_var();
    switch (n->kind) {
      case cmi::TypeExpr::Tvar:
      case cmi::TypeExpr::Tunivar: {
        auto it = memo.find(const_cast<cmi::TypeExpr*>(n));
        if (it != memo.end()) return it->second;
        auto v = generic_var();
        memo[const_cast<cmi::TypeExpr*>(n)] = v;
        return v;
      }
      case cmi::TypeExpr::Tarrow:
        return eng.arrow(from_cmi(n->dom, memo), from_cmi(n->cod, memo),
                         n->label_kind, n->label);
      case cmi::TypeExpr::Ttuple: {
        std::vector<TypePtr> es;
        for (auto& e : n->elems) es.push_back(from_cmi(e.second, memo));
        return eng.tuple(std::move(es));
      }
      case cmi::TypeExpr::Tconstr:
      case cmi::TypeExpr::Texpand: {
        std::string p = n->path ? cmi_path_str(*n->path) : "?";
        // Substituting a cmi modtype's abstract type (OrderedType's `t`) with the
        // functor argument's type during param-signature value loading.
        if (!cmi_abstract_subst_.empty() && n->path && n->path->kind == cmi::Path::Pident)
          if (auto s = cmi_abstract_subst_.find(n->path->id.name); s != cmi_abstract_subst_.end())
            return s->second;
        if (is_format_base(p)) { std::vector<TypePtr> fa; for (auto& a : n->args) fa.push_back(from_cmi(a, memo)); return eng.constr("format6", std::move(fa)); }
        // expand a same-module type abbreviation (Float.t = float, Int.t = int) --
        // but NOT in a functor result, where `elt = Ord.t` stays the abstract,
        // binding-qualified name (`IntSet.elt`), not its expansion.
        if (!fold_abbrevs_ &&
            !func_result_mode_ && cmi_types_ctx_ && n->path && n->path->kind == cmi::Path::Pident &&
            !cmi_expanding_.count(n->path->id.name))
          for (auto& td : *cmi_types_ctx_)
            if (td.name == n->path->id.name && td.manifest &&
                // Keep an EXTENSIBLE abbreviation as its own name, not its
                // manifest: `Effect.t = 'a eff = ..` prints `Effect.t`, not the
                // builtin `eff` (and `eff` would not unify with a typext's bare
                // `t`, leaving `perform (Set x)` polymorphic instead of unit).
                td.kind != cmi::TypeDecl::Open &&
                td.params.size() == n->args.size()) {
              std::unordered_map<cmi::TypeExpr*, TypePtr> m2;
              for (size_t i = 0; i < td.params.size(); ++i)
                m2[td.params[i].get()] = from_cmi(n->args[i], memo);
              cmi_expanding_.insert(td.name);
              TypePtr r = from_cmi(td.manifest, m2);
              cmi_expanding_.erase(td.name);
              return r;
            }
        // qualify a same-unit (Pident) type with its owning module.  Search the
        // enclosing scopes innermost->outermost so a parent-module type resolves
        // to its OWN module (`kind` in `Array1.create` -> `Bigarray.kind`).
        if (n->path && n->path->kind == cmi::Path::Pident) {
          if (!cmi_scopes_.empty()) {
            for (auto it = cmi_scopes_.rbegin(); it != cmi_scopes_.rend(); ++it) {
              bool found = false;
              for (auto& td : *it->first)
                if (td.name == n->path->id.name) { p = it->second + "." + p; found = true; break; }
              if (found) break;
            }
          } else if (cmi_types_ctx_ && !cmi_mod_prefix_.empty()) {
            for (auto& td : *cmi_types_ctx_)
              if (td.name == n->path->id.name) { p = cmi_mod_prefix_ + "." + p; break; }
          }
        }
        // Qualify a Pdot type whose HEAD is a submodule of the value's owning
        // module: `Bigarray.reshape`'s result `Genarray.t` -> `Bigarray.Genarray.t`.
        if (n->path && n->path->kind == cmi::Path::Pdot && !cmi_mod_scopes_.empty()) {
          const cmi::Path* h = n->path.get();
          while (h->kind == cmi::Path::Pdot && h->a) h = h->a.get();
          if (h->kind == cmi::Path::Pident) {
            const std::string& head = h->id.name;
            for (auto it = cmi_mod_scopes_.rbegin(); it != cmi_mod_scopes_.rend(); ++it) {
              bool found = false;
              for (auto& mm : *it->first)
                if (mm.name == head) { p = it->second + "." + p; found = true; break; }
              if (found) break;
            }
          }
        }
        std::vector<TypePtr> as;
        for (auto& a : n->args) as.push_back(from_cmi(a, memo));
        TypePtr r = eng.constr(std::move(p), std::move(as));
        // A functor-result type WITH a manifest (`type key = K.t` in
        // Ephemeron.K1.Make's result -> `HW.key`) is a transparent
        // functor-instance abbreviation: TOP priority in unify's family relink
        // (an int64/Int64.t-typed value used with `HW.mem` displays HW.key --
        // ocamlc's per-occurrence access path).  Abstract result types
        // (`'a HW.t`) stay unmarked: genuinely distinct.
        if (func_result_mode_ && r->args.empty() && cmi_types_ctx_ &&
            n->path && n->path->kind == cmi::Path::Pident)
          for (auto& td : *cmi_types_ctx_)
            if (td.name == n->path->id.name) {
              if (td.manifest && td.kind != cmi::TypeDecl::Open)
                r->functor_abbrev = true;
              break;
            }
        return r;
      }
      case cmi::TypeExpr::Tpoly:
        return from_cmi(n->link, memo);
      case cmi::TypeExpr::Tobject: {
        // The reader keeps no field structure, but the OPEN empty object
        // (`< .. >`, Oo.id's parameter) still displays faithfully as an open
        // Object row.  Memoized so a shared `(< .. > as 'a) -> 'a` scheme
        // keeps its sharing.  Non-strict only (the strict pass never reasons
        // about Object nodes -- keep its generic var).
        if (strict) return generic_var();
        auto it = memo.find(const_cast<cmi::TypeExpr*>(n));
        if (it != memo.end()) return it->second;
        TypePtr o = eng.object_type({}, {});
        o->variant_kind = 1;  // open row marker
        o->level = I::GENERIC_LEVEL;
        memo[const_cast<cmi::TypeExpr*>(n)] = o;
        return o;
      }
      default:
        return generic_var();  // object/variant/package/etc: unknown for now
    }
  }

  // ast core_type -> infer type, mapping type variables by name (generic).
  // A coretype's value kind for a record-field read op: "int" (immediate),
  // "float", or "" (boxed/generic).  Enough to pick FieldInt/FieldImm/FieldMut.
  std::string ct_kind(const CoreType& t) {
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      std::string b = lid_last(c->id.txt);
      if (b == "int" || b == "char" || b == "bool" || b == "unit") return "int";
      if (immediate_types_.count(b) || immediate_types_.count(lid_full(c->id.txt)))
        return "int";
      if (b == "float") return "float";
    }
    return "";
  }
  // A first-class-module type `(module S)`: a constr whose path renders verbatim
  // (constraints `with type ..` are dropped -- best effort).  A named unpack
  // param `(module M : S)` stores M in the constr's abbrev; show prints
  // `(module M : S)` when the displayed type depends on M (modular explicits).
  TypePtr package_type(const Ptyp_package& pk, const std::string& mod_name = "",
                       std::unordered_map<std::string, TypePtr>* vars = nullptr) {
    TypePtr t = eng.constr("(module " + lid_full(pk.path.txt) + ")");
    t->abbrev = mod_name;
    // `with type t = u` constraints ride as labels/args (display + tying).
    // Non-strict only: the strict pass keeps the bare constr, so an
    // occurrence written without the constraint can't arity-clash.  The
    // caller's vars map (when given) ties a constraint's `'a` to the
    // enclosing declaration's (`Pair of (module PAIR with type t = 'a)`).
    if (!strict)
      for (auto& [lid, ct] : pk.constraints) {
        std::unordered_map<std::string, TypePtr> local;
        t->labels.push_back(lid_full(lid.txt));
        t->args.push_back(from_coretype(*ct, vars ? *vars : local));
      }
    return t;
  }

  TypePtr from_coretype(const CoreType& t,
                        std::unordered_map<std::string, TypePtr>& vars) {
    if (std::holds_alternative<Ptyp_any>(t.desc)) return eng.fresh_var();
    if (auto* v = std::get_if<Ptyp_var>(&t.desc)) {
      auto it = vars.find(v->name);
      if (it != vars.end()) return it->second;
      auto g = generic_var();
      vars[v->name] = g;
      return g;
    }
    if (auto* a = std::get_if<Ptyp_arrow>(&t.desc)) {
      int lk = std::holds_alternative<Labelled>(a->label) ? 1
               : std::holds_alternative<Optional>(a->label) ? 2 : 0;
      std::string nm = lk == 1 ? std::get<Labelled>(a->label).name
                       : lk == 2 ? std::get<Optional>(a->label).name : "";
      return eng.arrow(from_coretype(*a->dom, vars), from_coretype(*a->cod, vars), lk, nm);
    }
    if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
      std::vector<TypePtr> es;
      for (auto& e : tu->elems) es.push_back(from_coretype(*e, vars));
      return eng.tuple(std::move(es));
    }
    // `'a 'b. t` (method poly types): the quantified body, vars as fresh.
    if (auto* pl = std::get_if<Ptyp_poly>(&t.desc))
      return from_coretype(*pl->type, vars);
    // `(t as 'a)`: the inner type, with 'a bound to it for later references.
    if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) {
      TypePtr inner = from_coretype(*al->type, vars);
      vars[al->name] = inner;
      return inner;
    }
    if (auto* pvr = std::get_if<Ptyp_variant>(&t.desc)) {
      // Build the row from a `[ .. ]` / `[< .. ]` / `[> .. ]` annotation.  Kind:
      // Open -> `[>` (0); Closed with present-tags (`[< .. > l]` / `[< ..]`) -> `[<`
      // (1); Closed exact `[ .. ]` -> exact (2).  Non-strict only (rows are a
      // non-strict feature; the strict pass keeps the immediate-int shortcut).
      // Accept rows built from explicit tags (`Rtag`) and/or inherited row types
      // (`Rinherit`, e.g. `[< int u]` -- the inherited type's tags form the allowed
      // bound, kept unexpanded for display).  Non-strict only.
      bool simple = !pvr->rows.empty();
      if (!strict && simple) {
        // A SINGLE inherit of a known local variant alias (`[> 'a lambda]`)
        // is that abbreviation's expansion at the written bound: tags come
        // from the alias (so merges union tag-wise and keep the covering
        // name), from_inherit marks the `[ | 'a lambda ]` display form for
        // an exact fixpoint (mixin2/mixin3).
        if (fold_abbrevs_ && pvr->rows.size() == 1)
          if (auto* ri0 = std::get_if<Rinherit>(&pvr->rows[0])) {
            TypePtr ex = I::Engine::repr(from_coretype(*ri0->ct, vars));
            if (ex->kind == I::Type::Kind::Variant && !ex->abbrev.empty()) {
              ex->variant_kind =
                  pvr->closed == ClosedFlag::Open ? 0 : (pvr->labels ? 1 : 2);
              ex->from_inherit = true;
              return ex;
            }
          }
        // ALL-inherit rows of known variant aliases (`[ 'a lambda | 'a expr ]`
        // -- lexpr's manifest): expand to the TAG UNION under one vars map, so
        // a use ties the shared param through the tags (`#lambda as x` in
        // lexpr_ops reaches lexpr's 'a).  A shared tag's args unify.
        if (fold_abbrevs_ && pvr->rows.size() > 1) {
          bool all_inh = true;
          std::vector<TypePtr> exps;
          for (auto& r : pvr->rows) {
            auto* ri = std::get_if<Rinherit>(&r);
            if (!ri) { all_inh = false; break; }
            TypePtr ex = I::Engine::repr(from_coretype(*ri->ct, vars));
            if (ex->kind != I::Type::Kind::Variant || ex->labels.empty()) {
              all_inh = false;
              break;
            }
            exps.push_back(ex);
          }
          if (all_inh) {
            std::vector<std::string> ut; std::vector<TypePtr> ua; std::vector<char> uh;
            for (auto& ex : exps)
              for (size_t i = 0; i < ex->labels.size(); ++i) {
                size_t k = 0;
                for (; k < ut.size(); ++k) if (ut[k] == ex->labels[i]) break;
                if (k < ut.size()) soft_unify(ua[k], ex->args[i]);
                else { ut.push_back(ex->labels[i]); ua.push_back(ex->args[i]); uh.push_back(ex->tag_has_arg[i]); }
              }
            int uvk = pvr->closed == ClosedFlag::Open ? 0 : (pvr->labels ? 1 : 2);
            return eng.variant_type(std::move(ut), std::move(ua), std::move(uh), uvk);
          }
        }
        std::vector<std::string> tags; std::vector<TypePtr> ats; std::vector<char> has;
        std::vector<TypePtr> inh;
        for (auto& r : pvr->rows) {
          if (auto* rt = std::get_if<Rtag>(&r)) {
            tags.push_back(rt->name);
            if (rt->types.empty()) { ats.push_back(eng.fresh_var()); has.push_back(0); }
            else { ats.push_back(from_coretype(*rt->types[0], vars)); has.push_back(1); }
          } else {
            auto* ri = std::get_if<Rinherit>(&r);
            inh.push_back(from_coretype(*ri->ct, vars));
          }
        }
        int vk = pvr->closed == ClosedFlag::Open ? 0 : (pvr->labels ? 1 : 2);
        TypePtr row = eng.variant_type(std::move(tags), std::move(ats), std::move(has), vk);
        row->inherited = std::move(inh);
        if (pvr->labels && !pvr->labels->empty())  // `[< L > P]` present tags
          row->present = *pvr->labels;
        return row;
      }
      // A closed all-constant variant is an immediate (tag hashes) -- type it int
      // so the [int] value kind flows (strict pass, and payload/inherited rows).
      bool all_const = pvr->closed == ClosedFlag::Closed && !pvr->rows.empty();
      for (auto& r : pvr->rows) {
        auto* rt = std::get_if<Rtag>(&r);
        if (!rt || !rt->constant) { all_const = false; break; }
      }
      if (all_const) return eng.constr("int");
    }
    // `(T1,T2) #ops` (a class-subtype annotation): when ops is a KNOWN local
    // class type, build the OPEN object row from its signature with params
    // substituted (abbrev carries the name for display: open prints
    // `(..) #ops`, a closed object value `(..) ops` -- mixin3).  An unknown
    // class stays the display-faithful opaque constr (`#castable`); the
    // strict pass keeps a fresh var.
    if (auto* cl = std::get_if<Ptyp_class>(&t.desc)) {
      if (!strict) {
        std::vector<TypePtr> as;
        for (auto& a : cl->args) as.push_back(from_coretype(*a, vars));
        auto cti = classtype_decls_.find(lid_last(cl->id.txt));
        if (cti != classtype_decls_.end() && cti->second.sig &&
            cti->second.params.size() == as.size()) {
          std::unordered_map<std::string, TypePtr> sub;
          for (size_t i = 0; i < as.size(); ++i)
            if (!cti->second.params[i].empty()) sub[cti->second.params[i]] = as[i];
          std::vector<std::string> mnames;
          std::vector<TypePtr> mtypes;
          for (auto& f : cti->second.sig->fields)
            if (auto* m = std::get_if<Pctf_method>(&f.desc))
              if (m->priv == PrivateFlag::Public) {
                mnames.push_back(m->name.txt);
                mtypes.push_back(from_coretype(*m->type, sub));
              }
          TypePtr ob = eng.object_type(std::move(mnames), std::move(mtypes));
          ob->variant_kind = 1;  // `#ops`: open (a self type may add methods)
          ob->abbrev = lid_last(cl->id.txt);
          ob->abbrev_args = std::move(as);
          return ob;
        }
        return eng.constr("#" + lid_full(cl->id.txt), std::move(as));
      }
      return eng.fresh_var();
    }
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      // a qualified type `M.t` whose head module is unbound is a soundness error
      if (strict)
        if (auto* d = std::get_if<Ldot>(&c->id.txt.v))
          if (module_head_unbound(*d->prefix))
            note_error("Unbound module " + mod_components(*d->prefix).front());
      // a bare locally-abstract type name resolves to its flexible var
      if (c->args.empty())
        if (auto* l = std::get_if<Lident>(&c->id.txt.v)) {
          auto nt = newtype_vars.find(l->name);
          if (nt != newtype_vars.end()) return nt->second;
        }
      // A printf format annotation: canonicalise the NAME to format6 but keep the
      // type arguments (matching from_cmi, which preserves them).  Dropping the
      // args left a format-typed function parameter (`g : (..) format -> 'a`)
      // unable to resolve its result from a format-literal argument (`g "@]"` ->
      // 'a instead of unit).  show renders a 3-arg format6 back as `format`.
      if (is_format_base(lid_full(c->id.txt))) {
        std::vector<TypePtr> fa;
        for (auto& a : c->args) fa.push_back(from_coretype(*a, vars));
        return eng.constr("format6", std::move(fa));
      }
      // `_ M.t` where M.t is a cmi type of arity N>1: record N so the dump can
      // fill every parameter slot with Ttyp_any (the local type_arity_ registry
      // in the transcriber only covers file-local declarations).
      if (c->args.size() == 1 &&
          std::holds_alternative<Ptyp_any>(c->args[0]->desc) &&
          std::holds_alternative<Ldot>(c->id.txt.v)) {
        int ar = qualified_type_arity(c->id.txt);
        if (ar > 1) type_any_arity[&t] = ar;
      }
      std::vector<TypePtr> as;
      for (auto& a : c->args) as.push_back(from_coretype(*a, vars));
      // Pad an under-applied type (e.g. an existential GADT's `_ raw_arity`
      // written with fewer wildcards than the type's arity) with Any, so it
      // unifies with the fully-applied form instead of clashing on arity.
      // Only when at least one argument was WRITTEN: type_arity is a flat
      // bare-name map, so a zero-ary type sharing its name with some other
      // module's parameterized type (`Pos.t` vs `Immutable_array.'a t`) would
      // otherwise grow a spurious Any arg -- which poisons a signature
      // ascription's value translation (test_generator's `Pos.t` params).
      auto ar = type_arity.find(lid_last(c->id.txt));
      if (ar != type_arity.end() && !as.empty())
        // Strict pads with Any (absorbs -- can't false-reject); the display
        // passes pad with a fresh VAR so the slot can be pinned by the body
        // and prints 'a, not `_` (issue479's `_ iter2gen` second param).
        while ((int)as.size() < ar->second)
          as.push_back(eng.fresh_var());  // P4-I: guard removed, corpus-validated
      // Expand a known type abbreviation (type (params) name = manifest), with a
      // recursion guard so a cyclic/recursive abbreviation falls back to opaque.
      std::string nm = lid_last(c->id.txt);
      auto ai = type_aliases.find(nm);
      // DISPLAY pass, variant abbreviation (`type 'a lambda = [ `Var .. ]` used
      // as `_ lambda`): expand to the ROW so it unifies with the body's rows
      // (tying tag args to the DECLARED types), but STAMP the abbreviation on
      // the node -- show prints `'a lambda`, matching ocamlc's abbrev memory.
      // (A folded constr would silently fail to unify with a row and the
      // annotation would be lost -- mixin's `: _ lambda -> _`.)
      if (fold_abbrevs_ && !strict && ai != type_aliases.end() &&
          ai->second.params.size() == as.size() && !expanding_.count(nm) &&
          std::holds_alternative<Ptyp_variant>(ai->second.manifest->desc)) {
        std::unordered_map<std::string, TypePtr> sub;
        for (size_t i = 0; i < as.size(); ++i)
          if (!ai->second.params[i].empty()) sub[ai->second.params[i]] = as[i];
        expanding_.insert(nm);
        bool saved_me = manifest_expansion_;
        manifest_expansion_ = true;
        TypePtr r = I::Engine::repr(from_coretype(*ai->second.manifest, sub));
        manifest_expansion_ = saved_me;
        expanding_.erase(nm);
        if (r->kind == I::Type::Kind::Variant) {
          r->abbrev = nm;
          r->abbrev_args = as;
        }
        return r;
      }
      // A PHANTOM abbreviation (`type 'a arg_t = 'at constraint 'a = (module
      // Y.S with type t = 'at)`) has a bare-variable manifest bound only
      // through its constraints: expand it even in the folded display pass
      // (the folded name displays nothing useful) and SOLVE the constraints
      // by unifying each side under the same substitution -- `t arg_t` at
      // t = (module X.Y.S with type t = unit) gives 'at = unit (pr6954).
      if (fold_abbrevs_ && !strict && ai != type_aliases.end() &&
          ai->second.params.size() == as.size() && !expanding_.count(nm) &&
          ai->second.constraints &&
          std::holds_alternative<Ptyp_var>(ai->second.manifest->desc)) {
        std::unordered_map<std::string, TypePtr> sub;
        for (size_t i = 0; i < as.size(); ++i)
          if (!ai->second.params[i].empty()) sub[ai->second.params[i]] = as[i];
        expanding_.insert(nm);
        TypePtr r = from_coretype(*ai->second.manifest, sub);
        // Solve with abbreviations EXPANDED: the constraint's sides only
        // matter for unification (`t` must become its package manifest to
        // meet `(module Y.S with type t = 'at)`), never for display.  A side
        // substituted to an already-built FOLDED alias node (the annotation's
        // arg was built in the folded display pass) is expanded one level.
        bool saved_fold = fold_abbrevs_;
        fold_abbrevs_ = false;
        auto expand_for_solve = [&](TypePtr x) -> TypePtr {
          TypePtr t = I::Engine::repr(x);
          if (t->kind != I::Type::Kind::Constr) return x;
          std::string p = t->path;
          if (auto d = p.rfind('.'); d != std::string::npos) p = p.substr(d + 1);
          auto a2 = type_aliases.find(p);
          if (a2 == type_aliases.end() || expanding_.count(p) ||
              a2->second.params.size() != t->args.size())
            return x;
          std::unordered_map<std::string, TypePtr> vars2;
          for (size_t i = 0; i < t->args.size(); ++i)
            if (!a2->second.params[i].empty()) vars2[a2->second.params[i]] = t->args[i];
          expanding_.insert(p);
          TypePtr r2 = from_coretype(*a2->second.manifest, vars2);
          expanding_.erase(p);
          return r2;
        };
        for (auto& tc : *ai->second.constraints)
          soft_unify(expand_for_solve(from_coretype(*tc.t1, sub)),
                     expand_for_solve(from_coretype(*tc.t2, sub)));
        fold_abbrevs_ = saved_fold;
        expanding_.erase(nm);
        return r;
      }
      if (!fold_abbrevs_ && !keep_local_abbrevs_ &&
          ai != type_aliases.end() && ai->second.params.size() == as.size() &&
          !expanding_.count(nm)) {
        std::unordered_map<std::string, TypePtr> sub;
        for (size_t i = 0; i < as.size(); ++i)
          if (!ai->second.params[i].empty()) sub[ai->second.params[i]] = as[i];
        expanding_.insert(nm);
        TypePtr r = from_coretype(*ai->second.manifest, sub);
        expanding_.erase(nm);
        return r;
      }
      // A qualified abbreviation (`int Seq.t`) expands from the module's cmi
      // manifest, like the cmi-side Texpand path -- otherwise an annotation
      // stays an opaque constr and clashes with the cmi-expanded form (Seq.t's
      // manifest is the *arrow* unit -> 'a node).
      // Inside a functor instantiation: a qualified `Elem.t` (the parameter's
      // type) substitutes to the argument's corresponding type.
      if (!functor_param_subst_.empty())
        if (auto s = functor_param_subst_.find(lid_full(c->id.txt));
            s != functor_param_subst_.end())
          return s->second;
      if (!fold_abbrevs_ && std::holds_alternative<Ldot>(c->id.txt.v))
        if (TypePtr r = expand_qualified_abbrev(c->id.txt, as)) return r;
      // Inside a functor-result-signature instantiation: a bare name that is one
      // of RS's own abstract types resolves to the per-instantiation fresh var.
      if (!functor_result_abstract_.empty())
        if (auto* l = std::get_if<Lident>(&c->id.txt.v))
          if (auto s = functor_result_abstract_.find(l->name);
              s != functor_result_abstract_.end())
            return s->second;
      // A bare reference to a local opaque type carries its identity stamp.
      int stamp = 0;
      if (auto* l = std::get_if<Lident>(&c->id.txt.v)) stamp = tenv_lookup(l->name);
      // The predefined effect type is the builtin `eff`; ocamlc shows it as its
      // public alias `Effect.t`.  Canonicalise so a typext written `_ eff += ..`
      // (or annotated `unit eff`) and `Effect.perform`'s param (also Effect.t)
      // unify and print alike.
      if (!stamp)
        if (auto* l = std::get_if<Lident>(&c->id.txt.v); l && l->name == "eff")
          return eng.constr("Effect.t", std::move(as));
      // A bare type name brought into scope by `open M` (M not Stdlib, not a
      // local type) renders with M's qualification, matching ocamlc (`c_layout`
      // after `open Bigarray` -> `Bigarray.c_layout`).
      if (!stamp)
        if (auto* l = std::get_if<Lident>(&c->id.txt.v))
          if (auto q = opened_type_quals_.find(l->name); q != opened_type_quals_.end())
            return eng.constr(q->second, std::move(as));
      // `Array1.t` after `open Bigarray` -> `Bigarray.Array1.t` (opened submodule).
      std::string path = lid_full(c->id.txt);
      // A bare reference to a module-nested type displays with the module's
      // qualification (`t` inside `module Buffer` -> `Buffer.t`), matching how
      // ocamlc names it once it escapes the module.  Unify still compares last
      // components, so the qualified path stays compatible with bare uses.
      if (std::holds_alternative<Lident>(c->id.txt.v)) {
        if (stamp) {
          if (auto sp = stamp_path_.find(stamp); sp != stamp_path_.end())
            path = sp->second;
        } else if (fold_abbrevs_ && ai != type_aliases.end() &&
                   !ai->second.display_path.empty()) {
          path = ai->second.display_path;
        }
      }
      if (std::holds_alternative<Ldot>(c->id.txt.v)) {
        auto dot = path.find('.');
        if (dot != std::string::npos)
          if (auto q = opened_submod_quals_.find(path.substr(0, dot));
              q != opened_submod_quals_.end())
            path = q->second + path.substr(dot);
      }
      TypePtr rc = eng.constr(std::move(path), std::move(as), stamp);
      // A SOURCE-WRITTEN path never relinks to a family abbreviation: the user
      // wrote it and ocamlc displays it as written (`(a : int32)` stays int32
      // even after `Int32.unsigned_compare a b`).  Finalize its family heads.
      // NOT during a decl-manifest expansion: the manifest's internals
      // (`type var = [`Var of string]`'s string) are ocamlc's fresh expansion
      // nodes, which stay adoptable (subst_var's `Var slot takes Subst.key);
      // the decl's own face is re-established at finalization by
      // restore_abbrev_rows.
      if (!(manifest_expansion_ && adoptable_annot_)) eng.finalize_family_heads(rc);
      return rc;
    }
    if (auto* pk = std::get_if<Ptyp_package>(&t.desc)) return package_type(*pk, "", &vars);
    // `< m : t; .. >` object annotation -> an Object node (methods; inherits
    // ignored -- best-effort, non-strict rows).  Was: fresh var, which dropped
    // the annotation entirely (pr14554_1's `< bark : ('self -> unit) > t`).
    if (auto* ob = std::get_if<Ptyp_object>(&t.desc)) {
      if (strict) return eng.fresh_var();
      std::vector<std::string> ms;
      std::vector<TypePtr> ts;
      for (auto& f : ob->fields)
        if (auto* ot = std::get_if<Otag>(&f)) {
          ms.push_back(ot->name.txt);
          ts.push_back(from_coretype(*ot->type, vars));
        }
      TypePtr r = eng.object_type(std::move(ms), std::move(ts));
      if (ob->closed == ClosedFlag::Open) r->variant_kind = 1;  // `< ..; .. >`
      return r;
    }
    return eng.fresh_var();
  }

  // Expand a qualified type abbreviation `M.t` from the module's cmi: find M's
  // signature, then t's manifest, and convert it with t's params bound to the
  // already-converted args.  Null when M.t isn't a loadable abbreviation (opaque
  // or datatype decls stay constrs).
  TypePtr expand_qualified_abbrev(const Longident& id, const std::vector<TypePtr>& as) {
    auto* d = std::get_if<Ldot>(&id.v);
    if (!d || cmi_expanding_.count(d->name)) return nullptr;
    auto comps = mod_components(*d->prefix);
    if (comps.empty()) return nullptr;
    auto* saved = cmi_types_ctx_;
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig)
        for (auto& td : sig->types)
          if (td.name == d->name && td.manifest &&
              td.kind != cmi::TypeDecl::Open &&  // keep `Effect.t = 'a eff = ..` as Effect.t
              td.params.size() == as.size()) {
            std::unordered_map<cmi::TypeExpr*, TypePtr> m2;
            for (size_t i = 0; i < as.size(); ++i) m2[td.params[i].get()] = as[i];
            cmi_types_ctx_ = &sig->types;
            std::string saved_pfx = cmi_mod_prefix_;
            std::string pfx;
            for (auto& cmp : comps) { if (!pfx.empty()) pfx += '.'; pfx += cmp; }
            cmi_mod_prefix_ = pfx;
            cmi_expanding_.insert(td.name);
            TypePtr r = from_cmi(td.manifest, m2);
            cmi_expanding_.erase(td.name);
            cmi_types_ctx_ = saved;
            cmi_mod_prefix_ = saved_pfx;
            return r;
          }
    } catch (...) {}
    cmi_types_ctx_ = saved;
    return nullptr;
  }

  TypePtr lookup_value(const Longident& lid) {
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      for (auto it = venv.rbegin(); it != venv.rend(); ++it) {
        auto f = it->find(l->name);
        if (f != it->end()) {
          TypePtr t = eng.instantiate(f->second);
          // ocamlc re-expands a folded abbreviation from its decl at each
          // use: an intact-abbrev row instance arriving from a previous
          // binding resets its DECL-GROUND fields (free_lambda's `Names.elt`
          // contamination doesn't reach free1), then THIS binding's contacts
          // re-adopt (subst1's `Var slot takes subst_lambda's Subst.key).
          if (fold_abbrevs_ && !strict) restore_abbrev_rows(t);
          return t;
        }
      }
      auto& s = stdlib_schemes();
      auto f = s.find(l->name);
      if (f != s.end()) return eng.instantiate(f->second);
      if (strict) note_error("Unbound value " + l->name);  // genuine error
      return eng.fresh_var();
    }
    // Qualified (M.x): resolve M's exports.  If M is known and has x, use x's
    // real type (a genuine check, not Any); if M is known but lacks x, that's a
    // real Unbound-value error; if M can't be resolved at all, stay dynamic
    // (it may be a sibling/external module we can't load -- never false-reject).
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      auto& ex = module_values_cached(*d->prefix);
      if (ex.empty()) {  // module unresolvable as-is
        // `Array1.create` after `open Bigarray`: the prefix head is an opened
        // submodule, so resolve through its parent path (`Bigarray.Array1`).
        auto pc = mod_components(*d->prefix);
        if (!pc.empty())
          if (auto q = opened_submod_quals_.find(pc[0]); q != opened_submod_quals_.end()) {
            std::vector<std::string> qc = mod_components_str(q->second);
            for (size_t i = 1; i < pc.size(); ++i) qc.push_back(pc[i]);
            auto& ex2 = module_values_cached_comps(qc);
            if (auto f = ex2.find(d->name); f != ex2.end())
              return eng.instantiate(f->second);
          }
        if (strict && module_head_unbound(*d->prefix))  // genuinely unbound -> error
          note_error("Unbound module " + mod_components(*d->prefix).front());
        // Empty exports can also mean the module RESOLVES but has no values
        // (a types-only sibling unit: `A.y` with a.ml = one type decl).  If
        // its cmi signature walks cleanly, the member is genuinely absent.
        else if (strict && cmi_path_sig_resolves(*d->prefix))
          note_error("Unbound value " + lid_full(lid));
        if (std::getenv("ANY_A_DBG"))
          fprintf(stderr, "ANY_A site1 %s\n", lid_full(lid).c_str());
        // Each occurrence minted fresh: an unresolvable module member only
        // clashes when a SINGLE occurrence meets incompatible contexts (a
        // genuine error).  The remaining population here is local module
        // machinery (functor-param submodules, recursive modules, unpacks);
        // the separate-compilation population resolves via the sibling cmis.
        return eng.fresh_var();
      }
      if (auto f = ex.find(d->name); f != ex.end())
        return eng.instantiate(f->second);  // present: use its real type
      if (strict) note_error("Unbound value " + lid_full(lid));  // genuinely absent
      if (std::getenv("ANY_A_DBG"))
        fprintf(stderr, "ANY_A site2 %s\n", lid_full(lid).c_str());
      return eng.fresh_var();
    }
    if (std::getenv("ANY_A_DBG"))
      fprintf(stderr, "ANY_A site3 %s\n", lid_full(lid).c_str());
    return eng.fresh_var();  // Lapply value path (0 corpus hits)
  }

  // Whether a stdlib (sub)module's cmi is loadable (cached): the head module of a
  // path is bound if its cmi exists, or it is a local module / functor.
  std::unordered_map<std::string, bool> cmi_exists_cache_;
  bool cmi_module_loads(const std::string& head) {
    auto it = cmi_exists_cache_.find(head);
    if (it != cmi_exists_cache_.end()) return it->second;
    bool ok = false;
    try {
      cmi::CmiFile::load(head_cmi(head));
      ok = true;
    } catch (...) {}
    return cmi_exists_cache_[head] = ok;
  }
  // Names brought into bare module scope by `open M` / `include M` (M's submodules)
  // -- so a reference to one of them is not flagged unbound.
  std::set<std::string> opened_submodules_;
  // The head module of a path is unbound: not a local module/functor, not opened,
  // and no loadable cmi.  Conservative -- used only to record a soundness error.
  // Module names bound somewhere in this file that the value resolution doesn't
  // track (recursive modules, first-class-module params/unpacks): collected so a
  // reference to one is never flagged unbound (over-inclusion only loses recall).
  std::set<std::string> bound_module_names_;
  bool module_head_unbound(const Longident& m) {
    auto comps = mod_components(m);
    if (comps.empty()) return false;
    const std::string& head = comps[0];
    if (modenv.count(head) || functor_env.count(head)) return false;
    if (opened_submodules_.count(head) || bound_module_names_.count(head)) return false;
    return !cmi_module_loads(head);
  }
  // Whether a dotted module path walks cleanly to a concrete cmi signature.
  // Distinguishes "module resolves but exports no values" (a genuine
  // Unbound-value on a missing member: types-only sibling a.ml vs `A.y`)
  // from "module we just can't load" (stay dynamic).  Local modules answer
  // false: their (possibly partial) exports are handled upstream.
  bool cmi_path_sig_resolves(const Longident& m) {
    auto comps = mod_components(m);
    if (comps.empty()) return false;
    if (modenv.count(comps.back())) return false;
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      return sig != nullptr;
    } catch (...) {}
    return false;
  }
  // The submodule names of a (cmi-resolvable) module path, so `open M` / `include
  // M` can bring them into bare scope (e.g. `open Bigarray` -> Array1, Array2..).
  std::set<std::string> module_submodule_names(const Longident& m) {
    std::set<std::string> out;
    auto comps = mod_components(m);
    if (comps.empty()) return out;
    try {
      const std::string& head = comps[0];
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(head)));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig) for (auto& mm : sig->modules) out.insert(mm.name);
    } catch (...) {}
    return out;
  }
  // `open M` (M a real, non-Stdlib cmi module): record each of M's type names ->
  // `M.t`, so a bare annotation `c_layout` after `open Bigarray` renders as the
  // canonical `Bigarray.c_layout` (ocamlc keeps non-Stdlib opened types
  // qualified; Stdlib is the default-open we instead SHORTEN, so skip it).
  std::unordered_map<std::string, std::string> opened_type_quals_;
  std::unordered_map<std::string, std::string> opened_submod_quals_;  // Array1 -> Bigarray.Array1
  void load_open_type_quals(const Longident& m) {
    auto comps = mod_components(m);
    if (comps.empty() || comps[0] == "Stdlib") return;
    std::string full = lid_full(m);
    if (full.rfind("Stdlib.", 0) == 0) return;
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig) {
        for (auto& td : sig->types) opened_type_quals_[td.name] = full + "." + td.name;
      }
    } catch (...) {}
  }
  // A submodule `Array1` of an opened `Bigarray`: `Array1.t` / `open Array1`
  // qualifies through `Bigarray.Array1`.  Runs in EVERY pass (strict needs the
  // reroute to resolve `open M; ... x` for M a sibling/otherlibs submodule).
  void load_open_submod_quals(const Longident& m) {
    auto comps = mod_components(m);
    if (comps.empty() || comps[0] == "Stdlib") return;
    std::string full = lid_full(m);
    if (full.rfind("Stdlib.", 0) == 0) return;
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig)
        for (auto& mm : sig->modules) opened_submod_quals_[mm.name] = full + "." + mm.name;
    } catch (...) {}
  }
  // `open M` where M's cmi declares module ALIASES (StdLabels's `module List =
  // ListLabels`): a bare `List.map` afterwards resolves through the alias
  // target, so the LABELLED map applies (`List.map xs ~f`).  bare name ->
  // target path components.
  std::unordered_map<std::string, std::vector<std::string>> opened_module_aliases_;
  void load_open_module_aliases(const Longident& m) {
    auto comps = mod_components(m);
    if (comps.empty()) return;
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig)
        for (auto& mm : sig->modules)
          if (mm.type && mm.type->kind == cmi::ModuleType::Alias && mm.type->path)
            opened_module_aliases_[mm.name] =
                mod_components_str(cmi_path_str(*mm.type->path));
    } catch (...) {}
  }
  // resolve_module_values memoized by module path (cmi loads are expensive and a
  // file may reference M.x many times).
  std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>> modvals_cache_;
  std::set<std::string> loaded_field_mods_;  // modules whose record fields we loaded
  const std::unordered_map<std::string, TypePtr>& module_values_cached(const Longident& m) {
    std::string key = lid_full(m);
    auto it = modvals_cache_.find(key);
    if (it != modvals_cache_.end()) return it->second;
    // First reference to module m: also load its record fields, so a
    // type-directed construction `Effect.Deep.match_with f x { retc = .. }`
    // resolves the handler record without an explicit `open Effect.Deep`.
    load_module_record_fields(m);  // (internally guarded against re-loading)
    return modvals_cache_.emplace(key, resolve_module_values(m)).first->second;
  }
  // As above, keyed by an explicit component path (for a qualified opened
  // submodule like `Bigarray.Array1`, built at the use site).
  const std::unordered_map<std::string, TypePtr>& module_values_cached_comps(
      const std::vector<std::string>& comps) {
    std::string key;
    for (auto& c : comps) { if (!key.empty()) key += '.'; key += c; }
    auto it = modvals_cache_.find(key);
    if (it != modvals_cache_.end()) return it->second;
    return modvals_cache_.emplace(key, resolve_module_values_comps(comps)).first->second;
  }
  static std::vector<std::string> mod_components_str(const std::string& path) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < path.size()) {
      size_t d = path.find('.', i);
      if (d == std::string::npos) { out.push_back(path.substr(i)); break; }
      out.push_back(path.substr(i, d - i));
      i = d + 1;
    }
    return out;
  }

  // Stdlib top-level value schemes, loaded once from stdlib.cmi.
  const std::unordered_map<std::string, TypePtr>& stdlib_schemes() {
    if (stdlib_ready_) return stdlib_;
    stdlib_ready_ = true;
    try {
      auto cmi = cmi::CmiFile::load(stdpath("stdlib.cmi"));
      for (auto& v : cmi.values()) {
        std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
        stdlib_[v.name] = from_cmi(v.type, memo);
        eng.finalize_family_heads(stdlib_[v.name], /*scheme=*/true);
      }
    } catch (...) {}
    return stdlib_;
  }

  // Components of a (possibly qualified) module longident: Effect.Deep -> {Effect,Deep}.
  static std::vector<std::string> mod_components(const Longident& m) {
    std::vector<std::string> out;
    std::function<void(const Longident&)> go = [&](const Longident& x) {
      if (auto* l = std::get_if<Lident>(&x.v)) out.push_back(l->name);
      else if (auto* d = std::get_if<Ldot>(&x.v)) { go(*d->prefix); out.push_back(d->name); }
    };
    go(m);
    return out;
  }

  // The signature of a module-decl type, following an alias (e.g. stdlib's
  // `module Array = Stdlib__Array`) by loading the aliased cmi into `loaded`.
  const cmi::Signature* module_sig(const cmi::ModuleTypePtr& mt,
                                   std::vector<cmi::CmiFile>& loaded) {
    if (!mt) return nullptr;
    if (mt->kind == cmi::ModuleType::Sig) return mt->sig.get();
    if (mt->kind == cmi::ModuleType::Alias && mt->path && loaded.size() < 16) {
      std::string p = cmi_path_str(*mt->path);  // e.g. "Stdlib__Array"
      if (p.empty()) return nullptr;
      std::string low = p;
      if (low[0] >= 'A' && low[0] <= 'Z') low[0] += 32;  // first-char-lowercased
      try {
        loaded.push_back(cmi::CmiFile::load(stdpath(low + ".cmi")));
        return &loaded.back().sig();
      } catch (...) {}
      // Not a stdlib-dir unit: an alias to a separately compiled unit
      // (`module A2235 = A2235` in a sibling lib) resolves through the same
      // search as any head module (stdlib__X naming + the -I dirs).
      std::string unit = p.rfind("Stdlib__", 0) == 0 ? p.substr(8)
                       : p.rfind("Stdlib.", 0) == 0 ? p.substr(7)
                                                    : p;
      if (unit.find('.') == std::string::npos) {
        try {
          loaded.push_back(cmi::CmiFile::load(head_cmi(unit)));
          return &loaded.back().sig();
        } catch (...) {}
      }
      return nullptr;
    }
    return nullptr;
  }

  // Instantiate a local functor `F(Arg)` from its stored result signature RS,
  // substituting the parameter's types with the argument's: `module Heap (Elem :
  // OrderedType) : sig type t val add : t -> Elem.t -> unit .. end` applied to
  // `struct type t = a .. end` yields `add : t0 -> a -> unit` (Elem.t -> a, RS's
  // own `t` -> a shared fresh var t0).  Non-strict only.  Returns {} to fall
  // back to the generic-var behaviour when it can't instantiate.
  std::unordered_map<std::string, TypePtr> instantiate_local_functor(
      const FunctorDef& fd, const ModuleExpr& argExpr) {
    if (strict || !fd.result_sig) return {};
    // The argument's type definitions: `type X = T` -> from_coretype(T); an
    // abstract `type X` -> a fresh var.
    const ModuleExpr* arg = &argExpr;
    while (auto* mc = std::get_if<Pmod_constraint>(&arg->desc)) arg = mc->me.get();
    auto* as = std::get_if<Pmod_structure>(&arg->desc);
    if (!as) return {};
    std::unordered_map<std::string, TypePtr> argtypes;
    for (auto& it : as->items)
      if (auto* ty = std::get_if<Pstr_type>(&it.desc))
        for (auto& d : ty->decls) {
          std::unordered_map<std::string, TypePtr> v;
          argtypes[d.name.txt] = d.manifest ? from_coretype(**d.manifest, v)
                                            : eng.fresh_var();
        }
    // Set up the substitutions: `Param.X` -> argtypes[X]; RS's own abstract types
    // -> shared fresh vars.
    auto saved_subst = functor_param_subst_;
    auto saved_abstract = functor_result_abstract_;
    for (auto& [k, v] : argtypes) functor_param_subst_[fd.param + "." + k] = v;
    auto* rs = std::get_if<Pmty_signature>(&fd.result_sig->desc);
    for (auto& it : rs->items)
      if (auto* t = std::get_if<Psig_type>(&it.desc))
        for (auto& d : t->decls) functor_result_abstract_[d.name.txt] = eng.fresh_var();
    std::unordered_map<std::string, TypePtr> out;
    for (auto& it : rs->items)
      if (auto* v = std::get_if<Psig_value>(&it.desc)) {
        std::unordered_map<std::string, TypePtr> vars;
        out[v->vd.name.txt] = from_coretype(*v->vd.type, vars);
      }
    // A typext in the result sig (`module Define (D : Desc) : sig type 'a tag
    // += C : D.t tag end`): bind its ctors while the substitutions are live,
    // so `StrM.C` from `module StrM = Msg.Define(struct type t = string ..
    // end)` gets `string tag` (msg.ml's write_string pins on it).  Into the
    // scoped cenv, not the flat map -- the functor BODY's harvest already put
    // an unsubstituted `C` there, and a second flat registration would only
    // mark the name ambiguous.
    bind_sig_typext_ctors(rs->items);
    functor_param_subst_ = std::move(saved_subst);
    functor_result_abstract_ = std::move(saved_abstract);
    // Tie the argument's values to the parameter signature's expected types
    // (struct `compare = cmp` vs OrderedType's `compare : t -> t -> int`, t = the
    // arg's t) so `cmp` gets `a -> a -> int`, not a free var.
    if (fd.param_sig) {
      auto argvals = module_exports(*arg);
      auto psvals = param_sig_value_schemes(*fd.param_sig, argtypes);
      for (auto& [nm, expected] : psvals)
        if (auto f = argvals.find(nm); f != argvals.end()) try_unify(f->second, expected);
    }
    return out;
  }

  // A parameter signature PS's value name -> expected type, with PS's abstract
  // types substituted by the functor argument's (`t` -> the arg's t).  PS may be
  // an inline `sig .. end`, a cmi module-type ident (Map.OrderedType), or `S with`.
  // Value name -> declared type of a signature's `val` items, with the abstract
  // types named in `argtypes` substituted (arrow labels are preserved, so an
  // optional param survives).
  std::unordered_map<std::string, TypePtr> sig_items_value_schemes(
      const ast::Signature& items,
      const std::unordered_map<std::string, TypePtr>& argtypes) {
    std::unordered_map<std::string, TypePtr> out;
    auto saved = functor_result_abstract_;
    for (auto& it : items)
      if (auto* t = std::get_if<Psig_type>(&it.desc))
        for (auto& d : t->decls)
          if (auto a = argtypes.find(d.name.txt); a != argtypes.end())
            functor_result_abstract_[d.name.txt] = a->second;
    // Sig-local `module Env : S` binds Env for the val types that follow
    // (`val code0 : Env.in_t -> out0`); count it bound so strict's
    // unbound-module check can't false-fire (flat over-inclusive set, like
    // the other bound_module_names_ producers).
    for (auto& it : items)
      if (auto* md = std::get_if<Psig_module>(&it.desc)) {
        if (md->md.name.txt) bound_module_names_.insert(*md->md.name.txt);
      }
    for (auto& it : items)
      if (auto* v = std::get_if<Psig_value>(&it.desc)) {
        std::unordered_map<std::string, TypePtr> vars;
        out[v->vd.name.txt] = from_coretype(*v->vd.type, vars);
      }
    functor_result_abstract_ = std::move(saved);
    return out;
  }

  // An arrow carrying just the SYNTACTIC parameter labels of a `let rec` RHS
  // function (fresh var types), to unify with the recursion var so a self-call's
  // optional args are desugared.  Null when the RHS isn't syntactically a fun.
  TypePtr syntactic_fun_arrow(const Expression& rhs) {
    auto* f = std::get_if<Pexp_function>(&rhs.desc);
    if (!f) return nullptr;
    std::vector<std::pair<int, std::string>> labels;
    for (auto& fp : f->params)
      if (auto* pv = std::get_if<Pparam_val>(&fp.desc))
        labels.push_back(arglabel(pv->label));
    bool cases = f->body && std::holds_alternative<Pfunction_cases>(f->body->v);
    if (labels.empty() && !cases) return nullptr;
    TypePtr arr = eng.fresh_var();                            // result
    if (cases) arr = eng.arrow(eng.fresh_var(), arr, 0, "");  // the `function` param
    for (auto it = labels.rbegin(); it != labels.rend(); ++it)
      arr = eng.arrow(eng.fresh_var(), arr, it->first, it->second);
    return arr;
  }

  std::unordered_map<std::string, TypePtr> param_sig_value_schemes(
      const ModuleType& ps, const std::unordered_map<std::string, TypePtr>& argtypes) {
    std::unordered_map<std::string, TypePtr> out;
    if (auto* sg = std::get_if<Pmty_signature>(&ps.desc)) {
      return sig_items_value_schemes(sg->items, argtypes);
    } else if (auto* mi = std::get_if<Pmty_ident>(&ps.desc)) {
      // A LOCAL `module type S = sig .. end` first (so a functor param `M : S`
      // whose S is defined in this file resolves M's members), else a cmi one.
      if (auto* l = std::get_if<Lident>(&mi->id.txt.v)) {
        auto it = modtype_sig_asts_.find(l->name);
        if (it != modtype_sig_asts_.end())
          return sig_items_value_schemes(*it->second, argtypes);
      }
      out = cmi_modtype_value_schemes(mi->id.txt, argtypes);
    } else if (auto* mw = std::get_if<Pmty_with>(&ps.desc)) {
      return param_sig_value_schemes(*mw->mt, argtypes);
    }
    return out;
  }

  // Value schemes of a cmi module type `M.S` (e.g. Map.OrderedType), with its
  // abstract types substituted by the functor argument's types.
  // Resolve a folded cmi abbreviation path ("Arg.anon_fun") to its manifest
  // translation, for unify's lenient cross-kind expansion.  Only PARAMETERLESS
  // manifest-carrying (non-extensible) decls; memoized, negative-cached, and
  // re-entrancy-guarded via the negative cache.
  std::unordered_map<std::string, TypePtr> abbrev_exp_cache_;
  std::set<std::string> abbrev_exp_neg_;
  int abbrev_local_depth_ = 0;  // bounds local-alias unify-retry expansion
  TypePtr resolve_abbrev_expansion(const std::string& path,
                                   const std::vector<TypePtr>& args) {
    if (args.empty())
      if (auto it = abbrev_exp_cache_.find(path); it != abbrev_exp_cache_.end())
        return it->second;
    if (abbrev_exp_neg_.count(path)) return nullptr;
    std::vector<std::string> comps;
    for (size_t p = 0, d; p < path.size(); p = d + 1) {
      d = path.find('.', p);
      if (d == std::string::npos) { comps.push_back(path.substr(p)); break; }
      comps.push_back(path.substr(p, d - p));
    }
    if (comps.size() < 2) {
      // A LOCAL abbreviation (`('a,'c) iter2gen = .. -> .. -> ..`): expand its
      // manifest under the args' substitution, so the lenient cross-kind retry
      // ties a FOLDED annotation's args to the inferred concrete type
      // (issue479's `let iter2gen : _ iter2gen = fun iter c -> ..`).  Not
      // negative-cached: type_aliases grows as the file processes.  Variant-
      // row manifests are excluded (rows have their own fold-pass expansion,
      // and re-expanding per unify retry loops on recursive rows -- mixin);
      // the depth cap bounds alias-of-alias retry chains the expanding_ guard
      // can't see across separate resolver calls.
      auto ai = type_aliases.find(path);
      if (ai != type_aliases.end() && ai->second.params.size() == args.size() &&
          !expanding_.count(path) && abbrev_local_depth_ < 32 &&
          !std::holds_alternative<Ptyp_variant>(ai->second.manifest->desc)) {
        std::unordered_map<std::string, TypePtr> sub;
        for (size_t i = 0; i < args.size(); ++i)
          if (!ai->second.params[i].empty()) sub[ai->second.params[i]] = args[i];
        expanding_.insert(path);
        ++abbrev_local_depth_;
        bool sf = fold_abbrevs_;
        fold_abbrevs_ = false;  // the expansion feeds unify, not display
        TypePtr r = from_coretype(*ai->second.manifest, sub);
        fold_abbrevs_ = sf;
        --abbrev_local_depth_;
        expanding_.erase(path);
        return r;
      }
      return nullptr;
    }
    abbrev_exp_neg_.insert(path);  // re-entrancy guard; erased on success
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i + 1 < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig)
        for (auto& td : sig->types)
          if (td.name == comps.back()) {
            if (!td.manifest || td.params.size() != args.size() ||
                td.kind == cmi::TypeDecl::Open)
              return nullptr;
            std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
            for (size_t i = 0; i < args.size(); ++i)
              memo[td.params[i].get()] = args[i];
            TypePtr r = from_cmi(td.manifest, memo);
            abbrev_exp_neg_.erase(path);
            if (r && args.empty()) abbrev_exp_cache_[path] = r;
            return r;
          }
    } catch (...) {}
    return nullptr;
  }

  std::unordered_map<std::string, TypePtr> cmi_modtype_value_schemes(
      const Longident& path, const std::unordered_map<std::string, TypePtr>& argtypes) {
    std::unordered_map<std::string, TypePtr> out;
    auto comps = mod_components(path);
    if (comps.empty()) return out;
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i + 1 < comps.size() && sig; ++i) {  // navigate submodules
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (!sig) return out;
      const cmi::ModuleType* mt = nullptr;  // the named module type
      for (auto& mtd : sig->modtypes) if (mtd.name == comps.back()) { mt = mtd.type.get(); break; }
      if (!mt || mt->kind != cmi::ModuleType::Sig || !mt->sig) return out;
      cmi_abstract_subst_ = argtypes;
      for (auto& v : mt->sig->values) {
        std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
        out[v.name] = from_cmi(v.type, memo);
        eng.finalize_family_heads(out[v.name], /*scheme=*/true);
      }
      cmi_abstract_subst_.clear();
    } catch (...) { cmi_abstract_subst_.clear(); }
    return out;
  }

  // Resolve a (possibly qualified) module path to its exported value schemes:
  // a local top-level module from modenv, else a stdlib module/submodule walked
  // through nested signatures (open Effect.Deep -> stdlib__Effect.cmi -> Deep).
  std::unordered_map<std::string, TypePtr> resolve_module_values(const Longident& m) {
    return resolve_module_values_comps(mod_components(m));
  }
  std::unordered_map<std::string, TypePtr> resolve_module_values_comps(
      std::vector<std::string> comps) {
    if (comps.empty()) return {};
    // local modules are recorded flat by simple name; a qualified local nested
    // module (e.g. include T.Int) is found by its last component.
    {
      auto it = modenv.find(comps.back());
      if (it != modenv.end()) return it->second;
    }
    // An opened module's ALIAS member (`open StdLabels` -> List = ListLabels):
    // reroute the head through the alias target.
    if (auto al = opened_module_aliases_.find(comps[0]);
        al != opened_module_aliases_.end()) {
      std::vector<std::string> t = al->second;
      t.insert(t.end(), comps.begin() + 1, comps.end());
      comps = std::move(t);
    }
    // An opened module's SUBMODULE (`open Deprecated_module` -> M): reroute
    // the head through the parent path so its values load from the cmi.
    else if (auto q = opened_submod_quals_.find(comps[0]);
             q != opened_submod_quals_.end()) {
      std::vector<std::string> t = mod_components_str(q->second);
      t.insert(t.end(), comps.begin() + 1, comps.end());
      comps = std::move(t);
    }
    std::unordered_map<std::string, TypePtr> out;
    try {
      const std::string& head = comps[0];
      // cmis stay alive for the whole walk; sig points into the last one.
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(head)));
      const cmi::Signature* sig = &loaded.back().sig();
      // Record each module level's (types, cumulative-prefix) as an enclosing
      // scope, so a Pident type owned by a PARENT module qualifies to its own
      // module (`Array1.create`'s `kind` -> `Bigarray.kind`, not the submodule).
      std::vector<std::pair<const std::vector<cmi::TypeDecl>*, std::string>> scopes;
      std::vector<std::pair<const std::vector<cmi::ModuleDecl>*, std::string>> mscopes;
      scopes.push_back({&sig->types, comps[0]});
      mscopes.push_back({&sig->modules, comps[0]});
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
        if (sig) {
          scopes.push_back({&sig->types, scopes.back().second + "." + comps[i]});
          mscopes.push_back({&sig->modules, mscopes.back().second + "." + comps[i]});
        }
      }
      if (sig) {
        cmi_types_ctx_ = &sig->types;  // enable same-module abbreviation expansion
        cmi_mod_prefix_ = scopes.back().second;
        cmi_scopes_ = scopes;
        cmi_mod_scopes_ = mscopes;
        for (auto& v : sig->values) {
          std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
          out[v.name] = from_cmi(v.type, memo);
          eng.finalize_family_heads(out[v.name], /*scheme=*/true);
        }
        cmi_types_ctx_ = nullptr;
        cmi_mod_prefix_.clear();
        cmi_scopes_.clear();
        cmi_mod_scopes_.clear();
      }
    } catch (...) { cmi_types_ctx_ = nullptr; cmi_mod_prefix_.clear(); cmi_scopes_.clear(); cmi_mod_scopes_.clear(); }
    return out;
  }

  // Load the record-field schemes of every record type in module `m` (navigated
  // through the cmis) into ext_fields_, qualified (`Effect.Deep.handler`).  So a
  // construction `{ retc; exnc; effc }` after `open Effect.Deep` resolves to the
  // handler record and its result type flows (match_with's `'c` -> unit).
  void load_module_record_fields(const Longident& m) {
    auto comps = mod_components(m);
    if (comps.empty()) return;
    // Guard against double-loading (a module both opened/referenced and aliased):
    // re-loading would push each label twice and make it spuriously ambiguous.
    if (!loaded_field_mods_.insert(lid_full(m)).second) return;
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (!sig) return;
      std::string pfx;
      for (auto& cmp : comps) { if (!pfx.empty()) pfx += '.'; pfx += cmp; }
      cmi_types_ctx_ = &sig->types;
      cmi_mod_prefix_ = pfx;
      for (auto& td : sig->types) {
        if (td.kind != cmi::TypeDecl::Record || td.labels.empty()) continue;
        std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
        std::vector<TypePtr> params;
        for (auto& p : td.params) params.push_back(from_cmi(p, memo));
        TypePtr recTy = eng.constr(pfx + "." + td.name, params);
        for (auto& l : td.labels)
          ext_fields_[l.name].push_back(eng.arrow(recTy, from_cmi(l.type, memo)));
      }
      cmi_types_ctx_ = nullptr; cmi_mod_prefix_.clear();
    } catch (...) { cmi_types_ctx_ = nullptr; cmi_mod_prefix_.clear(); }
  }
  // A module-qualified field `e.M[.P].label`: the explicit path names the
  // record's module authoritatively (OCaml's path-directed disambiguation), so
  // resolve the owning record decl from the cmis and return its accessor scheme
  // (recTy -> fieldTy, generalized), like field_scheme for local records.  The
  // LAST matching decl in the signature wins (later decls shadow earlier ones).
  // M's head reroutes through a file-local alias (`module MP = Gc.Memprof`).
  TypePtr qualified_field_scheme(const Longident& field) {
    auto* d = std::get_if<Ldot>(&field.v);
    if (!d) return nullptr;
    auto comps = mod_components(*d->prefix);
    if (comps.empty()) return nullptr;
    for (auto& [tgt, al] : module_aliases_)
      if (al == comps[0]) {
        std::vector<std::string> tc = mod_components_str(tgt);
        tc.insert(tc.end(), comps.begin() + 1, comps.end());
        comps = std::move(tc);
        break;
      }
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      // Track each module level's (types, cumulative-prefix) so a field type
      // owned by a PARENT module qualifies to its own module
      // (LargeFile.stats' `st_kind : file_kind` -> `Unix.file_kind`).
      std::vector<std::pair<const std::vector<cmi::TypeDecl>*, std::string>> scopes;
      std::vector<std::pair<const std::vector<cmi::ModuleDecl>*, std::string>> mscopes;
      scopes.push_back({&sig->types, comps[0]});
      mscopes.push_back({&sig->modules, comps[0]});
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
        if (sig) {
          scopes.push_back({&sig->types, scopes.back().second + "." + comps[i]});
          mscopes.push_back({&sig->modules, mscopes.back().second + "." + comps[i]});
        }
      }
      if (!sig) return nullptr;
      std::string pfx;
      for (auto& cmp : comps) { if (!pfx.empty()) pfx += '.'; pfx += cmp; }
      auto* saved_ctx = cmi_types_ctx_;
      std::string saved_pfx = cmi_mod_prefix_;
      bool saved_fold = fold_abbrevs_;
      auto saved_scopes = cmi_scopes_;
      auto saved_mscopes = cmi_mod_scopes_;
      cmi_types_ctx_ = &sig->types;
      cmi_mod_prefix_ = pfx;
      cmi_scopes_ = scopes;
      cmi_mod_scopes_ = mscopes;
      fold_abbrevs_ = !strict;
      TypePtr scheme = nullptr;
      for (auto it = sig->types.rbegin(); it != sig->types.rend() && !scheme; ++it) {
        if (it->kind != cmi::TypeDecl::Record) continue;
        for (auto& l : it->labels)
          if (l.name == d->name) {
            std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
            std::vector<TypePtr> params;
            for (auto& p : it->params) params.push_back(from_cmi(p, memo));
            TypePtr recTy = eng.constr(pfx + "." + it->name, params);
            scheme = eng.arrow(recTy, from_cmi(l.type, memo));
            break;
          }
      }
      cmi_types_ctx_ = saved_ctx;
      cmi_mod_prefix_ = saved_pfx;
      cmi_scopes_ = std::move(saved_scopes);
      cmi_mod_scopes_ = std::move(saved_mscopes);
      fold_abbrevs_ = saved_fold;
      return scheme;
    } catch (...) {
      cmi_types_ctx_ = nullptr; cmi_mod_prefix_.clear();
      cmi_scopes_.clear(); cmi_mod_scopes_.clear();
    }
    return nullptr;
  }

  // Scan top-level `open M` / `include M` (M a plain module path) and load their
  // record fields, so external record constructions resolve.
  void load_open_record_fields(const ast::Structure& items) {
    for (auto& it : items) {
      const ModuleExpr* me = nullptr;
      if (auto* op = std::get_if<Pstr_open>(&it.desc)) me = &op->expr;
      else if (auto* in = std::get_if<Pstr_include>(&it.desc)) me = &in->expr;
      if (me)
        if (auto* pi = std::get_if<Pmod_ident>(&me->desc))
          load_module_record_fields(pi->id.txt);
    }
  }

  // Resolve a functor application F(...)'s result value *names*, bound to fresh
  // polymorphic vars.  Applying a functor would require substituting the argument
  // signature into the body, which we don't do; binding the names (types left
  // fully generic) clears the unbound-value false-rejections without ever
  // introducing a clash.  F is given by its (possibly qualified) module path.
  std::unordered_map<std::string, TypePtr> functor_result_values(const Longident& fpath,
                                                                 int napp = 1,
                                                                 const ModuleExpr* arg1 = nullptr) {
    auto comps = mod_components(fpath);
    if (comps.empty()) return {};
    if (comps.size() == 1) {  // a local functor's recorded (fully-applied) body
      auto it = functor_env.find(comps[0]);
      if (it != functor_env.end()) return it->second;
    }
    // An opened module's SUBMODULE functor (`open MoreLabels` then
    // `Map.Make(..)`): the head resolves through the open, so the LABELLED
    // value schemes apply (MoreLabels.Map's fold has ~f/~init; Map's doesn't).
    if (auto q = opened_submod_quals_.find(comps[0]);
        q != opened_submod_quals_.end()) {
      auto qc = mod_components_str(q->second);
      qc.insert(qc.end(), comps.begin() + 1, comps.end());
      comps = std::move(qc);
    }
    std::unordered_map<std::string, TypePtr> out;
    try {
      const std::string& head = comps[0];
      auto cmi = cmi::CmiFile::load(head_cmi(head));
      const cmi::Signature* sig = &cmi.sig();
      const cmi::Signature* parent = sig;  // sig the functor was found in
      const cmi::ModuleType* mt = nullptr;
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        if (!md || !md->type) { sig = nullptr; break; }
        parent = sig;
        mt = md->type.get();
        sig = (mt->kind == cmi::ModuleType::Sig) ? mt->sig.get() : nullptr;
      }
      // Tie a struct argument's values to the cmi functor's PARAM signature
      // with the struct's own manifests substituted: `Set.Make(struct type t =
      // s let compare = cmp end)` gives cmp : s -> s -> int (make_set).  Only
      // still-unresolved (var) exports are tied.  Non-strict only.
      // Tie a struct argument's values to the cmi functor's PARAM signature.
      // The param signature may be a NAMED modtype (Set.Make's `OrderedType`):
      // resolve it in the signature the functor was found in.
      const cmi::ModuleType* fpt = nullptr;
      if (mt && mt->kind == cmi::ModuleType::Functor && mt->functor_param_type) {
        fpt = mt->functor_param_type.get();
        if (fpt->kind == cmi::ModuleType::Ident && fpt->path && parent) {
          const cmi::Path* p = fpt->path.get();
          const std::string& nm =
              p->kind == cmi::Path::Pdot ? p->s : p->id.name;
          fpt = nullptr;
          for (auto& mtd : parent->modtypes)
            if (mtd.name == nm) { fpt = mtd.type.get(); break; }
        }
      }
      if (!strict && arg1 && napp == 1 && fpt &&
          fpt->kind == cmi::ModuleType::Sig && fpt->sig) {
        const ModuleExpr* am = arg1;
        while (auto* mc = std::get_if<Pmod_constraint>(&am->desc)) am = mc->me.get();
        if (auto* as2 = std::get_if<Pmod_structure>(&am->desc)) {
          std::unordered_map<std::string, TypePtr> manifests;
          for (auto& it2 : as2->items)
            if (auto* ty = std::get_if<Pstr_type>(&it2.desc))
              for (auto& d : ty->decls)
                if (d.manifest) {
                  std::unordered_map<std::string, TypePtr> v;
                  manifests[d.name.txt] = from_coretype(**d.manifest, v);
                }
          auto argvals = module_exports(*am);
          cmi_abstract_subst_ = manifests;
          for (auto& v : fpt->sig->values) {
            auto f = argvals.find(v.name);
            if (f == argvals.end()) continue;
            if (I::Engine::repr(f->second)->kind != I::Type::Kind::Var) continue;
            std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
            soft_unify(f->second, from_cmi(v.type, memo));
          }
          cmi_abstract_subst_.clear();
        }
      }
      // Descend napp functor-body levels (curried application F(A)(B)...),
      // then take the resulting signature's value names.
      const cmi::ModuleType* cur = mt;
      for (int i = 0; i < napp && cur; ++i)
        cur = (cur->kind == cmi::ModuleType::Functor) ? cur->functor_body.get() : nullptr;
      if (cur && cur->kind == cmi::ModuleType::Sig && cur->sig) {
        // Name the result's abstract types after the binding (`M.t`), by
        // translating the value schemes with the result sig as the same-module
        // context and the binding name as the prefix.  The strict reject pass
        // keeps fully-generic schemes (which never clash); the value-kinds and
        // signature passes get the real, M-qualified types.
        bool real = !strict && !func_bind_name_.empty();
        if (real) { cmi_types_ctx_ = &cur->sig->types; cmi_mod_prefix_ = func_bind_name_;
                    func_result_mode_ = true; }
        for (auto& v : cur->sig->values) {
          if (real && v.type) {
            std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
            out[v.name] = from_cmi(v.type, memo);
            // A SCHEME's family heads are finalized: uses relink fresh copies,
            // never the shared scheme node (a live value adopting `HW.key`
            // must not corrupt HW.mem's own dom for the rest of the file).
            eng.finalize_family_heads(out[v.name], /*scheme=*/true);
          } else {
            out[v.name] = generic_var();
          }
        }
        cmi_types_ctx_ = nullptr; cmi_mod_prefix_.clear(); func_result_mode_ = false;
      }
    } catch (...) { cmi_types_ctx_ = nullptr; cmi_mod_prefix_.clear(); func_result_mode_ = false; }
    return out;
  }

  // Collect the value (and external) names declared in a signature.
  static void collect_sig_values(const ast::Signature& sig, std::vector<std::string>& out) {
    for (auto& it : sig) {
      if (auto* v = std::get_if<Psig_value>(&it.desc)) out.push_back(v->vd.name.txt);
      else if (auto* p = std::get_if<Psig_primitive>(&it.desc)) out.push_back(p->pd.name.txt);
    }
  }

  // The value names of a module type, as fresh polymorphic schemes (so an
  // unpack against it binds the names without introducing clashes): a local
  // `module type S = sig ... end`, or an inline signature.
  // A named module type's value names (fresh schemes), keyed by its path.
  std::unordered_map<std::string, TypePtr> modtype_values_of(const Longident& path) {
    std::unordered_map<std::string, TypePtr> out;
    if (auto* l = std::get_if<Lident>(&path.v)) {
      auto it = modtype_env.find(l->name);
      if (it != modtype_env.end())
        for (auto& n : it->second) out[n] = generic_var();
    }
    return out;
  }

  std::unordered_map<std::string, TypePtr> modtype_values(const ModuleType& mt) {
    std::vector<std::string> names;
    if (auto* mi = std::get_if<Pmty_ident>(&mt.desc)) {
      return modtype_values_of(mi->id.txt);
    } else if (auto* sg = std::get_if<Pmty_signature>(&mt.desc)) {
      collect_sig_values(sg->items, names);
    } else if (auto* mw = std::get_if<Pmty_with>(&mt.desc)) {
      return modtype_values(*mw->mt);  // `S with type t = u`: same value names
    }
    std::unordered_map<std::string, TypePtr> out;
    for (auto& n : names) out[n] = generic_var();
    return out;
  }

  void register_predef_ctors() {
    auto a = generic_var();
    ctors["[]"] = eng.constr("list", {a});
    ctors["::"] = eng.arrow(a, eng.arrow(eng.constr("list", {a}),
                                         eng.constr("list", {a})));
    auto o = generic_var();
    ctors["None"] = eng.constr("option", {o});
    ctors["Some"] = eng.arrow(o, eng.constr("option", {o}));
    ctors["true"] = eng.constr("bool");
    ctors["false"] = eng.constr("bool");
    ctors["()"] = eng.constr("unit");
    // result (predefined since 4.03): Ok of 'a / Error of 'b : ('a,'b) result.
    auto rok = generic_var(), rerr = generic_var();
    ctors["Ok"] = eng.arrow(rok, eng.constr("result", {rok, rerr}));
    ctors["Error"] = eng.arrow(rerr, eng.constr("result", {rok, rerr}));
    predef_ctors_ = {"[]", "::", "None", "Some", "true", "false", "()", "Ok", "Error"};
    type_ctors["bool"] = {"false", "true"};
    type_ctors["option"] = {"None", "Some"};
    type_ctors["list"] = {"[]", "::"};
    type_ctors["unit"] = {"()"};
    type_ctors["result"] = {"Ok", "Error"};
    // The predefined exceptions (Stdlib's `Predef`), so an unqualified use pins
    // its argument's type: `Assert_failure (f, line, 0)` gives line : int.
    TypePtr exn = eng.constr("exn");
    TypePtr sii = eng.tuple({eng.constr("string"), eng.constr("int"), eng.constr("int")});
    for (auto& n : {"Not_found", "Out_of_memory", "Stack_overflow", "End_of_file",
                    "Division_by_zero", "Sys_blocked_io"})
      ctors[n] = exn;
    for (auto& n : {"Failure", "Invalid_argument", "Sys_error"})
      ctors[n] = eng.arrow(eng.constr("string"), exn);
    for (auto& n : {"Match_failure", "Assert_failure", "Undefined_recursive_module"})
      ctors[n] = eng.arrow(sii, exn);
    for (auto& n : {"Not_found", "Out_of_memory", "Stack_overflow", "End_of_file",
                    "Division_by_zero", "Sys_blocked_io", "Failure", "Invalid_argument",
                    "Sys_error", "Match_failure", "Assert_failure",
                    "Undefined_recursive_module"})
      exn_ctors_.insert(n);
  }

  // Register top-level Stdlib variant CONSTANT constructors (e.g. fpclass's
  // FP_normal) so an unqualified use gets its real type (and an all-constant type
  // is marked immediate -> the [int] value kind).  Skips names already known
  // (predef wins) or ambiguous across stdlib types; constant ctors only (block
  // ctors need parameter handling and are left to Any).
  // The declared type of a stdlib (sub)module's record field (e.g. Gc.control's
  // `minor_heap_size`), instantiated into our type universe; null if not found.
  TypePtr stdlib_field_type(const std::string& mod, const std::string& label) {
    try {
      auto cmi = cmi::CmiFile::load(head_cmi(mod));
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Record) continue;
        for (auto& l : td.labels)
          if (l.name == label) {
            std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
            return from_cmi(l.type, memo);
          }
      }
    } catch (...) {}
    return nullptr;
  }
  void register_stdlib_ctors() {
    try {
      auto cmi = cmi::CmiFile::load(stdpath("stdlib.cmi"));
      std::set<std::string> ambiguous, seen;
      std::unordered_map<std::string, TypePtr> found;
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Variant || td.ctors.empty()) continue;
        bool gadt = false, all_const = true;
        for (auto& c : td.ctors) {
          if (c.res) gadt = true;
          if (!c.args.empty() || c.is_inline_record) all_const = false;
        }
        if (gadt) continue;
        std::vector<TypePtr> params;
        for (int i = 0; i < td.arity; ++i) params.push_back(generic_var());
        for (auto& c : td.ctors) {
          if (!c.args.empty() || c.is_inline_record) continue;  // constant ctors only
          if (seen.count(c.name) || ctors.count(c.name)) { ambiguous.insert(c.name); continue; }
          seen.insert(c.name);
          found[c.name] = eng.constr(td.name, params);
        }
        if (all_const) immediate_types_.insert(td.name);
      }
      for (auto& [name, ty] : found)
        if (!ambiguous.count(name)) {
          ctors[name] = ty;
          stdlib_ctor_types_[ty->path].push_back(name);
        }
    } catch (...) {}
  }
  // Stdlib types whose constant ctors register_stdlib_ctors bound, by type name
  // -> ctor names.  Consulted when a TOP-LEVEL decl shadows the type name: the
  // stdlib schemes are then requalified `Stdlib.name` (or `Stdlib/2.name` when
  // the file also binds a module named Stdlib), matching ocamlc's display.
  std::unordered_map<std::string, std::vector<std::string>> stdlib_ctor_types_;
  bool user_stdlib_module_ = false;      // file binds a top-level `module Stdlib`
  std::set<std::string> stdlib_keep_paths_;  // exact requalified paths, for show

  // Collect the type-variable names in a core type.  Sets `uncertain` when a
  // construct that can introduce/bind implicit row or universal variables
  // appears (poly-variant, object, alias, poly, package, class) -- we then skip
  // the unbound-variable check to stay sound (never false-reject).
  static void collect_tyvars(const CoreType& t, std::set<std::string>& vars, bool& uncertain) {
    if (auto* v = std::get_if<Ptyp_var>(&t.desc)) { vars.insert(v->name); return; }
    if (std::holds_alternative<Ptyp_any>(t.desc)) return;
    if (auto* a = std::get_if<Ptyp_arrow>(&t.desc)) {
      collect_tyvars(*a->dom, vars, uncertain); collect_tyvars(*a->cod, vars, uncertain); return;
    }
    if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
      for (auto& e : tu->elems) collect_tyvars(*e, vars, uncertain); return;
    }
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      for (auto& a : c->args) collect_tyvars(*a, vars, uncertain); return;
    }
    uncertain = true;  // variant/object/alias/poly/package/class/...: be safe
  }
  // The unbound-type-variable restriction on a type declaration: every variable
  // used in the body must be a declared parameter (or bound by a constraint /
  // constructor existential).  Conservative: skip GADTs and uncertain bodies.
  void check_type_vars(const TypeDeclaration& d) {
    if (!strict) return;
    std::set<std::string> allowed;
    bool uncertain = false;
    for (auto& p : d.params)
      if (auto* v = std::get_if<Ptyp_var>(&p->desc)) allowed.insert(v->name);
    for (auto& con : d.constraints) {  // constraint t1 = t2 binds its variables
      collect_tyvars(*con.t1, allowed, uncertain);
      collect_tyvars(*con.t2, allowed, uncertain);
    }
    std::set<std::string> used;
    if (d.manifest) collect_tyvars(*d.manifest->get(), used, uncertain);
    if (auto* rec = std::get_if<Ptype_record>(&d.kind))
      for (auto& f : rec->fields) collect_tyvars(*f.type, used, uncertain);
    if (auto* var = std::get_if<Ptype_variant>(&d.kind))
      for (auto& c : var->ctors) {
        if (c.res) { uncertain = true; break; }  // GADT: existential vars -- skip
        std::set<std::string> exi(allowed);
        for (auto& vn : c.vars) exi.insert(vn);  // A : 'a. ... -> t  existentials
        if (auto* tup = std::get_if<Pcstr_tuple>(&c.args))
          for (auto& e : tup->elems) {
            std::set<std::string> u; collect_tyvars(*e, u, uncertain);
            for (auto& vn : u) if (!exi.count(vn)) used.insert(vn);
          }
        else if (auto* r = std::get_if<Pcstr_record>(&c.args))
          for (auto& f : r->fields) {
            std::set<std::string> u; collect_tyvars(*f.type, u, uncertain);
            for (auto& vn : u) if (!exi.count(vn)) used.insert(vn);
          }
      }
    if (uncertain) return;
    for (auto& v : used)
      if (!allowed.count(v))
        note_error("The type variable \"'" + v + "\" is unbound in this type declaration.");
  }

  // Alias names referenced (transitively expandable) in a manifest core type.
  void collect_alias_refs(const CoreType& t, std::set<std::string>& out) {
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      // Only a bare (unqualified) name can refer to a file-local alias; a
      // qualified `M.t` is another module's type, not this alias (matching it by
      // last component would invent false cycles, e.g. `type t = T1.t = A`).
      if (auto* l = std::get_if<Lident>(&c->id.txt.v))
        if (type_aliases.count(l->name)) out.insert(l->name);
      for (auto& a : c->args) collect_alias_refs(*a, out);
    } else if (auto* a = std::get_if<Ptyp_arrow>(&t.desc)) {
      collect_alias_refs(*a->dom, out); collect_alias_refs(*a->cod, out);
    } else if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
      for (auto& e : tu->elems) collect_alias_refs(*e, out);
    } else if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) {
      collect_alias_refs(*al->type, out);
    }
  }
  // Cyclic type-abbreviation check: an abbreviation whose expansion refers back
  // to itself (`type t = t * t`, `type a = b and b = a`) is rejected (no
  // -rectypes).  Only file-local aliases participate, so this never
  // false-rejects external/valid types.
  void check_cyclic_aliases() {
    if (!strict) return;
    std::unordered_map<std::string, std::set<std::string>> refs;
    for (auto& [name, al] : type_aliases) collect_alias_refs(*al.manifest, refs[name]);
    for (auto& [name, al] : type_aliases) {
      std::set<std::string> seen;
      std::function<bool(const std::string&)> reaches = [&](const std::string& cur) -> bool {
        auto it = refs.find(cur);
        if (it == refs.end()) return false;
        for (auto& nx : it->second) {
          if (nx == name) return true;
          if (seen.insert(nx).second && reaches(nx)) return true;
        }
        return false;
      };
      if (reaches(name))
        note_error("The type abbreviation \"" + name + "\" is cyclic");
    }
  }

  // OCaml requires module-type names to be unique within a single structure or
  // signature scope (`module type X = ...` twice is an error, never valid code).
  // Walk every scope and flag a duplicate.  Sound: same-named module types in
  // different scopes (nested modules) are fine and not flagged.
  void check_dup_modtypes_mt(const ModuleType& mt) {
    if (auto* sg = std::get_if<Pmty_signature>(&mt.desc)) check_dup_modtypes_sig(sg->items);
    else if (auto* fn = std::get_if<Pmty_functor>(&mt.desc)) check_dup_modtypes_mt(*fn->body);
  }
  void check_dup_modtypes_me(const ModuleExpr& me) {
    if (auto* ms = std::get_if<Pmod_structure>(&me.desc)) check_dup_modtypes_struct(ms->items);
    else if (auto* mc = std::get_if<Pmod_constraint>(&me.desc)) {
      check_dup_modtypes_me(*mc->me); check_dup_modtypes_mt(*mc->mt);
    } else if (auto* mf = std::get_if<Pmod_functor>(&me.desc)) check_dup_modtypes_me(*mf->body);
  }
  void check_dup_modtypes_sig(const ast::Signature& items) {
    std::set<std::string> seen, seen_class, seen_classty, seen_module;
    for (auto& it : items) {
      if (auto* mt = std::get_if<Psig_modtype>(&it.desc)) {
        if (!seen.insert(mt->name.txt).second)
          note_error("Multiple definition of the module type name " + mt->name.txt);
        if (mt->type) check_dup_modtypes_mt(*mt->type);
      } else if (auto* md = std::get_if<Psig_module>(&it.desc)) {
        dup_module_name(seen_module, md->md.name);
        check_dup_modtypes_mt(*md->md.type);
      } else if (auto* rm = std::get_if<Psig_recmodule>(&it.desc)) {
        for (auto& d : rm->decls) { dup_module_name(seen_module, d.name); check_dup_modtypes_mt(*d.type); }
      } else if (auto* cl = std::get_if<Psig_class>(&it.desc)) {
        for (auto& d : cl->decls)
          if (!seen_class.insert(d.name.txt).second)
            note_error("Multiple definition of the class name " + d.name.txt);
      } else if (auto* ct = std::get_if<Psig_class_type>(&it.desc)) {
        for (auto& d : ct->decls)
          if (!seen_classty.insert(d.name.txt).second)
            note_error("Multiple definition of the class type name " + d.name.txt);
      }
    }
  }
  // A named module bound twice in the same structure/signature is an error
  // ("Multiple definition of the module name M").  A wildcard `module _` may
  // repeat, and an INCLUDE'd module may be shadowed by an explicit one -- so
  // only explicit, named bindings are tracked.
  void dup_module_name(std::set<std::string>& seen, const StrOptLoc& n) {
    if (n.txt && *n.txt != "_" && !seen.insert(*n.txt).second)
      note_error("Multiple definition of the module name " + *n.txt);
  }
  void check_dup_modtypes_struct(const ast::Structure& items) {
    if (!strict) return;
    std::set<std::string> seen, seen_class, seen_classty, seen_module;
    for (auto& it : items) {
      if (auto* mt = std::get_if<Pstr_modtype>(&it.desc)) {
        if (!seen.insert(mt->name.txt).second)
          note_error("Multiple definition of the module type name " + mt->name.txt);
        if (mt->type) check_dup_modtypes_mt(*mt->type);
      } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
        dup_module_name(seen_module, mb->binding.name);
        check_dup_modtypes_me(mb->binding.expr);
      } else if (auto* rm = std::get_if<Pstr_recmodule>(&it.desc)) {
        for (auto& b : rm->bindings) { dup_module_name(seen_module, b.name); check_dup_modtypes_me(b.expr); }
      } else if (auto* in = std::get_if<Pstr_include>(&it.desc)) {
        check_dup_modtypes_me(in->expr);
      } else if (auto* cl = std::get_if<Pstr_class>(&it.desc)) {
        for (auto& d : cl->decls)
          if (!seen_class.insert(d.name.txt).second)
            note_error("Multiple definition of the class name " + d.name.txt);
      } else if (auto* ct = std::get_if<Pstr_class_type>(&it.desc)) {
        for (auto& d : ct->decls)
          if (!seen_classty.insert(d.name.txt).second)
            note_error("Multiple definition of the class type name " + d.name.txt);
      }
    }
  }

  // An object override `{< l = e; .. >}` may only set instance variables of the
  // enclosing object.  Checked syntactically and conservatively: only when the
  // object has NO `inherit` (so its val set is complete -- inheritance could
  // bring in more vars we don't see) and only for an override that is directly a
  // method/initializer body (no general expression walk needed for the corpus).
  void check_object_overrides_cs(const ast::ClassStructure& cs) {
    for (auto& f : cs.fields)
      if (std::holds_alternative<Pcf_inherit>(f.desc)) return;  // unknown inherited vals
    std::set<std::string> vals;
    for (auto& f : cs.fields)
      if (auto* v = std::get_if<Pcf_val>(&f.desc)) vals.insert(v->name.txt);
    for (auto& f : cs.fields) {
      const Expression* body = nullptr;
      if (auto* m = std::get_if<Pcf_method>(&f.desc)) {
        if (auto* cc = std::get_if<Cfk_concrete>(&m->kind)) body = cc->e.get();
      } else if (auto* ini = std::get_if<Pcf_initializer>(&f.desc)) body = ini->e.get();
      if (!body) continue;
      if (auto* poly = std::get_if<Pexp_poly>(&body->desc)) body = poly->e.get();
      if (auto* ov = std::get_if<Pexp_override>(&body->desc))
        for (auto& [lbl, e] : ov->fields)
          if (!vals.count(lbl.txt)) {
            note_error("Unbound instance variable " + lbl.txt);
            return;
          }
    }
  }
  void check_object_overrides_ce(const ast::ClassExpr& ce) {
    const ast::ClassExpr* c = &ce;
    while (true) {  // unwrap class fun/let/constraint wrappers to the structure
      if (auto* fn = std::get_if<Pcl_fun>(&c->desc)) c = fn->body.get();
      else if (auto* lt = std::get_if<Pcl_let>(&c->desc)) c = lt->body.get();
      else if (auto* cn = std::get_if<Pcl_constraint>(&c->desc)) c = cn->ce.get();
      else break;
    }
    if (auto* st = std::get_if<Pcl_structure>(&c->desc)) check_object_overrides_cs(st->cs);
  }
  void check_object_overrides(const ast::Structure& items) {
    if (!strict) return;
    for (auto& it : items) {
      if (auto* cl = std::get_if<Pstr_class>(&it.desc)) {
        for (auto& d : cl->decls) check_object_overrides_ce(d.expr);
      } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
        const ModuleExpr* me = &mb->binding.expr;
        while (auto* mc = std::get_if<Pmod_constraint>(&me->desc)) me = mc->me.get();
        if (auto* ms = std::get_if<Pmod_structure>(&me->desc)) check_object_overrides(ms->items);
      }
    }
  }

  // A `module rec` group can hide a cyclic type abbreviation that the file-local
  // check misses, because the self-reference is *qualified* through the module
  // being defined (`module rec A : sig type t = A.t end`).  Build the abbreviation
  // graph over qualified names (Mod.t) within the group -- following manifest
  // edges only, like check_cyclic_aliases -- and reject a cycle.  Purely
  // syntactic, so it stays sound under the otherwise-error-suppressed recursion.
  void check_recmodule_cyclic_types(const Pstr_recmodule& rm) {
    if (!strict) return;
    struct QAlias { std::string mod; const CoreType* manifest; };
    std::unordered_map<std::string, QAlias> aliases;  // "Mod.t" -> defn (manifest only)
    std::set<std::string> group_types;                // every "Mod.t" in the group
    auto add_decls = [&](const std::string& mod, const std::vector<TypeDeclaration>& ds) {
      for (auto& d : ds) {
        group_types.insert(mod + "." + d.name.txt);
        if (d.manifest) aliases[mod + "." + d.name.txt] = {mod, d.manifest->get()};
      }
    };
    for (auto& b : rm.bindings) {
      if (!b.name.txt) continue;
      const std::string& mod = *b.name.txt;
      // Prefer the ascribed signature's type decls; else the struct body's.
      const ast::Signature* sg = nullptr;
      const ModuleExpr* m = &b.expr;
      while (auto* mc = std::get_if<Pmod_constraint>(&m->desc)) {
        if (auto* s = std::get_if<Pmty_signature>(&mc->mt->desc)) { sg = &s->items; break; }
        m = mc->me.get();
      }
      if (sg) {
        for (auto& si : *sg)
          if (auto* st = std::get_if<Psig_type>(&si.desc)) add_decls(mod, st->decls);
      } else if (auto* ms = std::get_if<Pmod_structure>(&m->desc)) {
        for (auto& it : ms->items)
          if (auto* pt = std::get_if<Pstr_type>(&it.desc)) add_decls(mod, pt->decls);
      }
    }
    if (aliases.empty()) return;
    // Qualified refs of a manifest (a bare name resolves within its own module).
    std::function<void(const CoreType&, const std::string&, std::set<std::string>&)> refs =
        [&](const CoreType& t, const std::string& cur, std::set<std::string>& out) {
          if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
            std::string q;
            if (auto* l = std::get_if<Lident>(&c->id.txt.v)) q = cur + "." + l->name;
            else if (auto* d = std::get_if<Ldot>(&c->id.txt.v)) {
              if (auto* p = std::get_if<Lident>(&d->prefix->v)) q = p->name + "." + d->name;
            }
            if (!q.empty() && aliases.count(q)) out.insert(q);
            for (auto& a : c->args) refs(*a, cur, out);
          } else if (auto* a = std::get_if<Ptyp_arrow>(&t.desc)) {
            refs(*a->dom, cur, out); refs(*a->cod, cur, out);
          } else if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
            for (auto& e : tu->elems) refs(*e, cur, out);
          } else if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) {
            refs(*al->type, cur, out);
          }
        };
    std::unordered_map<std::string, std::set<std::string>> graph;
    for (auto& [q, a] : aliases) refs(*a.manifest, a.mod, graph[q]);
    for (auto& [start, a] : aliases) {
      std::set<std::string> seen;
      std::function<bool(const std::string&)> reaches = [&](const std::string& cur) -> bool {
        auto it = graph.find(cur);
        if (it == graph.end()) return false;
        for (auto& nx : it->second) {
          if (nx == start) return true;
          if (seen.insert(nx).second && reaches(nx)) return true;
        }
        return false;
      };
      if (reaches(start)) { note_error("The type abbreviation \"" + start + "\" is cyclic"); break; }
    }

    // Regularity (subtle): a recursive group type may only be applied to its own
    // parameter variables.  Applying a group type N to a *non-variable* argument
    // is non-regular ONLY when N is mutually recursive with the type whose
    // manifest contains the application (same strongly-connected component of the
    // group-reference graph) -- otherwise it is an ordinary application of a
    // settled type (t10ok: `A.t = 'a list B.t` is fine because B never refers
    // back to A, so the parameter cannot grow without bound).
    auto qname = [](const Ptyp_constr& c, const std::string& cur) -> std::string {
      if (auto* l = std::get_if<Lident>(&c.id.txt.v)) return cur + "." + l->name;
      if (auto* d = std::get_if<Ldot>(&c.id.txt.v))
        if (auto* p = std::get_if<Lident>(&d->prefix->v)) return p->name + "." + d->name;
      return "";
    };
    using Visit = std::function<void(const Ptyp_constr&)>;
    std::function<void(const CoreType&, const Visit&)> descend =
        [&](const CoreType& t, const Visit& visit) {
          if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
            visit(*c);
            for (auto& a : c->args) descend(*a, visit);
          } else if (auto* a = std::get_if<Ptyp_arrow>(&t.desc)) {
            descend(*a->dom, visit); descend(*a->cod, visit);
          } else if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
            for (auto& e : tu->elems) descend(*e, visit);
          } else if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) {
            descend(*al->type, visit);
          } else if (auto* po = std::get_if<Ptyp_poly>(&t.desc)) {
            descend(*po->type, visit);
          } else if (auto* ob = std::get_if<Ptyp_object>(&t.desc)) {
            for (auto& f : ob->fields) {
              if (auto* ot = std::get_if<Otag>(&f)) descend(*ot->type, visit);
              else if (auto* oi = std::get_if<Oinherit>(&f)) descend(*oi->type, visit);
            }
          }
        };
    // Group-reference graph (any reference, regardless of arguments).
    std::unordered_map<std::string, std::set<std::string>> gref;
    for (auto& [q, a] : aliases)
      descend(*a.manifest, [&, mod = a.mod, key = q](const Ptyp_constr& c) {
        std::string n = qname(c, mod);
        if (!n.empty() && group_types.count(n)) gref[key].insert(n);
      });
    auto reaches = [&](const std::string& s, const std::string& tgt) {
      std::set<std::string> seen; std::vector<std::string> st{s};
      while (!st.empty()) {
        std::string x = st.back(); st.pop_back();
        auto it = gref.find(x); if (it == gref.end()) continue;
        for (auto& nx : it->second) {
          if (nx == tgt) return true;
          if (seen.insert(nx).second) st.push_back(nx);
        }
      }
      return false;
    };
    bool non_regular = false;
    for (auto& [q, a] : aliases) {
      descend(*a.manifest, [&, mod = a.mod, key = q](const Ptyp_constr& c) {
        std::string n = qname(c, mod);
        if (n.empty() || !group_types.count(n)) return;
        bool concrete = false;
        for (auto& arg : c.args)
          if (!std::get_if<Ptyp_var>(&arg->desc) && !std::get_if<Ptyp_any>(&arg->desc)) concrete = true;
        if (concrete && (n == key || (reaches(key, n) && reaches(n, key)))) non_regular = true;
      });
      if (non_regular) break;
    }
    if (non_regular) note_error("This recursive type is not regular");
  }

  // `[@@unboxed]` is valid only on a single-constructor variant whose
  // constructor takes exactly one argument, or a single-field record.  Flag the
  // clear count violations (sound: a valid 1-arg/1-field type is never flagged).
  void check_unboxed(const TypeDeclaration& d) {
    if (!strict) return;
    bool unboxed = false;
    for (auto& a : d.attrs) if (a.name == "unboxed" || a.name == "ocaml.unboxed") unboxed = true;
    if (!unboxed) return;
    bool bad = false;
    if (auto* v = std::get_if<Ptype_variant>(&d.kind)) {
      if (v->ctors.size() != 1) bad = true;
      else if (!v->ctors[0].res) {  // skip GADT constructors (subtler rules)
        if (auto* tup = std::get_if<Pcstr_tuple>(&v->ctors[0].args)) bad = tup->elems.size() != 1;
        else if (auto* r = std::get_if<Pcstr_record>(&v->ctors[0].args)) bad = r->fields.size() != 1;
      }
    } else if (auto* rec = std::get_if<Ptype_record>(&d.kind)) {
      bad = rec->fields.size() != 1;
    } else {
      return;  // abstract / open: not a count violation we can judge
    }
    if (bad)
      note_error("This type cannot be unboxed because it must have exactly one "
                 "constructor with a single argument, or one field");
  }

  // Register a user variant: A of t1*..*tn -> scheme t1->..->tn->(params) name.
  void register_type_decl(const TypeDeclaration& d) {
    check_type_vars(d);
    check_unboxed(d);
    type_arity[d.name.txt] = (int)d.params.size();
    // A top-level decl shadowing a stdlib variant type's name (`type fpclass =
    // A` over float's fpclass): the stdlib ctors' schemes requalify so their
    // displays stay unambiguous -- `Stdlib.fpclass`, or `Stdlib/2.fpclass`
    // when the file also binds its own `module Stdlib` (ocamlc's out-of-scope
    // marker).  Unify compares constr paths by last component, so the rewrite
    // cannot introduce a clash; the immediate (int) kind is carried over.
    if (mod_prefix_.empty())
      if (auto sc = stdlib_ctor_types_.find(d.name.txt);
          sc != stdlib_ctor_types_.end()) {
        std::string q =
            (user_stdlib_module_ ? "Stdlib/2." : "Stdlib.") + d.name.txt;
        for (auto& cn : sc->second)
          if (auto ci = ctors.find(cn); ci != ctors.end()) {
            TypePtr t = I::Engine::repr(ci->second);
            if (t->kind == I::Type::Kind::Constr && t->path == d.name.txt)
              t->path = q;
          }
        if (immediate_types_.count(d.name.txt)) immediate_types_.insert(q);
        stdlib_keep_paths_.insert(q);
      }
    // Opaque types (variant/record/abstract-without-manifest) have a distinct
    // identity; pure abbreviations are transparent (expanded), so unstamped.
    bool opaque = std::holds_alternative<Ptype_variant>(d.kind) ||
                  std::holds_alternative<Ptype_record>(d.kind) ||
                  (std::holds_alternative<Ptype_abstract>(d.kind) && !d.manifest);
    // A module re-exported bare by a top-level `include A` displays its type
    // names unqualified (ocamlc shows the included name).
    bool inc = included_module_prefixes_.count(mod_prefix_) != 0;
    if (opaque) {
      type_stamp_[&d] = next_type_stamp_++;
      if (!mod_prefix_.empty() && !inc)
        stamp_path_[type_stamp_[&d]] = mod_prefix_ + d.name.txt;
      stamp_type_decl_[type_stamp_[&d]] = &d;  // decl AST by identity (mcomp-lite)
      stamp_ctor_key_[type_stamp_[&d]] = mod_prefix_ + d.name.txt;
      qual_type_stamp_[mod_prefix_ + d.name.txt] = type_stamp_[&d];
      auto [bu, ins] = bare_unique_stamp_.emplace(d.name.txt, type_stamp_[&d]);
      if (!ins) bu->second = -1;  // bare name declared twice -> ambiguous
    }
    if (d.manifest) {  // `type (params) t = <manifest>`: a type abbreviation
      std::vector<std::string> ps;
      for (auto& p : d.params)
        ps.push_back(std::holds_alternative<Ptyp_var>(p->desc)
                         ? std::get<Ptyp_var>(p->desc).name : "");
      Alias na = {std::move(ps), d.manifest->get(),
                  d.constraints.empty() ? nullptr : &d.constraints,
                  mod_prefix_.empty() || inc ? "" : mod_prefix_ + d.name.txt};
      auto [f, ins] = type_aliases.emplace(d.name.txt, na);
      // A TOP-LEVEL alias keeps priority in the flat map over a module-NESTED
      // same-named one: a bare `t` outside the module means the top-level t
      // (core_array's `'a t` vs the nested Permissioned.Int.t).
      if (!ins && !(f->second.display_path.empty() && !na.display_path.empty()))
        f->second = std::move(na);
    }
    auto* v = std::get_if<Ptype_variant>(&d.kind);
    if (!v) return;
    if (d.priv == PrivateFlag::Private)
      for (auto& c : v->ctors) {
        private_variant_ctors_.insert(c.name.txt);
        private_ctor_type_[c.name.txt] = mod_prefix_ + d.name.txt;
      }
    std::vector<std::string> names;
    bool is_gadt = false, all_const = !v->ctors.empty();
    for (auto& c : v->ctors) {
      names.push_back(c.name.txt);
      if (c.res) is_gadt = true;
      auto* tup = std::get_if<Pcstr_tuple>(&c.args);
      if (!tup || !tup->elems.empty()) all_const = false;  // a block constructor
    }
    // An all-constant variant is immediate (its constructors are ints) -- this
    // holds for an all-constant GADT too (`Int : _ typ | Ptr : _ typ`), so the
    // immediacy does not depend on is_gadt.
    if (all_const) immediate_types_.insert(d.name.txt);
    // `[@@unboxed]` of a single immediate field shares its int representation, so
    // a value of the type gets the [int] value kind too.
    bool unboxed = false;
    for (auto& a : d.attrs) if (a.name == "unboxed" || a.name == "ocaml.unboxed") unboxed = true;
    if (unboxed && v->ctors.size() == 1)
      if (auto* tup = std::get_if<Pcstr_tuple>(&v->ctors[0].args))
        if (tup->elems.size() == 1)
          if (auto* c = std::get_if<Ptyp_constr>(&tup->elems[0]->desc)) {
            std::string n = lid_last(c->id.txt);
            if (n == "int" || n == "char" || n == "bool" || n == "unit" ||
                immediate_types_.count(n))
              immediate_types_.insert(d.name.txt);
          }
    if (is_gadt) {
      gadt_types.insert(d.name.txt);
      // A module-nested GADT redefining a predef ctor name (GPR#234's
      // `type hlist = [] : hlist | (::) : ..`) must not turn every outer
      // list-pattern match into a windowed GADT match -- the nested name
      // never shadows the outer scope.
      for (auto& c : v->ctors)
        if (mod_prefix_.empty() || !predef_ctors_.count(c.name.txt))
          gadt_ctors.insert(c.name.txt);
      // A constructor whose argument mentions a type variable absent from its
      // result type introduces an existential (`Any : 'a -> any`).
      for (auto& c : v->ctors) {
        if (!c.res) continue;
        bool unc = false;
        std::set<std::string> argv, resv;
        if (auto* tup = std::get_if<Pcstr_tuple>(&c.args))
          for (auto& e : tup->elems) collect_tyvars(*e, argv, unc);
        else if (auto* r = std::get_if<Pcstr_record>(&c.args))
          for (auto& f : r->fields) collect_tyvars(*f.type, argv, unc);
        collect_tyvars(**c.res, resv, unc);
        if (unc) continue;  // uncertain -> conservatively don't flag (never false-reject)
        for (auto& vn : argv)
          if (!resv.count(vn)) { existential_ctors_.insert(c.name.txt); break; }
      }
    }
    type_ctors[d.name.txt] = std::move(names);
    for (auto& c : v->ctors) {
      std::unordered_map<std::string, TypePtr> vars;
      std::vector<TypePtr> params;
      for (auto& p : d.params) params.push_back(from_coretype(*p, vars));
      // A GADT constructor's explicit result (`Float : float -> float dyn`)
      // refines the type's parameters, so a pattern `Float x` types its scrutinee
      // as `float dyn`, not the generic `'a dyn`.  Non-strict only: in the strict
      // pass the per-branch refinement needs windowing we keep conservative.
      TypePtr result = (!strict && c.res) ? from_coretype(**c.res, vars)
                                          : eng.constr(mod_prefix_ + d.name.txt, params, type_stamp_[&d]);
      TypePtr scheme = result;
      if (auto* tup = std::get_if<Pcstr_tuple>(&c.args)) {
        for (auto it = tup->elems.rbegin(); it != tup->elems.rend(); ++it)
          scheme = eng.arrow(from_coretype(**it, vars), scheme);
      }
      register_inline_record(c.args, result, vars);  // `C of { f : t }`
      bool nested_predef = !mod_prefix_.empty() && predef_ctors_.count(c.name.txt);
      if (ctors.count(c.name.txt)) {
        ambiguous_ctors_.insert(c.name.txt);
        if (predef_ctors_.count(c.name.txt) && mod_prefix_.empty())
          predef_toplevel_redef_.insert(c.name.txt);
      }
      if (!nested_predef) ctors[c.name.txt] = scheme;
      ctor_scheme_[&c] = scheme;  // for scoped (in-order) resolution via cenv
      type_ctor_schemes_[mod_prefix_ + d.name.txt].emplace_back(c.name.txt, scheme);
    }
  }

  // A GADT type declared inside an expression-level local structure (`let open
  // struct type _ t = C : (_ -> _) t .. end in match ..`) is never seen by the
  // top-level register_types_rec pass, so its ctors miss `gadt_ctors` and the
  // match on them isn't windowed (its branch-local refinement then leaks into
  // the result).  Register just the GADT markers (names) here so the window
  // fires; the ctors' value schemes come from process_item's open handling.
  void register_local_gadt_markers(const ast::Structure& items) {
    for (auto& it : items)
      if (auto* ty = std::get_if<Pstr_type>(&it.desc))
        for (auto& d : ty->decls)
          if (auto* v = std::get_if<Ptype_variant>(&d.kind)) {
            bool is_gadt = false;
            for (auto& c : v->ctors) if (c.res) is_gadt = true;
            if (is_gadt) {
              gadt_types.insert(d.name.txt);
              for (auto& c : v->ctors) gadt_ctors.insert(c.name.txt);
            }
          }
  }

  // Collect a record type's field schemes: label -> arrow((params) t, field),
  // sharing the params and carrying the type's identity stamp.  Uniqueness across
  // all record types is resolved later in finalize_fields.
  void register_record_decl(const TypeDeclaration& d) {
    auto* rec = std::get_if<Ptype_record>(&d.kind);
    if (!rec) return;
    std::unordered_map<std::string, TypePtr> vars;
    std::vector<TypePtr> params;
    for (auto& p : d.params) params.push_back(from_coretype(*p, vars));
    TypePtr recTy = eng.constr(mod_prefix_ + d.name.txt, params, type_stamp_[&d]);
    if (int s = type_stamp_[&d]) stamp_record_decl_[s] = &d;  // for ambiguous-field resolution
    if (auto it = name_record_decl_.find(d.name.txt);  // by-name fallback (only when UNIQUE)
        it != name_record_decl_.end() && it->second != &d)
      ambiguous_record_names_.insert(d.name.txt);
    name_record_decl_[d.name.txt] = &d;
    for (auto& f : rec->fields) {
      if (d.priv == PrivateFlag::Private) {
        private_record_fields_.insert(f.name.txt);
        private_field_type_[f.name.txt] = mod_prefix_ + d.name.txt;
      } else
        nonprivate_record_fields_.insert(f.name.txt);
    }
    for (auto& f : rec->fields) {
      // a universally-quantified field (`{ f : 'a. ... }`) is polymorphic per use;
      // a single monomorphic scheme would clash, so leave it to Any -- EXCEPT, in
      // the KIND pass only, a format-typed field (`{ pf : 'a. ('a,..) format ->
      // 'a }`): its uses must type string literals at format type or they stay
      // unlowered (= segfault); cross-use clashes are soft there.  The strict
      // pass keeps the skip (a monomorphic scheme false-rejects valid reuses).
      if (std::holds_alternative<Ptyp_poly>(f.type->desc) &&
          !(record_kinds_ && mentions_format(*f.type))) {
        poly_field_rec_candidates_[f.name.txt].push_back({recTy, &*f.type});  // pattern record-type resolution
        continue;
      }
      field_candidates_[f.name.txt].push_back(eng.arrow(recTy, from_coretype(*f.type, vars)));
    }
  }
  static bool mentions_format(const CoreType& t) {
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      if (is_format_base(lid_full(c->id.txt))) return true;
      for (auto& a : c->args) if (mentions_format(*a)) return true;
      return false;
    }
    if (auto* pl = std::get_if<Ptyp_poly>(&t.desc)) return mentions_format(*pl->type);
    if (auto* ar = std::get_if<Ptyp_arrow>(&t.desc))
      return mentions_format(*ar->dom) || mentions_format(*ar->cod);
    if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
      for (auto& el : tu->elems) if (mentions_format(*el)) return true;
      return false;
    }
    return false;
  }
  // Keep only labels unique across all record types (others need type-direction).
  void finalize_fields() {
    for (auto& [k, v] : field_candidates_)
      if (v.size() == 1) fields_[k] = v[0];
    // A poly-field label maps to its record type iff it is unique AND not also a
    // (mono) field elsewhere -- otherwise the label is ambiguous, leave to Any.
    for (auto& [k, v] : poly_field_rec_candidates_)
      if (v.size() == 1 && !field_candidates_.count(k)) poly_field_rec_[k] = v.front();
  }

  // An inline-record constructor argument (`C of { f : t; .. }`): register each
  // field so a pattern `C { f }` / expression `C { f = e }` resolves f to its
  // declared type instead of Any.  Mirrors register_record_decl's field handling
  // (a universally-quantified field stays Any, save a format one in the kind
  // pass); `result` is the constructor's result type, used as the field's
  // record-type domain.
  void register_inline_record(const ConstructorArguments& args, const TypePtr& result,
                              std::unordered_map<std::string, TypePtr>& vars) {
    auto* r = std::get_if<Pcstr_record>(&args);
    if (!r) return;
    for (auto& f : r->fields) {
      if (std::holds_alternative<Ptyp_poly>(f.type->desc) &&
          !(record_kinds_ && mentions_format(*f.type)))
        continue;
      field_candidates_[f.name.txt].push_back(
          eng.arrow(result, from_coretype(*f.type, vars)));
    }
  }

  // Register an exception/extension constructor: A of t1*..*tn => t1->..->tn->exn.
  // Participates in ambiguity detection so `exception E` + `type t = E` makes E
  // ambiguous (type-directed disambiguation, approximated as unknown).
  void register_exception(const ExtensionConstructor& ec) {
    if (!ext_ctor_registered_.insert(&ec).second) return;
    TypePtr scheme = eng.constr("exn");
    if (auto* d = std::get_if<Pext_decl>(&ec.kind)) {
      std::unordered_map<std::string, TypePtr> vars;
      if (auto* tup = std::get_if<Pcstr_tuple>(&d->args))
        for (auto it = tup->elems.rbegin(); it != tup->elems.rend(); ++it)
          scheme = eng.arrow(from_coretype(**it, vars), scheme);
      register_inline_record(d->args, eng.constr("exn"), vars);  // `exception E of { f }`
    }
    if (ctors.count(ec.name.txt)) ambiguous_ctors_.insert(ec.name.txt);
    ctors[ec.name.txt] = scheme;
    exn_ctors_.insert(ec.name.txt);
  }

  // A type extension `type ('a..) path += C [of args] [: res]` (extensible
  // variants and effects, e.g. `type _ t += E : unit t`).  Each constructor's
  // result is its explicit GADT result (`: unit t`) or the extended type applied
  // to the extension's parameters; `type exn += ..` are exceptions.  Typing these
  // lets `perform E`/`E` flow a real type instead of Any (effect return kinds).
  void register_typext(const TypeExtension& te) {
    bool is_exn = lid_last(te.path.txt) == "exn";
    // A non-exn rebind (`type 'a Msg.tag += String = StrM.C`) shares the target
    // ctor's scheme under the new name.  Outside the once-guard: the target may
    // only resolve in the in-order pass (e.g. a functor instance's cenv binding
    // made after the up-front registration already visited this node).  Into
    // the scoped cenv (the flat map would just mark the name ambiguous).  Exn
    // rebinds stay unknown as before (find_ctor skips cenv for exn names).
    // Non-strict only: the strict pass would resolve the target to a functor
    // BODY's unsubstituted scheme (D.t) and false-reject its uses.
    if (!is_exn && !strict)
      for (auto& ec : te.ctors)
        if (auto* rb = std::get_if<Pext_rebind>(&ec.kind))
          if (!ext_rebind_registered_.count(&ec))
            if (TypePtr* t = find_ctor(lid_last(rb->id.txt))) {
              ext_rebind_registered_.insert(&ec);
              cenv.back()[ec.name.txt] = *t;
            }
    if (!ext_ctor_registered_.insert(&te).second) return;
    for (auto& ec : te.ctors) {
      auto* d = std::get_if<Pext_decl>(&ec.kind);
      if (!d) continue;  // Pext_rebind (`+= C = M.C`): leave unknown
      std::unordered_map<std::string, TypePtr> vars;
      TypePtr result;
      if (d->res) result = from_coretype(**d->res, vars);
      else {
        std::vector<TypePtr> params;
        for (auto& p : te.params) params.push_back(from_coretype(*p, vars));
        result = is_exn ? eng.constr("exn") : eng.constr(lid_last(te.path.txt), params);
      }
      TypePtr scheme = result;
      if (auto* tup = std::get_if<Pcstr_tuple>(&d->args))
        for (auto it = tup->elems.rbegin(); it != tup->elems.rend(); ++it)
          scheme = eng.arrow(from_coretype(**it, vars), scheme);
      register_inline_record(d->args, result, vars);  // `type t += C of { f }`
      if (ctors.count(ec.name.txt)) ambiguous_ctors_.insert(ec.name.txt);
      ctors[ec.name.txt] = scheme;
      if (is_exn) exn_ctors_.insert(ec.name.txt);
    }
  }

  // Look up a constructor scheme.  The scoped cenv (in-order, module-scoped)
  // takes priority -- it resolves a name reused across local types to the
  // in-scope declaration.  Only when a name isn't in scope do we fall back to
  // the flat map, treating cross-module duplicates as ambiguous (unknown).
  TypePtr* find_ctor(const std::string& name) {
    // A variant constructor reusing a predef/exception name needs type-directed
    // disambiguation we don't have -> leave unknown rather than pick wrong.
    if (!predef_ctors_.count(name) && !exn_ctors_.count(name))
      for (auto it = cenv.rbegin(); it != cenv.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
      }
    // An exception constructor that collides with a stdlib variant ctor (a user
    // `exception Error of string` vs `Result.Error`) is marked ambiguous, but the
    // top-level exception definition SHADOWS the stdlib ctor for an unqualified
    // `Error` -- so in the non-strict passes resolve it to the (last-registered)
    // exn scheme rather than bailing to Any.  The strict reject pass keeps bailing
    // (an incorrect pick could false-reject).  Likewise a predef ctor whose only
    // redefinitions are module-NESTED (GPR#234's `type hlist = [] | (::)`) stays
    // resolvable non-strict: the nested decl never shadowed the outer name.
    bool predef_intact =
        predef_ctors_.count(name) && !predef_toplevel_redef_.count(name);
    if (ambiguous_ctors_.count(name) &&
        (strict || (!exn_ctors_.count(name) && !predef_intact)))
      return nullptr;
    auto it = ctors.find(name);
    return it == ctors.end() ? nullptr : &it->second;
  }

  // Resolve a QUALIFIED constructor `M.C` (M a single, non-opened module) to its
  // owning variant type `M.tname`, read from M's cmi.  `M.C` otherwise types as
  // Any (find_ctor only knows bare names), which loses the type that an optional-
  // argument default pins: predef's `decl0 ?(immediate = Type_immediacy.Unknown)`
  // gives `immediate : Type_immediacy.t`, type-directing a later `~immediate:Always`.
  // The tuple-arg arity of a qualified constructor `M.C` read straight from M's
  // cmi (0 when not found / not a Lident-qualified variant) -- so the dump can
  // flatten `M.C (a, b)` even when C isn't in the inferencer's ctor scope.
  int qualified_ctor_arity(const Longident& id) {
    auto* d = std::get_if<Ldot>(&id.v);
    if (!d) return 0;
    auto* pl = std::get_if<Lident>(&d->prefix->v);
    if (!pl) return 0;
    try {
      auto cmi = cmi::CmiFile::load(head_cmi(pl->name));
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Variant) continue;
        for (auto& c : td.ctors)
          if (c.name == d->name) return (int)c.args.size();
      }
      // Module-level extension ctors (`exception Unix_error of error * string
      // * string`) flatten by the same rule as variant ctors.
      for (auto& x : cmi.sig().typexts)
        if (x.name == d->name) return (int)x.args.size();
    } catch (...) {}
    return 0;
  }

  // The parameter count of a qualified type `M.t` (M possibly deep, possibly a
  // file-local alias of an external path like `module MP = Gc.Memprof`), read
  // by navigating the cmis.  0 when unresolvable.
  int qualified_type_arity(const Longident& id) {
    auto* d = std::get_if<Ldot>(&id.v);
    if (!d) return 0;
    auto comps = mod_components(*d->prefix);
    if (comps.empty()) return 0;
    for (auto& [tgt, al] : module_aliases_)
      if (al == comps[0]) {
        std::vector<std::string> tc = mod_components_str(tgt);
        tc.insert(tc.end(), comps.begin() + 1, comps.end());
        comps = std::move(tc);
        break;
      }
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig)
        for (auto& td : sig->types)
          if (td.name == d->name) return (int)td.params.size();
    } catch (...) {}
    return 0;
  }

  // The ordered field names of an external record type named by a dotted path
  // ("Gc.Memprof.tracker"), found by navigating the cmis (head cmi then nested
  // submodule signatures).  Empty when not a cmi-resolvable record.
  // Is a cmi label type the predefined `float` (a bare `Tconstr float`)?  Drives
  // the all-float record -> Record_float rule for external records.
  static bool cmi_is_float(const cmi::TypePtr& t) {
    if (!t || t->kind != cmi::TypeExpr::Tconstr || !t->args.empty()) return false;
    return t->path && cmi_path_str(*t->path) == "float";
  }

  std::vector<std::string> cmi_record_fields(const std::string& path,
                                             std::string* out_repr = nullptr) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return {};
    std::string tyname = path.substr(dot + 1), modpath = path.substr(0, dot);
    std::vector<std::string> comps;
    for (size_t i = 0;;) {
      size_t d = modpath.find('.', i);
      if (d == std::string::npos) { comps.push_back(modpath.substr(i)); break; }
      comps.push_back(modpath.substr(i, d - i)); i = d + 1;
    }
    try {
      auto cmi = cmi::CmiFile::load(head_cmi(comps[0]));
      const std::vector<cmi::TypeDecl>* types = &cmi.types();
      const std::vector<cmi::ModuleDecl>* modules = &cmi.modules();
      for (size_t k = 1; k < comps.size(); ++k) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& m : *modules) if (m.name == comps[k]) { md = &m; break; }
        if (!md || !md->type || md->type->kind != cmi::ModuleType::Sig ||
            !md->type->sig) return {};
        types = &md->type->sig->types;
        modules = &md->type->sig->modules;
      }
      for (auto& td : *types)
        if (td.name == tyname && td.kind == cmi::TypeDecl::Record) {
          std::vector<std::string> fs;
          bool all_float = !td.labels.empty();
          for (auto& l : td.labels) {
            fs.push_back(l.name);
            if (!cmi_is_float(l.type)) all_float = false;
          }
          if (out_repr && all_float) *out_repr = "Record_float";
          return fs;
        }
    } catch (...) {}
    return {};
  }

  TypePtr qualified_ctor_type(const Longident& id) {
    auto* d = std::get_if<Ldot>(&id.v);
    if (!d) return nullptr;
    auto* pl = std::get_if<Lident>(&d->prefix->v);
    if (!pl) return nullptr;  // nested-module qualifier: best-effort skip
    try {
      auto cmi = cmi::CmiFile::load(head_cmi(pl->name));
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Variant) continue;
        for (auto& c : td.ctors)
          if (c.name == d->name) {
            std::vector<TypePtr> params;
            for (int i = 0; i < td.arity; ++i) params.push_back(eng.fresh_var());
            return eng.constr(pl->name + "." + td.name, std::move(params));
          }
      }
    } catch (...) {}
    return nullptr;
  }

  // Full scheme of a qualified constructor `M.C` (arg1->..->result), so an
  // application `Either.Left "s"` pins the parameter (`(string, 'b) Either.t`),
  // not just the bare variant type.  Null when M.C isn't a loadable variant ctor.
  TypePtr qualified_ctor_scheme(const Longident& id) {
    auto* d = std::get_if<Ldot>(&id.v);
    if (!d) return nullptr;
    auto comps = mod_components(*d->prefix);
    if (comps.empty()) return nullptr;
    // The head may be an OPENED submodule (`open Runtime_events` then
    // `Type.Begin` -> Runtime_events.Type) or a file-local alias.
    if (auto q = opened_submod_quals_.find(comps[0]);
        q != opened_submod_quals_.end()) {
      std::vector<std::string> qc = mod_components_str(q->second);
      qc.insert(qc.end(), comps.begin() + 1, comps.end());
      comps = std::move(qc);
    } else
      for (auto& [tgt, al] : module_aliases_)
        if (al == comps[0]) {
          std::vector<std::string> tc = mod_components_str(tgt);
          tc.insert(tc.end(), comps.begin() + 1, comps.end());
          comps = std::move(tc);
          break;
        }
    try {
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back().sig();
      // Parent scopes: a ctor-arg type owned by an enclosing module qualifies
      // to its own module, as in resolve_module_values.
      std::vector<std::pair<const std::vector<cmi::TypeDecl>*, std::string>> scopes;
      std::vector<std::pair<const std::vector<cmi::ModuleDecl>*, std::string>> mscopes;
      scopes.push_back({&sig->types, comps[0]});
      mscopes.push_back({&sig->modules, comps[0]});
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
        if (sig) {
          scopes.push_back({&sig->types, scopes.back().second + "." + comps[i]});
          mscopes.push_back({&sig->modules, mscopes.back().second + "." + comps[i]});
        }
      }
      if (!sig) return nullptr;
      std::string pfx;
      for (auto& cmp : comps) { if (!pfx.empty()) pfx += '.'; pfx += cmp; }
      // A ctor ARGUMENT that names a same-module type (`Cons of 'a * 'a t` in Seq)
      // must qualify to that module (`'a Seq.t`), matching ocamlc's per-occurrence
      // path -- but WITHOUT expanding the abbreviation (`t` stays `Seq.t`, not
      // `unit -> 'a node`).  fold_abbrevs_ suppresses the expansion; cmi_types_ctx_
      // /cmi_mod_prefix_ drive the qualification.
      auto* saved_ctx = cmi_types_ctx_;
      std::string saved_pfx = cmi_mod_prefix_;
      bool saved_fold = fold_abbrevs_;
      auto saved_scopes = cmi_scopes_;
      auto saved_mscopes = cmi_mod_scopes_;
      cmi_types_ctx_ = &sig->types;
      cmi_mod_prefix_ = pfx;
      cmi_scopes_ = scopes;
      cmi_mod_scopes_ = mscopes;
      // Folded abbreviations are for DISPLAY (lenient unify swallows the
      // fold-vs-expansion contact); strict unify has no abbrev expansion, so a
      // folded `Seq.t` in the scheme would clash with its own expansion
      // (iterators.ml's `fun () -> Seq.Cons(..)` vs `int Seq.t`).  Expand there.
      fold_abbrevs_ = !strict;
      TypePtr scheme = nullptr;
      for (auto& td : sig->types) {
        if (td.kind != cmi::TypeDecl::Variant) continue;
        for (auto& c : td.ctors) {
          if (c.name != d->name || c.is_inline_record) continue;
          std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
          std::vector<TypePtr> params;
          for (auto& p : td.params) {
            TypePtr v = eng.fresh_var();
            if (p) memo[p.get()] = v;
            params.push_back(v);
          }
          TypePtr result = c.res ? from_cmi(c.res, memo)
                                 : eng.constr(pfx + "." + td.name, params);
          scheme = result;
          for (auto it = c.args.rbegin(); it != c.args.rend(); ++it)
            scheme = eng.arrow(from_cmi(*it, memo), scheme);
          break;
        }
        if (scheme) break;
      }
      // A module-level EXTENSION constructor -- usually `exception Error of ..`
      // (Dynlink.Error, whose bare name would otherwise hit result's Error):
      // args -> the extended type (exn for exceptions).
      if (!scheme)
        for (auto& x : sig->typexts) {
          if (x.name != d->name || x.is_inline_record) continue;
          std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
          TypePtr result;
          if (x.res) result = from_cmi(x.res, memo);
          else {
            std::string tp = x.type_path ? cmi_path_str(*x.type_path) : "exn";
            result = eng.constr(tp == "exn" || tp.find('.') != std::string::npos
                                    ? tp : pfx + "." + tp);
          }
          scheme = result;
          for (auto it = x.args.rbegin(); it != x.args.rend(); ++it)
            scheme = eng.arrow(from_cmi(*it, memo), scheme);
          break;
        }
      cmi_types_ctx_ = saved_ctx;
      cmi_mod_prefix_ = saved_pfx;
      cmi_scopes_ = std::move(saved_scopes);
      cmi_mod_scopes_ = std::move(saved_mscopes);
      fold_abbrevs_ = saved_fold;
      return scheme;
    } catch (...) {
      cmi_types_ctx_ = nullptr; cmi_mod_prefix_.clear();
      cmi_scopes_.clear(); cmi_mod_scopes_.clear();
    }
    return nullptr;
  }

  // `open M` (or a local `M.(..)` open) brings M's variant constructors into bare
  // scope, so `open Arg in [ Unit f; Set r ]` resolves `Unit`/`Set` to `Arg.spec`.
  // We register each into the scoped `cenv` (generalised, so each use instantiates
  // fresh).  Non-strict only -- a wrong pick can't reach the strict reject pass.
  void open_module_ctors(const Longident& modid) {
    auto* pl = std::get_if<Lident>(&modid.v);
    if (!pl) return;  // single-name modules only (matches qualified_ctor_scheme)
    try {
      auto cmi = cmi::CmiFile::load(head_cmi(pl->name));
      // Qualify (not expand) same-module ctor-arg types, as in qualified_ctor_scheme
      // -- so `Seq.(Cons (.., tail))` gives the tail `Seq.t`, matching the explicit
      // `Seq.Cons` path (ocamlc's per-occurrence path).
      auto* saved_ctx = cmi_types_ctx_;
      std::string saved_pfx = cmi_mod_prefix_;
      bool saved_fold = fold_abbrevs_;
      cmi_types_ctx_ = &cmi.types();
      cmi_mod_prefix_ = pl->name;
      fold_abbrevs_ = true;
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Variant) continue;
        for (auto& c : td.ctors) {
          if (c.is_inline_record) continue;
          std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
          std::vector<TypePtr> params;
          for (auto& p : td.params) {
            TypePtr v = eng.fresh_var();
            if (p) memo[p.get()] = v;
            params.push_back(v);
          }
          TypePtr result = c.res ? from_cmi(c.res, memo)
                                 : eng.constr(pl->name + "." + td.name, params);
          TypePtr scheme = result;
          for (auto it = c.args.rbegin(); it != c.args.rend(); ++it)
            scheme = eng.arrow(from_cmi(*it, memo), scheme);
          eng.generalize(scheme);
          cenv.back()[c.name] = scheme;
        }
      }
      // Module-level EXTENSION ctors (`exception Unix_error of error * string
      // * string`): register like variant ctors, so a bare `Unix_error (e,_,_)`
      // after `open Unix` resolves with its real arity (and flattens).
      for (auto& x : cmi.sig().typexts) {
        if (x.is_inline_record) continue;
        std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
        TypePtr result;
        if (x.res) result = from_cmi(x.res, memo);
        else {
          std::string tp = x.type_path ? cmi_path_str(*x.type_path) : "exn";
          result = eng.constr(tp == "exn" || tp.find('.') != std::string::npos
                                  ? tp : pl->name + "." + tp);
        }
        TypePtr scheme = result;
        for (auto it = x.args.rbegin(); it != x.args.rend(); ++it)
          scheme = eng.arrow(from_cmi(*it, memo), scheme);
        eng.generalize(scheme);
        cenv.back()[x.name] = scheme;
      }
      cmi_types_ctx_ = saved_ctx;
      cmi_mod_prefix_ = saved_pfx;
      fold_abbrevs_ = saved_fold;
    } catch (...) {}
  }

  // Split a (instantiated) constructor scheme into its argument types and result.
  static std::vector<TypePtr> ctor_params(const TypePtr& sch, TypePtr& result) {
    std::vector<TypePtr> ps;
    TypePtr c = I::Engine::repr(sch);
    while (c->kind == I::Type::Kind::Arrow) { ps.push_back(c->dom); c = I::Engine::repr(c->cod); }
    result = c;
    return ps;
  }

  // --- let rec value restriction (a sound subset of OCaml's Value_rec_check) ---
  // OCaml forbids dereferencing a recursively-bound name during its own
  // definition.  The full analysis is an intricate 3-mode (Dereference / Guard /
  // Return) traversal; modelling it partially false-rejects valid definitions
  // (e.g. `let rec f = let g = f in fun x -> g x`).  So we flag only the
  // unambiguous, top-level direct dereferences -- the RHS head is itself a rec
  // name, or applies/field-accesses one directly -- which never false-rejects
  // (it only under-catches the subtler illegal cases).
  // OCaml's Env.check_value_name: an operator-shaped value name (one that does
  // NOT start like a valid identifier -- letter/underscore/unicode letter) is an
  // error when it contains a '#' anywhere after the first character (`~##`,
  // `#~#`).  The parser accepts these as operator idents; the typer rejects them
  // at store_value.  Narrow enough to never hit a real value name (no valid
  // OCaml operator contains '#').
  static bool is_illegal_value_name(const std::string& n) {
    if (n.empty()) return false;
    unsigned char c0 = (unsigned char)n[0];
    if (std::isalpha(c0) || c0 == '_' || c0 >= 0x80) return false;  // starts like an ident
    for (size_t i = 1; i < n.size(); ++i)
      if (n[i] == '#') return true;
    return false;
  }
  static void pat_names(const Pattern& p, std::set<std::string>& out) {
    if (auto* v = std::get_if<Ppat_var>(&p.desc)) out.insert(v->name.txt);
    else if (auto* a = std::get_if<Ppat_alias>(&p.desc)) { out.insert(a->name.txt); pat_names(*a->p, out); }
    else if (auto* t = std::get_if<Ppat_tuple>(&p.desc)) { for (auto& e : t->elems) pat_names(*e, out); }
    else if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) pat_names(*c->p, out);
  }
  static const Expression* peel_constraint(const Expression* e) {
    while (auto* c = std::get_if<Pexp_constraint>(&e->desc)) e = c->e.get();
    return e;
  }
  static bool is_rec_ident(const Expression* e, const std::set<std::string>& recs) {
    e = peel_constraint(e);
    auto* id = std::get_if<Pexp_ident>(&e->desc);
    if (!id) return false;
    auto* l = std::get_if<Lident>(&id->id.txt.v);
    return l && recs.count(l->name);
  }
  // A direct, unguarded dereference of a rec name at the RHS head.  We flag the
  // name only in *forcing* positions -- the whole RHS, the function being
  // applied, or a field projection -- never in argument position (an argument
  // may merely be captured by a partial application, e.g. `let rec x = f ~x`,
  // which OCaml accepts), so this never false-rejects.
  static bool letrec_bad_top(const Expression* e0, const std::set<std::string>& recs) {
    const Expression* e = peel_constraint(e0);
    if (is_rec_ident(e, recs)) return true;                          // let rec x = x
    if (auto* ap = std::get_if<Pexp_apply>(&e->desc))               // let rec x = x e...
      return is_rec_ident(ap->fn.get(), recs);
    if (auto* fl = std::get_if<Pexp_field>(&e->desc))               // let rec x = x.field
      return is_rec_ident(fl->e.get(), recs);
    return false;
  }
  void check_letrec(const std::vector<ValueBinding>& bs) {
    if (!strict) return;
    std::set<std::string> recs;
    for (auto& b : bs) pat_names(b.pat, recs);
    if (recs.empty()) return;
    for (auto& b : bs)
      if (letrec_bad_top(b.expr.get(), recs))
        note_error("This kind of expression is not allowed as "
                   "right-hand side of \"let rec\"");
  }

  // Two purely-syntactic restrictions on a match/try case (always errors, so
  // checking them never false-rejects valid code):
  //   - an effect pattern must be at the top level of a case, not nested inside
  //     a constructor/tuple/record (`Some (effect A, _)` is illegal);
  //   - a guarded case may not mix value and exception patterns
  //     (`Some x | exception E x when g -> ...`).
  void check_case_structure(const Case& c) {
    if (!strict) return;
    std::function<void(const Pattern&, bool)> eff = [&](const Pattern& p, bool top) {
      if (auto* e = std::get_if<Ppat_effect>(&p.desc)) {
        if (!top) { note_error("Effect patterns must be at the top level of a match case."); return; }
        eff(*e->eff, false); eff(*e->cont, false);
      } else if (auto* o = std::get_if<Ppat_or>(&p.desc)) { eff(*o->l, top); eff(*o->r, top); }
      else if (auto* cn = std::get_if<Ppat_constraint>(&p.desc)) eff(*cn->p, top);
      else if (auto* al = std::get_if<Ppat_alias>(&p.desc)) eff(*al->p, top);
      else if (auto* tu = std::get_if<Ppat_tuple>(&p.desc)) { for (auto& s : tu->elems) eff(*s, false); }
      else if (auto* k = std::get_if<Ppat_construct>(&p.desc)) { if (k->arg) eff(**k->arg, false); }
      else if (auto* r = std::get_if<Ppat_record>(&p.desc)) { for (auto& [l, s] : r->fields) eff(*s, false); }
      else if (auto* a = std::get_if<Ppat_array>(&p.desc)) { for (auto& s : a->elems) eff(*s, false); }
      else if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) eff(*lz->p, false);
      else if (auto* ex = std::get_if<Ppat_exception>(&p.desc)) eff(*ex->p, false);
    };
    eff(c.lhs, true);
    if (c.guard) {  // a guard over an or-pattern mixing value and exception arms
      bool has_value = false, has_exn = false;
      std::function<void(const Pattern&)> vx = [&](const Pattern& p) {
        if (std::get_if<Ppat_exception>(&p.desc)) has_exn = true;
        else if (std::get_if<Ppat_effect>(&p.desc)) { /* neither */ }
        else if (auto* o = std::get_if<Ppat_or>(&p.desc)) { vx(*o->l); vx(*o->r); }
        else if (auto* cn = std::get_if<Ppat_constraint>(&p.desc)) vx(*cn->p);
        else if (auto* al = std::get_if<Ppat_alias>(&p.desc)) vx(*al->p);
        else has_value = true;
      };
      vx(c.lhs);
      if (has_value && has_exn)
        note_error("Mixing value and exception patterns under when-guards is not supported.");
    }
  }

  TypePtr try_(std::function<TypePtr()> f) {
    try { return f(); } catch (const I::TypeError&) { return eng.fresh_var(); }
  }
  void try_unify(const TypePtr& a, const TypePtr& b) {
    try { eng.unify(a, b); }
    catch (const I::TypeError& e) { if (strict) note_error(e.what()); }
  }

  // Like try_unify but never rejects: on clash the two types simply stay
  // unlinked.  Used where unification is for type *propagation* (driving value
  // kinds) and genuine errors are caught by a separate reliable check -- e.g.
  // function-argument positions, where `expected_clash` does the sound checking.
  // This lets real (e.g. qualified-stdlib) types flow without the inferencer's
  // incompletely-modelled features (formats, GADTs, abbreviations) false-rejecting.
  void soft_unify(const TypePtr& a, const TypePtr& b) {
    try { eng.unify(a, b); } catch (const I::TypeError&) {}
  }

  // A top-level case pattern that matches anything (no guard handled by caller).
  static bool is_catchall(const Pattern& p) {
    if (std::holds_alternative<Ppat_any>(p.desc)) return true;
    if (std::holds_alternative<Ppat_var>(p.desc)) return true;
    if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) return is_catchall(*c->p);
    if (auto* a = std::get_if<Ppat_alias>(&p.desc)) return is_catchall(*a->p);
    if (auto* o = std::get_if<Ppat_or>(&p.desc))
      return is_catchall(*o->l) || is_catchall(*o->r);
    return false;
  }
  static void collect_ctors(const Pattern& p, std::set<std::string>& out) {
    if (auto* k = std::get_if<Ppat_construct>(&p.desc)) out.insert(lid_last(k->id.txt));
    else if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      collect_ctors(*o->l, out); collect_ctors(*o->r, out);
    } else if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) collect_ctors(*c->p, out);
    else if (auto* a = std::get_if<Ppat_alias>(&p.desc)) collect_ctors(*a->p, out);
  }
  // Used after a GADT branch window is rolled back: a branch result that is
  // still ground did NOT depend on the (now-undone) refinement, so it is the
  // genuine match result and may be unified outward.
  static bool type_is_ground(const TypePtr& t0) {
    TypePtr t = I::Engine::repr(t0);
    switch (t->kind) {
      case I::Type::Kind::Var: return false;
      case I::Type::Kind::Arrow:
        return type_is_ground(t->dom) && type_is_ground(t->cod);
      case I::Type::Kind::Tuple:
      case I::Type::Kind::Constr:
        if (t->rigid) return false;  // a locally-abstract type is NOT ground
        for (auto& a : t->args) if (!type_is_ground(a)) return false;
        return true;
      default: return false;  // Object/Variant/Link: play safe, treat as non-ground
    }
  }

  // Structural equality of two GROUND types.  Compares constructor paths by
  // their LAST component (mirroring Engine::unify's own path leniency, so `int`
  // reached via two qualifications is equal), by arity, and recursively.  Used
  // to tell whether all GADT branches agree on a single ground result -- WITHOUT
  // routing through Engine::unify, whose `lenient` mode (on in the signature
  // pass) would silently accept a genuine clash (int vs int list).
  static bool ground_types_equal(const TypePtr& x0, const TypePtr& y0) {
    TypePtr x = I::Engine::repr(x0), y = I::Engine::repr(y0);
    if (x->kind != y->kind) return false;
    switch (x->kind) {
      case I::Type::Kind::Arrow:
        return ground_types_equal(x->dom, y->dom) && ground_types_equal(x->cod, y->cod);
      case I::Type::Kind::Tuple:
        if (x->args.size() != y->args.size()) return false;
        for (size_t i = 0; i < x->args.size(); ++i)
          if (!ground_types_equal(x->args[i], y->args[i])) return false;
        return true;
      case I::Type::Kind::Constr: {
        auto last = [](const std::string& p) {
          auto d = p.rfind('.'); return d == std::string::npos ? p : p.substr(d + 1); };
        if (last(x->path) != last(y->path) || x->args.size() != y->args.size()) return false;
        for (size_t i = 0; i < x->args.size(); ++i)
          if (!ground_types_equal(x->args[i], y->args[i])) return false;
        return true;
      }
      default: return false;
    }
  }

  // Does a pattern (recursively) use a GADT constructor?  Such a match refines
  // types branch-locally, so we must not unify its patterns/results globally.
  bool pat_has_gadt_ctor(const Pattern& p) {
    if (auto* k = std::get_if<Ppat_construct>(&p.desc)) {
      if (gadt_ctors.count(lid_last(k->id.txt))) return true;
      return k->arg && pat_has_gadt_ctor(**k->arg);
    }
    if (auto* tu = std::get_if<Ppat_tuple>(&p.desc)) {
      for (auto& e : tu->elems) if (pat_has_gadt_ctor(*e)) return true;
      return false;
    }
    if (auto* o = std::get_if<Ppat_or>(&p.desc))
      return pat_has_gadt_ctor(*o->l) || pat_has_gadt_ctor(*o->r);
    if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) return pat_has_gadt_ctor(*c->p);
    if (auto* a = std::get_if<Ppat_alias>(&p.desc)) return pat_has_gadt_ctor(*a->p);
    return false;
  }

  // Conservatively decide non-exhaustiveness: only true when CERTAIN — an
  // infinite builtin type or a finite variant missing a top-level constructor,
  // with no unguarded catch-all.  Unknown / fully-covered => Total (never a
  // false-positive Partial, so no regressions even if inference is imperfect).
  bool compute_partial(const TypePtr& scrut, const std::vector<Case>& cases) {
    for (auto& c : cases)
      if (!c.guard && is_catchall(c.lhs)) return false;  // unguarded catch-all
    bool any_unguarded = false;  // every case guarded -> a value can fall through
    for (auto& c : cases) if (!c.guard) any_unguarded = true;
    if (!any_unguarded) return true;
    TypePtr s = I::Engine::repr(scrut);
    if (s->kind == I::Type::Kind::Tuple) return tuple_gadt_partial(s, cases);
    if (s->kind != I::Type::Kind::Constr) return false;  // unknown type
    static const std::set<std::string> inf = {
        "int", "char", "string", "float", "int32", "int64", "nativeint", "bytes"};
    if (inf.count(s->path)) return true;  // infinite type, no catch-all
    auto it = type_ctors.find(s->path);
    if (it == type_ctors.end()) return false;  // unknown variant
    std::set<std::string> covered;
    for (auto& c : cases) if (!c.guard) collect_ctors(c.lhs, covered);
    for (auto& ctor : it->second) if (!covered.count(ctor)) return true;  // missing
    // All top-level ctors covered, but a constructor's polyvariant ARGUMENT may
    // still be under-covered (`A (`A|`C)` leaves `A `D` unmatched).
    if (poly_arg_partial(s, cases)) return true;
    return false;  // covers all top-level ctors => Total (conservative)
  }

  // A pattern built ENTIRELY of polyvariant tags (through or/alias/constraint),
  // with no data-carrying tag: collects the tags and returns true.  Anything
  // else (a nested pattern, a `` `A x `` with a non-catchall arg) returns false
  // so the caller conservatively treats the position as possibly-covering.
  static bool pure_variant_tags(const Pattern& p, std::set<std::string>& out) {
    if (auto* v = std::get_if<Ppat_variant>(&p.desc)) {
      if (v->arg && !is_catchall(**v->arg)) return false;  // `` `A (nested) ``
      out.insert(v->label);
      return true;
    }
    if (auto* o = std::get_if<Ppat_or>(&p.desc))
      return pure_variant_tags(*o->l, out) && pure_variant_tags(*o->r, out);
    if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) return pure_variant_tags(*c->p, out);
    if (auto* a = std::get_if<Ppat_alias>(&p.desc)) return pure_variant_tags(*a->p, out);
    return false;
  }
  // Collect (ctor-name -> argument-pattern) from a case pattern, descending
  // through a top-level or/alias/constraint.  A no-argument ctor maps to null.
  void collect_ctor_args(const Pattern& p,
                         std::multimap<std::string, const Pattern*>& out) {
    if (auto* k = std::get_if<Ppat_construct>(&p.desc))
      out.emplace(lid_last(k->id.txt), k->arg ? &**k->arg : nullptr);
    else if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      collect_ctor_args(*o->l, out);
      collect_ctor_args(*o->r, out);
    } else if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) collect_ctor_args(*c->p, out);
    else if (auto* a = std::get_if<Ppat_alias>(&p.desc)) collect_ctor_args(*a->p, out);
  }
  // The sub-pattern at argument position `i` of a ctor's `arg` pattern: for an
  // arity-1 ctor the whole `arg`; for arity>1 the i-th element of its tuple.
  static const Pattern* arg_position(const Pattern* arg, size_t i, size_t n) {
    if (!arg) return nullptr;
    while (auto* c = std::get_if<Ppat_constraint>(&arg->desc)) arg = c->p.get();
    if (n == 1) return i == 0 ? arg : nullptr;
    if (auto* t = std::get_if<Ppat_tuple>(&arg->desc))
      return i < t->elems.size() ? t->elems[i].get() : nullptr;
    return nullptr;  // a var/`_` covering the whole tuple -> caller sees no tuple
  }
  // Non-exhaustiveness via an under-covered polyvariant constructor ARGUMENT.
  // Only fires when a ctor's argument position has a CLOSED polyvariant type
  // (non-empty row) with a tag that no branch matches and none wildcards --
  // sound: such a value is unmatched by every branch.  Conservative elsewhere
  // (unanalyzable position / open row / non-variant arg => not flagged).
  bool poly_arg_partial(const TypePtr& scrut, const std::vector<Case>& cases) {
    if (strict) return false;  // dump-only; its try_unify must not touch the
                               // reject pass's inference (match_partial is
                               // discarded there anyway)
    TypePtr s = I::Engine::repr(scrut);
    if (s->kind != I::Type::Kind::Constr) return false;
    auto schemes = type_ctor_schemes_.find(s->path);
    if (schemes == type_ctor_schemes_.end()) return false;
    std::multimap<std::string, const Pattern*> args;
    for (auto& c : cases) if (!c.guard) collect_ctor_args(c.lhs, args);
    for (auto& [cname, sch] : schemes->second) {
      auto range = args.equal_range(cname);
      if (range.first == range.second) continue;  // ctor not matched here
      TypePtr result;
      auto ps = ctor_params(eng.instantiate(sch), result);
      if (ps.empty()) continue;  // constant ctor
      try_unify(result, s);      // fresh instantiation vars only -> safe
      for (size_t i = 0; i < ps.size(); ++i) {
        TypePtr pt = I::Engine::repr(ps[i]);
        if (pt->kind != I::Type::Kind::Variant || pt->labels.empty()) continue;
        std::set<std::string> covered;
        bool wildcard = false, analyzable = true;
        for (auto it = range.first; it != range.second; ++it) {
          const Pattern* pos = arg_position(it->second, i, ps.size());
          if (!pos || is_catchall(*pos)) { wildcard = true; break; }
          if (!pure_variant_tags(*pos, covered)) { analyzable = false; break; }
        }
        if (wildcard || !analyzable) continue;
        for (auto& tag : pt->labels)
          if (!covered.count(tag)) return true;  // uncovered row tag -> Partial
      }
    }
    return false;
  }

  // A type path projecting through a functor parameter / bound module (`X.v1`):
  // abstract here, so its equality with another such projection is undecidable
  // (a later `F(M)` could make X.v1 = X.v2).
  bool is_param_projection(const TypePtr& t) {
    if (t->kind != I::Type::Kind::Constr) return false;
    auto d = t->path.find('.');
    if (d == std::string::npos) return false;
    return bound_module_names_.count(t->path.substr(0, d)) != 0;
  }
  // Could two GADT type indices coincide (so an omitted constructor at one index
  // cannot be refuted at the other)?  Biased toward "provably distinct" (the
  // GADT-Total default): returns true only with genuine reason to coincide -- a
  // variable (unifiable), an abstract functor-param projection, or the same head.
  bool index_could_be_equal(const TypePtr& a0, const TypePtr& b0) {
    TypePtr a = I::Engine::repr(a0), b = I::Engine::repr(b0);
    if (a == b) return true;
    if (a->kind == I::Type::Kind::Var || b->kind == I::Type::Kind::Var) return true;
    if (is_param_projection(a) || is_param_projection(b)) return true;
    if (a->kind == I::Type::Kind::Constr && b->kind == I::Type::Kind::Constr)
      return a->path == b->path;
    return false;  // distinct shapes / concrete heads -> provably distinct
  }
  // A GADT `function`'s exhaustiveness, for the dump's Tfunction_cases (Partial)
  // marker.  Partial iff some UNCOVERED constructor is non-refutable: its result
  // index could equal the scrutinee's, so a value of that ctor exists at the
  // scrutinee type (pr7284_bad's `V2 : int -> X.v2 wit` cannot be ruled out from
  // `X.v1 wit` because X is an abstract functor parameter).  Provably-distinct
  // indices (`int` vs `string`, switch_opts) make the ctor refutable -> Total.
  bool gadt_function_partial(const TypePtr& scrut, const std::vector<Case>& cases) {
    TypePtr s = I::Engine::repr(scrut);
    if (s->kind != I::Type::Kind::Constr) return false;  // unknown -> keep Total
    auto tc = type_ctors.find(s->path);
    auto sc = type_ctor_schemes_.find(s->path);
    if (tc == type_ctors.end() || sc == type_ctor_schemes_.end()) return false;
    for (auto& c : cases)
      if (!c.guard && is_catchall(c.lhs)) return false;  // catch-all covers all
    std::set<std::string> covered;
    for (auto& c : cases) if (!c.guard) collect_ctors(c.lhs, covered);
    for (auto& cname : tc->second) {
      if (covered.count(cname)) continue;
      const TypePtr* schp = nullptr;
      for (auto& [n, sch] : sc->second) if (n == cname) { schp = &sch; break; }
      if (!schp) return false;  // unknown scheme -> can't prove non-refutable
      TypePtr result;
      ctor_params(eng.instantiate(*schp), result);
      TypePtr r = I::Engine::repr(result);
      if (r->kind != I::Type::Kind::Constr) return false;
      bool refutable = false;  // some index position provably distinct
      size_t n = std::min(r->args.size(), s->args.size());
      for (size_t i = 0; i < n; ++i)
        if (!index_could_be_equal(s->args[i], r->args[i])) { refutable = true; break; }
      if (!refutable) return true;  // this ctor can't be ruled out -> Partial
    }
    return false;  // every uncovered ctor is refutable -> Total
  }

  // ---- Tuple-scrutinee GADT exhaustiveness (robustmatch) -----------------
  // A Maranget usefulness check of the all-wildcard vector against the
  // (unguarded) pattern matrix, with per-branch GADT index refinement:
  // specializing a column on a GADT constructor records its index equations
  // in a LOCAL substitution (the scrutinee's `(type a)` newtype is a shared
  // Var node, so one binding refines every column), and a constructor whose
  // instantiated index is incompatible with the refined column type is
  // uninhabited there -- refuted, never a candidate.  Compatibility mirrors
  // Ctype.mcomp's over-approximation: abstract/unknown types are compatible
  // with everything; two DISTINCT variant (record) types are incompatible
  // only when their constructor (field) descriptions differ structurally --
  // `ab`=A|B vs `mab`=A|B stay compatible (module coherence!), A|B|C vs
  // X|Y|Z refute.  Rows whose head is impossible under the branch equations
  // (a string constant at a `c` column) are GADT-dead and simply drop.
  // Anything unmodeled throws MxBail -> Total (the pre-existing default), so
  // the analysis only adds Partial verdicts it can prove: a value shape that
  // avoids every row and is well-typed under the accumulated equations.
  struct MxBail {};
  using MxSubst = std::map<const I::Type*, TypePtr>;
  using MxRow = std::vector<const Pattern*>;  // null cell = wildcard
  int mx_fuel_ = 0;

  TypePtr mx_resolve(TypePtr t, const MxSubst& su) {
    t = I::Engine::repr(t);
    for (int i = 0; t->kind == I::Type::Kind::Var && i < 64; ++i) {
      auto it = su.find(t.get());
      if (it == su.end()) break;
      t = I::Engine::repr(it->second);
    }
    return t;
  }

  static std::string mx_base(const std::string& path) {
    auto d = path.rfind('.');
    return d == std::string::npos ? path : path.substr(d + 1);
  }

  struct MxClass {
    enum K { Variant, PredefVariant, Record, External, Open, Abstract, Unknown };
    K k = Unknown;
    const TypeDeclaration* decl = nullptr;  // Variant/Record when locally declared
    std::string key;   // Variant: type_ctor_schemes_ key ("" = flat fallback)
    std::string name;  // External: builtin base; PredefVariant/flat Variant: type name
  };

  MxClass mx_classify(const TypePtr& c) {
    MxClass r;
    std::string b = mx_base(c->path);
    if (c->stamp) {
      auto it = stamp_type_decl_.find(c->stamp);
      if (it == stamp_type_decl_.end()) return r;  // Unknown
      const TypeDeclaration* d = it->second;
      if (std::holds_alternative<Ptype_variant>(d->kind)) {
        r.k = MxClass::Variant; r.decl = d; r.key = stamp_ctor_key_[c->stamp];
      } else if (std::holds_alternative<Ptype_record>(d->kind)) {
        r.k = MxClass::Record; r.decl = d;
      } else if (std::holds_alternative<Ptype_open>(d->kind)) r.k = MxClass::Open;
      else if (std::holds_alternative<Ptype_external>(d->kind)) {
        r.k = MxClass::External; r.name = b;
      } else r.k = MxClass::Abstract;  // opaque abstract
      return r;
    }
    static const std::set<std::string> ext = {
        "int",   "char",  "string",     "float",  "bytes",  "int32",
        "int64", "nativeint", "floatarray", "array", "lazy_t", "format6"};
    static const std::set<std::string> predefv = {"bool", "option", "list",
                                                  "result", "unit"};
    bool bare = c->path.find('.') == std::string::npos;
    if (ext.count(b) && (bare || c->path.rfind("Stdlib.", 0) == 0)) {
      r.k = MxClass::External; r.name = b;
      return r;
    }
    if (bare) {
      if (b == "exn" || b == "eff") { r.k = MxClass::Open; return r; }
      if (predefv.count(b)) { r.k = MxClass::PredefVariant; r.name = b; return r; }
      if (type_ctor_schemes_.count(c->path)) {
        r.k = MxClass::Variant; r.key = c->path;
        return r;
      }
      if (type_ctors.count(c->path)) {  // names known, schemes via the flat map
        r.k = MxClass::Variant; r.name = c->path;
        return r;
      }
      if (auto rd = name_record_decl_.find(b);
          rd != name_record_decl_.end() && !ambiguous_record_names_.count(b)) {
        r.k = MxClass::Record; r.decl = rd->second;
        return r;
      }
      return r;  // Unknown
    }
    if (type_ctor_schemes_.count(c->path)) {  // module-qualified local variant
      r.k = MxClass::Variant; r.key = c->path;
      return r;
    }
    return r;  // dotted cmi/abstract type -> Unknown (compatible with all)
  }

  // Structural constructor description of a variant, for the mcomp variant
  // comparison: name + argument shape + GADT-result marker, in decl order.
  static std::vector<std::string> mx_variant_desc(const TypeDeclaration& d) {
    std::vector<std::string> out;
    for (auto& c : std::get<Ptype_variant>(d.kind).ctors) {
      std::string s = c.name.txt + "/";
      if (auto* t = std::get_if<Pcstr_tuple>(&c.args))
        s += std::to_string(t->elems.size());
      else {
        s += "r";
        for (auto& f : std::get<Pcstr_record>(c.args).fields) s += ":" + f.name.txt;
      }
      if (c.res) s += "/g";
      out.push_back(std::move(s));
    }
    return out;
  }
  static std::vector<std::string> mx_predef_desc(const std::string& name) {
    if (name == "bool") return {"false/0", "true/0"};
    if (name == "unit") return {"()/0"};
    if (name == "option") return {"None/0", "Some/1"};
    if (name == "list") return {"[]/0", "::/2"};
    if (name == "result") return {"Ok/1", "Error/1"};
    return {};
  }
  static std::vector<std::string> mx_desc_of(const MxClass& c) {
    if (c.k == MxClass::PredefVariant) return mx_predef_desc(c.name);
    if (c.k == MxClass::Variant && c.decl) return mx_variant_desc(*c.decl);
    return {};  // unknown description -> over-approximate compatible
  }
  static std::vector<std::string> mx_record_desc(const TypeDeclaration& d) {
    std::vector<std::string> out;
    for (auto& f : std::get<Ptype_record>(d.kind).fields)
      out.push_back(f.name.txt + (f.mut == MutableFlag::Mutable ? "/m" : ""));
    return out;
  }

  // Could `a` and `b` be equal under some implementation, given (and
  // extending) the equations in `su`?  False only when provably distinct.
  bool mx_compat(TypePtr a, TypePtr b, MxSubst& su) {
    if (--mx_fuel_ <= 0) throw MxBail{};
    a = mx_resolve(a, su);
    b = mx_resolve(b, su);
    if (a.get() == b.get()) return true;
    using K = I::Type::Kind;
    if (a->kind == K::Var) { su[a.get()] = b; return true; }
    if (b->kind == K::Var) { su[b.get()] = a; return true; }
    if (a->kind == K::Tuple && b->kind == K::Tuple) {
      if (a->args.size() != b->args.size()) return false;
      for (size_t i = 0; i < a->args.size(); ++i)
        if (!mx_compat(a->args[i], b->args[i], su)) return false;
      return true;
    }
    if (a->kind == K::Arrow && b->kind == K::Arrow)
      return mx_compat(a->dom, b->dom, su) && mx_compat(a->cod, b->cod, su);
    if (a->kind == K::Variant && b->kind == K::Variant) return true;  // rows: over-approx
    if (a->kind == K::Object && b->kind == K::Object) return true;
    if (a->kind == K::Constr && b->kind == K::Constr) {
      MxClass ca = mx_classify(a), cb = mx_classify(b);
      bool same = (a->stamp && a->stamp == b->stamp) ||
                  (!a->stamp && !b->stamp && a->path == b->path);
      auto args_compat = [&]() {
        if (a->args.size() != b->args.size()) return false;
        for (size_t i = 0; i < a->args.size(); ++i)
          if (!mx_compat(a->args[i], b->args[i], su)) return false;
        return true;
      };
      if (same) {
        // datatype params are injective (compare); abstract ones are not.
        if (ca.k == MxClass::Abstract || ca.k == MxClass::Unknown) return true;
        return args_compat();
      }
      if (ca.k == MxClass::Abstract || ca.k == MxClass::Unknown ||
          cb.k == MxClass::Abstract || cb.k == MxClass::Unknown)
        return true;
      if (ca.k == MxClass::Open && cb.k == MxClass::Open) return true;
      if (ca.k == MxClass::External && cb.k == MxClass::External)
        return ca.name == cb.name && args_compat();
      bool va = ca.k == MxClass::Variant || ca.k == MxClass::PredefVariant;
      bool vb = cb.k == MxClass::Variant || cb.k == MxClass::PredefVariant;
      if (va && vb) {
        auto da = mx_desc_of(ca), db = mx_desc_of(cb);
        if (da.empty() || db.empty()) return true;  // unknown desc -> compatible
        if (da != db) return false;
        return args_compat();
      }
      if (ca.k == MxClass::Record && cb.k == MxClass::Record) {
        if (!ca.decl || !cb.decl) return true;
        if (mx_record_desc(*ca.decl) != mx_record_desc(*cb.decl)) return false;
        return args_compat();
      }
      return false;  // distinct concrete kinds (variant vs record vs external)
    }
    if (a->kind == K::Constr || b->kind == K::Constr) {
      // a datatype can't alias a structural shape; an abstract type could.
      MxClass cc = mx_classify(a->kind == K::Constr ? a : b);
      return cc.k == MxClass::Abstract || cc.k == MxClass::Unknown;
    }
    return false;
  }

  // The (name, scheme) list of an enumerable variant column.  Per-type map
  // when the type's key is known; otherwise names from type_ctors + schemes
  // from the flat ctor map, sanity-checked to build the column's type.
  std::vector<std::pair<std::string, TypePtr>> mx_ctor_schemes(const MxClass& c,
                                                               const TypePtr& col) {
    if (c.k == MxClass::Variant && !c.key.empty()) {
      auto it = type_ctor_schemes_.find(c.key);
      if (it == type_ctor_schemes_.end()) throw MxBail{};
      return it->second;
    }
    auto names = type_ctors.find(c.name);
    if (names == type_ctors.end()) throw MxBail{};
    std::string colb = mx_base(col->path);
    std::vector<std::pair<std::string, TypePtr>> out;
    for (auto& n : names->second) {
      auto ci = ctors.find(n);
      if (ci == ctors.end()) throw MxBail{};
      TypePtr result;
      ctor_params(ci->second, result);
      TypePtr r = I::Engine::repr(result);
      if (r->kind != I::Type::Kind::Constr || mx_base(r->path) != colb)
        throw MxBail{};  // flat entry shadowed by another type's ctor
      out.emplace_back(n, ci->second);
    }
    return out;
  }

  // Ctor schemes are built at registration time, BEFORE tenv exists, so their
  // internal constr nodes carry bare stamp-0 paths (`c1`, not M3.c1/stamp).
  // Resolve such names lexically under the scheme's OWNING module prefix
  // (innermost first, then outer, then a globally-unique bare declaration) so
  // classification and enumeration find the right declaration.
  int mx_resolve_bare_stamp(const std::string& name, const std::string& prefix) {
    std::string p = prefix;
    for (;;) {
      auto it = qual_type_stamp_.find(p + name);
      if (it != qual_type_stamp_.end()) return it->second;
      if (p.empty()) break;
      auto d = p.rfind('.', p.size() - 2);  // strip the innermost "X."
      p = d == std::string::npos ? "" : p.substr(0, d + 1);
    }
    auto b = bare_unique_stamp_.find(name);
    return b != bare_unique_stamp_.end() && b->second > 0 ? b->second : 0;
  }
  // Deep-copy a (freshly instantiated, analysis-local) scheme, stamping bare
  // constr nodes resolved under `prefix`.  Vars/Any/rows are shared as-is.
  TypePtr mx_qualify(const TypePtr& t0, const std::string& prefix,
                     std::unordered_map<const I::Type*, TypePtr>& memo) {
    if (--mx_fuel_ <= 0) throw MxBail{};
    TypePtr t = I::Engine::repr(t0);
    if (auto m = memo.find(t.get()); m != memo.end()) return m->second;
    using K = I::Type::Kind;
    if (t->kind == K::Constr) {
      int stamp = t->stamp;
      if (!stamp && t->path.find('.') == std::string::npos)
        stamp = mx_resolve_bare_stamp(t->path, prefix);
      TypePtr n = eng.constr(t->path, {}, stamp);
      memo[t.get()] = n;  // before recursing: recursive schemes terminate
      for (auto& a : t->args) n->args.push_back(mx_qualify(a, prefix, memo));
      return n;
    }
    if (t->kind == K::Tuple) {
      TypePtr n = eng.tuple({});
      memo[t.get()] = n;
      for (auto& a : t->args) n->args.push_back(mx_qualify(a, prefix, memo));
      return n;
    }
    if (t->kind == K::Arrow) {
      TypePtr n = eng.arrow(nullptr, nullptr, t->arrow_label, t->arrow_lbl);
      memo[t.get()] = n;
      n->dom = mx_qualify(t->dom, prefix, memo);
      n->cod = mx_qualify(t->cod, prefix, memo);
      return n;
    }
    return t;  // Var/Any/Variant/Object: shared (row tag args left as-is)
  }

  // Peel wrappers that don't affect matching (constraint/alias/local open).
  static const Pattern* mx_peel(const Pattern* p) {
    while (p) {
      if (auto* c = std::get_if<Ppat_constraint>(&p->desc)) p = c->p.get();
      else if (auto* a = std::get_if<Ppat_alias>(&p->desc)) p = a->p.get();
      else if (auto* o = std::get_if<Ppat_open>(&p->desc)) p = o->p.get();
      else break;
    }
    return p;
  }
  static bool mx_wild(const Pattern* p) {
    return !p || std::holds_alternative<Ppat_any>(p->desc) ||
           std::holds_alternative<Ppat_var>(p->desc);
  }

  // Is the all-wildcard vector useful against `rows` at column types `cols`
  // under the equations in `su`?  True = some well-typed value avoids every
  // row = the match is Partial.
  bool mx_useful(std::vector<MxRow> rows, std::vector<TypePtr> cols, MxSubst su) {
    if (--mx_fuel_ <= 0) throw MxBail{};
    if (rows.empty()) return true;
    if (cols.empty()) return false;
    // Normalize column 0: peel wrappers, split or-patterns into extra rows.
    for (size_t i = 0; i < rows.size();) {
      const Pattern* p = mx_peel(rows[i][0]);
      rows[i][0] = p;
      if (p)
        if (auto* o = std::get_if<Ppat_or>(&p->desc)) {
          MxRow r2 = rows[i];
          rows[i][0] = o->l.get();
          r2[0] = o->r.get();
          rows.insert(rows.begin() + i + 1, std::move(r2));
          continue;  // reprocess the left branch (may itself wrap/or)
        }
      ++i;
    }
    auto tail_of = [](const MxRow& r) { return MxRow(r.begin() + 1, r.end()); };
    bool allw = true;
    for (auto& r : rows) if (!mx_wild(r[0])) { allw = false; break; }
    if (allw) {  // type-agnostic: candidate picks any inhabitant
      std::vector<MxRow> m2;
      for (auto& r : rows) m2.push_back(tail_of(r));
      return mx_useful(std::move(m2), {cols.begin() + 1, cols.end()}, std::move(su));
    }
    TypePtr t = mx_resolve(cols[0], su);
    std::vector<TypePtr> rest(cols.begin() + 1, cols.end());
    using K = I::Type::Kind;
    // D(M): rows with a wildcard in column 0, the column dropped.  Sound
    // whenever a candidate outside every listed head shape exists.
    auto default_matrix = [&]() {
      std::vector<MxRow> d;
      for (auto& r : rows) if (mx_wild(r[0])) d.push_back(tail_of(r));
      return d;
    };
    if (t->kind == K::Tuple) {
      size_t n = t->args.size();
      std::vector<MxRow> m2;
      for (auto& r : rows) {
        MxRow r2;
        if (mx_wild(r[0])) r2.assign(n, nullptr);
        else if (auto* tp = std::get_if<Ppat_tuple>(&r[0]->desc)) {
          if (tp->elems.size() != n || tp->closed != ClosedFlag::Closed) throw MxBail{};
          for (auto& l : tp->labels) if (l) throw MxBail{};
          for (auto& e : tp->elems) r2.push_back(e.get());
        } else continue;  // non-tuple head at tuple type: GADT-dead row
        r2.insert(r2.end(), r.begin() + 1, r.end());
        m2.push_back(std::move(r2));
      }
      std::vector<TypePtr> cols2 = t->args;
      cols2.insert(cols2.end(), rest.begin(), rest.end());
      return mx_useful(std::move(m2), std::move(cols2), std::move(su));
    }
    if (t->kind == K::Variant) {  // exact closed polyvariant row [ `A | `B ]
      if (t->labels.empty() || !t->inherited.empty() || t->variant_kind != 2)
        throw MxBail{};
      for (size_t ti = 0; ti < t->labels.size(); ++ti) {
        bool has_arg = t->tag_has_arg[ti];
        MxSubst su2 = su;
        std::vector<MxRow> s;
        bool in_sigma = false;
        for (auto& r : rows) {
          MxRow r2;
          if (mx_wild(r[0])) {
            if (has_arg) r2.push_back(nullptr);
          } else if (auto* v = std::get_if<Ppat_variant>(&r[0]->desc)) {
            if (v->label != t->labels[ti]) continue;
            in_sigma = true;
            if ((bool)v->arg != has_arg) throw MxBail{};
            if (has_arg) r2.push_back(v->arg->get());
          } else if (std::holds_alternative<Ppat_type>(r[0]->desc)) {
            throw MxBail{};  // #t covers a whole type's tags: unmodeled
          } else continue;  // dead row
          r2.insert(r2.end(), r.begin() + 1, r.end());
          s.push_back(std::move(r2));
        }
        std::vector<TypePtr> cols2;
        if (has_arg) cols2.push_back(t->args[ti]);
        cols2.insert(cols2.end(), rest.begin(), rest.end());
        if (in_sigma) {
          if (mx_useful(std::move(s), std::move(cols2), std::move(su2))) return true;
        } else if (mx_useful(default_matrix(), rest, std::move(su2))) return true;
      }
      return false;
    }
    if (t->kind != K::Constr) throw MxBail{};  // Var/Arrow/Object column w/ patterns
    MxClass tc = mx_classify(t);
    if (tc.k == MxClass::External && tc.name == "lazy_t" && t->args.size() == 1) {
      std::vector<MxRow> m2;  // lazy: a single transparent constructor
      for (auto& r : rows) {
        MxRow r2;
        if (mx_wild(r[0])) r2.push_back(nullptr);
        else if (auto* lz = std::get_if<Ppat_lazy>(&r[0]->desc)) r2.push_back(lz->p.get());
        else continue;  // dead row
        r2.insert(r2.end(), r.begin() + 1, r.end());
        m2.push_back(std::move(r2));
      }
      std::vector<TypePtr> cols2 = {t->args[0]};
      cols2.insert(cols2.end(), rest.begin(), rest.end());
      return mx_useful(std::move(m2), std::move(cols2), std::move(su));
    }
    if (tc.k == MxClass::External || tc.k == MxClass::Open) {
      // Never-complete columns (infinite constants, array lengths, open
      // types): a candidate outside every listed head always exists.
      // Intervals cover ranges we don't model -- a covering interval set
      // would make this a false Partial, so bail instead.
      for (auto& r : rows)
        if (r[0] && std::holds_alternative<Ppat_interval>(r[0]->desc)) throw MxBail{};
      return mx_useful(default_matrix(), std::move(rest), std::move(su));
    }
    if (tc.k == MxClass::Record) {
      if (!tc.decl) throw MxBail{};
      auto& fields = std::get<Ptype_record>(tc.decl->kind).fields;
      if (tc.decl->params.size() != t->args.size()) throw MxBail{};
      std::unordered_map<std::string, TypePtr> vars;
      for (size_t i = 0; i < t->args.size(); ++i)
        if (auto* pv = std::get_if<Ptyp_var>(&tc.decl->params[i]->desc))
          vars[pv->name] = t->args[i];
      std::vector<TypePtr> cols2;
      for (auto& f : fields) cols2.push_back(from_coretype(*f.type, vars));
      cols2.insert(cols2.end(), rest.begin(), rest.end());
      std::vector<MxRow> m2;
      for (auto& r : rows) {
        MxRow r2;
        if (mx_wild(r[0])) r2.assign(fields.size(), nullptr);
        else if (auto* rp = std::get_if<Ppat_record>(&r[0]->desc)) {
          r2.assign(fields.size(), nullptr);
          for (auto& [lid, pb] : rp->fields) {
            std::string fn = lid_last(lid.txt);
            size_t k = 0;
            for (; k < fields.size(); ++k) if (fields[k].name.txt == fn) break;
            if (k == fields.size()) throw MxBail{};  // foreign label
            r2[k] = pb.get();
          }
        } else continue;  // dead row
        r2.insert(r2.end(), r.begin() + 1, r.end());
        m2.push_back(std::move(r2));
      }
      return mx_useful(std::move(m2), std::move(cols2), std::move(su));
    }
    if (tc.k == MxClass::Variant || tc.k == MxClass::PredefVariant) {
      auto list = mx_ctor_schemes(tc, t);
      // the schemes' bare internal names resolve under their owning module
      std::string prefix;
      if (!tc.key.empty())
        if (auto d = tc.key.rfind('.'); d != std::string::npos)
          prefix = tc.key.substr(0, d + 1);
      for (auto& [cname, scheme] : list) {
        MxSubst su2 = su;
        TypePtr result;
        std::unordered_map<const I::Type*, TypePtr> memo;
        auto params =
            ctor_params(mx_qualify(eng.instantiate(scheme), prefix, memo), result);
        TypePtr r = I::Engine::repr(result);
        if (r->kind != K::Constr || r->args.size() != t->args.size()) throw MxBail{};
        bool inhabited = true;  // index equations flow into su2 here
        for (size_t i = 0; inhabited && i < r->args.size(); ++i)
          inhabited = mx_compat(r->args[i], t->args[i], su2);
        if (!inhabited) continue;  // refuted at this (refined) index
        std::vector<MxRow> s;
        bool in_sigma = false;
        for (auto& row : rows) {
          MxRow r2;
          if (mx_wild(row[0])) r2.assign(params.size(), nullptr);
          else if (auto* k = std::get_if<Ppat_construct>(&row[0]->desc)) {
            if (lid_last(k->id.txt) != cname) continue;  // other head / dead
            if (!k->vars.empty()) throw MxBail{};  // `C (type a) p`: unmodeled
            if (!k->arg) {
              if (!params.empty()) continue;  // arity mismatch: foreign dead ctor
            } else {
              const Pattern* ap = mx_peel(k->arg->get());
              if (params.empty()) continue;  // arity mismatch: foreign dead ctor
              if (params.size() == 1) r2.push_back(ap);
              else if (mx_wild(ap)) r2.assign(params.size(), nullptr);
              else if (auto* tp = std::get_if<Ppat_tuple>(&ap->desc)) {
                if (tp->elems.size() != params.size()) continue;  // foreign arity
                for (auto& l : tp->labels) if (l) throw MxBail{};
                for (auto& e : tp->elems) r2.push_back(e.get());
              } else if (std::holds_alternative<Ppat_record>(ap->desc)) {
                throw MxBail{};  // inline-record argument: unmodeled
              } else if (std::holds_alternative<Ppat_or>(ap->desc)) {
                throw MxBail{};  // or at a multi-slot argument: unmodeled
              } else continue;  // foreign shape
            }
            in_sigma = true;
          } else continue;  // constant/tuple/... at variant type: dead row
          r2.insert(r2.end(), row.begin() + 1, row.end());
          s.push_back(std::move(r2));
        }
        std::vector<TypePtr> cols2 = params;
        cols2.insert(cols2.end(), rest.begin(), rest.end());
        if (in_sigma) {
          if (mx_useful(std::move(s), std::move(cols2), std::move(su2))) return true;
        } else if (mx_useful(default_matrix(), rest, std::move(su2))) return true;
      }
      return false;
    }
    throw MxBail{};  // abstract/unknown column with real patterns
  }

  bool tuple_gadt_partial(const TypePtr& s, const std::vector<Case>& cases) {
    if (strict) return false;  // the reject pass discards partiality anyway
    bool gadt = false;  // gate: only GADT-involving tuple matches
    for (auto& el0 : s->args) {
      TypePtr el = I::Engine::repr(el0);
      if (el->kind == I::Type::Kind::Constr && gadt_types.count(mx_base(el->path)))
        gadt = true;
    }
    for (auto& c : cases) if (pat_has_gadt_ctor(c.lhs)) gadt = true;
    if (!gadt) return false;
    try {
      size_t n = s->args.size();
      std::vector<MxRow> rows;
      // flatten a top-level or into separate rows; drop exception/effect rows
      std::function<void(const Pattern*)> add = [&](const Pattern* p) {
        p = mx_peel(p);
        if (!p) return;
        if (auto* o = std::get_if<Ppat_or>(&p->desc)) {
          add(o->l.get());
          add(o->r.get());
          return;
        }
        if (std::holds_alternative<Ppat_exception>(p->desc) ||
            std::holds_alternative<Ppat_effect>(p->desc))
          return;  // no value coverage
        MxRow r;
        if (mx_wild(p)) r.assign(n, nullptr);
        else if (auto* tp = std::get_if<Ppat_tuple>(&p->desc)) {
          if (tp->elems.size() != n || tp->closed != ClosedFlag::Closed) throw MxBail{};
          for (auto& l : tp->labels) if (l) throw MxBail{};
          for (auto& e : tp->elems) r.push_back(e.get());
        } else throw MxBail{};
        rows.push_back(std::move(r));
      };
      for (auto& c : cases)
        if (!c.guard) add(&c.lhs);
      mx_fuel_ = 20000;
      return mx_useful(std::move(rows), s->args, MxSubst{});
    } catch (const MxBail&) {
      return false;  // unanalyzable -> Total (the pre-existing default)
    } catch (const I::TypeError&) {
      return false;
    }
  }

  // Collect the polyvariant tags a pattern matches (through alias/or/constraint).
  void collect_variant_tags(const Pattern& p, std::set<std::string>& out) {
    if (auto* v = std::get_if<Ppat_variant>(&p.desc)) out.insert(v->label);
    else if (auto* a = std::get_if<Ppat_alias>(&p.desc)) collect_variant_tags(*a->p, out);
    else if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) collect_variant_tags(*c->p, out);
    else if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      collect_variant_tags(*o->l, out);
      collect_variant_tags(*o->r, out);
    }
  }
  // Exhaustiveness of a SINGLE (parameter/let) pattern against its type: total
  // (false) unless a top-level constructor/tag is missing.  A polyvariant over a
  // closed row is total iff the pattern covers every row label; a non-variant,
  // non-Constr type (tuple/record/abstract/unknown) is total.
  // A `#t` pattern (through alias/constraint) matches every tag of type t, so it
  // is exhaustive for that polyvariant.
  static bool pat_is_hash_type(const Pattern& p) {
    if (std::holds_alternative<Ppat_type>(p.desc)) return true;
    if (auto* a = std::get_if<Ppat_alias>(&p.desc)) return pat_is_hash_type(*a->p);
    if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) return pat_is_hash_type(*c->p);
    return false;
  }
  bool param_pattern_partial(const TypePtr& scrut, const Pattern& pat) {
    if (is_catchall(pat)) return false;
    if (pat_is_hash_type(pat)) return false;  // `#t` covers its type
    TypePtr s = I::Engine::repr(scrut);
    if (s->kind == I::Type::Kind::Variant) {
      if (s->variant_kind == 0 && s->labels.empty()) return false;  // open, unknown
      std::set<std::string> covered;
      collect_variant_tags(pat, covered);
      for (auto& l : s->labels)
        if (!covered.count(l)) return true;  // a present tag is unmatched
      return false;
    }
    if (s->kind != I::Type::Kind::Constr) return false;  // tuple/record/abstract
    static const std::set<std::string> inf = {
        "int", "char", "string", "float", "int32", "int64", "nativeint", "bytes"};
    if (inf.count(s->path)) return true;  // infinite type, single pattern
    auto it = type_ctors.find(s->path);
    // Unknown / extensible type (exn, `type t = ..`): can't prove total, so keep
    // the syntactic verdict (this is consulted downgrade-only) -- a single
    // extension/exception ctor `M.E` param IS partial.
    if (it == type_ctors.end()) return true;
    std::set<std::string> covered;
    collect_ctors(pat, covered);
    for (auto& ctor : it->second) if (!covered.count(ctor)) return true;
    return false;
  }

  TypePtr constant_type(const Constant& c) {
    if (auto* i = std::get_if<Pconst_integer>(&c.desc)) {
      if (i->suffix == 'l') return eng.constr("int32");
      if (i->suffix == 'L') return eng.constr("int64");
      if (i->suffix == 'n') return eng.constr("nativeint");
      // A literal modifier other than l/L/n needs a ppx to interpret it; with
      // none run, ocamlc rejects it ("Unknown modifier g for literal ..").  The
      // oracle also runs ppx-free, so a valid file never carries one -- sound.
      if (strict && i->suffix)
        note_error("Unknown modifier " + std::string(1, *i->suffix) +
                   " for literal " + i->value);
      return eng.constr("int");  // unsuffixed (or user-defined suffix => int)
    }
    if (std::holds_alternative<Pconst_char>(c.desc)) return eng.constr("char");
    if (std::holds_alternative<Pconst_string>(c.desc)) return eng.constr("string");
    // Float literals have no built-in suffix; any modifier needs a ppx.
    if (auto* fl = std::get_if<Pconst_float>(&c.desc); fl && strict && fl->suffix)
      note_error("Unknown modifier " + std::string(1, *fl->suffix) +
                 " for literal " + fl->value);
    return eng.constr("float");
  }

  // Bind all variables of a pattern to Any (used for patterns in an unknown
  // context, e.g. record fields we don't type).
  void bind_pat_any(const Pattern& p) {
    if (auto* v = std::get_if<Ppat_var>(&p.desc)) { venv.back()[v->name.txt] = eng.fresh_var(); return; }  // P4-D: fresh, not Any (corpus-validated)
    if (auto* al = std::get_if<Ppat_alias>(&p.desc)) {
      venv.back()[al->name.txt] = eng.fresh_var(); bind_pat_any(*al->p); return;
    }
    if (auto* tu = std::get_if<Ppat_tuple>(&p.desc)) {
      for (auto& e : tu->elems) bind_pat_any(*e); return;
    }
    if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) { bind_pat_any(*c->p); return; }
    if (auto* o = std::get_if<Ppat_or>(&p.desc)) { bind_pat_any(*o->l); bind_pat_any(*o->r); return; }
    if (auto* k = std::get_if<Ppat_construct>(&p.desc)) { if (k->arg) bind_pat_any(**k->arg); return; }
    if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
      for (auto& [lid, sub] : r->fields) bind_pat_any(*sub); return;
    }
    if (auto* a = std::get_if<Ppat_array>(&p.desc)) { for (auto& e : a->elems) bind_pat_any(*e); return; }
    if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) { bind_pat_any(*lz->p); return; }
    // any/constant/etc: nothing to bind
  }

  // Bind a polymorphic record field's sub-pattern to the field's generic scheme
  // `fldTy` (venv holds schemes; lookup instantiates -> per-use polymorphism).
  // Only a plain binder (`{pf}` / `{pf = q}` / `pf as x`) is supported; anything
  // structural (the universal type can't be a tuple/constructor) returns false so
  // the caller falls back to Any.
  bool bind_poly_field(const Pattern& p, const TypePtr& fldTy) {
    if (auto* v = std::get_if<Ppat_var>(&p.desc)) { venv.back()[v->name.txt] = fldTy; return true; }
    if (std::holds_alternative<Ppat_any>(p.desc)) return true;
    if (auto* al = std::get_if<Ppat_alias>(&p.desc)) {
      venv.back()[al->name.txt] = fldTy; return bind_poly_field(*al->p, fldTy);
    }
    if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) return bind_poly_field(*c->p, fldTy);
    return false;
  }

  TypePtr infer_pat(const Pattern& p) {
    TypePtr t = infer_pat_impl(p);
    if (record_kinds_) rec_pat_[&p] = t;  // record every pattern's kind (params incl.)
    if (as_map_) (*as_map_)[&p] = t;      // side record for build_as_type under an alias
    return t;
  }

  // A `#t` pattern's row: t must be an abbreviation of a poly-variant row
  // (`type rte = [ `A of .. | `B ]`); the pattern means "any of t's tags", i.e.
  // the UPPER bound `[< tags-with-declared-args ]`.  Builds a fresh row instance
  // per call (each gets vk=1); returns null when t isn't a known variant
  // abbreviation.  Non-strict only (rows are a non-strict feature).
  TypePtr hash_type_row(const ast::Longident& id) {
    if (strict) return nullptr;
    auto ai = type_aliases.find(lid_last(id));
    if (ai == type_aliases.end() || !ai->second.manifest) return nullptr;
    if (!std::holds_alternative<Ptyp_variant>(ai->second.manifest->desc)) return nullptr;
    std::unordered_map<std::string, TypePtr> vars;
    bool saved_me = manifest_expansion_;
    manifest_expansion_ = true;
    TypePtr row = from_coretype(*ai->second.manifest, vars);
    manifest_expansion_ = saved_me;
    row = I::Engine::repr(row);
    if (row->kind != I::Type::Kind::Variant) return nullptr;
    row->variant_kind = 1;  // `#t` bounds ABOVE: `[<`, not the exact `[ .. ]`
    if (ai->second.params.empty()) row->abbrev = lid_last(id);  // `[< var ]` display
    return row;
  }

  // typecore.ml build_as_type: the variable bound by `pat as x` does not get the
  // scrutinee's type but one REBUILT from the pattern -- a constructor pattern
  // yields a fresh instance of the constructor's result type (argument slots
  // unified from the sub-patterns), so constructors that don't constrain a type
  // parameter leave it free.  `B _ | C _ as x -> x` can then return x at a
  // different parameter instantiation than the scrutinee's.  Patterns we don't
  // rebuild (records, constants, ...) keep their inferred type from `tys`.
  std::unordered_map<const Pattern*, TypePtr>* as_map_ = nullptr;
  TypePtr build_as_type(const Pattern& p,
                        const std::unordered_map<const Pattern*, TypePtr>& tys) {
    auto fallback = [&]() -> TypePtr {
      auto it = tys.find(&p);
      return it != tys.end() ? it->second : eng.fresh_var();
    };
    if (auto* al = std::get_if<Ppat_alias>(&p.desc)) return build_as_type(*al->p, tys);
    if (auto* tu = std::get_if<Ppat_tuple>(&p.desc)) {
      std::vector<TypePtr> es;
      for (auto& e : tu->elems) es.push_back(build_as_type(*e, tys));
      return eng.tuple(std::move(es));
    }
    if (auto* k = std::get_if<Ppat_construct>(&p.desc)) {
      TypePtr* sch = find_ctor(lid_last(k->id.txt));
      if (!sch) return fallback();
      TypePtr result;
      auto ps = ctor_params(eng.instantiate(*sch), result);
      if (k->arg) {
        auto* tup = std::get_if<Ppat_tuple>(&(*k->arg)->desc);
        if (ps.size() > 1 && tup && tup->elems.size() == ps.size()) {
          for (size_t i = 0; i < ps.size(); ++i)
            soft_unify(ps[i], build_as_type(*tup->elems[i], tys));
        } else if (!ps.empty()) {
          soft_unify(ps[0], build_as_type(**k->arg, tys));
        }
      }
      return result;
    }
    if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      TypePtr lt = build_as_type(*o->l, tys), rt = build_as_type(*o->r, tys);
      soft_unify(lt, rt);
      return lt;
    }
    // A poly-variant pattern rebuilds as a FRESH OPEN row `[> tag [of t]]`
    // (typecore build_as_type Tpat_variant): the alias-bound var is then a
    // DIFFERENT row from the matched `[<` scrutinee, so `| `Nil | `Cons _ as x
    // -> `A x` gives `[< `Cons|`Nil|`Snoc..] -> [> `A of [> `Cons|`Nil ] ..]`
    // (input and output rows independent), not one shared `as 'c` row.  The tag
    // ARG is shared with the scrutinee row's (fallback -> the inferred node).
    if (auto* pv = std::get_if<Ppat_variant>(&p.desc)) {
      if (strict) return fallback();
      TypePtr at = pv->arg ? build_as_type(**pv->arg, tys) : eng.fresh_var();
      return eng.variant_type({pv->label}, {at}, {(char)(pv->arg ? 1 : 0)}, 0);
    }
    // `#t as x`: x gets a fresh OPEN row (typecore's as-types are open) whose
    // FIELDS are the matched row's own nodes -- typecore's `{row with more =
    // newvar}`: fresh row variable, shared tag args.  Sharing is what ties a
    // downstream use of x back to the scrutinee's element types (mixin's
    // `#expr as e -> map_expr .. e` equates the scrutinee's param with
    // map_expr's row element).  Falls back to a fresh abbreviation instance
    // when the matched type isn't a row.
    if (auto* pt = std::get_if<Ppat_type>(&p.desc)) {
      if (!strict) {
        auto it = tys.find(&p);
        TypePtr inf = it != tys.end() ? I::Engine::repr(it->second) : nullptr;
        if (inf && inf->kind == I::Type::Kind::Variant) {
          TypePtr row =
              eng.variant_type(inf->labels, inf->args, inf->tag_has_arg, 0);
          row->abbrev = inf->abbrev;
          row->abbrev_args = inf->abbrev_args;
          return row;
        }
      }
      if (TypePtr row = hash_type_row(pt->id.txt)) {
        row->variant_kind = 0;
        return row;
      }
      return fallback();
    }
    return fallback();
  }
  // -- pressure-lite (ocamlc's Parmatch.pressure_variants approximation) --
  // A variant/`#t` pattern row is an upper bound `[<` ONLY when the match
  // needs closing for exhaustiveness.  A position whose column has a
  // catch-all (a var/`_` at the position or at an ancestor; a `#t` at a
  // strict ancestor -- its sub-positions are unconstrained) keeps the row
  // OPEN: its tags are lower-bound presence, like construction
  // (`function `A -> 1 | x -> 2` : `[> `A ] -> int`).  Guarded cases never
  // make a match exhaustive, so they contribute no wildcards.  Positions are
  // encoded as step-paths ("/`Abs/0/"); marked nodes are consumed by
  // infer_pat's Ppat_variant/Ppat_type row builders.
  std::unordered_set<const Pattern*> open_row_pats_;
  void mark_open_row_pats(const std::vector<Case>& cases) {
    if (strict) return;
    std::vector<std::string> wild_sub, wild_kids;
    auto covered = [&](const std::string& path) {
      for (auto& w : wild_sub)  // var/`_` at the position or an ancestor
        if (path.compare(0, w.size(), w) == 0) return true;
      for (auto& w : wild_kids)  // `#t` at a strict ancestor
        if (path.size() > w.size() && path.compare(0, w.size(), w) == 0)
          return true;
      return false;
    };
    // Is a pattern irrefutable (matches every value of its type)?
    // Conservative: constructors/variants/constants/#t count refutable.
    std::function<bool(const Pattern&)> irref = [&](const Pattern& p) -> bool {
      if (std::holds_alternative<Ppat_var>(p.desc) ||
          std::holds_alternative<Ppat_any>(p.desc))
        return true;
      if (auto* al = std::get_if<Ppat_alias>(&p.desc)) return irref(*al->p);
      if (auto* ct = std::get_if<Ppat_constraint>(&p.desc)) return irref(*ct->p);
      if (auto* op = std::get_if<Ppat_open>(&p.desc)) return irref(*op->p);
      if (auto* o = std::get_if<Ppat_or>(&p.desc)) return irref(*o->l) || irref(*o->r);
      if (auto* tu = std::get_if<Ppat_tuple>(&p.desc)) {
        for (auto& e : tu->elems)
          if (!irref(*e)) return false;
        return true;
      }
      if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
        for (auto& [id, fp] : r->fields)
          if (!irref(*fp)) return false;
        return true;
      }
      if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) return irref(*lz->p);
      return false;
    };
    // `ctx_ok` carries "every OFF-PATH sibling above is irrefutable": a
    // wildcard only covers its column when the arm can't fail elsewhere --
    // ocamlc's pressure specializes the whole matrix, so `_, `Unchanged`'s
    // `_` does NOT open the first component (morematch).
    std::function<void(const Pattern&, const std::string&, bool, bool)> walk =
        [&](const Pattern& p, const std::string& path, bool collect,
            bool ctx_ok) {
      if (collect && ctx_ok &&
          (std::holds_alternative<Ppat_var>(p.desc) ||
           std::holds_alternative<Ppat_any>(p.desc))) {
        wild_sub.push_back(path);
        return;
      }
      if (auto* al = std::get_if<Ppat_alias>(&p.desc)) return walk(*al->p, path, collect, ctx_ok);
      if (auto* ct = std::get_if<Ppat_constraint>(&p.desc)) return walk(*ct->p, path, collect, ctx_ok);
      if (auto* op = std::get_if<Ppat_open>(&p.desc)) return walk(*op->p, path, collect, ctx_ok);
      if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
        walk(*o->l, path, collect, ctx_ok);
        walk(*o->r, path, collect, ctx_ok);
        return;
      }
      if (std::holds_alternative<Ppat_type>(p.desc)) {
        if (collect) {
          if (ctx_ok) wild_kids.push_back(path);
        } else if (covered(path)) open_row_pats_.insert(&p);
        return;
      }
      if (auto* v = std::get_if<Ppat_variant>(&p.desc)) {
        if (!collect && covered(path)) open_row_pats_.insert(&p);
        if (v->arg) walk(**v->arg, path + "`" + v->label + "/", collect, ctx_ok);
        return;
      }
      if (auto* tu = std::get_if<Ppat_tuple>(&p.desc)) {
        for (size_t i = 0; i < tu->elems.size(); ++i) {
          bool ok = ctx_ok;
          for (size_t j = 0; ok && j < tu->elems.size(); ++j)
            if (j != i && !irref(*tu->elems[j])) ok = false;
          walk(*tu->elems[i], path + std::to_string(i) + "/", collect, ok);
        }
        return;
      }
      if (auto* k = std::get_if<Ppat_construct>(&p.desc)) {
        if (k->arg) walk(**k->arg, path + lid_last(k->id.txt) + "/", collect, ctx_ok);
        return;
      }
      if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
        for (auto& [id, fp] : r->fields) {
          bool ok = ctx_ok;
          for (auto& [id2, fp2] : r->fields)
            if (fp2.get() != fp.get() && !irref(*fp2)) ok = false;
          walk(*fp, path + "." + lid_last(id.txt) + "/", collect, ok);
        }
        return;
      }
      if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) return walk(*lz->p, path + "lazy/", collect, ctx_ok);
    };
    for (auto& c : cases)
      if (!c.guard) walk(c.lhs, "/", /*collect=*/true, /*ctx_ok=*/true);
    if (wild_sub.empty() && wild_kids.empty()) return;
    for (auto& c : cases) walk(c.lhs, "/", /*collect=*/false, /*ctx_ok=*/true);
  }

  TypePtr infer_pat_impl(const Pattern& p) {
    if (auto* v = std::get_if<Ppat_var>(&p.desc)) {
      auto t = eng.fresh_var();
      venv.back()[v->name.txt] = t;  // monomorphic in its scope
      return t;
    }
    if (std::holds_alternative<Ppat_any>(p.desc)) return eng.fresh_var();
    if (auto* c = std::get_if<Ppat_constant>(&p.desc)) return constant_type(c->c);
    if (auto* tu = std::get_if<Ppat_tuple>(&p.desc)) {
      std::vector<TypePtr> es;
      for (auto& e : tu->elems) es.push_back(infer_pat(*e));
      return eng.tuple(std::move(es));
    }
    if (auto* k = std::get_if<Ppat_construct>(&p.desc)) {
      TypePtr* sch = find_ctor(lid_last(k->id.txt));
      // A QUALIFIED pattern ctor (`Float.FP_normal`) keeps M's own type path
      // (`Float.fpclass`), not the base its bare name resolves to -- ocamlc
      // follows the access path.  Prefer the qualified scheme when M's cmi
      // yields the ctor (falls through to the qualified branch).  All passes:
      // the bare-name hit can be an UNRELATED type's ctor (Dynlink.Error vs
      // result's Error), which false-rejects in strict.
      if (sch && std::holds_alternative<Ldot>(k->id.txt.v) &&
          qualified_ctor_scheme(k->id.txt))
        sch = nullptr;
      if (!sch) {
        // A qualified `M.C` not in scope: flatten its tuple by the cmi arity.
        if (auto* tup = k->arg ? std::get_if<Ppat_tuple>(&(*k->arg)->desc) : nullptr) {
          int ar = qualified_ctor_arity(k->id.txt);
          if (ar > 1 && (size_t)ar == tup->elems.size()) flatten_construct.insert(&p);
        } else if (k->arg && std::holds_alternative<Ppat_any>((*k->arg)->desc)) {
          // `M.C _`: the lone `_` fills every arity slot in the dump.
          int ar = qualified_ctor_arity(k->id.txt);
          if (ar > 1) construct_any_arity[&p] = ar;
        }
        // The qualified ctor's scheme pins the pattern: `Either.Left s` binds
        // s:'a and types the scrutinee `('a, 'b) Either.t`.
        if (TypePtr scheme = qualified_ctor_scheme(k->id.txt)) {
          TypePtr result;
          auto ps = ctor_params(scheme, result);
          if (k->arg) {
            auto* tup = std::get_if<Ppat_tuple>(&(*k->arg)->desc);
            if (ps.size() > 1 && tup && tup->elems.size() == ps.size())
              for (size_t i = 0; i < ps.size(); ++i) try_unify(ps[i], infer_pat(*tup->elems[i]));
            else if (!ps.empty()) try_unify(ps[0], infer_pat(**k->arg));
            else infer_pat(**k->arg);
          }
          return result;
        }
        if (k->arg) infer_pat(**k->arg);
        // In the kind pass, return a fresh var (not Any) so unification against
        // the scrutinee binds it to the constructor's real type -- the back end
        // then resolves the unqualified constructor through that type
        // (type-directed disambiguation: Visible/Hidden : Load_path.visibility
        // matched without `open Load_path`).  The strict pass keeps Any.
        return eng.fresh_var();  // P4-C: unknown ctor pattern -> fresh var (was Any)
      }
      TypePtr result;
      auto ps = ctor_params(eng.instantiate(*sch), result);
      if (k->arg) {
        auto* tup = std::get_if<Ppat_tuple>(&(*k->arg)->desc);
        // A multi-argument constructor `B of t1*t2` (arity>1) destructures a
        // tuple pattern element-wise; a single tuple-typed argument
        // `A of (t1*t2)` (arity 1) unifies the whole tuple against the one arg.
        if (ps.size() > 1 && tup && tup->elems.size() == ps.size()) {
          flatten_construct.insert(&p);
          for (size_t i = 0; i < ps.size(); ++i) try_unify(ps[i], infer_pat(*tup->elems[i]));
        } else if (!ps.empty()) {
          // `C _`: the lone `_` fills every arity slot in the dump (the local
          // ctor_arity_ registry covers local decls; this covers cmi ctors
          // brought into bare scope by an open).
          if (ps.size() > 1 && std::holds_alternative<Ppat_any>((*k->arg)->desc))
            construct_any_arity[&p] = (int)ps.size();
          try_unify(ps[0], infer_pat(**k->arg));
        } else {
          infer_pat(**k->arg);
        }
      }
      return result;
    }
    if (auto* ct = std::get_if<Ppat_constraint>(&p.desc)) {
      TypePtr pt = infer_pat(*ct->p);
      std::unordered_map<std::string, TypePtr> local;
      TypePtr at = from_coretype(*ct->t, annot_vars_ ? *annot_vars_ : local);
      try_unify(pt, at);
      return at;
    }
    if (auto* al = std::get_if<Ppat_alias>(&p.desc)) {
      if (record_kinds_) {  // kind pass: the scrutinee type gives the better kind
        TypePtr t = infer_pat(*al->p);
        venv.back()[al->name.txt] = t;
        return t;
      }
      std::unordered_map<const Pattern*, TypePtr> tys;
      auto* saved = as_map_;
      as_map_ = &tys;
      TypePtr t = infer_pat(*al->p);
      as_map_ = saved;
      venv.back()[al->name.txt] =
          pat_has_gadt_ctor(*al->p) ? t : build_as_type(*al->p, tys);
      return t;
    }
    if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
      // Type fields via the unique-label registry; an ambiguous/unknown label's
      // sub-pattern vars bind to Any (a polymorphic field used at several types
      // must not clash through one monomorphic var).
      TypePtr recTy = nullptr;
      for (auto& [lid, sub] : r->fields) {
        auto it = fields_.find(lid_last(lid.txt));
        if (it == fields_.end()) {
          // The predefined `'a ref = { mutable contents : 'a }`: a `{contents=x}`
          // pattern types as `'a ref`, binding x:'a (non-strict only -- strict
          // stays Any since a user record could also declare `contents`).
          if (!strict && lid_last(lid.txt) == "contents") {
            TypePtr el = infer_pat(*sub);
            TypePtr rt = eng.constr("ref", {el});
            if (recTy) try_unify(recTy, rt); else recTy = rt;
            continue;
          }
          // A polymorphic-field label (`{pf}`): resolve the record TYPE from it,
          // and bind the field variable to the field's GENERALIZED type so each
          // use in the body instantiates fresh (the universal `'a. ..` -- this is
          // the polymorphic-record-field feature).  from_coretype mints generic
          // vars, so storing that scheme in venv gives per-use polymorphism; a
          // single monomorphic binding (Any) would lose the result type.
          {  // strict too (P4): a fresh-var binding for a poly field would
             // clash across its uses (domains.ml {pf} at two format types)
            auto pit = poly_field_rec_.find(lid_last(lid.txt));
            if (pit != poly_field_rec_.end()) {
              std::unordered_map<std::string, TypePtr> fv;
              TypePtr fldTy = from_coretype(*pit->second.ftype, fv);
              if (!bind_poly_field(*sub, fldTy)) bind_pat_any(*sub);
              TypePtr rt = eng.instantiate(pit->second.recTy);
              if (recTy) try_unify(recTy, rt); else recTy = rt;
              continue;
            }
          }
          bind_pat_any(*sub); continue;
        }
        TypePtr s = I::Engine::repr(eng.instantiate(it->second));
        try_unify(infer_pat(*sub), s->cod);
        if (recTy) try_unify(recTy, s->dom); else recTy = s->dom;
      }
      return recTy ? recTy : eng.fresh_var();  // P4-B: no field resolved -> fresh var
    }
    if (auto* a = std::get_if<Ppat_array>(&p.desc)) {
      // `[| x; y |]` matches `'a array`, all elements sharing the element type.
      TypePtr el = eng.fresh_var();
      for (auto& e : a->elems) try_unify(el, infer_pat(*e));
      return eng.constr("array", {el});
    }
    if (auto* lz = std::get_if<Ppat_lazy>(&p.desc))  // `lazy p` matches `'a lazy_t`, p:'a
      return eng.constr("lazy_t", {infer_pat(*lz->p)});
    if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      // Both arms bind the same variables, and each shared variable must have
      // a unifiable type across the two arms (typecore: or-pattern arms share
      // their idents).  Unify the arm *types*, AND unify each same-named bound
      // variable across the arms -- otherwise a clash like `(x,_) | (_,x)`
      // matched at (int, string) goes undetected (left x : int, right x : string
      // are never tied, so neither clashes with the scrutinee).
      std::unordered_map<std::string, TypePtr> before = venv.back();
      TypePtr lt = infer_pat(*o->l);
      std::unordered_map<std::string, TypePtr> left;  // vars introduced by the left arm
      for (auto& [k, v] : venv.back()) {
        auto it = before.find(k);
        if (it == before.end() || it->second != v) left[k] = v;
      }
      TypePtr rt = infer_pat(*o->r);
      for (auto& [k, lv] : left) {
        auto it = venv.back().find(k);  // the same name as rebound by the right arm
        if (it != venv.back().end()) try_unify(lv, it->second);
      }
      try_unify(lt, rt);
      return lt;
    }
    if (auto* pv = std::get_if<Ppat_variant>(&p.desc)) {
      // A poly-variant pattern `` `A [p] `` bounds the scrutinee ABOVE: `[< `A
      // [of t]]`.  A match's arms merge to `[< tag-union ..]` (the scrutinee is at
      // most those tags).  Non-strict only (matched variants are conjunctive; the
      // strict pass stays a fresh var -- see the 1st reverted attempt).
      // A column with a catch-all needs no closing (pressure-lite): OPEN row,
      // the tag is lower-bound presence.
      TypePtr at = pv->arg ? infer_pat(**pv->arg) : eng.fresh_var();
      if (strict) return eng.fresh_var();
      return eng.variant_type({pv->label}, {at}, {(char)(pv->arg ? 1 : 0)},
                              open_row_pats_.count(&p) ? 0 : 1);
    }
    // `#t`: matches any of the variant abbreviation t's tags -> the row
    // `[< t's tags-with-declared-args ]` (so a scrutinee arm-merge unions the
    // tags AND ties the shared tags' args to the DECLARED types, e.g. maf's
    // `#recurs_type_expr` gives `` `TConstr of type_expr list ``, not a var).
    if (auto* ht = std::get_if<Ppat_type>(&p.desc)) {
      if (TypePtr row = hash_type_row(ht->id.txt)) {
        if (open_row_pats_.count(&p)) row->variant_kind = 0;  // pressure-lite
        return row;
      }
      return eng.fresh_var();
    }
    if (auto* ex = std::get_if<Ppat_exception>(&p.desc)) {
      infer_pat(*ex->p);  // binds vars; matches an exn, independent of scrutinee
      return eng.fresh_var();
    }
    if (auto* op = std::get_if<Ppat_open>(&p.desc)) {
      venv.emplace_back();
      cenv.emplace_back();  // M.(C ..): M's ctors resolve bare inside the pattern
      open_into(op->mod_.txt);
      if (!strict) open_module_ctors(op->mod_.txt);
      std::set<std::string> opened;  // names introduced by the open, not by the pat
      for (auto& [k, v] : venv.back()) opened.insert(k);
      TypePtr t = infer_pat(*op->p);
      cenv.pop_back();
      auto inner = std::move(venv.back());
      venv.pop_back();
      // hoist only the pattern's own bound vars; the opened names stay scoped out
      for (auto& [k, v] : inner)
        if (!opened.count(k)) venv.back()[k] = v;
      return t;
    }
    if (auto* ef = std::get_if<Ppat_effect>(&p.desc)) {
      infer_pat(*ef->eff);   // effect payload vars
      infer_pat(*ef->cont);  // continuation k
      return eng.fresh_var();
    }
    // `(module M : S)`: a first-class-module parameter has the package type
    // `(module S)` (the module name M is bound to a module, not a value).
    if (auto* up = std::get_if<Ppat_unpack>(&p.desc))
      return up->pkg ? package_type(*up->pkg, up->name.txt ? *up->name.txt : "")
                     : eng.fresh_var();
    return eng.fresh_var();
  }

  // Is `t` (after repr) a printf-family format type — format / format4 /
  // format6, possibly module-qualified?  A string literal in such a position is
  // a valid format and must be accepted as that type, not as `string`.
  static bool is_format_constr(const TypePtr& t0) {
    TypePtr t = I::Engine::repr(t0);
    return t->kind == I::Type::Kind::Constr && is_format_base(t->path);
  }

  // The argument arrow of a format string (printf "%d %s" -> int -> string -> 'r),
  // so a format-consuming application flows argument value-kinds (x:int in
  // `printf "%d" x`).  %a consumes two args, %t one; unknown directives -> Any.
  TypePtr format_arrow(const std::string& s, const std::vector<TypePtr>& fmtargs) {
    auto isdig = [](char c) { return c >= '0' && c <= '9'; };
    // format6 params: [0]=args-fn (built here), [1]=channel type for %a/%t
    // printers (Format.formatter/out_channel), [2]=printer result, back()=final
    // result.  %a ties its printer's value param to the value argument.
    // A short fmtargs (a degenerate format type) leaves chan/pres unconstrained
    // -- fresh vars, not Any: nothing else can name them, so they cannot clash
    // (P4 bucket G: any-removal).
    TypePtr result = fmtargs.back();
    TypePtr chan = fmtargs.size() > 1 ? fmtargs[1] : eng.fresh_var();
    TypePtr pres = fmtargs.size() > 2 ? fmtargs[2] : eng.fresh_var();
    std::vector<TypePtr> args;
    size_t i = 0, n = s.size();
    while (i < n) {
      if (s[i] != '%') { ++i; continue; }
      ++i;
      if (i >= n) break;
      if (s[i] == '%' || s[i] == '@' || s[i] == '!' || s[i] == ',') { ++i; continue; }
      bool ignored = false;  // `%_d` etc. read-and-discard: consume no argument
      if (s[i] == '_') { ignored = true; ++i; }
      auto add = [&](TypePtr t) { if (!ignored) args.push_back(std::move(t)); };
      for (; i < n; ++i) {  // flags / width / precision (`*` is an int arg)
        char c = s[i];
        if (c == '*') add(eng.constr("int"));
        else if (c != '+' && c != '-' && c != '#' && c != ' ' && c != '.' && !isdig(c)) break;
      }
      if (i >= n) break;
      char len = 0;  // l/n/L is a length modifier only before an int conversion;
                     // bare (or N) it is the deprecated counter directive %u
      if ((s[i] == 'l' || s[i] == 'n' || s[i] == 'L') && i + 1 < n &&
          std::string_view("dixXou").find(s[i + 1]) != std::string_view::npos) {
        len = s[i]; ++i;
      }
      char c = s[i]; ++i;
      switch (c) {
        case 'd': case 'i': case 'x': case 'X': case 'o': case 'u':
        case 'l': case 'n': case 'L': case 'N':  // Scan_get_counter: one int arg
          add(eng.constr(len == 'l' ? "int32" : len == 'n' ? "nativeint"
                         : len == 'L' ? "int64" : "int")); break;
        case 's': case 'S': add(eng.constr("string")); break;
        case 'c': case 'C': add(eng.constr("char")); break;
        case 'f': case 'e': case 'E': case 'g': case 'G': case 'F': case 'h': case 'H':
          add(eng.constr("float")); break;
        case 'b': case 'B': add(eng.constr("bool")); break;
        case 'a': {  // printer `chan -> 'v -> pres` + value `'v` (tied)
          TypePtr v = eng.fresh_var();
          add(eng.arrow(chan, eng.arrow(v, pres)));
          add(v);
          break;
        }
        case 't': add(eng.arrow(chan, pres)); break;  // printer `chan -> pres`
        case '(': {  // %(...%): the argument is itself a format6 (substitution).
          add(eng.constr("format6"));  // so a string-literal arg is typed as a format
          int depth = 1;               // skip the inner format up to the matching %)
          while (i < n && depth > 0) {
            if (s[i] == '%' && i + 1 < n) {
              if (s[i + 1] == '(') { depth++; i += 2; }
              else if (s[i + 1] == ')') { depth--; i += 2; }
              else i += 2;
            } else ++i;
          }
          break;
        }
        case '{': {  // %{...%}: a format6 argument (its type-digest is printed)
          add(eng.constr("format6"));
          int depth = 1;
          while (i < n && depth > 0) {
            if (s[i] == '%' && i + 1 < n) {
              if (s[i + 1] == '{') { depth++; i += 2; }
              else if (s[i + 1] == '}') { depth--; i += 2; }
              else i += 2;
            } else ++i;
          }
          break;
        }
        case '[': {  // scanf set `%[0-9a-z]`: reads a string
          if (i < n && s[i] == '^') ++i;   // negated set
          if (i < n && s[i] == ']') ++i;   // a literal ']' as the first member
          while (i < n && s[i] != ']') ++i;
          if (i < n) ++i;                  // the closing ']'
          add(eng.constr("string"));
          break;
        }
        default: add(eng.fresh_var()); break;  // a directive we don't type
                                               // (scanf %r reader): one arg of
                                               // unconstrained type, not Any
      }
    }
    TypePtr r = result;  // the printf function's result is the format's result param
    for (auto it = args.rbegin(); it != args.rend(); ++it) r = eng.arrow(*it, r);
    return r;
  }

  // Validate a format literal's flag / precision / conversion compatibility -- a
  // SUBSET of OCaml's CamlinternalFormat checks, restricted to combinations that
  // are ALWAYS an error, so this never false-rejects a valid format (printf is
  // pervasive).  Returns an empty string when no violation is found.
  static std::string format_validity_error(const std::string& s) {
    auto isdig = [](char c) { return c >= '0' && c <= '9'; };
    auto in = [](char c, const char* set) {
      return std::string_view(set).find(c) != std::string_view::npos;
    };
    size_t i = 0, n = s.size();
    while (i < n) {
      if (s[i] != '%') { ++i; continue; }
      ++i;
      if (i >= n) break;
      if (in(s[i], "%@!,")) { ++i; continue; }  // %% %@ %! %, : not conversions
      if (s[i] == '_') ++i;                      // %_d : ignored read
      bool fminus = false, fplus = false, fspace = false, fzero = false;
      while (i < n && in(s[i], "-+ #0")) {
        if (s[i] == '-') fminus = true; else if (s[i] == '+') fplus = true;
        else if (s[i] == ' ') fspace = true; else if (s[i] == '0') fzero = true;
        ++i;
      }
      bool has_width = false;
      if (i < n && s[i] == '*') { has_width = true; ++i; }
      else while (i < n && isdig(s[i])) { has_width = true; ++i; }
      bool has_prec = false;
      if (i < n && s[i] == '.') {
        has_prec = true; ++i;
        if (i < n && s[i] == '*') ++i; else while (i < n && isdig(s[i])) ++i;
      }
      if (i >= n) break;
      if (in(s[i], "lnL") && i + 1 < n && in(s[i + 1], "dixXou")) ++i;  // length modifier
      char c = s[i]; ++i;
      bool is_int = in(c, "dioxXunlLN");
      bool is_strchar = in(c, "sScC");
      bool is_float = in(c, "feEgGFhH");
      // '-' (left-justify) requires an explicit width.
      if (fminus && !has_width && (is_int || is_strchar || is_float))
        return "'-' without padding";
      // '+' / ' ' sign flags apply only to numeric conversions.
      if ((fplus || fspace) && is_strchar)
        return std::string("'") + (fplus ? '+' : ' ') + "' is incompatible with '" + c + "'";
      // Precision is incompatible with string / char conversions.
      if (has_prec && is_strchar)
        return std::string("precision is incompatible with '") + c + "'";
      // The '0' pad flag is incompatible with a precision on integer conversions.
      if (fzero && has_prec && is_int)
        return "precision is incompatible with '0'";
    }
    return std::string();
  }

  // Record the string literals at an expression's result positions (see the
  // call site in infer_expr_expected).
  void record_result_fmt_lits(const Expression& e) {
    if (auto* c = std::get_if<Pexp_constant>(&e.desc)) {
      if (std::holds_alternative<Pconst_string>(c->c.desc)) fmt_lits_.insert(&e);
      return;
    }
    if (auto* m = std::get_if<Pexp_match>(&e.desc)) {
      for (auto& cs : m->cases) record_result_fmt_lits(*cs.rhs);
    } else if (auto* tr = std::get_if<Pexp_try>(&e.desc)) {
      for (auto& cs : tr->cases) record_result_fmt_lits(*cs.rhs);
    } else if (auto* l = std::get_if<Pexp_let>(&e.desc)) {
      record_result_fmt_lits(*l->body);
    } else if (auto* sq = std::get_if<Pexp_sequence>(&e.desc)) {
      record_result_fmt_lits(*sq->e2);
    }
  }

  // Infer an expression with an expected type pushed down (bidirectional).  A
  // string literal expected at a format type is accepted as that format (OCaml's
  // type_format), with its argument arrow filled in so the consuming application
  // (printf/sprintf/...) flows argument value-kinds.
  TypePtr infer_expr_expected(const Expression& e, const TypePtr& expected) {
    mark_if_iarray(e, expected);  // `[|..|]` expected at iarray -> Immutable dump
    if (auto* c = std::get_if<Pexp_constant>(&e.desc))
      if (auto* s = std::get_if<Pconst_string>(&c->c.desc); s && is_format_constr(expected)) {
        if (strict)
          if (std::string fe = format_validity_error(s->s); !fe.empty())
            note_error("invalid format \"" + s->s + "\": " + fe);
        if (record_kinds_ || record_fmt_lits_)
          fmt_lits_.insert(&e);  // Lambda lowers it as a format; dump desugars it
        auto er = I::Engine::repr(expected);  // format6's arg0 ('a) is the args function
        if (er->kind == I::Type::Kind::Constr && !er->args.empty()) {
          std::vector<TypePtr> a = er->args; a[0] = format_arrow(s->s, a);
          // A literal's format6 ties slots 3 and 4 (oracle: `"%S\n" :
          // (string -> 'a, 'b, 'c, 'd, 'd, 'a) format6`).  This routes a
          // scanf receiver: Scanf.scanner's slot4 `'a -> 'd` flows into
          // slot3 'c (the scanner result), so `bscanf ib "%S\n" recv`
          // applies recv : (string -> 'x) -> 'x.  Printf formats already
          // have the slots equal, so the tie is a no-op there.
          if (a.size() >= 5) soft_unify(a[3], a[4]);
          return eng.constr(er->path, std::move(a));
        }
        return expected;
      }
    // A format string can be wrapped in an `if` (`printf (if b then "a" else
    // "b")`): push the expected format type into each branch so the literals are
    // typed as formats and the consumer's result param resolves (else the
    // branches type as plain `string` and printf's result stays 'a).
    if (is_format_constr(expected))
      if (auto* it = std::get_if<Pexp_ifthenelse>(&e.desc); it && it->else_) {
        try_unify(infer_expr(*it->cond), eng.constr("bool"));
        TypePtr tt = infer_expr_expected(*it->then_, expected);
        TypePtr te = infer_expr_expected(**it->else_, expected);
        try_unify(tt, te);
        return tt;
      }
    // A format-expected expression built from result positions (match/try arms,
    // let/sequence tails): the oracle's type_expect pushes the format type into
    // those positions, so their string literals type as formats and desugar in
    // the typed tree (`pr "%(%d%)" (match p with A -> "x%d" | ...)`).  Dump-only
    // (record_fmt_lits_): recording retypes nothing, so inference, the strict
    // pass, and the back end are untouched.
    if (record_fmt_lits_ && is_format_constr(expected))
      record_result_fmt_lits(e);
    TypePtr t = infer_expr(e);
    // Type-directed bare-constructor resolution: an unqualified constructor we
    // couldn't resolve (typed Any) whose EXPECTED type is a module-qualified
    // variant -- record that type at the node so infer_value_kinds exposes it via
    // expr_constr and the back end registers the type's ctors, resolving the bare
    // ctor (predef's `decl0 ~immediate:Always`, with immediate : Type_immediacy.t).
    if (record_kinds_)
      if (auto* k = std::get_if<Pexp_construct>(&e.desc))
        if (!find_ctor(lid_last(k->id.txt))) {
          TypePtr er = I::Engine::repr(expected);
          if (er->kind == I::Type::Kind::Constr && er->path.find('.') != std::string::npos)
            rec_expr_[&e] = expected;
        }
    // Optional-argument erasure (ocaml's type_argument): a value of type
    // `?l:.. -> ..` used where a non-optional arrow is expected is eta-expanded
    // with None for the omitted optional(s).  Recorded for the Lambda back end;
    // only in the value-kinds pass (the strict pass uses soft propagation, so the
    // un-erased type returned here never causes a false-rejection).
    if (record_kinds_ || record_fmt_lits_) {
      std::vector<bool> slots;
      std::vector<EtaSlot> eslots;
      bool erased = false;
      TypePtr a = I::Engine::repr(t), ex = I::Engine::repr(expected);
      while (a->kind == I::Type::Kind::Arrow) {
        bool ex_arrow = ex->kind == I::Type::Kind::Arrow;
        if (a->arrow_label == 2 &&
            !(ex_arrow && ex->arrow_label == 2 && ex->arrow_lbl == a->arrow_lbl)) {
          slots.push_back(true);  // erase this optional -> None
          eslots.push_back({true, a->arrow_label, a->arrow_lbl});
          erased = true;
          a = I::Engine::repr(a->cod);
          continue;
        }
        if (!ex_arrow) break;
        slots.push_back(false);  // a kept parameter -> eta param
        eslots.push_back({false, a->arrow_label, a->arrow_lbl});
        a = I::Engine::repr(a->cod);
        ex = I::Engine::repr(ex->cod);
      }
      // Need at least one erased optional and at least one kept (eta) parameter:
      // a trailing-only optional with nothing after it isn't eta-expandable here.
      bool has_kept = false;
      for (bool none_slot : slots) if (!none_slot) has_kept = true;
      if (erased && has_kept) {
        if (record_kinds_) erasures_[&e] = std::move(slots);
        if (record_fmt_lits_) eta_erasures_[&e] = std::move(eslots);
      }
    }
    // SIGNATURE pass: the erasure also changes the value's TYPE at this use --
    // `bump @@ x` (bump : ?cap:int -> int -> int, %apply expects 'a -> 'b)
    // views bump as `int -> int`, so _f : int -> int (ocamlc's type_argument).
    // Rebuilt (never mutated): bump's own scheme keeps its optional.  Only
    // where the EXPECTED type is an arrow at the erased position -- an
    // unknown/var expectation keeps the full type.  Strict and kinds passes
    // keep today's behavior (soft propagation / erasures_ recording).
    if (!strict && !record_kinds_) {
      struct Kept { TypePtr dom; int lk; std::string nm; };
      std::vector<Kept> keep;
      bool erased = false;
      TypePtr a = I::Engine::repr(t), ex = I::Engine::repr(expected);
      while (a->kind == I::Type::Kind::Arrow && ex->kind == I::Type::Kind::Arrow) {
        if (a->arrow_label == 2 &&
            !(ex->arrow_label == 2 && ex->arrow_lbl == a->arrow_lbl)) {
          erased = true;
          a = I::Engine::repr(a->cod);
          continue;
        }
        keep.push_back({a->dom, a->arrow_label, a->arrow_lbl});
        a = I::Engine::repr(a->cod);
        ex = I::Engine::repr(ex->cod);
      }
      if (erased) {
        TypePtr r = a;
        for (auto it = keep.rbegin(); it != keep.rend(); ++it)
          r = eng.arrow(it->dom, r, it->lk, it->nm);
        return r;
      }
    }
    return t;
  }

  TypePtr infer_expr(const Expression& e) {
    TypePtr t = infer_expr_impl(e);
    if (record_kinds_) rec_expr_[&e] = t;
    return t;
  }

  TypePtr infer_expr_impl(const Expression& e) {
    if (e.loc.start.lnum) cur_line_ = e.loc.start.lnum;
    if (auto* c = std::get_if<Pexp_constant>(&e.desc)) return constant_type(c->c);
    if (auto* id = std::get_if<Pexp_ident>(&e.desc))
      return lookup_value(id->id.txt);
    if (auto* a = std::get_if<Pexp_apply>(&e.desc))
      return infer_apply(*a, e);
    if (auto* f = std::get_if<Pexp_function>(&e.desc)) return infer_function(*f);
    if (auto* nt = std::get_if<Pexp_newtype>(&e.desc)) {  // fun (type a) -> e
      newtype_vars[nt->name.txt] = newtype_binding();
      return infer_expr(*nt->body);
    }
    if (auto* le = std::get_if<Pexp_let>(&e.desc)) {
      venv.emplace_back();
      infer_bindings(le->rf, le->bindings);
      TypePtr bt = infer_expr(*le->body);
      venv.pop_back();
      return bt;
    }
    if (auto* lo = std::get_if<Pexp_letop>(&e.desc)) {
      // `let+ p1 = e1 and+ p2 = e2 in body` desugars to
      //   (let+) ((and+) e1 e2) (fun (p1, p2) -> body)
      // with the and-ops folded left-associatively into nested pairs.  Typing
      // the desugaring lets a misused operator clash (e.g. `(let+) = 7` applied
      // as a function) instead of the whole letop collapsing to Any.
      auto op_type = [&](const std::string& nm) {
        return lookup_value(Longident{Lident{nm}});
      };
      // Fold the source value (and its operators) left-to-right.
      TypePtr src = infer_expr(*lo->let_.exp);
      for (auto& a : lo->ands) {
        TypePtr res = eng.fresh_var();
        try_unify(op_type(a.op.txt), eng.arrow(src, eng.arrow(infer_expr(*a.exp), res)));
        src = res;
      }
      // The binding function: parameter pattern nested to match the fold.
      venv.emplace_back();
      TypePtr patTy = infer_pat(lo->let_.pat);
      for (auto& a : lo->ands) patTy = eng.tuple({patTy, infer_pat(a.pat)});
      TypePtr funTy = eng.arrow(patTy, infer_expr(*lo->body));
      venv.pop_back();
      TypePtr result = eng.fresh_var();
      try_unify(op_type(lo->let_.op.txt), eng.arrow(src, eng.arrow(funTy, result)));
      return result;
    }
    if (auto* t = std::get_if<Pexp_tuple>(&e.desc)) {
      std::vector<TypePtr> es;
      for (auto& el : t->elems) es.push_back(infer_expr(*el));
      return eng.tuple(std::move(es));
    }
    if (auto* k = std::get_if<Pexp_construct>(&e.desc)) {
      if (strict) {
        std::string cn = lid_last(k->id.txt);
        if (private_variant_ctors_.count(cn) && !ambiguous_ctors_.count(cn)) {
          auto it = private_ctor_type_.find(cn);
          note_error("Cannot create values of the private type " +
                     (it != private_ctor_type_.end() ? it->second : cn));
        }
      }
      TypePtr* sch = find_ctor(lid_last(k->id.txt));
      // A QUALIFIED `M.C` (`Result.Ok`) keeps M's own type path (`Result.t`),
      // not the re-exported base (`result`) its bare name resolves to -- ocamlc
      // follows the access path.  When M's cmi yields the ctor, prefer that
      // scheme by falling through to the qualified branch below.  All passes:
      // the bare-name hit can be an UNRELATED type's ctor (Dynlink.Error vs
      // result's Error), which false-rejects in strict.
      if (sch && std::holds_alternative<Ldot>(k->id.txt.v) &&
          qualified_ctor_scheme(k->id.txt))
        sch = nullptr;
      if (!sch) {
        // A qualified `M.C` whose bare name isn't in scope: recover its variant
        // type from M's cmi (so an optional-arg default fixes the param type).
        if (auto* tup = k->arg ? std::get_if<Pexp_tuple>(&(*k->arg)->desc) : nullptr) {
          int ar = qualified_ctor_arity(k->id.txt);
          if (ar > 1 && (size_t)ar == tup->elems.size()) flatten_construct.insert(&e);
        }
        // The qualified ctor's full scheme pins its argument: `Either.Left "s"`
        // gives `(string, 'b) Either.t`, not `('a, 'b) Either.t`.
        if (TypePtr scheme = qualified_ctor_scheme(k->id.txt)) {
          TypePtr result;
          auto ps = ctor_params(scheme, result);
          if (k->arg) {
            auto* tup = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
            if (ps.size() > 1 && tup && tup->elems.size() == ps.size())
              for (size_t i = 0; i < ps.size(); ++i) try_unify(ps[i], infer_expr(*tup->elems[i]));
            else if (!ps.empty()) try_unify(ps[0], infer_expr(**k->arg));
            else infer_expr(**k->arg);
          }
          return result;
        }
        if (TypePtr qt = qualified_ctor_type(k->id.txt)) {
          if (k->arg) infer_expr(**k->arg);
          return qt;
        }
        if (k->arg) infer_expr(**k->arg);
        return eng.fresh_var();  // P4-C: unknown ctor -> fresh var (corpus-validated)
      }
      TypePtr result;
      auto ps = ctor_params(eng.instantiate(*sch), result);
      if (k->arg) {
        auto* tup = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
        if (ps.size() > 1 && tup && tup->elems.size() == ps.size()) {
          flatten_construct.insert(&e);
          for (size_t i = 0; i < ps.size(); ++i) try_unify(ps[i], infer_expr(*tup->elems[i]));
        } else if (!ps.empty()) {
          try_unify(ps[0], infer_expr(**k->arg));
        } else {
          infer_expr(**k->arg);
        }
      }
      return result;
    }
    if (auto* it = std::get_if<Pexp_ifthenelse>(&e.desc)) {
      try_unify(infer_expr(*it->cond), eng.constr("bool"));
      TypePtr tt = infer_expr(*it->then_);
      if (it->else_) { try_unify(tt, infer_expr(**it->else_)); return tt; }
      // No else: the then-branch must be unit and the whole expression is unit
      // (so a unit-returning `if c then e` body annotates `: int`).  Constrain
      // the branch softly in the strict pass to avoid false-rejecting a branch
      // we mis-typed; force it in the value-kinds pass so the unit kind flows.
      if (strict) soft_unify(tt, eng.constr("unit"));
      else try_unify(tt, eng.constr("unit"));
      return eng.constr("unit");
    }
    if (auto* s = std::get_if<Pexp_sequence>(&e.desc)) {
      infer_expr(*s->e1);
      return infer_expr(*s->e2);
    }
    if (auto* fo = std::get_if<Pexp_for>(&e.desc)) {
      try_unify(infer_expr(*fo->lo), eng.constr("int"));
      try_unify(infer_expr(*fo->hi), eng.constr("int"));
      venv.emplace_back();
      try_unify(infer_pat(fo->var), eng.constr("int"));
      infer_expr(*fo->body);
      venv.pop_back();
      return eng.constr("unit");
    }
    if (auto* wh = std::get_if<Pexp_while>(&e.desc)) {
      try_unify(infer_expr(*wh->cond), eng.constr("bool"));
      infer_expr(*wh->body);
      // `while true do .. done` never terminates, so it takes the expected
      // type (typecore's Texp_construct "true" special case): a fresh var here.
      if (auto* k = std::get_if<Pexp_construct>(&wh->cond->desc))
        if (auto* l = std::get_if<Lident>(&k->id.txt.v); l && l->name == "true" && !k->arg)
          return eng.fresh_var();
      return eng.constr("unit");
    }
    if (auto* tr = std::get_if<Pexp_try>(&e.desc)) {
      // Body and every handler share the result type; unify them into a fresh var
      // (not the body's type) so a handler pins it even when the body is bottom --
      // `try (..; assert false) with _ -> 0` is int, from the handler.
      TypePtr t = eng.fresh_var();
      try_unify(t, infer_expr(*tr->e));
      for (auto& c : tr->cases) {
        check_case_structure(c);
        venv.emplace_back();
        // A handler pattern always matches an `exn` value, so pin it -- a bare
        // `with e -> ..` then gives e : exn (not a free var).
        try_unify(infer_pat(c.lhs), eng.constr("exn"));
        if (c.guard) try_unify(infer_expr(**c.guard), eng.constr("bool"));
        try_unify(t, infer_expr(*c.rhs));
        venv.pop_back();
      }
      return t;
    }
    if (auto* m = std::get_if<Pexp_match>(&e.desc)) {
      TypePtr se = infer_expr(*m->e);
      TypePtr sr = I::Engine::repr(se);
      // Matching a GADT refines types branch-locally (e.g. Int -> int, Ptr ->
      // int list at result type 'a; or a scrutinee component refined to ab in
      // one branch and xy in another).  Our global unification can't model that,
      // so for a GADT match -- detected by the scrutinee's type OR by any case
      // using a GADT constructor -- we skip both the pattern/scrutinee unify and
      // the branch-result cross-unify, leaving types open.
      bool gadt = sr->kind == I::Type::Kind::Constr && gadt_types.count(sr->path);
      for (auto& c : m->cases) if (pat_has_gadt_ctor(c.lhs)) gadt = true;
      // Branch-local refinement (strict pass): each arm types inside an engine
      // window -- the pattern/scrutinee and result unifications happen (soft:
      // refinement bindings, clashes swallowed) and roll back after the arm, so
      // one arm's `a := float` cannot leak into the next arm's `a := int32`.
      // The kind pass keeps the historical skip (rolled-back bindings would
      // erase value kinds it records).
      // The DISPLAY pass needs no window at all: locally-abstract types are
      // RIGID there, so an arm's equation (`a = float`) is a lenient constr
      // mismatch that cannot leak -- and full unification lets an arm body's
      // ORDINARY pins persist (`Buffer.set typ c_buffer` types the captured
      // buffer; a window rolled those back too -- common.ml's tail).
      bool display = fold_abbrevs_ && !strict;
      bool window = gadt && !record_kinds_ && !display;
      if (display) gadt = false;  // full (lenient) unify, like a plain match
      TypePtr rt = eng.fresh_var();
      // For a windowed (GADT) match, recover the result type when EVERY branch,
      // after its refinement is rolled back, yields the SAME ground type: then
      // the result genuinely is that type (e.g. all arms return `unit`/`string`),
      // not an abstract per-branch one.  If arms disagree on a ground type
      // (`Int -> 100` vs `Ptr -> p` : int vs int list) OR any arm is non-ground,
      // the result is abstract (`: a`) and must stay open -- so a later
      // annotation (`: a`) can pin it.  This flips w04_failure without
      // over-specialising register_typing.
      bool all_ground = window, ground_clash = false;
      TypePtr gacc = nullptr;
      // (A SPLIT window -- rolling the refinement back before the arm body so
      // body side effects persist -- was tried and reverted: arm bodies typed
      // under an un-refined pattern leak wrong bindings; -4/+1 corpus-wide.)
      mark_open_row_pats(m->cases);
      for (auto& c : m->cases) {
        check_case_structure(c);
        venv.emplace_back();
        size_t wm = window ? eng.mark() : 0;
        TypePtr pt = infer_pat(c.lhs);
        if (window) soft_unify(pt, se);
        else if (!gadt) try_unify(pt, se);
        if (c.guard) infer_expr(**c.guard);
        TypePtr br = infer_expr(*c.rhs);
        if (window) soft_unify(br, rt);
        else if (!gadt) try_unify(br, rt);
        if (window) {
          eng.undo_to(wm);
          TypePtr brr = I::Engine::repr(br);
          if (type_is_ground(brr)) {
            if (!gacc) gacc = brr;
            else if (!ground_types_equal(gacc, brr)) ground_clash = true;
          } else all_ground = false;
        }
        venv.pop_back();
      }
      if (window && all_ground && !ground_clash && gacc) soft_unify(rt, gacc);
      match_partial[&e] = compute_partial(se, m->cases);  // for the dump (Slice 3)
      return rt;
    }
    if (auto* ct = std::get_if<Pexp_constraint>(&e.desc)) {
      TypePtr et = infer_expr(*ct->e);
      std::unordered_map<std::string, TypePtr> vars;
      TypePtr at = from_coretype(*ct->t, vars);
      if (strict && expected_clash(et, at))  // (e : T) with e of a clashing type
        note_error("expression does not match the type constraint");
      mark_if_iarray(*ct->e, at);  // `([|..|] : _ iarray)` -> Immutable dump
      // Flow the annotation into the inner expression in the NON-strict passes
      // (value-kinds AND signature): `ignore (f s : int)` then pins `f : _ -> int`
      // in the inferred signature, not just the value kinds.  Not in the strict
      // reject pass, where an incomplete unify can propagate a spurious clash and
      // cost a false-rejection.  Soft (try_unify) so a stray clash can't abort.
      if (!strict) try_unify(et, at);
      return at;
    }
    if (auto* co = std::get_if<Pexp_coerce>(&e.desc)) {
      // A coercion `(e :> T)` or `(e : T1 :> T2)` has the TARGET type T/T2.
      infer_expr(*co->e);  // infer the source for its kinds/effects
      std::unordered_map<std::string, TypePtr> vars;
      return from_coretype(*co->to_, vars);
    }
    if (auto* pk = std::get_if<Pexp_pack>(&e.desc)) {
      // `(module ME : S)` is a first-class module of package type `(module S)`.
      if (pk->pkg) {
        // Packing a LOCAL module ties its exports to the modtype's declared
        // val types with the `with type` constraints substituted:
        // `(module M : S with type t = s)` unifies M's to_string against
        // `s -> string`, so create's param displays `('a -> string)`.
        if (!strict) {
          const ModuleExpr* m = pk->me.get();
          while (auto* mc = std::get_if<Pmod_constraint>(&m->desc)) m = mc->me.get();
          const Pmod_ident* mi = std::get_if<Pmod_ident>(&m->desc);
          const Lident* ml = mi ? std::get_if<Lident>(&mi->id.txt.v) : nullptr;
          auto ex = ml ? modenv.find(ml->name) : modenv.end();
          if (ex != modenv.end()) {
            std::unordered_map<std::string, TypePtr> argtypes;
            for (auto& [lid, ctb] : pk->pkg->constraints) {
              std::unordered_map<std::string, TypePtr> vars;
              argtypes[lid_full(lid.txt)] = from_coretype(*ctb, vars);
            }
            // The packed struct's own manifests resolve the sig's remaining
            // abstract types (`type t1 = s1` in P -> the sig's t1 IS s1).
            if (auto st = local_module_structs_.find(ml->name);
                st != local_module_structs_.end())
              for (auto& sit : *st->second)
                if (auto* ty = std::get_if<Pstr_type>(&sit.desc))
                  for (auto& d : ty->decls)
                    if (d.manifest && !argtypes.count(d.name.txt)) {
                      std::unordered_map<std::string, TypePtr> vars;
                      argtypes[d.name.txt] = from_coretype(**d.manifest, vars);
                    }
            std::unordered_map<std::string, TypePtr> schemes;
            if (auto* pl = std::get_if<Lident>(&pk->pkg->path.txt.v)) {
              if (auto sg = modtype_sig_asts_.find(pl->name);
                  sg != modtype_sig_asts_.end())
                schemes = unpack_module_values(ml->name, *sg->second, &argtypes);
            }
            if (schemes.empty())
              schemes = cmi_modtype_value_schemes(pk->pkg->path.txt, argtypes);
            // Tie only exports still UNRESOLVED (a var): flowing the sig type
            // into a concrete export would capture free vars the other way
            // (S.elt swallowing make_set's newtype).
            for (auto& [nm, sch] : schemes)
              if (auto f = ex->second.find(nm); f != ex->second.end())
                if (I::Engine::repr(f->second)->kind == I::Type::Kind::Var)
                  soft_unify(f->second, eng.instantiate(sch));
          }
        }
        return package_type(*pk->pkg);
      }
      // Unconstrained pack `(module M)` (no `: S`): the package type comes
      // from the CONTEXT (a fresh var per occurrence unifies with it); only a
      // single occurrence meeting incompatible contexts clashes.
      return eng.fresh_var();
    }
    if (auto* nw = std::get_if<Pexp_new>(&e.desc)) {
      // `new c` for a parameterless local class is its object type; a class
      // with constructor params is the constructor arrow (mixin2's
      // `lazy_fix (new lambda_ops)`).
      if (auto it = class_types_.find(lid_last(nw->id.txt)); it != class_types_.end())
        return eng.instantiate(it->second);
      if (auto it = class_ctor_types_.find(lid_last(nw->id.txt));
          it != class_ctor_types_.end())
        return eng.instantiate(it->second);
      return eng.fresh_var();  // P4-D: unknown class, per-occurrence var
    }
    if (auto* lz = std::get_if<Pexp_lazy>(&e.desc)) {
      // `lazy e` : the PRIMITIVE `e lazy_t` (what ocamlc -i shows for a
      // constructed lazy value).  The engine's lazy-family unify makes lazy_t
      // and the cmi-loaded Lazy.t compatible AND relinks a live construction
      // that flows into a Lazy.t context (hamming), while generalize/demote's
      // finalized-head stamp + instantiate's fresh copy protect a finished
      // binding from later-use relinks (`let l = lazy 1;; Lazy.force l` keeps
      // `int lazy_t`).  This is the per-occurrence access-path rule.
      TypePtr inner = infer_expr(*lz->e);
      return eng.constr("lazy_t", {inner});  // P4-H: guard removed, corpus-validated
    }
    if (auto* as = std::get_if<Pexp_assert>(&e.desc)) {
      TypePtr ct = infer_expr(*as->e);  // infer the condition (flows operand kinds)
      // `assert false` is bottom ('a, never returns): a fresh var, so the
      // surrounding result type is decided by the other branches -- it neither
      // pins a polymorphic result (a fold accumulator) nor absorbs a concrete one
      // (`try (..; assert false) with _ -> 0` is int, from the handler).
      if (auto* ctr = std::get_if<Pexp_construct>(&as->e->desc))
        if (lid_last(ctr->id.txt) == "false") return generic_var();
      // `assert e` forces e : bool, so `assert (f x)` pins `f x : bool` (and thus
      // f's result).  Non-strict only (an incomplete strict inference could clash).
      try_unify(ct, eng.constr("bool"));  // P4: strict too (corpus-validated)
      // `assert e` (e != false) is unit.  A concrete unit can cause our
      // incomplete strict pass to false-reject, so there alone we keep Any; the
      // value-kinds and signature passes commit to unit.
      return eng.constr("unit");  // P4-H: guard removed, corpus-validated
    }
    // Records, via the unique-label registry (ambiguous labels -> Any).
    if (auto* fld = std::get_if<Pexp_field>(&e.desc)) {
      // A QUALIFIED label `e.M[.P].label` resolves through its written path
      // (OCaml's path-directed disambiguation) BEFORE the bare-name registry:
      // the path is authoritative, and it also PINS the base's record type
      // (`(stat file).Unix.st_size` gives stat : 'a -> Unix.stats).
      if (std::holds_alternative<Ldot>(fld->field.txt.v))
        if (TypePtr qfs = qualified_field_scheme(fld->field.txt)) {
          TypePtr s = I::Engine::repr(eng.instantiate(qfs));
          TypePtr bt = infer_expr(*fld->e);
          try_unify(bt, s->dom);
          if (record_kinds_) pending_field_.push_back({&e, bt, lid_last(fld->field.txt)});
          return s->cod;
        }
      if (TypePtr fsch = field_scheme(lid_last(fld->field.txt))) {
        TypePtr s = I::Engine::repr(eng.instantiate(fsch));  // recTy -> fldTy
        TypePtr bt = infer_expr(*fld->e);
        try_unify(bt, s->dom);
        // A label the inferencer sees as UNIQUE (it models only local records) can
        // still be ambiguous to the back end (the opened `type_expr.level` vs the
        // local `pool.level`); record the base+label so the post-inference pass
        // emits the resolved index and the back end doesn't pick the wrong offset.
        if (record_kinds_) pending_field_.push_back({&e, bt, lid_last(fld->field.txt)});
        return s->cod;
      }
      TypePtr bt = infer_expr(*fld->e);
      // An AMBIGUOUS label (omitted from fields_) resolved through the base's
      // type IDENTITY: find the stamped record decl and read the field's index/
      // mutability/kind, so the back end need not guess between same-named records.
      {
        TypePtr rb = I::Engine::repr(bt);
        std::string lbl = lid_last(fld->field.txt);
        // Queue for the post-inference pass: `bt`'s repr may only become a stamped
        // record after a later field read unifies the base's type.  Record for ANY
        // field on a record base, not just labels the inferencer deems ambiguous --
        // the inferencer models only LOCAL records, so a label it sees as unique
        // (`pool.level`) is AMBIGUOUS to the back end (which also sees the opened
        // `type_expr.level`); without the resolved index the back end picks the
        // last-opened (wrong) offset -> e.g. pool_of_level loops forever.
        if (record_kinds_) pending_field_.push_back({&e, bt, lbl});
        if (rb->kind == I::Type::Kind::Constr && rb->stamp) {
          auto dit = stamp_record_decl_.find(rb->stamp);
          if (dit != stamp_record_decl_.end())
            if (auto* rec = std::get_if<Ptype_record>(&dit->second->kind)) {
              bool all_float = !rec->fields.empty();
              for (auto& f : rec->fields)
                if (ct_kind(*f.type) != "float") { all_float = false; break; }
              if (!all_float)
                for (int i = 0; i < (int)rec->fields.size(); ++i)
                  if (rec->fields[i].name.txt == lbl) {
                    field_resolved_[&e] = {i, rec->fields[i].mut == MutableFlag::Mutable,
                                           ct_kind(*rec->fields[i].type)};
                    break;
                  }
            }
        }
      }
      // The predefined `'a ref = { mutable contents : 'a }` cell.
      if (lid_last(fld->field.txt) == "contents") {
        TypePtr rb = I::Engine::repr(bt);
        if (rb->kind == I::Type::Kind::Constr && rb->path == "ref" && rb->args.size() == 1)
          return rb->args[0];
        // Base not yet known to be a ref: in the non-strict passes commit it to
        // `'a ref` (the only predefined record with a `contents` field), so
        // `fun r -> r.contents + 1` infers `int ref -> int`, not `'a -> int`.
        // Strict stays Any: a user record could also declare `contents`.
        if (!strict && rb->kind == I::Type::Kind::Var) {
          TypePtr el = eng.fresh_var();
          try_unify(bt, eng.constr("ref", {el}));
          return el;
        }
      }
      // A module-qualified field `e.M.label` of a stdlib record: its declared type.
      if (auto* d = std::get_if<Ldot>(&fld->field.txt.v))
        if (auto* pl = std::get_if<Lident>(&d->prefix->v))
          if (TypePtr ft = stdlib_field_type(pl->name, d->name)) return ft;
      // Resolve an unqualified label through the base's inferred type
      // (`(Gc.quick_stat ()).major_collections` with the base : Gc.stat), so the
      // value-kinds pass specializes comparisons/kinds AND the signature pass
      // gets the field's real type instead of leaking Any.  Strict stays Any (a
      // local record could share the label).
      if (!strict) {
        TypePtr rb = I::Engine::repr(bt);
        if (rb->kind == I::Type::Kind::Constr) {
          auto dpos = rb->path.rfind('.');
          if (dpos != std::string::npos &&
              rb->path.find('.') == dpos)  // single-module prefix
            if (TypePtr ft = stdlib_field_type(rb->path.substr(0, dpos),
                                               lid_last(fld->field.txt)))
              return ft;
        }
      }
      return eng.fresh_var();  // P4-B: unresolved field access -> fresh var (was Any)
    }
    if (auto* rc = std::get_if<Pexp_record>(&e.desc)) {
      // Record update `{ e with ... }`: ocamlc types the result as a FRESH
      // instance of the record type, tied to the base only through the KEPT
      // (non-overridden) fields (typecore's unify_kept).  A type parameter that
      // appears only in overridden fields may therefore CHANGE across the
      // update: `{ node with schedule = f node.schedule }` maps `'a node` to
      // `'b node`.  Soundness value is low, so all of this stays non-strict.
      if (rc->base) {
        bool sv = strict; strict = false;
        TypePtr bt = infer_expr(**rc->base);
        std::vector<std::pair<std::string, TypePtr>> overr;
        for (auto& [lbl, val] : rc->fields)
          overr.emplace_back(lid_last(lbl.txt), infer_expr(*val));
        // Pin the base's record identity through the first resolvable label so
        // the decl lookup below sees a Constr even when the base is a bare var.
        for (auto& [lbl, vt] : overr)
          if (TypePtr fsch = field_scheme(lbl)) {
            TypePtr s = I::Engine::repr(eng.instantiate(fsch));
            try_unify(bt, s->dom);
            break;
          }
        // The two-instance model needs the decl's full field list (to know the
        // kept fields), with every field's scheme resolvable and every written
        // label belonging to the decl; otherwise fall back to result == base.
        TypePtr rb = I::Engine::repr(bt);
        const TypeDeclaration* decl = nullptr;
        if (rb->kind == I::Type::Kind::Constr) {
          if (rb->stamp) { auto it = stamp_record_decl_.find(rb->stamp);
                           if (it != stamp_record_decl_.end()) decl = it->second; }
          if (!decl && !ambiguous_record_names_.count(rb->path)) {
            auto it = name_record_decl_.find(rb->path);
            if (it != name_record_decl_.end()) decl = it->second;
          }
        }
        const Ptype_record* rec =
            decl ? std::get_if<Ptype_record>(&decl->kind) : nullptr;
        bool split = rec != nullptr;
        if (split) {
          std::set<std::string> declset;
          for (auto& f : rec->fields) declset.insert(f.name.txt);
          for (auto& f : rec->fields)
            if (!field_scheme(f.name.txt)) { split = false; break; }
          for (auto& [lbl, vt] : overr)
            if (!declset.count(lbl)) { split = false; break; }
        }
        TypePtr resTy = nullptr;
        if (split) {
          std::set<std::string> overrset;
          for (auto& [lbl, vt] : overr) {
            overrset.insert(lbl);
            TypePtr s = I::Engine::repr(eng.instantiate(field_scheme(lbl)));
            if (resTy) try_unify(resTy, s->dom); else resTy = s->dom;
            try_unify(vt, s->cod);
          }
          for (auto& f : rec->fields) {  // unify_kept
            if (overrset.count(f.name.txt)) continue;
            TypePtr fsch = field_scheme(f.name.txt);
            TypePtr s1 = I::Engine::repr(eng.instantiate(fsch));
            try_unify(s1->dom, bt);
            TypePtr s2 = I::Engine::repr(eng.instantiate(fsch));
            try_unify(s2->dom, resTy);
            try_unify(s1->cod, s2->cod);
          }
        } else {
          // Constrain each overridden field to its declared type, tying the field
          // scheme's record-type (dom) to the base record so its type parameters are
          // shared: `{ M.null_tracker with alloc_minor }` recovers alloc_minor's
          // type (`M.allocation -> 'a option`) instead of leaking a free var.
          for (auto& [lbl, vt] : overr)
            if (TypePtr fsch = field_scheme(lbl)) {
              TypePtr s = I::Engine::repr(eng.instantiate(fsch));
              try_unify(bt, s->dom);
              try_unify(vt, s->cod);
            }
          // The update's type IS the base record's type; returning it (instead
          // of `any`) lets a field read on the result (`let it = {super with ..}
          // in it.it_module_type`) resolve its label through that record type.
          resTy = bt;
        }
        strict = sv;
        // For the dump's `<kept>` fields, resolve an EXTERNAL record type's full
        // ordered field list from the cmis (local records use the transcriber's
        // own field registry).
        { TypePtr rb2 = I::Engine::repr(bt);
          if (rb2->kind == I::Type::Kind::Constr) {
            std::string repr;
            auto fs = cmi_record_fields(rb2->path, &repr);
            if (!fs.empty()) record_fields[&e] = std::move(fs);
            if (!repr.empty()) record_reprs[&e] = std::move(repr);
          } }
        return resTy;
      }
      TypePtr recTy = nullptr;
      for (auto& [lbl, val] : rc->fields) {
        // Infer the field value for its type, but suppress errors from inside its
        // body: traversing field values exposes unrelated inference incompleteness
        // (effect handlers, polymorphic recursion).  The record-shape check below
        // still records (strict restored).
        bool sv = strict; strict = false;
        TypePtr vt = infer_expr(*val);
        strict = sv;
        TypePtr fsch = field_scheme(lid_last(lbl.txt));
        if (!fsch) {
          // `{contents = e}` builds the predefined `'a ref` (non-strict only).
          if (!strict && lid_last(lbl.txt) == "contents") {
            TypePtr rt = eng.constr("ref", {vt});
            if (recTy) try_unify(recTy, rt); else recTy = rt;
          }
          continue;
        }
        TypePtr s = I::Engine::repr(eng.instantiate(fsch));
        try_unify(vt, s->cod);
        if (recTy) try_unify(recTy, s->dom); else recTy = s->dom;
      }
      // Completeness: a plain record construction must define every field of its
      // type.  Resolve the record decl unambiguously (stamp, else unique name)
      // and flag a missing field -- but only when every provided label belongs to
      // that record, so a mis-resolved type can't cause a false report.
      if ((strict || recmod_body_) && recTy) {
        TypePtr rb = I::Engine::repr(recTy);
        const TypeDeclaration* decl = nullptr;
        if (rb->kind == I::Type::Kind::Constr) {
          if (rb->stamp) { auto it = stamp_record_decl_.find(rb->stamp);
                           if (it != stamp_record_decl_.end()) decl = it->second; }
          if (!decl && !ambiguous_record_names_.count(rb->path)) {
            auto it = name_record_decl_.find(rb->path);
            if (it != name_record_decl_.end()) decl = it->second;
          }
        }
        if (decl)
          if (auto* rec = std::get_if<Ptype_record>(&decl->kind)) {
            std::set<std::string> provided, declset;
            for (auto& [lbl, val] : rc->fields) provided.insert(lid_last(lbl.txt));
            for (auto& f : rec->fields) declset.insert(f.name.txt);
            bool all_known = true;
            for (auto& p : provided) if (!declset.count(p)) { all_known = false; break; }
            if (all_known)
              for (auto& f : rec->fields)
                if (!provided.count(f.name.txt)) {
                  note_error("Some record fields are undefined: " + f.name.txt);
                  break;
                }
          }
      }
      // Record an EXTERNAL record type's declared field order so the transcriber
      // emits fields in decl order (not source order) -- e.g. a plain
      // `{ MP.alloc_minor; promote; alloc_major; ... }` from a cmi record.
      // Local records already carry their order in the transcriber's registry.
      if (recTy) {
        TypePtr rb = I::Engine::repr(recTy);
        if (rb->kind == I::Type::Kind::Constr) {
          std::string repr;
          auto fs = cmi_record_fields(rb->path, &repr);
          if (!fs.empty()) record_fields[&e] = std::move(fs);
          if (!repr.empty()) record_reprs[&e] = std::move(repr);
        }
      }
      return recTy ? recTy : eng.fresh_var();  // P4-B: no field resolved -> fresh var
    }
    if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) {
      if (strict) {
        std::string fn = lid_last(sf->field.txt);
        if (private_record_fields_.count(fn) && !nonprivate_record_fields_.count(fn)) {
          auto it = private_field_type_.find(fn);
          note_error("Cannot assign field " + fn + " of the private type " +
                     (it != private_field_type_.end() ? it->second : fn));
        }
      }
      if (TypePtr fsch = field_scheme(lid_last(sf->field.txt))) {
        TypePtr s = I::Engine::repr(eng.instantiate(fsch));
        try_unify(infer_expr(*sf->obj), s->dom);
        try_unify(infer_expr(*sf->value), s->cod);
      } else { infer_expr(*sf->obj); infer_expr(*sf->value); }
      return eng.constr("unit");
    }
    if (auto* a = std::get_if<Pexp_array>(&e.desc)) {
      TypePtr el = eng.fresh_var();
      for (auto& x : a->elems) try_unify(el, infer_expr(*x));
      return eng.constr("array", {el});
    }
    if (auto* sti = std::get_if<Pexp_struct_item>(&e.desc)) {
      // `let open M in e`, `let module M = ... in e`, `let exception ... in e`:
      // process the item into a fresh scope, then type the body.
      venv.emplace_back();
      cenv.emplace_back();  // a local `M.(..)` open may bring M's ctors into scope
      // A LOCAL exception / type extension is only seen here (the top-level
      // register_types_rec pass that registers exception/typext ctors never
      // descends into expressions), so register its ctors now -- otherwise
      // `let exception E of t in E x` leaves `E x` as Any instead of exn.
      if (auto* ex = std::get_if<Pstr_exception>(&sti->item->desc))
        register_exception(ex->exn.ctor);
      else if (auto* tx = std::get_if<Pstr_typext>(&sti->item->desc))
        register_typext(tx->ext);
      else if (auto* op = std::get_if<Pstr_open>(&sti->item->desc)) {
        // `let open struct type _ t = C : .. t .. end in ..`: register the local
        // struct's GADT markers so a match on its ctors is windowed.
        if (auto* ms = std::get_if<Pmod_structure>(&op->expr.desc))
          register_local_gadt_markers(ms->items);
      } else if (auto* lm = std::get_if<Pstr_module>(&sti->item->desc)) {
        if (lm->binding.name.txt && !strict) {
          std::string p = resolve_local_module_path(lm->binding.expr);
          auto [f, inserted] = local_module_paths_.emplace(*lm->binding.name.txt, p);
          if (!inserted && f->second != p) f->second = "";  // conflicting rebind
          const ModuleExpr* lme = &lm->binding.expr;
          while (auto* mc2 = std::get_if<Pmod_constraint>(&lme->desc)) lme = mc2->me.get();
          if (auto* ms = std::get_if<Pmod_structure>(&lme->desc))
            local_module_structs_[*lm->binding.name.txt] = &ms->items;
        }
      }
      // A `let open M in e` is scoped to `e`, but process_item mutates the
      // file-wide display-qualification maps (bare `t` -> M.t) in place.  Save
      // and restore them across a scoped open so the open does not leak past `e`
      // -- otherwise `let open Printexc` (after `open Effect`) rewrites the
      // earlier `type _ t += ..`'s `t` to Printexc.t at the later emission phase.
      bool scoped_open = std::holds_alternative<Pstr_open>(sti->item->desc);
      auto saved_type_quals = opened_type_quals_;
      auto saved_submod_quals = opened_submod_quals_;
      process_item(*sti->item);
      TypePtr bt = infer_expr(*sti->body);
      if (scoped_open) {
        opened_type_quals_ = std::move(saved_type_quals);
        opened_submod_quals_ = std::move(saved_submod_quals);
      }
      cenv.pop_back();
      venv.pop_back();
      return bt;
    }
    if (auto* sd = std::get_if<Pexp_send>(&e.desc)) {  // o#m: the method's type
      TypePtr ot = I::Engine::repr(infer_expr(*sd->obj));
      // A class-typed receiver (`y : alfa`, y coerced/annotated to a class): the
      // object appears as the class's Constr, so resolve the method through the
      // class's object row (woodyatt: `y#x` where x : format -> 'a).
      if (ot->kind == I::Type::Kind::Constr)
        if (auto it = class_types_.find(ot->path); it != class_types_.end())
          ot = I::Engine::repr(eng.instantiate(it->second));
      if (ot->kind == I::Type::Kind::Object)
        for (size_t i = 0; i < ot->labels.size(); ++i)
          if (ot->labels[i] == sd->meth.txt) return ot->args[i];
      return eng.fresh_var();  // P4-D: unknown receiver/method, per-occurrence var
    }
    if (auto* si = std::get_if<Pexp_setinstvar>(&e.desc)) {  // n <- e: e has n's type
      TypePtr vt = infer_expr(*si->value);
      for (auto it = venv.rbegin(); it != venv.rend(); ++it)
        if (auto f = it->find(si->name.txt); f != it->end()) { try_unify(f->second, vt); break; }
      return eng.constr("unit");  // P4: n <- e is unit (corpus-validated)
    }
    if (auto* pv = std::get_if<Pexp_variant>(&e.desc)) {
      // A constructed polymorphic variant `` `A [e] `` has the open row type
      // `[> `A [of t]]`; rows merge through unification (if/match branches).
      // Construction rows unify shared-tag args -- OCaml's semantics for `[>`
      // (two `` `A `` at clashing arg types IS an error).  The conjunctive trap
      // is the MATCHED side only (`[<` args conjoin, not unify), and strict's
      // Ppat_variant stays a fresh var, so no `[<` row reaches strict unify.
      TypePtr at = pv->arg ? infer_expr(**pv->arg) : eng.fresh_var();
      return eng.variant_type({pv->label}, {at}, {(char)(pv->arg ? 1 : 0)});
    }
    if (auto* ob = std::get_if<Pexp_object>(&e.desc)) {
      // All passes type the method bodies and build the object type
      // `< m : t; .. >` (the signature pass renders it; the value-kind pass needs
      // the bodies for kinds/format literals; strict checks the bodies -- the
      // same routine already runs in strict for class declarations).
      return infer_object_body(*ob->cs);
    }
    if (auto* ov = std::get_if<Pexp_override>(&e.desc)) {
      // `{< x = e >}`: each e takes the instance variable's type; the result
      // is the SELF type, which the engine doesn't model -- fresh per
      // occurrence (the object rows core remains future work).
      for (auto& [nm, fe] : ov->fields) infer_expr(*fe);
      return eng.fresh_var();
    }
    if (auto* xt = std::get_if<Pexp_extension>(&e.desc)) {
      // The compiler-interpreted expression extensions type for real; any
      // other extension is what ocamlc rejects as uninterpreted.
      if (xt->name == "extension_constructor" || xt->name == "ocaml.extension_constructor")
        return eng.constr("extension_constructor");
      if (xt->name == "atomic.loc" && !xt->payload.str.empty())
        if (auto* ev = std::get_if<Pstr_eval>(&xt->payload.str[0].desc))
          return eng.constr("Atomic.Loc.t", {infer_expr(*ev->e)});
      if (strict) note_error("Uninterpreted extension '" + xt->name + "'.");
      return eng.fresh_var();
    }
    if (std::holds_alternative<Pexp_unreachable>(e.desc))
      return eng.fresh_var();  // `.` refutation body: types as anything
    if (std::getenv("ANY_A_DBG"))
      fprintf(stderr, "ANY_J expr#%d\n", (int)e.desc.index());
    // The unhandled-form net: nothing on the 1853-file corpus reaches here
    // (the last three forms -- override, extension, unreachable -- are
    // handled above); a future unhandled form takes a fresh var per
    // occurrence like every other incomplete corner.
    return eng.fresh_var();
  }

  // Type an application, matching arguments to parameters by label (OCaml allows
  // labelled args in any order and optional args to be omitted).  When the
  // function's arrow spine is known, do label-aware matching; otherwise fall back
  // to plain positional peeling so unknown/var-typed callees never false-reject.
  // Find the spine parameter index matching an argument label (lk/nm), among the
  // not-yet-`used` params; returns -1 if none.  Labelled/optional args match by
  // name, positional args match the next unlabelled param.
  static int match_param(const std::vector<TypePtr>& spine, const std::vector<bool>& used,
                         int lk, const std::string& nm) {
    for (size_t i = 0; i < spine.size(); ++i) {
      if (used[i]) continue;
      if (lk == 0 ? spine[i]->arrow_label == 0
                  : (spine[i]->arrow_label != 0 && spine[i]->arrow_lbl == nm))
        return (int)i;
    }
    return -1;
  }

  // Slice 3: reconstruct the application's argument list against the callee's
  // inferred parameter labels, for the typed-tree dump.  Stored only when the
  // plan is NON-TRIVIAL (some omitted optional, a Some-wrap, or a reordering) --
  // a plain positional call already dumps correctly from source order.
  void record_apply_plan(const Pexp_apply& a, const Expression& enode, const TypePtr& ft) {
    std::vector<applymatch::Param> params;
    TypePtr cur = I::Engine::repr(ft);
    while (cur->kind == I::Type::Kind::Arrow) {
      params.push_back({cur->arrow_label, cur->arrow_lbl});
      cur = I::Engine::repr(cur->cod);
    }
    if (params.empty()) return;  // not a function type we can place args against
    std::vector<applymatch::Arg> args;
    for (auto& [lbl, arg] : a.args) {
      auto [k, nm] = arglabel(lbl);
      args.push_back({k, nm});
    }
    applymatch::Result m = applymatch::match(params, args);
    if (!m.ok) return;
    bool trivial = m.slots.size() == args.size();
    if (trivial)
      for (size_t i = 0; i < m.slots.size(); ++i) {
        const auto& s = m.slots[i];
        if (s.omitted || s.some_wrap || s.arg_index != (int)i) {
          trivial = false;
          break;
        }
        // A positional arg placed against a labelled parameter must be recorded
        // so the transcriber emits the parameter's label, not the source's.
        if (s.param_label != args[i].label || s.param_name != args[i].name) {
          trivial = false;
          break;
        }
      }
    if (!trivial) apply_plans[&enode] = std::move(m.slots);
  }
  // A %apply/%revapply COLLAPSE (`bump @@ x` -> `bump x`) inserts a ghost None
  // for any optional the FUNCTION operand skips (`bump : ?cap:int -> int -> int`
  // applied to just `x` -> `Optional "cap" None`).  Record the collapsed call's
  // plan keyed by the operator node, so the dump's revapply branch emits it.
  // Only for the GENERIC operator (its function-operand parameter is an arrow),
  // not a monomorphic `external (@@) : f -> x -> int` which is a plain 2-arg
  // application (apply.ml's A.@@).  Dump pass only.
  void record_revapply_plan(const Pexp_apply& a, const Expression& enode,
                            const TypePtr& ft) {
    if (strict) return;
    if (a.args.size() != 2 || !std::holds_alternative<Nolabel>(a.args[0].first) ||
        !std::holds_alternative<Nolabel>(a.args[1].first))
      return;
    auto* fid = std::get_if<Pexp_ident>(&a.fn->desc);
    auto* fl = fid ? std::get_if<Lident>(&fid->id.txt.v) : nullptr;
    int kind = fl ? (fl->name == "@@" ? 2 : fl->name == "|>" ? 1 : 0) : 0;
    if (!kind) return;
    // The operator's function-operand parameter position must be an arrow/var.
    TypePtr op = I::Engine::repr(ft);
    if (op->kind != I::Type::Kind::Arrow) return;
    TypePtr fnpos = I::Engine::repr(kind == 2 ? op->dom
                                              : (I::Engine::repr(op->cod)->kind ==
                                                         I::Type::Kind::Arrow
                                                     ? I::Engine::repr(op->cod)->dom
                                                     : op->cod));
    if (fnpos->kind != I::Type::Kind::Arrow && fnpos->kind != I::Type::Kind::Var)
      return;
    // The function operand's parameter spine vs the single argument operand.
    const Expression& fnop = kind == 2 ? *a.args[0].second : *a.args[1].second;
    TypePtr cur = I::Engine::repr(infer_expr(fnop));
    std::vector<applymatch::Param> params;
    while (cur->kind == I::Type::Kind::Arrow) {
      params.push_back({cur->arrow_label, cur->arrow_lbl});
      cur = I::Engine::repr(cur->cod);
    }
    if (params.empty()) return;
    applymatch::Result m = applymatch::match(params, {{0, ""}});  // one Nolabel arg
    if (!m.ok) return;
    bool trivial = m.slots.size() == 1 && !m.slots[0].omitted &&
                   !m.slots[0].some_wrap && m.slots[0].param_label == 0;
    if (!trivial) apply_plans[&enode] = std::move(m.slots);
  }
  // Rebuild `t` with constr paths under head `from` ("M.") reheaded to `to`
  // ("String."), SHARING unaffected subtrees (the instantiated type may share
  // nodes with the callee's scheme, which must keep its own M.t display).
  // Cyclic back-edges resolve to the original node (pre-registered memo).
  TypePtr subst_path_head(const TypePtr& t0, const std::string& from,
                          const std::string& to,
                          std::unordered_map<I::Type*, TypePtr>& memo) {
    TypePtr t = I::Engine::repr(t0);
    if (auto m = memo.find(t.get()); m != memo.end()) return m->second;
    memo[t.get()] = t;
    switch (t->kind) {
      case I::Type::Kind::Arrow: {
        TypePtr d = subst_path_head(t->dom, from, to, memo);
        TypePtr c = subst_path_head(t->cod, from, to, memo);
        if (d.get() == I::Engine::repr(t->dom).get() &&
            c.get() == I::Engine::repr(t->cod).get())
          return t;
        TypePtr r = eng.arrow(d, c, t->arrow_label, t->arrow_lbl);
        memo[t.get()] = r;
        return r;
      }
      case I::Type::Kind::Tuple:
      case I::Type::Kind::Constr: {
        bool hit = t->kind == I::Type::Kind::Constr && t->path.rfind(from, 0) == 0;
        std::vector<TypePtr> as;
        bool changed = hit;
        for (auto& a : t->args) {
          as.push_back(subst_path_head(a, from, to, memo));
          if (as.back().get() != I::Engine::repr(a).get()) changed = true;
        }
        if (!changed) return t;
        TypePtr r = t->kind == I::Type::Kind::Tuple
                        ? eng.tuple(std::move(as))
                        : eng.constr(hit ? to + t->path.substr(from.size()) : t->path,
                                     std::move(as), t->stamp);
        memo[t.get()] = r;
        return r;
      }
      default:
        return t;
    }
  }

  TypePtr infer_apply(const Pexp_apply& a, const Expression& enode) {
    TypePtr ft = infer_expr(*a.fn);
    record_apply_plan(a, enode, ft);
    record_revapply_plan(a, enode, ft);
    // Applying a value of a reliable non-function type (`1 2`, `"x" y`) is a
    // definite error -- a builtin like int/string is never an arrow.
    if (strict && !a.args.empty()) {
      TypePtr fr = I::Engine::repr(ft);
      if (fr->kind == I::Type::Kind::Constr && reliable_builtin(fr->path))
        note_error("This expression is not a function; it cannot be applied");
    }
    // Pure soundness check: a qualified callee M.x is otherwise typed as Any, so
    // its argument types go unchecked.  Resolve its real type and check each
    // positional argument against the matching parameter via expected_clash
    // (pure, reliable-only) -- catches e.g. `String.length 5`.
    if (strict)
      if (auto* id = std::get_if<Pexp_ident>(&a.fn->desc))
        if (auto* d = std::get_if<Ldot>(&id->id.txt.v)) {
          auto& ex = module_values_cached(*d->prefix);
          auto f = ex.find(d->name);
          if (f != ex.end()) {
            TypePtr rt = I::Engine::repr(eng.instantiate(f->second));
            for (auto& [lbl, arg] : a.args) {
              if (rt->kind != I::Type::Kind::Arrow) break;
              if (std::holds_alternative<Nolabel>(lbl) && rt->arrow_label == 0 &&
                  expected_clash(infer_expr(*arg), rt->dom))
                note_error("argument type mismatch for " + lid_full(id->id.txt));
              rt = I::Engine::repr(rt->cod);
            }
          }
        }
    // Only argument-check a callee whose type we trust: a pervasive/stdlib value
    // (a bare operator like `+` resolved from stdlib, not shadowed locally).  A
    // local function's inferred type may be wrong (GADTs, abstract types), so
    // flagging its arguments would false-reject.
    bool reliable_callee = false;
    if (auto* id = std::get_if<Pexp_ident>(&a.fn->desc))
      if (auto* l = std::get_if<Lident>(&id->id.txt.v)) {
        bool local = false;
        for (auto& sc : venv) if (sc.count(l->name)) { local = true; break; }
        reliable_callee = !local && stdlib_schemes().count(l->name);
      }
    // Collect the function's known arrow spine.
    std::vector<TypePtr> spine;
    TypePtr cur = I::Engine::repr(ft);
    while (cur->kind == I::Type::Kind::Arrow) {
      spine.push_back(cur);
      cur = I::Engine::repr(cur->cod);
    }
    TypePtr tail = cur;

    // Dry run: can every argument be matched to a parameter by label?  (This is
    // the OCaml commutation rule: labelled args in any order, positional args to
    // the next unlabelled param, optionals skippable.)
    std::vector<bool> dry(spine.size(), false);
    bool can = !spine.empty();
    for (auto& [lbl, arg] : a.args) {
      auto [lk, nm] = arglabel(lbl);
      int idx = match_param(spine, dry, lk, nm);
      if (idx < 0) { can = false; break; }
      dry[idx] = true;
    }

    if (can) {  // label-aware application
      std::vector<bool> used(spine.size(), false);
      int maxc = -1;          // max consumed POSITIONAL index: only a positional
                              // argument past an optional forces it defaulted.  A
                              // later LABELLED arg commutes past an optional WITHOUT
                              // erasing it (`f ~check:false ~rebind:false` keeps the
                              // intervening `?shape` in the result type).
      // A named package param `(module M : T)` applied to `(module String)`
      // substitutes M's paths in the RESULT: `g (module String) "x"` gives
      // `String.t * int`, per ocamlc's dependent-application rule.
      std::vector<std::pair<std::string, std::string>> pkg_substs;
      for (auto& [lbl, arg] : a.args) {
        auto [lk, nm] = arglabel(lbl);
        int idx = match_param(spine, used, lk, nm);
        used[idx] = true;
        if (lk == 0 && idx > maxc) maxc = idx;
        if (!strict) {
          TypePtr dr = I::Engine::repr(spine[idx]->dom);
          if (dr->kind == I::Type::Kind::Constr && !dr->abbrev.empty() &&
              dr->path.rfind("(module ", 0) == 0)
            if (auto* pk2 = std::get_if<Pexp_pack>(&arg->desc)) {
              const ModuleExpr* m = pk2->me.get();
              while (auto* mc = std::get_if<Pmod_constraint>(&m->desc)) m = mc->me.get();
              if (auto* mi = std::get_if<Pmod_ident>(&m->desc))
                pkg_substs.emplace_back(dr->abbrev + ".", lid_full(mi->id.txt) + ".");
            }
        }
        TypePtr at = infer_expr_expected(*arg, spine[idx]->dom);
        // A reliable-callee argument whose inferred type structurally clashes with
        // the expected parameter on a reliable builtin (int vs float, ...) is a
        // definite error.  Restrict to arguments whose inferred type is TRUSTWORTHY:
        // a literal constant, or an application whose result comes from a function's
        // codomain (`(1 + 2) +. 3.`).  A bare variable is excluded -- it may be
        // bound by a GADT/existential pattern and cross-unified to a wrong concrete
        // builtin across match arms (`Int -> print_int body`), which would then
        // false-reject.  builtin_clash itself never fires on vars/stamps/Any.
        if (strict && reliable_callee &&
            (std::holds_alternative<Pexp_constant>(arg->desc) ||
             std::holds_alternative<Pexp_apply>(arg->desc) ||
             std::holds_alternative<Pexp_send>(arg->desc) ||
             std::holds_alternative<Pexp_variant>(arg->desc)) &&
            builtin_clash(at, spine[idx]->dom))
          note_error("This expression has a type that clashes with the expected type");
        soft_unify(spine[idx]->dom, at);  // propagate; genuine errors via expected_clash
      }
      // result = unconsumed params chained onto the tail, erasing any leading
      // optional that precedes a consumed positional (it is defaulted).
      TypePtr res = tail;
      for (int i = (int)spine.size() - 1; i >= 0; --i) {
        if (used[i]) continue;
        if (spine[i]->arrow_label == 2 && i < maxc) continue;  // erased optional
        res = eng.arrow(spine[i]->dom, res, spine[i]->arrow_label, spine[i]->arrow_lbl);
      }
      for (auto& [from, to] : pkg_substs) {
        std::unordered_map<I::Type*, TypePtr> memo;
        res = subst_path_head(res, from, to, memo);
      }
      return res;
    }

    // Fallback: spine unknown/insufficient -- peel positionally (bidirectional
    // on each argument), so a var-typed callee never false-rejects.
    for (auto& [lbl, arg] : a.args) {
      auto [lk, nm] = arglabel(lbl);
      // A POSITIONAL arg skips past leading OPTIONAL params (they are defaulted),
      // aligning with the next non-optional param.  Without this,
      // `Location.errorf "%d" 3` aligns "%d" with `?loc` instead of the `format6`
      // param (the `3` overflows the known spine, so the label-aware path bails
      // to here), so the string literal isn't recognised as a format and is
      // lowered as a plain string -> make_printf crash at runtime.
      if (lk == 0) {
        TypePtr c = I::Engine::repr(ft);
        while (c->kind == I::Type::Kind::Arrow && c->arrow_label == 2) {
          ft = I::Engine::repr(c->cod); c = ft;
        }
      }
      // Carry the argument's label onto the built arrow: for a var-typed callee
      // (unknown spine, e.g. a `let rec` self-call `g ~first:false` or a labelled
      // higher-order param `f ~a ~b`), this is the only place the arrow's label
      // is set, so dropping it here loses `~first:`/`~a:` from the inferred type.
      // soft_unify of two arrows only recurses dom/cod (labels aren't checked),
      // so tagging a label here can never false-reject against a known callee.
      TypePtr dom = eng.fresh_var(), r = eng.fresh_var();
      soft_unify(ft, eng.arrow(dom, r, lk, nm));
      TypePtr at = infer_expr_expected(*arg, dom);
      soft_unify(dom, at);
      ft = I::Engine::repr(r);
    }
    return ft;
  }

  // Type an object/class body for value kinds: instance vars from their initialiser,
  // method/initializer bodies (so params/results get kinds and format literals are
  // recorded).  Value-kind pass only.
  // Infer an object/class body; returns the object type `< m : t; .. >` (concrete
  // methods only -- inherited/virtual methods would need the full class model).
  TypePtr infer_object_body(const ast::ClassStructure& cs,
                         const std::vector<const ast::Pcl_fun*>* cl_params = nullptr,
                         const std::vector<const ast::Pcl_let*>* cl_lets = nullptr,
                         std::unordered_map<std::string, TypePtr>* cvars = nullptr,
                         std::vector<TypePtr>* param_tys = nullptr,
                         std::vector<std::pair<std::string, TypePtr>>* out_instvars = nullptr) {
    std::vector<std::string> mnames;
    std::vector<TypePtr> mtypes;
    venv.emplace_back();
    // Class parameters: bind each so a val initialiser referencing one shares its
    // type var with the instance variable (method-body unification then flows back).
    // An optional parameter's type is its default's type.
    if (cl_params)
      for (auto* pf : *cl_params) {
        TypePtr pt = infer_pat(pf->pat);
        if (pf->default_) try_unify(pt, infer_expr(**pf->default_));
        if (param_tys) param_tys->push_back(pt);
      }
    // `constraint 'a = [> 'a lambda]` fields pin the class's type params
    // (translated under the CLASS vars map, so 'a is the declared param).
    if (cvars)
      for (auto& f : cs.fields)
        if (auto* ct = std::get_if<Pcf_constraint>(&f.desc))
          soft_unify(from_coretype(*ct->t1, *cvars), from_coretype(*ct->t2, *cvars));
    // `class c = let .. in object`: the local bindings, before the fields.
    if (cl_lets) for (auto* lg : *cl_lets) infer_bindings(lg->rf, lg->bindings);
    // Pre-create a type variable per concrete method and bind `self` to the
    // object type built from them, so a method body's `self#other` resolves to
    // the (possibly forward-declared) sibling method's var, which a later
    // unification ties to that method's body type.  Without this, `self` was
    // Any and every self-method-call returned Any.
    std::unordered_map<std::string, TypePtr> mvar;
    // A self-type coercion `object (self : (T1,T2) #ops) .. end`: the
    // annotation's object row (from the class type's signature) pins every
    // method's type; self binds to it so self#m resolves through it (mixin3).
    TypePtr self_annot = nullptr;
    {
      std::vector<std::string> sn;
      std::vector<TypePtr> st;
      for (auto& f : cs.fields)
        if (auto* m = std::get_if<Pcf_method>(&f.desc))
          if (std::get_if<Cfk_concrete>(&m->kind)) {
            TypePtr v = eng.fresh_var();
            mvar[m->name.txt] = v;
            sn.push_back(m->name.txt);
            st.push_back(v);
          }
      TypePtr selfTy = eng.object_type(std::move(sn), std::move(st));
      const Pattern* sp = &cs.self;
      const CoreType* sct = nullptr;
      while (auto* pc = std::get_if<Ppat_constraint>(&sp->desc)) {
        sct = pc->t.get();
        sp = pc->p.get();
      }
      if (sct) {
        std::unordered_map<std::string, TypePtr> avars;
        TypePtr at = I::Engine::repr(
            from_coretype(*sct, cvars ? *cvars : avars));
        if (at->kind == I::Type::Kind::Object) {
          self_annot = at;
          soft_unify(selfTy, at);  // ties each method var to its declared type
          selfTy = at;
        }
      }
      if (auto* sv = std::get_if<Ppat_var>(&sp->desc)) venv.back()[sv->name.txt] = selfTy;
    }
    // `inherit P args`: bring P's instance vars into scope so the subclass'
    // methods/initializers resolve them (charlie's `y` from bravo).  Done before
    // this class' own vals, which may shadow.
    for (auto& f : cs.fields)
      if (auto* inh = std::get_if<Pcf_inherit>(&f.desc)) {
        const ClassExpr* pce = inh->ce.get();
        while (auto* ap = std::get_if<Pcl_apply>(&pce->desc)) pce = ap->ce.get();
        if (auto* pc = std::get_if<Pcl_constr>(&pce->desc))
          if (auto it = class_instvars_.find(lid_last(pc->id.txt)); it != class_instvars_.end())
            for (auto& [nm, ty] : it->second) {
              venv.back()[nm] = ty;
              if (out_instvars) out_instvars->emplace_back(nm, ty);
            }
      }
    for (auto& f : cs.fields)
      if (auto* v = std::get_if<Pcf_val>(&f.desc))
        if (auto* cc = std::get_if<Cfk_concrete>(&v->kind)) {
          TypePtr vt = infer_expr(*cc->e);
          venv.back()[v->name.txt] = vt;
          if (out_instvars) out_instvars->emplace_back(v->name.txt, vt);
        }
    for (auto& f : cs.fields) {
      if (auto* m = std::get_if<Pcf_method>(&f.desc)) {
        if (auto* cc = std::get_if<Cfk_concrete>(&m->kind)) {
          const Expression* body = cc->e.get();
          const CoreType* pty = nullptr;  // `method m : T = ...` poly annotation
          if (auto* poly = std::get_if<Pexp_poly>(&body->desc)) {
            if (poly->t) pty = poly->t->get();
            body = poly->e.get();
          }
          TypePtr bt = infer_expr(*body);
          if (pty) {  // an annotated method type pins the signature
            std::unordered_map<std::string, TypePtr> vars;
            TypePtr at = from_coretype(*pty, vars);
            if (strict) soft_unify(bt, at); else { try { try_unify(bt, at); } catch (...) {} bt = at; }
          }
          // Tie the pre-declared method var to the inferred body type, so any
          // `self#m` use elsewhere sees the real type.
          if (auto it = mvar.find(m->name.txt); it != mvar.end()) {
            try_unify(it->second, bt);
            bt = it->second;
          }
          if (m->priv == PrivateFlag::Private) continue;  // not in the public type
          mnames.push_back(m->name.txt);
          mtypes.push_back(bt);
        }
      } else if (auto* ini = std::get_if<Pcf_initializer>(&f.desc)) {
        infer_expr(*ini->e);
      } else if (auto* inh = std::get_if<Pcf_inherit>(&f.desc)) {
        if (auto* ap = std::get_if<Pcl_apply>(&inh->ce->desc))
          for (auto& [l, e] : ap->args) infer_expr(*e);
      }
    }
    venv.pop_back();
    // A self-coerced object's PUBLIC type is the annotation's row, CLOSED
    // (an object value has exactly its methods): `(T1,T2) ops`, not `#ops`
    // -- private methods (mixin3's `method private map`) are not public
    // either way, since the annotation's row lists only the class type's.
    if (self_annot) {
      TypePtr closed = eng.object_type(self_annot->labels, self_annot->args);
      closed->abbrev = self_annot->abbrev;
      closed->abbrev_args = self_annot->abbrev_args;
      return closed;
    }
    return eng.object_type(std::move(mnames), std::move(mtypes));
  }

  TypePtr infer_function(const Pexp_function& f) {
    venv.emplace_back();
    cenv.emplace_back();  // scope for module-param ctor bindings (see below)
    // Bind all (type a) params to flexible vars first, so value-param
    // annotations mentioning them resolve regardless of order.  Save any
    // shadowed outer binding of the same name and restore it on exit -- a nested
    // function's `(type a)` must NOT clobber an enclosing `(type a)` (else the
    // enclosing function's return annotation resolves `a` to the wrong node).
    std::vector<std::pair<std::string, std::optional<TypePtr>>> saved_newtypes;
    for (auto& fp : f.params)
      if (auto* nt = std::get_if<Pparam_newtype>(&fp.desc)) {
        auto it = newtype_vars.find(nt->name.txt);
        saved_newtypes.push_back({nt->name.txt,
            it != newtype_vars.end() ? std::optional<TypePtr>(it->second) : std::nullopt});
        newtype_vars[nt->name.txt] = newtype_binding();
      }
    struct Param { TypePtr ty; int lk; std::string nm; };
    std::vector<Param> params;
    std::vector<std::pair<const Pattern*, TypePtr>> ppat_types;  // param partiality
    // First-class-module params `(module M : S)` bind M's values (at
    // M-qualified abstract types) for the body -- `P.print x` ties x : P.t.
    // Saved/restored around the body so M doesn't leak past the function.
    std::vector<std::pair<std::string,
                          std::optional<std::unordered_map<std::string, TypePtr>>>>
        saved_mods;
    for (auto& fp : f.params) {
      // (type a) introduces a locally-abstract type, not a value argument, so it
      // contributes no arrow to the function's type.
      if (auto* pv = std::get_if<Pparam_val>(&fp.desc)) {
        auto [lk, nm] = arglabel(pv->label);
        TypePtr pt = infer_pat(pv->pat);
        // an optional parameter's type is its default's type: `?(c = 100)` => int
        if (pv->default_) try_unify(pt, infer_expr(**pv->default_));
        params.push_back({pt, lk, nm});
        ppat_types.emplace_back(&pv->pat, pt);  // for param-pattern partiality
        if (!strict) {
          const Ppat_unpack* up = std::get_if<Ppat_unpack>(&pv->pat.desc);
          const Ptyp_package* upkg = up && up->pkg ? &*up->pkg : nullptr;
          // `?opt:((module M) = (module M1 : S))`: the bare unpack's package
          // type comes from the default's pack annotation.
          if (up && !upkg && pv->default_)
            if (auto* dp = std::get_if<Pexp_pack>(&(*pv->default_)->desc))
              if (dp->pkg) upkg = &*dp->pkg;
          if (up && up->name.txt && upkg)
            if (auto* pl = std::get_if<Lident>(&upkg->path.txt.v))
              if (auto sg = modtype_sig_asts_.find(pl->name);
                  sg != modtype_sig_asts_.end()) {
                auto prior = modenv.find(*up->name.txt);
                saved_mods.emplace_back(*up->name.txt,
                    prior != modenv.end() ? std::optional(prior->second)
                                          : std::nullopt);
                // `with type t = a` constraints substitute into the bound
                // values (M's t IS a; qualifying it M.t would capture a).
                std::unordered_map<std::string, TypePtr> argtypes;
                for (auto& [lid, ctb] : upkg->constraints) {
                  std::unordered_map<std::string, TypePtr> vars;
                  argtypes[lid_full(lid.txt)] = from_coretype(*ctb, vars);
                }
                modenv[*up->name.txt] =
                    unpack_module_values(*up->name.txt, *sg->second, &argtypes);
                // The sig's typext ctors (`type t += E`) resolve as `M.E` in
                // the body (binding1's `?(opt = M.E)`) -- function-scoped cenv.
                bind_sig_typext_ctors(*sg->second);
              }
        }
      }
    }
    TypePtr body;
    TypePtr constrained = nullptr;  // what f.constraint_ annotates, when not `body`
    if (auto* fb = std::get_if<Pfunction_body>(&f.body->v)) {
      body = infer_expr(*fb->e);
    } else {
      auto& fc = std::get<Pfunction_cases>(f.body->v);
      TypePtr arg = eng.fresh_var(), rt = eng.fresh_var();
      // A GADT `function` refines branch-locally exactly like a GADT match
      // (`let default : type a. a t -> a = function Float -> exp 0. | Int32 ->
      // ... constant` returns float in one arm, int32 in the other): window +
      // soft unify + rollback in the strict pass, the historical full unify in
      // the kind pass (value kinds need the bindings kept).
      bool gadt = false;
      for (auto& c : fc.cases) if (pat_has_gadt_ctor(c.lhs)) gadt = true;
      // Display pass: no window -- rigid newtypes make refinements leak-proof
      // and full unification keeps arm-body pins (see the Pexp_match twin).
      bool display = fold_abbrevs_ && !strict;
      bool window = gadt && !record_kinds_ && !display;
      if (display) gadt = false;
      // Ground recovery (mirrors the Pexp_match window): when every arm,
      // after rollback, yields the SAME ground pattern/result type, that is
      // the genuine scrutinee/result (expand_test's arms all match `test`
      // ctors and all build `Test (..)` : simplified_test).  Arms that
      // disagree on a ground type or any non-ground arm leave it open, so a
      // `: a` annotation still pins it.
      bool pat_all_ground = window, pat_clash = false;
      bool res_all_ground = window, res_clash = false;
      TypePtr pacc = nullptr, racc = nullptr;
      mark_open_row_pats(fc.cases);
      // Display pass: flow the return annotation DOWN before the cases, like
      // ocamlc's expected-type propagation.  A `: _ lambda -> _` row
      // annotation then meets the patterns as the scrutinee, so a pattern var
      // binds the DECL expansion's (finalized) nodes -- a body contact
      // (`Names.remove s`) can no longer rename a decl arg in place (mixin's
      // free_lambda keeps `Abs of string).  The post-loop constraint
      // processing still runs (idempotent for what's already unified).
      if (fold_abbrevs_ && !strict && f.constraint_)
        if (auto* pc0 = std::get_if<Pconstraint>(&*f.constraint_)) {
          std::unordered_map<std::string, TypePtr> local0;
          bool saved_ad = adoptable_annot_;
          adoptable_annot_ = true;
          TypePtr at0 =
              from_coretype(*pc0->type, annot_vars_ ? *annot_vars_ : local0);
          adoptable_annot_ = saved_ad;
          soft_unify(eng.arrow(arg, rt), at0);
        }
      for (auto& c : fc.cases) {
        venv.emplace_back();
        size_t wm = window ? eng.mark() : 0;
        if (window) {
          TypePtr pt = infer_pat(c.lhs);
          soft_unify(pt, arg);
          if (c.guard) infer_expr(**c.guard);
          TypePtr br = infer_expr(*c.rhs);
          soft_unify(br, rt);
          eng.undo_to(wm);
          TypePtr pr = I::Engine::repr(pt);
          if (type_is_ground(pr)) {
            if (!pacc) pacc = pr;
            else if (!ground_types_equal(pacc, pr)) pat_clash = true;
          } else pat_all_ground = false;
          TypePtr brr = I::Engine::repr(br);
          if (type_is_ground(brr)) {
            if (!racc) racc = brr;
            else if (!ground_types_equal(racc, brr)) res_clash = true;
          } else res_all_ground = false;
        } else {
          try_unify(infer_pat(c.lhs), arg);
          if (c.guard) infer_expr(**c.guard);  // flows operand kinds; not bool-constrained
          try_unify(infer_expr(*c.rhs), rt);
        }
        venv.pop_back();
      }
      if (window && pat_all_ground && !pat_clash && pacc) soft_unify(arg, pacc);
      if (window && res_all_ground && !res_clash && racc) soft_unify(rt, racc);
      // Exhaustiveness of the cases against the parameter type, for the dump's
      // Tfunction_cases (Partial) marker (same conservative check as a match).
      // GADT scrutinees are exempt: refinement (a ctor at an incompatible type
      // index can't match) makes them total in ways compute_partial can't see,
      // so claiming Partial would be wrong (switch_opts' `int gadt` omits the
      // `string gadt` ctors yet is total).
      TypePtr sarg = I::Engine::repr(arg);
      bool arg_gadt =
          sarg->kind == I::Type::Kind::Constr && gadt_types.count(sarg->path);
      // A GADT scrutinee is Total by branch refinement UNLESS an omitted ctor's
      // index can't be proven distinct from the scrutinee's (pr7284_bad); a
      // non-GADT uses the ordinary coverage check.
      function_cases_partial[&fc] =
          arg_gadt ? gadt_function_partial(sarg, fc.cases)
                   : compute_partial(arg, fc.cases);
      params.push_back({arg, 0, ""});
      body = rt;
      constrained = eng.arrow(arg, rt);  // the constraint annotates arg -> rt
    }
    // A return-type annotation (`fun .. : t -> e`) pins the body's type to t --
    // resolving fresh/Any results in the signature.  Skipped in the strict pass
    // (our incomplete inference could make a valid body clash with t and
    // false-reject); soft elsewhere so a stray clash can't abort the pass.
    // For a `function`-cases body the annotation covers the WHOLE `arg -> rt`
    // arrow (`let f ~x : (t1 -> t2) = function ..`) -- unifying it against rt
    // alone silently dropped it (arrow vs result: lenient no-op), losing e.g.
    // mixin's `: _ lambda -> _` row annotations.
    if (f.constraint_ && !strict)
      if (auto* pc = std::get_if<Pconstraint>(&*f.constraint_)) {
        std::unordered_map<std::string, TypePtr> local;
        TypePtr at = from_coretype(*pc->type, annot_vars_ ? *annot_vars_ : local);
        soft_unify(constrained ? constrained : body, at);
        // A PACKAGE-typed return annotation is the result (ocamlc's ascription
        // display): a lenient path mismatch (`(module X.S)` body vs
        // `(module Y.S)` annotation, distinct spellings of one modtype) must
        // not let the body's own spelling win (fstclassmod's _f).
        if (!constrained) {
          TypePtr ar = I::Engine::repr(at);
          if (ar->kind == I::Type::Kind::Constr && ar->path.rfind("(module ", 0) == 0)
            body = at;
          // In the DISPLAY pass the return annotation IS the displayed result
          // (the soft_unify above flowed the body's pins into its flexible
          // vars), matching the `(e : T)` and `let x : T = ..` display rules.
          // A body typed at `((int,int) continuation -> int) option` under a
          // rigid `a = int` arm keeps the annotation's `(a, int)` face
          // (frame-pointers' effc handler).  Skipped when the translation has
          // an untranslated corner (Any) -- the inferred body knows more.
          else if (fold_abbrevs_) body = at;
        }
      }
    if (record_kinds_) rec_ret_[&f] = body;
    // Param-pattern exhaustiveness (for Param_pat (Partial)), now that the body
    // has constrained each param type: a `` `Var s `` param over a closed row is
    // total, an unannotated variant/tuple/record is total, `Some x` over option
    // is partial.  Computed against the resolved type -- more precise than the
    // syntactic pat_irrefutable the transcriber falls back to.
    for (auto& [pp, pt] : ppat_types)
      param_partial[pp] = param_pattern_partial(pt, *pp);
    TypePtr t = body;
    for (auto it = params.rbegin(); it != params.rend(); ++it)
      t = eng.arrow(it->ty, t, it->lk, it->nm);
    cenv.pop_back();
    venv.pop_back();
    for (auto& [nm, prior] : saved_newtypes) {
      if (prior) newtype_vars[nm] = *prior;
      else newtype_vars.erase(nm);
    }
    for (auto& [nm, prior] : saved_mods) {
      if (prior) modenv[nm] = std::move(*prior);
      else modenv.erase(nm);
    }
    return t;
  }

  // ocamlc re-expands a folded abbreviation from its DECL at each use, so a
  // name adopted during one binding's body (free_lambda's `Names.remove s`
  // renaming the lambda expansion's `string` to `Names.elt`) never leaks into
  // later uses -- but a live row CAN adopt within its own binding (subst_var's
  // `[> `Var of Subst.key ]`).  Our expansion rows are shared into the scheme,
  // so at top-level finalization (display pass) each INTACT abbreviation row
  // restores its DECL-GROUND tag args from the declaration: zip the manifest's
  // Rtag types against the row's args, re-pointing a position the decl spells
  // as a ground constr back to a decl-named node.  Var/param positions keep
  // the live nodes (the instance's ties).
  void restore_abbrev_rows(const TypePtr& t0) {
    std::unordered_set<I::Type*> seen;
    std::function<TypePtr(const CoreType&, const TypePtr&)> zip =
        [&](const CoreType& ct, const TypePtr& cur) -> TypePtr {
      TypePtr c = I::Engine::repr(cur);
      if (auto* cc = std::get_if<Ptyp_constr>(&ct.desc)) {
        // Only PRIM-ish decl leaves (string, int, M.t) re-expand; a decl
        // position that is itself a local ALIAS would recurse through its
        // manifest (morematch's recursive `type_expr` overflowed the stack).
        if (type_aliases.count(lid_last(cc->id.txt))) return cur;
        std::unordered_map<std::string, TypePtr> vars;
        // The re-expanded node is a fresh EXPANSION node: adoptable, so the
        // new binding's own contacts can rename it (subst1's `Var of key).
        bool sm = manifest_expansion_, sa = adoptable_annot_;
        manifest_expansion_ = adoptable_annot_ = true;
        TypePtr fresh = from_coretype(ct, vars);
        manifest_expansion_ = sm;
        adoptable_annot_ = sa;
        if (!vars.empty()) return cur;  // decl mentions params: keep live node
        TypePtr fr = I::Engine::repr(fresh);
        if (fr->kind != I::Type::Kind::Constr) return cur;  // decl side expands
        // Always the FRESH node -- even when the name still matches: the
        // point is breaking the scheme's arg-row/result-row SHARING (ocamlc's
        // instance re-expands the folded arg, so a use-site rename of the arg
        // field can't reach the result row -- subst_lambda keeps map_lambda's
        // `Abs of string while the scrutinee's own field adopts Subst.key).
        // The replacement inherits the old node's finalization ONLY for a
        // source-written face (a PARAM annotation's expansion: GENERIC
        // without scheme_head) -- `(v : var)` keeps `Var of string.  A
        // SCHEME-finalized node (scheme_head) is exactly what this
        // re-expansion replaces: the fresh node stays adoptable, so the new
        // binding's contacts can rename it (subst1's `Var of Subst.key).
        if (c->kind == I::Type::Kind::Constr && c->level == I::GENERIC_LEVEL &&
            !c->scheme_head && fr->kind == I::Type::Kind::Constr)
          fr->level = I::GENERIC_LEVEL;
        return fresh;
      }
      if (auto* tu = std::get_if<Ptyp_tuple>(&ct.desc)) {
        if (c->kind == I::Type::Kind::Tuple && c->args.size() == tu->elems.size()) {
          std::vector<TypePtr> es;
          bool changed = false;
          for (size_t i = 0; i < tu->elems.size(); ++i) {
            es.push_back(zip(*tu->elems[i], c->args[i]));
            if (I::Engine::repr(es.back()) != I::Engine::repr(c->args[i]))
              changed = true;
          }
          if (changed) return eng.tuple(std::move(es));
        }
      }
      return cur;
    };
    // Iterative worklist + node budget: this runs per lookup_value
    // instantiation, and a morematch-sized shared graph would be re-walked
    // (and its ground fields re-freshened) at every use -- quadratic.  Small
    // types (where the re-expansion display matters) fit the budget; giant
    // graphs bail unchanged.
    std::vector<TypePtr> work{t0};
    int budget = 512;
    while (!work.empty()) {
      if (--budget < 0) return;
      TypePtr t = I::Engine::repr(work.back());
      work.pop_back();
      if (!seen.insert(t.get()).second) continue;
      if (t->kind == I::Type::Kind::Arrow) {
        work.push_back(t->dom);
        work.push_back(t->cod);
        continue;
      }
      bool self_fix = false;  // fixpoint (`'a lambda as 'a`): a LIVE row that
      for (auto& aa : t->abbrev_args) {  // displays unfolded -- ocamlc keeps
        TypePtr ar = I::Engine::repr(aa);  // its nodes.  An instantiated
        if (ar.get() == t.get() ||         // fixpoint's back-edge may point at
            (ar->kind == I::Type::Kind::Variant &&  // the SCHEME's row (plain
             ar->abbrev == t->abbrev))     // copy shares back-edges) -- same
          self_fix = true;                 // abbreviation counts.
      }
      if (t->kind == I::Type::Kind::Variant && !t->abbrev.empty() && !self_fix) {
        auto ai = type_aliases.find(t->abbrev);
        if (ai != type_aliases.end() && ai->second.manifest &&
            ai->second.params.size() == t->abbrev_args.size()) {
          if (auto* pv = std::get_if<Ptyp_variant>(&ai->second.manifest->desc)) {
            for (auto& r : pv->rows) {
              auto* rt = std::get_if<Rtag>(&r);
              if (!rt || rt->types.empty()) continue;
              for (size_t i = 0; i < t->labels.size(); ++i)
                if (t->labels[i] == rt->name && i < t->args.size())
                  t->args[i] = zip(*rt->types[0], t->args[i]);
            }
          }
        }
      }
      for (auto& a : t->args) work.push_back(a);
      for (auto& a : t->abbrev_args) work.push_back(a);
      for (auto& a : t->inherited) work.push_back(a);
    }
  }

  // Bind a let group (generalizing each RHS at the outer level).  `toplevel`
  // marks a STRUCTURE-ITEM binding: its family-participant constr heads are
  // finalized (stamped GENERIC) so later items' uses can't relink the
  // displayed path -- while an inner let/rec-group node stays live and adopts
  // abbreviations on contact (ephetest3's `let y = hashcons .. in fill_hw y`).
  // The name of an existential-introducing constructor destructured by this
  // pattern (empty if none).  Only a Ppat_construct that *matches* such a ctor
  // extracts the existential; a plain `let z = A ()` (Ppat_var) does not.
  std::string pat_existential_ctor(const Pattern& p) const {
    if (auto* c = std::get_if<Ppat_construct>(&p.desc)) {
      std::string n = lid_last(c->id.txt);
      if (existential_ctors_.count(n) && !ambiguous_ctors_.count(n)) return n;
      return c->arg ? pat_existential_ctor(**c->arg) : std::string();
    }
    if (auto* t = std::get_if<Ppat_tuple>(&p.desc)) {
      for (auto& e : t->elems) { auto n = pat_existential_ctor(*e); if (!n.empty()) return n; }
    } else if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
      for (auto& f : r->fields) { auto n = pat_existential_ctor(*f.second); if (!n.empty()) return n; }
    } else if (auto* a = std::get_if<Ppat_array>(&p.desc)) {
      for (auto& e : a->elems) { auto n = pat_existential_ctor(*e); if (!n.empty()) return n; }
    } else if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      auto n = pat_existential_ctor(*o->l); return n.empty() ? pat_existential_ctor(*o->r) : n;
    } else if (auto* al = std::get_if<Ppat_alias>(&p.desc)) {
      return pat_existential_ctor(*al->p);
    } else if (auto* cn = std::get_if<Ppat_constraint>(&p.desc)) {
      return pat_existential_ctor(*cn->p);
    } else if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) {
      return pat_existential_ctor(*lz->p);
    } else if (auto* op = std::get_if<Ppat_open>(&p.desc)) {
      return pat_existential_ctor(*op->p);
    }
    return std::string();
  }

  void infer_bindings(RecFlag rf, const std::vector<ValueBinding>& bs,
                      bool toplevel = false) {
    // A structure-level (module-toplevel) binding may not destructure an
    // existential-introducing constructor: the extracted variable would carry an
    // existential type that escapes its scope.  (Inside a `let .. in`, function
    // parameter, or match arm the existential stays scoped, so those are fine.)
    if (strict && toplevel)
      for (auto& b : bs) {
        std::string n = pat_existential_ctor(b.pat);
        if (!n.empty()) {
          note_error("Existential types are not allowed in toplevel bindings, "
                     "but the constructor " + n + " introduces existential types.");
          break;
        }
      }
    // Illegal operator-shaped value names (`~##`, `#~#`) -- rejected at binding.
    if (strict)
      for (auto& b : bs) {
        std::set<std::string> names;
        pat_names(b.pat, names);
        for (auto& n : names)
          if (is_illegal_value_name(n))
            note_error(n + " is not a valid value identifier.");
      }
    if (rf == RecFlag::Recursive) {
      // Pre-bind each name; a `let rec f : type a. T = ...` annotation makes f
      // polymorphic-recursive -- bind it to the (generic) annotation so recursive
      // calls instantiate fresh, rather than forcing one monomorphic type (which
      // for a GADT recursion yields a spurious occurs-check).  Plain bindings get
      // a monomorphic var unified with the inferred body.
      check_letrec(bs);  // the value-recursion restriction
      // Generalize the rec group like the non-recursive path: infer the bodies at
      // a RAISED level, then generalize each bound name so a LATER use in the same
      // module instantiates a fresh copy (`let rec map f = .. ;; map succ [1]` must
      // keep `map : ('a -> 'b) -> 'a list -> 'b list`, not monomorphize to int).
      // Recursion itself stays monomorphic (the pre-bound var is shared,
      // non-generic, during body inference) -- standard ML let-rec.
      eng.enter_level();
      std::vector<TypePtr> tv(bs.size(), nullptr);      // plain: the recursion var
      std::vector<TypePtr> bound(bs.size(), nullptr);   // scheme to generalize
      std::vector<char> plain_annot(bs.size(), 0);      // `let rec x : T = e`, no univars
      // Named type vars are shared across each binding's annotations exactly
      // like the non-recursive path (`let rec run (c : 'c event) : 'c = ..`
      // ties the param and return to ONE 'c) -- essential when the body is a
      // windowed GADT match whose structural unifications roll back, leaving
      // the annotation tie as the only source of the result type.
      std::vector<std::unordered_map<std::string, TypePtr>> avmaps(bs.size());
      auto* saved_av = annot_vars_;
      for (size_t i = 0; i < bs.size(); ++i) {
        const ValueBinding& b = bs[i];
        annot_vars_ = &avmaps[i];
        const Pvc_constraint* pc =
            b.constraint_ ? std::get_if<Pvc_constraint>(&*b.constraint_) : nullptr;
        if (pc && !pc->univars.empty()) {
          for (auto& u : pc->univars) newtype_vars[u.txt] = newtype_binding();
          bound[i] = from_coretype(*pc->typ, avmaps[i]);
          bind_pattern_scheme(b.pat, bound[i]);
        } else if (pc && !strict) {
          // A plain declared type `let rec x : T = e` pins x to T -- bind the
          // name to the annotation rather than the (possibly Any) body, so a
          // body that infers Any (`(module struct end)`) or an under-determined
          // value (`[||]`) doesn't erase the declared type.  tv stays null: the
          // body is still inferred below (effects/kinds); it soft-unifies INTO
          // the annotation (below), pinning flexible holes (`lexpr -> _`)
          // while the annotation stays the displayed face.
          bound[i] = from_coretype(*pc->typ, avmaps[i]);
          plain_annot[i] = true;
          bind_pattern_scheme(b.pat, bound[i]);
        } else {
          tv[i] = infer_pat(b.pat);
          bound[i] = tv[i];
          // Give the recursion var the RHS function's syntactic parameter labels
          // up front, so a self-call omitting an optional param is desugared
          // (ghost None) -- the body inference otherwise only unifies the arrow
          // AFTER the self-call is typed, leaving it label-less (optargs).
          if (TypePtr arr = syntactic_fun_arrow(*b.expr)) try_unify(tv[i], arr);
        }
      }
      annot_vars_ = saved_av;
      for (size_t i = 0; i < bs.size(); ++i) {
        annot_vars_ = &avmaps[i];
        TypePtr te = infer_expr(*bs[i].expr);  // check body (best-effort)
        annot_vars_ = saved_av;
        // Pin a plain annotation's flexible holes from the body (display/kind
        // passes): `let rec eval : lexpr -> _ = ..` fills the `_` even when no
        // recursive use does.  Soft and INTO the annotation: the declared type
        // stays the face; an Any body can't erase it.
        if (plain_annot[i] && bound[i]) soft_unify(te, bound[i]);
        if (tv[i]) {
          try_unify(tv[i], te);
          // Display slots: while BOTH spines are arrows, take the DOM from the
          // FUN's own arrow (te) -- the recursion var's arrow may have come
          // from a recursive-call fallback embedding an ARG node in the dom
          // (fill_hw must show the param's adopted HW.key, not the argument's
          // SW.data) -- but the TAIL from the recursion var (a partial
          // recursive application pins it to the FOLDED abbreviation:
          // infinite's `Seq.t`, not te's raw `unit -> .. Seq.node`).  The two
          // sides are dom/cod-unified, so this is display-slot choice only.
          std::function<TypePtr(const TypePtr&, const TypePtr&, int)> zip =
              [&](const TypePtr& tvx, const TypePtr& tex, int d) -> TypePtr {
            TypePtr tr = I::Engine::repr(tvx), er = I::Engine::repr(tex);
            if (tr == er || d > 32) return tvx;  // shared graph: nothing to pick
            if (tr->kind == I::Type::Kind::Arrow && er->kind == I::Type::Kind::Arrow)
              return eng.arrow(er->dom, zip(tr->cod, er->cod, d + 1),
                               er->arrow_label, er->arrow_lbl);
            return tvx;  // tv's tail keeps folded abbreviations
          };
          TypePtr disp = zip(tv[i], te, 0);
          if (disp != tv[i]) {
            bound[i] = disp;
            bind_pattern_scheme(bs[i].pat, disp);
          }
        }
        // A plain declared type `let rec f : T = e` must not clash with the
        // body (reliable check only; mirrors the non-recursive path below).
        if (strict && bs[i].constraint_)
          if (auto* pc = std::get_if<Pvc_constraint>(&*bs[i].constraint_))
            if (pc->univars.empty()) {
              std::unordered_map<std::string, TypePtr> vars;
              if (expected_clash(te, from_coretype(*pc->typ, vars)))
                note_error("type mismatch against declared type");
            }
      }
      eng.leave_level();
      // Value restriction (mirrors the non-recursive path): generalize a
      // non-expansive RHS, otherwise demote its vars to the outer level.
      for (size_t i = 0; i < bs.size(); ++i)
        if (bound[i]) {
          if (strict || non_expansive(*bs[i].expr)) eng.generalize(bound[i]);
          else eng.demote(bound[i]);
          if (toplevel && !strict) eng.finalize_family_heads(bound[i], /*scheme=*/true);
          else if (!strict) eng.finalize_owned_family_heads(bound[i]);
        }
      return;
    }
    for (auto& b : bs) {
      // Share named type vars across this binding's annotations (params, return,
      // declared type): `let f (x:'a) (y:'a) : 'a = ..` ties them to one 'a.
      std::unordered_map<std::string, TypePtr> avars;
      auto* saved_av = annot_vars_; annot_vars_ = &avars;
      eng.enter_level();
      TypePtr te = infer_expr(*b.expr);
      TypePtr annot = nullptr;
      // A declared type `let f : T = e`: check the inferred type's identities
      // against T (a distinct local type used where another is declared is an
      // error).  We only flag identity (stamp) clashes, not structural ones --
      // structural inference is still incomplete, so unifying T into te would
      // false-reject (e.g. array vs iarray); the identity layer is reliable.
      if (b.constraint_)
        if (auto* pc = std::get_if<Pvc_constraint>(&*b.constraint_)) {
          for (auto& u : pc->univars) newtype_vars[u.txt] = newtype_binding();
          annot = from_coretype(*pc->typ, avars);
          if (strict && expected_clash(te, annot))
            note_error("type mismatch against declared type");
          mark_if_iarray(*b.expr, annot);  // `let a : _ iarray = [|..|]` -> Immutable
          // Flow the declared type `let x : T = e` into the inferred one (pins
          // an under-determined result, e.g. `let why : unit -> unit = fun () ->
          // raise Exit`).  Non-strict only (the strict pass keeps the inferred
          // type so an incomplete-inference clash can't false-reject); soft so a
          // stray clash can't abort the pass.
          if (!strict) soft_unify(te, annot);
        } else if (auto* co = std::get_if<Pvc_coercion>(&*b.constraint_)) {
          // `let x : T1 :> T2 = e` (a binding-level coercion) binds x to the
          // TARGET T2, exactly like a `(e : T1 :> T2)` expression coercion.  The
          // body `e` was already inferred above for its kinds/effects; the
          // widening `:>` deliberately loosens the type, so we do NOT unify the
          // target back into the (narrower) body -- we only adopt it for display.
          std::unordered_map<std::string, TypePtr> vars;
          annot = from_coretype(*co->coercion, vars);
        }
      eng.leave_level();
      // In the --infer DISPLAY pass the binding's type IS its annotation:
      // `let x : int Seq.t = fun () -> ..` prints `int Seq.t`, not the body's
      // expanded arrow.  (soft_unify above already flowed the body's constraints
      // into the annotation's flexible vars.)
      TypePtr bound = (fold_abbrevs_ && annot) ? annot : te;
      // Value restriction: generalise only a non-expansive (syntactic-value) RHS,
      // so `ref []` stays weak and is pinned by later use (`int list ref`, not
      // `'a list ref`).  Strict pass keeps generalising everything (an
      // over-eager weak var could false-reject a valid polymorphic use).
      if (strict || non_expansive(*b.expr)) eng.generalize(bound);
      else eng.demote(bound);  // value restriction: lower, don't trap at inner level
      if (toplevel && !strict) eng.finalize_family_heads(bound, /*scheme=*/true);
      else if (!strict) eng.finalize_owned_family_heads(bound);
      bind_pattern_scheme(b.pat, bound);
      annot_vars_ = saved_av;
    }
  }

  // Value-restriction non-expansiveness (a conservative subset of OCaml's
  // is_nonexpansive): syntactic values whose type may be generalised.  Anything
  // not certainly a value (application, if/match/try, ...) is expansive -> weak.
  bool non_expansive(const Expression& e) {
    if (std::holds_alternative<Pexp_ident>(e.desc) ||
        std::holds_alternative<Pexp_constant>(e.desc) ||
        std::holds_alternative<Pexp_function>(e.desc)) return true;
    if (auto* t = std::get_if<Pexp_tuple>(&e.desc)) {
      for (auto& x : t->elems) if (!non_expansive(*x)) return false;
      return true;
    }
    if (auto* k = std::get_if<Pexp_construct>(&e.desc))
      return !k->arg || non_expansive(**k->arg);
    if (auto* v = std::get_if<Pexp_variant>(&e.desc))
      return !v->arg || non_expansive(**v->arg);
    if (auto* r = std::get_if<Pexp_record>(&e.desc)) {
      if (r->base && !non_expansive(**r->base)) return false;
      for (auto& [_, x] : r->fields) if (!non_expansive(*x)) return false;
      return true;
    }
    if (auto* f = std::get_if<Pexp_field>(&e.desc)) return non_expansive(*f->e);
    if (auto* c = std::get_if<Pexp_constraint>(&e.desc)) return non_expansive(*c->e);
    if (auto* c = std::get_if<Pexp_coerce>(&e.desc)) return non_expansive(*c->e);
    if (auto* nt = std::get_if<Pexp_newtype>(&e.desc)) return non_expansive(*nt->body);
    if (std::holds_alternative<Pexp_lazy>(e.desc)) return true;  // lazy is a value
    // An object literal is a value when its fields are (ocamlc's
    // is_nonexpansive Texp_object: immutable vals + methods).  mixin3's
    // `let var = object .. end` generalizes -- `([> var ], var) ops`.
    if (auto* ob = std::get_if<Pexp_object>(&e.desc)) {
      for (auto& f : ob->cs->fields) {
        if (auto* v = std::get_if<Pcf_val>(&f.desc)) {
          if (v->mut == MutableFlag::Mutable) return false;
          if (auto* cc = std::get_if<Cfk_concrete>(&v->kind))
            if (!non_expansive(*cc->e)) return false;
        }
      }
      return true;
    }
    // A local open `M.(e)` (Pexp_struct_item wrapping an open) is as expansive as
    // its body -- the open introduces no computation.  So `let a, b = M.(x, y)`
    // generalizes like the bare tuple would (matches ocamlc's is_nonexpansive on
    // Texp_open).
    if (auto* si = std::get_if<Pexp_struct_item>(&e.desc))
      return non_expansive(*si->body);
    if (auto* l = std::get_if<Pexp_let>(&e.desc)) {
      for (auto& b : l->bindings) if (!non_expansive(*b.expr)) return false;
      return non_expansive(*l->body);
    }
    return false;  // apply / if / match / try / sequence / while / for / send / ...
  }

  // Pure (no-mutation) check: do two types carry distinct local type identities
  // at corresponding positions?  Used to apply a binding's declared type without
  // the structural unification that incomplete inference would trip on.
  static bool identity_clash(const TypePtr& a0, const TypePtr& b0) {
    TypePtr a = I::Engine::repr(a0), b = I::Engine::repr(b0);
    if (a->kind == I::Type::Kind::Constr && b->kind == I::Type::Kind::Constr) {
      if (a->stamp && b->stamp && a->stamp != b->stamp) return true;
      size_t n = std::min(a->args.size(), b->args.size());
      for (size_t i = 0; i < n; ++i) if (identity_clash(a->args[i], b->args[i])) return true;
      return false;
    }
    if (a->kind == I::Type::Kind::Arrow && b->kind == I::Type::Kind::Arrow)
      return identity_clash(a->dom, b->dom) || identity_clash(a->cod, b->cod);
    if (a->kind == I::Type::Kind::Tuple && b->kind == I::Type::Kind::Tuple) {
      size_t n = std::min(a->args.size(), b->args.size());
      for (size_t i = 0; i < n; ++i) if (identity_clash(a->args[i], b->args[i])) return true;
      return false;
    }
    return false;
  }

  // Signature inclusion (Includemod): every value required by an ascribed
  // signature must be provided with a compatible type.  Missing names are a
  // certain error; value-type mismatches are flagged only via expected_clash
  // (reliable builtins + identity), so an incomplete inferred structure type
  // can't false-report.  Inline signatures only (a named sig has no types here);
  // skip sigs with `include` (we can't be sure what they require).
  // Does a module expression's body bring names in via a top-level open?  If so,
  // our flat export map conflates opened (non-exported, possibly shadowing) names
  // with the module's real exports, so value-type checking is unreliable there.
  static bool body_has_toplevel_open(const ModuleExpr& me) {
    const ModuleExpr* m = &me;
    while (auto* mc = std::get_if<Pmod_constraint>(&m->desc)) m = mc->me.get();
    if (auto* ms = std::get_if<Pmod_structure>(&m->desc))
      for (auto& it : ms->items)
        if (std::holds_alternative<Pstr_open>(it.desc)) return true;
    return false;
  }
  void check_sig_missing(const std::unordered_map<std::string, TypePtr>& provided,
                         const ModuleType& mt, bool reliable_types = true) {
    if (!strict) return;
    if (auto* mw = std::get_if<Pmty_with>(&mt.desc)) { check_sig_missing(provided, *mw->mt, reliable_types); return; }
    if (auto* mi = std::get_if<Pmty_ident>(&mt.desc)) {  // named sig: names only
      auto* l = std::get_if<Lident>(&mi->id.txt.v);
      if (!l) return;
      auto e = modtype_env.find(l->name);
      if (e == modtype_env.end()) return;
      for (auto& n : e->second)
        if (!provided.count(n))
          note_error("Signature mismatch: the value \"" + n + "\" is required but not provided");
      return;
    }
    auto* sg = std::get_if<Pmty_signature>(&mt.desc);
    if (!sg) return;
    for (auto& it : sg->items)
      if (std::holds_alternative<Psig_include>(it.desc)) return;  // can't be sure
    for (auto& it : sg->items) {
      const ValueDescription* vd = nullptr;
      if (auto* v = std::get_if<Psig_value>(&it.desc)) vd = &v->vd;
      else if (auto* p = std::get_if<Psig_primitive>(&it.desc))
        { /* external: name only */ if (!provided.count(p->pd.name.txt))
            note_error("Signature mismatch: the value \"" + p->pd.name.txt +
                       "\" is required but not provided"); continue; }
      if (!vd) continue;
      auto f = provided.find(vd->name.txt);
      if (f == provided.end()) {
        note_error("Signature mismatch: the value \"" + vd->name.txt +
                   "\" is required but not provided");
      } else if (reliable_types) {
        std::unordered_map<std::string, TypePtr> vars;
        if (expected_clash(f->second, from_coretype(*vd->type, vars)))
          note_error("Signature mismatch: the value \"" + vd->name.txt +
                     "\" has an incompatible type");
      }
    }
  }

  // Do a structure's type definition and a signature's differ incompatibly?
  // Conservative: an abstract spec accepts anything; otherwise compare only
  // like-with-like (both manifests -> reliable-builtin/identity clash; both
  // variants/records -> constructor/field name sets), skipping GADTs and
  // cross-kind/abstract-impl cases so we never false-reject.
  // Reliable clash between two written core types (used for inclusion checks):
  // a clash on builtins/arrows/tuples, never on user types (incomplete inference).
  bool ct_clash(const CoreType& a, const CoreType& b) {
    std::unordered_map<std::string, TypePtr> va, vb;
    return expected_clash(from_coretype(a, va), from_coretype(b, vb));
  }
  // Reliable clash between two constructor argument lists.
  bool ctor_args_clash(const ConstructorArguments& a, const ConstructorArguments& b) {
    auto* at = std::get_if<Pcstr_tuple>(&a);
    auto* bt = std::get_if<Pcstr_tuple>(&b);
    if (at && bt) {
      if (at->elems.size() != bt->elems.size()) return true;  // different arity
      for (size_t i = 0; i < at->elems.size(); ++i)
        if (ct_clash(*at->elems[i], *bt->elems[i])) return true;
      return false;
    }
    auto* ar = std::get_if<Pcstr_record>(&a);
    auto* br = std::get_if<Pcstr_record>(&b);
    if (ar && br) {
      if (ar->fields.size() != br->fields.size()) return true;
      for (size_t i = 0; i < ar->fields.size(); ++i)
        if (ar->fields[i].name.txt != br->fields[i].name.txt ||
            ct_clash(*ar->fields[i].type, *br->fields[i].type)) return true;
      return false;
    }
    return (at != nullptr) != (bt != nullptr);  // tuple vs inline-record: a clash
  }
  bool type_decls_clash(const TypeDeclaration& impl, const TypeDeclaration& spec) {
    if (impl.params.size() != spec.params.size())
      return true;  // arity mismatch (`type 'a t` vs `type t`): always an error
    if (std::holds_alternative<Ptype_abstract>(spec.kind) && !spec.manifest)
      return false;  // spec abstract (matching arity): any implementation is fine
    if (impl.manifest && spec.manifest)
      return ct_clash(*impl.manifest->get(), *spec.manifest->get());
    auto* iv = std::get_if<Ptype_variant>(&impl.kind);
    auto* sv = std::get_if<Ptype_variant>(&spec.kind);
    if (iv && sv) {
      std::unordered_map<std::string, const ConstructorDecl*> mi, ms;
      for (auto& c : iv->ctors) { if (c.res) return false; mi[c.name.txt] = &c; }
      for (auto& c : sv->ctors) { if (c.res) return false; ms[c.name.txt] = &c; }
      if (mi.size() != ms.size()) return true;
      for (auto& [n, sc] : ms) {
        auto f = mi.find(n);
        if (f == mi.end()) return true;                       // name-set mismatch
        if (ctor_args_clash(f->second->args, sc->args)) return true;  // arg-type mismatch
      }
      return false;
    }
    auto* ir = std::get_if<Ptype_record>(&impl.kind);
    auto* sr = std::get_if<Ptype_record>(&spec.kind);
    if (ir && sr) {
      if (ir->fields.size() != sr->fields.size()) return true;
      std::unordered_map<std::string, const LabelDecl*> mi;
      for (auto& f : ir->fields) mi[f.name.txt] = &f;
      for (auto& f : sr->fields) {
        auto g = mi.find(f.name.txt);
        if (g == mi.end()) return true;                       // field-set mismatch
        if (ct_clash(*g->second->type, *f.type)) return true;  // field-type mismatch
      }
      return false;
    }
    return false;  // cross-kind / abstract impl: not sure -> don't flag
  }
  // Includemod, type side: compare the structure's own top-level type decls
  // against those the ascribed signature declares.
  void check_sig_types(const ModuleExpr& me, const ModuleType& mt) {
    if (!strict) return;
    auto* sg = std::get_if<Pmty_signature>(&mt.desc);
    if (!sg) return;
    const ModuleExpr* m = &me;
    while (auto* mc = std::get_if<Pmod_constraint>(&m->desc)) m = mc->me.get();
    auto* ms = std::get_if<Pmod_structure>(&m->desc);
    if (!ms) return;
    std::unordered_map<std::string, const TypeDeclaration*> impl;
    for (auto& it : ms->items)
      if (auto* ty = std::get_if<Pstr_type>(&it.desc))
        for (auto& d : ty->decls) impl[d.name.txt] = &d;
    for (auto& it : sg->items)
      if (auto* pst = std::get_if<Psig_type>(&it.desc))
        for (auto& sd : pst->decls) {
          auto f = impl.find(sd.name.txt);  // missing-type: provided elsewhere -> skip
          if (f != impl.end() && type_decls_clash(*f->second, sd))
            note_error("Signature mismatch: type \"" + sd.name.txt +
                       "\" does not match its signature");
        }
  }

  // Value restriction (Typemod's non-generalizable-escape check): after a whole
  // structure is typed, an EXPORTED top-level value binding whose expansive RHS
  // leaves a weak (non-generalizable) type variable in its FINAL type is an error
  // (`let blurp = f 0` : '_weak -> '_weak, never resolved).  A later use that
  // pins the variable (`x := [1]`) clears it; a syntactic value generalizes.
  //
  // Scan a type for a weak variable in a position OCaml's RELAXED value
  // restriction cannot generalize.  Relaxed VR generalizes a weak var that
  // occurs only COVARIANTLY (`'a option`, `'a array list` when non-expansive) --
  // so those must NOT be flagged.  It does NOT generalize a var reachable through
  // a contravariant (arrow-domain) or invariant (ref/array) position (`'_weak ->
  // '_weak`, `'_weak list ref`).  `pol`: +1 covariant, -1 contravariant, 0
  // invariant.  We only flag a plain Var at pol != +1; we descend UNKNOWN
  // constructors as covariant-safe (+1) so an unknown-variance user type can
  // never cause a false reject.  `any` (our incompleteness marker) suppresses.
  static bool invariant_ctor(const std::string& p) {
    auto d = p.rfind('.');
    std::string b = d == std::string::npos ? p : p.substr(d + 1);
    return b == "ref" || b == "array" || b == "iarray";
  }
  static void scan_weak(const TypePtr& t0, int pol, std::set<I::Type*>& seen,
                        bool& weak, bool& any) {
    TypePtr t = I::Engine::repr(t0);
    if (!seen.insert(t.get()).second) return;
    using K = I::Type::Kind;
    switch (t->kind) {
      case K::Var:
        if (t->level != I::GENERIC_LEVEL && pol != 1) weak = true;
        return;
      case K::Arrow:
        scan_weak(t->dom, pol == 0 ? 0 : -pol, seen, weak, any);  // domain flips
        scan_weak(t->cod, pol, seen, weak, any);
        return;
      case K::Tuple:
        for (auto& a : t->args) scan_weak(a, pol, seen, weak, any);  // covariant
        return;
      case K::Constr: {
        // ref/array/iarray are invariant; every other (user/covariant) ctor we
        // descend as covariant-safe (+1) -- sound: never flags a var OCaml might
        // generalize under an unknown-variance parameter.
        int cp = invariant_ctor(t->path) ? 0 : 1;
        for (auto& a : t->args) scan_weak(a, cp, seen, weak, any);
        return;
      }
      case K::Variant:
      case K::Object:  // rows: never flag the node; descend args covariant-safe
        for (auto& a : t->args) scan_weak(a, 1, seen, weak, any);
        return;
      default:
        return;
    }
  }
  // Run on a NON-STRICT checker after run_checker (value restriction respected,
  // so weak vars survive as non-GENERIC; the strict pass force-generalizes and
  // can't see them).  Returns the error for the first offending top-level
  // NON-RECURSIVE simple-var binding whose expansive RHS leaves a
  // non-generalizable weak var, else "".  (let rec generalization is subtler --
  // `let rec x = let y = [||] in y :: x` is a value -- so those are skipped.)
  std::string weak_escape_error(const ast::Structure& s) {
    for (auto& it : s) {
      auto* sv = std::get_if<Pstr_value>(&it.desc);
      if (!sv || sv->rf == RecFlag::Recursive) continue;
      for (auto& b : sv->bindings) {
        auto* pv = std::get_if<Ppat_var>(&b.pat.desc);
        if (!pv) continue;                       // simple `let x = e` only
        if (non_expansive(*b.expr)) continue;    // value: OCaml generalizes it
        auto f = venv.back().find(pv->name.txt);
        if (f == venv.back().end()) continue;
        std::set<I::Type*> seen;
        bool weak = false, any = false;
        scan_weak(f->second, 1, seen, weak, any);
        if (weak && !any)
          return "The type of this expression contains the non-generalizable "
                 "type variable(s) (value restriction): " + pv->name.txt;
      }
    }
    return "";
  }

  // Builtins whose inferred type we trust enough to flag against an expected
  // type (constants, arithmetic, comparisons produce these reliably).  Excludes
  // array/list/user types, where our inference is still incomplete.
  static bool reliable_builtin(const std::string& path) {
    auto d = path.rfind('.');
    std::string b = d == std::string::npos ? path : path.substr(d + 1);
    static const std::set<std::string> s = {
        "int", "char", "string", "float", "bool", "unit",
        "int32", "int64", "nativeint", "exn", "bytes"};
    return s.count(b);
  }
  // Like identity_clash, but also flags a mismatch between two distinct reliable
  // builtins (int vs string, float vs int).  Pure, no mutation.  Used to apply a
  // declared/expected type without the full structural unify that incomplete
  // inference trips on.
  // A stricter clash than expected_clash for argument positions: fire ONLY on a
  // reliable-builtin mismatch (int vs string, float vs int, ...), recursing into
  // same-head constructors/arrows/tuples.  Never on differing stamps -- those
  // include GADT-refined and locally-abstract types that instantiate at the call,
  // so flagging them would false-reject valid code.
  static bool builtin_clash(const TypePtr& a0, const TypePtr& b0) {
    TypePtr a = I::Engine::repr(a0), b = I::Engine::repr(b0);
    auto last = [](const std::string& p) {
      auto d = p.rfind('.'); return d == std::string::npos ? p : p.substr(d + 1);
    };
    if (a->kind == I::Type::Kind::Constr && b->kind == I::Type::Kind::Constr) {
      if (reliable_builtin(a->path) && reliable_builtin(b->path) &&
          last(a->path) != last(b->path))
        return true;
      if (last(a->path) == last(b->path)) {  // same head: compare arguments
        size_t n = std::min(a->args.size(), b->args.size());
        for (size_t i = 0; i < n; ++i) if (builtin_clash(a->args[i], b->args[i])) return true;
      }
      return false;
    }
    if (a->kind == I::Type::Kind::Arrow && b->kind == I::Type::Kind::Arrow)
      return builtin_clash(a->dom, b->dom) || builtin_clash(a->cod, b->cod);
    if (a->kind == I::Type::Kind::Tuple && b->kind == I::Type::Kind::Tuple) {
      size_t n = std::min(a->args.size(), b->args.size());
      for (size_t i = 0; i < n; ++i) if (builtin_clash(a->args[i], b->args[i])) return true;
    }
    // A polymorphic-variant row can never be a scalar builtin (a Constr
    // ABBREVIATING a variant has its own path, not a builtin's, so it never
    // reaches this arm).
    auto scalar = [](const TypePtr& t) {
      return t->kind == I::Type::Kind::Constr && t->args.empty() &&
             reliable_builtin(t->path);
    };
    if ((a->kind == I::Type::Kind::Variant && scalar(b)) ||
        (b->kind == I::Type::Kind::Variant && scalar(a)))
      return true;
    return false;
  }
  static bool expected_clash(const TypePtr& a0, const TypePtr& b0) {
    TypePtr a = I::Engine::repr(a0), b = I::Engine::repr(b0);
    // A nullary reliable builtin (int, string, ...) can never be a function or
    // tuple, so a shape mismatch against one is a definite clash (`let f : int =
    // fun x -> x`).  Restricted to reliable builtins so a user abbreviation that
    // expands to an arrow/tuple (`type fn = int -> int`) is never mis-flagged.
    auto rigid_base = [](const TypePtr& t) {
      return t->kind == I::Type::Kind::Constr && reliable_builtin(t->path);
    };
    auto arrow_or_tuple = [](const TypePtr& t) {
      return t->kind == I::Type::Kind::Arrow || t->kind == I::Type::Kind::Tuple;
    };
    if ((rigid_base(a) && arrow_or_tuple(b)) || (rigid_base(b) && arrow_or_tuple(a)))
      return true;
    if (a->kind == I::Type::Kind::Constr && b->kind == I::Type::Kind::Constr) {
      if (a->stamp && b->stamp && a->stamp != b->stamp) return true;
      auto last = [](const std::string& p) {
        auto d = p.rfind('.'); return d == std::string::npos ? p : p.substr(d + 1);
      };
      if (reliable_builtin(a->path) && reliable_builtin(b->path) &&
          last(a->path) != last(b->path))
        return true;
      size_t n = std::min(a->args.size(), b->args.size());
      for (size_t i = 0; i < n; ++i) if (expected_clash(a->args[i], b->args[i])) return true;
      return false;
    }
    if (a->kind == I::Type::Kind::Arrow && b->kind == I::Type::Kind::Arrow)
      return expected_clash(a->dom, b->dom) || expected_clash(a->cod, b->cod);
    if (a->kind == I::Type::Kind::Tuple && b->kind == I::Type::Kind::Tuple) {
      size_t n = std::min(a->args.size(), b->args.size());
      for (size_t i = 0; i < n; ++i) if (expected_clash(a->args[i], b->args[i])) return true;
      return false;
    }
    return false;
  }

  // Bind a let pattern's variables to a (generalized) type.
  void bind_pattern_scheme(const Pattern& p, const TypePtr& te) {
    if (auto* v = std::get_if<Ppat_var>(&p.desc)) {
      if (record_kinds_) rec_pat_[&p] = te;
      venv.back()[v->name.txt] = te;
      return;
    }
    // `let (a, b) = (e1, e2)` -- generalize component-wise.  te was generalized
    // as a whole (its component types carry generic vars), so binding each var
    // to its corresponding component gives each name its own polymorphic scheme
    // (`let f, g = (fun x -> x), (fun y -> y)` => both `'a -> 'a`).  Unifying a
    // fresh monomorphic infer_pat() var against te instead (the fallback below)
    // would trap each name at the current level -> monomorphic, pinned by later
    // use.  Only fires when the pattern and type shapes match; otherwise falls
    // through to the unify path.
    if (auto* tp = std::get_if<Ppat_tuple>(&p.desc)) {
      TypePtr r = I::Engine::repr(te);
      if (r->kind == I::Type::Kind::Tuple && r->args.size() == tp->elems.size()) {
        for (size_t i = 0; i < tp->elems.size(); ++i)
          bind_pattern_scheme(*tp->elems[i], r->args[i]);
        return;
      }
    }
    // complex pattern: unify and bind its vars monomorphically
    try_unify(infer_pat(p), te);
  }

  // Bring a module's exported value schemes into the current scope (open M):
  // local module from modenv, else a Stdlib submodule cmi.
  void open_into(const Longident& m) {
    for (auto& [k, v] : resolve_module_values(m)) venv.back()[k] = v;
  }

  // The value exports of a module expression (value name -> scheme).
  std::unordered_map<std::string, TypePtr> module_exports(const ModuleExpr& me) {
    if (auto* ms = std::get_if<Pmod_structure>(&me.desc)) {
      venv.emplace_back();
      tenv.emplace_back();  // the module's type declarations are scoped here
      cenv.emplace_back();  // and its constructors
      process_items(ms->items);
      cenv.pop_back();
      tenv.pop_back();
      auto exports = std::move(venv.back());
      venv.pop_back();
      return exports;
    }
    if (auto* mi = std::get_if<Pmod_ident>(&me.desc)) {
      if (strict && module_head_unbound(mi->id.txt))  // module F = <unbound>
        note_error("Unbound module " + mod_components(mi->id.txt).front());
      return resolve_module_values(mi->id.txt);  // local alias or stdlib (sub)module
    }
    if (auto* mc = std::get_if<Pmod_constraint>(&me.desc)) {
      // Is the constrained module a structure (an ascription to check) or
      // something else like `(val e : S)` (which yields S's values)?
      const ModuleExpr* m = mc->me.get();
      while (auto* c = std::get_if<Pmod_constraint>(&m->desc)) m = c->me.get();
      bool is_struct = std::holds_alternative<Pmod_structure>(m->desc);
      auto inner = module_exports(*mc->me);
      if (is_struct || !inner.empty()) {
        check_sig_missing(inner, *mc->mt, !body_has_toplevel_open(*mc->me));
        check_sig_types(*mc->me, *mc->mt);
        // `(M : S)` with M a named module ascribes: only S's names are visible
        // outside, at M's types.  `open (List : sig val map : .. end)` must not
        // leak List.hd over a user-defined `hd` (accepted_batch).  A literal
        // struct keeps its full exports (the ascription was checked above).
        if (!is_struct) {
          auto restricted = modtype_values(*mc->mt);
          if (!restricted.empty()) {
            for (auto& [k, v] : restricted)
              if (auto f = inner.find(k); f != inner.end()) v = f->second;
            inner = std::move(restricted);
          }
        }
        // An ascribed val takes the SIGNATURE's declared type, which is what
        // ocamlc reports at every outside use (`val write : 'a tag -> 'a ->
        // unit` wins over the struct body's raise-typed `.. -> 'b`; msg.ml).
        // Bare constrs naming the ascription's own types are qualified by the
        // binding (`t` -> `Msg.t`) to match the struct-inferred paths.  A
        // translation containing Any (an untranslated corner) keeps the
        // struct-inferred type.  Non-strict only.
        if (!strict)
          if (auto* sg = std::get_if<Pmty_signature>(&mc->mt->desc)) {
            std::set<std::string> own;
            for (auto& sit : sg->items)
              if (auto* pt = std::get_if<Psig_type>(&sit.desc))
                for (auto& d : pt->decls) own.insert(d.name.txt);
            // qualify own-type constrs
            std::function<bool(const TypePtr&, std::set<const I::Type*>&)> qual =
                [&](const TypePtr& t0, std::set<const I::Type*>& seen) -> bool {
              TypePtr t = I::Engine::repr(t0);
              if (!t || !seen.insert(t.get()).second) return true;
              if (t->kind == I::Type::Kind::Constr &&
                  t->path.find('.') == std::string::npos && own.count(t->path) &&
                  !func_bind_name_.empty())
                t->path = func_bind_name_ + "." + t->path;
              bool ok = true;
              if (t->dom) ok &= qual(t->dom, seen);
              if (t->cod) ok &= qual(t->cod, seen);
              for (auto& a : t->args) ok &= qual(a, seen);
              for (auto& a : t->abbrev_args) ok &= qual(a, seen);
              for (auto& a : t->inherited) ok &= qual(a, seen);
              return ok;
            };
            for (auto& sit : sg->items)
              if (auto* pv = std::get_if<Psig_value>(&sit.desc)) {
                std::unordered_map<std::string, TypePtr> vars;
                TypePtr t = from_coretype(*pv->vd.type, vars);
                std::set<const I::Type*> seen;
                if (t && qual(t, seen)) inner[pv->vd.name.txt] = t;
              }
          }
        return inner;
      }
      return modtype_values(*mc->mt);  // e.g. `(val e : S)` parsed as a constraint
    }
    if (auto* mu = std::get_if<Pmod_unpack>(&me.desc)) {
      // (val e : S ...): resolve S's value names from the expression's package type
      const Expression* ie = mu->e.get();
      if (auto* ct = std::get_if<Pexp_constraint>(&ie->desc))
        if (auto* pk = std::get_if<Ptyp_package>(&ct->t->desc)) {
          if (!strict) {
            // Tie the unpacked expression to the package type, so a plain
            // param flowing in adopts `(module Set.S with type elt = 's)`
            // (fstclassmod's `let module Set = (val set : ..)`).
            soft_unify(infer_expr(*ct->e), package_type(*pk));
            // Bind the module's values at the modtype's declared types with
            // the `with type` constraints substituted (elt := s), so the
            // body's `Set.add l Set.empty` ties l : s list.
            std::unordered_map<std::string, TypePtr> argtypes;
            for (auto& [lid, ctb] : pk->constraints) {
              std::unordered_map<std::string, TypePtr> vars;
              argtypes[lid_full(lid.txt)] = from_coretype(*ctb, vars);
            }
            auto vals = cmi_modtype_value_schemes(pk->path.txt, argtypes);
            if (!vals.empty()) return vals;
            if (auto* pl = std::get_if<Lident>(&pk->path.txt.v))
              if (auto sg = modtype_sig_asts_.find(pl->name);
                  sg != modtype_sig_asts_.end())
                return unpack_module_values(
                    func_bind_name_.empty() ? pl->name : func_bind_name_,
                    *sg->second, &argtypes);
          }
          return modtype_values_of(pk->path.txt);
        }
      return {};
    }
    if (std::get_if<Pmod_apply>(&me.desc) || std::get_if<Pmod_apply_unit>(&me.desc)) {
      // possibly-curried functor application F(A)(B)...: count the applications
      // and find the head functor ident.
      int napp = 0;
      const ModuleExpr* h = &me;
      while (true) {
        if (auto* a = std::get_if<Pmod_apply>(&h->desc)) { ++napp; h = a->f.get(); }
        else if (auto* au = std::get_if<Pmod_apply_unit>(&h->desc)) { ++napp; h = au->f.get(); }
        else break;
      }
      if (auto* fi = std::get_if<Pmod_ident>(&h->desc)) {
        if (strict && module_head_unbound(fi->id.txt))  // module O = <unbound>.Make(..)
          note_error("Unbound module " + mod_components(fi->id.txt).front());
        // A single-application LOCAL functor with a known result signature:
        // instantiate it with the argument's types (Elem.t -> the arg's t).
        if (napp == 1)
          if (auto* ap = std::get_if<Pmod_apply>(&me.desc)) {
            auto comps = mod_components(fi->id.txt);
            if (comps.size() == 1)
              if (auto fd = functor_defs_.find(comps[0]); fd != functor_defs_.end()) {
                auto r = instantiate_local_functor(fd->second, *ap->arg);
                if (!r.empty()) return r;
              }
            // `Msg.Define(struct ..)`: a functor DECLARED in a local module's
            // ascription signature.  Build its FunctorDef from the declared
            // functor type and instantiate (this also registers the result
            // sig's typext ctors with the parameter substituted -- msg.ml).
            if (comps.size() == 2)
              if (auto ms = module_sig_asts_.find(comps[0]); ms != module_sig_asts_.end())
                for (auto& sit : *ms->second)
                  if (auto* pm = std::get_if<Psig_module>(&sit.desc))
                    if (pm->md.name.txt && *pm->md.name.txt == comps[1])
                      if (auto* mf = std::get_if<Pmty_functor>(&pm->md.type->desc)) {
                        FunctorDef fd;
                        if (auto* fn = std::get_if<Functor_named>(&mf->param)) {
                          if (fn->name.txt) fd.param = *fn->name.txt;
                          fd.param_sig = fn->type.get();
                        }
                        if (std::holds_alternative<Pmty_signature>(mf->body->desc))
                          fd.result_sig = mf->body.get();
                        if (!fd.param.empty() && fd.result_sig) {
                          auto r = instantiate_local_functor(fd, *ap->arg);
                          if (!r.empty()) return r;
                        }
                      }
          }
        const ModuleExpr* arg1 = nullptr;
        if (napp == 1)
          if (auto* ap = std::get_if<Pmod_apply>(&me.desc)) arg1 = ap->arg.get();
        // A LOCAL functor whose body is itself a functor application resolves
        // through the body's head: `IntSetSet = PowerSet(IntSet)(..)` with
        // PowerSet's body `Set.Make(SetOrd(BaseSet))` takes Set.Make's result
        // values, abstract types named after the binding (IntSetSet.t) -- the
        // plain functor_env harvest would leave every export a generic var.
        {
          auto comps = mod_components(fi->id.txt);
          if (comps.size() == 1)
            if (auto fb = functor_body_exprs_.find(comps[0]);
                fb != functor_body_exprs_.end()) {
              int bnapp = 0;
              const ModuleExpr* bh = fb->second;
              while (true) {
                if (auto* a2 = std::get_if<Pmod_apply>(&bh->desc)) { ++bnapp; bh = a2->f.get(); }
                else if (auto* au2 = std::get_if<Pmod_apply_unit>(&bh->desc)) { ++bnapp; bh = au2->f.get(); }
                else break;
              }
              if (bnapp > 0)
                if (auto* bhi = std::get_if<Pmod_ident>(&bh->desc))
                  if (mod_components(bhi->id.txt) != comps) {  // no self-recursion
                    auto r = functor_result_values(bhi->id.txt, bnapp, nullptr);
                    if (!r.empty()) return r;
                  }
            }
        }
        return functor_result_values(fi->id.txt, napp, arg1);
      }
      return {};
    }
    return {};  // functor definition itself: no values
  }

  // Process structure items into the current scope, populating modenv for
  // submodules.  Per-item best-effort (a bad item doesn't abort the rest).
  void process_items(const ast::Structure& items) {
    for (auto& it : items) process_item(it);
  }

  // Pre-pass: collect every module name bound in the file (modules, recursive
  // modules, functor parameters, first-class-module parameters/unpacks) so the
  // unbound-module check never flags one.  Over-inclusion only loses recall.
  void collect_bound_modules(const ast::Structure& items) {
    for (auto& it : items) {
      if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
        if (mb->binding.name.txt) bound_module_names_.insert(*mb->binding.name.txt);
        collect_bound_modules_me(mb->binding.expr);
      } else if (auto* rm = std::get_if<Pstr_recmodule>(&it.desc)) {
        for (auto& b : rm->bindings) {
          if (b.name.txt) bound_module_names_.insert(*b.name.txt);
          collect_bound_modules_me(b.expr);
        }
      } else if (auto* in = std::get_if<Pstr_include>(&it.desc)) {
        collect_bound_modules_me(in->expr);
        if (auto* pi = std::get_if<Pmod_ident>(&in->expr.desc)) {
          for (auto& s : module_submodule_names(pi->id.txt)) opened_submodules_.insert(s);
          included_module_prefixes_.insert(lid_full(pi->id.txt) + ".");
        }
      } else if (auto* op = std::get_if<Pstr_open>(&it.desc)) {
        // `open M` brings M's submodules into bare scope -- collect now (before
        // type declarations referencing them are checked).
        if (auto* pi = std::get_if<Pmod_ident>(&op->expr.desc))
          for (auto& s : module_submodule_names(pi->id.txt)) opened_submodules_.insert(s);
      } else if (auto* sv = std::get_if<Pstr_value>(&it.desc)) {
        for (auto& b : sv->bindings) collect_bound_modules_expr(*b.expr);
      } else if (auto* ev = std::get_if<Pstr_eval>(&it.desc)) {
        collect_bound_modules_expr(*ev->e);
      }
    }
  }
  void collect_bound_modules_me(const ModuleExpr& me) {
    if (auto* pf = std::get_if<Pmod_functor>(&me.desc)) {
      if (auto* fp = std::get_if<Functor_named>(&pf->param); fp && fp->name.txt)
        bound_module_names_.insert(*fp->name.txt);
      collect_bound_modules_me(*pf->body);
    } else if (auto* ps = std::get_if<Pmod_structure>(&me.desc)) {
      collect_bound_modules(ps->items);
    } else if (auto* pc = std::get_if<Pmod_constraint>(&me.desc)) {
      collect_bound_modules_me(*pc->me);
    }
  }
  // Walk an expression for module-binding sites: `let module M = ..`, first-class
  // module function parameters `(module P : S)`, and nested functions/lets.
  void collect_bound_modules_expr(const Expression& e) {
    if (auto* fn = std::get_if<Pexp_function>(&e.desc)) {
      for (auto& p : fn->params)
        if (auto* pv = std::get_if<Pparam_val>(&p.desc)) {
          const Pattern* pat = &pv->pat;
          while (auto* pc = std::get_if<Ppat_constraint>(&pat->desc)) pat = pc->p.get();
          if (auto* up = std::get_if<Ppat_unpack>(&pat->desc); up && up->name.txt)
            bound_module_names_.insert(*up->name.txt);
        }
      if (auto* b = std::get_if<Pfunction_body>(&fn->body->v)) collect_bound_modules_expr(*b->e);
    } else if (auto* si = std::get_if<Pexp_struct_item>(&e.desc)) {
      if (auto* mb = std::get_if<Pstr_module>(&si->item->desc))
        if (mb->binding.name.txt) bound_module_names_.insert(*mb->binding.name.txt);
      collect_bound_modules_expr(*si->body);
    } else if (auto* le = std::get_if<Pexp_let>(&e.desc)) {
      for (auto& b : le->bindings) collect_bound_modules_expr(*b.expr);
      collect_bound_modules_expr(*le->body);
    }
  }

  void process_item(const StructureItem& it) {
    {
      try {
        if (auto* ty = std::get_if<Pstr_type>(&it.desc)) {
          // bind these type names' identities and constructors in scope, in order
          for (auto& d : ty->decls) {
            if (type_stamp_.count(&d)) tenv.back()[d.name.txt] = type_stamp_[&d];
            if (auto* v = std::get_if<Ptype_variant>(&d.kind))
              for (auto& c : v->ctors)
                if (ctor_scheme_.count(&c)) cenv.back()[c.name.txt] = ctor_scheme_[&c];
          }
        } else if (auto* ex = std::get_if<Pstr_exception>(&it.desc)) {
          // A local module's exception (`let module M = struct exception E of t ..`)
          // is reached only here -- register_types_rec never descends into an
          // expression's `let module`.  Register its ctor so `E x` inside pins x's
          // type (idempotent: top-level ones are already registered).
          register_exception(ex->exn.ctor);
        } else if (auto* tx = std::get_if<Pstr_typext>(&it.desc)) {
          register_typext(tx->ext);
        } else if (auto* sv = std::get_if<Pstr_value>(&it.desc))
          infer_bindings(sv->rf, sv->bindings, /*toplevel=*/true);
        else if (auto* pc = std::get_if<Pstr_class>(&it.desc)) {
          {  // all passes: class method bodies (Pexp_object) + class_types_ for `new`
            for (auto& d : pc->decls) {
              const ClassExpr* ce = &d.expr;
              std::vector<const Pcl_fun*> params;  // `class c x = ...` parameters
              std::vector<const Pcl_let*> lets;    // `class c = let .. in object`
              for (;;) {
                if (auto* pf = std::get_if<Pcl_fun>(&ce->desc)) {
                  params.push_back(pf);
                  ce = pf->body.get();
                } else if (auto* pl = std::get_if<Pcl_let>(&ce->desc)) {
                  lets.push_back(pl);
                  ce = pl->body.get();
                } else break;
              }
              if (auto* ps = std::get_if<Pcl_structure>(&ce->desc)) {
                // The class's TYPE params (`class ['a] lambda_ops`) scope over
                // the body's annotations and `constraint` fields; the object
                // then carries the CLASS name (`'a lambda_ops`), and `new c`
                // gets the constructor arrow over the value params (mixin2).
                eng.enter_level();  // the ctor scheme generalizes like a let
                std::unordered_map<std::string, TypePtr> cvars;
                std::vector<TypePtr> tparams;
                for (auto& p : d.params) {
                  TypePtr v = eng.fresh_var();
                  if (auto* pv2 = std::get_if<Ptyp_var>(&p->desc))
                    cvars[pv2->name] = v;
                  tparams.push_back(v);
                }
                std::vector<TypePtr> ptys;
                std::vector<std::pair<std::string, TypePtr>> instvars;
                TypePtr ot = infer_object_body(ps->cs, params.empty() ? nullptr : &params,
                                               lets.empty() ? nullptr : &lets,
                                               d.params.empty() ? nullptr : &cvars,
                                               &ptys, &instvars);
                eng.leave_level();
                class_instvars_[d.name.txt] = std::move(instvars);
                // A parameterless class: `new c` is its object type.  Generalise
                // so each `new c` instantiates fresh.
                if (params.empty() && d.params.empty()) {
                  eng.generalize(ot);
                  class_types_[d.name.txt] = ot;
                } else {
                  TypePtr obj = I::Engine::repr(ot);
                  if (obj->kind == I::Type::Kind::Object && !d.params.empty()) {
                    obj->abbrev = d.name.txt;
                    obj->abbrev_args = tparams;
                  }
                  TypePtr ctor = ot;
                  for (size_t i = ptys.size(); i-- > 0;) {
                    auto [lk, nm] = arglabel(params[i]->label);
                    ctor = eng.arrow(ptys[i], ctor, lk, nm);
                  }
                  eng.generalize(ctor);
                  class_ctor_types_[d.name.txt] = ctor;
                }
              }
            }
          }
        } else if (auto* pct = std::get_if<Pstr_class_type>(&it.desc)) {
          if (!strict)
            for (auto& d : pct->decls)
              if (auto* cs = std::get_if<Pcty_signature>(&d.expr.desc)) {
                ClassTypeInfo info;
                for (auto& p : d.params) {
                  auto* v = std::get_if<Ptyp_var>(&p->desc);
                  info.params.push_back(v ? v->name : "");
                }
                info.sig = &cs->cs;
                classtype_decls_[d.name.txt] = std::move(info);
              }
        } else if (auto* ev = std::get_if<Pstr_eval>(&it.desc))
          infer_expr(*ev->e);
        else if (auto* op = std::get_if<Pstr_open>(&it.desc)) {
          // `open F(X)`: name the functor result's abstract types by the
          // applicative path (`Set.Make(String).t`), as ocamlc displays them.
          // Save/restore func_bind_name_ -- this open may sit inside a module
          // binding whose own prefix is mid-flight.
          std::string saved_fbn = func_bind_name_;
          if (!strict && std::holds_alternative<Pmod_apply>(op->expr.desc))
            func_bind_name_ = resolve_local_module_path(op->expr);
          // `open F(X)` of a LOCAL functor with a struct body: register the
          // body's type decls and (GADT) ctors under the applicative path, so
          // an opened `'a event` annotation displays `MkReify(PC).event` and
          // a match on Ret/Eff resolves + windows (shallow2deep).
          if (!strict && !func_bind_name_.empty())
            if (auto* ap = std::get_if<Pmod_apply>(&op->expr.desc))
              if (auto* fh = std::get_if<Pmod_ident>(&ap->f->desc)) {
                auto comps = mod_components(fh->id.txt);
                if (comps.size() == 1)
                  if (auto fb = functor_body_exprs_.find(comps[0]);
                      fb != functor_body_exprs_.end())
                    if (auto* bs = std::get_if<Pmod_structure>(&fb->second->desc)) {
                      std::string savedp = mod_prefix_;
                      mod_prefix_ = func_bind_name_ + ".";
                      for (auto& bit : bs->items)
                        if (auto* ty2 = std::get_if<Pstr_type>(&bit.desc)) {
                          for (auto& d : ty2->decls) {
                            register_type_decl(d);
                            register_record_decl(d);
                          }
                          for (auto& d : ty2->decls) {
                            if (type_stamp_.count(&d))
                              tenv.back()[d.name.txt] = type_stamp_[&d];
                            if (auto* v2 = std::get_if<Ptype_variant>(&d.kind))
                              for (auto& c : v2->ctors)
                                if (ctor_scheme_.count(&c))
                                  cenv.back()[c.name.txt] = ctor_scheme_[&c];
                          }
                        }
                      mod_prefix_ = savedp;
                    }
              }
          for (auto& [k, v] : module_exports(op->expr)) venv.back()[k] = v;
          func_bind_name_ = saved_fbn;
          if (auto* pi = std::get_if<Pmod_ident>(&op->expr.desc)) {  // open M -> M's submodules
            for (auto& s : module_submodule_names(pi->id.txt)) opened_submodules_.insert(s);
            // Load the opened module's record fields into the label registry --
            // a QUALIFIED M.x use does this via module_values_cached, but a
            // local open `Unix.LargeFile.(..)` was skipping it, leaving
            // `st_size` spuriously UNIQUE at the parent's `Unix.stats : int`
            // (the submodule's int64 field never registered, so the ambiguity
            // that forces type-directed resolution never arose).
            load_module_record_fields(pi->id.txt);
            load_open_submod_quals(pi->id.txt);  // bare Sub -> M.Sub (every pass)
            if (!strict) {
              load_open_type_quals(pi->id.txt);  // bare type -> M.t (display)
              open_module_ctors(pi->id.txt);     // bare ctor -> M's variant ctor
              load_open_module_aliases(pi->id.txt);  // bare List -> ListLabels
            }
          }
        } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
          if (mb->binding.name.txt) {
            // A functor: record its body's exports as the application result.
            const ModuleExpr* me = &mb->binding.expr;
            // `module M : sig .. end = ..`: keep the ascription signature so a
            // functor DECLARED in it (`Msg.Define`) can later be instantiated.
            if (auto* mc0 = std::get_if<Pmod_constraint>(&me->desc))
              if (auto* sg0 = std::get_if<Pmty_signature>(&mc0->mt->desc))
                module_sig_asts_[*mb->binding.name.txt] = &sg0->items;
            while (auto* mc = std::get_if<Pmod_constraint>(&me->desc)) me = mc->me.get();
            if (std::holds_alternative<Pmod_functor>(me->desc)) {
              // Record a single-parameter functor with an explicit result
              // signature so `F(Arg)` can be instantiated (param types substituted)
              // rather than collapsed to generic vars.
              if (auto* mf0 = std::get_if<Pmod_functor>(&me->desc)) {
                const ModuleExpr* body = mf0->body.get();
                while (auto* mc = std::get_if<Pmod_constraint>(&body->desc)) {
                  FunctorDef fd;
                  if (auto* fn = std::get_if<Functor_named>(&mf0->param))
                    if (fn->name.txt) fd.param = *fn->name.txt;
                  if (auto* fn = std::get_if<Functor_named>(&mf0->param))
                    fd.param_sig = fn->type.get();
                  if (std::holds_alternative<Pmty_signature>(mc->mt->desc))
                    fd.result_sig = mc->mt.get();
                  if (!fd.param.empty() && fd.result_sig &&
                      !std::holds_alternative<Pmod_functor>(mc->me->desc))
                    functor_defs_[*mb->binding.name.txt] = fd;
                  break;
                }
              }
              // Collect the parameters (name + signature) as we unwrap to the body.
              std::vector<std::pair<std::string, const ModuleType*>> fparams;
              for (const ModuleExpr* w = me;
                   auto* mf = std::get_if<Pmod_functor>(&w->desc); w = mf->body.get())
                if (auto* fn = std::get_if<Functor_named>(&mf->param))
                  if (fn->name.txt && fn->type)
                    fparams.emplace_back(*fn->name.txt, fn->type.get());
              while (auto* mf = std::get_if<Pmod_functor>(&me->desc)) me = mf->body.get();
              // Remember the fully-unwrapped body so an APPLICATION of this
              // functor can resolve through the body's own head functor
              // (PowerSet's body `Set.Make(SetOrd(BaseSet))` -- sets.ml).
              functor_body_exprs_[*mb->binding.name.txt] = me;
              // Register each parameter's value members with their REAL types
              // (arrow labels intact) so `M.x` / `include M` inside the body
              // resolves an optional param -> the transcriber fills the omitted
              // optionals with ghost None (htbl, pr7601).  The param abstract
              // types stay abstract (no arg substitution).  Saved/restored around
              // the harvest so the params don't leak into the sibling scope.
              std::vector<std::pair<std::string,
                  std::optional<std::unordered_map<std::string, TypePtr>>>> saved_penv;
              for (auto& [pn, psig] : fparams) {
                auto prev = modenv.find(pn);
                saved_penv.emplace_back(
                    pn, prev != modenv.end() ? std::optional(prev->second) : std::nullopt);
                modenv[pn] = param_sig_value_schemes(*psig, {});
              }
              // Keep only the result's value *names* (fresh polymorphic types):
              // the body's concrete types depend on the (unsubstituted) argument,
              // so using them would surface spurious clashes -- names suffice to
              // clear unbound false-rejections without ever adding a clash.  The
              // body also can't be soundly checked without applying the functor,
              // so suppress error recording while harvesting its names.
              bool saved = strict;
              strict = false;
              auto ex = module_exports(*me);
              strict = saved;
              for (auto& [pn, prev] : saved_penv) {
                if (prev) modenv[pn] = std::move(*prev); else modenv.erase(pn);
              }
              for (auto& [k, v] : ex) v = generic_var();
              functor_env[*mb->binding.name.txt] = std::move(ex);
            } else {
              // `module MP = Long.Path`: remember the alias so displayed type
              // paths keep the alias (ocamlc prints `MP.t`, not `Long.Path.t`).
              if (auto* pi = std::get_if<Pmod_ident>(&me->desc)) {
                std::string tgt = lid_full(pi->id.txt);
                // Only alias an EXTERNAL target (a cmi module like Gc.Memprof).  A
                // local target (Std2.M) can be accessed both directly and via the
                // alias in the same file, and ocamlc keeps each occurrence's own
                // path -- a uniform rewrite would corrupt the direct occurrences.
                std::string head = tgt.substr(0, tgt.find('.'));
                if (tgt.find('.') != std::string::npos && tgt != *mb->binding.name.txt &&
                    !bound_module_names_.count(head)) {
                  module_aliases_.emplace_back(tgt, *mb->binding.name.txt);
                  // Load the aliased module's record fields (like `open`/`include`),
                  // so a functional update `{ M.rec with field }` can recover the
                  // overridden field's type.  The alias display rewrite (above) maps
                  // the loaded `Long.Path.` prefix back to the alias.
                  load_module_record_fields(pi->id.txt);
                }
              }
              func_bind_name_ = *mb->binding.name.txt;
              modenv[*mb->binding.name.txt] = module_exports(mb->binding.expr);
              func_bind_name_.clear();
            }
          }
        } else if (auto* rm = std::get_if<Pstr_recmodule>(&it.desc)) {
          check_recmodule_cyclic_types(*rm);  // syntactic; safe under the recursion
          // Visit each recursive-module body so its expressions are inferred and
          // its format-string literals collected (a `module rec` body's Printf
          // would otherwise be left as a raw string -> runtime crash).  Names are
          // pre-registered for sibling references; errors are suppressed (the
          // dummies make a sound check impossible without applying the recursion).
          for (auto& b : rm->bindings)
            if (b.name.txt)
              modenv.emplace(*b.name.txt, std::unordered_map<std::string, TypePtr>{});
          for (auto& b : rm->bindings) {
            bool saved = strict; strict = false;
            // Record-completeness is a purely local, self-guarding check (it fires
            // only when every provided label belongs to the resolved record), so it
            // stays sound under the recursion even though full inference does not.
            // Re-enable JUST that check for the strict checker's body visit.
            bool savedrm = recmod_body_; recmod_body_ = saved;
            auto ex = module_exports(b.expr);
            recmod_body_ = savedrm;
            strict = saved;
            if (b.name.txt) modenv[*b.name.txt] = std::move(ex);
          }
        } else if (auto* in = std::get_if<Pstr_include>(&it.desc)) {
          for (auto& [k, v] : module_exports(in->expr)) venv.back()[k] = v;
          if (auto* pi = std::get_if<Pmod_ident>(&in->expr.desc))  // include M -> M's submodules
            for (auto& s : module_submodule_names(pi->id.txt)) opened_submodules_.insert(s);
        } else if (auto* pr = std::get_if<Pstr_primitive>(&it.desc)) {
          if (pr->prim.type) {  // external f : t = "..." binds f : t
            std::unordered_map<std::string, TypePtr> vars;
            // An external's type annotation is fully universally quantified
            // (`'a array -> int -> 'a`), so GENERALIZE it -- else every use of
            // the primitive shares one type variable and a single monomorphic use
            // poisons the rest (array.ml's `unsafe_get` collapsed to a float array
            // -> Array.iter/map garbage on string arrays).
            eng.enter_level();
            TypePtr ty = from_coretype(*pr->prim.type, vars);
            eng.leave_level();
            eng.generalize(ty);
            eng.finalize_family_heads(ty, /*scheme=*/true);
            venv.back()[pr->prim.name.txt] = ty;
          }
        } else if (auto* mt = std::get_if<Pstr_modtype>(&it.desc)) {
          if (mt->type)  // record a signature module type's value names for unpacks
            if (auto* sg = std::get_if<Pmty_signature>(&mt->type->desc)) {
              collect_sig_values(sg->items, modtype_env[mt->name.txt]);
              modtype_sig_asts_[mt->name.txt] = &sg->items;
            }
        }
      } catch (const I::TypeError&) {
      }
    }
  }
};

}  // namespace

// Register variant constructors from type decls, recursing into local module
// structures (a flat ctor namespace — best-effort, so `open M; A` resolves).
// Register ONLY the variant/record TYPE declarations of a functor body (so the
// dump's GADT exhaustiveness marker knows the body's `type 'a wit = V1 : ..`),
// descending into nested plain-module structures.  Deliberately does NOT touch
// typext/exception ctors: those would enter global ctor resolution and, in a
// non-strict pass, shadow the correct through-the-application resolution
// (msg.ml's `C : D.t tag` must stay abstract in the body but resolve to
// `string tag` through `Define(struct type t = string ..)`).
static void register_functor_body_types(Checker& ck, const ast::Structure& s) {
  for (auto& it : s) {
    if (auto* ty = std::get_if<Pstr_type>(&it.desc)) {
      for (auto& d : ty->decls) ck.register_type_decl(d);
      for (auto& d : ty->decls) ck.register_record_decl(d);
    } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
      const ModuleExpr* me = &mb->binding.expr;
      while (auto* mc = std::get_if<Pmod_constraint>(&me->desc)) me = mc->me.get();
      if (auto* ms = std::get_if<Pmod_structure>(&me->desc))
        register_functor_body_types(ck, ms->items);
    }
  }
}

static void register_types_rec(Checker& ck, const ast::Structure& s) {
  for (auto& it : s) {
    if (auto* ty = std::get_if<Pstr_type>(&it.desc)) {
      // two passes so a mutually-recursive group's aliases are all registered
      // before any record fields/ctors that reference them are built.
      for (auto& d : ty->decls) ck.register_type_decl(d);
      for (auto& d : ty->decls) ck.register_record_decl(d);
    } else if (auto* ex = std::get_if<Pstr_exception>(&it.desc))
      ck.register_exception(ex->exn.ctor);
    else if (auto* tx = std::get_if<Pstr_typext>(&it.desc))
      ck.register_typext(tx->ext);
    else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
      const ModuleExpr* me = &mb->binding.expr;
      while (auto* mc = std::get_if<Pmod_constraint>(&me->desc)) me = mc->me.get();
      if (auto* ms = std::get_if<Pmod_structure>(&me->desc)) {
        std::string saved = ck.mod_prefix_;
        if (mb->binding.name.txt) ck.mod_prefix_ += *mb->binding.name.txt + ".";
        register_types_rec(ck, ms->items);
        ck.mod_prefix_ = saved;
      } else if (!ck.strict && std::holds_alternative<Pmod_functor>(me->desc)) {
        // A functor body's own type declarations (`module F(X:S) = struct type
        // 'a wit = V1 : .. end`).  Registered ONLY in the non-strict (dump/kind)
        // passes so the exhaustiveness marker for a body `function` sees its GADT
        // -- the strict reject pass must NOT gain the body's ctors globally (they
        // depend on the unapplied argument and could mis-resolve a bare ctor).
        // No name prefix: the body references its own types unqualified.
        const ModuleExpr* b = me;
        while (auto* mf = std::get_if<Pmod_functor>(&b->desc)) b = mf->body.get();
        while (auto* mc = std::get_if<Pmod_constraint>(&b->desc)) b = mc->me.get();
        if (auto* fs = std::get_if<Pmod_structure>(&b->desc))
          register_functor_body_types(ck, fs->items);
      }
    } else if (auto* rm = std::get_if<Pstr_recmodule>(&it.desc)) {
      // `module rec Typ : sig .. end = struct type 'a typ = Int of .. end`:
      // register the struct bodies' types/ctors like plain modules, so
      // `open Typ; Int TypEq.refl` resolves (fstclassmod).
      for (auto& b : rm->bindings) {
        const ModuleExpr* me2 = &b.expr;
        while (auto* mc = std::get_if<Pmod_constraint>(&me2->desc)) me2 = mc->me.get();
        if (auto* ms2 = std::get_if<Pmod_structure>(&me2->desc)) {
          std::string saved = ck.mod_prefix_;
          if (b.name.txt) ck.mod_prefix_ += *b.name.txt + ".";
          register_types_rec(ck, ms2->items);
          ck.mod_prefix_ = saved;
        }
      }
    }
  }
}

// Shared setup: register constructors, then run best-effort inference over the
// structure (populating ck.match_partial and ck.errors as it traverses).
static void run_checker(Checker& ck, const ast::Structure& s) {
  // unify's lenient cross-kind expansion of folded cmi abbreviations
  // (Arg.anon_fun vs an arrow); consulted by the lenient pass only.
  ck.eng.abbrev_resolver = [&ck](const std::string& p, const std::vector<I::TypePtr>& as) {
    return ck.resolve_abbrev_expansion(p, as);
  };
  ck.register_predef_ctors();
  ck.register_stdlib_ctors();
  ck.collect_bound_modules(s);  // pre-collect bound module names (before type checks)
  // A TOP-LEVEL `module Stdlib` shadows the default-open Stdlib: real-Stdlib
  // types that need requalifying then print `Stdlib/2.` (set before
  // register_types_rec, whose decl registration consumes it).
  for (auto& it : s)
    if (auto* mb = std::get_if<Pstr_module>(&it.desc))
      if (mb->binding.name.txt && *mb->binding.name.txt == "Stdlib")
        ck.user_stdlib_module_ = true;
  register_types_rec(ck, s);
  ck.load_open_record_fields(s);
  ck.finalize_fields();
  ck.check_cyclic_aliases();
  ck.check_dup_modtypes_struct(s);
  ck.check_object_overrides(s);
  ck.process_items(s);
}

std::unordered_map<const ast::Expression*, bool> infer_match_partiality(
    const ast::Structure& s) {
  Checker ck;
  run_checker(ck, s);
  return std::move(ck.match_partial);
}

DumpAux infer_dump_aux(const ast::Structure& s) {
  Checker ck;
  ck.record_fmt_lits_ = true;  // collect format literals for the dump (additive)
  run_checker(ck, s);
  DumpAux out;
  out.match_partial = std::move(ck.match_partial);
  out.function_cases_partial = std::move(ck.function_cases_partial);
  out.param_partial = std::move(ck.param_partial);
  out.apply_plans = std::move(ck.apply_plans);
  out.flatten_construct = std::move(ck.flatten_construct);
  out.construct_any_arity = std::move(ck.construct_any_arity);
  out.type_any_arity = std::move(ck.type_any_arity);
  out.record_fields = std::move(ck.record_fields);
  out.record_reprs = std::move(ck.record_reprs);
  out.format_lits = std::move(ck.fmt_lits_);
  out.iarray_lits = std::move(ck.iarray_lits_);
  out.eta_erasures = std::move(ck.eta_erasures_);
  return out;
}

// The Lambda value_kind of an inferred type, as -dlambda spells it.
static std::string kind_str(const TypePtr& t0, const std::set<std::string>& imm) {
  TypePtr t = I::Engine::repr(t0);
  // functions and tuples are always boxed (Typeopt: Paddrarray, lazy Shortcut)
  if (t->kind == I::Type::Kind::Arrow || t->kind == I::Type::Kind::Tuple) return "addr";
  if (t->kind != I::Type::Kind::Constr) return "";
  auto d = t->path.rfind('.');
  std::string b = d == std::string::npos ? t->path : t->path.substr(d + 1);
  if (b == "int" || b == "char" || b == "bool" || b == "unit")
    return "int";  // immediates (unit is the immediate 0)
  if (imm.count(t->path)) return "int";  // all-constant local variant
  if (b == "float") return "float";
  if (b == "int32") return "int32";
  if (b == "int64") return "int64";
  if (b == "nativeint") return "nativeint";
  if (b == "string") return "string";  // not a value kind, but drives string compares
  // A known boxed type (record/block-variant/string/...): not a value kind, but
  // an `addr` array element (vs a type variable, which is `gen`).
  return "addr";
}

// If `t0` is an array type, sets `out` to its element kind_str ("" for a generic
// element) and returns true; otherwise returns false.
static bool array_elem_str(const TypePtr& t0, const std::set<std::string>& imm, std::string& out) {
  TypePtr t = I::Engine::repr(t0);
  if (t->kind != I::Type::Kind::Constr || t->args.empty()) return false;
  auto d = t->path.rfind('.');
  std::string b = d == std::string::npos ? t->path : t->path.substr(d + 1);
  if (b != "array" && b != "iarray") return false;
  out = kind_str(t->args[0], imm);
  return true;
}

ValueKinds infer_value_kinds(const ast::Structure& s) {
  Checker ck;
  ck.record_kinds_ = true;
  run_checker(ck, s);
  ck.resolve_pending_fields();  // re-resolve ambiguous field reads with final types
  ValueKinds vk;
  for (auto& [p, t] : ck.rec_pat_) {
    vk.pat[p] = kind_str(t, ck.immediate_types_);
    // A constructor pattern whose type resolved to a module-qualified variant:
    // record the path so the back end can register that type's constructors.
    if (std::holds_alternative<ast::Ppat_construct>(p->desc)) {
      TypePtr r = I::Engine::repr(t);
      if (r->kind == I::Type::Kind::Constr && r->path.find('.') != std::string::npos)
        vk.pat_constr[p] = r->path;
    }
    // A record pattern's matched-value type (resolved by unify with the
    // scrutinee): lets the back end disambiguate an ambiguous field by type.
    if (std::holds_alternative<ast::Ppat_record>(p->desc)) {
      TypePtr r = I::Engine::repr(t);
      if (r->kind == I::Type::Kind::Constr && !r->path.empty())
        vk.pat_record_type[p] = r->path;
    }
  }
  for (auto& [f, t] : ck.rec_ret_) vk.fn_ret[f] = kind_str(t, ck.immediate_types_);
  for (auto& [e, t] : ck.rec_expr_) {
    vk.expr[e] = kind_str(t, ck.immediate_types_);
    std::string ek;
    if (array_elem_str(t, ck.immediate_types_, ek)) vk.array_elem[e] = ek;  // "" = gen element
    TypePtr r = I::Engine::repr(t);
    if (r->kind == I::Type::Kind::Constr && r->path.find('.') != std::string::npos)
      vk.expr_constr[e] = r->path;  // module-qualified type, e.g. "Gc.stat"
  }
  for (auto& [e, fr] : ck.field_resolved_)
    vk.field_resolved[e] = {std::get<0>(fr), std::get<1>(fr), std::get<2>(fr)};
  vk.format_lits = std::move(ck.fmt_lits_);
  vk.optional_erasures = std::move(ck.erasures_);
  return vk;
}

// ---------------------------------------------------------------------------
// Recursive-value check ("let rec" RHS validity) -- a faithful parsetree port
// of typing/value_rec_check.ml.  A `let rec x = e` is rejected when e would need
// x's value before it is built (e.g. `let rec x = x + 1`, `match`ing the rec var).
//
// SOUNDNESS DISCIPLINE (to never raise reject_parity above 0): every rule assigns
// each variable a usage mode <= OCaml's, and classifies an expression Dynamic
// only where OCaml certainly does.  Unhandled forms contribute the empty
// environment (mode Ignore) and classify Static -- both under-approximations.
// So we reject a strict SUBSET of what OCaml rejects: no false rejections.
namespace valrec {

// Ignore < Delay < Guard < Return < Dereference  (rank = the enum value).
enum Mode { Ignore = 0, Delay = 1, Guard = 2, Return = 3, Dereference = 4 };

inline Mode mode_join(Mode a, Mode b) { return a >= b ? a : b; }
// compose m' m  (typing/value_rec_check.ml Mode.compose).
inline Mode mode_compose(Mode mp, Mode m) {
  if (mp == Ignore || m == Ignore) return Ignore;
  if (mp == Dereference) return Dereference;
  if (mp == Delay) return Delay;
  if (mp == Guard) return m == Return ? Guard : m;   // m in {Deref,Guard,Delay}
  return m;                                           // mp == Return
}

using Env = std::unordered_map<std::string, Mode>;
inline void env_join_into(Env& dst, const Env& src) {
  for (auto& [k, v] : src) { auto& d = dst[k]; d = mode_join(d, v); }
}
inline Env env_compose(Mode m, const Env& e) {
  Env out;
  for (auto& [k, v] : e) out[k] = mode_compose(m, v);
  return out;
}

// Names a pattern binds (for shadowing/removal); the rec group's idlist too.
void pat_names(const Pattern& p, std::vector<std::string>& out) {
  if (auto* v = std::get_if<Ppat_var>(&p.desc)) out.push_back(v->name.txt);
  else if (auto* a = std::get_if<Ppat_alias>(&p.desc)) { pat_names(*a->p, out); out.push_back(a->name.txt); }
  else if (auto* t = std::get_if<Ppat_tuple>(&p.desc)) { for (auto& e : t->elems) pat_names(*e, out); }
  else if (auto* a = std::get_if<Ppat_array>(&p.desc)) { for (auto& e : a->elems) pat_names(*e, out); }
  else if (auto* c = std::get_if<Ppat_construct>(&p.desc)) { if (c->arg) pat_names(**c->arg, out); }
  else if (auto* v = std::get_if<Ppat_variant>(&p.desc)) { if (v->arg) pat_names(**v->arg, out); }
  else if (auto* r = std::get_if<Ppat_record>(&p.desc)) { for (auto& f : r->fields) pat_names(*f.second, out); }
  else if (auto* o = std::get_if<Ppat_or>(&p.desc)) { pat_names(*o->l, out); }  // both arms bind the same
  else if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) pat_names(*c->p, out);
  else if (auto* l = std::get_if<Ppat_lazy>(&p.desc)) pat_names(*l->p, out);
  else if (auto* op = std::get_if<Ppat_open>(&p.desc)) pat_names(*op->p, out);
  else if (auto* ex = std::get_if<Ppat_exception>(&p.desc)) pat_names(*ex->p, out);
  else if (auto* ef = std::get_if<Ppat_effect>(&p.desc)) { pat_names(*ef->eff, out); pat_names(*ef->cont, out); }
}
// A destructuring pattern places its scrutinee in Dereference (else Guard).
// Unknown patterns default to NON-destructuring (Guard) -- the safe under-mode.
bool is_destructuring(const Pattern& p) {
  if (std::holds_alternative<Ppat_constant>(p.desc) ||
      std::holds_alternative<Ppat_interval>(p.desc) ||
      std::holds_alternative<Ppat_tuple>(p.desc) ||
      std::holds_alternative<Ppat_construct>(p.desc) ||
      std::holds_alternative<Ppat_variant>(p.desc) ||
      std::holds_alternative<Ppat_record>(p.desc) ||
      std::holds_alternative<Ppat_array>(p.desc) ||
      std::holds_alternative<Ppat_lazy>(p.desc)) return true;
  if (auto* a = std::get_if<Ppat_alias>(&p.desc)) return is_destructuring(*a->p);
  if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) return is_destructuring(*c->p);
  if (auto* o = std::get_if<Ppat_open>(&p.desc)) return is_destructuring(*o->p);
  if (auto* o = std::get_if<Ppat_or>(&p.desc)) return is_destructuring(*o->l) || is_destructuring(*o->r);
  return false;  // any / var / exception / unpack / unknown
}

enum SD { Static, Dynamic };

// `ref e` (the Stdlib primitive): the argument is only Guarded, and the result
// has a statically-known size (Static).  Matching by name is a safe
// under-approximation -- a shadowing `ref` only makes us reject LESS.
inline bool is_ref_apply(const Pexp_apply& a) {
  if (a.args.size() != 1) return false;
  auto* id = std::get_if<Pexp_ident>(&a.fn->desc);
  if (!id) return false;
  auto* li = std::get_if<Lident>(&id->id.txt.v);
  return li && li->name == "ref";
}

struct RecCheck {
  // classify-env: simple let-bound var -> its size (Static/Dynamic).  A var not
  // here classifies Dynamic (OCaml) -- but to stay sound against our own gaps we
  // only trust a recorded Static; an absent var is treated Static (under).
  // (See classify_ident.)
  std::unordered_map<std::string, SD> csize;

  // pattern mode: how the bound value of `pat` is used, given env of its body.
  Mode pattern_mode(const Pattern& pat, const Env& env) {
    Mode m = is_destructuring(pat) ? Dereference : Guard;
    std::vector<std::string> ns; pat_names(pat, ns);
    for (auto& n : ns) { auto it = env.find(n); if (it != env.end()) m = mode_join(m, it->second); }
    return m;
  }

  // --- classify (static or dynamic size) ---
  SD classify(const Expression& e) {
    if (auto* l = std::get_if<Pexp_let>(&e.desc)) {
      auto saved = csize;
      for (auto& b : l->bindings)
        if (auto* v = std::get_if<Ppat_var>(&b.pat.desc)) csize[v->name.txt] = classify(*b.expr);
      SD r = classify(*l->body);
      csize = std::move(saved);
      return r;
    }
    if (auto* id = std::get_if<Pexp_ident>(&e.desc)) {
      if (auto* li = std::get_if<Lident>(&id->id.txt.v)) {
        auto it = csize.find(li->name);
        if (it != csize.end()) return it->second;
      }
      return Static;  // not-found: OCaml says Dynamic, but Static is the safe under-classify
    }
    if (auto* s = std::get_if<Pexp_sequence>(&e.desc)) return classify(*s->e2);
    if (auto* c = std::get_if<Pexp_constraint>(&e.desc)) return classify(*c->e);
    if (auto* c = std::get_if<Pexp_coerce>(&e.desc)) return classify(*c->e);
    if (auto* nt = std::get_if<Pexp_newtype>(&e.desc)) return classify(*nt->body);
    if (auto* p = std::get_if<Pexp_poly>(&e.desc)) return classify(*p->e);
    if (auto* si = std::get_if<Pexp_struct_item>(&e.desc)) return classify(*si->body);
    // An application is Dynamic only when fully positional: a `ref e` is Static,
    // and a partial/labelled application may be a closure (Static) -- so any
    // labelled-argument application is classified Static (the safe under-mode).
    if (auto* a = std::get_if<Pexp_apply>(&e.desc)) {
      if (is_ref_apply(*a)) return Static;
      for (auto& [lbl, _] : a->args)
        if (!std::holds_alternative<Nolabel>(lbl)) return Static;
      return Dynamic;
    }
    // Certainly-Dynamic forms (must be a SUBSET of OCaml's, else we over-reject).
    if (std::holds_alternative<Pexp_ifthenelse>(e.desc) ||
        std::holds_alternative<Pexp_match>(e.desc) ||
        std::holds_alternative<Pexp_try>(e.desc) ||
        std::holds_alternative<Pexp_field>(e.desc) ||
        std::holds_alternative<Pexp_send>(e.desc) ||
        std::holds_alternative<Pexp_assert>(e.desc) ||
        std::holds_alternative<Pexp_new>(e.desc) ||
        std::holds_alternative<Pexp_letop>(e.desc) ||
        std::holds_alternative<Pexp_object>(e.desc) ||
        std::holds_alternative<Pexp_override>(e.desc))
      return Dynamic;
    return Static;  // construct/record/tuple/function/array/lazy/.../unknown
  }

  // --- usage-mode judgment: expression -> (mode -> Env) ---
  Env J(const Expression& e, Mode m) {
    if (m == Ignore) return {};
    if (auto* id = std::get_if<Pexp_ident>(&e.desc)) {
      if (auto* li = std::get_if<Lident>(&id->id.txt.v)) return {{li->name, m}};
      return {};  // qualified path: head is a module, never a rec value var
    }
    if (std::holds_alternative<Pexp_constant>(e.desc)) return {};
    if (auto* l = std::get_if<Pexp_let>(&e.desc)) return let_judg(*l, m);
    if (auto* a = std::get_if<Pexp_apply>(&e.desc)) {
      if (is_ref_apply(*a)) return J(*a->args[0].second, mode_compose(m, Guard));
      bool positional = true;
      for (auto& [lbl, _] : a->args) if (!std::holds_alternative<Nolabel>(lbl)) positional = false;
      Mode fmode = a->args.empty() ? Guard : (positional ? Dereference : Guard);
      Mode amode = positional ? Dereference : Guard;  // omitted/labelled -> safe under (Guard)
      Env env = J(*a->fn, mode_compose(m, fmode));
      for (auto& [_, arg] : a->args) env_join_into(env, J(*arg, mode_compose(m, amode)));
      return env;
    }
    if (auto* t = std::get_if<Pexp_tuple>(&e.desc)) {
      Env env; for (auto& el : t->elems) env_join_into(env, J(*el, mode_compose(m, Guard)));
      return env;
    }
    if (auto* c = std::get_if<Pexp_construct>(&e.desc))
      return c->arg ? J(**c->arg, mode_compose(m, Guard)) : Env{};
    if (auto* v = std::get_if<Pexp_variant>(&e.desc))
      return v->arg ? J(**v->arg, mode_compose(m, Guard)) : Env{};
    if (auto* r = std::get_if<Pexp_record>(&e.desc)) {
      Env env;
      for (auto& [_, ev] : r->fields) env_join_into(env, J(*ev, mode_compose(m, Guard)));
      if (r->base) env_join_into(env, J(**r->base, mode_compose(m, Dereference)));
      return env;
    }
    if (auto* f = std::get_if<Pexp_field>(&e.desc)) return J(*f->e, mode_compose(m, Dereference));
    if (auto* s = std::get_if<Pexp_setfield>(&e.desc)) {
      Env env = J(*s->obj, mode_compose(m, Dereference));
      env_join_into(env, J(*s->value, mode_compose(m, Dereference)));
      return env;
    }
    if (auto* i = std::get_if<Pexp_ifthenelse>(&e.desc)) {
      Env env = J(*i->cond, mode_compose(m, Dereference));
      env_join_into(env, J(*i->then_, m));
      if (i->else_) env_join_into(env, J(**i->else_, m));
      return env;
    }
    if (auto* s = std::get_if<Pexp_sequence>(&e.desc)) {
      Env env = J(*s->e1, mode_compose(m, Guard));
      env_join_into(env, J(*s->e2, m));
      return env;
    }
    if (auto* w = std::get_if<Pexp_while>(&e.desc)) {
      Env env = J(*w->cond, mode_compose(m, Dereference));
      env_join_into(env, J(*w->body, mode_compose(m, Guard)));
      return env;
    }
    if (auto* fo = std::get_if<Pexp_for>(&e.desc)) {
      Env env = J(*fo->lo, mode_compose(m, Dereference));
      env_join_into(env, J(*fo->hi, mode_compose(m, Dereference)));
      Env body = J(*fo->body, mode_compose(m, Guard));
      std::vector<std::string> ns; pat_names(fo->var, ns);
      for (auto& n : ns) body.erase(n);
      env_join_into(env, body);
      return env;
    }
    if (auto* mt = std::get_if<Pexp_match>(&e.desc)) return match_judg(*mt->e, mt->cases, m);
    if (auto* tr = std::get_if<Pexp_try>(&e.desc)) {
      Env env = J(*tr->e, m);
      for (auto& c : tr->cases) { Mode sm; env_join_into(env, case_env(c, m, sm)); }
      return env;
    }
    if (auto* f = std::get_if<Pexp_function>(&e.desc)) return function_judg(*f, m);
    if (auto* lz = std::get_if<Pexp_lazy>(&e.desc)) return J(*lz->e, mode_compose(m, Delay));
    if (auto* as = std::get_if<Pexp_assert>(&e.desc)) return J(*as->e, mode_compose(m, Dereference));
    if (auto* sd = std::get_if<Pexp_send>(&e.desc)) return J(*sd->obj, mode_compose(m, Dereference));
    if (auto* c = std::get_if<Pexp_constraint>(&e.desc)) return J(*c->e, m);
    if (auto* c = std::get_if<Pexp_coerce>(&e.desc)) return J(*c->e, m);
    if (auto* nt = std::get_if<Pexp_newtype>(&e.desc)) return J(*nt->body, m);
    if (auto* p = std::get_if<Pexp_poly>(&e.desc)) return J(*p->e, m);
    if (auto* ar = std::get_if<Pexp_array>(&e.desc)) {
      Env env; for (auto& el : ar->elems) env_join_into(env, J(*el, mode_compose(m, Guard)));
      return env;
    }
    if (auto* si = std::get_if<Pexp_struct_item>(&e.desc)) return J(*si->body, m);
    return {};  // letop / object / new / override / pack / extension / ... -> Ignore (safe under)
  }

  // case body+guard env at mode m, with the pattern's bound names removed; sets
  // scrut_mode = the mode the matched value (scrutinee) is placed in.
  Env case_env(const Case& c, Mode m, Mode& scrut_mode) {
    Env env = J(*c.rhs, m);
    if (c.guard) env_join_into(env, J(**c.guard, mode_compose(m, Dereference)));
    scrut_mode = mode_compose(m, pattern_mode(c.lhs, env));
    std::vector<std::string> ns; pat_names(c.lhs, ns);
    for (auto& n : ns) env.erase(n);
    return env;
  }

  Env match_judg(const Expression& scrut, const std::vector<Case>& cases, Mode m) {
    Env env; Mode sjoin = Ignore;
    for (auto& c : cases) { Mode sm; env_join_into(env, case_env(c, m, sm)); sjoin = mode_join(sjoin, sm); }
    env_join_into(env, J(scrut, sjoin));
    return env;
  }

  Env function_judg(const Pexp_function& f, Mode m) {
    Mode inner = mode_compose(m, Delay);
    Env env;
    if (auto* fb = std::get_if<Pfunction_body>(&f.body->v)) env = J(*fb->e, inner);
    else {
      auto& fc = std::get<Pfunction_cases>(f.body->v);
      for (auto& c : fc.cases) { Mode sm; env_join_into(env, case_env(c, inner, sm)); }
    }
    std::vector<std::string> ns;
    for (auto& param : f.params)
      if (auto* pv = std::get_if<Pparam_val>(&param.desc)) {
        if (pv->default_) env_join_into(env, J(**pv->default_, inner));
        pat_names(pv->pat, ns);
      }
    for (auto& n : ns) env.erase(n);
    return env;
  }

  Env let_judg(const Pexp_let& l, Mode m) {
    std::vector<std::string> bound;
    for (auto& b : l.bindings) pat_names(b.pat, bound);
    Env body_env = J(*l.body, m);
    Env out = body_env;
    for (auto& n : bound) out.erase(n);
    if (l.rf == RecFlag::Nonrecursive) {
      for (auto& b : l.bindings) {
        Mode mb = pattern_mode(b.pat, body_env);
        Env rhs = J(*b.expr, mode_compose(m, mb));
        std::vector<std::string> ns; pat_names(b.pat, ns);
        for (auto& n : ns) rhs.erase(n);
        env_join_into(out, rhs);
      }
      return out;
    }
    // Recursive: least-fixpoint transitive closure (value_rec_check value_bindings).
    size_t n = l.bindings.size();
    std::vector<Env> g(n);                       // immediate deps (binders removed)
    std::vector<std::vector<Mode>> mdef(n, std::vector<Mode>(n, Ignore));
    for (size_t i = 0; i < n; ++i) {
      Mode mb = pattern_mode(l.bindings[i].pat, body_env);
      Env rhs = J(*l.bindings[i].expr, mode_compose(m, mb));   // siblings in scope
      for (size_t j = 0; j < n; ++j) mdef[i][j] = pattern_mode(l.bindings[j].pat, rhs);
      for (auto& bn : bound) rhs.erase(bn);
      g[i] = std::move(rhs);
    }
    std::vector<Env> gp = g;
    for (size_t iter = 0; iter < n * 5 + 8; ++iter) {
      std::vector<Env> nxt(n);
      bool changed = false;
      for (size_t i = 0; i < n; ++i) {
        Env e = g[i];
        for (size_t j = 0; j < n; ++j) env_join_into(e, env_compose(mdef[i][j], gp[j]));
        if (e != gp[i]) changed = true;
        nxt[i] = std::move(e);
      }
      gp = std::move(nxt);
      if (!changed) break;
    }
    for (auto& e : gp) env_join_into(out, e);
    return out;
  }

  // is_valid_recursive_expression: false => the binding RHS is an illegal let-rec.
  bool valid(const std::vector<std::string>& idlist, const Expression& rhs) {
    if (std::holds_alternative<Pexp_function>(rhs.desc)) return true;  // fast path
    csize.clear();
    SD sd = classify(rhs);
    Env env = J(rhs, Return);
    Mode threshold = sd == Static ? Guard : Ignore;  // reject if any id mode > threshold
    for (auto& id : idlist) {
      auto it = env.find(id);
      if (it != env.end() && it->second > threshold) return false;
    }
    return true;
  }
};

// Walk the structure; check every `let rec` group (top-level and nested).
struct Walk {
  std::vector<std::string>* errs;
  RecCheck rc;
  void check_group(const std::vector<ValueBinding>& bindings) {
    std::vector<std::string> idlist;
    for (auto& b : bindings) pat_names(b.pat, idlist);
    for (auto& b : bindings)
      if (!rc.valid(idlist, *b.expr)) {
        errs->push_back("This kind of expression is not allowed as "
                        "right-hand side of `let rec'");
        break;  // one error per group is enough for the accept/reject gate
      }
  }
  void w_expr(const Expression& e) {
    if (auto* l = std::get_if<Pexp_let>(&e.desc)) {
      if (l->rf == RecFlag::Recursive) check_group(l->bindings);
      for (auto& b : l->bindings) w_expr(*b.expr);
      w_expr(*l->body);
      return;
    }
    // Recurse into all sub-expressions to find nested `let rec`s.
    visit_subexprs(e, [&](const Expression& s) { w_expr(s); });
  }
  template <class F> void visit_subexprs(const Expression& e, F f);
  void w_item(const StructureItem& it);
};

template <class F> void Walk::visit_subexprs(const Expression& e, F f) {
  auto go = [&](const ExprBox& b) { f(*b); };
  auto opt = [&](const std::optional<ExprBox>& b) { if (b) f(**b); };
  if (auto* a = std::get_if<Pexp_apply>(&e.desc)) { go(a->fn); for (auto& [_, x] : a->args) go(x); }
  else if (auto* t = std::get_if<Pexp_tuple>(&e.desc)) { for (auto& x : t->elems) go(x); }
  else if (auto* i = std::get_if<Pexp_ifthenelse>(&e.desc)) { go(i->cond); go(i->then_); opt(i->else_); }
  else if (auto* c = std::get_if<Pexp_construct>(&e.desc)) { if (c->arg) f(**c->arg); }
  else if (auto* m = std::get_if<Pexp_match>(&e.desc)) { go(m->e); for (auto& c : m->cases) { f(*c.rhs); if (c.guard) f(**c.guard); } }
  else if (auto* tr = std::get_if<Pexp_try>(&e.desc)) { go(tr->e); for (auto& c : tr->cases) { f(*c.rhs); if (c.guard) f(**c.guard); } }
  else if (auto* s = std::get_if<Pexp_sequence>(&e.desc)) { go(s->e1); go(s->e2); }
  else if (auto* c = std::get_if<Pexp_constraint>(&e.desc)) go(c->e);
  else if (auto* fld = std::get_if<Pexp_field>(&e.desc)) go(fld->e);
  else if (auto* r = std::get_if<Pexp_record>(&e.desc)) { for (auto& [_, x] : r->fields) go(x); if (r->base) f(**r->base); }
  else if (auto* as = std::get_if<Pexp_assert>(&e.desc)) go(as->e);
  else if (auto* lz = std::get_if<Pexp_lazy>(&e.desc)) go(lz->e);
  else if (auto* w = std::get_if<Pexp_while>(&e.desc)) { go(w->cond); go(w->body); }
  else if (auto* fo = std::get_if<Pexp_for>(&e.desc)) { go(fo->lo); go(fo->hi); go(fo->body); }
  else if (auto* v = std::get_if<Pexp_variant>(&e.desc)) { if (v->arg) f(**v->arg); }
  else if (auto* nt = std::get_if<Pexp_newtype>(&e.desc)) go(nt->body);
  else if (auto* si = std::get_if<Pexp_struct_item>(&e.desc)) { w_item(*si->item); go(si->body); }
  else if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) { go(sf->obj); go(sf->value); }
  else if (auto* co = std::get_if<Pexp_coerce>(&e.desc)) go(co->e);
  else if (auto* sd = std::get_if<Pexp_send>(&e.desc)) go(sd->obj);
  else if (auto* fn = std::get_if<Pexp_function>(&e.desc)) {
    for (auto& p : fn->params) if (auto* pv = std::get_if<Pparam_val>(&p.desc)) if (pv->default_) f(**pv->default_);
    if (auto* fb = std::get_if<Pfunction_body>(&fn->body->v)) go(fb->e);
    else { auto& fc = std::get<Pfunction_cases>(fn->body->v); for (auto& c : fc.cases) { f(*c.rhs); if (c.guard) f(**c.guard); } }
  }
  else if (auto* lo = std::get_if<Pexp_letop>(&e.desc)) { go(lo->let_.exp); for (auto& a : lo->ands) go(a.exp); go(lo->body); }
  else if (auto* p = std::get_if<Pexp_poly>(&e.desc)) go(p->e);
  else if (auto* ov = std::get_if<Pexp_override>(&e.desc)) { for (auto& [_, x] : ov->fields) go(x); }
  // Pexp_let handled by the caller; ident/constant/pack/object/new/extension/unreachable: no sub-exprs to walk
}

void Walk::w_item(const StructureItem& it) {
  if (auto* v = std::get_if<Pstr_value>(&it.desc)) {
    if (v->rf == RecFlag::Recursive) check_group(v->bindings);
    for (auto& b : v->bindings) w_expr(*b.expr);
  } else if (auto* e = std::get_if<Pstr_eval>(&it.desc)) {
    w_expr(*e->e);
  } else if (auto* m = std::get_if<Pstr_module>(&it.desc)) {
    // descend into a submodule's structure body if present
    if (auto* st = std::get_if<Pmod_structure>(&m->binding.expr.desc))
      for (auto& sit : st->items) w_item(sit);
  }
}

std::vector<std::string> value_rec_errors(const ast::Structure& s) {
  std::vector<std::string> errs;
  Walk w; w.errs = &errs;
  for (auto& it : s) w.w_item(it);
  return errs;
}

}  // namespace valrec

// ---------------------------------------------------------------------------
// Uninterpreted-extension check: an extension node `[%foo]` / `[%%foo]` that
// survives to type-checking (no ppx expanded it) is an error, UNLESS it is one
// of the compiler's built-in extensions.  We flag the first non-built-in
// extension found anywhere in the structure.  SOUND: the allowlist covers every
// extension the oracle accepts in the corpus (verified against reject_parity);
// positions we don't traverse simply go undetected (a safe under-approximation).
namespace extcheck {

inline bool ext_allowed(const std::string& n) {
  // Built-in / typer-interpreted extensions (never an "uninterpreted" error).
  static const std::set<std::string> ok = {
    "extension_constructor", "ocaml.extension_constructor", "atomic.loc",
    "src_pos", "call_pos", "probe", "probe_is_enabled", "ocaml.probe",
  };
  if (ok.count(n)) return true;
  return n.rfind("ocaml.", 0) == 0;  // the compiler's builtin attribute namespace
}

struct Find {
  std::string bad;  // first offending extension name ("" => none); short-circuits
  bool done() const { return !bad.empty(); }
  void hit(const std::string& n) { if (bad.empty() && !ext_allowed(n)) bad = n; }

  void ty(const CoreType& t) {
    if (done()) return;
    if (auto* e = std::get_if<Ptyp_extension>(&t.desc)) { hit(e->name); return; }
    if (auto* a = std::get_if<Ptyp_arrow>(&t.desc)) { ty(*a->dom); ty(*a->cod); }
    else if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) { for (auto& x : tu->elems) ty(*x); }
    else if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) { for (auto& x : c->args) ty(*x); }
    else if (auto* p = std::get_if<Ptyp_poly>(&t.desc)) ty(*p->type);
    else if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) ty(*al->type);
  }
  void pat(const Pattern& p) {
    if (done()) return;
    if (auto* e = std::get_if<Ppat_extension>(&p.desc)) { hit(e->name); return; }
    if (auto* t = std::get_if<Ppat_tuple>(&p.desc)) { for (auto& x : t->elems) pat(*x); }
    else if (auto* a = std::get_if<Ppat_array>(&p.desc)) { for (auto& x : a->elems) pat(*x); }
    else if (auto* c = std::get_if<Ppat_construct>(&p.desc)) { if (c->arg) pat(**c->arg); }
    else if (auto* v = std::get_if<Ppat_variant>(&p.desc)) { if (v->arg) pat(**v->arg); }
    else if (auto* r = std::get_if<Ppat_record>(&p.desc)) { for (auto& f : r->fields) pat(*f.second); }
    else if (auto* o = std::get_if<Ppat_or>(&p.desc)) { pat(*o->l); pat(*o->r); }
    else if (auto* a = std::get_if<Ppat_alias>(&p.desc)) pat(*a->p);
    else if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) { pat(*c->p); ty(*c->t); }
    else if (auto* l = std::get_if<Ppat_lazy>(&p.desc)) pat(*l->p);
    else if (auto* op = std::get_if<Ppat_open>(&p.desc)) pat(*op->p);
    else if (auto* ex = std::get_if<Ppat_exception>(&p.desc)) pat(*ex->p);
  }
  void cse(const Case& c) { if (done()) return; pat(c.lhs); if (c.guard) ex(**c.guard); ex(*c.rhs); }
  void ex(const Expression& e) {
    if (done()) return;
    if (auto* x = std::get_if<Pexp_extension>(&e.desc)) { hit(x->name); return; }
    if (auto* a = std::get_if<Pexp_apply>(&e.desc)) { ex(*a->fn); for (auto& [_, x] : a->args) ex(*x); }
    else if (auto* t = std::get_if<Pexp_tuple>(&e.desc)) { for (auto& x : t->elems) ex(*x); }
    else if (auto* l = std::get_if<Pexp_let>(&e.desc)) { for (auto& b : l->bindings) { pat(b.pat); ex(*b.expr); } ex(*l->body); }
    else if (auto* f = std::get_if<Pexp_function>(&e.desc)) {
      for (auto& pm : f->params) if (auto* pv = std::get_if<Pparam_val>(&pm.desc)) { pat(pv->pat); if (pv->default_) ex(**pv->default_); }
      if (auto* fb = std::get_if<Pfunction_body>(&f->body->v)) ex(*fb->e);
      else for (auto& c : std::get<Pfunction_cases>(f->body->v).cases) cse(c);
    }
    else if (auto* i = std::get_if<Pexp_ifthenelse>(&e.desc)) { ex(*i->cond); ex(*i->then_); if (i->else_) ex(**i->else_); }
    else if (auto* c = std::get_if<Pexp_construct>(&e.desc)) { if (c->arg) ex(**c->arg); }
    else if (auto* m = std::get_if<Pexp_match>(&e.desc)) { ex(*m->e); for (auto& c : m->cases) cse(c); }
    else if (auto* tr = std::get_if<Pexp_try>(&e.desc)) { ex(*tr->e); for (auto& c : tr->cases) cse(c); }
    else if (auto* s = std::get_if<Pexp_sequence>(&e.desc)) { ex(*s->e1); ex(*s->e2); }
    else if (auto* c = std::get_if<Pexp_constraint>(&e.desc)) { ex(*c->e); ty(*c->t); }
    else if (auto* c = std::get_if<Pexp_coerce>(&e.desc)) { ex(*c->e); if (c->from) ty(**c->from); ty(*c->to_); }
    else if (auto* f = std::get_if<Pexp_field>(&e.desc)) ex(*f->e);
    else if (auto* r = std::get_if<Pexp_record>(&e.desc)) { for (auto& [_, x] : r->fields) ex(*x); if (r->base) ex(**r->base); }
    else if (auto* as = std::get_if<Pexp_assert>(&e.desc)) ex(*as->e);
    else if (auto* lz = std::get_if<Pexp_lazy>(&e.desc)) ex(*lz->e);
    else if (auto* w = std::get_if<Pexp_while>(&e.desc)) { ex(*w->cond); ex(*w->body); }
    else if (auto* fo = std::get_if<Pexp_for>(&e.desc)) { pat(fo->var); ex(*fo->lo); ex(*fo->hi); ex(*fo->body); }
    else if (auto* v = std::get_if<Pexp_variant>(&e.desc)) { if (v->arg) ex(**v->arg); }
    else if (auto* nt = std::get_if<Pexp_newtype>(&e.desc)) ex(*nt->body);
    else if (auto* si = std::get_if<Pexp_struct_item>(&e.desc)) { item(*si->item); ex(*si->body); }
    else if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) { ex(*sf->obj); ex(*sf->value); }
    else if (auto* sd = std::get_if<Pexp_send>(&e.desc)) ex(*sd->obj);
    else if (auto* p = std::get_if<Pexp_poly>(&e.desc)) { ex(*p->e); if (p->t) ty(**p->t); }
    else if (auto* ar = std::get_if<Pexp_array>(&e.desc)) { for (auto& x : ar->elems) ex(*x); }
    else if (auto* lo = std::get_if<Pexp_letop>(&e.desc)) { ex(*lo->let_.exp); for (auto& a : lo->ands) ex(*a.exp); ex(*lo->body); }
    else if (auto* ov = std::get_if<Pexp_override>(&e.desc)) { for (auto& [_, x] : ov->fields) ex(*x); }
  }
  void mexp(const ModuleExpr& m) {
    if (done()) return;
    if (auto* e = std::get_if<Pmod_extension>(&m.desc)) { hit(e->name); return; }
    if (auto* st = std::get_if<Pmod_structure>(&m.desc)) for (auto& it : st->items) item(it);
    else if (auto* c = std::get_if<Pmod_constraint>(&m.desc)) mexp(*c->me);
    else if (auto* f = std::get_if<Pmod_functor>(&m.desc)) mexp(*f->body);
  }
  void item(const StructureItem& it) {
    if (done()) return;
    if (auto* e = std::get_if<Pstr_extension>(&it.desc)) { hit(e->name); return; }
    if (auto* v = std::get_if<Pstr_value>(&it.desc)) { for (auto& b : v->bindings) { pat(b.pat); ex(*b.expr); } }
    else if (auto* e = std::get_if<Pstr_eval>(&it.desc)) ex(*e->e);
    else if (auto* m = std::get_if<Pstr_module>(&it.desc)) mexp(m->binding.expr);
    else if (auto* p = std::get_if<Pstr_primitive>(&it.desc)) { if (p->prim.type) ty(*p->prim.type); }
    else if (auto* p = std::get_if<Pstr_val>(&it.desc)) ty(*p->vd.type);
  }
};

std::vector<std::string> errors(const ast::Structure& s) {
  Find f;
  for (auto& it : s) { f.item(it); if (f.done()) break; }
  std::vector<std::string> out;
  if (f.done()) out.push_back("Uninterpreted extension '" + f.bad + "'.");
  return out;
}

}  // namespace extcheck

// ---------------------------------------------------------------------------
// Unbound-module check (post-pass): flag a reference to a module whose head is
// bound nowhere -- not a local module/functor/opened submodule (Checker tracks
// these via the over-inclusive bound_module_names_), and no loadable cmi.  Run
// against the FINAL scope sets, so it flags a SUBSET of what the per-reference
// value check (already sound, reject 0.0%) would -- hence no new false rejects --
// while covering the type / module-type / module-expr / constructor positions the
// per-reference checks don't reach.  Only DOTTED paths (or bare module idents in
// a module position) are module references; a bare lower-case value/type name is
// not.  Deep-unbound submodules (`M.Gone.x`) are not detected (safe under-mode).
struct UnboundWalk {
  Checker* ck;
  std::string bad;
  bool done() const { return !bad.empty(); }
  // A functor-application path (`X.F(Y).t`) has an Lapply node; head extraction
  // is unreliable there and the applied functor may be local -- skip it.
  static bool has_lapply(const Longident& id) {
    if (std::holds_alternative<Lapply>(id.v)) return true;
    if (auto* d = std::get_if<Ldot>(&id.v)) return has_lapply(*d->prefix);
    return false;
  }
  void flag(const Longident& id) {
    if (done() || has_lapply(id)) return;
    if (ck->module_head_unbound(id)) {
      auto c = Checker::mod_components(id);
      if (!c.empty()) bad = c[0];
    }
  }
  void path(const Longident& id) { if (std::holds_alternative<Ldot>(id.v)) flag(id); }

  void ty(const CoreType& t) {
    if (done()) return;
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) { path(c->id.txt); for (auto& a : c->args) ty(*a); }
    else if (auto* a = std::get_if<Ptyp_arrow>(&t.desc)) { ty(*a->dom); ty(*a->cod); }
    else if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) { for (auto& x : tu->elems) ty(*x); }
    else if (auto* p = std::get_if<Ptyp_poly>(&t.desc)) ty(*p->type);
    else if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) ty(*al->type);
  }
  void pat(const Pattern& p) {
    if (done()) return;
    if (auto* c = std::get_if<Ppat_construct>(&p.desc)) { path(c->id.txt); if (c->arg) pat(**c->arg); }
    else if (auto* t = std::get_if<Ppat_tuple>(&p.desc)) { for (auto& x : t->elems) pat(*x); }
    else if (auto* a = std::get_if<Ppat_array>(&p.desc)) { for (auto& x : a->elems) pat(*x); }
    else if (auto* v = std::get_if<Ppat_variant>(&p.desc)) { if (v->arg) pat(**v->arg); }
    else if (auto* r = std::get_if<Ppat_record>(&p.desc)) { for (auto& f : r->fields) pat(*f.second); }
    else if (auto* o = std::get_if<Ppat_or>(&p.desc)) { pat(*o->l); pat(*o->r); }
    else if (auto* a = std::get_if<Ppat_alias>(&p.desc)) pat(*a->p);
    else if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) { pat(*c->p); ty(*c->t); }
    else if (auto* l = std::get_if<Ppat_lazy>(&p.desc)) pat(*l->p);
    else if (auto* op = std::get_if<Ppat_open>(&p.desc)) pat(*op->p);
  }
  void cse(const Case& c) { if (done()) return; pat(c.lhs); if (c.guard) ex(**c.guard); ex(*c.rhs); }
  void ex(const Expression& e) {
    if (done()) return;
    if (auto* id = std::get_if<Pexp_ident>(&e.desc)) path(id->id.txt);
    else if (auto* k = std::get_if<Pexp_construct>(&e.desc)) { path(k->id.txt); if (k->arg) ex(**k->arg); }
    else if (auto* nw = std::get_if<Pexp_new>(&e.desc)) path(nw->id.txt);
    else if (auto* a = std::get_if<Pexp_apply>(&e.desc)) { ex(*a->fn); for (auto& [_, x] : a->args) ex(*x); }
    else if (auto* t = std::get_if<Pexp_tuple>(&e.desc)) { for (auto& x : t->elems) ex(*x); }
    else if (auto* l = std::get_if<Pexp_let>(&e.desc)) { for (auto& b : l->bindings) { pat(b.pat); ex(*b.expr); } ex(*l->body); }
    else if (auto* f = std::get_if<Pexp_function>(&e.desc)) {
      for (auto& pm : f->params) if (auto* pv = std::get_if<Pparam_val>(&pm.desc)) { pat(pv->pat); if (pv->default_) ex(**pv->default_); }
      if (auto* fb = std::get_if<Pfunction_body>(&f->body->v)) ex(*fb->e);
      else for (auto& c : std::get<Pfunction_cases>(f->body->v).cases) cse(c);
    }
    else if (auto* i = std::get_if<Pexp_ifthenelse>(&e.desc)) { ex(*i->cond); ex(*i->then_); if (i->else_) ex(**i->else_); }
    else if (auto* m = std::get_if<Pexp_match>(&e.desc)) { ex(*m->e); for (auto& c : m->cases) cse(c); }
    else if (auto* tr = std::get_if<Pexp_try>(&e.desc)) { ex(*tr->e); for (auto& c : tr->cases) cse(c); }
    else if (auto* s = std::get_if<Pexp_sequence>(&e.desc)) { ex(*s->e1); ex(*s->e2); }
    else if (auto* c = std::get_if<Pexp_constraint>(&e.desc)) { ex(*c->e); ty(*c->t); }
    else if (auto* c = std::get_if<Pexp_coerce>(&e.desc)) { ex(*c->e); if (c->from) ty(**c->from); ty(*c->to_); }
    else if (auto* f = std::get_if<Pexp_field>(&e.desc)) ex(*f->e);
    else if (auto* r = std::get_if<Pexp_record>(&e.desc)) { for (auto& [_, x] : r->fields) ex(*x); if (r->base) ex(**r->base); }
    else if (auto* as = std::get_if<Pexp_assert>(&e.desc)) ex(*as->e);
    else if (auto* lz = std::get_if<Pexp_lazy>(&e.desc)) ex(*lz->e);
    else if (auto* w = std::get_if<Pexp_while>(&e.desc)) { ex(*w->cond); ex(*w->body); }
    else if (auto* fo = std::get_if<Pexp_for>(&e.desc)) { pat(fo->var); ex(*fo->lo); ex(*fo->hi); ex(*fo->body); }
    else if (auto* v = std::get_if<Pexp_variant>(&e.desc)) { if (v->arg) ex(**v->arg); }
    else if (auto* nt = std::get_if<Pexp_newtype>(&e.desc)) ex(*nt->body);
    else if (auto* si = std::get_if<Pexp_struct_item>(&e.desc)) { item(*si->item); ex(*si->body); }
    else if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) { ex(*sf->obj); ex(*sf->value); }
    else if (auto* sd = std::get_if<Pexp_send>(&e.desc)) ex(*sd->obj);
    else if (auto* p = std::get_if<Pexp_poly>(&e.desc)) { ex(*p->e); if (p->t) ty(**p->t); }
    else if (auto* ar = std::get_if<Pexp_array>(&e.desc)) { for (auto& x : ar->elems) ex(*x); }
    else if (auto* pk = std::get_if<Pexp_pack>(&e.desc)) mexp(*pk->me);
    else if (auto* lo = std::get_if<Pexp_letop>(&e.desc)) { ex(*lo->let_.exp); for (auto& a : lo->ands) ex(*a.exp); ex(*lo->body); }
  }
  void mty(const ModuleType& m) {
    if (done()) return;
    // A bare module-type name (`S`) lives in the module-TYPE namespace, not the
    // module namespace -- only a DOTTED `M.S` references a module (head M).
    if (auto* id = std::get_if<Pmty_ident>(&m.desc)) path(id->id.txt);
    else if (auto* al = std::get_if<Pmty_alias>(&m.desc)) flag(al->id.txt);
    // A `sig ... end` body introduces LOCAL module scopes our flat bound-set
    // doesn't model (e.g. `module Spec : ..` then `val f : t Spec.extra`), so we
    // don't descend into it -- a safe recall trade-off.
    else if (std::holds_alternative<Pmty_signature>(m.desc)) { /* skip sig body */ }
    else if (auto* fn = std::get_if<Pmty_functor>(&m.desc)) {
      if (auto* fp = std::get_if<Functor_named>(&fn->param); fp && fp->type) mty(*fp->type);
      mty(*fn->body);
    }
    else if (auto* w = std::get_if<Pmty_with>(&m.desc)) mty(*w->mt);
    else if (auto* to = std::get_if<Pmty_typeof>(&m.desc)) mexp(*to->me);
  }
  void mexp(const ModuleExpr& m) {
    if (done()) return;
    // Only a DOTTED module path (head = a real module) is checkable; a bare
    // module ident is often a local param/alias our bound-set may not capture.
    if (auto* id = std::get_if<Pmod_ident>(&m.desc)) path(id->id.txt);
    else if (auto* st = std::get_if<Pmod_structure>(&m.desc)) for (auto& it : st->items) item(it);
    else if (auto* c = std::get_if<Pmod_constraint>(&m.desc)) { mexp(*c->me); mty(*c->mt); }
    else if (auto* a = std::get_if<Pmod_apply>(&m.desc)) { mexp(*a->f); mexp(*a->arg); }
    else if (auto* fn = std::get_if<Pmod_functor>(&m.desc)) {
      if (auto* fp = std::get_if<Functor_named>(&fn->param); fp && fp->type) mty(*fp->type);
      mexp(*fn->body);
    }
    else if (auto* up = std::get_if<Pmod_unpack>(&m.desc)) ex(*up->e);
  }
  void type_decl(const TypeDeclaration& d) {
    if (d.manifest) ty(**d.manifest);
    for (auto& p : d.params) ty(*p);
    if (auto* v = std::get_if<Ptype_variant>(&d.kind))
      for (auto& c : v->ctors) { if (auto* tu = std::get_if<Pcstr_tuple>(&c.args)) for (auto& a : tu->elems) ty(*a); if (c.res) ty(**c.res); }
    else if (auto* r = std::get_if<Ptype_record>(&d.kind))
      for (auto& fld : r->fields) ty(*fld.type);
    for (auto& tc : d.constraints) { ty(*tc.t1); ty(*tc.t2); }
  }
  void item(const StructureItem& it) {
    if (done()) return;
    if (auto* v = std::get_if<Pstr_value>(&it.desc)) { for (auto& b : v->bindings) { pat(b.pat); ex(*b.expr); } }
    else if (auto* e = std::get_if<Pstr_eval>(&it.desc)) ex(*e->e);
    else if (auto* t = std::get_if<Pstr_type>(&it.desc)) for (auto& d : t->decls) type_decl(d);
    else if (auto* m = std::get_if<Pstr_module>(&it.desc)) mexp(m->binding.expr);
    else if (auto* rm = std::get_if<Pstr_recmodule>(&it.desc)) for (auto& b : rm->bindings) mexp(b.expr);
    else if (auto* mt = std::get_if<Pstr_modtype>(&it.desc)) { if (mt->type) mty(*mt->type); }
    else if (auto* in = std::get_if<Pstr_include>(&it.desc)) mexp(in->expr);
    else if (auto* op = std::get_if<Pstr_open>(&it.desc)) mexp(op->expr);
    else if (auto* p = std::get_if<Pstr_primitive>(&it.desc)) { if (p->prim.type) ty(*p->prim.type); }
    else if (auto* p = std::get_if<Pstr_val>(&it.desc)) ty(*p->vd.type);
  }
};

// Strict type-check: returns the definite type errors found (empty => accepted).
// Conservative — only DEFINITE errors (unqualified unbound value, type clash);
// unknown/unsupported constructs and qualified names are assumed OK so that
// engine incompleteness shows up as false-rejections to be driven out, not as
// spurious accepts.
std::vector<std::string> structure_typecheck(const ast::Structure& s) {
  Checker ck;
  ck.strict = true;
  run_checker(ck, s);
  auto vr = valrec::value_rec_errors(s);
  for (auto& e : vr) ck.errors.push_back(std::move(e));
  auto ex = extcheck::errors(s);
  for (auto& e : ex) ck.errors.push_back(std::move(e));
  { UnboundWalk uw; uw.ck = &ck;
    for (auto& it : s) { uw.item(it); if (uw.done()) break; }
    if (uw.done()) ck.errors.push_back("Unbound module " + uw.bad); }
  // Value restriction: a fresh NON-STRICT pass (respects value restriction, so
  // weak vars survive) then scan exported top-level bindings for an escaping
  // non-generalizable variable.  Isolated: reads only its own venv.
  { Checker vk;
    run_checker(vk, s);
    std::string e = vk.weak_escape_error(s);
    if (!e.empty()) ck.errors.push_back(std::move(e)); }
  return std::move(ck.errors);
}

std::vector<std::pair<std::string, std::string>> infer_structure_types(
    const ast::Structure& s) {
  // Use the FULL plain-pass pipeline (run_checker registers record fields,
  // ctors, modules and finalises field uniqueness, and leaves the generalised
  // top-level schemes in venv.back()).  The earlier reduced loop skipped record
  // registration, so every local record construction/projection leaked Any.
  Checker ck;
  ck.eng.lenient = true;  // signature pass: best-effort unify (see Engine::lenient)
  ck.fold_abbrevs_ = true;  // keep abbreviations folded for display (see fold_abbrevs_)
  run_checker(ck, s);
  // Apply `module MP = Long.Path` aliases to a rendered signature: rewrite each
  // `Long.Path.` prefix back to `MP.`.  Longest target first so a nested alias
  // wins over a shorter one.  Purely textual on the type-path substrings.
  auto aliases = ck.module_aliases_;
  std::sort(aliases.begin(), aliases.end(),
            [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
  auto apply_aliases = [&](std::string s) {
    for (auto& [tgt, al] : aliases) {
      std::string from = tgt + ".", to = al + ".";
      for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size())
        s.replace(p, from.size(), to);
    }
    return s;
  };
  // An expression-local module name escaping into a signature prints as its
  // definition path (`let module N = Map.Make(S) in .. : int N.t` -> ocamlc's
  // `int Map.Make(String).t`).  A name also bound by a top-level module stays
  // in scope at the signature, so it is NOT rewritten.  Path-boundary-checked
  // (a match must start a path: not preceded by an identifier char or '.').
  std::set<std::string> toplevel_mods;
  for (auto& it : s)
    if (auto* mb = std::get_if<Pstr_module>(&it.desc))
      if (mb->binding.name.txt) toplevel_mods.insert(*mb->binding.name.txt);
  auto apply_local_mods = [&](std::string str) {
    for (auto& [nm, path] : ck.local_module_paths_) {
      if (path.empty() || toplevel_mods.count(nm)) continue;
      std::string from = nm + ".", to = path + ".";
      for (size_t p = 0; (p = str.find(from, p)) != std::string::npos;) {
        char b = p ? str[p - 1] : ' ';
        if (isalnum((unsigned char)b) || b == '_' || b == '\'' || b == '.') { ++p; continue; }
        str.replace(p, from.size(), to);
        p += to.size();
      }
    }
    return str;
  };
  std::vector<std::pair<std::string, std::string>> all;
  auto emit = [&](const std::string& nm) {
    auto f = ck.venv.back().find(nm);
    if (f != ck.venv.back().end())
      all.emplace_back(nm, apply_aliases(apply_local_mods(I::show(f->second))));
  };
  // Resolve `include M` to the included structure (a local struct, directly or
  // via a local module binding), so we can emit its flattened value names too --
  // `ocamlc -i` lists an included module's values in this module's signature.
  std::function<const ast::Structure*(const ModuleExpr&)> incstruct =
      [&](const ModuleExpr& me) -> const ast::Structure* {
    if (auto* ms = std::get_if<Pmod_structure>(&me.desc)) return &ms->items;
    if (auto* mi = std::get_if<Pmod_ident>(&me.desc)) {
      std::string nm = lid_last(mi->id.txt);
      for (auto& it2 : s)
        if (auto* mb = std::get_if<Pstr_module>(&it2.desc))
          if (mb->binding.name.txt && *mb->binding.name.txt == nm)
            if (auto* ms2 = std::get_if<Pmod_structure>(&mb->binding.expr.desc))
              return &ms2->items;
    }
    return nullptr;
  };
  std::function<void(const ast::Structure&)> walk = [&](const ast::Structure& items) {
    for (auto& it : items) {
      if (auto* sv = std::get_if<Pstr_value>(&it.desc)) {
        // Emit EVERY variable a binding's pattern binds, in pattern order -- a
        // destructuring `let (a, b) = e` exports both a and b, not just simple vars.
        for (auto& b : sv->bindings) {
          std::vector<std::string> names;
          valrec::pat_names(b.pat, names);
          for (auto& nm : names) emit(nm);
        }
      } else if (auto* in = std::get_if<Pstr_include>(&it.desc)) {
        // `include (struct.. : SIG)`: the exported values are SIG's, in order.
        if (auto* mc = std::get_if<Pmod_constraint>(&in->expr.desc)) {
          if (auto* sg = std::get_if<Pmty_signature>(&mc->mt->desc)) {
            std::vector<std::string> ns;
            Checker::collect_sig_values(sg->items, ns);
            for (auto& nm : ns) emit(nm);
          }
        } else if (const ast::Structure* inc = incstruct(in->expr)) {
          walk(*inc);
        }
      }
    }
  };
  // Stdlib-shadow display controls (see infer.hpp): a user `module Stdlib`
  // keeps ALL Stdlib. prefixes; a requalified shadowed type keeps its own.
  I::g_keep_stdlib_prefix = ck.user_stdlib_module_;
  I::g_keep_stdlib_paths =
      ck.stdlib_keep_paths_.empty() ? nullptr : &ck.stdlib_keep_paths_;
  walk(s);
  I::g_keep_stdlib_prefix = false;
  I::g_keep_stdlib_paths = nullptr;
  // A shadowed name appears once in the signature, at (and with the type of) its
  // LAST binding -- keep only the final occurrence of each name.
  std::unordered_map<std::string, size_t> last;
  for (size_t i = 0; i < all.size(); ++i) last[all[i].first] = i;
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t i = 0; i < all.size(); ++i)
    if (last[all[i].first] == i) out.push_back(std::move(all[i]));
  return out;
}

// Bridge an inferencer type to a cmiw type descriptor.  `vars` shares type-var
// nodes of equal identity within one value's scheme (so `'a -> 'a` is one var);
// Any becomes a fresh var (opaque).  Constr paths are passed through -- the cmi
// writer keeps predefined ones and renders the rest as opaque vars.
static cmi::cmiw::TyPtr bridge_ty_rec(const TypePtr& t0,
                                      std::unordered_map<const I::Type*, int>& vars, int& nextvar,
                                      std::unordered_set<const I::Type*>& visiting);
static cmi::cmiw::TyPtr bridge_ty(const TypePtr& t0,
                                  std::unordered_map<const I::Type*, int>& vars, int& nextvar) {
  std::unordered_set<const I::Type*> visiting;
  return bridge_ty_rec(t0, vars, nextvar, visiting);
}
static cmi::cmiw::TyPtr bridge_ty_rec(const TypePtr& t0,
                                      std::unordered_map<const I::Type*, int>& vars, int& nextvar,
                                      std::unordered_set<const I::Type*>& visiting) {
  TypePtr t = I::Engine::repr(t0);
  auto bridge_ty = [&](const TypePtr& u, std::unordered_map<const I::Type*, int>& v, int& nv) {
    return bridge_ty_rec(u, v, nv, visiting);
  };
  using K = I::Type::Kind;
  switch (t->kind) {
    case K::Var: {
      auto it = vars.find(t.get());
      if (it != vars.end()) return cmi::cmiw::ty_var(it->second);
      int id = nextvar++; vars[t.get()] = id; return cmi::cmiw::ty_var(id);
    }
    case K::Object: {  // closed structural object `< m1 : t1; m2 : t2 >`
      // A RECURSIVE object type (`< bark : 'self -> unit > as 'self`) is a
      // cycle in the engine graph the writer can't express -- degrade the
      // inner recursive occurrence to an opaque var instead of looping.
      if (!visiting.insert(t.get()).second) return cmi::cmiw::ty_var(nextvar++);
      std::vector<cmi::cmiw::TyPtr> mtys;
      for (auto& a : t->args) mtys.push_back(bridge_ty(a, vars, nextvar));
      visiting.erase(t.get());
      return cmi::cmiw::ty_object(t->labels, std::move(mtys));
    }
    case K::Variant: return cmi::cmiw::ty_var(nextvar++);  // opaque in the .cmi for now
    case K::Arrow:
      return cmi::cmiw::ty_arrow_lbl(bridge_ty(t->dom, vars, nextvar),
                                     bridge_ty(t->cod, vars, nextvar),
                                     t->arrow_label, t->arrow_lbl);
    case K::Tuple: {
      std::vector<cmi::cmiw::TyPtr> as;
      for (auto& a : t->args) as.push_back(bridge_ty(a, vars, nextvar));
      return cmi::cmiw::ty_tuple(std::move(as));
    }
    case K::Constr: {
      std::vector<cmi::cmiw::TyPtr> as;
      for (auto& a : t->args) as.push_back(bridge_ty(a, vars, nextvar));
      std::string path = t->path;
      // The printf-family format type (format/format4/format6, all canonicalised
      // to "format6" by from_coretype) lives in CamlinternalFormatBasics, but
      // ocamlc stores the SOURCE abbreviation, not the expansion: a 3-visible-arg
      // format prints back as `format`, a 4-arg one as `format4` (both are
      // Stdlib-toplevel aliases the cmi writer emits as a real Pdot Tconstr the
      // reader still recognises via is_format_base); only a genuine 6-arg one
      // keeps the CamlinternalFormatBasics.format6 name.  Emitting the expansion
      // instead was a gratuitous DIFF vs the oracle's .cmi.
      if (path == "format6")
        path = as.size() == 3   ? "format"
             : as.size() == 4   ? "format4"
                                : "CamlinternalFormatBasics.format6";
      // Lazy.t is the public Stdlib abbreviation of CamlinternalLazy.t; ocamlc
      // stores the abbreviation, so emit it too rather than the internal name.
      else if (path == "CamlinternalLazy.t") path = "Lazy.t";
      return cmi::cmiw::ty_constr(path, std::move(as));
    }
    case K::Link: return bridge_ty(t->link, vars, nextvar);
  }
  return cmi::cmiw::ty_var(nextvar++);
}

// Convert a run of `type ... and ...` declarations (shared by structure and
// signature emission -- both hold a std::vector<TypeDeclaration>) into SigItems.
static void emit_type_decls(Checker& ck, const std::vector<TypeDeclaration>& decls,
                            std::vector<cmi::cmiw::SigItem>& out) {
  size_t first_new = out.size();
  for (auto& d : decls) {
    // `[@@immediate]` / `[@@immediate64]` -> the type_declaration's Type_immediacy
    // (Always / Always_on_64bits); Printtyp renders it back as the attribute.
    int immed = 0;
    bool unboxed = false;
    for (auto& a : d.attrs) {
      if (a.name == "immediate64") immed = 2;
      else if (a.name == "immediate" && immed == 0) immed = 1;
      else if (a.name == "unboxed" || a.name == "ocaml.unboxed") unboxed = true;
    }
    std::unordered_map<std::string, TypePtr> tvars;        // param name -> engine var
    std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;  // shared across params+manifest
    std::vector<cmi::cmiw::TyPtr> params;
    for (auto& p : d.params) {
      auto pv = bridge_ty(ck.from_coretype(*p, tvars), bvars, nextvar);
      // Params keep their SOURCE names (Tvar Some): ocamlc prints
      // `('outputValue, 'message) fieldStatus` back verbatim.  The shared
      // var node carries the name into every ctor/label occurrence.  An
      // ANONYMOUS param (`type _ t`, common on GADT indices) is stored by ocamlc
      // as Tvar (Some "_") -- Printtyp renders that as `_` and never registers
      // it as a real name -- so carry the literal "_" too; without it the writer
      // emits Tvar None and Printtyp invents `'a`.  (With a manifest ocamlc may
      // rewrite the `_` back to a real name if it occurs there; our failing cases
      // are all manifest-free, so scope the "_" to that case.)
      if (pv->k == cmi::cmiw::Ty::Var) {
        if (auto* v = std::get_if<Ptyp_var>(&p->desc)) pv->var_name = v->name;
        else if (std::holds_alternative<Ptyp_any>(p->desc) && !d.manifest)
          pv->var_name = "_";
      }
      params.push_back(std::move(pv));
    }
    // A variant type: emit its constructors (Cstr_tuple args OR an inline record
    // `Ctor of {l;..}`), with the GADT return type (`Any : 'a -> any`) in cd_res.
    if (auto* var = std::get_if<Ptype_variant>(&d.kind)) {
      std::vector<cmi::cmiw::Ctor> ctors;
      for (auto& c : var->ctors) {
        cmi::cmiw::Ctor cc; cc.name = c.name.txt;
        if (c.res) cc.res = bridge_ty(ck.from_coretype(**c.res, tvars), bvars, nextvar);
        if (auto* tup = std::get_if<Pcstr_tuple>(&c.args))
          for (auto& a : tup->elems) cc.args.push_back(bridge_ty(ck.from_coretype(*a, tvars), bvars, nextvar));
        else if (auto* r = std::get_if<Pcstr_record>(&c.args))
          // Inline record (Typedtree's `Texp_record of {fields; representation;
          // extended_expression}`): emit the labels so a consumer matching
          // `Ctor {l = ..}` resolves the labels via the ctor's rlabels.
          for (auto& f : r->fields) {
            cmi::cmiw::Label lab;
            lab.name = f.name.txt;
            lab.mut = (f.mut == MutableFlag::Mutable);
            lab.ty = bridge_ty(ck.from_coretype(*f.type, tvars), bvars, nextvar);
            cc.inline_record.push_back(std::move(lab));
          }
        ctors.push_back(std::move(cc));
      }
      auto si = cmi::cmiw::sig_variant(d.name.txt, std::move(params), std::move(ctors));
      si.type_private = (d.priv == PrivateFlag::Private);
      si.type_immediate = immed;
      si.type_unboxed = unboxed;
      out.push_back(std::move(si));
      continue;
    }
    if (auto* rec = std::get_if<Ptype_record>(&d.kind)) {
      std::vector<cmi::cmiw::Label> labels;
      for (auto& f : rec->fields) {
        cmi::cmiw::Label lab;
        lab.name = f.name.txt;
        lab.mut = (f.mut == MutableFlag::Mutable);
        lab.ty = bridge_ty(ck.from_coretype(*f.type, tvars), bvars, nextvar);
        labels.push_back(std::move(lab));
      }
      auto si = cmi::cmiw::sig_record(d.name.txt, std::move(params), std::move(labels));
      si.type_private = (d.priv == PrivateFlag::Private);
      si.type_immediate = immed;
      si.type_unboxed = unboxed;
      out.push_back(std::move(si));
      continue;
    }
    cmi::cmiw::TyPtr manifest = nullptr;
    if (d.manifest) {
      // A FLAT closed polymorphic-variant abbreviation (`type view = [ `A | `B ]`,
      // all direct tags, no inheritance): emit its tag set so a consumer's
      // `#view` pattern resolves the tags cross-module.  An abbreviation that
      // INHERITS another polyvariant (`[ Simple.view | `Or ]`) is left abstract --
      // emitting only its direct tags would be an INCOMPLETE set (wrongly matching).
      if (auto* pv = std::get_if<Ptyp_variant>(&d.manifest->get()->desc)) {
        std::vector<std::string> tags; bool all_tag = true;
        for (auto& rf : pv->rows) {
          if (auto* rt = std::get_if<Rtag>(&rf)) tags.push_back(rt->name);
          else { all_tag = false; break; }
        }
        if (all_tag && !tags.empty()) manifest = cmi::cmiw::ty_variant(std::move(tags));
      }
      if (!manifest) manifest = bridge_ty(ck.from_coretype(**d.manifest, tvars), bvars, nextvar);
    }
    auto si = cmi::cmiw::sig_type(d.name.txt, std::move(params), manifest);
    // `type t = ..`: an extensible (Type_open) declaration, not abstract --
    // its `type t += ..` extensions cite it and ocamlc prints the `= ..`.
    si.type_open = std::holds_alternative<Ptype_open>(d.kind);
    si.type_private = (d.priv == PrivateFlag::Private);
    si.type_immediate = immed;
    out.push_back(std::move(si));
  }
  // A `type a .. and b ..` group: Trec_first on the head, Trec_next after
  // (ocamlc prints the group back with `and`).
  for (size_t i = first_new; i < out.size(); ++i)
    out[i].rec_status = (i == first_new) ? 1 : 2;
}

// ---- include module type of M: splice M's (already-compiled) cmi signature ----
// Render a cmi type path as the writer's bare name convention (no Stdlib__).
static std::string bare_cmi_path(const cmi::Path& p) {
  std::string s = cmi_path_str(p);
  if (s.rfind("Stdlib__", 0) == 0) s = s.substr(8);
  else if (s.rfind("Stdlib.", 0) == 0) s = s.substr(7);
  return s;
}
// cmi reader type -> cmi writer type.  Best-effort: shapes the back end / arg
// matching cares about (arrows + labels, tuples, constructors, vars) are
// preserved; anything else degrades to a fresh type variable (always valid).
static cmi::cmiw::TyPtr conv_cmi_ty(const cmi::TypePtr& t0,
    std::unordered_map<const cmi::TypeExpr*, int>& vars, int& nextvar) {
  cmi::TypePtr t = t0;
  // Unwrap Tlink/Tsubst indirections AND Tpoly: a module type's polymorphic
  // value (`val eprintf : ('a,..) format -> 'a` in Signatures.LOG) reaches us as
  // Tpoly(body,[vars]) with the real type in `link`.  Without unwrapping it,
  // conv_cmi_ty hit the default branch and degraded the whole type (incl. the
  // `format6` constructor) to a fresh var -- so a spliced `include Sig` dropped
  // the format type, and `Log.eprintf "%s"` was typed as a plain string, passing
  // a raw string where a format value was needed -> the format reader segfaulted.
  while (t && (t->kind == cmi::TypeExpr::Tlink || t->kind == cmi::TypeExpr::Tsubst ||
               t->kind == cmi::TypeExpr::Tpoly))
    t = t->link;
  if (!t) return cmi::cmiw::ty_var(nextvar++);
  switch (t->kind) {
    case cmi::TypeExpr::Tvar:
    case cmi::TypeExpr::Tunivar: {
      auto it = vars.find(t.get());
      if (it != vars.end()) return cmi::cmiw::ty_var(it->second);
      int id = nextvar++; vars[t.get()] = id; return cmi::cmiw::ty_var(id);
    }
    case cmi::TypeExpr::Tarrow:
      return cmi::cmiw::ty_arrow_lbl(conv_cmi_ty(t->dom, vars, nextvar),
                                     conv_cmi_ty(t->cod, vars, nextvar),
                                     t->label_kind, t->label);
    case cmi::TypeExpr::Ttuple: {
      std::vector<cmi::cmiw::TyPtr> es;
      for (auto& e : t->elems) es.push_back(conv_cmi_ty(e.second, vars, nextvar));
      return cmi::cmiw::ty_tuple(std::move(es));
    }
    case cmi::TypeExpr::Tconstr:
    case cmi::TypeExpr::Texpand: {
      std::vector<cmi::cmiw::TyPtr> as;
      for (auto& a : t->args) as.push_back(conv_cmi_ty(a, vars, nextvar));
      std::string nm = t->path ? bare_cmi_path(*t->path) : "";
      if (nm.empty()) return cmi::cmiw::ty_var(nextvar++);
      return cmi::cmiw::ty_constr(nm, std::move(as));
    }
    default:
      return cmi::cmiw::ty_var(nextvar++);
  }
}
static std::vector<cmi::cmiw::SigItem> cmi_sig_to_items(const cmi::Signature& sig) {
  std::vector<cmi::cmiw::SigItem> out;
  // Types and module-types take no runtime field; emit them first.
  for (auto& td : sig.types) {
    std::unordered_map<const cmi::TypeExpr*, int> vars; int nv = 0;
    std::vector<cmi::cmiw::TyPtr> params;
    for (auto& p : td.params) params.push_back(conv_cmi_ty(p, vars, nv));
    if (td.kind == cmi::TypeDecl::Record) {
      std::vector<cmi::cmiw::Label> ls;
      for (auto& l : td.labels)
        ls.push_back({l.name, l.mutable_, conv_cmi_ty(l.type, vars, nv)});
      out.push_back(cmi::cmiw::sig_record(td.name, std::move(params), std::move(ls)));
    } else if (td.kind == cmi::TypeDecl::Variant) {
      std::vector<cmi::cmiw::Ctor> cs;
      for (auto& c : td.ctors) {
        std::vector<cmi::cmiw::TyPtr> as;
        for (auto& a : c.args) as.push_back(conv_cmi_ty(a, vars, nv));
        cs.push_back({c.name, std::move(as)});
      }
      out.push_back(cmi::cmiw::sig_variant(td.name, std::move(params), std::move(cs)));
    } else {
      cmi::cmiw::TyPtr man = td.manifest ? conv_cmi_ty(td.manifest, vars, nv) : nullptr;
      out.push_back(cmi::cmiw::sig_type(td.name, std::move(params), man));
    }
  }
  for (auto& mt : sig.modtypes)
    if (mt.type && mt.type->kind == cmi::ModuleType::Sig && mt.type->sig)
      out.push_back(cmi::cmiw::sig_modtype(mt.name, cmi_sig_to_items(*mt.type->sig)));
  // Primitive values take no field either; emit before the field-takers.
  for (auto& v : sig.values) {
    if (v.prim.empty()) continue;
    std::unordered_map<const cmi::TypeExpr*, int> vars; int nv = 0;
    out.push_back(cmi::cmiw::sig_external(v.name, conv_cmi_ty(v.type, vars, nv), v.prim, ""));
  }
  // Field-taking items in the recorded runtime field order, so the spliced
  // layout matches the .cmo block (`include M` copies M's non-prim fields).
  std::unordered_map<std::string, const cmi::SigValue*> vmap;
  for (auto& v : sig.values) if (v.prim.empty()) vmap[v.name] = &v;
  std::unordered_map<std::string, const cmi::ModuleDecl*> mmap;
  for (auto& m : sig.modules) mmap[m.name] = &m;
  std::unordered_map<std::string, const cmi::ExtConstructor*> xmap;
  for (auto& x : sig.typexts) xmap[x.name] = &x;
  for (auto& fn : sig.fields) {
    if (auto it = vmap.find(fn); it != vmap.end()) {
      std::unordered_map<const cmi::TypeExpr*, int> vars; int nv = 0;
      out.push_back(cmi::cmiw::sig_value(fn, conv_cmi_ty(it->second->type, vars, nv)));
    } else if (auto it = mmap.find(fn); it != mmap.end()) {
      const cmi::ModuleDecl* md = it->second;
      if (md->type && md->type->kind == cmi::ModuleType::Sig && md->type->sig)
        out.push_back(cmi::cmiw::sig_module(fn, cmi_sig_to_items(*md->type->sig)));
      else if (md->type && md->type->kind == cmi::ModuleType::Alias && md->type->path)
        out.push_back(cmi::cmiw::sig_module_alias(fn, bare_cmi_path(*md->type->path)));
      else
        out.push_back(cmi::cmiw::sig_module(fn, {}));  // opaque, but keeps the field
    } else if (auto it = xmap.find(fn); it != xmap.end()) {
      std::unordered_map<const cmi::TypeExpr*, int> vars; int nv = 0;
      std::vector<cmi::cmiw::TyPtr> args;
      for (auto& a : it->second->args) args.push_back(conv_cmi_ty(a, vars, nv));
      out.push_back(cmi::cmiw::sig_exception(fn, std::move(args)));
    }
  }
  return out;
}

// Emit a Sig_typext for an `exception E [of t.. | of {l;..}]` declaration,
// preserving an inline-record payload (Cstr_record) so a consumer matching
// `M.E {l = ..}` resolves the labels -- otherwise ext_match bails on that arm
// and the whole match collapses to its first arm (the cause of the bootstrapped
// includemod_errorprinter's `Includemod.Apply_error {..}` collapse + crash).
// The extended type's writer path for a `type t += ..` item: a bare name
// resolves like a bare Ptyp_constr head would -- a name brought in by
// `open M` takes M's qualification (`t` under `open Effect` -> "Effect.t").
// Bare `eff` stays the predef (ocamlc stores what the source wrote: predef
// eff for `type _ eff +=`, Stdlib.Effect.t for `type _ Effect.t +=`).  A
// dotted path is taken verbatim (the writer's ladder resolves its head).
static std::string typext_path(Checker& ck, const Longident& lid) {
  if (auto* l = std::get_if<Lident>(&lid.v)) {
    if (auto q = ck.opened_type_quals_.find(l->name); q != ck.opened_type_quals_.end())
      return q->second;
    return l->name;
  }
  // A dotted extended-type path (`User.tag`): if the HEAD module was pulled into
  // scope by an `open` (open Runtime_events => User = Runtime_events.User),
  // qualify it so the .cmi stores the resolved path -- ocamlc records
  // `Runtime_events.User.tag`, not the source-written `User.tag`.
  std::string full = lid_full(lid);
  if (auto dot = full.find('.'); dot != std::string::npos)
    if (auto q = ck.opened_submod_quals_.find(full.substr(0, dot));
        q != ck.opened_submod_quals_.end())
      return q->second + full.substr(dot);
  return full;
}

// The engine canonicalises the builtin `eff` to its public alias Effect.t so
// annotations and Effect.perform unify/print alike; a typext that extends
// bare `eff` keeps the PREDEF path in the .cmi (ocamlc stores the source
// form, `Conversion_failure : string -> int eff`), so rewrite it back.
static void rewrite_eff_back(const cmi::cmiw::TyPtr& t) {
  if (!t) return;
  if (t->k == cmi::cmiw::Ty::Constr && t->name == "Effect.t") t->name = "eff";
  for (auto& a : t->args) rewrite_eff_back(a);
}

// ---- external representation attributes ------------------------------------
// [@@noalloc] -> prim_alloc=false; [@@unboxed]/[@@untagged] (or the per-arg
// `(int [@untagged])` form) -> the native_repr of each eligible arg/result,
// mirroring typedecl.ml's native_repr_of_type: unboxed applies to
// float/int32/int64/nativeint, untagged to immediates (int/char/bool/unit and
// local all-constant variants).  Ineligible positions stay Same_as_ocaml_repr.
static bool attrs_have(const ast::Attributes& attrs, const char* n) {
  for (auto& a : attrs) if (a.name == n) return true;
  return false;
}
static int native_repr_code(const CoreType& t, int kind,
                            const std::set<std::string>& immediates) {
  if (attrs_have(t.attrs, "unboxed")) kind = 1;
  else if (attrs_have(t.attrs, "untagged")) kind = 2;
  if (!kind) return 0;
  auto* c = std::get_if<Ptyp_constr>(&t.desc);
  if (!c) return 0;
  std::string n = lid_full(c->id.txt);
  if (kind == 1) {
    if (n == "float") return 1;
    if (n == "int32") return 3;
    if (n == "int64") return 4;
    if (n == "nativeint") return 5;
    return 0;
  }
  if (n == "int" || n == "char" || n == "bool" || n == "unit" ||
      immediates.count(n))
    return 2;
  return 0;
}
// Local all-constant-constructor variants already emitted into `out` (they are
// Lambda.Immediate, so `[@untagged]` applies to them, e.g. c-api's `data`).
static std::set<std::string> local_immediates(const std::vector<cmi::cmiw::SigItem>& out) {
  std::set<std::string> s;
  for (auto& it : out) {
    if (it.k != cmi::cmiw::SigItem::Type || it.ctors.empty() || it.type_open) continue;
    bool allconst = true;
    for (auto& c : it.ctors)
      if (!c.args.empty() || !c.inline_record.empty()) { allconst = false; break; }
    if (allconst) s.insert(it.name);
  }
  return s;
}
static void apply_prim_attrs(const ast::PrimitiveDescription& pd,
                             const std::vector<cmi::cmiw::SigItem>& out,
                             cmi::cmiw::SigItem& item) {
  item.prim_alloc = !attrs_have(pd.attrs, "noalloc");
  int gkind = attrs_have(pd.attrs, "unboxed") ? 1
            : attrs_have(pd.attrs, "untagged") ? 2 : 0;
  std::set<std::string> immediates = local_immediates(out);
  const CoreType* t = pd.type ? &*pd.type : nullptr;
  while (t) {
    auto* ar = std::get_if<Ptyp_arrow>(&t->desc);
    if (!ar) break;
    item.prim_reprs.push_back(native_repr_code(*ar->dom, gkind, immediates));
    t = &*ar->cod;
  }
  if (t) item.prim_repr_res = native_repr_code(*t, gkind, immediates);
}

// The declared params' SOURCE names ("_" for Ptyp_any): ocamlc stores each as
// Tvar(Some name) in ext_type_params and prints it back verbatim.
static std::vector<std::string> typext_param_names(const ast::TypeExtension& ext) {
  std::vector<std::string> names;
  for (auto& p : ext.params) {
    if (auto* v = std::get_if<Ptyp_var>(&p->desc)) names.push_back(v->name);
    else names.push_back("_");
  }
  return names;
}

// Emit the Sig_typext item for one extension constructor: a plain
// `exception E [of t.. | of {l;..}]` (ext = null, Text_exception), or a
// `type t += E ..` member (ext set: carries the extended type's path/arity,
// Text_first for the group's first ctor, Text_next after).  A GADT return
// (`E : unit t`) shares the arg conversion's tvar scope.  The inline-record
// payload (Cstr_record) is preserved so a consumer matching `M.E {l = ..}`
// resolves the labels -- otherwise ext_match bails on that arm and the whole
// match collapses to its first arm (the cause of the bootstrapped
// includemod_errorprinter's `Includemod.Apply_error {..}` collapse + crash).
static cmi::cmiw::SigItem exn_sigitem(Checker& ck, const std::string& name,
                                      const ast::Pext_decl& pd,
                                      const ast::TypeExtension* ext = nullptr,
                                      bool first = false) {
  std::unordered_map<std::string, TypePtr> tvars;
  std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
  cmi::cmiw::SigItem item;
  if (auto* rec = std::get_if<Pcstr_record>(&pd.args)) {
    std::vector<cmi::cmiw::Label> labels;
    for (auto& f : rec->fields) {
      cmi::cmiw::Label lab;
      lab.name = f.name.txt;
      lab.mut = (f.mut == MutableFlag::Mutable);
      lab.ty = bridge_ty(ck.from_coretype(*f.type, tvars), bvars, nextvar);
      labels.push_back(std::move(lab));
    }
    item = cmi::cmiw::sig_exception_record(name, std::move(labels));
  } else {
    std::vector<cmi::cmiw::TyPtr> args;
    if (auto* tup = std::get_if<Pcstr_tuple>(&pd.args))
      for (auto& a : tup->elems)
        args.push_back(bridge_ty(ck.from_coretype(*a, tvars), bvars, nextvar));
    item = cmi::cmiw::sig_exception(name, std::move(args));
  }
  if (pd.res)
    item.ext_ret = bridge_ty(ck.from_coretype(**pd.res, tvars), bvars, nextvar);
  if (ext) {
    item.ext_path = typext_path(ck, ext->path.txt);
    item.ext_params = typext_param_names(*ext);
    item.text_kind = first ? 0 : 1;  // Text_first / Text_next
    if (item.ext_path == "eff") {
      for (auto& c : item.ctors) {
        for (auto& a : c.args) rewrite_eff_back(a);
        for (auto& l : c.inline_record) rewrite_eff_back(l.ty);
      }
      rewrite_eff_back(item.ext_ret);
    }
  }
  return item;
}

// Build a .cmi signature from a hand-written interface (.mli) -- the explicit
// path, used when an interface file exists.  Types are taken verbatim from the
// declarations (no inference); local type names are pre-registered so qualified
// and same-module references resolve.
std::vector<cmi::cmiw::SigItem> signature_to_cmi(
    const ast::Signature& s,
    const std::unordered_map<std::string, const ast::Signature*>* outer,
    const std::unordered_map<std::string, const ast::Signature*>* outer_mods) {
  Checker ck;
  ck.record_kinds_ = true;
  ck.keep_local_abbrevs_ = true;  // verbatim path: `t` stays `t`, not its manifest
  // Collect `module type S = sig .. end` so a functor result `: S` (Map.Make)
  // can be resolved to S's signature items.  Inherit ENCLOSING modtypes too: a
  // nested signature (`module type S = sig include Thing; module Map : Map end`
  // in identifiable.mli) references modtypes declared in its outer scope.
  std::unordered_map<std::string, const ast::Signature*> modtypes;
  if (outer) modtypes = *outer;
  // Local module name -> its inline signature, threaded through nesting so an
  // `open M` (below) can pull M's module-type decls into scope even when M is a
  // SIBLING declared in an enclosing structure (camlinternalMenhirLib's
  // `module Engine : sig open EngineTypes; module Make (T:TABLE) : ENGINE .. end`
  // -- TABLE/ENGINE live in the sibling EngineTypes).
  std::unordered_map<std::string, const ast::Signature*> module_sigs;
  if (outer_mods) module_sigs = *outer_mods;
  for (auto& it : s) {
    if (auto* pm = std::get_if<Psig_module>(&it.desc))
      if (pm->md.name.txt && pm->md.type)
        if (auto* ps = std::get_if<Pmty_signature>(&pm->md.type->desc))
          module_sigs[*pm->md.name.txt] = &ps->items;
  }
  // Import the module-type decls of an opened module (`open EngineTypes`) so a
  // following unqualified `: ENGINE` / `(T : TABLE)` resolves to its members.
  auto import_modtypes_of = [&](const ast::Signature& msig) {
    for (auto& mit : msig) {
      if (auto* pmt = std::get_if<Psig_modtype>(&mit.desc))
        if (pmt->type)
          if (auto* ps = std::get_if<Pmty_signature>(&pmt->type->desc))
            modtypes[pmt->name.txt] = &ps->items;
      if (auto* pms = std::get_if<Psig_modtypesubst>(&mit.desc))
        if (auto* ps = std::get_if<Pmty_signature>(&pms->type.desc))
          modtypes[pms->name.txt] = &ps->items;
    }
  };
  for (auto& it : s) {
    if (auto* pt = std::get_if<Psig_type>(&it.desc))
      for (auto& d : pt->decls) ck.register_type_decl(d);
    if (auto* pmt = std::get_if<Psig_modtype>(&it.desc))
      if (pmt->type)
        if (auto* ps = std::get_if<Pmty_signature>(&pmt->type->desc))
          modtypes[pmt->name.txt] = &ps->items;
    // `module type S := sig .. end` (a destructive modtype SUBSTITUTION): S takes
    // no field and is erased from the output, but a later `include S with ..` must
    // still expand to its members.  printtyp.mli declares `module type Printers :=`
    // then `include Printers with type 'a printer := ..`; without registering the
    // subst, body_sig couldn't resolve the include and 22 items (ident/path/
    // type_expr/..) were dropped -> Printtyp.* read wrong fields.
    if (auto* pms = std::get_if<Psig_modtypesubst>(&it.desc))
      if (auto* ps = std::get_if<Pmty_signature>(&pms->type.desc))
        modtypes[pms->name.txt] = &ps->items;
    if (auto* po = std::get_if<Psig_open>(&it.desc))
      if (auto* l = std::get_if<Lident>(&po->id.txt.v))
        if (auto f = module_sigs.find(l->name); f != module_sigs.end())
          import_modtypes_of(*f->second);
  }
  // Resolve a module type to its signature items (Pmty_signature directly, a
  // named modtype `S`, or `S with ...` -- the with-constraints are ignored).
  std::function<const ast::Signature*(const ast::ModuleType&)> body_sig =
      [&](const ast::ModuleType& mt) -> const ast::Signature* {
    if (auto* ps = std::get_if<Pmty_signature>(&mt.desc)) return &ps->items;
    if (auto* pi = std::get_if<Pmty_ident>(&mt.desc))
      if (auto* l = std::get_if<Lident>(&pi->id.txt.v))
        if (auto f = modtypes.find(l->name); f != modtypes.end()) return f->second;
    if (auto* pw = std::get_if<Pmty_with>(&mt.desc)) return body_sig(*pw->mt);
    return nullptr;
  };
  // A QUALIFIED named module type (`Set.S`, `Hashtbl.S with type key = string`):
  // resolve through the head module's cmi modtype decl and convert to SigItems.
  // (the with-constraints only refine types, which take no runtime field).  This
  // lets `module Set : Set.S with ..` (a functor result) emit its members so
  // `M.Set.empty` resolves -- otherwise the whole submodule is dropped.
  auto qual_modtype_items =
      [&](const ast::ModuleType& mt0) -> std::vector<cmi::cmiw::SigItem> {
    const ast::ModuleType* mt = &mt0;
    while (auto* pw = std::get_if<Pmty_with>(&mt->desc)) mt = pw->mt.get();
    auto* pi = std::get_if<Pmty_ident>(&mt->desc);
    if (!pi) return {};
    // Flatten the modtype path `A.B...MT` into components (outermost first); the
    // last is the module-type name, the rest is the module path to navigate.
    std::vector<std::string> comps;
    std::function<bool(const ast::Longident*)> flat =
        [&](const ast::Longident* lid) -> bool {
      if (auto* l = std::get_if<Lident>(&lid->v)) { comps.push_back(l->name); return true; }
      if (auto* dd = std::get_if<Ldot>(&lid->v)) {
        if (!flat(dd->prefix.get())) return false;
        comps.push_back(dd->name); return true;
      }
      return false;  // Lapply: unsupported
    };
    if (!flat(&pi->id.txt) || comps.size() < 2) return {};
    const std::string& mtname = comps.back();
    // The head may be a LOCAL submodule (`IncrementalEngine.INCREMENTAL_ENGINE`
    // where IncrementalEngine is a sibling, not a separate cmi): find its
    // module-type decl in the threaded module signatures and emit its items.
    if (comps.size() == 2)
      if (auto f = module_sigs.find(comps[0]); f != module_sigs.end())
        for (auto& mit : *f->second)
          if (auto* pmt = std::get_if<Psig_modtype>(&mit.desc))
            if (pmt->name.txt == mtname && pmt->type)
              if (auto* ps = std::get_if<Pmty_signature>(&pmt->type->desc))
                return signature_to_cmi(ps->items, &modtypes, &module_sigs);
    // Cross-module, possibly DEEP (`CamlinternalMenhirLib.IncrementalEngine.
    // INCREMENTAL_ENGINE`): load the head cmi, navigate intermediate submodules,
    // then read the module-type's signature.  Previously only a single-component
    // prefix (`A.MT`) resolved -- a deeper prefix was a `Ldot`, not a `Lident`,
    // so the whole `include` was dropped (the parser's MenhirInterpreter lost the
    // 23 INCREMENTAL_ENGINE values -> a short module block -> a wild call at parse).
    try {
      auto cmi = cmi::CmiFile::load(head_cmi(comps[0]));
      const cmi::Signature* sig = &cmi.sig();
      for (size_t i = 1; i + 1 < comps.size(); ++i) {  // walk submodules
        const cmi::Signature* next = nullptr;
        for (auto& md : sig->modules)
          if (md.name == comps[i] && md.type &&
              md.type->kind == cmi::ModuleType::Sig && md.type->sig) {
            next = md.type->sig.get(); break;
          }
        if (!next) return {};
        sig = next;
      }
      for (auto& md : sig->modtypes)
        if (md.name == mtname && md.type &&
            md.type->kind == cmi::ModuleType::Sig && md.type->sig)
          return cmi_sig_to_items(*md.type->sig);
    } catch (...) {}
    return {};
  };
  std::vector<cmi::cmiw::SigItem> out;
  for (auto& it : s) {
    if (auto* pv = std::get_if<Psig_value>(&it.desc)) {
      if (!pv->vd.type) continue;
      std::unordered_map<std::string, TypePtr> tvars;
      std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
      out.push_back(cmi::cmiw::sig_value(pv->vd.name.txt,
                      bridge_ty(ck.from_coretype(*pv->vd.type, tvars), bvars, nextvar)));
    } else if (auto* pr = std::get_if<Psig_primitive>(&it.desc)) {
      if (pr->pd.type && !pr->pd.prims.empty()) {
        std::unordered_map<std::string, TypePtr> tvars;
        std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
        auto ty = bridge_ty(ck.from_coretype(*pr->pd.type, tvars), bvars, nextvar);
        std::string native = pr->pd.prims.size() > 1 ? pr->pd.prims[1] : "";
        auto item = cmi::cmiw::sig_external(pr->pd.name.txt, ty, pr->pd.prims[0], native);
        apply_prim_attrs(pr->pd, out, item);
        out.push_back(std::move(item));
      }
    } else if (auto* pt = std::get_if<Psig_type>(&it.desc)) {
      emit_type_decls(ck, pt->decls, out);
    } else if (auto* pm = std::get_if<Psig_module>(&it.desc)) {
      if (pm->md.name.txt && pm->md.type) {
        if (auto* ps = std::get_if<Pmty_signature>(&pm->md.type->desc))
          out.push_back(cmi::cmiw::sig_module(*pm->md.name.txt,
                                              signature_to_cmi(ps->items, &modtypes, &module_sigs)));
        else if (auto* al = std::get_if<Pmty_alias>(&pm->md.type->desc)) {
          // `module M = Target` (stdlib.mli's `module List = Stdlib__List`, or a
          // DOTTED target like types.mli's `module Uid = Shape.Uid`).  Emit the
          // full path so `M.x` resolves through the alias (a dotted target was
          // previously dropped, losing the whole submodule).
          if (!std::holds_alternative<Lapply>(al->id.txt.v))
            out.push_back(cmi::cmiw::sig_module_alias(*pm->md.name.txt,
                                                      lid_full(al->id.txt)));
        } else if (auto* pf = std::get_if<Pmty_functor>(&pm->md.type->desc)) {
          // `module Make (Ord : _) : S with ...` (Map/Set/Hashtbl): emit a functor
          // module so Make takes a field and Make(Arg).x resolves via S's layout.
          // Peel curried params; the result body is the innermost non-functor mt.
          std::string param;
          std::vector<cmi::cmiw::SigItem> param_sig;
          if (auto* fn = std::get_if<Functor_named>(&pf->param)) {
            if (fn->name.txt) param = *fn->name.txt;
            // the parameter's signature (OrderedType): consumers coerce the
            // functor ARGUMENT (Int) to this layout so `Ord.compare` resolves to
            // the right field -- without it, the whole argument is passed and
            // `Ord.compare` reads a wrong slot (Map.Make(Int).find segfaults).
            if (fn->type) {
              if (const ast::Signature* psg = body_sig(*fn->type))
                param_sig = signature_to_cmi(*psg, &modtypes, &module_sigs);
              else  // a QUALIFIED parameter modtype (`MakeEngineTable (T :
                    // TableFormat.TABLES)`): resolve it so the ARGUMENT is projected.
                param_sig = qual_modtype_items(*fn->type);
            }
          }
          const ast::ModuleType* body = pf->body.get();
          while (auto* pf2 = std::get_if<Pmty_functor>(&body->desc)) body = pf2->body.get();
          std::vector<cmi::cmiw::SigItem> result;
          if (const ast::Signature* rs = body_sig(*body))
            result = signature_to_cmi(*rs, &modtypes, &module_sigs);
          else  // a QUALIFIED result modtype (`MakeEngineTable (..) : EngineTypes.TABLE
                // with ..`): resolve it through the local/cross-module signature.
            result = qual_modtype_items(*body);
          out.push_back(cmi::cmiw::sig_module_functor(*pm->md.name.txt, param,
                          std::move(param_sig), std::move(result)));
        } else if (const ast::Signature* bs = body_sig(*pm->md.type)) {
          // `module MD5 : S` (a NAMED module type) or `S with ...`: emit the
          // submodule with S's resolved signature inline, so a consumer can
          // resolve `Digest.MD5.bytes` to its field (else the module is dropped).
          auto items = signature_to_cmi(*bs, &modtypes, &module_sigs);
          drop_modsubst(items, with_modsubst_names(*pm->md.type));
          out.push_back(cmi::cmiw::sig_module(*pm->md.name.txt, std::move(items)));
        } else if (auto items = qual_modtype_items(*pm->md.type); !items.empty()) {
          // `module Set : Set.S with ..` (a qualified functor-result module type).
          drop_modsubst(items, with_modsubst_names(*pm->md.type));
          out.push_back(cmi::cmiw::sig_module(*pm->md.name.txt, std::move(items)));
        } else {
          // Any other module type (`module Consistbl : module type of struct ..
          // end`): emit an opaque submodule so it still TAKES A FIELD -- else the
          // surrounding value layout is short of the .cmo and every following
          // member (Persistent_env.empty) resolves to the wrong slot.
          out.push_back(cmi::cmiw::sig_module(*pm->md.name.txt, {}));
        }
      }
    } else if (auto* pmt = std::get_if<Psig_modtype>(&it.desc)) {
      // `module type S = sig .. end`: emit it so a functor parameter typed by S
      // (`Make (H : Hashtbl.HashedType)`) can resolve H's members to fields.
      if (pmt->type)
        if (auto* ps = std::get_if<Pmty_signature>(&pmt->type->desc))
          out.push_back(cmi::cmiw::sig_modtype(pmt->name.txt,
                                               signature_to_cmi(ps->items, &modtypes, &module_sigs)));
    } else if (auto* pe = std::get_if<Psig_exception>(&it.desc)) {
      // `exception E [of t..]`: emit Sig_typext (takes a runtime field).  Without
      // it the .cmi value layout is short of the .cmo (Parsing.Parse_error/YYexit
      // shifted peek_val -> the C parse engine read the wrong table fields).
      const ExtensionConstructor& ec = pe->exn.ctor;
      if (!ec.name.txt.empty())
        if (auto* pd = std::get_if<Pext_decl>(&ec.kind))
          out.push_back(exn_sigitem(ck, ec.name.txt, *pd));
    } else if (auto* px = std::get_if<Psig_typext>(&it.desc)) {
      // `type exn += Error of t`: each extension constructor TAKES A FIELD (like
      // an exception).  persistent_env.mli's `type exn += private Error` was
      // dropped, shifting `empty` to the wrong slot -> `Persistent_env.empty ()`
      // applied a non-closure -> SIGSEGV in env's init.
      bool first = true;
      for (auto& ec : px->ext.ctors) {
        if (ec.name.txt.empty()) continue;
        if (auto* pd = std::get_if<Pext_decl>(&ec.kind)) {
          out.push_back(exn_sigitem(ck, ec.name.txt, *pd, &px->ext, first));
        } else {
          auto item = cmi::cmiw::sig_exception(ec.name.txt, {});  // rebind `+= C = D`
          item.ext_path = typext_path(ck, px->ext.path.txt);
          item.ext_params = typext_param_names(px->ext);
          item.text_kind = first ? 0 : 1;
          out.push_back(std::move(item));
        }
        first = false;
      }
    } else if (auto* pinc = std::get_if<Psig_include>(&it.desc)) {
      // `include module type of M`: splice M's compiled cmi signature here so the
      // .cmi records M's values (with prim flags) and types -- otherwise the
      // included members are absent and the field layout is short of the .cmo.
      if (auto* pto = std::get_if<Pmty_typeof>(&pinc->mt.desc)) {
        if (auto* pi = std::get_if<Pmod_ident>(&pto->me->desc))
          if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) try {
            auto cmi = cmi::CmiFile::load(head_cmi(l->name));
            for (auto& si : cmi_sig_to_items(cmi.sig())) out.push_back(std::move(si));
          } catch (...) {}
      } else if (const ast::Signature* bs = body_sig(pinc->mt)) {
        // `include S` (named local modtype) / `include sig .. end`
        for (auto& si : signature_to_cmi(*bs, &modtypes, &module_sigs)) out.push_back(std::move(si));
      } else if (auto items = qual_modtype_items(pinc->mt); !items.empty()) {
        // `include Identifiable.S with type t = int` (a QUALIFIED cross-module
        // module type): splice its members from the head module's cmi, so the
        // .cmi records the included values/submodules (Numbers.Int gets Map/Set/
        // compare).  Without it a downstream `include Numbers.Int` builds a short
        // block missing Key.Map -> Arg_helper.Make's parsed record is garbage.
        for (auto& si : items) out.push_back(std::move(si));
      }
    }
  }
  // Canonical shadowing dedup: a FIELD-TAKING member redeclared later (same
  // namespace) keeps only its LAST occurrence, at that position (OCaml semantics).
  // `include module type of String; .. val for_all`; `include A; include B` both
  // carrying a member (main_args' Bytecomp_options).  Writing both would add a slot
  // before every later field; the AST layouts (sig_layout / register_sig_layouts in
  // lambda.cpp) dedup IDENTICALLY, so .cmi index == .cmo block index ==
  // functor-param index everywhere.  Values / modules / exception ctors are
  // separate namespaces (a value `x` and a module `x` both take a field).
  out = cmi::cmiw::dedup_shadowed_fields(std::move(out));
  return out;
}

// All variable binders of a top-level let pattern, in source (left-to-right)
// order -- `let (a, b) = ..` / `let {x; y} = ..` / `let (C v) = ..` export
// every binder as a value, exactly like a plain `let a = ..` does.
static void toplevel_pat_vars(const ast::Pattern& p, std::vector<std::string>& out) {
  if (auto* v = std::get_if<Ppat_var>(&p.desc)) { out.push_back(v->name.txt); return; }
  if (auto* t = std::get_if<Ppat_tuple>(&p.desc)) {
    for (auto& e : t->elems) toplevel_pat_vars(*e, out);
  } else if (auto* c = std::get_if<Ppat_construct>(&p.desc)) {
    if (c->arg) toplevel_pat_vars(**c->arg, out);
  } else if (auto* a = std::get_if<Ppat_alias>(&p.desc)) {
    toplevel_pat_vars(*a->p, out); out.push_back(a->name.txt);
  } else if (auto* ct = std::get_if<Ppat_constraint>(&p.desc)) {
    toplevel_pat_vars(*ct->p, out);
  } else if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
    for (auto& f : r->fields) toplevel_pat_vars(*f.second, out);
  } else if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) {
    toplevel_pat_vars(*lz->p, out);
  } else if (auto* vr = std::get_if<Ppat_variant>(&p.desc)) {
    if (vr->arg) toplevel_pat_vars(**vr->arg, out);
  } else if (auto* ar = std::get_if<Ppat_array>(&p.desc)) {
    for (auto& e : ar->elems) toplevel_pat_vars(*e, out);
  } else if (auto* op = std::get_if<Ppat_open>(&p.desc)) {
    toplevel_pat_vars(*op->p, out);
  } else if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
    toplevel_pat_vars(*o->l, out);  // both sides bind the same set
  }
}

// The Sig_module item for one module binding, by shape: a structure body is
// inferred recursively; `module M : sig .. end = ..` takes the CONSTRAINT
// signature verbatim (it is authoritative, like a .mli); a functor emits
// Mty_functor with its named param's signature and its body's.  Null when the
// shape isn't representable yet (Pmod_ident/apply/unpack).
static std::optional<cmi::cmiw::SigItem> module_binding_sigitem(
    const std::string& name, const ast::ModuleExpr& me) {
  if (auto* ms = std::get_if<Pmod_structure>(&me.desc))
    return cmi::cmiw::sig_module(name, infer_signature(ms->items));
  if (auto* pi = std::get_if<Pmod_ident>(&me.desc)) {
    // `module MP = Gc.Memprof` / `module Alias = A`: a module alias binding.
    // ocamlc records Mty_alias(<target path>) (Mp_absent -- transparent, takes
    // no runtime field), printed `module MP = Gc.Memprof`.  A functor
    // application (Lapply) has no path form -- leave it dropped.
    if (!std::holds_alternative<Lapply>(pi->id.txt.v))
      return cmi::cmiw::sig_module_alias(name, lid_full(pi->id.txt));
    return std::nullopt;
  }
  if (auto* mc = std::get_if<Pmod_constraint>(&me.desc)) {
    if (mc->mt)
      if (auto* ps = std::get_if<Pmty_signature>(&mc->mt->desc))
        return cmi::cmiw::sig_module(name, signature_to_cmi(ps->items));
    return module_binding_sigitem(name, *mc->me);
  }
  if (auto* mf = std::get_if<Pmod_functor>(&me.desc)) {
    std::string pname;
    std::vector<cmi::cmiw::SigItem> psig;
    if (auto* fn = std::get_if<Functor_named>(&mf->param)) {
      if (fn->name.txt) pname = *fn->name.txt;
      if (fn->type)
        if (auto* ps = std::get_if<Pmty_signature>(&fn->type->desc))
          psig = signature_to_cmi(ps->items);
    }
    std::vector<cmi::cmiw::SigItem> result;
    if (auto* bs = std::get_if<Pmod_structure>(&mf->body->desc))
      result = infer_signature(bs->items);
    else if (auto* bc = std::get_if<Pmod_constraint>(&mf->body->desc)) {
      if (bc->mt)
        if (auto* ps = std::get_if<Pmty_signature>(&bc->mt->desc))
          result = signature_to_cmi(ps->items);
    }
    auto item = cmi::cmiw::sig_module_functor(name, pname, std::move(psig),
                                              std::move(result));
    // `module F () -> ..`: the parameter is Unit (generative), printed `()`.
    item.functor_unit = std::holds_alternative<Functor_unit>(mf->param);
    return item;
  }
  return std::nullopt;
}

std::vector<cmi::cmiw::SigItem> infer_signature(const ast::Structure& s) {
  Checker ck;
  ck.record_kinds_ = true;
  run_checker(ck, s);  // leaves top-level bindings in venv.back()
  // Emission phase: checking is DONE, every from_coretype below only converts
  // declaration types for the .cmi -- keep local abbreviations as written
  // (`startDate : (int, message) fieldStatus` stores `message`, not string).
  ck.keep_local_abbrevs_ = true;
  std::vector<cmi::cmiw::SigItem> out;
  for (auto& it : s) {
    if (auto* sv = std::get_if<Pstr_value>(&it.desc)) {
      for (auto& b : sv->bindings) {
        std::vector<std::string> names;
        toplevel_pat_vars(b.pat, names);  // every binder, incl. destructuring lets
        for (auto& nm : names) {
          auto f = ck.venv.back().find(nm);
          if (f == ck.venv.back().end()) continue;
          std::unordered_map<const I::Type*, int> vars; int nextvar = 0;
          out.push_back(cmi::cmiw::sig_value(nm, bridge_ty(f->second, vars, nextvar)));
        }
      }
    } else if (auto* pr = std::get_if<Pstr_primitive>(&it.desc)) {
      // `external f : t = "prim"`: a Val_prim value (typed from the annotation).
      if (pr->prim.type && !pr->prim.prims.empty()) {
        std::unordered_map<std::string, TypePtr> tvars;
        std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
        auto ty = bridge_ty(ck.from_coretype(*pr->prim.type, tvars), bvars, nextvar);
        std::string native = pr->prim.prims.size() > 1 ? pr->prim.prims[1] : "";
        auto item = cmi::cmiw::sig_external(pr->prim.name.txt, ty, pr->prim.prims[0], native);
        apply_prim_attrs(pr->prim, out, item);
        out.push_back(std::move(item));
      }
    } else if (auto* ty = std::get_if<Pstr_type>(&it.desc)) {
      emit_type_decls(ck, ty->decls, out);
    } else if (auto* pe = std::get_if<Pstr_exception>(&it.desc)) {
      // `exception E [of ..]` in a .ml without a .mli: emit the Sig_typext so
      // the inferred .cmi carries the exception (it takes a runtime field, and a
      // qualified `M.E` use must resolve it -- otherwise the match collapses).
      const ExtensionConstructor& ec = pe->exn.ctor;
      if (!ec.name.txt.empty()) {
        if (auto* pd = std::get_if<Pext_decl>(&ec.kind))
          out.push_back(exn_sigitem(ck, ec.name.txt, *pd));
        else if (std::holds_alternative<Pext_rebind>(ec.kind))
          // `exception F = E`: a rebind over the predefined exn.  ocamlc records a
          // Sig_typext (Text_exception, Text_rebind) that Printtyp prints as bare
          // `exception F`; without it a submodule `struct exception F = E end`
          // came out `sig end`.  (The signature path already handles this at the
          // Psig_typext rebind branch.)
          out.push_back(cmi::cmiw::sig_exception(ec.name.txt, {}));
      }
    } else if (auto* px = std::get_if<Pstr_typext>(&it.desc)) {
      bool first = true;
      for (auto& ec : px->ext.ctors)
        if (!ec.name.txt.empty())
          if (auto* pd = std::get_if<Pext_decl>(&ec.kind)) {
            out.push_back(exn_sigitem(ck, ec.name.txt, *pd, &px->ext, first));
            first = false;
          }
    } else if (auto* pmt = std::get_if<Pstr_modtype>(&it.desc)) {
      // `module type S = sig .. end` in a .ml (no .mli): emit Sig_modtype so the
      // inferred .cmi carries it (a modtype takes no runtime field, so it never
      // shifts the value layout).  Only the signature body is representable yet.
      if (pmt->type)
        if (auto* ps = std::get_if<Pmty_signature>(&pmt->type->desc))
          out.push_back(cmi::cmiw::sig_modtype(pmt->name.txt, signature_to_cmi(ps->items)));
    } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
      // A submodule: emit Sig_module so the oracle can resolve `Outer.Inner.x`
      // and so the submodule's runtime field keeps the surrounding value layout
      // aligned.  Structures are inferred recursively (self-contained
      // submodules; outer refs not yet); constrained/functor shapes via
      // module_binding_sigitem.
      if (!mb->binding.name.txt) continue;  // `module _ = ...`
      if (auto item = module_binding_sigitem(*mb->binding.name.txt, mb->binding.expr))
        out.push_back(std::move(*item));
    } else if (auto* mr = std::get_if<Pstr_recmodule>(&it.desc)) {
      // `module rec A .. and B ..`: each binding like Pstr_module, marked
      // Trec_first/Trec_next so ocamlc prints the group as one `module rec`.
      int rs = 1;
      for (auto& b : mr->bindings) {
        if (!b.name.txt) continue;
        if (auto item = module_binding_sigitem(*b.name.txt, b.expr)) {
          item->rec_status = rs;
          out.push_back(std::move(*item));
        }
        rs = 2;
      }
    } else if (auto* in = std::get_if<Pstr_include>(&it.desc)) {
      // `include M` / `include (struct .. end)`: build_module FLATTENS the
      // included module's members into THIS module's record (each takes its own
      // field), so the inferred .cmi must list them too -- otherwise every field
      // after the include sits one slot too low and a cross-module read lands on
      // the wrong field (ocamlbuild's Log: `module Debug = ..; include Debug`).
      const ast::Structure* inc = nullptr;
      if (auto* ms = std::get_if<Pmod_structure>(&in->expr.desc))
        inc = &ms->items;                              // include (struct .. end)
      else if (auto* mi = std::get_if<Pmod_ident>(&in->expr.desc)) {
        std::string nm = lid_last(mi->id.txt);         // include LocalModule
        for (auto& it2 : s)
          if (auto* mb2 = std::get_if<Pstr_module>(&it2.desc))
            if (mb2->binding.name.txt && *mb2->binding.name.txt == nm) {
              if (auto* ms2 = std::get_if<Pmod_structure>(&mb2->binding.expr.desc))
                inc = &ms2->items;
              break;
            }
      }
      if (inc)
        for (auto& si : infer_signature(*inc)) out.push_back(si);
    }
  }
  // Canonical shadowing dedup: a name bound twice at top level (e.g. ocamllex's
  // rule `token` then a hand-written wrapper `token` in lexer.ml's trailer) keeps
  // only its LAST occurrence, at that position -- exactly as the .cmo block layout
  // (sig_layout / register_sig_layouts) dedups.  Without this the inferred .cmi
  // carries BOTH, so its field index for the survivor is one slot too high and an
  // external `Lexer.token` reads the wrong block field (a closure where a token is
  // expected -> a SWITCH past its table -> heap corruption).  The explicit-.mli
  // path (signature_to_cmi) already dedups identically.
  out = cmi::cmiw::dedup_shadowed_fields(std::move(out));
  return out;
}

}  // namespace cppcaml
