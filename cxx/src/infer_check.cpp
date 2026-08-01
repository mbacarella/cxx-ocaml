#include "cppcaml/infer_check.hpp"

#include <algorithm>
#include <deque>
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
// head module name -> resolved .cmi path (head_cmi below).  Valid only while
// the filesystem is stable: flushed on any dir reconfiguration and at the start
// of each compiled unit (an earlier unit in the same invocation writes a .cmi
// a later unit may reference).
static std::unordered_map<std::string, std::string> g_head_cmi_memo;
void clear_head_cmi_cache() { g_head_cmi_memo.clear(); }
void set_infer_stdlib_dir(const std::string& dir) {
  g_stdlib_dir = dir;
  g_head_cmi_memo.clear();
}
void set_infer_module_dirs(std::vector<std::string> dirs) {
  g_infer_module_dirs = std::move(dirs);
  g_head_cmi_memo.clear();
}
const std::vector<std::string>& infer_module_dirs() { return g_infer_module_dirs; }

namespace {

namespace I = infer;
using namespace ast;

// Path of a .cmi in the configured stdlib directory.
std::string stdpath(const std::string& file) { return g_stdlib_dir + "/" + file; }
// The .cmi of a head module: the stdlib naming pattern, else (for a separately
// compiled local module like `A`) the first <head>.cmi found in the -I dirs.
std::string head_cmi_uncached(const std::string& head) {
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
std::string head_cmi(const std::string& head) {
  auto it = g_head_cmi_memo.find(head);
  if (it != g_head_cmi_memo.end()) return it->second;
  std::string r = head_cmi_uncached(head);
  g_head_cmi_memo.emplace(head, r);
  return r;
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
  // current object's self type, one entry per nested object body -- `{< .. >}`
  // (Pexp_override) returns the innermost self, making a `method m = {< >}`
  // recursive so Printtyp emits the `object ('a) .. method m : 'a end` binder.
  std::vector<TypePtr> self_ty_stack_;
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
  // Resolver for a SIBLING inline-sig module's type manifest
  // (`module Simple : sig type view = [..] end` then `[ Simple.view | .. ]`
  // in a later sibling) -- set by signature_to_cmi over its module_sigs map.
  std::function<const CoreType*(const std::string&, const std::string&)>
      sibling_type_manifest_;
  // `type t := rhs` (Psig_typesubst): every following citation of `t` is
  // REPLACED by rhs (the decl itself takes no signature item).  Unlike
  // type_aliases these expand unconditionally -- printtyp.mli's
  // `type namespace := Shape.Sig_component_kind.t` must substitute into each
  // val or the vals degrade to a free 'a.
  std::unordered_map<std::string, Alias> type_substs_;
  // Decl-position opened quals for a manifest's bare type names.  OCaml
  // resolves a manifest at its DECL position, but expansion here happens
  // lazily at USE position, where a LATER local decl may already have
  // shadowed an opened name (patterns.mli: Simple.view's row cites `pattern`
  // = Typedtree.pattern via `open Typedtree`; the local `type pattern` two
  // items later erases the qual before `val omega`/`val erase` expand the
  // row, so the cite degraded to the local decl).  Keyed by the manifest AST
  // node and SHARED across a signature's sub-checkers, so a sibling-module
  // manifest expansion (sibling_type_manifest_) sees the declaring module's
  // snapshot.
  std::shared_ptr<std::unordered_map<
      const CoreType*, std::vector<std::pair<std::string, std::string>>>>
      manifest_decl_quals_ = std::make_shared<std::unordered_map<
          const CoreType*, std::vector<std::pair<std::string, std::string>>>>();
  // Names declared in this checker's own scope so far (decl order): a
  // manifest citing one of these means the LOCAL decl, not an opened qual,
  // so it takes no snapshot entry.
  std::set<std::string> local_declared_;
  // RAII: resolve a manifest's bare names as of its DECL position -- restores
  // the snapshotted opened quals and hides a later-declared same-named local
  // alias for the expansion's duration.  No-op for a manifest without a
  // snapshot or whose names still resolve as they did at decl time.
  struct DeclQualOverlay {
    Checker& ck;
    std::vector<std::pair<std::string, std::optional<std::string>>> squals;
    std::vector<std::pair<std::string, Alias>> saliases;
    DeclQualOverlay(Checker& c, const CoreType* man) : ck(c) {
      auto it = ck.manifest_decl_quals_->find(man);
      if (it == ck.manifest_decl_quals_->end()) return;
      for (auto& [n, q] : it->second) {
        auto qi = ck.opened_type_quals_.find(n);
        if (qi != ck.opened_type_quals_.end() && qi->second == q &&
            !ck.type_aliases.count(n))
          continue;  // still resolves as at decl time
        squals.emplace_back(n, qi != ck.opened_type_quals_.end()
                                   ? std::optional<std::string>(qi->second)
                                   : std::nullopt);
        ck.opened_type_quals_[n] = q;
        if (auto ai = ck.type_aliases.find(n); ai != ck.type_aliases.end()) {
          saliases.emplace_back(n, ai->second);
          ck.type_aliases.erase(ai);
        }
      }
    }
    ~DeclQualOverlay() {
      for (auto& [n, q] : squals) {
        if (q) ck.opened_type_quals_[n] = *q;
        else ck.opened_type_quals_.erase(n);
      }
      for (auto& [n, a] : saliases) ck.type_aliases[n] = a;
    }
  };
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
  // Imported (cmi) GADT variants, resolved lazily by DOTTED type path
  // ("Typedtree.pattern_desc"): ctor (name, scheme) pairs in decl order with
  // cd_res-refined results, for the PARTIALITY machinery only.  Deliberately
  // NOT fed into gadt_types/gadt_ctors: those drive match windowing, and
  // re-windowing every match on imported GADT ctors would change inference
  // far beyond exhaustiveness.  nullopt = memoized "not a GADT variant".
  std::unordered_map<std::string,
                     std::optional<std::vector<std::pair<std::string, TypePtr>>>>
      imported_gadt_memo_;
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
  // PROCESS-GLOBAL (inline static): schemes seeded across checkers (a module
  // body's emission re-inference receiving the outer checker's cenv/venv) must
  // keep outer and inner decls of the same name DISTINCT -- per-checker
  // counters made an outer `t` and an inner re-declared `t` collide on stamp.
  inline static int next_type_stamp_ = 1;
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
  // Same idea for `type t += C` extension constructors (keyed by the extension
  // ctor AST node): process_item overlays them into the enclosing module's cenv
  // so a local `type t += B` shadows an outer variant's `B` when a value in the
  // module matches on it (patmatch's MPR7761 -- else `B` resolved to the file's
  // top-level `type t = B of int | ..` and cited the wrong `t`).
  std::unordered_map<const void*, TypePtr> ext_ctor_scheme_;
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
  // A construct/pattern node used as a constructor's argument whose (ambiguous)
  // name must be disambiguated by the enclosing ctor's DECLARED argument type,
  // not by lexical scope.  Node -> that expected type; the back end reads it via
  // expr_constr / pat_constr to pick the right same-named ctor's TAG.  BOTH the
  // producer (expression) and consumer (pattern) sides must route through this,
  // or a construct/match pair would resolve the same ctor to DIFFERENT tags (a
  // producer/consumer miscompile).
  std::unordered_map<const void*, TypePtr> ctor_arg_type_;      // expression args
  std::unordered_map<const void*, TypePtr> pat_ctor_arg_type_;  // pattern args
  std::unordered_map<const void*, TypePtr> pat_record_arg_type_;  // record-pattern ctor args
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
  // Type names declared ABSTRACT (Ptype_abstract, no manifest) somewhere in the
  // file -- in a struct, or in a functor-param / module-type signature (`type
  // t`, `type act`).  Typeopt.classify maps an abstract type constructor to
  // `Any` (a GENERIC array element, `caml_array_get`), not `Addr` -- so an
  // `act array` access must NOT specialize to `caml_array_get_addr` the way a
  // known record/variant element does.  Consulted by array_kind_str; a name
  // that is ALSO declared concrete (algebraic_names_) is excluded so a same-file
  // concrete type is never mis-widened.
  std::set<std::string> abstract_names_;
  // Type names declared with a concrete algebraic definition (record / variant /
  // extensible open).  Such elements ARE `Addr` in Typeopt.classify, so they
  // keep the `[addr]` array specialization; they veto abstract_names_ on a name
  // collision.
  std::set<std::string> algebraic_names_;
  // Qualified names ("A.t") of abstract-without-manifest types declared by a
  // FUNCTOR PARAMETER's signature -- the precise (collision-free) form for
  // widening a dotted `A.t array` element to gen.  Collected by the pre-pass
  // in infer_value_kinds; matched exactly against the element's path.
  std::set<std::string> param_abstract_quals_;
  // Local `[@@unboxed]` single-field types -> the last component of the wrapped
  // field/argument type (`type compunit = Compunit of string [@@unboxed]` ->
  // "string").  OCaml's Typeopt.scrape_ty sees through such wrappers, so a
  // polymorphic compare on the type specializes to the representation's compare
  // (caml_string_notequal for a string-wrapper).  Cross-module wrappers are
  // resolved on demand from the cmi; see is_unboxed_string.
  std::unordered_map<std::string, std::string> local_unboxed_inner_;
  std::unordered_map<std::string, int> unboxed_string_cache_;  // path -> 0 no / 1 yes
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
  // The single predefined ground type EVERY candidate record gives an
  // AMBIGUOUS label, else "".  Agreement makes the field's type independent
  // of which record ocamlc's type-directed disambiguation picks, so using it
  // is sound under every resolution.  Predefined ground bases only: a
  // same-NAMED nominal/abbreviated path in two records could denote
  // different types, so those stay unresolved.
  std::string ambiguous_field_ground(const std::string& label) {
    auto ci = field_candidates_.find(label);
    if (ci == field_candidates_.end() || ci->second.size() < 2) return "";
    static const std::set<std::string> ground = {
        "string", "int", "char", "bool", "unit", "float",
        "int32", "int64", "nativeint"};
    std::string agreed;
    for (auto& cand : ci->second) {
      TypePtr a = I::Engine::repr(eng.instantiate(cand));
      if (a->kind != I::Type::Kind::Arrow) return "";
      TypePtr cod = I::Engine::repr(a->cod);
      if (cod->kind != I::Type::Kind::Constr || !cod->args.empty() ||
          cod->stamp != 0 || !ground.count(cod->path)) return "";
      if (agreed.empty()) agreed = cod->path;
      else if (agreed != cod->path) return "";
    }
    return agreed;
  }
  // A polymorphic field (`{ pf : 'a. .. }`) gets no monomorphic value scheme (it
  // would clash across uses), so it never lands in `fields_`.  But a record
  // PATTERN `{pf}` still identifies its record TYPE by that label -- so record the
  // owning record type per poly-field label (unique labels only, in
  // finalize_fields), letting the pattern resolve to `pf` while its bound field
  // stays polymorphic (Any).  label -> record type candidates.
  struct PolyField { TypePtr recTy; const CoreType* ftype; };
  std::unordered_map<std::string, std::vector<PolyField>> poly_field_rec_candidates_;
  std::unordered_map<std::string, PolyField> poly_field_rec_;
  // Poly-field labels the KIND pass also registered as mono format arrows
  // (see register_record_decl) -- excluded from finalize_fields' ambiguity
  // test, and the PATTERN path prefers the poly binding for them.
  std::set<std::string> poly_format_labels_;
  // locally-abstract types `(type a)`: bound to a fresh (flexible) var so that
  // annotations mentioning `a` unify rather than clashing as an opaque constr.
  std::unordered_map<std::string, TypePtr> newtype_vars;
  // Depth of enclosing locally-abstract-type scopes (`fun (type a) -> ..`,
  // `let f : type a. ..`).  Inside one, an unpinned GADT match scrutinee may
  // secretly BE the abstract type (our kinds pass ties a binding's annotation
  // to its body only after inferring it, so the scrutinee just looks like a
  // fresh var) -- rigid in ocamlc, where nothing is refutable -- so
  // gadt_match_partial must not pin it from the covered arms' indices.
  int la_scope_ = 0;
  // The binding for one locally-abstract type.  The DISPLAY pass binds a RIGID
  // node (ocamlc's newtype model): a GADT arm's equation `a = int` is then a
  // lenient constr mismatch that never LEAKS into the signature, while the
  // arm's ordinary unifications ('b := int) persist -- no trail window needed.
  // show prints a rigid node as a type variable ('a), its generalized face.
  // The strict and value-kind passes keep the flexible var (the reject pass
  // needs the equations to type arm bodies; the kind pass needs their kinds).
  TypePtr newtype_binding(const std::string& name = "") {
    if (fold_abbrevs_ && !strict) {
      TypePtr t = eng.constr("");
      t->rigid = true;
      // The SOURCE name (`(type t)` / `'t.`): ocamlc's .cmi stores the
      // generalized face as Tvar(Some "t"), so the cmi bridge needs it.
      t->rigid_name = name;
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
  // match / function-cases nodes whose TOTAL verdict came from a COMPLETED
  // GADT refutation of every uncovered ctor -- as opposed to the conservative
  // "unknown -> Total" defaults above.  Only these may drop the Match_failure
  // default in the back end (see gadt_function_partial's `proven`).
  std::set<const void*> total_proven;
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
  // PARAMETERIZED functor-param types (`type +'a t` under `(T : S)`): bare
  // name -> qualified path; uses rebuild `('r) T.t` with the written args
  // (the nullary map above can't carry args).  Scoped like the map above.
  std::unordered_map<std::string, std::string> functor_param_type_quals_;
  // ROW-PHANTOM aliases from a functor-param/include sig (`type 'a mr =
  // [< .. ] as 'a`): an application expands to the arg tied to the bound.
  std::unordered_map<std::string, Alias> row_phantom_aliases_;
  // Functor-param sig type decls with manifests/constraints, keyed by the
  // QUALIFIED name ("A.t"): an application in the body solves the decl's
  // constraints against its args (pr4775's `'a A.t` pins 'a to `[> ]`), and
  // a bare-var (phantom) manifest expands to the arg in constraint position.
  std::unordered_map<std::string, Alias> param_sig_type_decls_;
  // Every type name a functor param's sig declares (abstract included), keyed by
  // the param name: `open X` (X a functor param, no cmi on disk) registers each
  // `t` -> `X.t` in opened_type_quals_ so a bare `'a op` after `open X` resolves
  // to `'a X.op` instead of degrading to a fresh var (shallow2deep).
  std::unordered_map<std::string, std::vector<std::string>> param_sig_type_names_;
  std::unordered_map<std::string, TypePtr> cmi_abstract_subst_;       // a cmi modtype's "t" -> arg type
  // A parameterless class's object type, so `new c` yields it (non-strict only).
  std::unordered_map<std::string, TypePtr> class_types_;
  // Class CONSTRUCTOR schemes (`new c` for a class with params): the arrow
  // over the constructor's value params to the class's object type (mixin2).
  std::unordered_map<std::string, TypePtr> class_ctor_types_;
  // The same types keyed by DECLARATION NODE: the flat name maps above are
  // clobbered by a shadowing declaration (`class c ..; open struct class c ..
  // end` -- generalized-open/shadowing), but the cmi emission wants the type
  // of the exact declaration it's walking.
  std::unordered_map<const ast::ClassDeclaration*, TypePtr> class_node_types_;
  // The SELF object node inferred for each class body (what `self` bound to):
  // the cmi producer maps it to csig_self so method types citing the self
  // (`unit -> 'self`) marshal as the shared node and print `object ('a) ..`.
  std::unordered_map<const ast::ClassDeclaration*, TypePtr> class_self_types_;
  // ALIAS classes (`class c = other [args]`, through lets/opens/parens): the
  // target class path as written -- the producer emits Cty_constr(target)
  // (`class c : with_param`, toplevel_lets M3/M4).
  std::unordered_map<const ast::ClassDeclaration*, std::string> class_alias_refs_;
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
      case cmi::TypeExpr::Tvariant: {
        // A fully-decoded polymorphic-variant row converts to a real engine
        // row, so a cmi GADT result like `[< `X ] t` (typing-multifile's D.C)
        // survives into an inferred val instead of degrading to a var.
        // Non-strict only (the strict pass never reasons about Variant nodes);
        // a tags-only legacy decode keeps the old generic var.  Memoized fresh
        // var FIRST: a self-citing fixpoint row degrades its inner occurrence
        // (matching conv_cmi_ty's guard) instead of recursing forever.
        if (strict || n->pv_args.size() != n->pv_tags.size() ||
            n->pv_present.size() != n->pv_tags.size())
          return generic_var();
        auto it = memo.find(const_cast<cmi::TypeExpr*>(n));
        if (it != memo.end()) return it->second;
        memo[const_cast<cmi::TypeExpr*>(n)] = generic_var();
        bool all_present = true;
        for (char p : n->pv_present) if (!p) all_present = false;
        int vk = !n->row_closed ? 0 : (n->row_more_nil && all_present ? 2 : 1);
        std::vector<TypePtr> ats;
        std::vector<char> has;
        std::vector<std::string> present;
        for (std::size_t i = 0; i < n->pv_tags.size(); ++i) {
          has.push_back(n->pv_args[i] ? 1 : 0);
          ats.push_back(n->pv_args[i] ? from_cmi(n->pv_args[i], memo)
                                      : eng.fresh_var());
          if (vk == 1 && n->pv_present[i]) present.push_back(n->pv_tags[i]);
        }
        TypePtr row = eng.variant_type(n->pv_tags, std::move(ats),
                                       std::move(has), vk);
        row->present = std::move(present);
        row->level = I::GENERIC_LEVEL;
        memo[const_cast<cmi::TypeExpr*>(n)] = row;
        return row;
      }
      default:
        return generic_var();  // package/etc: unknown for now
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
    if (auto* op = std::get_if<Ptyp_open>(&t.desc)) {
      // `N.(t)`: a type-scope local open.  A bare non-predef constr head
      // inside resolves through N (ocamlc stores the qualified path N.t);
      // anything else converts as written.
      if (auto* c = std::get_if<Ptyp_constr>(&op->type->desc))
        if (auto* l = std::get_if<Lident>(&c->id.txt.v)) {
          static const std::set<std::string> predefs = {
              "int", "char", "bytes", "float", "bool", "unit", "exn", "eff",
              "continuation", "array", "list", "option", "nativeint", "int32",
              "int64", "lazy_t", "string", "extension_constructor", "floatarray"};
          if (!predefs.count(l->name)) {
            std::vector<TypePtr> as;
            for (auto& a : c->args) as.push_back(from_coretype(*a, vars));
            return eng.constr(lid_full(op->mod_.txt) + "." + l->name,
                              std::move(as));
          }
        }
      return from_coretype(*op->type, vars);
    }
    if (auto* v = std::get_if<Ptyp_var>(&t.desc)) {
      auto it = vars.find(v->name);
      if (it != vars.end()) return it->second;
      auto g = generic_var();
      g->var_hint = v->name;  // keep the written name for the cmi's Tvar(Some _)
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
      TypePtr r = eng.tuple(std::move(es));
      // A LABELED tuple (`arg_label list * is_ret_tvar:bool`, ctype.mli):
      // keep the component labels ("" = unlabeled) -- `labels` is otherwise
      // unused on a Tuple node.  Dropping them made the cmi a plain tuple,
      // so a consumer's labeled pattern failed to match.
      if (!tu->labels.empty())
        for (auto& l : tu->labels) r->labels.push_back(l ? *l : "");
      return r;
    }
    // `'a 'b. t` (method poly types): the quantified body, vars as fresh.
    if (auto* pl = std::get_if<Ptyp_poly>(&t.desc))
      return from_coretype(*pl->type, vars);
    // `(t as 'a)`: the inner type, with 'a bound to it for later references.
    // An ALREADY-BOUND 'a (a decl param: `type 'a t = <.. [< `A ] as 'a ..>`)
    // UNIFIES with the aliased type -- the param becomes the row, which is
    // what ocamlc stores (the decl prints back with a `constraint 'a = ..`
    // clause).  Rebinding instead silently dropped the tie (Entities).
    if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) {
      TypePtr inner = from_coretype(*al->type, vars);
      auto ex = vars.find(al->name);
      if (ex != vars.end()) {
        soft_unify(ex->second, inner);
        return I::Engine::repr(ex->second);
      }
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
      // An EMPTY open row `[> ]` is legal (pr4775's `constraint 'a = [> ]`);
      // an empty exact/upper row is not written syntax.
      bool simple = !pvr->rows.empty() || pvr->closed == ClosedFlag::Open;
      if (!strict && simple) {
        // A SINGLE inherit of a known local variant alias (`[> 'a lambda]`)
        // is that abbreviation's expansion at the written bound: tags come
        // from the alias (so merges union tag-wise and keep the covering
        // name), from_inherit marks the `[ | 'a lambda ]` display form for
        // an exact fixpoint (mixin2/mixin3).
        if ((fold_abbrevs_ || keep_local_abbrevs_) && pvr->rows.size() == 1)
          if (auto* ri0 = std::get_if<Rinherit>(&pvr->rows[0])) {
            TypePtr ex = I::Engine::repr(from_coretype(*ri0->ct, vars));
            if (ex->kind == I::Type::Kind::Variant && !ex->abbrev.empty()) {
              ex->variant_kind =
                  pvr->closed == ClosedFlag::Open ? 0 : (pvr->labels ? 1 : 2);
              ex->from_inherit = true;
              if (pvr->labels && !pvr->labels->empty())
                ex->present = *pvr->labels;
              return ex;
            }
            // A CHAINED abbreviation (`[< int u]` where u = t, t = [`A|`B]):
            // the folded conversion stays an opaque constr.  Expand with
            // folding OFF to reach the row, then stamp the abbreviation AS
            // WRITTEN back on it -- ocamlc stores the full row with row_name
            // = (u, [int]) and prints `[< int u > `A ]`.  Exact `[ int u ]`
            // keeps the old path (a named exact row is a plain Tconstr).
            int ivk = pvr->closed == ClosedFlag::Open ? 0 : (pvr->labels ? 1 : 2);
            if (ex->kind == I::Type::Kind::Constr && !ex->path.empty() &&
                ivk != 2) {
              bool sf = fold_abbrevs_, sk = keep_local_abbrevs_;
              fold_abbrevs_ = false; keep_local_abbrevs_ = false;
              TypePtr deep = I::Engine::repr(from_coretype(*ri0->ct, vars));
              fold_abbrevs_ = sf; keep_local_abbrevs_ = sk;
              if (deep->kind == I::Type::Kind::Variant &&
                  !deep->labels.empty()) {
                deep->variant_kind = ivk;
                deep->abbrev = ex->path;
                deep->abbrev_args = ex->args;
                deep->from_inherit = true;
                if (pvr->labels && !pvr->labels->empty())
                  deep->present = *pvr->labels;
                return deep;
              }
            }
          }
        // ALL-inherit rows of known variant aliases (`[ 'a lambda | 'a expr ]`
        // -- lexpr's manifest): expand to the TAG UNION under one vars map, so
        // a use ties the shared param through the tags (`#lambda as x` in
        // lexpr_ops reaches lexpr's 'a).  A shared tag's args unify.
        // The VERBATIM path (keep_local_abbrevs_) takes it too -- ocamlc
        // stores the expanded union for `[< finite | infinite ]` bounds in
        // sig items (range_intf) -- with the inherited abbreviation itself
        // expanded in display mode.
        if ((fold_abbrevs_ || keep_local_abbrevs_) && pvr->rows.size() > 1) {
          bool all_inh = true;
          std::vector<TypePtr> exps;
          for (auto& r : pvr->rows) {
            auto* ri = std::get_if<Rinherit>(&r);
            if (!ri) { all_inh = false; break; }
            bool sf = fold_abbrevs_, sk = keep_local_abbrevs_;
            fold_abbrevs_ = true; keep_local_abbrevs_ = false;
            TypePtr ex = I::Engine::repr(from_coretype(*ri->ct, vars));
            fold_abbrevs_ = sf; keep_local_abbrevs_ = sk;
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
            // has=2: CONJUNCTIVE constant (`` `A of & t ``, constant flag AND
            // an arg type -- Reither{no_arg=true; arg_type=[t]}); truthy for
            // every has-an-arg consumer, distinguished by the writer bridge.
            else { ats.push_back(from_coretype(*rt->types[0], vars));
                   has.push_back(rt->constant ? 2 : 1); }
          } else {
            auto* ri = std::get_if<Rinherit>(&r);
            // A MIXED row's inherit (`[ Simple.view | \`Or of .. ]`) expands
            // its tags into the union like the all-inherit path -- ocamlc's
            // .cmi stores the flattened row (patterns.mli Half_simple.view).
            // An unexpandable inherit keeps the previous opaque handling.
            if (fold_abbrevs_ || keep_local_abbrevs_) {
              // A SIBLING-module citation (`Simple.view`) expands through its
              // AST manifest directly (the flat alias map keys by LAST name,
              // so three sibling `view`s would collide).
              const CoreType* src = ri->ct.get();
              if (sibling_type_manifest_)
                if (auto* c2 = std::get_if<Ptyp_constr>(&src->desc))
                  if (auto* dd = std::get_if<Ldot>(&c2->id.txt.v))
                    if (auto* pl2 = std::get_if<Lident>(&dd->prefix->v))
                      if (const CoreType* man =
                              sibling_type_manifest_(pl2->name, dd->name))
                        src = man;
              bool sf = fold_abbrevs_, sk = keep_local_abbrevs_;
              fold_abbrevs_ = true; keep_local_abbrevs_ = false;
              TypePtr ex;
              {  // a sibling MANIFEST resolves its bare cites at ITS decl
                 // position (patterns.mli Half_simple.view's `pattern`)
                DeclQualOverlay dq(*this, src);
                ex = I::Engine::repr(from_coretype(*src, vars));
              }
              fold_abbrevs_ = sf; keep_local_abbrevs_ = sk;
              if (ex->kind == I::Type::Kind::Variant && !ex->labels.empty()) {
                for (size_t i = 0; i < ex->labels.size(); ++i) {
                  size_t k = 0;
                  for (; k < tags.size(); ++k) if (tags[k] == ex->labels[i]) break;
                  if (k < tags.size()) soft_unify(ats[k], ex->args[i]);
                  else { tags.push_back(ex->labels[i]);
                         ats.push_back(ex->args[i]);
                         has.push_back(ex->tag_has_arg[i]); }
                }
                continue;
              }
            }
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
      // Loading a qualified type `M.t` in an annotation (`i : Ast_iterator.
      // iterator`) also brings M's record fields into ext_fields_, so a label
      // M shares with another record (`structure`, also in Ast_mapper.mapper)
      // is seen as AMBIGUOUS -- and field_scheme returns null instead of
      // adopting whichever module happened to be loaded first (a value ref to
      // Ast_mapper), which would wrongly pin `i` to mapper and read its
      // `structure`@38 instead of iterator's @37.  Codegen pass only.
      if (record_kinds_)
        if (auto* d = std::get_if<Ldot>(&c->id.txt.v))
          load_module_record_fields(*d->prefix);
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
      // Bare names only: a DOTTED head's arity is its own module's business --
      // padding `'a A.t` from the flat bare-name map (keyed "t", possibly this
      // body's own 2-ary t) invented `('a, 'd) A.t` (pr4775).
      auto ar = std::holds_alternative<Lident>(c->id.txt.v)
                    ? type_arity.find(lid_last(c->id.txt))
                    : type_arity.end();
      if (ar != type_arity.end() && !as.empty())
        // Strict pads with Any (absorbs -- can't false-reject); the display
        // passes pad with a fresh VAR so the slot can be pinned by the body
        // and prints 'a, not `_` (issue479's `_ iter2gen` second param).
        while ((int)as.size() < ar->second)
          as.push_back(eng.fresh_var());  // P4-I: guard removed, corpus-validated
      // A KIND-ful local decl with `constraint 'a = ..` clauses (record/
      // variant/abstract -- constrained ALIASES go through the phantom path
      // below) enforces them at every application: `'a range` pins the use's
      // 'a to the declared bound, which is what ocamlc stores back in the
      // val's scheme (`([< finite | infinite ] as 'a) range` -- range_intf).
      if (!strict)
        if (auto* l = std::get_if<Lident>(&c->id.txt.v))
          if (!expanding_.count(l->name)) {
            auto bi = bare_unique_stamp_.find(l->name);
            auto di = bi != bare_unique_stamp_.end() && bi->second > 0
                          ? stamp_type_decl_.find(bi->second)
                          : stamp_type_decl_.end();
            if (di != stamp_type_decl_.end() && di->second &&
                !di->second->constraints.empty() && !di->second->manifest &&
                di->second->params.size() == as.size()) {
              const TypeDeclaration& dd = *di->second;
              std::unordered_map<std::string, TypePtr> sub;
              for (size_t i = 0; i < as.size(); ++i)
                if (auto* pv2 = std::get_if<Ptyp_var>(&dd.params[i]->desc))
                  sub[pv2->name] = as[i];
              expanding_.insert(l->name);
              for (auto& tc : dd.constraints)
                soft_unify(from_coretype(*tc.t1, sub),
                           from_coretype(*tc.t2, sub));
              expanding_.erase(l->name);
            }
          }
      // Expand a known type abbreviation (type (params) name = manifest), with a
      // recursion guard so a cyclic/recursive abbreviation falls back to opaque.
      std::string nm = lid_last(c->id.txt);
      // A `type t := rhs` substitution: a BARE citation of t expands to rhs
      // unconditionally (the subst is erased from the signature, so keeping
      // the name would cite nothing).
      if (std::holds_alternative<Lident>(c->id.txt.v))
        if (auto si = type_substs_.find(nm);
            si != type_substs_.end() && !expanding_.count(nm) &&
            si->second.params.size() == as.size()) {
          std::unordered_map<std::string, TypePtr> sub;
          for (size_t i = 0; i < as.size(); ++i)
            if (!si->second.params[i].empty()) sub[si->second.params[i]] = as[i];
          expanding_.insert(nm);
          TypePtr r = from_coretype(*si->second.manifest, sub);
          expanding_.erase(nm);
          return r;
        }
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
        TypePtr r;
        {  // decl-position name resolution for the manifest's bare cites
          DeclQualOverlay dq(*this, ai->second.manifest);
          r = I::Engine::repr(from_coretype(*ai->second.manifest, sub));
        }
        manifest_expansion_ = saved_me;
        expanding_.erase(nm);
        if (r->kind == I::Type::Kind::Variant) {
          r->abbrev = nm;
          r->abbrev_args = as;
        }
        return r;
      }
      // A ROW-PHANTOM alias registered from a functor-param/include sig:
      // `R mr` IS R (manifest = the param), tied to the row bound -- return
      // the arg so inferred use sites hold the bare row (pr7601's Make).
      if (!strict && !expanding_.count(nm))
        if (auto rp = row_phantom_aliases_.find(nm);
            rp != row_phantom_aliases_.end() &&
            rp->second.params.size() == as.size()) {
          auto* al = std::get_if<Ptyp_alias>(&rp->second.manifest->desc);
          std::size_t pi = rp->second.params.size();
          for (std::size_t i = 0; i < rp->second.params.size(); ++i)
            if (rp->second.params[i] == al->name) { pi = i; break; }
          if (pi < as.size()) {
            std::unordered_map<std::string, TypePtr> sub;
            for (std::size_t i = 0; i < as.size(); ++i)
              if (!rp->second.params[i].empty()) sub[rp->second.params[i]] = as[i];
            expanding_.insert(nm);
            from_coretype(*rp->second.manifest, sub);  // ties the arg to the bound
            expanding_.erase(nm);
            return I::Engine::repr(as[pi]);
          }
        }
      // A ROW-PHANTOM alias (`type 'a mr = [< `L of t .. ] as 'a` -- the
      // param IS the row): applying it ties the WRITTEN arg to the row bound,
      // pinning the arg's `_` slots to the declared types (pr7601's
      // `[ `Location of _ | .. ] maybe_region`).  Side effect only -- the
      // application still emits as the folded opaque Tconstr below.
      if (!strict && ai != type_aliases.end() &&
          ai->second.params.size() == as.size() && !as.empty() &&
          !expanding_.count(nm))
        if (auto* al = std::get_if<Ptyp_alias>(&ai->second.manifest->desc))
          if (std::holds_alternative<Ptyp_variant>(al->type->desc)) {
            bool is_param = false;
            for (auto& p : ai->second.params) is_param |= (p == al->name);
            if (is_param) {
              std::unordered_map<std::string, TypePtr> sub;
              for (size_t i = 0; i < as.size(); ++i)
                if (!ai->second.params[i].empty()) sub[ai->second.params[i]] = as[i];
              expanding_.insert(nm);
              from_coretype(*ai->second.manifest, sub);  // Ptyp_alias unifies the arg with the bound
              expanding_.erase(nm);
            }
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
      // A BARE name can never refer to ANOTHER module's nested abbreviation
      // (OCaml scoping) -- expand through the flat map only when the alias is
      // top-level (display_path empty) or we are inside its declaring module.
      // shape.ml's nested `Item.t = string * K.t` squatted the flat "t" and
      // hijacked the outer recursive record `t` inside `desc`'s ctor types:
      // the tuple-vs-record clash collapsed of_path's letrec inference and
      // the Path.t match degenerated to one unconditional arm (bug #12).
      // A NESTED alias (display_path "Item.t") must not capture an unrelated
      // same-last-name reference through the flat map -- shape.ml's
      // `Item.t = string * K.t` hijacked both the outer record `t` and the
      // dotted `Sig_component_kind.t`, collapsing of_path's inference and
      // degenerating the Path.t match to one unconditional arm (bug #12).
      // BARE name: blocked when an enclosing OPAQUE decl owns it (tenv stamp
      // in scope; inside the alias's own module the alias has no stamp, so
      // AbstractFloat's `float -> t` still expands and keeps its [float]
      // kind).  DOTTED name: it must actually NAME the alias (dp == the
      // written path, or dp declared deeper and ending in ".<path>").
      bool nested_shadowed = false;
      if (ai != type_aliases.end() && !ai->second.display_path.empty()) {
        const std::string& dp = ai->second.display_path;
        if (auto* l = std::get_if<Lident>(&c->id.txt.v)) {
          if (tenv_lookup(l->name)) nested_shadowed = true;
        } else {
          std::string full = lid_full(c->id.txt);
          bool names_it =
              dp == full ||
              (dp.size() > full.size() &&
               dp.compare(dp.size() - full.size() - 1, full.size() + 1,
                          "." + full) == 0);
          if (!names_it) nested_shadowed = true;
        }
      }
      if (getenv("CTDBG") && ai != type_aliases.end() && nm == "t")
        fprintf(stderr,
                "[CTDBG] expand? nm=t lident=%d fold=%d keep=%d shadowed=%d "
                "dp=%s modpfx=%s\n",
                (int)std::holds_alternative<Lident>(c->id.txt.v),
                (int)fold_abbrevs_, (int)keep_local_abbrevs_,
                (int)nested_shadowed, ai->second.display_path.c_str(),
                mod_prefix_.c_str());
      if (!fold_abbrevs_ && !keep_local_abbrevs_ && !nested_shadowed &&
          ai != type_aliases.end() && ai->second.params.size() == as.size() &&
          !expanding_.count(nm)) {
        std::unordered_map<std::string, TypePtr> sub;
        for (size_t i = 0; i < as.size(); ++i)
          if (!ai->second.params[i].empty()) sub[ai->second.params[i]] = as[i];
        expanding_.insert(nm);
        TypePtr r;
        {  // decl-position name resolution for the manifest's bare cites
          DeclQualOverlay dq(*this, ai->second.manifest);
          r = from_coretype(*ai->second.manifest, sub);
        }
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
      // A PARAMETERIZED functor-param type used bare in the param's own sig
      // (`val foo : [ `A ] t -> unit` under `(T : S)`): rebuild with the
      // written args at the qualified path (`[ `A ] T.t`), so body vals
      // unifying through it export the access path (pr7199).
      if (!functor_param_type_quals_.empty())
        if (auto* l = std::get_if<Lident>(&c->id.txt.v))
          if (auto s = functor_param_type_quals_.find(l->name);
              s != functor_param_type_quals_.end())
            return eng.constr(s->second, std::move(as));
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
                   !ai->second.display_path.empty() &&
                   // Only when the alias's ARITY matches the written args: a bare
                   // `t` applied to more (or fewer) args than the same-named local
                   // abbreviation isn't that abbreviation -- it's an OUTER type the
                   // submodule's checker can't see (pr7152's `_ t` is `Simple.t`,
                   // arity 1, not the nested `Data.t = int`, arity 0).  Requalifying
                   // to the nested alias printed `'a Data.t` for `Simple.M`'s key.
                   ai->second.params.size() == as.size()) {
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
      std::vector<std::vector<TypePtr>> mpv;
      std::vector<std::vector<std::string>> mpn;
      bool any_poly = false;
      for (auto& f : ob->fields) {
        if (auto* ot = std::get_if<Otag>(&f)) {
          ms.push_back(ot->name.txt);
          // An explicitly polymorphic method `< m : 'a. 'a t >`: the binders
          // scope to the METHOD (fresh vars in a scoped copy of `vars`), and
          // are recorded on the object node so the cmi bridge emits Tpoly.
          auto* pp = std::get_if<Ptyp_poly>(&ot->type->desc);
          if (pp && !pp->vars.empty()) {
            auto sub = vars;
            std::vector<TypePtr> bs;
            for (auto& n : pp->vars) { auto fv = generic_var(); sub[n] = fv; bs.push_back(fv); }
            ts.push_back(from_coretype(*pp->type, sub));
            mpv.push_back(std::move(bs)); mpn.push_back(pp->vars);
            any_poly = true;
          } else {
            ts.push_back(from_coretype(*ot->type, vars));
            mpv.emplace_back(); mpn.emplace_back();
          }
        } else if (auto* oi = std::get_if<Oinherit>(&f)) {
          // `< foo : int; ob; .. >`: an INHERITED local object abbreviation
          // splices its fields (one level; t01's obj_type gets ob's `f`).
          if (auto* pc = std::get_if<Ptyp_constr>(&oi->type->desc))
            if (pc->args.empty())
              if (auto* l = std::get_if<Lident>(&pc->id.txt.v))
                if (auto ai = type_aliases.find(l->name);
                    ai != type_aliases.end() && ai->second.manifest)
                  if (auto* iob = std::get_if<Ptyp_object>(
                          &ai->second.manifest->desc))
                    for (auto& f2 : iob->fields)
                      if (auto* ot2 = std::get_if<Otag>(&f2)) {
                        ms.push_back(ot2->name.txt);
                        ts.push_back(from_coretype(*ot2->type, vars));
                        mpv.emplace_back(); mpn.emplace_back();
                      }
        }
      }
      TypePtr r = eng.object_type(std::move(ms), std::move(ts));
      if (any_poly) {
        r->method_polys = std::move(mpv);
        r->method_poly_names = std::move(mpn);
      }
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
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
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
  // Enclosing-scope `open M` module paths to replay before this checker's own
  // body is inferred.  The emission pass re-infers each submodule with a FRESH
  // checker (module_binding_sigitem -> infer_signature), which otherwise loses
  // the outer file's opens -- so `open Printf; module M = struct let f x =
  // eprintf "%s" (List.hd x) end` degraded M.f's arg to a fresh var (the format
  // constraint on `eprintf`, unresolvable when `eprintf` is unbound, never
  // fired).  Set from g_inherited_opens; applied by run_checker.
  std::vector<const ast::Longident*> replay_opens_;
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
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
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
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(head)));
      const cmi::Signature* sig = &loaded.back()->sig();
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
  // `open X` where X is an enclosing FUNCTOR PARAMETER (no cmi on disk): its
  // sig's type names were harvested into param_sig_type_names_ at param
  // registration; register each `t` -> `X.t` so a bare use afterwards resolves
  // to the qualified path (shallow2deep's `'a op` -> `'a X.op`).
  void load_open_param_type_quals(const Longident& m) {
    auto* pl = std::get_if<Lident>(&m.v);
    if (!pl) return;
    auto it = param_sig_type_names_.find(pl->name);
    if (it == param_sig_type_names_.end()) return;
    for (auto& t : it->second)
      opened_type_quals_[t] = pl->name + "." + t;
  }
  void load_open_type_quals(const Longident& m) {
    auto comps = mod_components(m);
    if (comps.empty() || comps[0] == "Stdlib") return;
    std::string full = lid_full(m);
    if (full.rfind("Stdlib.", 0) == 0) return;
    try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
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
  // `open M` where M is a LOCAL module declared in this file (no cmi on disk):
  // register each type it declares as a bare -> `M.t` qual, so a following
  // annotation `(x : t)` resolves to `M.t`.  load_open_type_quals only reaches
  // on-disk modules; without this a bare opaque type (variant/record) fell to
  // an unresolved constr and was dropped to a fresh var (robustmatch's
  // `f (t1 : x t)` under `open M`).  Abbreviations already resolve via the flat
  // type_aliases map; registering them here too is harmless (same `M.t` path).
  void load_open_local_type_quals(const Longident& m) {
    auto* pl = std::get_if<Lident>(&m.v);
    if (!pl) return;
    auto it = module_direct_types_.find(pl->name);
    if (it == module_direct_types_.end()) return;
    for (auto& n : it->second) opened_type_quals_[n] = pl->name + "." + n;
  }
  // Simple module name -> the type names DIRECTLY declared in its struct body,
  // filled by register_types_rec (which already descends every module).  Used
  // only to seed opened_type_quals_ on `open M` for a local module.
  std::unordered_map<std::string, std::vector<std::string>> module_direct_types_;
  // A submodule `Array1` of an opened `Bigarray`: `Array1.t` / `open Array1`
  // qualifies through `Bigarray.Array1`.  Runs in EVERY pass (strict needs the
  // reroute to resolve `open M; ... x` for M a sibling/otherlibs submodule).
  void load_open_submod_quals(const Longident& m) {
    auto comps = mod_components(m);
    if (comps.empty() || comps[0] == "Stdlib") return;
    std::string full = lid_full(m);
    if (full.rfind("Stdlib.", 0) == 0) return;
    try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig) {
        for (auto& mm : sig->modules) opened_submod_quals_[mm.name] = full + "." + mm.name;
        // Its MODULE TYPES too: `open Globroots` then `(G : GLOBREF)` cites
        // Globroots.GLOBREF (the cmi writer records the qualified Mty_ident).
        for (auto& mt : sig->modtypes)
          opened_modtype_quals_[mt.name] = full + "." + mt.name;
      }
    } catch (...) {}
  }
  std::unordered_map<std::string, std::string> opened_modtype_quals_;
  // `open M` where M's cmi declares module ALIASES (StdLabels's `module List =
  // ListLabels`): a bare `List.map` afterwards resolves through the alias
  // target, so the LABELLED map applies (`List.map xs ~f`).  bare name ->
  // target path components.
  std::unordered_map<std::string, std::vector<std::string>> opened_module_aliases_;
  void load_open_module_aliases(const Longident& m) {
    auto comps = mod_components(m);
    if (comps.empty()) return;
    try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
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
      const auto& cmi = cmi::CmiFile::load(stdpath("stdlib.cmi"));
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
                                   std::deque<const cmi::CmiFile*>& loaded) {
    if (!mt) return nullptr;
    if (mt->kind == cmi::ModuleType::Sig) return mt->sig.get();
    if (mt->kind == cmi::ModuleType::Alias && mt->path && loaded.size() < 16) {
      std::string p = cmi_path_str(*mt->path);  // e.g. "Stdlib__Array"
      if (p.empty()) return nullptr;
      std::string low = p;
      if (low[0] >= 'A' && low[0] <= 'Z') low[0] += 32;  // first-char-lowercased
      try {
        loaded.push_back(&cmi::CmiFile::load(stdpath(low + ".cmi")));
        return &loaded.back()->sig();
      } catch (...) {}
      // Not a stdlib-dir unit: an alias to a separately compiled unit
      // (`module A2235 = A2235` in a sibling lib) resolves through the same
      // search as any head module (stdlib__X naming + the -I dirs).
      std::string unit = p.rfind("Stdlib__", 0) == 0 ? p.substr(8)
                       : p.rfind("Stdlib.", 0) == 0 ? p.substr(7)
                                                    : p;
      if (unit.find('.') == std::string::npos) {
        try {
          loaded.push_back(&cmi::CmiFile::load(head_cmi(unit)));
          return &loaded.back()->sig();
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
  // Is this coretype a CLOSED simple shape (constrs/tuples/arrows of concrete
  // names, no type variables)?  Conservative: anything else says no, so the
  // phantom-abbreviation fold below only fires on shapes we can translate.
  static bool closed_simple_coretype(const CoreType& t) {
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      for (auto& a : c->args)
        if (!closed_simple_coretype(*a)) return false;
      return true;
    }
    if (auto* ar = std::get_if<Ptyp_arrow>(&t.desc))
      return closed_simple_coretype(*ar->dom) && closed_simple_coretype(*ar->cod);
    if (auto* tp = std::get_if<Ptyp_tuple>(&t.desc)) {
      for (auto& e : tp->elems)
        if (!closed_simple_coretype(*e)) return false;
      return true;
    }
    return false;
  }
  std::unordered_map<std::string, TypePtr> sig_items_value_schemes(
      const ast::Signature& items,
      const std::unordered_map<std::string, TypePtr>& argtypes,
      const std::string& qual = "") {
    std::unordered_map<std::string, TypePtr> out;
    auto saved = functor_result_abstract_;
    auto saved_pq = functor_param_type_quals_;
    auto saved_rp = row_phantom_aliases_;
    for (auto& it : items)
      if (auto* t = std::get_if<Psig_type>(&it.desc))
        for (auto& d : t->decls) {
          if (auto a = argtypes.find(d.name.txt); a != argtypes.end())
            functor_result_abstract_[d.name.txt] = a->second;
          // A functor PARAMETER's nullary type is the param's own (`(G :
          // GLOBREF)` makes bare `t` mean `G.t`), so body exports show the
          // qualified type instead of a fresh var.  MANIFESTED decls too:
          // `(X : sig type t = int val x : t end)` gives `val y = X.x` the
          // stored type X.t, exactly ocamlc (index_functor's G).
          else if (!qual.empty() && d.params.empty())
            functor_result_abstract_[d.name.txt] =
                eng.constr(qual + "." + d.name.txt, {});
          // A parameterized abbreviation with a CLOSED manifest (`type 'a u
          // = string`, phantom params) folds to the manifest at use sites --
          // ocamlc gives `let t = M.f ()` (f : unit -> 'a u t) the type
          // `string M.t`, not `'a M.u M.t`; a manifest citing its params
          // (`'a u = 'a list`) stays qualified (pr5663).
          else if (!qual.empty() && d.manifest &&
                   closed_simple_coretype(**d.manifest)) {
            std::unordered_map<std::string, TypePtr> mv;
            functor_result_abstract_[d.name.txt] =
                from_coretype(**d.manifest, mv);
          }
          // A ROW-PHANTOM alias (`type 'a mr = [< .. ] as 'a`): an application
          // `R mr` IS R tied to the bound, so the include/param scheme holds
          // the EXPANSION -- ocamlc's inferred use sites print the bare row
          // (pr7601's Make).  Registered for the val conversions below.
          else if (!d.params.empty() && d.manifest) {
            if (auto* al = std::get_if<Ptyp_alias>(&(*d.manifest)->desc);
                al && std::holds_alternative<Ptyp_variant>(al->type->desc)) {
              Alias a;
              for (auto& p : d.params) {
                auto* v = std::get_if<Ptyp_var>(&p->desc);
                a.params.push_back(v ? v->name : "");
              }
              bool is_param = false;
              for (auto& p : a.params) is_param |= (p == al->name);
              if (is_param) {
                a.manifest = d.manifest->get();
                row_phantom_aliases_[d.name.txt] = std::move(a);
              } else if (!qual.empty()) {
                functor_param_type_quals_[d.name.txt] = qual + "." + d.name.txt;
              }
            } else if (!qual.empty()) {
              functor_param_type_quals_[d.name.txt] = qual + "." + d.name.txt;
            }
          }
          else if (!qual.empty())
            functor_param_type_quals_[d.name.txt] = qual + "." + d.name.txt;
        }
    // Sig-local `module Env : S` binds Env for the val types that follow
    // (`val code0 : Env.in_t -> out0`); count it bound so strict's
    // unbound-module check can't false-fire (flat over-inclusive set, like
    // the other bound_module_names_ producers).
    auto saved_sq = opened_submod_quals_;
    for (auto& it : items)
      if (auto* md = std::get_if<Psig_module>(&it.desc)) {
        if (md->md.name.txt) {
          bound_module_names_.insert(*md->md.name.txt);
          // A functor PARAMETER's SUBMODULE member type is qualified through the
          // param (`(D : sig module Data : sig type t end val key : Data.t t end)`
          // makes `Data.t` mean `D.Data.t`), so a body val citing it exports the
          // param-qualified path -- pr7152's `Register (D:S)`'s key : D.Data.t t.
          if (!qual.empty())
            opened_submod_quals_[*md->md.name.txt] = qual + "." + *md->md.name.txt;
        }
      }
    for (auto& it : items)
      if (auto* v = std::get_if<Psig_value>(&it.desc)) {
        std::unordered_map<std::string, TypePtr> vars;
        out[v->vd.name.txt] = from_coretype(*v->vd.type, vars);
      }
    functor_result_abstract_ = std::move(saved);
    functor_param_type_quals_ = std::move(saved_pq);
    row_phantom_aliases_ = std::move(saved_rp);
    opened_submod_quals_ = std::move(saved_sq);
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
      const ModuleType& ps, const std::unordered_map<std::string, TypePtr>& argtypes,
      const std::string& qual = "") {
    std::unordered_map<std::string, TypePtr> out;
    if (auto* sg = std::get_if<Pmty_signature>(&ps.desc)) {
      return sig_items_value_schemes(sg->items, argtypes, qual);
    } else if (auto* mi = std::get_if<Pmty_ident>(&ps.desc)) {
      // A LOCAL `module type S = sig .. end` first (so a functor param `M : S`
      // whose S is defined in this file resolves M's members), else a cmi one.
      if (auto* l = std::get_if<Lident>(&mi->id.txt.v)) {
        auto it = modtype_sig_asts_.find(l->name);
        if (it != modtype_sig_asts_.end())
          return sig_items_value_schemes(*it->second, argtypes, qual);
      }
      out = cmi_modtype_value_schemes(mi->id.txt, argtypes, qual);
    } else if (auto* mw = std::get_if<Pmty_with>(&ps.desc)) {
      return param_sig_value_schemes(*mw->mt, argtypes, qual);
    }
    return out;
  }

  // Functor-param DATATYPE members: `(X : T)` where T declares variants /
  // records / typexts.  Qualified body uses (`X.A`, `{ X.x = () }`, `X.E`)
  // resolve through these; without them the body's exported vals degrade to
  // fresh vars (index_functor's I).  Ctor schemes land in
  // param_ctor_schemes_ ("X.A" -> generic arg1->..->result); record labels
  // land in the flat fields_ map (emplace -- a local record's own labels,
  // registered later by run_checker's assignment, still win).
  std::unordered_map<std::string, TypePtr> param_ctor_schemes_;
  // Solve a functor-param decl's constraints against an application's args
  // (`'a A.t` under A : Poly pins 'a to `[> ]`, pr4775).  Returns the PHANTOM
  // expansion (a bare-var manifest bound to a param -> that arg) or null;
  // callers in constraint position substitute it, manifest callers keep the
  // fold and take the side effect only.
  TypePtr solve_param_sig_constraints(const TypePtr& t0) {
    if (strict) return nullptr;
    TypePtr t = I::Engine::repr(t0);
    if (t->kind != I::Type::Kind::Constr) return nullptr;
    auto pd = param_sig_type_decls_.find(t->path);
    if (pd == param_sig_type_decls_.end() ||
        pd->second.params.size() != t->args.size() || expanding_.count(t->path))
      return nullptr;
    std::unordered_map<std::string, TypePtr> sub;
    for (std::size_t i = 0; i < t->args.size(); ++i)
      if (!pd->second.params[i].empty()) sub[pd->second.params[i]] = t->args[i];
    expanding_.insert(t->path);
    if (pd->second.constraints)
      for (auto& tc : *pd->second.constraints)
        soft_unify(from_coretype(*tc.t1, sub), from_coretype(*tc.t2, sub));
    TypePtr rep = nullptr;
    if (pd->second.manifest)
      if (auto* v = std::get_if<Ptyp_var>(&pd->second.manifest->desc))
        if (auto s = sub.find(v->name); s != sub.end()) rep = s->second;
    expanding_.erase(t->path);
    return rep;
  }
  void register_param_sig_members(const std::string& pn, const ModuleType& ps) {
    const ast::Signature* items = nullptr;
    if (auto* sg = std::get_if<Pmty_signature>(&ps.desc)) items = &sg->items;
    else if (auto* mi = std::get_if<Pmty_ident>(&ps.desc)) {
      if (auto* l = std::get_if<Lident>(&mi->id.txt.v))
        if (auto it = modtype_sig_asts_.find(l->name);
            it != modtype_sig_asts_.end())
          items = it->second;
    } else if (auto* mw = std::get_if<Pmty_with>(&ps.desc)) {
      return register_param_sig_members(pn, *mw->mt);
    }
    if (!items) return;
    auto reg_ctor = [&](const std::string& cname, const TypePtr& result,
                        const ConstructorArguments& cargs,
                        std::unordered_map<std::string, TypePtr>& vars) {
      TypePtr scheme = result;
      if (auto* tup = std::get_if<Pcstr_tuple>(&cargs)) {
        for (auto it2 = tup->elems.rbegin(); it2 != tup->elems.rend(); ++it2)
          scheme = eng.arrow(from_coretype(**it2, vars), scheme);
      } else if (std::get_if<Pcstr_record>(&cargs)) {
        // Inline record: a loose one-arg arrow (the payload types stay
        // unmodelled; the RESULT type is what the export needs).
        scheme = eng.arrow(generic_var(), scheme);
      }
      param_ctor_schemes_[pn + "." + cname] = scheme;
    };
    for (auto& it : *items) {
      if (auto* t = std::get_if<Psig_type>(&it.desc)) {
        for (auto& d : t->decls) {
          param_sig_type_names_[pn].push_back(d.name.txt);
          note_type_kind(d);
          if (d.manifest || !d.constraints.empty()) {
            Alias a;
            for (auto& p : d.params) {
              auto* v = std::get_if<Ptyp_var>(&p->desc);
              a.params.push_back(v ? v->name : "");
            }
            a.manifest = d.manifest ? d.manifest->get() : nullptr;
            if (!d.constraints.empty()) a.constraints = &d.constraints;
            param_sig_type_decls_[pn + "." + d.name.txt] = std::move(a);
          }
          std::unordered_map<std::string, TypePtr> vars;
          std::vector<TypePtr> params;
          for (auto& p : d.params) params.push_back(from_coretype(*p, vars));
          TypePtr result = eng.constr(pn + "." + d.name.txt, params);
          if (auto* var = std::get_if<Ptype_variant>(&d.kind)) {
            for (auto& c : var->ctors) {
              TypePtr res = c.res ? from_coretype(**c.res, vars) : result;
              reg_ctor(c.name.txt, res, c.args, vars);
            }
          } else if (auto* rec = std::get_if<Ptype_record>(&d.kind)) {
            for (auto& f : rec->fields)
              fields_.emplace(f.name.txt,
                              eng.arrow(result, from_coretype(*f.type, vars)));
          }
        }
      } else if (auto* tx = std::get_if<Psig_typext>(&it.desc)) {
        // `type e += E ..`: the EXTENDED type keeps its own path (the
        // enclosing scope's e, not X.e).
        std::unordered_map<std::string, TypePtr> vars;
        std::vector<TypePtr> params;
        for (auto& p : tx->ext.params) params.push_back(from_coretype(*p, vars));
        TypePtr result = eng.constr(lid_full(tx->ext.path.txt), params);
        for (auto& c : tx->ext.ctors)
          if (auto* dc = std::get_if<Pext_decl>(&c.kind)) {
            TypePtr res = dc->res ? from_coretype(**dc->res, vars) : result;
            reg_ctor(c.name.txt, res, dc->args, vars);
          }
      }
    }
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
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
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
            // Qualify same-module type refs in the manifest (Seq.t's `unit -> 'a
            // node` mentions `node`, a Pident that must resolve to `Seq.node`)
            // -- otherwise the bare "node" leaks and unification/display treats
            // it as an unknown local (it renders as a bare var).  Mirror the
            // other cmi-abbrev expansion site: point cmi context at the owning
            // module's sig for the from_cmi call.
            const std::vector<cmi::TypeDecl>* saved_ctx = cmi_types_ctx_;
            std::string saved_pfx = cmi_mod_prefix_;
            auto saved_scopes = cmi_scopes_;
            cmi_scopes_.clear();
            cmi_types_ctx_ = &sig->types;
            cmi_mod_prefix_.clear();
            for (size_t i = 0; i + 1 < comps.size(); ++i) {
              if (!cmi_mod_prefix_.empty()) cmi_mod_prefix_ += '.';
              cmi_mod_prefix_ += comps[i];
            }
            TypePtr r = from_cmi(td.manifest, memo);
            cmi_types_ctx_ = saved_ctx;
            cmi_mod_prefix_ = saved_pfx;
            cmi_scopes_ = saved_scopes;
            abbrev_exp_neg_.erase(path);
            if (r && args.empty()) abbrev_exp_cache_[path] = r;
            return r;
          }
    } catch (...) {}
    return nullptr;
  }

  std::unordered_map<std::string, TypePtr> cmi_modtype_value_schemes(
      const Longident& path, const std::unordered_map<std::string, TypePtr>& argtypes,
      const std::string& qual = "") {
    std::unordered_map<std::string, TypePtr> out;
    auto comps = mod_components(path);
    if (comps.empty()) return out;
    // A BARE name declared by an opened module (`open Globroots` then
    // `(G : GLOBREF)`) resolves through the open's qualification.
    if (comps.size() == 1)
      if (auto q = opened_modtype_quals_.find(comps[0]);
          q != opened_modtype_quals_.end())
        comps = mod_components_str(q->second);
    try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
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
      // With a qualifier (a functor PARAM's name), local abstract types take
      // the param-qualified path: `(G : GLOBREF)` types G.register as
      // string -> G.t, not string -> 'a (globroots).
      if (!qual.empty()) {
        cmi_types_ctx_ = &mt->sig->types;
        cmi_mod_prefix_ = qual;
        func_result_mode_ = true;
      }
      for (auto& v : mt->sig->values) {
        std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
        out[v.name] = from_cmi(v.type, memo);
        eng.finalize_family_heads(out[v.name], /*scheme=*/true);
      }
      cmi_abstract_subst_.clear();
      cmi_types_ctx_ = nullptr; cmi_mod_prefix_.clear(); func_result_mode_ = false;
    } catch (...) {
      cmi_abstract_subst_.clear();
      cmi_types_ctx_ = nullptr; cmi_mod_prefix_.clear(); func_result_mode_ = false;
    }
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
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(head)));
      const cmi::Signature* sig = &loaded.back()->sig();
      // ocamlc's `Mtype.strengthen` roots a cmi-loaded value's type references at
      // the ACCESS path P of the value's own module (alias route, `Bigarray.Array1`
      // -> `Stdlib.Bigarray.Array1`) -- but ONLY for types owned by that module or
      // a descendant.  A type owned by a STRICT ANCESTOR of the value's module is
      // free in the accessed signature and keeps the CANONICAL compilation-unit
      // path baked in the cmi (`Array1.create`'s `kind` -> `Stdlib__Bigarray.kind`,
      // not `Stdlib.Bigarray.kind`).  So ancestor scopes use the canonical head; the
      // deepest (accessed) scope and its submodules use the alias head.
      std::string canon_head = loaded.back()->module_name();
      if (canon_head.empty()) canon_head = comps[0];
      // Record each module level's (types, cumulative-prefix) as an enclosing scope.
      // Ancestor levels carry the canonical prefix; the accessed (deepest) level is
      // rewritten to the alias prefix once the walk finishes.
      std::vector<std::pair<const std::vector<cmi::TypeDecl>*, std::string>> scopes;
      std::vector<std::pair<const std::vector<cmi::ModuleDecl>*, std::string>> mscopes;
      std::string alias_pfx = comps[0];       // access route, joined from comps
      std::string canon_pfx = canon_head;     // canonical unit-rooted route
      scopes.push_back({&sig->types, canon_pfx});
      mscopes.push_back({&sig->modules, canon_pfx});
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
        if (sig) {
          alias_pfx += "." + comps[i];
          canon_pfx += "." + comps[i];
          scopes.push_back({&sig->types, canon_pfx});
          mscopes.push_back({&sig->modules, canon_pfx});
        }
      }
      // The accessed module (deepest scope) and its own members use the alias route.
      if (sig && !scopes.empty()) {
        scopes.back().second = alias_pfx;
        mscopes.back().second = alias_pfx;
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
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
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
  // Walk a written type and load the record fields of every qualified type's
  // module (`M.t` -> M).  Called on a binding's declared type BEFORE the body is
  // inferred, so a label the annotation's module shares with another record is
  // already AMBIGUOUS when the body's field access is typed -- otherwise the
  // body pins the base to whichever module happened to be loaded first (a value
  // reference elsewhere in the unit).  Codegen pass only.
  void preload_annot_record_fields(const CoreType& t) {
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      if (auto* d = std::get_if<Ldot>(&c->id.txt.v)) load_module_record_fields(*d->prefix);
      for (auto& a : c->args) preload_annot_record_fields(*a);
    } else if (auto* ar = std::get_if<Ptyp_arrow>(&t.desc)) {
      preload_annot_record_fields(*ar->dom); preload_annot_record_fields(*ar->cod);
    } else if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
      for (auto& x : tu->elems) preload_annot_record_fields(*x);
    }
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
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
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
      const auto& cmi = cmi::CmiFile::load(head_cmi(head));
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
      // A NAMED result modtype (`module F() : Ret`): resolve Ret in the
      // signature the functor was found in, like the param side above
      // (missing_set_of_closures' `let module X = A.F() in X.g`).
      if (cur && cur->kind == cmi::ModuleType::Ident && cur->path && parent) {
        const cmi::Path* p = cur->path.get();
        const std::string& nm = p->kind == cmi::Path::Pdot ? p->s : p->id.name;
        cur = nullptr;
        for (auto& mtd : parent->modtypes)
          if (mtd.name == nm) { cur = mtd.type.get(); break; }
      }
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
  // Bind a cmi record decl's OWN parameters to the base expression's type
  // arguments, so a polymorphic field reads at the instance type: `txt` of
  // `string Asttypes.loc` is `string`, not a fresh var (which would leave
  // typedecl's `name = pld.pld_name.txt` a polymorphic caml_equal).  A no-op
  // unless the arities line up.  from_cmi memoizes on the LINK-FOLLOWED node,
  // so follow links here too.
  static void seed_decl_params(std::unordered_map<cmi::TypeExpr*, TypePtr>& memo,
                               const cmi::TypeDecl& td,
                               const std::vector<TypePtr>& args) {
    if (args.size() != td.params.size()) return;
    for (size_t i = 0; i < td.params.size(); ++i) {
      const cmi::TypeExpr* n = td.params[i].get();
      while (n && (n->kind == cmi::TypeExpr::Tlink || n->kind == cmi::TypeExpr::Tsubst))
        n = n->link.get();
      if (n && (n->kind == cmi::TypeExpr::Tvar || n->kind == cmi::TypeExpr::Tunivar))
        memo[const_cast<cmi::TypeExpr*>(n)] = args[i];
    }
  }
  // The declared type of a stdlib (sub)module's record field (e.g. Gc.control's
  // `minor_heap_size`), instantiated into our type universe; null if not found.
  // The lookup is by LABEL alone, so `inst` (the base's type arguments) is
  // applied only when the record we land on is the base's own -- `tyname` names
  // it; empty leaves the field's declared type unsubstituted, as before.
  TypePtr stdlib_field_type(const std::string& mod, const std::string& label,
                            const std::string& tyname = "",
                            const std::vector<TypePtr>* inst = nullptr) {
    try {
      const auto& cmi = cmi::CmiFile::load(head_cmi(mod));
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Record) continue;
        for (auto& l : td.labels)
          if (l.name == label) {
            std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
            if (inst && !tyname.empty() && td.name == tyname)
              seed_decl_params(memo, td, *inst);
            return from_cmi(l.type, memo);
          }
      }
    } catch (...) {}
    return nullptr;
  }
  // Resolve `<typepath>.<label>` where typepath is a base expression's full
  // inferred type path (e.g. "Stdlib.Lexing.position" or "Gc.stat").  The record
  // lives in the LEAF module (the component just before the type name); we verify
  // the record decl's own name matches the type name, so a same-labelled field in
  // an unrelated record can't mis-resolve the field's type (which would mis-drive
  // comparison/kind specialization).  Handles the wrapped-stdlib multi-component
  // path (`Stdlib.Lexing.position`) that stdlib_field_type's single-module lookup
  // cannot -- otherwise `loc.loc_start.pos_lnum` chains leak a fresh var.
  TypePtr typepath_field_type(const std::string& typepath, const std::string& label,
                              const std::vector<TypePtr>* inst = nullptr) {
    auto tpos = typepath.rfind('.');
    if (tpos == std::string::npos) return nullptr;
    std::string tyname = typepath.substr(tpos + 1);
    std::string modpath = typepath.substr(0, tpos);
    auto mpos = modpath.rfind('.');
    std::string leaf_mod = (mpos == std::string::npos) ? modpath
                                                       : modpath.substr(mpos + 1);
    try {
      const auto& cmi = cmi::CmiFile::load(head_cmi(leaf_mod));
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Record || td.name != tyname) continue;
        for (auto& l : td.labels)
          if (l.name == label) {
            std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
            if (inst) seed_decl_params(memo, td, *inst);
            return from_cmi(l.type, memo);
          }
      }
    } catch (...) {}
    return nullptr;
  }
  void register_stdlib_ctors() {
    try {
      const auto& cmi = cmi::CmiFile::load(stdpath("stdlib.cmi"));
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
  // Bare (unqualified) type names cited anywhere in a core type -- unlike
  // collect_alias_refs (cyclic-abbreviation check) it recurses into variant
  // rows and doesn't filter by the alias map: it feeds the decl-position
  // qual snapshot (manifest_decl_quals_).
  void collect_bare_type_names(const CoreType& t, std::set<std::string>& out) {
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      if (auto* l = std::get_if<Lident>(&c->id.txt.v)) out.insert(l->name);
      for (auto& a : c->args) collect_bare_type_names(*a, out);
    } else if (auto* a = std::get_if<Ptyp_arrow>(&t.desc)) {
      collect_bare_type_names(*a->dom, out);
      collect_bare_type_names(*a->cod, out);
    } else if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
      for (auto& e : tu->elems) collect_bare_type_names(*e, out);
    } else if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) {
      collect_bare_type_names(*al->type, out);
    } else if (auto* pv = std::get_if<Ptyp_variant>(&t.desc)) {
      for (auto& r : pv->rows) {
        if (auto* rt = std::get_if<Rtag>(&r))
          for (auto& ty : rt->types) collect_bare_type_names(*ty, out);
        else if (auto* ri = std::get_if<Rinherit>(&r))
          collect_bare_type_names(*ri->ct, out);
      }
    } else if (auto* cl = std::get_if<Ptyp_class>(&t.desc)) {
      for (auto& a : cl->args) collect_bare_type_names(*a, out);
    } else if (auto* po = std::get_if<Ptyp_poly>(&t.desc)) {
      collect_bare_type_names(*po->type, out);
    }
  }
  // ---- Variance computation (Typedecl_variance port, cmi-writer side) ----
  // ocamlc COMPUTES type_variance for concrete decls; the reader's subtype/
  // unify checks consume it (patterns.cmi's `Half_simple.pattern :>
  // General.pattern` width coercion was rejected because pattern_data's
  // param carried Variance.unknown instead of the computed covariance).
  // Computed (finalized) variances of decls already emitted by this checker,
  // keyed by bare name -- consulted when a later decl's body cites them.
  std::unordered_map<std::string, std::vector<int>> computed_variances_;
  // The variance signature of an EXTERNAL type constructor: predef table,
  // locally computed decls, or the head unit's cmi (reader-parsed
  // td.variances).  Empty = unknown (caller walks args with unknown).
  std::vector<int> external_type_variance_sig(const Ptyp_constr& c,
                                              std::size_t arity) {
    static const int COV = 25, FULL = 63;
    auto builtin = [&](const std::string& n) -> std::vector<int> {
      if (arity == 1 &&
          (n == "list" || n == "option" || n == "lazy_t" || n == "iarray" ||
           n == "Lazy.t" || n == "Seq.t"))
        return {COV};
      if (arity == 1 && (n == "array" || n == "ref" || n == "atomic_loc" ||
                         n == "eff" || n == "Atomic.t"))
        return {FULL};
      if (arity == 2 && n == "result") return {COV, COV};
      if (arity == 2 && n == "continuation") return {46, COV};
      return {};
    };
    std::string dotted;
    if (auto* l = std::get_if<Lident>(&c.id.txt.v)) {
      if (auto f = computed_variances_.find(l->name);
          f != computed_variances_.end() && f->second.size() == arity)
        return f->second;
      if (auto b = builtin(l->name); !b.empty()) return b;
      if (auto q = opened_type_quals_.find(l->name);
          q != opened_type_quals_.end())
        dotted = q->second;
      else
        return {};
    } else if (std::holds_alternative<Ldot>(c.id.txt.v)) {
      dotted = lid_full(c.id.txt);
    } else {
      return {};
    }
    if (auto b = builtin(dotted); !b.empty()) return b;
    // Navigate the head unit's cmi (same pattern as expand_qualified_abbrev).
    auto dp = dotted.rfind('.');
    if (dp == std::string::npos) return {};
    std::string tyname = dotted.substr(dp + 1);
    std::vector<std::string> comps;
    for (std::size_t p = 0, d2; p < dp; p = d2 + 1) {
      d2 = dotted.find('.', p);
      if (d2 == std::string::npos || d2 > dp) d2 = dp;
      comps.push_back(dotted.substr(p, d2 - p));
    }
    try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
      for (std::size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig)
        for (auto& td : sig->types)
          if (td.name == tyname && td.variances.size() == arity)
            return std::vector<int>(td.variances.begin(), td.variances.end());
    } catch (...) {}
    return {};
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

  // Record a type declaration's concrete/abstract kind for array_kind_str, so an
  // abstract element (`type act`) drops its `[addr]` array specialization (see
  // abstract_names_).  An abstract type WITH a manifest is a transparent
  // abbreviation -- classify scrapes through it -- so it is neither abstract nor
  // algebraic here.
  void note_type_kind(const TypeDeclaration& d) {
    if (std::holds_alternative<Ptype_record>(d.kind) ||
        std::holds_alternative<Ptype_variant>(d.kind) ||
        std::holds_alternative<Ptype_open>(d.kind))
      algebraic_names_.insert(d.name.txt);
    else if (std::holds_alternative<Ptype_abstract>(d.kind) && !d.manifest)
      abstract_names_.insert(d.name.txt);
  }

  // ---- array_kind_str pre-pass: collect type-decl kinds file-wide -------------
  // A named module type's abstract-without-manifest top-level type names, so a
  // functor param `(A : S)` can resolve `A.t`'s kind.
  std::unordered_map<std::string, std::set<std::string>> modtype_abstract_;
  // Deferred `(param, modtype-name)` functor params, resolved against
  // modtype_abstract_ once the whole file (all `module type` decls) is walked.
  std::vector<std::pair<std::string, std::string>> pending_param_modtypes_;

  // Abstract-without-manifest type names declared at a module type's TOP level
  // (only a literal signature exposes them structurally).
  std::set<std::string> mty_top_abstract(const ast::ModuleType& mt) {
    std::set<std::string> out;
    if (auto* sg = std::get_if<ast::Pmty_signature>(&mt.desc))
      for (auto& it : sg->items)
        if (auto* t = std::get_if<ast::Psig_type>(&it.desc))
          for (auto& d : t->decls)
            if (std::holds_alternative<ast::Ptype_abstract>(d.kind) && !d.manifest)
              out.insert(d.name.txt);
    return out;
  }
  // Memoized: does a DOTTED type path ("Obj.t", "Stdlib.Obj.t") name an
  // ABSTRACT (or external) manifest-less type in the owning unit's cmi?  This
  // is Env.find_type + Typeopt.classify's `Type_abstract _ | Type_external _
  // -> Any` case: such an array element is GENERIC.  A local module shadowing
  // a unit name is excluded by the caller (bound_module_names_).
  std::unordered_map<std::string, bool> cmi_abstract_memo_;
  bool cmi_type_is_abstract(const std::string& path) {
    if (auto it = cmi_abstract_memo_.find(path); it != cmi_abstract_memo_.end())
      return it->second;
    cmi_abstract_memo_[path] = false;  // re-entrancy guard (manifest cycles)
    bool r = false;
    std::vector<std::string> comps = mod_components_str(path);
    if (comps.size() >= 2) try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
      for (size_t i = 1; i + 1 < comps.size() && sig; ++i) {
        // A functor-APPLICATION component (`Make(T)`): the member kinds come
        // from the functor's RESULT signature.  No argument substitution is
        // needed to judge abstractness -- a result manifest stays a manifest
        // under substitution, and Env.find_type on a Papply preserves the
        // decl's kind.  Unwrap one Functor layer per application.
        std::string name = comps[i];
        int applications = 0;
        if (auto par = name.find('('); par != std::string::npos) {
          for (char c : name) applications += c == '(';
          name = name.substr(0, par);
        }
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == name) { md = &mm; break; }
        if (!md) { sig = nullptr; break; }
        cmi::ModuleTypePtr mt = md->type;
        for (int a = 0; a < applications && mt; ++a)
          mt = mt->kind == cmi::ModuleType::Functor ? mt->functor_body : nullptr;
        sig = module_sig(mt, loaded);
      }
      if (sig)
        for (auto& td : sig->types)
          if (td.name == comps.back()) {
            if (td.kind != cmi::TypeDecl::Abstract &&
                td.kind != cmi::TypeDecl::External)
              break;  // record/variant/open: concrete
            if (!td.manifest) { r = true; break; }
            // A manifest = an abbreviation: scrape_ty follows the chain to the
            // TERMINAL decl (a strengthened cmi spells `module Set`'s member as
            // `type t = Stdlib.Set.Make(T).t`, whose terminal is abstract).
            // Hop through link nodes, then recurse on a dotted Tconstr head;
            // any other shape (tuple/arrow/base) is concrete -- addr/int both
            // take the _addr accessor in bytecode, so stopping is exact there.
            const cmi::TypeExpr* m = td.manifest.get();
            while (m && (m->kind == cmi::TypeExpr::Tlink ||
                         m->kind == cmi::TypeExpr::Tsubst))
              m = m->link.get();
            if (m && m->kind == cmi::TypeExpr::Tconstr && m->path) {
              std::string mp = cmi_path_str(*m->path);
              if (mp != path && mp.find('.') != std::string::npos)
                r = cmi_type_is_abstract(mp);
            }
            break;
          }
    } catch (...) {}
    cmi_abstract_memo_[path] = r;
    return r;
  }
  // Is the CROSS-MODULE type at `path` marked Type_immediacy.Always in its
  // owning unit's cmi (an all-constant variant like Clflags.Compiler_pass.t,
  // or `[@@immediate]`)?  This is Env.find_type + Ctype.immediacy: ocamlc's
  // value_kind/maybe_pointer consult the decl's type_immediate FIRST, so every
  // kind site (comparison specialization, block shapes, param annotations)
  // sees [int].  Always only -- Always_on_64bits is NOT immediate in bytecode
  // (Typeopt.is_immediate).  The decl's own flag already accounts for a
  // manifest (Typedecl_immediacy computes it through the abbreviation), so no
  // manifest chase is needed.  Same walk as cmi_type_is_abstract above.
  std::unordered_map<std::string, bool> cmi_immediate_memo_;
  bool cmi_type_is_immediate(const std::string& path) {
    if (auto it = cmi_immediate_memo_.find(path); it != cmi_immediate_memo_.end())
      return it->second;
    bool r = false;
    std::vector<std::string> comps = mod_components_str(path);
    if (comps.size() >= 2) try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
      for (size_t i = 1; i + 1 < comps.size() && sig; ++i) {
        std::string name = comps[i];
        int applications = 0;
        if (auto par = name.find('('); par != std::string::npos) {
          for (char c : name) applications += c == '(';
          name = name.substr(0, par);
        }
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == name) { md = &mm; break; }
        if (!md) { sig = nullptr; break; }
        cmi::ModuleTypePtr mt = md->type;
        for (int a = 0; a < applications && mt; ++a)
          mt = mt->kind == cmi::ModuleType::Functor ? mt->functor_body : nullptr;
        sig = module_sig(mt, loaded);
      }
      if (sig)
        for (auto& td : sig->types)
          if (td.name == comps.back()) { r = td.immediate == 1; break; }
    } catch (...) {}
    cmi_immediate_memo_[path] = r;
    return r;
  }
  // Does the cross-module type ABBREVIATION `path` resolve (through its cmi
  // manifest chain) to the predefined `string`?  `Asttypes.label = string` in
  // Btype.hash_variant's signature is why a plain `l <> l'` on variant tags must
  // still specialize to caml_string_notequal.  kind_str's literal `b=="string"`
  // and is_unboxed_string (unboxed wrappers) both miss a bare abbreviation, so
  // walk the manifest here -- like cmi_type_is_immediate, but chasing the
  // manifest to `string` rather than reading td.immediate.  "string" only enables
  // the string-compare specialization (it is not a value_kind), and a genuine
  // abbreviation of string has string's runtime representation, so this cannot
  // mis-specialize.  Memoized; depth-bounded and cycle-guarded.
  std::unordered_map<std::string, bool> cmi_string_memo_;
  bool cmi_type_resolves_to_string(const std::string& path, int depth = 0) {
    if (depth > 8) return false;
    if (auto it = cmi_string_memo_.find(path); it != cmi_string_memo_.end())
      return it->second;
    cmi_string_memo_[path] = false;  // guard a cyclic abbreviation
    bool r = false;
    std::vector<std::string> comps = mod_components_str(path);
    if (comps.size() >= 2) try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
      for (size_t i = 1; i + 1 < comps.size() && sig; ++i) {
        std::string name = comps[i];
        int applications = 0;
        if (auto par = name.find('('); par != std::string::npos) {
          for (char c : name) applications += c == '(';
          name = name.substr(0, par);
        }
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == name) { md = &mm; break; }
        if (!md) { sig = nullptr; break; }
        cmi::ModuleTypePtr mt = md->type;
        for (int a = 0; a < applications && mt; ++a)
          mt = mt->kind == cmi::ModuleType::Functor ? mt->functor_body : nullptr;
        sig = module_sig(mt, loaded);
      }
      if (sig)
        for (auto& td : sig->types)
          if (td.name == comps.back()) {
            cmi::TypePtr m = td.manifest;
            while (m && (m->kind == cmi::TypeExpr::Tlink ||
                         m->kind == cmi::TypeExpr::Tsubst)) m = m->link;
            if (m && m->kind == cmi::TypeExpr::Tconstr && m->path) {
              std::string mp = cmi_path_str(*m->path);
              auto dd = mp.rfind('.');
              std::string mb = dd == std::string::npos ? mp : mp.substr(dd + 1);
              if (mb == "string") r = true;
              else if (dd != std::string::npos)  // a further named abbreviation
                r = cmi_type_resolves_to_string(mp, depth + 1);
            }
            break;
          }
    } catch (...) {}
    cmi_string_memo_[path] = r;
    return r;
  }
  // The abstract-without-manifest type names of a CROSS-MODULE named module
  // type ("Identifiable.S", "Map.S"), read from the owning unit's cmi -- the
  // cross-module analog of mty_top_abstract, for a functor param typed by a
  // foreign modtype (`Make (Id : Identifiable.S)`).
  std::set<std::string> cmi_modtype_abstract_names(const std::string& path) {
    std::set<std::string> out;
    std::vector<std::string> comps = mod_components_str(path);
    if (comps.size() < 2) return out;
    try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
      for (size_t i = 1; i + 1 < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (!sig) return out;
      const cmi::ModuleType* mt = nullptr;
      for (auto& mtd : sig->modtypes)
        if (mtd.name == comps.back()) { mt = mtd.type.get(); break; }
      if (mt && mt->kind == cmi::ModuleType::Sig && mt->sig)
        for (auto& td : mt->sig->types)
          if ((td.kind == cmi::TypeDecl::Abstract ||
               td.kind == cmi::TypeDecl::External) && !td.manifest)
            out.insert(td.name);
    } catch (...) {}
    return out;
  }

  void ck_param(const ast::FunctorParam& fp) {
    auto* nm = std::get_if<ast::Functor_named>(&fp);
    if (!nm || !nm->type) return;
    ck_walk_mty(*nm->type);
    std::string pn = nm->name.txt.value_or("");
    if (pn.empty()) return;
    for (auto& n : mty_top_abstract(*nm->type)) param_abstract_quals_.insert(pn + "." + n);
    if (auto* mi = std::get_if<ast::Pmty_ident>(&nm->type->desc)) {
      if (auto* l = std::get_if<ast::Lident>(&mi->id.txt.v))
        pending_param_modtypes_.push_back({pn, l->name});
      else if (std::holds_alternative<ast::Ldot>(mi->id.txt.v))
        pending_param_modtypes_.push_back({pn, lid_full(mi->id.txt)});
    }
  }
  void ck_walk_mty(const ast::ModuleType& mt) {
    if (auto* sg = std::get_if<ast::Pmty_signature>(&mt.desc)) ck_walk_sig(sg->items);
    else if (auto* fn = std::get_if<ast::Pmty_functor>(&mt.desc)) { ck_param(fn->param); ck_walk_mty(*fn->body); }
    else if (auto* w = std::get_if<ast::Pmty_with>(&mt.desc)) ck_walk_mty(*w->mt);
    else if (auto* to = std::get_if<ast::Pmty_typeof>(&mt.desc)) ck_walk_me(*to->me);
  }
  void ck_walk_me(const ast::ModuleExpr& me) {
    if (auto* ms = std::get_if<ast::Pmod_structure>(&me.desc)) ck_walk_struct(ms->items);
    else if (auto* fn = std::get_if<ast::Pmod_functor>(&me.desc)) { ck_param(fn->param); ck_walk_me(*fn->body); }
    else if (auto* c = std::get_if<ast::Pmod_constraint>(&me.desc)) { ck_walk_me(*c->me); ck_walk_mty(*c->mt); }
    else if (auto* a = std::get_if<ast::Pmod_apply>(&me.desc)) { ck_walk_me(*a->f); ck_walk_me(*a->arg); }
  }
  void ck_walk_sig(const ast::Signature& items) {
    for (auto& it : items) {
      if (auto* t = std::get_if<ast::Psig_type>(&it.desc)) for (auto& d : t->decls) note_type_kind(d);
      else if (auto* mt = std::get_if<ast::Psig_modtype>(&it.desc)) {
        if (mt->type) { modtype_abstract_[mt->name.txt] = mty_top_abstract(*mt->type); ck_walk_mty(*mt->type); }
      } else if (auto* md = std::get_if<ast::Psig_module>(&it.desc)) ck_walk_mty(*md->md.type);
      else if (auto* rm = std::get_if<ast::Psig_recmodule>(&it.desc)) for (auto& d : rm->decls) ck_walk_mty(*d.type);
      else if (auto* inc = std::get_if<ast::Psig_include>(&it.desc)) ck_walk_mty(inc->mt);
    }
  }
  void ck_walk_struct(const ast::Structure& items) {
    for (auto& it : items) {
      if (auto* t = std::get_if<ast::Pstr_type>(&it.desc)) for (auto& d : t->decls) note_type_kind(d);
      else if (auto* mt = std::get_if<ast::Pstr_modtype>(&it.desc)) {
        if (mt->type) { modtype_abstract_[mt->name.txt] = mty_top_abstract(*mt->type); ck_walk_mty(*mt->type); }
      } else if (auto* m = std::get_if<ast::Pstr_module>(&it.desc)) ck_walk_me(m->binding.expr);
      else if (auto* rm = std::get_if<ast::Pstr_recmodule>(&it.desc)) for (auto& b : rm->bindings) ck_walk_me(b.expr);
      else if (auto* inc = std::get_if<ast::Pstr_include>(&it.desc)) ck_walk_me(inc->expr);
    }
  }
  // Entry point: walk the whole file, then resolve deferred named-modtype params.
  void collect_type_kinds(const ast::Structure& s) {
    ck_walk_struct(s);
    for (auto& [pn, s2] : pending_param_modtypes_) {
      if (auto it = modtype_abstract_.find(s2); it != modtype_abstract_.end()) {
        for (auto& n : it->second) param_abstract_quals_.insert(pn + "." + n);
      } else if (s2.find('.') != std::string::npos) {
        // A CROSS-MODULE modtype (`(Id : Identifiable.S)`): its abstract type
        // names come from the owning unit's cmi.
        for (auto& n : cmi_modtype_abstract_names(s2))
          param_abstract_quals_.insert(pn + "." + n);
      }
    }
  }

  // Register a user variant: A of t1*..*tn -> scheme t1->..->tn->(params) name.
  void register_type_decl(const TypeDeclaration& d) {
    check_type_vars(d);
    check_unboxed(d);
    note_type_kind(d);
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
    // Opaque types (variant/record/extensible/abstract-without-manifest) have a
    // distinct nominal identity; pure abbreviations are transparent (expanded),
    // so unstamped.  An extensible `type t = ..` (Ptype_open) is nominal too --
    // without a stamp a nested `type t = ..` shadowing an enclosing `t` (its
    // `type t += ..` ctors, and any value matching on them) mis-resolved bare
    // `t` to the OUTER decl, so Printtyp disambiguated the cite as `t/2`.
    bool opaque = std::holds_alternative<Ptype_variant>(d.kind) ||
                  std::holds_alternative<Ptype_record>(d.kind) ||
                  std::holds_alternative<Ptype_open>(d.kind) ||
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
      // Decl-position snapshot (see manifest_decl_quals_): record the opened
      // qual of every bare name the manifest cites that isn't already a
      // local decl or alias -- a LATER local decl shadowing the name must
      // not capture this manifest's citation.  Skips names that resolve to
      // an existing alias/subst (use-time resolution prefers those anyway).
      std::set<std::string> bare;
      collect_bare_type_names(*d.manifest->get(), bare);
      std::vector<std::pair<std::string, std::string>> snap;
      for (auto& n : bare) {
        if (n == d.name.txt || local_declared_.count(n) ||
            type_aliases.count(n) || type_substs_.count(n))
          continue;
        if (auto q = opened_type_quals_.find(n); q != opened_type_quals_.end())
          snap.emplace_back(n, q->second);
      }
      if (!snap.empty())
        (*manifest_decl_quals_)[d.manifest->get()] = std::move(snap);
    }
    local_declared_.insert(d.name.txt);
    // A local `[@@unboxed]` single-field record wrapping a string compares via
    // caml_string_* (see is_unboxed_string); the variant form is handled below.
    if (auto* rec = std::get_if<Ptype_record>(&d.kind); rec && rec->fields.size() == 1) {
      bool unboxed = false;
      for (auto& a : d.attrs) if (a.name == "unboxed" || a.name == "ocaml.unboxed") unboxed = true;
      if (unboxed)
        if (auto* c = std::get_if<Ptyp_constr>(&rec->fields[0].type->desc))
          local_unboxed_inner_[d.name.txt] = lid_last(c->id.txt);
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
    // immediacy does not depend on is_gadt.  Record BOTH the bare name and the
    // module-qualified path: a use INSIDE the defining module infers the
    // qualified type path (`Separability.t`, from mod_prefix_), which kind_str
    // looks up verbatim -- without the qualified entry an in-module comparison
    // on the enum stayed the polymorphic caml_equal instead of the int `==`.
    if (all_const) {
      immediate_types_.insert(d.name.txt);
      if (!mod_prefix_.empty()) immediate_types_.insert(mod_prefix_ + d.name.txt);
    }
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
            // Record the wrapped head for the string-compare specialization; a
            // string-wrapper (`Compunit of string [@@unboxed]`) compares via
            // caml_string_* like OCaml's scrape_ty (see is_unboxed_string).
            local_unboxed_inner_[d.name.txt] = n;
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
      if (std::holds_alternative<Ptyp_poly>(f.type->desc)) {
        poly_field_rec_candidates_[f.name.txt].push_back({recTy, &*f.type});  // pattern record-type resolution
        // The kind pass ALSO registers a format-typed poly field as a mono
        // arrow so expression-side uses ({pf=Format.eprintf}) type string
        // literals at format type; the PATTERN side still binds the
        // generalized poly scheme (poly_field_rec_ wins there), so cross-use
        // instances don't clash through one var (domains.ml's test).
        if (record_kinds_ && mentions_format(*f.type)) {
          poly_format_labels_.insert(f.name.txt);
          field_candidates_[f.name.txt].push_back(
              eng.arrow(recTy, from_coretype(*f.type, vars)));
        }
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
    // The kind pass's own format-poly mono registration doesn't count as
    // "elsewhere" (it's the same declaration, see register_record_decl).
    for (auto& [k, v] : poly_field_rec_candidates_)
      if (v.size() == 1 &&
          (!field_candidates_.count(k) ||
           (poly_format_labels_.count(k) && field_candidates_[k].size() == 1)))
        poly_field_rec_[k] = v.front();
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
        // A DOTTED extended type (`type Common.msg += Reload ..`) keeps its
        // qualified path (typext_path semantics): a bare "msg" resolves to no
        // local decl, so the ctor's uses (and the cmi writer) degrade to a
        // var.  Non-strict only -- the strict pass would false-reject a
        // dotted-vs-bare unification against the module's own exports.
        std::string tpath = lid_last(te.path.txt);
        if (!strict && std::holds_alternative<Ldot>(te.path.txt.v)) {
          tpath = lid_full(te.path.txt);
          if (auto dot = tpath.find('.'); dot != std::string::npos)
            if (auto q = opened_submod_quals_.find(tpath.substr(0, dot));
                q != opened_submod_quals_.end())
              tpath = q->second + tpath.substr(dot);
        }
        // Cite the extended decl's own stamp (now that `type t = ..` is
        // stamped) so a value matching on this ctor carries the extended type's
        // identity, not an outer same-named decl's.  Bare tpath only; a dotted
        // path keeps the current unstamped (var-degrading) behaviour.
        int est = tpath.find('.') == std::string::npos
                      ? mx_resolve_bare_stamp(tpath, mod_prefix_)
                      : 0;
        result = is_exn ? eng.constr("exn") : eng.constr(tpath, params, est);
      }
      TypePtr scheme = result;
      if (auto* tup = std::get_if<Pcstr_tuple>(&d->args))
        for (auto it = tup->elems.rbegin(); it != tup->elems.rend(); ++it)
          scheme = eng.arrow(from_coretype(**it, vars), scheme);
      register_inline_record(d->args, result, vars);  // `type t += C of { f }`
      if (ctors.count(ec.name.txt)) ambiguous_ctors_.insert(ec.name.txt);
      ctors[ec.name.txt] = scheme;
      if (!is_exn) ext_ctor_scheme_[&ec] = scheme;  // for in-scope cenv overlay
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
    // Exception: in the NON-STRICT (dump/cmi) pass a predef name IS consulted in
    // the scoped cenv, so a value INSIDE the module that redefined `::`/`[]`
    // (GPR#234's `type hlist = [] | (::)`) resolves to the local ctor by lexical
    // scoping -- cenv only holds `::` where an enclosing decl bound it, so outer
    // uses still fall through to the predef.  Strict stays conservative (a
    // type-directed re-pick we can't do could otherwise false-reject).
    if (!exn_ctors_.count(name) && (!predef_ctors_.count(name) || !strict))
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

  // Scheme of a ctor QUALIFIED by a FILE-LOCAL module (`Datatype_kind.Record`):
  // resolve through the module's own registered per-type schemes
  // (type_ctor_schemes_ keys are "<ModPath>.<type>").  The bare last-in-scope
  // hit can be an UNRELATED type's same-named ctor (typecore's
  // wrong_kind_sort.Record, tag 1, squats "Record" over Datatype_kind's tag 0),
  // and qualified_ctor_scheme only knows cmi-loadable modules.  Null when the
  // qualifier isn't a local module or the ctor isn't declared there.
  TypePtr* local_module_ctor_scheme(const Longident& id) {
    auto* d = std::get_if<Ldot>(&id.v);
    if (!d) return nullptr;
    auto comps = mod_components(*d->prefix);
    if (comps.empty()) return nullptr;
    if (auto f = local_module_paths_.find(comps[0]);
        f != local_module_paths_.end() && !f->second.empty())
      comps[0] = f->second;  // `module DK = Datatype_kind` alias head
    std::string pref;
    for (auto& c : comps) { pref += c; pref += '.'; }
    for (auto& [key, lst] : type_ctor_schemes_) {
      if (key.compare(0, pref.size(), pref) != 0) continue;
      if (key.find('.', pref.size()) != std::string::npos) continue;  // deeper module
      for (auto& [cn2, sc] : lst)
        if (cn2 == d->name) return &sc;
    }
    return nullptr;
  }

  // A constructor's argument that is ITSELF a construct/pattern whose name is
  // shared across types (`ambiguous_ctors_`): record the enclosing ctor's
  // DECLARED argument type (`dom`) at the inner node, so the back end
  // disambiguates that inner ctor by the EXPECTED type rather than by lexical
  // scope -- scope may pick a same-named ctor of a different type whose TAG
  // differs (includemod's top-level `symptom` vs `Error.core_sigitem_symptom`).
  // infer_expr / infer_pat on a bare construct/pattern argument carries no
  // expected type, so this is the one place the domain is known.  The producer
  // (expr) and consumer (pat) forms MUST stay in lockstep.
  void record_ctor_arg_type(const Expression* arg, TypePtr dom) {
    if (!record_kinds_) return;
    auto* k = std::get_if<Pexp_construct>(&arg->desc);
    if (!k || !ambiguous_ctors_.count(lid_last(k->id.txt))) return;
    TypePtr d = I::Engine::repr(dom);
    if (d->kind == I::Type::Kind::Constr && !d->path.empty())
      ctor_arg_type_[arg] = d;
  }
  void record_pat_ctor_arg_type(const Pattern* arg, TypePtr dom) {
    if (!record_kinds_) return;
    auto* k = std::get_if<Ppat_construct>(&arg->desc);
    if (!k || !ambiguous_ctors_.count(lid_last(k->id.txt))) return;
    TypePtr d = I::Engine::repr(dom);
    if (d->kind == I::Type::Kind::Constr && !d->path.empty())
      pat_ctor_arg_type_[arg] = d;
  }
  // A RECORD pattern used as a constructor's argument (`Val_prim {prim_name=..}`):
  // pin its record type to the enclosing ctor's DECLARED argument type, so an
  // ambiguous field label (`prim_name`, in both Primitive.description@0 and
  // Typedtree.primitive_description@1) resolves by the EXPECTED type rather than
  // by field-set guessing -- infer_pat on a bare record arg carries no expected
  // type and can pick the wrong same-labelled record (OCaml's type-directed
  // record disambiguation).  Producer/consumer parity is not at stake here (a
  // record arg has one representation), but the FIELD INDEX is: reading
  // prim_arity (@1) where prim_name (@0) is meant crashes a downstream string
  // compare.
  void record_pat_record_arg_type(const Pattern* arg, TypePtr dom) {
    if (!record_kinds_) return;
    if (!std::holds_alternative<Ppat_record>(arg->desc)) return;
    TypePtr d = I::Engine::repr(dom);
    if (d->kind == I::Type::Kind::Constr && !d->path.empty())
      pat_record_arg_type_[arg] = d;
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
      const auto& cmi = cmi::CmiFile::load(head_cmi(pl->name));
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
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
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
      const auto& cmi = cmi::CmiFile::load(head_cmi(comps[0]));
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

  // The last component of the field/argument type wrapped by an `[@@unboxed]`
  // single-field type named `path` ("" if `path` is not such a wrapper).  Local
  // decls are recorded in local_unboxed_inner_; a module-qualified path is walked
  // through the owning cmi (mirroring cmi_record_fields).
  std::string unboxed_inner_head(const std::string& path) {
    if (auto it = local_unboxed_inner_.find(path); it != local_unboxed_inner_.end())
      return it->second;
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
      const auto& cmi = cmi::CmiFile::load(head_cmi(comps[0]));
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
      for (auto& td : *types) {
        if (td.name != tyname || !td.unboxed) continue;
        cmi::TypePtr inner;
        if (td.kind == cmi::TypeDecl::Record && !td.labels.empty())
          inner = td.labels[0].type;
        else if (td.kind == cmi::TypeDecl::Variant && !td.ctors.empty() &&
                 !td.ctors[0].args.empty())
          inner = td.ctors[0].args[0];
        if (inner && inner->kind == cmi::TypeExpr::Tconstr && inner->path) {
          std::string ip = cmi_path_str(*inner->path);
          size_t id = ip.rfind('.');
          return id == std::string::npos ? ip : ip.substr(id + 1);
        }
        return {};
      }
    } catch (...) {}
    return {};
  }
  // Does `path` name an `[@@unboxed]` single-field type whose representation is
  // (recursively) `string`?  Memoized; the cycle guard also bounds recursion.
  bool is_unboxed_string(const std::string& path) {
    if (path.empty()) return false;
    if (auto c = unboxed_string_cache_.find(path); c != unboxed_string_cache_.end())
      return c->second == 1;
    unboxed_string_cache_[path] = 0;  // guard against a cyclic wrapper
    std::string head = unboxed_inner_head(path);
    bool r = head == "string" || (!head.empty() && head != path && is_unboxed_string(head));
    unboxed_string_cache_[path] = r ? 1 : 0;
    return r;
  }

  TypePtr qualified_ctor_type(const Longident& id) {
    auto* d = std::get_if<Ldot>(&id.v);
    if (!d) return nullptr;
    auto* pl = std::get_if<Lident>(&d->prefix->v);
    if (!pl) return nullptr;  // nested-module qualifier: best-effort skip
    try {
      const auto& cmi = cmi::CmiFile::load(head_cmi(pl->name));
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
    // A functor PARAMETER's ctor (`X.A` under `(X : T)` -- registered from
    // the param signature) resolves before any cmi lookup.
    if (auto pc = param_ctor_schemes_.find(lid_full(id));
        pc != param_ctor_schemes_.end())
      return pc->second;
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
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* sig = &loaded.back()->sig();
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
    // `open M` where M is a local ALIAS of a unit SUBMODULE (typemod's `module
    // Sig_component_kind = Shape.Sig_component_kind` then `let open
    // Sig_component_kind in match ..`): open the TARGET's ctors, result-typed
    // at the full dotted path, so the match rows record a dotted pat_constr
    // and the back end force-registers the right type -- otherwise the bare
    // constant ctors resolved to Typedtree.item_declaration's same-named BLOCK
    // ctors and the match tag-tested immediates (bootstrap bug#13).
    // A DIRECTLY dotted open (`let open Patterns.Head in match ..`, parmatch)
    // walks the same component path; a local/unbound head just fails head_cmi
    // and falls out through the catch, as before.  Functor applications stay
    // out (lid_full renders them with parens).
    std::string path = pl ? pl->name : lid_full(modid);
    if (path.find('(') != std::string::npos) return;
    if (pl)
      if (auto f = local_module_paths_.find(pl->name);
          f != local_module_paths_.end() && !f->second.empty() &&
          f->second.find('.') != std::string::npos)
        path = f->second;
    std::vector<std::string> comps;
    for (size_t p0 = 0;;) {
      size_t q = path.find('.', p0);
      comps.push_back(
          path.substr(p0, q == std::string::npos ? std::string::npos : q - p0));
      if (q == std::string::npos) break;
      p0 = q + 1;
    }
    try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* msig = &loaded.back()->sig();
      for (size_t i = 1; i < comps.size() && msig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : msig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        msig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (!msig) return;
      // Qualify (not expand) same-module ctor-arg types, as in qualified_ctor_scheme
      // -- so `Seq.(Cons (.., tail))` gives the tail `Seq.t`, matching the explicit
      // `Seq.Cons` path (ocamlc's per-occurrence path).
      auto* saved_ctx = cmi_types_ctx_;
      std::string saved_pfx = cmi_mod_prefix_;
      bool saved_fold = fold_abbrevs_;
      cmi_types_ctx_ = &msig->types;
      cmi_mod_prefix_ = path;
      fold_abbrevs_ = true;
      for (auto& td : msig->types) {
        if (td.kind != cmi::TypeDecl::Variant) continue;
        for (auto& c : td.ctors) {
          std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
          std::vector<TypePtr> params;
          for (auto& p : td.params) {
            TypePtr v = eng.fresh_var();
            if (p) memo[p.get()] = v;
            params.push_back(v);
          }
          TypePtr result = c.res ? from_cmi(c.res, memo)
                                 : eng.constr(path + "." + td.name, params);
          // An inline-record ctor (`C : { f : .. } -> ..`) mirrors the local
          // registration: scheme = result only, fields via field_candidates_
          // (a pattern `C { f }` resolves f through the field registry).
          if (c.is_inline_record) {
            for (auto& l : c.inline_record) {
              TypePtr fa = eng.arrow(result, from_cmi(l.type, memo));
              eng.generalize(fa);
              field_candidates_[l.name].push_back(std::move(fa));
            }
            eng.generalize(result);
            cenv.back()[c.name] = result;
            continue;
          }
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
      for (auto& x : msig->typexts) {
        if (x.is_inline_record) continue;
        std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
        TypePtr result;
        if (x.res) result = from_cmi(x.res, memo);
        else {
          std::string tp = x.type_path ? cmi_path_str(*x.type_path) : "exn";
          result = eng.constr(tp == "exn" || tp.find('.') != std::string::npos
                                  ? tp : path + "." + tp);
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

  // Resolve a dotted type path to an imported GADT variant's ctor schemes (see
  // imported_gadt_memo_).  Scheme building mirrors open_module_ctors: results
  // come from cd_res via from_cmi (so an uncovered ctor's concrete index is
  // visible to the refutation), constructors without one get the generic
  // `(params) path`.  An inline-record ctor keeps a result-only scheme -- the
  // partiality check reads nothing but the result index.
  const std::vector<std::pair<std::string, TypePtr>>*
  imported_gadt(const std::string& path) {
    if (path.find('.') == std::string::npos) return nullptr;
    auto mit = imported_gadt_memo_.find(path);
    if (mit != imported_gadt_memo_.end())
      return mit->second ? &*mit->second : nullptr;
    auto& slot = imported_gadt_memo_[path];  // any bail memoizes the negative
    size_t dot = path.rfind('.');
    std::string tyname = path.substr(dot + 1), modpath = path.substr(0, dot);
    std::vector<std::string> comps;
    for (size_t i = 0;;) {
      size_t d = modpath.find('.', i);
      if (d == std::string::npos) { comps.push_back(modpath.substr(i)); break; }
      comps.push_back(modpath.substr(i, d - i)); i = d + 1;
    }
    // A functor-param/bound-module projection has no cmi on disk (and its
    // types are abstract here anyway -- nothing refutable).
    if (bound_module_names_.count(comps[0])) return nullptr;
    try {
      std::deque<const cmi::CmiFile*> loaded;
      loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
      const cmi::Signature* msig = &loaded.back()->sig();
      for (size_t i = 1; i < comps.size() && msig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : msig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        msig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (!msig) return nullptr;
      const cmi::TypeDecl* td = nullptr;
      for (auto& t : msig->types)
        if (t.name == tyname) { td = &t; break; }
      if (!td || td->kind != cmi::TypeDecl::Variant || td->ctors.empty())
        return nullptr;
      bool is_gadt = false;
      for (auto& c : td->ctors) if (c.res) is_gadt = true;
      if (!is_gadt) return nullptr;
      auto* saved_ctx = cmi_types_ctx_;
      std::string saved_pfx = cmi_mod_prefix_;
      bool saved_fold = fold_abbrevs_;
      cmi_types_ctx_ = &msig->types;
      cmi_mod_prefix_ = modpath;
      fold_abbrevs_ = true;
      std::vector<std::pair<std::string, TypePtr>> out;
      for (auto& c : td->ctors) {
        std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
        std::vector<TypePtr> params;
        for (auto& p : td->params) {
          TypePtr v = eng.fresh_var();
          if (p) memo[p.get()] = v;
          params.push_back(v);
        }
        TypePtr result = c.res ? from_cmi(c.res, memo) : eng.constr(path, params);
        TypePtr scheme = result;
        if (!c.is_inline_record)
          for (auto it = c.args.rbegin(); it != c.args.rend(); ++it)
            scheme = eng.arrow(from_cmi(*it, memo), scheme);
        eng.generalize(scheme);
        out.emplace_back(c.name, std::move(scheme));
      }
      cmi_types_ctx_ = saved_ctx;
      cmi_mod_prefix_ = saved_pfx;
      fold_abbrevs_ = saved_fold;
      slot = std::move(out);
      return &*slot;
    } catch (...) {}
    return nullptr;
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
  // `proven` (optional out, see gadt_function_partial): set only when a Total
  // verdict comes from tuple_gadt_partial's COMPLETED usefulness analysis --
  // never from the conservative defaults, which carry no license to drop a
  // Match_failure the back end would otherwise emit.
  bool compute_partial(const TypePtr& scrut, const std::vector<Case>& cases,
                       bool* proven = nullptr) {
    for (auto& c : cases)
      if (!c.guard && is_catchall(c.lhs)) return false;  // unguarded catch-all
    bool any_unguarded = false;  // every case guarded -> a value can fall through
    for (auto& c : cases) if (!c.guard) any_unguarded = true;
    if (!any_unguarded) return true;
    TypePtr s = I::Engine::repr(scrut);
    if (s->kind == I::Type::Kind::Tuple)
      return tuple_gadt_partial(s, cases, proven);
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
  // `proven` (optional out): set only when the Total verdict comes from a
  // COMPLETED refutation of every uncovered ctor -- never from the "unknown ->
  // keep Total" defaults.  The back end may drop a Match_failure default (and
  // route the refuted tags through switch holes, as ocamlc's Total lowering
  // does) only on a proven verdict.
  bool gadt_function_partial(const TypePtr& scrut, const std::vector<Case>& cases,
                             bool* proven = nullptr) {
    TypePtr s = I::Engine::repr(scrut);
    if (s->kind != I::Type::Kind::Constr) return false;  // unknown -> keep Total
    for (auto& c : cases)
      if (!c.guard && is_catchall(c.lhs)) return false;  // catch-all covers all
    std::set<std::string> covered;
    for (auto& c : cases) if (!c.guard) collect_ctors(c.lhs, covered);
    auto v = gadt_refute_uncovered(s, covered, proven);
    return v ? *v : false;  // unknown type -> keep Total
  }

  // The refutation core, shared with the record-field projection: refute every
  // uncovered ctor of the (repr'd Constr) GADT column type `s` against the
  // covered-name set.  nullopt when s isn't a known (local or cmi) variant.
  std::optional<bool> gadt_refute_uncovered(const TypePtr& s,
                                            const std::set<std::string>& covered,
                                            bool* proven) {
    auto tc = type_ctors.find(s->path);
    auto sc = type_ctor_schemes_.find(s->path);
    const std::vector<std::pair<std::string, TypePtr>>* schemes = nullptr;
    std::vector<std::string> universe;
    if (tc != type_ctors.end() && sc != type_ctor_schemes_.end()) {
      schemes = &sc->second;
      universe = tc->second;
    } else if (auto* imp = imported_gadt(s->path)) {  // cmi-declared GADT
      schemes = imp;
      for (auto& [n, sch] : *imp) universe.push_back(n);
    } else return std::nullopt;
    for (auto& cname : universe) {
      if (covered.count(cname)) continue;
      const TypePtr* schp = nullptr;
      for (auto& [n, sch] : *schemes) if (n == cname) { schp = &sch; break; }
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
    if (proven) *proven = true;
    return false;  // every uncovered ctor is refutable -> Total
  }

  // A GADT `match`'s exhaustiveness.  ocamlc types a GADT ctor pattern against
  // a FLEXIBLE scrutinee index by UNIFYING (pinning) it -- refinement equations
  // are reserved for rigid (locally-abstract) indices -- and the delayed
  // check_partial then refutes each uncovered ctor whose result index cannot
  // equal the pinned one (typedtree's split_pattern: matching Tpat_value pins
  // the index to `computation`, refuting the ten `value` ctors -> Total, no
  // Match_failure).  Our engine skips the pattern/scrutinee unify for GADT
  // matches entirely (branch refinement is unmodeled), so the scrutinee is
  // often still an unpinned var here: pin it the way ocamlc would -- softly
  // unify each covered ctor's result scheme against it inside a trail window --
  // run gadt_function_partial's refutation against the pinned type, then roll
  // the window back.  A scrutinee that IS a locally-abstract var stays unpinned
  // (rigid in ocamlc: nothing refutable), falling back to the coverage check;
  // an annotated rigid index (`k pd` scrutinee) arrives as a Constr whose var
  // argument index_could_be_equal never refutes.
  bool gadt_match_partial(const TypePtr& scrut, const std::vector<Case>& cases,
                          bool* proven = nullptr) {
    TypePtr s = I::Engine::repr(scrut);
    if (s->kind == I::Type::Kind::Constr &&
        (gadt_types.count(s->path) || imported_gadt(s->path)))
      return gadt_function_partial(scrut, cases, proven);
    // The compute_partial fallbacks all thread `proven` through: a TUPLE
    // scrutinee lands here when its ctor patterns are GADT (pat_has_gadt_ctor
    // descends tuples), and tuple_gadt_partial's completed usefulness proof
    // (classify_pattern_desc's pair) is as licensing as the direct refutation.
    if (s->kind != I::Type::Kind::Var) return compute_partial(scrut, cases, proven);
    if (la_scope_ > 0) return compute_partial(scrut, cases, proven);
    for (auto& [n, v] : newtype_vars)
      if (I::Engine::repr(v).get() == s.get())
        return compute_partial(scrut, cases, proven);
    std::set<std::string> covered;
    for (auto& c : cases) if (!c.guard) collect_ctors(c.lhs, covered);
    // The owning type, through the first covered ctor with a known scheme.
    std::string path;
    for (auto& cn : covered) {
      auto it = ctors.find(cn);
      if (it == ctors.end()) continue;
      TypePtr result;
      ctor_params(eng.instantiate(it->second), result);
      TypePtr r = I::Engine::repr(result);
      if (r->kind == I::Type::Kind::Constr) { path = r->path; break; }
    }
    if (path.empty() || !gadt_types.count(path))
      return compute_partial(scrut, cases, proven);
    auto sc = type_ctor_schemes_.find(path);
    if (sc == type_ctor_schemes_.end())
      return compute_partial(scrut, cases, proven);
    size_t wm = eng.mark();
    for (auto& cn : covered)
      for (auto& [n, sch] : sc->second)
        if (n == cn) {
          TypePtr result;
          ctor_params(eng.instantiate(sch), result);
          try_unify(result, scrut);  // instantiation vars + the scrutinee var
          break;
        }
    TypePtr sp = I::Engine::repr(scrut);
    bool partial = (sp->kind == I::Type::Kind::Constr && gadt_types.count(sp->path))
                       ? gadt_function_partial(scrut, cases, proven)
                       : compute_partial(scrut, cases, proven);
    eng.undo_to(wm);
    return partial;
  }

  // ---- Record-field GADT projection (parmatch's set_args) -----------------
  // `match q with {pat_desc = Tpat_tuple ..} | {pat_desc = ..} | ..`: the GADT
  // column sits BEHIND a record field, so the scrutinee reprs as the record
  // Constr and the direct GADT detection misses (compute_partial's
  // unknown-type default then keeps an unproven Total -- no license to drop
  // the Match_failure).  When every unguarded arm is a record pattern whose
  // only refutable sub-pattern sits on ONE common field, project: resolve the
  // field's type through the record's accessor scheme (unified against the
  // pinned scrutinee inside a rolled-back trail window) and run the
  // uncovered-ctor refutation on the projected column.  nullopt = projection
  // not applicable -> the caller falls back to compute_partial.
  std::optional<bool> record_field_gadt_partial(const TypePtr& scrut,
                                                const std::vector<Case>& cases,
                                                bool* proven) {
    TypePtr s = I::Engine::repr(scrut);
    if (s->kind != I::Type::Kind::Constr) return std::nullopt;
    std::string field;
    std::vector<const Pattern*> col;
    for (auto& c : cases) {
      if (c.guard) continue;  // a guarded arm covers nothing
      auto* r = std::get_if<Ppat_record>(&c.lhs.desc);
      if (!r) return std::nullopt;  // incl. catch-all arms: not this shape
      const Pattern* sub = nullptr;
      for (auto& [lid, sp] : r->fields) {
        if (is_catchall(*sp)) continue;  // irrefutable sub-pattern: any field
        if (sub) return std::nullopt;    // two refutable columns: out of scope
        if (!field.empty() && lid_last(lid.txt) != field) return std::nullopt;
        field = lid_last(lid.txt);
        sub = &*sp;
      }
      if (!sub) return std::nullopt;  // an all-wild record arm = catch-all row
      col.push_back(sub);
    }
    if (field.empty() || col.empty()) return std::nullopt;
    // Candidate accessors (`recTy -> fieldTy` arrows): the local field first,
    // then the unambiguous external one -- first whose domain unifies with the
    // pinned scrutinee wins (a same-named local label must not hijack an
    // external record's projection, so a failed unify just tries the next).
    std::vector<TypePtr> accs;
    if (auto fit = fields_.find(field); fit != fields_.end())
      accs.push_back(fit->second);
    if (auto eit = ext_fields_.find(field);
        eit != ext_fields_.end() && eit->second.size() == 1)
      accs.push_back(eit->second[0]);
    std::optional<bool> verdict;
    for (auto& acc : accs) {
      size_t wm = eng.mark();
      TypePtr a = I::Engine::repr(eng.instantiate(acc));
      bool ok = false;
      if (a->kind == I::Type::Kind::Arrow) {
        // Not try_unify: a clash here is "wrong candidate", never a user error.
        try { eng.unify(a->dom, s); ok = true; } catch (const I::TypeError&) {}
      }
      if (ok) {
        TypePtr ft = I::Engine::repr(a->cod);
        if (ft->kind == I::Type::Kind::Constr) {
          std::set<std::string> covered;
          for (auto* p : col) collect_ctors(*p, covered);
          verdict = gadt_refute_uncovered(ft, covered, proven);
        }
      }
      eng.undo_to(wm);
      if (ok) break;  // right record found: its verdict (or nullopt) is final
    }
    return verdict;
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
    // Vars and RIGID newtype constrs (`(type k)`: display-pass rigid nodes)
    // both carry branch-local equations in su.
    for (int i = 0;
         (t->kind == I::Type::Kind::Var ||
          (t->kind == I::Type::Kind::Constr && t->rigid)) && i < 64; ++i) {
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
        // A bare stamp-0 node (an arm-pinned scrutinee index in the flexible
        // passes): attach the decl when the name resolves uniquely, so the
        // mcomp variant comparison has its description (else desc-unknown
        // over-approximates compatible and the pair refutation never fires).
        if (int st = mx_resolve_bare_stamp(c->path, ""))
          if (auto sd = stamp_type_decl_.find(st); sd != stamp_type_decl_.end())
            r.decl = sd->second;
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
    // A rigid newtype constr is a locally-abstract `(type k)`: compatible with
    // anything, but matching a GADT ctor against it RECORDS the equation
    // branch-locally (ocamlc's refinement) -- so the classify_pattern_desc
    // pair's second column sees k=value and refutes Computation there.
    if (a->kind == K::Constr && a->rigid) { su[a.get()] = b; return true; }
    if (b->kind == K::Constr && b->rigid) { su[b.get()] = a; return true; }
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

  bool tuple_gadt_partial(const TypePtr& s, const std::vector<Case>& cases,
                          bool* proven = nullptr) {
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
      bool useful = mx_useful(std::move(rows), s->args, MxSubst{});
      // A COMPLETED not-useful analysis is a real Total proof (typedtree's
      // classify_pattern_desc pair: (Value,Computation) jointly contradictory
      // through the shared rigid `k`) -- license dropping the Match_failure.
      if (!useful && proven) *proven = true;
      return useful;
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

  // Type-directed disambiguation of a shadowed constructor in PATTERN position
  // (consumer twin of the infer_expr_expected producer fix): a bare pattern ctor
  // resolves by the SCRUTINEE type, not by lexical scope.  infer_pat used
  // find_ctor (last-in-scope), so when its recorded type disagrees with the
  // scrutinee variant, re-record the scrutinee type -- else `match n with Type ->`
  // (n : Shape.Sig_component_kind.t, under `open Typedtree`) tested the shadowing
  // item_declaration.Type BLOCK tag where the constant tag was meant, disagreeing
  // with a type-directed CONSTRUCTION of the same value (a producer/consumer
  // miscompile the effid/DDC gates don't catch -- only a construct+match exec).
  void disambig_pat_by_scrut(const Pattern& lhs, const TypePtr& scrut) {
    if (!record_kinds_) return;
    // DEFERRED to resolve_pending_disambig (after the inference fixpoint): the
    // column type is often pinned only by a LATER use -- includemod's `match
    // (arg:Error.functor_arg_descr), param with ..` types `param` (unannotated)
    // only when an arm body reaches `Incompatible_params(arg,param)`, so an
    // inline walk here sees a free var where the disambiguating variant is.
    pending_pat_disambig_.emplace_back(&lhs, scrut);
  }
  std::vector<std::pair<const Pattern*, TypePtr>> pending_pat_disambig_;
  std::vector<std::pair<const Expression*, TypePtr>> pending_expr_disambig_;
  // Functor-argument structs whose bindings were already inferred (the kinds
  // pass descends them for their records; module_exports can re-visit a node).
  std::unordered_set<const void*> inferred_arg_structs_;
  // Does the (possibly qualified) type at `path` DECLARE constructor `cn`?
  // Local per-type tables first (mod_prefix-qualified and bare), then the
  // owning unit's cmi (same submodule walk as cmi_type_is_immediate).  An
  // ALIAS/abstract/unknown type answers false -- the disambiguation walks use
  // this to reject abbreviation paths (Misc.Stdlib.Result.t == result) whose
  // last component would otherwise read as a bogus type mismatch.
  std::unordered_map<std::string, std::set<std::string>> cmi_ctors_memo_;
  bool scrut_owns_ctor(const std::string& path, const std::string& cn) {
    if (auto ts = type_ctor_schemes_.find(path); ts != type_ctor_schemes_.end()) {
      for (auto& [n, s] : ts->second)
        if (n == cn) return true;
      return false;
    }
    if (path.find('.') == std::string::npos) {
      auto tc = type_ctors.find(path);
      if (tc == type_ctors.end()) return false;
      for (auto& n : tc->second)
        if (n == cn) return true;
      return false;
    }
    auto it = cmi_ctors_memo_.find(path);
    if (it == cmi_ctors_memo_.end()) {
      std::set<std::string> cs;
      std::vector<std::string> comps = mod_components_str(path);
      if (comps.size() >= 2) try {
        std::deque<const cmi::CmiFile*> loaded;
        loaded.push_back(&cmi::CmiFile::load(head_cmi(comps[0])));
        const cmi::Signature* sig = &loaded.back()->sig();
        for (size_t i = 1; i + 1 < comps.size() && sig; ++i) {
          std::string name = comps[i];
          int applications = 0;
          if (auto par = name.find('('); par != std::string::npos) {
            for (char c : name) applications += c == '(';
            name = name.substr(0, par);
          }
          const cmi::ModuleDecl* md = nullptr;
          for (auto& mm : sig->modules) if (mm.name == name) { md = &mm; break; }
          if (!md) { sig = nullptr; break; }
          cmi::ModuleTypePtr mt = md->type;
          for (int a = 0; a < applications && mt; ++a)
            mt = mt->kind == cmi::ModuleType::Functor ? mt->functor_body : nullptr;
          sig = module_sig(mt, loaded);
        }
        if (sig)
          for (auto& td : sig->types)
            if (td.name == comps.back()) {
              if (td.kind == cmi::TypeDecl::Variant)
                for (auto& c : td.ctors) cs.insert(c.name);
              break;
            }
      } catch (...) {}
      it = cmi_ctors_memo_.emplace(path, std::move(cs)).first;
    }
    return it->second.count(cn) > 0;
  }
  void resolve_pending_disambig() {
    for (auto& [p, t] : pending_pat_disambig_) disambig_pat_now(*p, t);
    for (auto& [e, t] : pending_expr_disambig_)
      disambig_expr_now(*e, t, /*allow_defer=*/false, /*deferred=*/true);
  }
  // Type-directed bare-constructor resolution in EXPRESSION position: an
  // unqualified constructor we couldn't resolve (typed Any) whose EXPECTED type
  // is a module-qualified variant is recorded so infer_value_kinds exposes it
  // via expr_constr (predef's `decl0 ~immediate:Always`).  With allow_defer, an
  // expected type still a VAR is queued and re-checked after the fixpoint (the
  // expectation is often pinned only by a later constraint).
  void disambig_expr_now(const Expression& e, const TypePtr& expected,
                         bool allow_defer, bool deferred = false) {
    auto* k = std::get_if<Pexp_construct>(&e.desc);
    if (!k) return;
    std::string cn = lid_last(k->id.txt);
    TypePtr er = I::Engine::repr(expected);
    if (allow_defer && er->kind == I::Type::Kind::Var &&
        std::holds_alternative<Lident>(k->id.txt.v)) {
      pending_expr_disambig_.emplace_back(&e, expected);
      return;
    }
    bool er_variant = er->kind == I::Type::Kind::Constr &&
                      er->path.find('.') != std::string::npos &&
                      // A DEFERRED expectation (a var when seen inline, pinned
                      // only at the fixpoint) is a NEW path: require the
                      // expected type to provably declare the ctor, rejecting
                      // abbreviation paths whose last component would read as
                      // a bogus mismatch.  The inline dotted path keeps its
                      // established behavior.
                      (!deferred || scrut_owns_ctor(er->path, cn));
    // A DOTLESS expected type disambiguates a ctor name the file declares
    // AMBIGUOUSLY (two local types sharing it -- u.ml's `test Unit Unit`
    // where arg1 : descr, arg2 : parameter): the lexical last-in-scope
    // resolution picks the wrong tag there.  exn/predef names keep their
    // own machinery (pattern-side twin: disambig_pat_by_scrut).
    bool er_local_amb = er->kind == I::Type::Kind::Constr &&
                        !er->path.empty() &&
                        er->path.find('.') == std::string::npos &&
                        std::holds_alternative<Lident>(k->id.txt.v) &&
                        ambiguous_ctors_.count(cn) &&
                        !exn_ctors_.count(cn) && !predef_ctors_.count(cn) &&
                        scrut_owns_ctor(er->path, cn);
    TypePtr* sch = find_ctor(cn);
    if ((er_variant || er_local_amb) && !sch) {
      rec_expr_[&e] = expected;
      if (er_local_amb) ctor_arg_type_[&e] = er;
    } else if ((er_variant || er_local_amb) && sch &&
               std::holds_alternative<Lident>(k->id.txt.v)) {
      // Type-directed disambiguation of a SHADOWED constructor: a bare ctor
      // resolves by its EXPECTED type, not by lexical scope.  find_ctor
      // returns the LAST-in-scope scheme, so when that scheme's owning type
      // differs from the expected variant, record the expected type instead
      // -- the back end then reads the right tag/constant-ness.  cmt_format's
      // `f ~namespace:Type` (expected Shape.Sig_component_kind.t) otherwise
      // resolved to the shadowing `open Typedtree` item_declaration.Type, a
      // tag-3 BLOCK, where the constant tag-1 was meant (a mis-tagged atom,
      // not an immediate).  Cross-unit shadows aren't in ambiguous_ctors_,
      // so key on the type mismatch directly; a genuinely-correct resolution
      // has found == expected (same last component) and is left untouched.
      TypePtr res;
      ctor_params(eng.instantiate(*sch), res);
      TypePtr rr = I::Engine::repr(res);
      auto lastc = [](const std::string& s) {
        auto d = s.rfind('.');
        return d == std::string::npos ? s : s.substr(d + 1);
      };
      // Same LAST component does not mean same type across modules
      // (Parsetree and Typedtree both declare `functor_parameter`;
      // untypeast's `Named (name, mt)` found Typedtree's arity-3 ctor
      // where the annotated Parsetree arity-2 one was meant, boxing the
      // written pair into one field).  When BOTH paths are dotted,
      // compare them fully, resolving each head through file-local
      // module aliases so `T.x` vs `Typedtree.x` stays a match.
      auto alias_norm = [&](const std::string& s) {
        auto d = s.find('.');
        if (d == std::string::npos) return s;
        auto f = local_module_paths_.find(s.substr(0, d));
        if (f != local_module_paths_.end() && !f->second.empty())
          return f->second + s.substr(d);
        return s;
      };
      std::string found = rr->kind == I::Type::Kind::Constr ? rr->path : "";
      bool diff;
      if (!found.empty() && found.find('.') != std::string::npos)
        diff = alias_norm(found) != alias_norm(er->path);
      else
        diff = !found.empty() && lastc(found) != lastc(er->path);
      if (getenv("CTDBG"))
        fprintf(stderr, "[CTDBG-I] shadow-gate ctor %s found=%s expected=%s diff=%d\n",
                cn.c_str(), found.c_str(), er->path.c_str(), (int)diff);
      if (diff) {
        rec_expr_[&e] = expected;
        // Authoritative even DOTLESS for a locally-ambiguous name: route
        // through the ctor-arg map so vk.expr_constr records it (the
        // general rec_expr_ harvest gates dotless paths on stamp
        // identity, which an annotation-built Constr may not carry).
        if (er_local_amb) ctor_arg_type_[&e] = er;
      }
    }
  }
  void disambig_pat_now(const Pattern& lhs, const TypePtr& scrut, int depth = 0) {
    // Recurse through the shapes whose sub-patterns keep a known COLUMN type
    // (tuple columns, or-alternatives, aliases), so a NESTED ambiguous ctor is
    // re-resolved by its column too -- u.ml's `match arg, param with
    // (Unit|Empty_struct), Unit -> ..` has both same-named `Unit`s under a
    // tuple; ctor ARGS are covered separately by record_pat_ctor_arg_type.
    if (auto* al = std::get_if<ast::Ppat_alias>(&lhs.desc))
      return disambig_pat_now(*al->p, scrut, depth + 1);
    if (auto* ct = std::get_if<ast::Ppat_constraint>(&lhs.desc))
      return disambig_pat_now(*ct->p, scrut, depth + 1);
    if (auto* op = std::get_if<ast::Ppat_open>(&lhs.desc))
      return disambig_pat_now(*op->p, scrut, depth + 1);
    if (auto* o = std::get_if<ast::Ppat_or>(&lhs.desc)) {
      disambig_pat_now(*o->l, scrut, depth + 1);
      disambig_pat_now(*o->r, scrut, depth + 1);
      return;
    }
    if (auto* tu = std::get_if<ast::Ppat_tuple>(&lhs.desc)) {
      TypePtr sr = I::Engine::repr(scrut);
      if (sr->kind == I::Type::Kind::Tuple &&
          sr->args.size() == tu->elems.size())
        for (size_t i = 0; i < tu->elems.size(); ++i)
          disambig_pat_now(*tu->elems[i], sr->args[i], depth + 1);
      return;
    }
    auto* k = std::get_if<ast::Ppat_construct>(&lhs.desc);
    if (!k || !std::holds_alternative<Lident>(k->id.txt.v)) return;
    TypePtr sr = I::Engine::repr(scrut);
    if (sr->kind != I::Type::Kind::Constr || sr->path.empty()) return;
    std::string cn = lid_last(k->id.txt);
    bool dotted = sr->path.find('.') != std::string::npos;
    // A ctor name the file declares AMBIGUOUSLY (two local types sharing it):
    // the scrutinee type is authoritative, even DOTLESS.  exn/predef names
    // keep their own machinery.
    bool local_amb = ambiguous_ctors_.count(cn) && !exn_ctors_.count(cn) &&
                     !predef_ctors_.count(cn) &&
                     scrut_owns_ctor(sr->path, cn);
    // A NESTED leaf (this walk is new there; the top-level dotted case keeps
    // its established behavior) acts only when the scrutinee type PROVABLY
    // declares the ctor: the last-component comparison below cannot see
    // through abbreviations, so a column typed by an ALIAS path
    // (Misc.Stdlib.Result.t == result) would false-positive on every
    // `Ok x | Error x` and knock the builtin-result dispatch off course.
    if (depth > 0) {
      if (!local_amb && !(dotted && scrut_owns_ctor(sr->path, cn))) return;
    } else if (!dotted && !local_amb) {
      return;
    }
    auto rp = rec_pat_.find(&lhs);
    if (rp == rec_pat_.end()) return;
    TypePtr pr = I::Engine::repr(rp->second);
    std::string found = pr->kind == I::Type::Kind::Constr ? pr->path : "";
    auto lastc = [](const std::string& s) {
      auto d = s.rfind('.');
      return d == std::string::npos ? s : s.substr(d + 1);
    };
    if (getenv("CTDBG"))
      fprintf(stderr, "[CTDBG-P] pat-disambig %s found=%s scrut=%s amb=%d d=%d\n",
              cn.c_str(), found.c_str(), sr->path.c_str(), (int)local_amb, depth);
    if (!found.empty() && lastc(found) != lastc(sr->path)) {
      rec_pat_[&lhs] = scrut;
      // Route through the ctor-arg map so vk.pat_constr records it even
      // DOTLESS (the general rec_pat_ harvest only records qualified paths).
      if (local_amb) pat_ctor_arg_type_[&lhs] = sr;
    }
  }

  // A `#t` pattern's row: t must be an abbreviation of a poly-variant row
  // (`type rte = [ `A of .. | `B ]`); the pattern means "any of t's tags", i.e.
  // the UPPER bound `[< tags-with-declared-args ]`.  Builds a fresh row instance
  // per call (each gets vk=1); returns null when t isn't a known variant
  // abbreviation.  Non-strict only (rows are a non-strict feature).
  TypePtr hash_type_row(const ast::Longident& id) {
    if (strict) return nullptr;
    const ast::CoreType* manifest = nullptr;
    bool params_empty = true;
    bool local = false;  // t comes from an expression-local module (unnameable)
    // `#M.t` where M is an expression-local module (`let module M = struct..`):
    // resolve `t` through M's OWN type decls, not the flat bare-name alias map
    // (lid_last would collide `M.t` with an enclosing same-named `t` -- pr11887
    // has an outer `a1=[`CA]` and a local `T.a1=[`Common|..]`; `#T.a1` must pick
    // T's row, not the outer one).
    if (auto* dot = std::get_if<ast::Ldot>(&id.v))
      if (auto* head = std::get_if<ast::Lident>(&dot->prefix->v)) {
        auto st = local_module_structs_.find(head->name);
        if (st != local_module_structs_.end()) {
          for (auto& it : *st->second)
            if (auto* ty = std::get_if<ast::Pstr_type>(&it.desc))
              for (auto& d : ty->decls)
                if (d.name.txt == dot->name && d.manifest) {
                  manifest = d.manifest->get();
                  params_empty = d.params.empty();
                  local = true;
                }
          if (!manifest) return nullptr;  // local M but no such alias decl
        }
      }
    if (!manifest) {
      auto ai = type_aliases.find(lid_last(id));
      if (ai == type_aliases.end() || !ai->second.manifest) return nullptr;
      manifest = ai->second.manifest;
      params_empty = ai->second.params.empty();
    }
    if (!std::holds_alternative<Ptyp_variant>(manifest->desc)) return nullptr;
    std::unordered_map<std::string, TypePtr> vars;
    bool saved_me = manifest_expansion_;
    manifest_expansion_ = true;
    TypePtr row = from_coretype(*manifest, vars);
    manifest_expansion_ = saved_me;
    row = I::Engine::repr(row);
    if (row->kind != I::Type::Kind::Variant) return nullptr;
    row->variant_kind = 1;  // `#t` bounds ABOVE: `[<`, not the exact `[ .. ]`
    // A local-module type has no signature-level name, so ocamlc expands its
    // tags (`[< `Common | ..]`) rather than citing the abbreviation; stamping
    // `a1` here would misname it as the enclosing same-named type.
    if (params_empty && !local) row->abbrev = lid_last(id);  // `[< var ]` display
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
              for (size_t i = 0; i < ps.size(); ++i) {
                try_unify(ps[i], infer_pat(*tup->elems[i]));
                record_pat_record_arg_type(tup->elems[i].get(), ps[i]);
              }
            else if (!ps.empty()) {
              try_unify(ps[0], infer_pat(**k->arg));
              record_pat_record_arg_type(k->arg->get(), ps[0]);
            } else infer_pat(**k->arg);
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
          for (size_t i = 0; i < ps.size(); ++i) {
            try_unify(ps[i], infer_pat(*tup->elems[i]));
            record_pat_ctor_arg_type(tup->elems[i].get(), ps[i]);
            record_pat_record_arg_type(tup->elems[i].get(), ps[i]);
          }
        } else if (!ps.empty()) {
          // `C _`: the lone `_` fills every arity slot in the dump (the local
          // ctor_arity_ registry covers local decls; this covers cmi ctors
          // brought into bare scope by an open).
          if (ps.size() > 1 && std::holds_alternative<Ppat_any>((*k->arg)->desc))
            construct_any_arity[&p] = (int)ps.size();
          try_unify(ps[0], infer_pat(**k->arg));
          record_pat_ctor_arg_type(k->arg->get(), ps[0]);
          record_pat_record_arg_type(k->arg->get(), ps[0]);
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
      // `pat as x`: x is bound not to the scrutinee's type but to one REBUILT
      // from the pattern (typecore build_as_type), so a poly-variant or-pattern
      // `` `Nil | `Cons _ as x `` gives x its own OPEN sub-row `[> `Nil | `Cons ]`
      // independent of the `[<` scrutinee -- both the kind (emit) pass and the
      // display pass need this, else `` `A x `` ties the output row back to the
      // whole input (morematch's shared `as 'a`).  The sub-pattern types feed
      // build_as_type through as_map_.
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
      // Whether any label is a LOCAL record field: if so this pattern is
      // anchored to a local record and we must NOT let an externally-loaded
      // same-named label (a red herring from another unit's record) hijack
      // recTy.  Only a PURELY-external pattern (`{rf_loc; rf_desc; ..}` under
      // `module T = Typedtree`) falls back to ext_fields_ (below).
      bool any_local = false;
      for (auto& [lid, sub] : r->fields)
        if (fields_.count(lid_last(lid.txt))) { any_local = true; break; }
      for (auto& [lid, sub] : r->fields) {
        auto it = fields_.find(lid_last(lid.txt));
        // A format-poly field is in BOTH maps (kind pass); the pattern binds
        // the GENERALIZED poly scheme so each body use instantiates fresh.
        if (it != fields_.end() && poly_format_labels_.count(lid_last(lid.txt)) &&
            poly_field_rec_.count(lid_last(lid.txt)))
          it = fields_.end();
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
          // A label owned by an EXTERNALLY-loaded record (`{rf_loc; rf_desc; ..}`
          // destructuring a `Typedtree.row_field` under `module T = Typedtree`):
          // resolve the record type from the unique ext_fields_ accessor so the
          // pattern binds real field types -- otherwise the whole function body's
          // type-directed match degenerates.  Only when the label is UNAMBIGUOUS
          // (a single external record declares it); non-strict, so the corpus's
          // strict pass is untouched.
          if (!strict && !any_local) {
            auto eit = ext_fields_.find(lid_last(lid.txt));
            if (eit != ext_fields_.end() && eit->second.size() == 1) {
              TypePtr s = I::Engine::repr(eng.instantiate(eit->second[0]));
              if (s->kind == I::Type::Kind::Arrow) {
                try_unify(infer_pat(*sub), s->cod);
                if (recTy) try_unify(recTy, s->dom); else recTy = s->dom;
                continue;
              }
            }
          }
          // An AMBIGUOUS label whose candidate records ALL give the field the
          // SAME predefined ground type (binutils FlexDLL: `name : string` in
          // both `section` and `symbol`): the binder has that type under EVERY
          // possible disambiguation, so binding it is sound whichever record
          // ocamlc's type-directed pass picks -- and `name = sectname`
          // specializes (caml_string_equal) exactly as ocamlc.
          // recTy is left alone -- the record identity itself stays ambiguous.
          if (!strict) {
            std::string agreed = ambiguous_field_ground(lid_last(lid.txt));
            if (!agreed.empty()) {
              try_unify(infer_pat(*sub), eng.constr(agreed));
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

  // A syntactic format annotation (`(_,_,_) format` / format4 / format6,
  // possibly qualified).  An annotated expression (`(e : _ format)`, `let f :
  // _ format = e`) must have the format type pushed INTO it before inference —
  // OCaml's type_expect types its string literals as formats, and the back end
  // lowers them to CamlinternalFormatBasics values only when so marked.
  static bool coretype_is_format(const CoreType& t) {
    auto* c = std::get_if<Ptyp_constr>(&t.desc);
    return c && is_format_base(lid_full(c->id.txt));
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
    } else if (auto* it = std::get_if<Pexp_ifthenelse>(&e.desc)) {
      record_result_fmt_lits(*it->then_);
      if (it->else_) record_result_fmt_lits(**it->else_);
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
    // the typed tree (`pr "%(%d%)" (match p with A -> "x%d" | ...)`).  Recording
    // retypes nothing; in the kinds pass it marks the literals so the back end
    // LOWERS them as formats (a plain-string lowering is a miscompile: the
    // value flows into make_printf as a raw string — bootstrap bug#13).
    if ((record_fmt_lits_ || record_kinds_) && is_format_constr(expected))
      record_result_fmt_lits(e);
    TypePtr t = infer_expr(e);
    // Type-directed bare-constructor resolution: an unqualified constructor we
    // couldn't resolve (typed Any) whose EXPECTED type is a module-qualified
    // variant -- record that type at the node so infer_value_kinds exposes it via
    // expr_constr and the back end registers the type's ctors, resolving the bare
    // ctor (predef's `decl0 ~immediate:Always`, with immediate : Type_immediacy.t).
    if (record_kinds_) disambig_expr_now(e, expected, /*allow_defer=*/true);
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
      newtype_vars[nt->name.txt] = newtype_binding(nt->name.txt);
      ++la_scope_;
      TypePtr t = infer_expr(*nt->body);
      --la_scope_;
      return t;
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
      // A ctor qualified by a FILE-LOCAL module outranks the bare hit (which
      // can be an unrelated same-named ctor squatting the scope) -- the cmi
      // arbitration below cannot see local modules.
      TypePtr* lsch = std::holds_alternative<Ldot>(k->id.txt.v)
                          ? local_module_ctor_scheme(k->id.txt)
                          : nullptr;
      if (lsch) sch = lsch;
      // A QUALIFIED `M.C` (`Result.Ok`) keeps M's own type path (`Result.t`),
      // not the re-exported base (`result`) its bare name resolves to -- ocamlc
      // follows the access path.  When M's cmi yields the ctor, prefer that
      // scheme by falling through to the qualified branch below.  All passes:
      // the bare-name hit can be an UNRELATED type's ctor (Dynlink.Error vs
      // result's Error), which false-rejects in strict.
      if (sch && !lsch && std::holds_alternative<Ldot>(k->id.txt.v) &&
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
          for (size_t i = 0; i < ps.size(); ++i) {
            try_unify(ps[i], infer_expr(*tup->elems[i]));
            record_ctor_arg_type(tup->elems[i].get(), ps[i]);
          }
        } else if (!ps.empty()) {
          try_unify(ps[0], infer_expr(**k->arg));
          record_ctor_arg_type(k->arg->get(), ps[0]);
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
        disambig_pat_by_scrut(c.lhs, se);
        if (c.guard) infer_expr(**c.guard);
        TypePtr br = infer_expr(*c.rhs);
        if (window) soft_unify(br, rt);
        else if (!gadt) try_unify(br, rt);
        // The value-kinds/lambda pass skips the arm-result unify for a GADT match
        // (above), so an enclosing annotation on the match (`meth : iterator ->
        // .. -> a -> unit`) never reaches the arm bodies -- and an arm's field
        // base (`fun i -> i.structure`, i : iterator) is left an unresolved var,
        // so the back end guesses the wrong same-named record (Ast_mapper.mapper's
        // `structure`@38, not iterator's @37).  Softly tie each arm body to the
        // result type (NOT the pattern/scrutinee, whose GADT refinement must stay
        // branch-local) so the annotation flows in and the field base gets its
        // record identity; lenient, so a payload clash across arms is swallowed.
        else if (gadt && record_kinds_) soft_unify(br, rt);
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
      bool tproven = false;                                         // for the
      // An imported GADT (its ctors live only in the cmi, so gadt/gadt_ctors
      // never fire) is detectable HERE: the arms' full unify has pinned the
      // scrutinee to the concrete dotted type by now.  Partiality-only.
      bool pgadt = gadt;
      if (!pgadt) {
        TypePtr spp = I::Engine::repr(se);
        pgadt = spp->kind == I::Type::Kind::Constr && imported_gadt(spp->path);
      }
      std::optional<bool> proj;  // GADT column behind a record field
      if (!pgadt) proj = record_field_gadt_partial(se, m->cases, &tproven);
      match_partial[&e] = pgadt ? gadt_match_partial(se, m->cases, &tproven)
                        : proj  ? *proj
                                : compute_partial(se, m->cases, &tproven);  // dump
      if (tproven && !match_partial[&e]) total_proven.insert(&e);
      return rt;
    }
    if (auto* ct = std::get_if<Pexp_constraint>(&e.desc)) {
      TypePtr et, at;
      std::unordered_map<std::string, TypePtr> vars;
      if (coretype_is_format(*ct->t)) {
        // `("%s" : _ format)`: push the format type into the expression so its
        // string literals type (and lower) as formats, not plain strings.
        at = from_coretype(*ct->t, vars);
        et = infer_expr_expected(*ct->e, at);
      } else {
        et = infer_expr(*ct->e);
        at = from_coretype(*ct->t, vars);
      }
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
      TypePtr src = infer_expr(*co->e);  // infer the source for its kinds/effects
      // `(a :> c)` for a local class c and a still-unconstrained source:
      // ocamlc types the SOURCE as #c (enlarge_type) -- the OPEN row of c's
      // methods with the abbreviation carried.  A class param var pinned this
      // way is what the ctor arrow stores (`class bravo : #alfa -> ..`,
      // woodyatt).  Conservative: bare paramless class, source still a var.
      if (!strict && !co->from && src)
        if (auto* pc = std::get_if<Ptyp_constr>(&co->to_->desc);
            pc && pc->args.empty())
          if (auto* l = std::get_if<Lident>(&pc->id.txt.v))
            if (auto ct = class_types_.find(l->name); ct != class_types_.end())
              if (I::Engine::repr(src)->kind == I::Type::Kind::Var) {
                TypePtr inst = I::Engine::repr(eng.instantiate(ct->second));
                if (inst->kind == I::Type::Kind::Object) {
                  TypePtr open_ = eng.object_type(inst->labels, inst->args);
                  open_->variant_kind = 1;  // open row (`< .. ; .. >`)
                  open_->abbrev = inst->abbrev.empty() ? l->name : inst->abbrev;
                  open_->abbrev_args = inst->abbrev_args;
                  open_->method_polys = inst->method_polys;
                  open_->method_poly_names = inst->method_poly_names;
                  soft_unify(src, open_);
                }
              }
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
      // A POLYMORPHIC local field (`pat: 'k . iterator -> ..`): fields_ excludes
      // it (no mono scheme), so field_scheme would fall through to ext_fields_
      // and adopt a FOREIGN same-labelled mono record -- tast_iterator's
      // `sub.pat` typed sub as Ast_iterator.iterator and read its index 32
      // instead of the local iterator's 25 (bootstrap #13).  Resolve the base's
      // record type from the local poly field, like the pattern path does; the
      // access's type comes from from_coretype, which mints fresh generic vars
      // per use (per-use polymorphism).
      if (auto pit = poly_field_rec_.find(lid_last(fld->field.txt));
          pit != poly_field_rec_.end() && !fields_.count(lid_last(fld->field.txt))) {
        TypePtr bt = infer_expr(*fld->e);
        try_unify(bt, eng.instantiate(pit->second.recTy));
        if (record_kinds_) pending_field_.push_back({&e, bt, lid_last(fld->field.txt)});
        std::unordered_map<std::string, TypePtr> fv;
        return from_coretype(*pit->second.ftype, fv);
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
        // The scheme's dom is the field's OWNING record type; when it is a
        // module-qualified (dotted) path but the base's own repr stays a bare
        // LOCAL alias (`a : t` where `type t = Location.error`, unexpanded), the
        // base gets no expr_constr and a foreign label falls to a const-0 read
        // (typing_recovery's Error_set `a.main.loc..`).  Force the base's
        // recorded type to the dotted dom (bt and s->dom are already unified, so
        // this only picks the resolvable representative).  Restricted to a base
        // whose OWN repr is not already a dotted Constr -- overriding a base that
        // resolved on its own changes which same-named record a label picks
        // (profile/signature_matching).  Dump/kinds pass only.
        if (record_kinds_) {
          TypePtr rb = I::Engine::repr(bt), rd = I::Engine::repr(s->dom);
          bool base_dotted = rb->kind == I::Type::Kind::Constr &&
                             rb->path.find('.') != std::string::npos;
          if (!base_dotted && rd->kind == I::Type::Kind::Constr &&
              rd->path.find('.') != std::string::npos)
            rec_expr_[fld->e.get()] = s->dom;
        }
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
                                               lid_last(fld->field.txt),
                                               rb->path.substr(dpos + 1), &rb->args))
              return ft;
          // A wrapped-stdlib / nested type path (`Stdlib.Lexing.position`): resolve
          // through the leaf module, verifying the record's type name -- so a
          // chained `loc.loc_start.pos_lnum` gets `int`, not a fresh var (which
          // would leave `startline = endline` a polymorphic caml_equal).
          if (rb->path.find('.') != dpos)
            if (TypePtr ft = typepath_field_type(rb->path, lid_last(fld->field.txt),
                                                 &rb->args))
              return ft;
        }
      }
      // An AMBIGUOUS label whose candidate records ALL agree on a predefined
      // ground field type: the ACCESS's type is that type under every
      // possible disambiguation (same rule as the record-pattern path), even
      // though the record identity stays unresolved.  ident's
      // `(get_desc us).stamp` -- `stamp : int` in Unscoped.desc AND every
      // inline record -- makes `stamp : t -> int`, so `stamp us1 = stamp us2`
      // specializes to `==` exactly as ocamlc.
      if (!strict) {
        std::string agreed = ambiguous_field_ground(lid_last(fld->field.txt));
        if (!agreed.empty()) return eng.constr(agreed);
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
      // `{< .. >}` has the self type of the enclosing object; returning it (not a
      // fresh var) makes `method m = {< >}` recursive so its type IS self.
      if (!self_ty_stack_.empty()) return self_ty_stack_.back();
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
    // The %apply/%revapply SPECIALIZED TYPING rule (typecore's
    // check_apply_prim_type): `f @@ x` / `x |> f` types as the DIRECT
    // application `f x`, so a leading optional parameter of f is defaulted
    // and erased (`bump @@ x` with bump : ?cap:int -> int -> int gives int,
    // not int -> int).  Only when the operator's own type has the generic
    // (a -> b) -> a -> b shape with SHARED vars -- a monomorphic
    // `external (@@) : f -> x -> int` stays a plain 2-arg application.
    if (a.args.size() == 2 &&
        std::holds_alternative<Nolabel>(a.args[0].first) &&
        std::holds_alternative<Nolabel>(a.args[1].first)) {
      auto* fid = std::get_if<Pexp_ident>(&a.fn->desc);
      auto* fl = fid ? std::get_if<Lident>(&fid->id.txt.v) : nullptr;
      int kind = fl ? (fl->name == "@@" ? 2 : fl->name == "|>" ? 1 : 0) : 0;
      TypePtr op = kind ? I::Engine::repr(ft) : nullptr;
      bool generic = false;
      if (op && op->kind == I::Type::Kind::Arrow && op->arrow_label == 0) {
        TypePtr c1 = I::Engine::repr(op->cod);
        if (c1->kind == I::Type::Kind::Arrow && c1->arrow_label == 0) {
          TypePtr fpos = I::Engine::repr(kind == 2 ? op->dom : c1->dom);
          TypePtr xpos = I::Engine::repr(kind == 2 ? c1->dom : op->dom);
          TypePtr res = I::Engine::repr(c1->cod);
          if (fpos->kind == I::Type::Kind::Arrow && fpos->arrow_label == 0) {
            TypePtr fa = I::Engine::repr(fpos->dom);
            TypePtr fr = I::Engine::repr(fpos->cod);
            generic = fa->kind == I::Type::Kind::Var &&
                      fr->kind == I::Type::Kind::Var &&
                      fa.get() == xpos.get() && fr.get() == res.get();
          }
        }
      }
      if (generic) {
        const Expression& fexp = kind == 2 ? *a.args[0].second : *a.args[1].second;
        const Expression& xexp = kind == 2 ? *a.args[1].second : *a.args[0].second;
        TypePtr fty = I::Engine::repr(infer_expr(fexp));
        std::vector<TypePtr> fsp;
        TypePtr fcur = fty;
        while (fcur->kind == I::Type::Kind::Arrow) {
          fsp.push_back(fcur);
          fcur = I::Engine::repr(fcur->cod);
        }
        // The positional argument consumes the first NON-OPTIONAL param;
        // skipped leading optionals are defaulted (erased from the result).
        std::size_t idx = 0;
        while (idx < fsp.size() && fsp[idx]->arrow_label == 2) ++idx;
        if (idx < fsp.size() && fsp[idx]->arrow_label == 0) {
          TypePtr at = infer_expr_expected(xexp, fsp[idx]->dom);
          soft_unify(fsp[idx]->dom, at);
          TypePtr r = fcur;
          for (std::size_t j = fsp.size(); j-- > idx + 1;)
            r = eng.arrow(fsp[j]->dom, r, fsp[j]->arrow_label, fsp[j]->arrow_lbl);
          return r;
        }
        // f's spine unknown (a var): the generic operator scheme already
        // gives the same result as direct application -- fall through.
      }
    }
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
    // A callee whose type is a FOLDED abbreviation of an arrow -- `f : out_type
    // Fmt.printer` with `type 'a printer = formatter -> 'a -> unit` (possibly a
    // cross-module chain: Oprint.printer = 'a Fmt.printer ref, so `!f` is
    // `out_type Fmt.printer`) -- must be expanded so the arguments unify against
    // the real parameters.  Otherwise a bare type-directed constructor argument
    // (`!Oprint.out_type ppf (Otyp_tuple ..)`, where Otyp_tuple is not in scope
    // and resolves only by the expected `out_type`) stays an unpinned var: its
    // inferred type is never recorded in expr_constr, and the back end lowers it
    // to a dangling `?Otyp_tuple` free variable -- a miscompile (the value read
    // is garbage; printtyp's constructor_arguments printed junk).  Value-kinds
    // pass only (this feeds lowering, not the strict error pass); bounded
    // against an abbreviation-of-abbreviation chain.
    for (int g = 0; !strict && cur->kind == I::Type::Kind::Constr &&
                    !a.args.empty() && g < 8; ++g) {
      TypePtr ex = resolve_abbrev_expansion(cur->path, cur->args);
      if (!ex) break;
      cur = I::Engine::repr(ex);
    }
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
        TypePtr expected = spine[idx]->dom;
        // `~l:v` passed to an OPTIONAL `?l` parameter Some-wraps (typecore):
        // v unifies with the option PAYLOAD, not the option itself --
        // `Callbacks.create ~runtime_counter` pins the callback's declared
        // arrow onto the local function (test_caml_counters' value : int).
        if (lk == 1 && spine[idx]->arrow_label == 2) {
          TypePtr dr = I::Engine::repr(expected);
          if (dr->kind == I::Type::Kind::Constr && dr->path == "option" &&
              dr->args.size() == 1)
            expected = dr->args[0];
        }
        TypePtr at = infer_expr_expected(*arg, expected);
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
            builtin_clash(at, expected))
          note_error("This expression has a type that clashes with the expected type");
        soft_unify(expected, at);  // propagate; genuine errors via expected_clash
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
                         std::vector<std::pair<std::string, TypePtr>>* out_instvars = nullptr,
                         TypePtr* out_self = nullptr) {
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
    // `object (self : 'self)`: the self TYPE VARIABLE names the self object
    // for the whole body, so a method annotation `unit -> 'self` cites THE
    // self node (ocamlc prints `object ('a) .. method m : unit -> 'a`).
    std::string self_tyvar;
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
        if (auto* pv = std::get_if<Ptyp_var>(&sct->desc)) self_tyvar = pv->name;
        else if (auto* pa = std::get_if<Ptyp_alias>(&sct->desc)) self_tyvar = pa->name;
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
      if (out_self) *out_self = selfTy;
      self_ty_stack_.push_back(selfTy);
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
            // The class' TYPE params scope over method annotations, so `'a` in
            // `method bar : 'a -> 'a` binds to the class param `['a] c` (else it
            // would be a fresh var, printed apart from the class' `'a`).  The
            // self pattern's type var (`object (self : 'self)`) scopes over
            // them too: `s -> 'self` cites the self object itself (pr7293).
            std::unordered_map<std::string, TypePtr> vars;
            if (cvars) vars = *cvars;
            if (!self_tyvar.empty())
              vars.emplace(self_tyvar, self_ty_stack_.back());
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
        if (auto* ap = std::get_if<Pcl_apply>(&inh->ce->desc)) {
          // `inherit P args`: each arg meets P's constructor parameter type
          // (charlie's `a` adopts bravo's `#alfa` domain -- woodyatt).
          TypePtr ctor = nullptr;
          if (auto* pc = std::get_if<Pcl_constr>(&ap->ce->desc))
            if (auto it = class_ctor_types_.find(lid_last(pc->id.txt));
                it != class_ctor_types_.end())
              ctor = eng.instantiate(it->second);
          for (auto& [l, e] : ap->args) {
            TypePtr at = infer_expr(*e);
            if (!ctor) continue;
            TypePtr c = I::Engine::repr(ctor);
            if (c->kind == I::Type::Kind::Arrow) {
              soft_unify(at, c->dom);
              ctor = c->cod;
            } else {
              ctor = nullptr;
            }
          }
        }
      }
    }
    venv.pop_back();
    self_ty_stack_.pop_back();
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
        newtype_vars[nt->name.txt] = newtype_binding(nt->name.txt);
      }
    la_scope_ += (int)saved_newtypes.size();
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
          disambig_pat_by_scrut(c.lhs, arg);
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
      bool fproven = false;
      bool cases_gadt = arg_gadt;  // an unpinned param can still be a GADT
      for (auto& c : fc.cases)     // match: detect through the case patterns,
        if (pat_has_gadt_ctor(c.lhs)) cases_gadt = true;  // like a Pexp_match
      if (!cases_gadt)             // imported GADT: partiality-only, as at match
        cases_gadt =
            sarg->kind == I::Type::Kind::Constr && imported_gadt(sarg->path);
      std::optional<bool> fproj;   // GADT column behind a record field
      if (!cases_gadt) fproj = record_field_gadt_partial(arg, fc.cases, &fproven);
      function_cases_partial[&fc] =
          cases_gadt ? gadt_match_partial(arg, fc.cases, &fproven)
          : fproj    ? *fproj
                     : compute_partial(arg, fc.cases, &fproven);
      if (fproven && !function_cases_partial[&fc]) total_proven.insert(&fc);
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
    la_scope_ -= (int)saved_newtypes.size();
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
          for (auto& u : pc->univars) newtype_vars[u.txt] = newtype_binding(u.txt);
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
        // check body (best-effort); a format-annotated rec binding pushes the
        // declared format type into the body (string literals lower as formats)
        const Pvc_constraint* upc =
            bs[i].constraint_ ? std::get_if<Pvc_constraint>(&*bs[i].constraint_)
                              : nullptr;
        if (upc && !upc->univars.empty()) ++la_scope_;
        TypePtr te = (bound[i] && is_format_constr(bound[i]))
                         ? infer_expr_expected(*bs[i].expr, bound[i])
                         : infer_expr(*bs[i].expr);
        if (upc && !upc->univars.empty()) --la_scope_;
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
      // Load the declared type's modules' record fields before inferring the
      // body, so a shared label (`i : Ast_iterator.iterator` then `i.structure`,
      // a label Ast_mapper.mapper also declares) is ambiguous at the field read.
      if (record_kinds_ && b.constraint_)
        if (auto* pc = std::get_if<Pvc_constraint>(&*b.constraint_))
          preload_annot_record_fields(*pc->typ);
      // `let f : _ format = e`: build the annotation FIRST and push it into the
      // body, so string literals in result positions type (and lower) as formats.
      TypePtr fmt_annot = nullptr;
      if (b.constraint_)
        if (auto* pc = std::get_if<Pvc_constraint>(&*b.constraint_))
          if (pc->univars.empty() && coretype_is_format(*pc->typ))
            fmt_annot = from_coretype(*pc->typ, avars);
      const Pvc_constraint* upc =
          b.constraint_ ? std::get_if<Pvc_constraint>(&*b.constraint_) : nullptr;
      if (upc && !upc->univars.empty()) ++la_scope_;
      TypePtr te = fmt_annot ? infer_expr_expected(*b.expr, fmt_annot)
                             : infer_expr(*b.expr);
      if (upc && !upc->univars.empty()) --la_scope_;
      TypePtr annot = nullptr;
      // A declared type `let f : T = e`: check the inferred type's identities
      // against T (a distinct local type used where another is declared is an
      // error).  We only flag identity (stamp) clashes, not structural ones --
      // structural inference is still incomplete, so unifying T into te would
      // false-reject (e.g. array vs iarray); the identity layer is reliable.
      if (b.constraint_)
        if (auto* pc = std::get_if<Pvc_constraint>(&*b.constraint_)) {
          for (auto& u : pc->univars) newtype_vars[u.txt] = newtype_binding(u.txt);
          annot = fmt_annot ? fmt_annot : from_coretype(*pc->typ, avars);
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

  // Replay an enclosing-scope `open M` (module identifier) into this checker's
  // top scope: values, submodule names, record fields, and (non-strict)
  // ctor/type/alias qualifications -- mirroring the structure-level Pstr_open
  // path so a re-inferred submodule body sees the same names.
  void replay_open_module(const Longident& m) {
    for (auto& [k, v] : resolve_module_values(m)) venv.back()[k] = v;
    for (auto& s : module_submodule_names(m)) opened_submodules_.insert(s);
    load_module_record_fields(m);
    load_open_submod_quals(m);
    if (!strict) {
      load_open_type_quals(m);
      load_open_param_type_quals(m);
      open_module_ctors(m);
      load_open_module_aliases(m);
    }
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
      // `(A : S)` with A unresolvable here (an enclosing-scope module seen
      // during a submodule's re-inference): bind S's values at their DECLARED
      // types, not fresh vars -- `include (A : S); let z = f x` must infer
      // z : int through the sig's f : t -> int (includestruct).  Non-strict
      // only; the strict pass keeps the old degrade.
      if (!strict)
        if (auto* sg = std::get_if<Pmty_signature>(&mc->mt->desc)) {
          std::unordered_map<std::string, TypePtr> declared;
          for (auto& sit : sg->items)
            if (auto* pv = std::get_if<Psig_value>(&sit.desc)) {
              std::unordered_map<std::string, TypePtr> vars;
              if (TypePtr t = from_coretype(*pv->vd.type, vars))
                declared[pv->vd.name.txt] = t;
            }
          if (!declared.empty()) return declared;
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
      // KINDS pass: infer the value bindings of a literal STRUCT argument.
      // The functor-result harvest below never descends the argument, so a
      // `let module Compute = Diff.Right_variadic(struct .. let test .. end)`
      // body got NO inference records at all -- its matches then compiled
      // against the wrong ctor universe (includemod's functor-diff `test`
      // resolved the param column's bare `Unit`/`Named` as functor_arg_descr,
      // raising Match_failure on any Unit parameter).  Side-effect only (the
      // recorded kinds/disambiguations); the returned exports are unchanged.
      if (record_kinds_) {
        const ModuleExpr* h2 = &me;
        while (true) {
          const ModuleExpr* arg = nullptr;
          if (auto* a = std::get_if<Pmod_apply>(&h2->desc)) {
            arg = a->arg.get(); h2 = a->f.get();
          } else if (auto* au = std::get_if<Pmod_apply_unit>(&h2->desc)) {
            h2 = au->f.get();
          } else break;
          const ast::Pmod_structure* as2 =
              arg ? std::get_if<Pmod_structure>(&arg->desc) : nullptr;
          if (as2 && inferred_arg_structs_.insert(arg).second) {
            venv.emplace_back();
            tenv.emplace_back();
            cenv.emplace_back();
            process_items(as2->items);
            cenv.pop_back();
            tenv.pop_back();
            venv.pop_back();
          }
        }
      }
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
          // Overlay this module's extension ctors so they shadow an outer
          // same-named ctor for values later in the module (see ext_ctor_scheme_).
          for (auto& ec : tx->ext.ctors)
            if (auto s = ext_ctor_scheme_.find(&ec); s != ext_ctor_scheme_.end())
              cenv.back()[ec.name.txt] = s->second;
        } else if (auto* sv = std::get_if<Pstr_value>(&it.desc))
          infer_bindings(sv->rf, sv->bindings, /*toplevel=*/true);
        else if (auto* pc = std::get_if<Pstr_class>(&it.desc)) {
          // A `class .. and ..` group is mutually recursive: `new sibling arg`
          // in one body must resolve to (and pin) the sibling's constructor,
          // even a sibling defined later.  So we PRE-REGISTER a placeholder var
          // per class (shared, non-generic), infer every body against it, then
          // generalize once the whole group is closed.  Without this, a forward
          // `new bar "asdf"` saw no bar yet, so bar's unused param stayed 'a
          // instead of string (backtrace/methods, names).
          struct ClassPend {
            const ClassDeclaration* d;
            std::vector<const Pcl_fun*> params;  // `class c x = ...` parameters
            std::vector<const Pcl_let*> lets;    // `class c = let .. in object`
            const Pcl_structure* ps;
            TypePtr placeholder;  // the node registered in class_types_/ctor_
            bool paramless;
          };
          std::vector<ClassPend> pend;
          eng.enter_level();  // the ctor schemes generalize like a let group
          for (auto& d : pc->decls) {
            const ClassExpr* ce = &d.expr;
            std::vector<const Pcl_fun*> params;
            std::vector<const Pcl_let*> lets;
            for (;;) {
              if (auto* pf = std::get_if<Pcl_fun>(&ce->desc)) {
                params.push_back(pf);
                ce = pf->body.get();
              } else if (auto* pl = std::get_if<Pcl_let>(&ce->desc)) {
                lets.push_back(pl);
                ce = pl->body.get();
              } else if (auto* pcn = std::get_if<Pcl_constraint>(&ce->desc)) {
                ce = pcn->ce.get();  // `(object .. end : object .. end)`
              } else if (auto* po = std::get_if<Pcl_open>(&ce->desc)) {
                ce = po->body.get();  // `let open M in object .. end`
              } else break;
            }
            auto* ps = std::get_if<Pcl_structure>(&ce->desc);
            if (!ps) {
              // `class c = other [args]` (possibly through applications):
              // an ALIAS class.  Register c's object type as the target's
              // (value-param arrows consumed by the applied args), renamed
              // to c, and remember the target path for Cty_constr emission.
              const ClassExpr* h = ce;
              int napp = 0;
              for (;;) {
                if (auto* ap = std::get_if<Pcl_apply>(&h->desc)) {
                  napp += (int)ap->args.size(); h = ap->ce.get();
                } else if (auto* pl2 = std::get_if<Pcl_let>(&h->desc)) h = pl2->body.get();
                else if (auto* po2 = std::get_if<Pcl_open>(&h->desc)) h = po2->body.get();
                else if (auto* pcn2 = std::get_if<Pcl_constraint>(&h->desc)) h = pcn2->ce.get();
                else break;
              }
              if (auto* pcr = std::get_if<Pcl_constr>(&h->desc)) {
                std::string tgt = lid_last(pcr->id.txt);
                TypePtr t;
                if (auto f = class_types_.find(tgt); f != class_types_.end()) t = f->second;
                else if (auto f2 = class_ctor_types_.find(tgt); f2 != class_ctor_types_.end())
                  t = f2->second;
                if (t) {
                  TypePtr obj = I::Engine::repr(eng.instantiate(t));
                  while (napp-- > 0 && obj->kind == I::Type::Kind::Arrow)
                    obj = I::Engine::repr(obj->cod);
                  if (obj->kind == I::Type::Kind::Object) {
                    // a FRESH node (mutating the target's own abbrev would
                    // rename the target class everywhere)
                    TypePtr named = eng.object_type(obj->labels, obj->args);
                    named->abbrev = d.name.txt;
                    class_types_[d.name.txt] = named;
                    class_node_types_[&d] = named;
                    class_alias_refs_[&d] = lid_full(pcr->id.txt);
                    pend.push_back({&d, {}, {}, nullptr, named, true});
                  }
                }
              }
              continue;
            }
            bool paramless = params.empty() && d.params.empty();
            // Build an OBJECT SHELL (public concrete method names -> fresh
            // vars) so a forward `new sibling`'s `#meth arg` resolves against a
            // real row and pins the method's arg types -- a bare var receiver
            // would make `#meth` return a disconnected fresh var, losing the
            // pin (backtrace/methods' `other#go 1 2 3`).  Pass 2 unifies this
            // shell with the class' truly-inferred object, merging the rows.
            std::vector<std::string> shnames;
            std::vector<TypePtr> shvars;
            for (auto& f : ps->cs.fields)
              if (auto* m = std::get_if<Pcf_method>(&f.desc))
                if (std::get_if<Cfk_concrete>(&m->kind) && m->priv != PrivateFlag::Private) {
                  shnames.push_back(m->name.txt);
                  shvars.push_back(eng.fresh_var());
                }
            TypePtr ph = eng.object_type(shnames, shvars);
            for (size_t i = params.size(); i-- > 0;) {
              auto [lk, nm] = arglabel(params[i]->label);
              ph = eng.arrow(eng.fresh_var(), ph, lk, nm);  // value-param arrow
            }
            if (paramless) class_types_[d.name.txt] = ph;
            else class_ctor_types_[d.name.txt] = ph;
            class_node_types_[&d] = ph;
            pend.push_back({&d, std::move(params), std::move(lets), ps, ph, paramless});
          }
          for (auto& pe : pend) {
            if (!pe.ps) continue;  // alias class: no body to infer
            const ClassDeclaration& d = *pe.d;
            // The class's TYPE params (`class ['a] lambda_ops`) scope over the
            // body's annotations and `constraint` fields; the object then
            // carries the CLASS name (`'a lambda_ops`), and `new c` gets the
            // constructor arrow over the value params (mixin2).
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
            TypePtr selfty;
            TypePtr ot = infer_object_body(pe.ps->cs, pe.params.empty() ? nullptr : &pe.params,
                                           pe.lets.empty() ? nullptr : &pe.lets,
                                           d.params.empty() ? nullptr : &cvars,
                                           &ptys, &instvars, &selfty);
            if (selfty) class_self_types_[&d] = selfty;
            class_instvars_[d.name.txt] = std::move(instvars);
            // Unify the pre-registered placeholder with the real inferred
            // ctor/object type so forward `new` uses (which grabbed the
            // placeholder) flow back into this class' body.
            if (pe.paramless) {
              try_unify(pe.placeholder, ot);
              // Name the class object: a value `let o = new c` is typed BY
              // NAME in ocamlc (`val o : c`, the class ghost type), so the
              // bridge must see which class the row came from.
              TypePtr obj = I::Engine::repr(ot);
              if (obj->kind == I::Type::Kind::Object && obj->abbrev.empty())
                obj->abbrev = d.name.txt;
              TypePtr ph2 = I::Engine::repr(pe.placeholder);
              if (ph2->kind == I::Type::Kind::Object && ph2->abbrev.empty())
                ph2->abbrev = d.name.txt;
            } else {
              TypePtr obj = I::Engine::repr(ot);
              if (obj->kind == I::Type::Kind::Object && !d.params.empty()) {
                obj->abbrev = d.name.txt;
                obj->abbrev_args = tparams;
              }
              TypePtr ctor = ot;
              for (size_t i = ptys.size(); i-- > 0;) {
                auto [lk, nm] = arglabel(pe.params[i]->label);
                ctor = eng.arrow(ptys[i], ctor, lk, nm);
              }
              try_unify(pe.placeholder, ctor);
            }
            // The writer reads the class type from the map (= the placeholder,
            // which object-object unify does NOT link to `ot`), so record the
            // type params on the PLACEHOLDER's own object node too.
            if (!d.params.empty()) {
              TypePtr ph = I::Engine::repr(pe.placeholder);
              while (ph->kind == I::Type::Kind::Arrow) ph = I::Engine::repr(ph->cod);
              if (ph->kind == I::Type::Kind::Object) {
                ph->abbrev = d.name.txt;
                ph->abbrev_args = tparams;
              }
            }
          }
          eng.leave_level();
          // Generalise each scheme now that the group is closed, so each later
          // `new c` instantiates fresh.
          for (auto& pe : pend) eng.generalize(pe.placeholder);
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
          if (!strict && std::holds_alternative<Pmod_apply>(op->expr.desc)) {
            func_bind_name_ = resolve_local_module_path(op->expr);
            // ocamlc names a generalized open's result types by the RAW unit
            // path (Stdlib__Set.Make(String).t) -- the mangled head keys the
            // writer's raw emission; a source-written Set.Make(X).t stays
            // alias-routed (accepted_batch).
            if (!func_bind_name_.empty()) {
              std::string head =
                  func_bind_name_.substr(0, func_bind_name_.find_first_of(".("));
              if (!head.empty() && head.rfind("Stdlib__", 0) != 0) {
                std::string hc = head_cmi(head);
                std::string base = hc.substr(hc.rfind('/') + 1);
                if (base.rfind("stdlib__", 0) == 0 &&
                    std::filesystem::exists(hc))
                  func_bind_name_ = "Stdlib__" + func_bind_name_;
              }
            }
          }
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
              load_open_local_type_quals(pi->id.txt);  // local M: bare type -> M.t
              load_open_param_type_quals(pi->id.txt);  // functor-param X.t
              open_module_ctors(pi->id.txt);     // bare ctor -> M's variant ctor
              load_open_module_aliases(pi->id.txt);  // bare List -> ListLabels
            }
          }
        } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
          if (mb->binding.name.txt) {
            // Record the alias TARGET path (`module Sig_component_kind =
            // Shape.Sig_component_kind`) so a later `open Sig_component_kind`
            // opens the target's ctors (open_module_ctors; bootstrap bug#13).
            // ONLY plain module-path aliases: recording a functor/struct
            // binding (empty path) poisoned resolve_local_module_path for a
            // later `open F(X)` (shallow2deep's MkReify(PC).event collapsed).
            if (!strict &&
                std::holds_alternative<Pmod_ident>(mb->binding.expr.desc)) {
              std::string p = resolve_local_module_path(mb->binding.expr);
              if (!p.empty()) {
                auto [f, ins] =
                    local_module_paths_.emplace(*mb->binding.name.txt, p);
                if (!ins && f->second != p) f->second = "";  // conflicting rebind
              }
            }
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
                } else if (tgt.find('.') == std::string::npos &&
                           tgt != *mb->binding.name.txt &&
                           !bound_module_names_.count(tgt)) {
                  // `module T = Typedtree`: a bare top-level UNIT alias.  Unlike a
                  // dotted alias we DON'T display-rewrite (ocaml keeps each written
                  // path per-occurrence, and rewriting `Typedtree.` -> `T.` would
                  // corrupt cmi type paths), but type-directed disambiguation of a
                  // bare `exp.exp_desc` field / `Texp_ident` ctor against the unit's
                  // types still needs its records + ctors loaded -- otherwise a
                  // whole `match exp.exp_desc with ..` degenerates to arm 0 with
                  // unresolved `?`-vars (untypeast's mapper).  No-op if T has no cmi.
                  load_module_record_fields(pi->id.txt);
                  if (!strict) open_module_ctors(pi->id.txt);
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
          if (mt->type) {  // record a signature module type's value names for unpacks
            if (auto* sg = std::get_if<Pmty_signature>(&mt->type->desc)) {
              collect_sig_values(sg->items, modtype_env[mt->name.txt]);
              modtype_sig_asts_[mt->name.txt] = &sg->items;
            }
            // `module type S2 = S1`: the alias resolves to the target's items.
            else if (auto* pid = std::get_if<Pmty_ident>(&mt->type->desc))
              if (auto* l = std::get_if<Lident>(&pid->id.txt.v))
                if (auto f = modtype_sig_asts_.find(l->name);
                    f != modtype_sig_asts_.end()) {
                  modtype_sig_asts_[mt->name.txt] = f->second;
                  collect_sig_values(*f->second, modtype_env[mt->name.txt]);
                }
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
        if (mb->binding.name.txt) {
          ck.mod_prefix_ += *mb->binding.name.txt + ".";
          auto& names = ck.module_direct_types_[*mb->binding.name.txt];
          for (auto& sit : ms->items)
            if (auto* ty2 = std::get_if<Pstr_type>(&sit.desc))
              for (auto& d : ty2->decls) names.push_back(d.name.txt);
        }
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
  // Replay enclosing-scope opens (set by infer_signature when re-inferring a
  // submodule) so this level's value inference sees the outer file's `open`s.
  for (auto* m : ck.replay_opens_) ck.replay_open_module(*m);
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
static std::string kind_str(const TypePtr& t0, Checker& ck) {
  const std::set<std::string>& imm = ck.immediate_types_;
  TypePtr t = I::Engine::repr(t0);
  // functions and tuples are always boxed (Typeopt: Paddrarray, lazy Shortcut)
  if (t->kind == I::Type::Kind::Arrow || t->kind == I::Type::Kind::Tuple) return "addr";
  if (t->kind != I::Type::Kind::Constr) return "";
  auto d = t->path.rfind('.');
  std::string b = d == std::string::npos ? t->path : t->path.substr(d + 1);
  if (b == "int" || b == "char" || b == "bool" || b == "unit")
    return "int";  // immediates (unit is the immediate 0)
  if (imm.count(t->path)) return "int";  // all-constant local variant
  // A BARE name brought into scope by `open M` (`label` after `open Asttypes`):
  // the registration pre-pass converts a record field's declared type before
  // any opens are replayed, so the field's scheme cites the unqualified name
  // and the dotted cmi consults below never fire -- `tag = tag'` over
  // row_fields stayed the polymorphic caml_equal (types.ml get_row_field).
  // Requalify through the opened qual exactly as from_coretype's annotation
  // path does: a local decl (tenv stamp) takes precedence, and the qualified
  // head still passes through the bound_module_names_ guards below.
  std::string path = t->path;
  if (d == std::string::npos && !ck.tenv_lookup(b))
    if (auto q = ck.opened_type_quals_.find(b); q != ck.opened_type_quals_.end()) {
      path = q->second;
      d = path.rfind('.');
    }
  // A CROSS-MODULE type whose cmi decl is Type_immediacy.Always (see
  // cmi_type_is_immediate).  A locally bound module shadowing the unit name is
  // excluded (bound_module_names_, as in array_kind_str below).
  if (d != std::string::npos &&
      !ck.bound_module_names_.count(path.substr(0, path.find('.'))) &&
      ck.cmi_type_is_immediate(path))
    return "int";
  if (b == "float") return "float";
  if (b == "int32") return "int32";
  if (b == "int64") return "int64";
  if (b == "nativeint") return "nativeint";
  if (b == "string") return "string";  // not a value kind, but drives string compares
  // An `[@@unboxed]` wrapper of a string is represented as its string, so a
  // polymorphic compare on it specializes to caml_string_* (Typeopt.scrape_ty).
  // "string" is not a value kind either -- it only differs from "addr" in
  // enabling that specialization -- so this cannot change any value_kind.
  if (ck.is_unboxed_string(t->path)) return "string";
  // A cross-module type abbreviation resolving to string (`Asttypes.label`):
  // guard against a locally bound module shadowing the head name, as the
  // immediate case above does.
  if (d != std::string::npos &&
      !ck.bound_module_names_.count(path.substr(0, path.find('.'))) &&
      ck.cmi_type_resolves_to_string(path))
    return "string";
  // A known boxed type (record/block-variant/string/...): not a value kind, but
  // an `addr` array element (vs a type variable, which is `gen`).
  return "addr";
}

// The array-element kind for `t0`, tracking Typeopt.array_type_kind rather than
// the value_kind: an ABSTRACT type constructor classifies as `Any` (a GENERIC
// element -> `caml_array_get`), NOT `Addr`, even though it is boxed.  kind_str
// defaults any non-base Constr to "addr"; here we downgrade that to "" (gen)
// when the element's type name is known abstract (declared `type t` with no
// manifest in a sig/struct) and NOT also concretely defined -- matching
// ocamlc, which specializes to `caml_array_get_addr` only for record/variant
// elements.  Widening addr->gen is always runtime-safe (the generic accessor
// handles every element kind), so a missed abstract only costs fidelity.
static std::string array_kind_str(const TypePtr& t0, Checker& ck) {
  std::string k = kind_str(t0, ck);
  if (k != "addr") return k;
  TypePtr t = I::Engine::repr(t0);
  if (t->kind != I::Type::Kind::Constr) return k;
  auto d = t->path.rfind('.');
  if (d == std::string::npos) {
    // Bare element name (`act`, a local/param abstract un-qualified in scope):
    // widen to gen only when the file declares it abstract and never concrete.
    const std::string& b = t->path;
    if (ck.abstract_names_.count(b) && !ck.algebraic_names_.count(b)) return "";
    return k;
  }
  // Dotted `Q.b`: widen only for a FUNCTOR-PARAMETER abstract type (`A.t`,
  // `Id.t`) with no manifest -- collected exactly as "Q.b".  This avoids
  // conflating a cross-module concrete `Path.t`/`Location.t` (a variant/record)
  // with a same-named local abstract `t`.
  if (ck.param_abstract_quals_.count(t->path)) return "";
  // A CROSS-MODULE type (`Obj.t`, `Stdlib.Obj.t`): consult the owning unit's
  // cmi for the decl's kind -- exactly what Env.find_type gives classify.
  // Skipped when the HEAD is a locally bound module (a local `module Obj`
  // shadows the unit; bound_module_names_ is over-inclusive, which only costs
  // a missed widening, never a wrong one).
  std::string headm = t->path.substr(0, t->path.find('.'));
  if (!ck.bound_module_names_.count(headm) && ck.cmi_type_is_abstract(t->path))
    return "";
  return k;
}

// If `t0` is an array type, sets `out` to its element kind_str ("" for a generic
// element) and returns true; otherwise returns false.
static bool array_elem_str(const TypePtr& t0, Checker& ck, std::string& out) {
  TypePtr t = I::Engine::repr(t0);
  if (t->kind != I::Type::Kind::Constr || t->args.empty()) return false;
  auto d = t->path.rfind('.');
  std::string b = d == std::string::npos ? t->path : t->path.substr(d + 1);
  if (b != "array" && b != "iarray") return false;
  out = array_kind_str(t->args[0], ck);
  return true;
}

// Any reachable non-generalized (value-restriction "weak") type variable: the
// signature of an exported binding whose type still carries such a var can pin it
// against the unit's interface below.
static bool type_has_weak_var(const TypePtr& t0, std::set<I::Type*>& seen) {
  TypePtr t = I::Engine::repr(t0);
  if (!seen.insert(t.get()).second) return false;
  using K = I::Type::Kind;
  switch (t->kind) {
    case K::Var: return t->level != I::GENERIC_LEVEL;
    case K::Arrow:
      return type_has_weak_var(t->dom, seen) || type_has_weak_var(t->cod, seen);
    default:
      for (auto& a : t->args)
        if (type_has_weak_var(a, seen)) return true;
      return false;
  }
}

ValueKinds infer_value_kinds(const ast::Structure& s,
                             const std::string& iface_cmi_path) {
  Checker ck;
  ck.record_kinds_ = true;
  ck.collect_type_kinds(s);  // file-wide concrete/abstract type-decl kinds (array_kind_str)
  run_checker(ck, s);
  // Pin value-restriction weak vars in exported bindings against this unit's own
  // interface, mirroring Includemod's moregeneral: `all_passes = ref []` is
  // `'_weak list ref` in the .ml but the .mli declares `string list ref`; ocamlc
  // unifies the two during signature matching, pinning `'_weak := string` BEFORE
  // translation, so an eta-expanded comparison over it (`List.filter ((<>) s)`)
  // specializes to caml_string_notequal.  Only weak (non-generalized) vars are
  // touched: instantiate() shares them while refreshing generic vars, so a
  // polymorphic `let f x = x : 'a -> 'a` matched against a monomorphic .mli
  // signature is unaffected.  Guarded throughout -- a genuine mismatch (or a
  // missing/foreign cmi) is diagnosed by the real signature check, not here.
  if (!iface_cmi_path.empty()) {
    try {
      const auto& c = cmi::CmiFile::load(iface_cmi_path);
      for (auto& v : c.values()) {
        if (!v.prim.empty() || !v.type) continue;  // externals: no runtime binding
        auto f = ck.venv.back().find(v.name);
        if (f == ck.venv.back().end()) continue;
        std::set<I::Type*> seen;
        if (!type_has_weak_var(f->second, seen)) continue;
        std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
        TypePtr mli_ty = ck.from_cmi(v.type, memo);
        try { ck.eng.unify(ck.eng.instantiate(f->second), mli_ty); }
        catch (...) {}
      }
    } catch (...) {}
  }
  ck.resolve_pending_fields();  // re-resolve ambiguous field reads with final types
  ck.resolve_pending_disambig();  // ctor disambiguation with post-fixpoint types
  ValueKinds vk;
  for (auto& [p, t] : ck.rec_pat_) {
    vk.pat[p] = kind_str(t, ck);
    // An abstract-ctor-typed PATTERN (an array pattern's element): a generic
    // array element even though boxed -- mirrors the expression-side marking.
    if (vk.pat[p] == "addr" && array_kind_str(t, ck).empty()) vk.abstract_elem.insert(p);
    // A constructor pattern whose type resolved to a module-qualified variant:
    // record the path so the back end can register that type's constructors.
    if (std::holds_alternative<ast::Ppat_construct>(p->desc)) {
      TypePtr r = I::Engine::repr(t);
      if (getenv("CTDBG"))
        fprintf(stderr, "[CTDBG] rec_pat construct kind=%d path=%s\n",
                (int)r->kind,
                r->kind == I::Type::Kind::Constr ? r->path.c_str() : "-");
      // "exn" (dotless) is recorded too: an EXN-typed ctor pattern must take
      // the extension-identity reading even when a same-named variant ctor
      // squats the flat map (tmc.ml's error handler `function Error (..) ->`
      // vs result's builtin Error -- matching by variant TAG misread foreign
      // exceptions and crashed every error report).
      if (r->kind == I::Type::Kind::Constr &&
          (r->path.find('.') != std::string::npos || r->path == "exn"))
        vk.pat_constr[p] = r->path;
    }
    // A record pattern's matched-value type (resolved by unify with the
    // scrutinee): lets the back end disambiguate an ambiguous field by type.
    // A record pattern that is a constructor's argument gets its type from the
    // enclosing ctor's DECLARED arg type (authoritative, pinned above) in
    // preference to the unify-inferred `t` -- infer_pat on a bare record arg
    // can pick a wrong same-labelled record when a failed unify leaves `t`
    // pointing at it (Val_prim {prim_name} -> Primitive.description@0, not the
    // guessed Typedtree.primitive_description@1).
    if (std::holds_alternative<ast::Ppat_record>(p->desc)) {
      if (auto ra = ck.pat_record_arg_type_.find(p);
          ra != ck.pat_record_arg_type_.end()) {
        TypePtr r = I::Engine::repr(ra->second);
        if (r->kind == I::Type::Kind::Constr && !r->path.empty())
          vk.pat_record_type[p] = r->path;
      }
      if (!vk.pat_record_type.count(p)) {
        TypePtr r = I::Engine::repr(t);
        if (r->kind == I::Type::Kind::Constr && !r->path.empty())
          vk.pat_record_type[p] = r->path;
      }
    }
  }
  // CONSUMER half of the type-directed ctor-arg disambiguation (mirrors the
  // producer expr_constr override below): an ambiguous ctor PATTERN used as
  // another ctor's argument gets its owning type from the enclosing ctor's
  // declared arg type, so pat_ctor_resolve reads the right tag (matching the
  // construct side; a producer/consumer tag mismatch is a miscompile).
  for (auto& [p, d] : ck.pat_ctor_arg_type_) {
    TypePtr r = I::Engine::repr(d);
    if (r->kind == I::Type::Kind::Constr && !r->path.empty())
      vk.pat_constr[p] = r->path;
  }
  for (auto& [f, t] : ck.rec_ret_) vk.fn_ret[f] = kind_str(t, ck);
  for (auto& [e, t] : ck.rec_expr_) {
    vk.expr[e] = kind_str(t, ck);
    // An abstract-ctor-typed expr (`a.(i) : Id.t`) is a GENERIC array element
    // even though it is boxed ("addr"): array_kind_str downgrades it to gen.
    if (vk.expr[e] == "addr" && array_kind_str(t, ck).empty()) vk.abstract_elem.insert(e);
    std::string ek;
    if (array_elem_str(t, ck, ek)) vk.array_elem[e] = ek;  // "" = gen element
    // A function-typed reference whose first parameter is a specializable base
    // type: record the operand kind so an eta-expanded comparison primitive
    // lowers to the type-specialized comparison (int_replace_polymorphic_compare
    // -- `let (=) : int -> int -> bool = Stdlib.(=)`).
    if (TypePtr rt = I::Engine::repr(t); rt->kind == I::Type::Kind::Arrow) {
      std::string dk = kind_str(rt->dom, ck);
      if (dk == "int" || dk == "float" || dk == "string" || dk == "int32" ||
          dk == "int64" || dk == "nativeint")
        vk.cmp_operand[e] = dk;
    }
    TypePtr r = I::Engine::repr(t);
    if (r->kind == I::Type::Kind::Constr && r->path.find('.') != std::string::npos)
      vk.expr_constr[e] = r->path;  // module-qualified type, e.g. "Gc.stat"
    else if (r->kind == I::Type::Kind::Constr && !r->path.empty() &&
             std::holds_alternative<ast::Pexp_construct>(
                 static_cast<const ast::Expression*>(e)->desc)) {
      // A CONSTRUCT node's inferred type is recorded even when FILE-LOCAL
      // (dotless): the back end verifies the flat ctor entry's owning type
      // against it -- lambda.ml's own `lambda_of_const` builds
      // Lambda.structured_constant ctors whose names ALSO exist in the
      // opened Asttypes.constant at different tags (Const_nativeint 5 vs 6);
      // the flat map silently built the wrong tag and every nativeint/float
      // CONSTANT the bootstrapped compiler emitted was a corrupt block.
      // ONLY when the bare name uniquely identifies the decl AND the
      // inferred stamp agrees -- a shadowed name (patmatch's several `t`s,
      // functor-param types under `open T`) is unreliable here.
      auto bu = ck.bare_unique_stamp_.find(r->path);
      if (bu != ck.bare_unique_stamp_.end() && bu->second > 0 &&
          bu->second == r->stamp)
        vk.expr_constr[e] = r->path;
    }
  }
  // PRODUCER half: an ambiguous ctor CONSTRUCT used as another ctor's argument
  // gets its owning type from the enclosing ctor's declared arg type, so the
  // back end's construct-site override reads the right tag (see the pat_constr
  // consumer half above -- both must agree).  Authoritative even DOTLESS (a
  // file-local type), unlike the general rec_expr_ path.
  for (auto& [e, d] : ck.ctor_arg_type_) {
    TypePtr r = I::Engine::repr(d);
    if (r->kind == I::Type::Kind::Constr && !r->path.empty())
      vk.expr_constr[e] = r->path;
  }
  for (auto& [e, fr] : ck.field_resolved_)
    vk.field_resolved[e] = {std::get<0>(fr), std::get<1>(fr), std::get<2>(fr)};
  vk.format_lits = std::move(ck.fmt_lits_);
  vk.optional_erasures = std::move(ck.erasures_);
  vk.match_partial = std::move(ck.match_partial);
  vk.function_cases_partial = std::move(ck.function_cases_partial);
  vk.total_proven = std::move(ck.total_proven);
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
struct BridgeCtx {
  // In-progress nodes, pre-registered before their children bridge: a CYCLIC
  // engine graph (a recursive row/object, `< bark : 'a -> unit > t as 'a`)
  // closes back onto the one node instead of unrolling; the writer marshals
  // the loop as a CODE_SHARED back-reference and Printtyp prints `as 'a`.
  std::unordered_map<const I::Type*, cmi::cmiw::TyPtr> in_progress;
  std::unordered_map<const I::Type*, cmi::cmiw::TyPtr> nodes;  // shared rows -> one Ty node
  // SOURCE names for engine vars (a type decl's coretype tvars map reversed):
  // a GADT ctor's existential (`Test : 'b * 'a * ..`) keeps its written name.
  std::unordered_map<const I::Type*, std::string> var_names;
  // Universally-quantified names (a poly field's `'a.` binders): matching
  // vars emit as Tunivar, not Tvar.
  std::unordered_set<std::string> univars;
  // Bridging a WRITTEN declaration coretype (bridge_ty_named/bridge_label_ty):
  // open/upper rows are as-written, never weak '_weak rows, so they are
  // emittable even at non-generic level (`val bar : [< `A | `B ] t -> unit`).
  bool written = false;
};
static cmi::cmiw::TyPtr bridge_ty_rec(const TypePtr& t0,
                                      std::unordered_map<const I::Type*, int>& vars, int& nextvar,
                                      BridgeCtx& ctx);
static cmi::cmiw::TyPtr bridge_ty(const TypePtr& t0,
                                  std::unordered_map<const I::Type*, int>& vars, int& nextvar) {
  BridgeCtx ctx;
  return bridge_ty_rec(t0, vars, nextvar, ctx);
}
// As bridge_ty, but with the declaration's written tvar names (`name -> var`),
// so fresh vars carry Tvar(Some name) like ocamlc stores them.  A `shared`
// BridgeCtx spans several calls (one type DECLARATION's params + kind +
// manifest), so a non-var node reached twice -- a constrained param row also
// cited by a label -- bridges to ONE writer node and Printtyp names it 'a.
static cmi::cmiw::TyPtr bridge_ty_named(const TypePtr& t0,
                                        std::unordered_map<const I::Type*, int>& vars, int& nextvar,
                                        const std::unordered_map<std::string, TypePtr>& tvars,
                                        BridgeCtx* shared = nullptr) {
  BridgeCtx local;
  BridgeCtx& ctx = shared ? *shared : local;
  ctx.written = true;
  for (auto& [n, tp] : tvars)
    if (tp) ctx.var_names[I::Engine::repr(tp).get()] = n;
  return bridge_ty_rec(t0, vars, nextvar, ctx);
}
// Bridge a DECLARATION field/label coretype: a `'a. t` poly field becomes
// Ty::Poly over Tunivar binders; anything else = bridge_ty_named.
static cmi::cmiw::TyPtr bridge_label_ty(Checker& ck, const ast::CoreType& ct,
                                        std::unordered_map<std::string, TypePtr>& tvars,
                                        std::unordered_map<const I::Type*, int>& vars,
                                        int& nextvar,
                                        BridgeCtx* shared = nullptr) {
  auto* pp = std::get_if<Ptyp_poly>(&ct.desc);
  if (!pp || pp->vars.empty())
    return bridge_ty_named(ck.from_coretype(ct, tvars), vars, nextvar, tvars,
                           shared);
  TypePtr body = ck.from_coretype(ct, tvars);  // strips the poly, binds names
  BridgeCtx local;
  BridgeCtx& ctx = shared ? *shared : local;
  ctx.written = true;
  for (auto& [n, tp] : tvars)
    if (tp) ctx.var_names[I::Engine::repr(tp).get()] = n;
  std::vector<std::string> added;
  for (auto& n : pp->vars)
    if (ctx.univars.insert(n).second) added.push_back(n);
  auto b = bridge_ty_rec(body, vars, nextvar, ctx);
  for (auto& n : added) ctx.univars.erase(n);
  std::vector<int> pids;
  for (auto& n : pp->vars)
    if (auto tv2 = tvars.find(n); tv2 != tvars.end())
      if (auto vi = vars.find(I::Engine::repr(tv2->second).get()); vi != vars.end())
        pids.push_back(vi->second);
  return cmi::cmiw::ty_poly(std::move(b), std::move(pids));
}
static cmi::cmiw::TyPtr bridge_ty_body(const TypePtr& t,
                                       std::unordered_map<const I::Type*, int>& vars, int& nextvar,
                                       BridgeCtx& ctx);
static cmi::cmiw::TyPtr bridge_ty_rec(const TypePtr& t0,
                                      std::unordered_map<const I::Type*, int>& vars, int& nextvar,
                                      BridgeCtx& ctx) {
  TypePtr t = I::Engine::repr(t0);
  // A node explicitly registered for sharing (a constrained decl PARAM --
  // its label/manifest citations must Tlink to the one emitted node so
  // Printtyp prints the param's alias name back) short-circuits here.
  // Var nodes are never registered: their branch re-applies the source
  // name on every occurrence.
  if (t->kind != I::Type::Kind::Var && t->kind != I::Type::Kind::Variant &&
      t->kind != I::Type::Kind::Object)
    if (auto it = ctx.nodes.find(t.get()); it != ctx.nodes.end())
      return it->second;
  if (t->kind == I::Type::Kind::Var || t->kind == I::Type::Kind::Link)
    return bridge_ty_body(t, vars, nextvar, ctx);
  // Pre-register an in-progress node: a cycle through this node returns it
  // instead of unrolling/degrading, and the finished shape is grafted in
  // below.  A body result that nothing cycled onto is returned as-is.
  if (auto it = ctx.in_progress.find(t.get()); it != ctx.in_progress.end())
    return it->second;
  auto node = std::make_shared<cmi::cmiw::Ty>();
  ctx.in_progress[t.get()] = node;
  auto res = bridge_ty_body(t, vars, nextvar, ctx);
  ctx.in_progress.erase(t.get());
  if (node.use_count() == 1) return res;  // no cycle closed onto the node
  *node = *res;  // graft (copy: res may be a previously-registered shared node)
  // A registration the body just made must keep pointing at the SURVIVING node.
  if (auto it = ctx.nodes.find(t.get()); it != ctx.nodes.end() && it->second == res)
    it->second = node;
  return node;
}
static cmi::cmiw::TyPtr bridge_ty_body(const TypePtr& t,
                                       std::unordered_map<const I::Type*, int>& vars, int& nextvar,
                                       BridgeCtx& ctx) {
  auto bridge_ty = [&](const TypePtr& u, std::unordered_map<const I::Type*, int>& v, int& nv) {
    return bridge_ty_rec(u, v, nv, ctx);
  };
  using K = I::Type::Kind;
  switch (t->kind) {
    case K::Var: {
      // Every occurrence carries the name (not just the first-bridged one):
      // TyEmit memoizes vars by id in MARSHAL order, so if an unnamed
      // duplicate (e.g. a GADT ctor's cd_args copy, marshalled before cd_res)
      // reached the writer first, the name would be lost (`'f` printed 'a).
      auto it = vars.find(t.get());
      int id = (it != vars.end()) ? it->second : nextvar++;
      if (it == vars.end()) vars[t.get()] = id;
      auto v = cmi::cmiw::ty_var(id);
      if (auto nm = ctx.var_names.find(t.get()); nm != ctx.var_names.end())
        v->var_name = nm->second;
      else if (!t->rigid_name.empty())
        v->var_name = t->rigid_name;  // a generalized `(type t)` newtype
      else if (!t->var_hint.empty())
        v->var_name = t->var_hint;  // source-annotated var in an INFERRED val
      if (!v->var_name.empty() && ctx.univars.count(v->var_name))
        v->univar = true;  // a poly field's `'a.` binder -> Tunivar
      return v;
    }
    case K::Object: {  // structural object `< m1 : t1; m2 : t2 [; ..] >`
      // A row referenced twice in one scheme must bridge to ONE Ty node (the
      // writer then marshals it shared and Printtyp names it `as 'a`); a
      // RECURSIVE object type (`< bark : 'self -> unit > as 'self`) closes
      // through the wrapper's in_progress node.
      // A NAMED class object (abbrev = the class / class-type name, args =
      // its type params) is stored by ocamlc as a Tconstr of the class ghost
      // type (`let o = new c` gives `val o : c`), which the writer resolves
      // through the class item's Local ident.
      if (!t->abbrev.empty() && t->variant_kind != 1) {
        std::vector<cmi::cmiw::TyPtr> as;
        for (auto& a : t->abbrev_args) as.push_back(bridge_ty(a, vars, nextvar));
        return cmi::cmiw::ty_constr(t->abbrev, std::move(as));
      }
      // A named OPEN object (`#c`, e.g. a coerced class param): the full row
      // with Tobject's name = Some(c, rowvar :: params); Printtyp prints `#c`.
      // The emitter supplies the row variable as the first name arg.
      if (auto it = ctx.nodes.find(t.get()); it != ctx.nodes.end()) return it->second;
      std::vector<cmi::cmiw::TyPtr> mtys;
      for (std::size_t mi = 0; mi < t->args.size(); ++mi) {
        // A WRITTEN poly method `< m : 'a. 'a t >` (binders recorded by
        // from_coretype): register the binder names as univars, bridge the
        // body, and wrap it in Tpoly citing the binder ids.
        if (mi < t->method_polys.size() && !t->method_polys[mi].empty()) {
          std::vector<std::string> added;
          for (std::size_t bj = 0; bj < t->method_polys[mi].size(); ++bj) {
            const auto& bn = t->method_poly_names[mi][bj];
            auto* bp = I::Engine::repr(t->method_polys[mi][bj]).get();
            ctx.var_names[bp] = bn;
            if (ctx.univars.insert(bn).second) added.push_back(bn);
          }
          auto body = bridge_ty(t->args[mi], vars, nextvar);
          std::vector<int> pids;
          for (auto& bv : t->method_polys[mi])
            if (auto vi = vars.find(I::Engine::repr(bv).get()); vi != vars.end())
              pids.push_back(vi->second);
          for (auto& n : added) ctx.univars.erase(n);
          mtys.push_back(cmi::cmiw::ty_poly(std::move(body), std::move(pids)));
        } else {
          mtys.push_back(bridge_ty(t->args[mi], vars, nextvar));
        }
      }
      auto ty = cmi::cmiw::ty_object(t->labels, std::move(mtys));
      // engine variant_kind 1 on an Object marks an OPEN row (`< ..; .. >`)
      if (t->variant_kind == 1) ty->row_kind = 0;
      if (!t->abbrev.empty() && t->variant_kind == 1) {
        ty->row_name = t->abbrev;
        for (auto& a : t->abbrev_args)
          ty->row_name_args.push_back(bridge_ty(a, vars, nextvar));
      }
      ctx.nodes[t.get()] = ty;
      return ty;
    }
    case K::Variant: {  // polymorphic-variant row
      // A fixpoint row (`'a lambda as 'a`) recursing into this same node --
      // even through the abbrev branch below -- closes onto the wrapper's
      // in_progress node.
      // An abbreviated EXACT row (`val crash : var_t`) is stored by ocamlc as a
      // plain Tconstr of the abbreviation, not the expanded row.
      if (!t->abbrev.empty() && t->variant_kind == 2 && !t->from_inherit) {
        std::vector<cmi::cmiw::TyPtr> as;
        for (auto& a : t->abbrev_args) as.push_back(bridge_ty(a, vars, nextvar));
        return cmi::cmiw::ty_constr(t->abbrev, std::move(as));
      }
      // A NAMED row bound (`[< int u]` -- an inherit expanded to its full
      // tag set with the abbreviation stamped back): emittable as the row
      // plus row_desc.row_name = (u, [int]); Printtyp prints
      // `[< int u > `A ]`, exactly ocamlc's storage.
      // from_inherit is NOT required: a `#t` pattern's row (hash_type_row) is a
      // named `[< t]` upper bound too (pr11887's `#T.a1` scrutinee), stored by
      // ocamlc as the row with row_name = (t, args) -- Printtyp prints `[< t]`.
      // A weak (non-generic) such row is still caught by the level guard below.
      bool named_bound = !t->abbrev.empty() && t->variant_kind != 2 &&
                         !t->labels.empty() && t->inherited.empty();
      // Anonymous inherited-row bounds and weak (non-generalized, '_weak)
      // open/upper rows stay opaque for now.  An EXACT row is emittable
      // regardless of level: declaration coretypes converted during the
      // emission phase are never generalized, and a weak row is never exact.
      // A WRITTEN open/upper row (ctx.written -- sig val / decl coretypes)
      // is as-written, never weak: emittable too.
      if ((!named_bound && (!t->abbrev.empty() || !t->inherited.empty())) ||
          (t->level != I::GENERIC_LEVEL && t->variant_kind != 2 &&
           !ctx.written)) {
        if (getenv("ROWDBG")) {
          fprintf(stderr, "[rowdbg] abbrev=%s vk=%d from_inh=%d lvl=%d written=%d labels=[",
                  t->abbrev.c_str(), t->variant_kind, (int)t->from_inherit,
                  t->level, (int)ctx.written);
          for (auto& l : t->labels) fprintf(stderr, "%s,", l.c_str());
          fprintf(stderr, "] present=[");
          for (auto& p : t->present) fprintf(stderr, "%s,", p.c_str());
          fprintf(stderr, "] inh=%zu abbrev_args=%zu\n", t->inherited.size(),
                  t->abbrev_args.size());
        }
        return cmi::cmiw::ty_var(nextvar++);
      }
      if (auto it = ctx.nodes.find(t.get()); it != ctx.nodes.end()) return it->second;
      std::vector<cmi::cmiw::TyPtr> targs;
      std::vector<char> conj;
      bool any_conj = false;
      for (std::size_t i = 0; i < t->labels.size(); ++i) {
        bool has = i < t->tag_has_arg.size() && t->tag_has_arg[i] &&
                   i < t->args.size() && t->args[i];
        targs.push_back(has ? bridge_ty(t->args[i], vars, nextvar) : nullptr);
        // tag_has_arg == 2: conjunctive constant (`` `A of & t ``)
        bool c = has && t->tag_has_arg[i] == 2;
        conj.push_back(c ? 1 : 0);
        any_conj |= c;
      }
      auto ty = cmi::cmiw::ty_variant_row(t->labels, std::move(targs),
                                          t->variant_kind, t->present);
      if (any_conj) ty->pv_conj = std::move(conj);
      if (named_bound) {
        ty->row_name = t->abbrev;
        for (auto& a : t->abbrev_args)
          ty->row_name_args.push_back(bridge_ty(a, vars, nextvar));
      }
      ctx.nodes[t.get()] = ty;
      return ty;
    }
    case K::Arrow:
      return cmi::cmiw::ty_arrow_lbl(bridge_ty(t->dom, vars, nextvar),
                                     bridge_ty(t->cod, vars, nextvar),
                                     t->arrow_label, t->arrow_lbl);
    case K::Tuple: {
      std::vector<cmi::cmiw::TyPtr> as;
      for (auto& a : t->args) as.push_back(bridge_ty(a, vars, nextvar));
      auto r = cmi::cmiw::ty_tuple(std::move(as));
      // Labeled-tuple component labels ride pv_tags ("" = unlabeled).
      if (!t->labels.empty()) r->pv_tags = t->labels;
      return r;
    }
    case K::Constr: {
      // A generalized rigid newtype (`let f (type t) () = ..` escaping into
      // f's scheme): ocamlc stores its face as Tvar(Some "t") -- one shared
      // var per node, carrying the SOURCE name.
      if (t->rigid && t->path.empty()) {
        auto it = vars.find(t.get());
        if (it != vars.end()) return cmi::cmiw::ty_var(it->second);
        int id = nextvar++; vars[t.get()] = id;
        auto v = cmi::cmiw::ty_var(id);
        v->var_name = t->rigid_name;
        return v;
      }
      // A first-class module `(module S)` is an engine Constr with the parened
      // path; `with type` constraints live in labels/args.  ocamlc stores
      // Tpackage{pack_path; pack_constraints}.
      if (t->path.rfind("(module ", 0) == 0 && t->path.back() == ')') {
        std::string mty = t->path.substr(8, t->path.size() - 9);
        std::vector<cmi::cmiw::TyPtr> ctys;
        std::vector<std::string> cnames;
        if (!t->labels.empty() && t->labels.size() == t->args.size()) {
          cnames = t->labels;
          for (auto& a : t->args) ctys.push_back(bridge_ty(a, vars, nextvar));
        }
        auto pk = cmi::cmiw::ty_package(std::move(mty), std::move(cnames),
                                        std::move(ctys));
        pk->binder = t->abbrev;  // `(module M : T)` param: M, else empty
        return pk;
      }
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
      // (A source-written `CamlinternalLazy.t` stays as written: ocamlc stores
      // the declared path -- lazy.mli's own `type 'a t = 'a CamlinternalLazy.t`
      // must cite the internal unit, not fold back to `Lazy.t` (self-capture).
      // The engine's inferred lazies are canonical `lazy_t`, not this path, so
      // no display back-map is needed here.)
      auto r = cmi::cmiw::ty_constr(path, std::move(as));
      r->engine_stamp = t->stamp;  // decl identity for shadow-aware citation
      if (getenv("BRIDGEDBG"))
        fprintf(stderr, "[bridge] constr %s stamp=%d\n", r->name.c_str(), t->stamp);
      return r;
    }
    case K::Link: return bridge_ty(t->link, vars, nextvar);
  }
  return cmi::cmiw::ty_var(nextvar++);
}

// A FLAT polymorphic-variant abbreviation with all-direct tags (no inheritance,
// no conjunctive `of t1 & t2`): emit its row so a consumer's `#view` pattern
// resolves the tags cross-module and Printtyp renders the `[ .. ]` / `[> .. ]`
// / `[< .. ]` bound verbatim.  The bound maps to a row_kind: exact `[ ]` -> 2
// (row_more Tnil), open `[> ]` -> 0 (row_more Tvar, all RFpresent), upper
// `[< .. > present]` -> 1 (present tags RFpresent, the rest RFeither).  An
// abbreviation that INHERITS another polyvariant (`[ Simple.view | `Or ]`)
// returns null (emitting only its direct tags would be an INCOMPLETE set).
// Collect a Ptyp_variant's tags.  An INHERITED row (`[ `B | g | `C ]`) is
// spliced when it resolves to a local abbreviation whose manifest is itself an
// EXACT flat polyvariant (a complete tag set), recursively; the writer stores
// row_fields sorted, so splice position doesn't matter (ocamlc prints
// alphabetically).  Returns false when any field isn't representable
// (conjunctive `of t1 & t2`, an unresolvable/open inherit).
static bool collect_pv_row(
    Checker& ck, const Ptyp_variant& pv,
    std::vector<std::string>& tags, std::vector<cmi::cmiw::TyPtr>& targs,
    std::unordered_map<std::string, TypePtr>& tvars,
    std::unordered_map<const I::Type*, int>& bvars, int& nextvar,
    std::set<std::string>& visiting, bool allow_inherit) {
  for (auto& rf : pv.rows) {
    if (auto* rt = std::get_if<Rtag>(&rf)) {
      if (rt->types.size() > 1) return false;  // conjunctive `of t1 & t2`
      tags.push_back(rt->name);
      targs.push_back(rt->constant || rt->types.empty()
                          ? nullptr
                          : bridge_ty_named(ck.from_coretype(*rt->types[0], tvars),
                                            bvars, nextvar, tvars));
    } else if (auto* ri = std::get_if<Rinherit>(&rf)) {
      if (!allow_inherit) return false;
      auto* pc = std::get_if<Ptyp_constr>(&ri->ct->desc);
      if (!pc || !pc->args.empty()) return false;
      auto* l = std::get_if<Lident>(&pc->id.txt.v);
      if (!l || visiting.count(l->name)) return false;
      auto ai = ck.type_aliases.find(l->name);
      if (ai == ck.type_aliases.end() || !ai->second.manifest) return false;
      auto* ipv = std::get_if<Ptyp_variant>(&ai->second.manifest->desc);
      if (!ipv || ipv->closed != ClosedFlag::Closed || ipv->labels)
        return false;  // only an EXACT inherited row is a complete tag set
      visiting.insert(l->name);
      if (!collect_pv_row(ck, *ipv, tags, targs, tvars, bvars, nextvar,
                          visiting, true))
        return false;
      visiting.erase(l->name);
    } else {
      return false;
    }
  }
  return true;
}

static cmi::cmiw::TyPtr pv_row_manifest(
    Checker& ck, const ast::CoreType& t,
    std::unordered_map<std::string, TypePtr>& tvars,
    std::unordered_map<const I::Type*, int>& bvars, int& nextvar) {
  auto* pv = std::get_if<Ptyp_variant>(&t.desc);
  if (!pv) return nullptr;
  std::vector<std::string> tags; std::vector<cmi::cmiw::TyPtr> targs;
  int rk; std::vector<std::string> present;
  if (pv->closed == ClosedFlag::Open) rk = 0;            // `[> .. ]`
  else if (pv->labels) { rk = 1; present = *pv->labels; }  // `[< .. > present]`
  else rk = 2;                                            // `[ .. ]` exact
  std::set<std::string> visiting;
  // Inherit-splicing only inside an EXACT row (the spliced set stays complete).
  if (!collect_pv_row(ck, *pv, tags, targs, tvars, bvars, nextvar, visiting,
                      /*allow_inherit=*/rk == 2))
    return nullptr;
  if (tags.empty()) return nullptr;
  return cmi::cmiw::ty_variant_row(std::move(tags), std::move(targs), rk,
                                   std::move(present));
}

// ---- Variance computation (Typedecl_variance port) ----
// Types.Variance bit encoding: May_pos=1, May_neg=2+4, May_weak=4, Inj=8,
// Pos=16+8+1, Neg=32+8+4+2, Inv=63; unknown=7, covariant=Pos, full=Inv.
namespace vrn {
constexpr int MAY_POS = 1, MAY_NEG = 6, MAY_WEAK = 4, INJ = 8, POS = 25,
              NEG = 46, INV = 63, UNKNOWN = 7;
inline bool mem(int f, int v) { return (v & f) == f; }
inline int set_if(bool b, int f, int v) { return b ? (v | f) : v; }
inline int make(bool p, bool n, bool i) {
  return set_if(p, MAY_POS, set_if(n, MAY_NEG, set_if(i, INJ, 0)));
}
inline int conjugate(int v) {
  int vp = v & (INJ | MAY_WEAK);
  int r = set_if(mem(MAY_NEG, v), MAY_POS, set_if(mem(MAY_POS, v), MAY_NEG, vp));
  return set_if(mem(NEG, v), POS, set_if(mem(POS, v), NEG, r));
}
inline int compose(int v1, int v2) {
  if (mem(INV, v1) && mem(INJ, v2)) return INV;
  bool mp = (mem(MAY_POS, v1) && mem(MAY_POS, v2)) ||
            (mem(MAY_NEG, v1) && mem(MAY_NEG, v2));
  bool mn = (mem(MAY_POS, v1) && mem(MAY_NEG, v2)) ||
            (mem(MAY_NEG, v1) && mem(MAY_POS, v2));
  bool mw = (mem(MAY_WEAK, v1) && v2 != 0) || (v1 != 0 && mem(MAY_WEAK, v2));
  bool inj = mem(INJ, v1) && mem(INJ, v2);
  bool pos = (mem(POS, v1) && mem(POS, v2)) || (mem(NEG, v1) && mem(NEG, v2));
  bool neg = (mem(POS, v1) && mem(NEG, v2)) || (mem(NEG, v1) && mem(POS, v2));
  int v = 0;
  v = set_if(mp, MAY_POS, v); v = set_if(mn, MAY_NEG, v);
  v = set_if(mw, MAY_WEAK, v); v = set_if(inj, INJ, v);
  v = set_if(pos, POS, v); v = set_if(neg, NEG, v);
  return v;
}
inline int strengthen(int v) {
  return mem(MAY_NEG, v) ? v : (v & (INV - MAY_WEAK));
}
}  // namespace vrn

// Occurrence walk over an AST core type (compute_variance's shape).  `slot`
// maps a param var name to its index; Tconstr args compose through the
// callee's variance signature (current group iterate, earlier local decls,
// predef table, or the head unit's cmi); an unresolvable callee walks its
// args with Variance.unknown, exactly like ocamlc's Not_found branch.
static void variance_walk(
    Checker& ck, const CoreType& t, int v,
    const std::map<std::string, std::size_t>& slot, std::vector<int>& vari,
    const std::map<std::string, std::vector<int>>& group, int depth = 0) {
  if (depth > 60) return;
  auto same = [&](const CoreType& t2) {
    variance_walk(ck, t2, v, slot, vari, group, depth + 1);
  };
  if (auto* var = std::get_if<Ptyp_var>(&t.desc)) {
    if (auto s = slot.find(var->name); s != slot.end())
      vari[s->second] |= v;
  } else if (auto* ar = std::get_if<Ptyp_arrow>(&t.desc)) {
    variance_walk(ck, *ar->dom, vrn::conjugate(v), slot, vari, group, depth + 1);
    same(*ar->cod);
  } else if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
    for (auto& e : tu->elems) same(*e);
  } else if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
    if (c->args.empty()) return;
    std::vector<int> sig;
    if (auto* l = std::get_if<Lident>(&c->id.txt.v))
      if (auto g = group.find(l->name);
          g != group.end() && g->second.size() == c->args.size())
        sig = g->second;
    if (sig.empty()) sig = ck.external_type_variance_sig(*c, c->args.size());
    for (std::size_t i = 0; i < c->args.size(); ++i)
      variance_walk(ck, *c->args[i],
                    sig.size() == c->args.size() ? vrn::compose(v, sig[i])
                                                 : vrn::UNKNOWN,
                    slot, vari, group, depth + 1);
  } else if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) {
    if (auto s = slot.find(al->name); s != slot.end()) vari[s->second] |= v;
    same(*al->type);
  } else if (auto* pv = std::get_if<Ptyp_variant>(&t.desc)) {
    std::set<std::string> present;
    if (pv->labels) present.insert(pv->labels->begin(), pv->labels->end());
    bool upper = pv->closed == ClosedFlag::Closed && pv->labels;
    for (auto& r : pv->rows) {
      if (auto* rt = std::get_if<Rtag>(&r)) {
        int fv = (upper && !present.count(rt->name)) ? (v & vrn::UNKNOWN) : v;
        for (auto& ty : rt->types)
          variance_walk(ck, *ty, fv, slot, vari, group, depth + 1);
      } else if (auto* ri = std::get_if<Rinherit>(&r)) {
        same(*ri->ct);
      }
    }
  } else if (auto* ob = std::get_if<Ptyp_object>(&t.desc)) {
    for (auto& f : ob->fields) {
      if (auto* ot = std::get_if<Otag>(&f)) same(*ot->type);
      else if (auto* oi = std::get_if<Oinherit>(&f)) same(*oi->type);
    }
  } else if (auto* po = std::get_if<Ptyp_poly>(&t.desc)) {
    same(*po->type);
  } else if (auto* pk = std::get_if<Ptyp_package>(&t.desc)) {
    for (auto& [_, ct] : pk->constraints)
      variance_walk(ck, *ct, vrn::compose(v, vrn::INV), slot, vari, group,
                    depth + 1);
  } else if (auto* cl = std::get_if<Ptyp_class>(&t.desc)) {
    for (auto& a : cl->args)
      variance_walk(ck, *a, vrn::UNKNOWN, slot, vari, group, depth + 1);
  }
}

// compute_variance_type's finalization for one decl.  `is_var[i]` = the
// param is still a type VARIABLE at type level; a row-aliased param
// (`[< .. ] as 'a` -- the param IS the row) takes ocamlc's non-Tvar
// branches: required (p,n) always applied, plus full/covariant on concrete
// kinds.
static std::vector<int> variance_finalize(
    const TypeDeclaration& d, const std::vector<int>& vari, bool concr,
    bool do_strengthen, const std::vector<bool>& is_var) {
  std::vector<int> out(vari.size());
  for (std::size_t i = 0; i < vari.size(); ++i) {
    int req = i < d.param_variances.size() ? d.param_variances[i] : 7;
    bool p = (req & 1) != 0, n = (req & 2) != 0;
    bool priv = d.priv == PrivateFlag::Private;
    bool set_pn = priv || !is_var[i];
    int v = vari[i] |
            vrn::make(set_pn ? p : false, set_pn ? n : false, concr);
    if (concr && !is_var[i])
      v |= p ? (n ? vrn::INV : vrn::POS) : vrn::conjugate(vrn::POS);
    out[i] = do_strengthen ? vrn::strengthen(v) : v;
  }
  return out;
}

// Does the core type contain `.. as 'name` (a row/object alias binding)?
static bool has_alias_named(const CoreType& t, const std::string& name,
                            int depth = 0) {
  if (depth > 60) return false;
  if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) {
    if (al->name == name) return true;
    return has_alias_named(*al->type, name, depth + 1);
  }
  if (auto* ar = std::get_if<Ptyp_arrow>(&t.desc))
    return has_alias_named(*ar->dom, name, depth + 1) ||
           has_alias_named(*ar->cod, name, depth + 1);
  if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
    for (auto& e : tu->elems)
      if (has_alias_named(*e, name, depth + 1)) return true;
    return false;
  }
  if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
    for (auto& a : c->args)
      if (has_alias_named(*a, name, depth + 1)) return true;
    return false;
  }
  if (auto* pv = std::get_if<Ptyp_variant>(&t.desc)) {
    for (auto& r : pv->rows) {
      if (auto* rt = std::get_if<Rtag>(&r)) {
        for (auto& ty : rt->types)
          if (has_alias_named(*ty, name, depth + 1)) return true;
      } else if (auto* ri = std::get_if<Rinherit>(&r)) {
        if (has_alias_named(*ri->ct, name, depth + 1)) return true;
      }
    }
    return false;
  }
  if (auto* ob = std::get_if<Ptyp_object>(&t.desc)) {
    for (auto& f : ob->fields) {
      if (auto* ot = std::get_if<Otag>(&f)) {
        if (has_alias_named(*ot->type, name, depth + 1)) return true;
      } else if (auto* oi = std::get_if<Oinherit>(&f)) {
        if (has_alias_named(*oi->type, name, depth + 1)) return true;
      }
    }
    return false;
  }
  if (auto* po = std::get_if<Ptyp_poly>(&t.desc))
    return has_alias_named(*po->type, name, depth + 1);
  return false;
}

// One decl's computed variance list, or empty when the decl keeps the
// writer's current behavior (abstract-without-manifest, constrained).
static std::vector<int> compute_decl_variance(
    Checker& ck, const TypeDeclaration& d,
    const std::map<std::string, std::vector<int>>& group) {
  if (d.params.empty() || !d.constraints.empty()) return {};
  auto* rec = std::get_if<Ptype_record>(&d.kind);
  auto* var = std::get_if<Ptype_variant>(&d.kind);
  bool abstract_kind = !rec && !var;
  if (abstract_kind && !d.manifest) return {};
  std::map<std::string, std::size_t> slot;
  for (std::size_t i = 0; i < d.params.size(); ++i)
    if (auto* pv = std::get_if<Ptyp_var>(&d.params[i]->desc))
      slot[pv->name] = i;
  bool is_gadt = false;
  if (var)
    for (auto& c : var->ctors)
      if (c.res) is_gadt = true;
  if (!is_gadt) {
    std::vector<int> vari(d.params.size(), 0);
    if (d.manifest)
      variance_walk(ck, **d.manifest, vrn::POS, slot, vari, group);
    if (rec)
      for (auto& f : rec->fields)
        variance_walk(ck, *f.type,
                      f.mut == MutableFlag::Mutable ? vrn::INV : vrn::POS,
                      slot, vari, group);
    if (var)
      for (auto& c : var->ctors) {
        if (auto* tup = std::get_if<Pcstr_tuple>(&c.args))
          for (auto& e : tup->elems)
            variance_walk(ck, *e, vrn::POS, slot, vari, group);
        else if (auto* r = std::get_if<Pcstr_record>(&c.args))
          for (auto& f : r->fields)
            variance_walk(ck, *f.type,
                          f.mut == MutableFlag::Mutable ? vrn::INV : vrn::POS,
                          slot, vari, group);
      }
    bool concr = !abstract_kind;
    bool do_strengthen = !d.manifest || !abstract_kind;
    // A row-aliased param (`[< .. ] as 'a` anywhere in the body): the param
    // is INSTANTIATED to the row at type level -- non-Tvar finalization.
    std::vector<bool> is_var(d.params.size(), true);
    for (std::size_t i = 0; i < d.params.size(); ++i) {
      auto* pv2 = std::get_if<Ptyp_var>(&d.params[i]->desc);
      if (!pv2) { is_var[i] = false; continue; }
      auto aliased = [&](const CoreType& t) {
        return has_alias_named(t, pv2->name);
      };
      if (d.manifest && aliased(**d.manifest)) is_var[i] = false;
      if (rec)
        for (auto& f : rec->fields)
          if (aliased(*f.type)) is_var[i] = false;
      if (var)
        for (auto& c : var->ctors) {
          if (auto* tup = std::get_if<Pcstr_tuple>(&c.args)) {
            for (auto& e : tup->elems)
              if (aliased(*e)) is_var[i] = false;
          } else if (auto* r = std::get_if<Pcstr_record>(&c.args)) {
            for (auto& f : r->fields)
              if (aliased(*f.type)) is_var[i] = false;
          }
        }
    }
    return variance_finalize(d, vari, concr, do_strengthen, is_var);
  }
  // GADT: per-constructor computation with the result type's args standing
  // in for the params (compute_variance_gadt), unioned; type_private forced
  // Private per part; strengthen applied to the union (kind is concrete).
  std::vector<int> acc(d.params.size(), 0);
  bool any_part = false;
  auto add_part = [&](const std::vector<int>& part) {
    for (std::size_t i = 0; i < acc.size() && i < part.size(); ++i)
      acc[i] |= part[i];
    any_part = true;
  };
  TypeDeclaration dpriv_proto;  // finalize under Private via a flag instead
  if (d.manifest) {
    std::vector<int> vari(d.params.size(), 0);
    variance_walk(ck, **d.manifest, vrn::POS, slot, vari, group);
    std::vector<int> part(vari.size());
    for (std::size_t i = 0; i < vari.size(); ++i) {
      int req = i < d.param_variances.size() ? d.param_variances[i] : 7;
      part[i] = vari[i] | vrn::make((req & 1) != 0, (req & 2) != 0, true);
    }
    add_part(part);
  }
  for (auto& c : var->ctors) {
    std::map<std::string, std::size_t> cslot;
    // Which per-position "params" are type VARIABLES: a GADT ctor's result
    // args stand in for the params (compute_variance_gadt), and a CONCRETE
    // ret arg (`V : value cat`) takes the non-Tvar finalization -- union
    // with full/covariant per the required (p,n).
    std::vector<bool> is_var(d.params.size(), true);
    if (c.res) {
      if (auto* rc = std::get_if<Ptyp_constr>(&(*c.res)->desc))
        for (std::size_t i = 0; i < d.params.size(); ++i) {
          if (i < rc->args.size() &&
              std::holds_alternative<Ptyp_var>(rc->args[i]->desc))
            cslot[std::get<Ptyp_var>(rc->args[i]->desc).name] = i;
          else
            is_var[i] = false;
        }
    } else {
      cslot = slot;
    }
    std::vector<int> vari(d.params.size(), 0);
    if (auto* tup = std::get_if<Pcstr_tuple>(&c.args))
      for (auto& e : tup->elems)
        variance_walk(ck, *e, vrn::POS, cslot, vari, group);
    else if (auto* r = std::get_if<Pcstr_record>(&c.args))
      for (auto& f : r->fields)
        variance_walk(ck, *f.type,
                      f.mut == MutableFlag::Mutable ? vrn::INV : vrn::POS,
                      cslot, vari, group);
    std::vector<int> part(vari.size());
    for (std::size_t i = 0; i < vari.size(); ++i) {
      int req = i < d.param_variances.size() ? d.param_variances[i] : 7;
      bool p = (req & 1) != 0, n = (req & 2) != 0;
      // Private per compute_variance_gadt: required (p,n) applied.
      part[i] = vari[i] | vrn::make(p, n, true);
      if (!is_var[i])
        part[i] |= p ? (n ? vrn::INV : vrn::POS) : vrn::conjugate(vrn::POS);
    }
    add_part(part);
  }
  (void)dpriv_proto;
  if (!any_part) return {};
  for (auto& v : acc) v = vrn::strengthen(v);
  return acc;
}

static cmi::cmiw::Loc conv_loc(const ast::Location& l);  // defined below

static void rewrite_eff_back(const cmi::cmiw::TyPtr& t);  // defined below

// Typedecl_immediacy.compute_decl's abstract-with-manifest arm: the manifest's
// HEAD constructor decl's type_immediate, verbatim and WITHOUT expansion
// (Ctype.immediacy), so `type label = int` is Always -- consumers reading the
// written flag back (kind_str's cmi_type_is_immediate) then specialize `=` on
// it to `==` exactly as ocamlc.  A closed all-constant polymorphic-variant
// manifest is Always too.  Resolution is name-based and conservative: a
// same-signature decl shadows a predef (searched newest-first), a dotted path
// reads the owning cmi (Always only), and anything unresolved -- an OPENED
// module's abbreviation, a same-rec-group forward reference -- stays Unknown,
// which can only under-specialize, never miscompile.
static int manifest_immediacy(Checker& ck, const ast::CoreType& m,
                              const std::vector<cmi::cmiw::SigItem>& out) {
  if (auto* pv = std::get_if<Ptyp_variant>(&m.desc)) {
    if (pv->closed != ClosedFlag::Closed) return 0;
    for (auto& r : pv->rows) {
      if (auto* tag = std::get_if<Rtag>(&r)) {
        if (!tag->types.empty()) return 0;
        continue;
      }
      // An INHERITED row (`[ abstract_type_constr | .. ]`, predef.mli): the
      // cited type must itself be a polyvariant, and a polyvariant is Always
      // exactly when closed and all-constant -- so its flag answers for its
      // rows, and recursing treats the citation like a manifest head.
      if (manifest_immediacy(ck, *std::get<Rinherit>(r).ct, out) != 1) return 0;
    }
    return 1;
  }
  auto* c = std::get_if<Ptyp_constr>(&m.desc);
  if (!c) return 0;  // args are irrelevant: the HEAD decl's flag decides
  if (auto* l = std::get_if<Lident>(&c->id.txt.v)) {
    for (auto it = out.rbegin(); it != out.rend(); ++it)
      if (it->k == cmi::cmiw::SigItem::Type && it->name == l->name)
        return it->type_immediate == 1 ? 1 : 0;
    if (l->name == "int" || l->name == "char" || l->name == "bool" ||
        l->name == "unit")
      return 1;
    return ck.immediate_types_.count(l->name) ? 1 : 0;
  }
  if (!std::holds_alternative<Ldot>(c->id.txt.v)) return 0;
  std::string dotted = lid_full(c->id.txt);
  if (ck.bound_module_names_.count(dotted.substr(0, dotted.find('.'))))
    return 0;
  return ck.cmi_type_is_immediate(dotted) ? 1 : 0;
}

// Convert a run of `type ... and ...` declarations (shared by structure and
// signature emission -- both hold a std::vector<TypeDeclaration>) into SigItems.
static void emit_type_decls(Checker& ck, const std::vector<TypeDeclaration>& decls,
                            std::vector<cmi::cmiw::SigItem>& out,
                            bool nonrec_ = false) {
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
    // The attribute as WRITTEN -- type_attributes emission keys on this, the
    // derived immediacy below must not print `[@@immediate]` back.
    int immed_attr = immed;
    // Typedecl_immediacy.compute_decl: a non-unboxed variant whose ctors ALL
    // have empty Cstr_tuple args is Always -- no-arg GADT ctors and the empty
    // variant included, an inline-record ctor excluded.  (The unboxed
    // derivation is not modelled; it stays at the attribute value, i.e.
    // possibly Unknown where ocamlc computes Always.)
    if (auto* var = std::get_if<Ptype_variant>(&d.kind); var && !unboxed) {
      bool all_const = true;
      for (auto& c : var->ctors) {
        auto* tup = std::get_if<Pcstr_tuple>(&c.args);
        if (!tup || !tup->elems.empty()) { all_const = false; break; }
      }
      if (all_const) immed = 1;
    }
    // Abstract-with-manifest (`type label = int`): the manifest head's flag.
    if (immed == 0 && d.manifest &&
        std::holds_alternative<Ptype_abstract>(d.kind))
      immed = manifest_immediacy(ck, **d.manifest, out);
    std::unordered_map<std::string, TypePtr> tvars;        // param name -> engine var
    std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;  // shared across params+manifest
    BridgeCtx dctx;  // ONE bridge context per decl: a non-var node cited from
                     // two slots (constrained param row in a label) emits once
    // bvars keys are raw Type*: every bridged root must stay alive for the
    // whole decl, else a freed Ptyp_any fresh var's address gets reused and
    // two `_` params collide into ONE emitted node (`type (_, _) t` printed
    // back as `(_, 'a) t constraint 'a = _`).
    std::vector<TypePtr> keep;
    auto fc = [&](const ast::CoreType& c) -> const TypePtr& {
      keep.push_back(ck.from_coretype(c, tvars)); return keep.back();
    };
    // Build every param's engine type FIRST, then apply the decl-level
    // `constraint t1 = t2` clauses under the same param scope: a constrained
    // param var BECOMES the constraint type (ocamlc stores the row/package in
    // type_params; Printtyp prints a fresh var back plus the `constraint 'a =
    // ..` clause).  Bridging afterwards shares the constrained node into the
    // kind/manifest occurrences.  Best-effort: a clash leaves the sides as-is.
    std::vector<TypePtr> eparams;
    for (auto& p : d.params) eparams.push_back(fc(*p));
    for (auto& con : d.constraints) {
      TypePtr lhs = fc(*con.t1);  // sequenced: fc's keep.push_back can
      TypePtr rhs = fc(*con.t2);  // reallocate and dangle a prior fc ref
      // A functor-param type in constraint position (`'b B.t`) solves its
      // decl's own constraints and, when phantom, EXPANDS to the arg --
      // ocamlc stores `constraint 'b = 'a`, not the folded B.t (pr4775).
      if (TypePtr ex = ck.solve_param_sig_constraints(lhs)) lhs = ex;
      if (TypePtr ex = ck.solve_param_sig_constraints(rhs)) rhs = ex;
      ck.soft_unify(lhs, rhs);
    }
    // An ABSTRACT decl's manifest converts to the ENGINE before the params
    // bridge: an `as 'a` alias to a param inside it (`type 'a t = <..
    // [< `A of & amp ] as 'a ..>` -- Entities) unifies the param with the
    // row, and the bridge then shares the one node.
    TypePtr eman = nullptr;
    if (d.manifest && !std::holds_alternative<Ptype_variant>(d.kind) &&
        !std::holds_alternative<Ptype_record>(d.kind)) {
      eman = fc(**d.manifest);
      // A functor-param type as the manifest (`= 'a A.t`) keeps the fold but
      // still solves its decl's constraints (pinning 'a to `[> ]`, pr4775).
      ck.solve_param_sig_constraints(eman);
    }
    std::vector<cmi::cmiw::TyPtr> params;
    for (size_t pi = 0; pi < d.params.size(); ++pi) {
      auto& p = d.params[pi];
      auto pv = bridge_ty_named(eparams[pi], bvars, nextvar, tvars, &dctx);
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
      } else {
        // A CONSTRAINED param (unified to its bound above): register the
        // emitted node so kind/manifest citations Tlink to it and Printtyp
        // prints the param's fresh alias name (`{ v : 'a; } constraint ..`).
        dctx.nodes[I::Engine::repr(eparams[pi]).get()] = pv;
      }
      params.push_back(std::move(pv));
    }
    // A variant type: emit its constructors (Cstr_tuple args OR an inline record
    // `Ctor of {l;..}`), with the GADT return type (`Any : 'a -> any`) in cd_res.
    if (auto* var = std::get_if<Ptype_variant>(&d.kind)) {
      std::vector<cmi::cmiw::Ctor> ctors;
      for (auto& c : var->ctors) {
        cmi::cmiw::Ctor cc; cc.name = c.name.txt; cc.loc = conv_loc(c.loc);
        if (c.res) cc.res = bridge_ty_named(fc(**c.res), bvars, nextvar, tvars, &dctx);
        if (auto* tup = std::get_if<Pcstr_tuple>(&c.args))
          for (auto& a : tup->elems) cc.args.push_back(bridge_ty_named(fc(*a), bvars, nextvar, tvars, &dctx));
        else if (auto* r = std::get_if<Pcstr_record>(&c.args))
          // Inline record (Typedtree's `Texp_record of {fields; representation;
          // extended_expression}`): emit the labels so a consumer matching
          // `Ctor {l = ..}` resolves the labels via the ctor's rlabels.
          for (auto& f : r->fields) {
            cmi::cmiw::Label lab;
            lab.name = f.name.txt; lab.loc = conv_loc(f.loc);
            lab.mut = (f.mut == MutableFlag::Mutable);
        for (auto& la : f.attrs) if (la.name == "atomic" || la.name == "ocaml.atomic") lab.atomic = true;
            lab.ty = bridge_label_ty(ck, *f.type, tvars, bvars, nextvar, &dctx);
            cc.inline_record.push_back(std::move(lab));
          }
        ctors.push_back(std::move(cc));
      }
      auto si = cmi::cmiw::sig_variant(d.name.txt, std::move(params), std::move(ctors));
      if (auto ts = ck.type_stamp_.find(&d); ts != ck.type_stamp_.end())
        si.engine_stamp = ts->second;
      si.type_empty_variant = var->ctors.empty();  // `type empty = |`
      si.type_private = (d.priv == PrivateFlag::Private);
      si.type_immediate = immed; si.type_immediate_attr = immed_attr;
      si.type_unboxed = unboxed;
      // A re-exported datatype (`type s = t = A | B`) carries BOTH a manifest
      // (the `= t` equation) and the variant kind: ocamlc stores type_manifest =
      // Some (Tconstr t) alongside Type_variant.  Bridge the manifest (sharing
      // the param var-ids) so Printtyp renders the `= t =` link.
      if (d.manifest)
        si.manifest = bridge_ty_named(fc(**d.manifest), bvars, nextvar, tvars, &dctx);
      out.push_back(std::move(si));
      continue;
    }
    if (auto* rec = std::get_if<Ptype_record>(&d.kind)) {
      std::vector<cmi::cmiw::Label> labels;
      for (auto& f : rec->fields) {
        cmi::cmiw::Label lab;
        lab.name = f.name.txt; lab.loc = conv_loc(f.loc);
        lab.mut = (f.mut == MutableFlag::Mutable);
        for (auto& la : f.attrs) if (la.name == "atomic" || la.name == "ocaml.atomic") lab.atomic = true;
        lab.ty = bridge_label_ty(ck, *f.type, tvars, bvars, nextvar, &dctx);
        labels.push_back(std::move(lab));
      }
      auto si = cmi::cmiw::sig_record(d.name.txt, std::move(params), std::move(labels));
      if (auto ts = ck.type_stamp_.find(&d); ts != ck.type_stamp_.end())
        si.engine_stamp = ts->second;
      si.type_private = (d.priv == PrivateFlag::Private);
      si.type_immediate = immed; si.type_immediate_attr = immed_attr;
      si.type_unboxed = unboxed;
      // A CONSTRAINED param is stored as its bound (not a var), so Printtyp
      // prints its variance chip even on concrete decls -- carry the WRITTEN
      // annotation (`type +'a range = { .. } constraint ..` -- range_intf).
      if (!d.constraints.empty() &&
          d.param_variances.size() == si.params.size()) {
        bool annotated = false;
        for (int v : d.param_variances) if (v != 7) annotated = true;
        if (annotated)
          si.type_variances.assign(d.param_variances.begin(),
                                   d.param_variances.end());
      }
      // A re-exported record (`type s = t = { .. }`) keeps its `= t` manifest
      // alongside the record kind, just like the variant case above.
      if (d.manifest)
        si.manifest = bridge_ty_named(fc(**d.manifest), bvars, nextvar, tvars, &dctx);
      out.push_back(std::move(si));
      continue;
    }
    cmi::cmiw::TyPtr manifest = nullptr;
    if (d.manifest) {
      // With a CONSTRAINED param (non-var repr after the manifest/constraint
      // unifications above), the manifest must bridge through the ENGINE node
      // so the param sharing survives -- the AST-level pv_row_manifest would
      // build an unshared copy.
      bool constrained_param = false;
      for (auto& ep : eparams)
        if (I::Engine::repr(ep)->kind != I::Type::Kind::Var)
          constrained_param = true;
      if (!constrained_param)
        manifest = pv_row_manifest(ck, **d.manifest, tvars, bvars, nextvar);
      if (!manifest) manifest = bridge_ty_named(eman, bvars, nextvar, tvars, &dctx);
    }
    // An EXTENSIBLE re-export of the predef `eff` (`type 'a t = 'a eff = ..`,
    // i.e. Effect.t itself): the engine canonicalised the manifest `eff` to its
    // public alias Effect.t, but writing that as t's OWN manifest makes it
    // self-referential (`t = Effect.t = t`) -- a cyclic manifest the typer loops
    // on when a later `type _ Effect.t += ..` expands it.  Restore the predef
    // path so the manifest stays `eff` (like the typext arg/return rewrite).
    if (std::holds_alternative<Ptype_open>(d.kind) && manifest)
      rewrite_eff_back(manifest);
    auto si = cmi::cmiw::sig_type(d.name.txt, std::move(params), manifest);
    if (auto ts = ck.type_stamp_.find(&d); ts != ck.type_stamp_.end())
      si.engine_stamp = ts->second;
    // `type t = ..`: an extensible (Type_open) declaration, not abstract --
    // its `type t += ..` extensions cite it and ocamlc prints the `= ..`.
    si.type_open = std::holds_alternative<Ptype_open>(d.kind);
    si.type_private = (d.priv == PrivateFlag::Private);
    si.type_immediate = immed; si.type_immediate_attr = immed_attr;
    // Written variance/injectivity (`type +!'a t`) survives on ABSTRACT
    // manifest-free decls -- the only place Printtyp prints it back
    // (concrete/manifest decls carry COMPUTED variance, printed as nothing).
    if (!manifest && !si.type_open &&
        d.param_variances.size() == si.params.size()) {
      bool annotated = false;
      for (int v : d.param_variances) if (v != 7) annotated = true;
      if (annotated)
        si.type_variances.assign(d.param_variances.begin(),
                                 d.param_variances.end());
    }
    out.push_back(std::move(si));
  }
  // COMPUTED variance for concrete decls (record/variant/manifest), fixed
  // point over the recursive group -- ocamlc's Typedecl_properties iterate.
  // Abstract-without-manifest and constrained decls keep the written/default
  // behavior above (compute_decl_variance returns empty for them).
  {
    std::map<std::string, std::vector<int>> cur;
    for (auto& d : decls)
      if (!d.params.empty())
        cur[d.name.txt] = std::vector<int>(d.params.size(), 0);
    for (int iter = 0; iter < 16; ++iter) {
      bool changed = false;
      for (auto& d : decls) {
        auto v = compute_decl_variance(ck, d, cur);
        if (v.empty()) continue;
        auto& slotv = cur[d.name.txt];
        if (slotv != v) { slotv = v; changed = true; }
      }
      if (!changed) break;
    }
    for (auto& d : decls) {
      auto v = compute_decl_variance(ck, d, cur);
      if (v.empty()) continue;
      ck.computed_variances_[d.name.txt] = v;
      for (size_t i = first_new; i < out.size(); ++i)
        if (out[i].k == cmi::cmiw::SigItem::Type && out[i].name == d.name.txt) {
          out[i].type_variances.assign(v.begin(), v.end());
          break;
        }
    }
  }
  // A `type a .. and b ..` group: Trec_first on the head, Trec_next after
  // (ocamlc prints the group back with `and`).  A `type nonrec` head is
  // Trec_not -- carried as -1 (0 doubles as "unset -> Trec_first" in the
  // writer); Printtyp prints the keyword back from it.
  for (size_t i = first_new; i < out.size(); ++i)
    out[i].rec_status = (i == first_new) ? (nonrec_ ? -1 : 1) : 2;
}

// ---- include module type of M: splice M's (already-compiled) cmi signature ----
// Render a cmi type path as the writer's bare name convention (no Stdlib__).
// A FUNCTOR-APPLICATION path keeps the explicit Stdlib head: `Set.Make(T).t`
// re-emitted bare would let a sibling `module Set` CAPTURE the head
// (ident.mli's spliced Identifiable.S Set.t = Stdlib.Set.Make(T).t cited
// ident's own Set -> the oracle looped expanding the recursive manifest).
static std::string bare_cmi_path(const cmi::Path& p) {
  std::string s = cmi_path_str(p);
  bool has_app = s.find('(') != std::string::npos;
  if (s.rfind("Stdlib__", 0) == 0)
    s = has_app ? "Stdlib." + s.substr(8) : s.substr(8);
  else if (s.rfind("Stdlib.", 0) == 0 && !has_app)
    s = s.substr(7);
  return s;
}
// cmi reader type -> cmi writer type.  Best-effort: shapes the back end / arg
// matching cares about (arrows + labels, tuples, constructors, vars, and --
// when a `nodes` sharing map is supplied -- polymorphic-variant rows) are
// preserved; anything else degrades to a fresh type variable (always valid).
// `nodes` spans one item/decl conversion: a node cited twice (a constrained
// decl param in its labels, a row shared through a val scheme) converts to
// ONE writer node so the marshal sharing -- and Printtyp's `as 'a` naming --
// survives the round trip.  A nullptr entry marks in-progress conversion
// (cycle guard: a self-citing fixpoint row degrades to a var, as before).
static cmi::cmiw::TyPtr conv_cmi_ty(const cmi::TypePtr& t0,
    std::unordered_map<const cmi::TypeExpr*, int>& vars, int& nextvar,
    std::unordered_map<const cmi::TypeExpr*, cmi::cmiw::TyPtr>* nodes = nullptr) {
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
  if (t->kind == cmi::TypeExpr::Tvar || t->kind == cmi::TypeExpr::Tunivar) {
    auto it = vars.find(t.get());
    if (it != vars.end()) return cmi::cmiw::ty_var(it->second);
    int id = nextvar++; vars[t.get()] = id;
    auto r = cmi::cmiw::ty_var(id);
    if (t->name) r->var_name = *t->name;  // keep Tvar(Some "acc") -> 'acc
    return r;
  }
  if (nodes) {
    auto it = nodes->find(t.get());
    if (it != nodes->end())
      return it->second ? it->second : cmi::cmiw::ty_var(nextvar++);
    (*nodes)[t.get()] = nullptr;  // visiting marker
  }
  cmi::cmiw::TyPtr r;
  switch (t->kind) {
    case cmi::TypeExpr::Tarrow:
      r = cmi::cmiw::ty_arrow_lbl(conv_cmi_ty(t->dom, vars, nextvar, nodes),
                                  conv_cmi_ty(t->cod, vars, nextvar, nodes),
                                  t->label_kind, t->label);
      break;
    case cmi::TypeExpr::Ttuple: {
      std::vector<cmi::cmiw::TyPtr> es;
      for (auto& e : t->elems) es.push_back(conv_cmi_ty(e.second, vars, nextvar, nodes));
      r = cmi::cmiw::ty_tuple(std::move(es));
      break;
    }
    case cmi::TypeExpr::Tconstr:
    case cmi::TypeExpr::Texpand: {
      std::vector<cmi::cmiw::TyPtr> as;
      for (auto& a : t->args) as.push_back(conv_cmi_ty(a, vars, nextvar, nodes));
      std::string nm = t->path ? bare_cmi_path(*t->path) : "";
      if (nm.find('(') != std::string::npos && getenv("CONVDBG"))
        fprintf(stderr, "[CONVDBG] conv_cmi_ty app name: %s\n", nm.c_str());
      r = nm.empty() ? cmi::cmiw::ty_var(nextvar++)
                     : cmi::cmiw::ty_constr(nm, std::move(as));
      break;
    }
    case cmi::TypeExpr::Tvariant: {
      // A fully-decoded row converts faithfully (only under a `nodes` map --
      // it doubles as the cycle guard); a tags-only legacy decode degrades.
      if (!nodes || t->pv_args.size() != t->pv_tags.size() ||
          t->pv_present.size() != t->pv_tags.size()) {
        r = cmi::cmiw::ty_var(nextvar++);
        break;
      }
      bool all_present = true;
      for (char p : t->pv_present) if (!p) all_present = false;
      int rk = !t->row_closed ? 0 : (t->row_more_nil && all_present ? 2 : 1);
      std::vector<cmi::cmiw::TyPtr> targs;
      std::vector<std::string> present;
      for (std::size_t i = 0; i < t->pv_tags.size(); ++i) {
        targs.push_back(t->pv_args[i]
                            ? conv_cmi_ty(t->pv_args[i], vars, nextvar, nodes)
                            : nullptr);
        if (rk == 1 && t->pv_present[i]) present.push_back(t->pv_tags[i]);
      }
      r = cmi::cmiw::ty_variant_row(t->pv_tags, std::move(targs), rk,
                                    std::move(present));
      break;
    }
    default:
      r = cmi::cmiw::ty_var(nextvar++);
      break;
  }
  if (nodes) (*nodes)[t.get()] = r;
  return r;
}
// Per-signature-level map of type name -> the decl's own Ident stamp, stacked
// outermost..innermost while materializing a cmi signature.  Lets the
// converter RECONSTRUCT SigItem::with_scope_skip: a stored manifest that cites
// an ENCLOSING level's decl by a bare Pident (a `with type t = t` splice kept
// in the cmi as a stamped ident) degrades to a bare NAME here, and re-emission
// inside the inner sig would self-capture without the skip.
using StampScope = std::unordered_map<std::string, long long>;
static int manifest_scope_skip(const cmi::TypeDecl& td,
                               const std::vector<const StampScope*>& stack) {
  if (!td.manifest) return 0;
  const cmi::TypeExpr* m = td.manifest.get();
  while (m && (m->kind == cmi::TypeExpr::Tlink || m->kind == cmi::TypeExpr::Tsubst) &&
         m->link)
    m = m->link.get();
  if (!m || m->kind != cmi::TypeExpr::Tconstr || !m->path) return 0;
  if (m->path->kind != cmi::Path::Pident) return 0;
  const cmi::Ident& id = m->path->id;
  if (id.stamp == 0) return 0;
  // Find the level whose decl OWNS the cited stamp (innermost..outermost).
  for (int up = 0; up < (int)stack.size(); ++up) {
    auto f = stack[stack.size() - 1 - up]->find(id.name);
    if (f != stack[stack.size() - 1 - up]->end() && f->second == id.stamp)
      return up;
  }
  return 0;
}
// A source location decoded from a dependency's cmi -> the writer's Loc, with
// the foreign pos_fname carried verbatim (emit_pos prefers it over file_id).
static cmi::cmiw::Loc rloc_to_loc(const cmi::RLoc& r) {
  cmi::cmiw::Loc l;
  l.ghost = r.ghost;
  l.start = {r.l_s, r.b_s, r.c_s, 0, r.fname};
  l.end = {r.l_e, r.b_e, r.c_e, 0, r.fname};
  return l;
}
static cmi::cmiw::SigItem cmi_type_to_item(const cmi::TypeDecl& td);
// A Sig_typext decoded from a cmi -> the writer's item.  An inline-record
// payload (`exception Inconsistency of { unit_name : ..; .. }`) must survive
// the round trip: a spliced signature that drops it re-exports the exception as
// NULLARY, so a consumer matching `M.E { .. }` reads fields off a payload-less
// block (persistent_env's Consistbl -> segfault in the bootstrapped compiler).
static cmi::cmiw::SigItem cmi_typext_to_item(const cmi::ExtConstructor& x) {
  std::unordered_map<const cmi::TypeExpr*, int> vars; int nv = 0;
  if (x.is_inline_record) {
    // ONE sharing map across the payload, like cmi_type_to_item's record path.
    std::unordered_map<const cmi::TypeExpr*, cmi::cmiw::TyPtr> nodes;
    std::vector<cmi::cmiw::Label> ls;
    for (auto& l : x.inline_record) {
      cmi::cmiw::Label lw{l.name, l.mutable_, false,
                          conv_cmi_ty(l.type, vars, nv, &nodes)};
      lw.loc = rloc_to_loc(l.loc);
      ls.push_back(std::move(lw));
    }
    return cmi::cmiw::sig_exception_record(x.name, std::move(ls));
  }
  std::vector<cmi::cmiw::TyPtr> args;
  for (auto& a : x.args) args.push_back(conv_cmi_ty(a, vars, nv));
  return cmi::cmiw::sig_exception(x.name, std::move(args));
}
static cmi::cmiw::SigItem cmi_module_to_item(const std::string& name,
                                             const cmi::ModuleDecl& md,
                                             const std::string& origin,
                                             const std::vector<const StampScope*>* outer_stamps = nullptr);
// `origin`: the module path the signature was READ from ("Opt", "Outcome") --
// a BARE Mty_ident modtype ref inside it (same-unit Pident) requalifies as
// origin.ref, which is how ocamlc records the spliced form (Opt.Config).
static std::vector<cmi::cmiw::SigItem> cmi_sig_to_items(const cmi::Signature& sig,
                                                        const std::string& origin = "",
                                                        const std::vector<const StampScope*>* outer_stamps = nullptr) {
  std::vector<cmi::cmiw::SigItem> out;
  // This level's type-decl stamps, stacked under the enclosing levels': lets
  // a bare-Pident manifest citing an ENCLOSING decl (a stored `with type`
  // splice) reconstruct with_scope_skip -- see manifest_scope_skip.
  StampScope own;
  for (auto& td : sig.types)
    if (td.stamp) own[td.name] = td.stamp;
  std::vector<const StampScope*> stack;
  if (outer_stamps) stack = *outer_stamps;
  stack.push_back(&own);
  auto conv_type = [&](const cmi::TypeDecl& td) {
    auto si = cmi_type_to_item(td);
    si.with_scope_skip = manifest_scope_skip(td, stack);
    return si;
  };
  // The decode-time `order` table interleaves the items exactly as declared
  // (type t / val make / type in_t -- shared.mli): follow it when present, so
  // a spliced signature prints back in source order.
  if (!sig.order.empty()) {
    for (auto& oe : sig.order) {
      switch (oe.kind) {
        case cmi::Signature::OrderEnt::Type:
          out.push_back(conv_type(sig.types.at(oe.idx)));
          break;
        case cmi::Signature::OrderEnt::Modtype: {
          auto& mt = sig.modtypes.at(oe.idx);
          if (mt.type && mt.type->kind == cmi::ModuleType::Sig && mt.type->sig) {
            out.push_back(cmi::cmiw::sig_modtype(mt.name, cmi_sig_to_items(*mt.type->sig, origin, &stack)));
            out.back().loc = rloc_to_loc(mt.loc);
          }
          break;
        }
        case cmi::Signature::OrderEnt::Value: {
          auto& v = sig.values.at(oe.idx);
          std::unordered_map<const cmi::TypeExpr*, int> vars; int nv = 0;
          std::unordered_map<const cmi::TypeExpr*, cmi::cmiw::TyPtr> nodes;
          if (!v.prim.empty()) {
            auto se = cmi::cmiw::sig_external(
                v.name, conv_cmi_ty(v.type, vars, nv, &nodes), v.prim,
                v.prim_native);
            se.prim_alloc = v.prim_alloc;
            se.prim_reprs = v.prim_reprs;
            se.prim_repr_res = v.prim_repr_res;
            out.push_back(std::move(se));
          } else
            out.push_back(cmi::cmiw::sig_value(v.name, conv_cmi_ty(v.type, vars, nv, &nodes)));
          out.back().loc = rloc_to_loc(v.loc);
          break;
        }
        case cmi::Signature::OrderEnt::Module: {
          auto mit = cmi_module_to_item(sig.modules.at(oe.idx).name,
                                        sig.modules.at(oe.idx), origin, &stack);
          mit.loc = rloc_to_loc(sig.modules.at(oe.idx).loc);
          out.push_back(std::move(mit));
          break;
        }
        case cmi::Signature::OrderEnt::Typext:
          out.push_back(cmi_typext_to_item(sig.typexts.at(oe.idx)));
          break;
      }
    }
    return out;
  }
  // Types and module-types take no runtime field; emit them first.
  for (auto& td : sig.types) out.push_back(conv_type(td));
  for (auto& mt : sig.modtypes)
    if (mt.type && mt.type->kind == cmi::ModuleType::Sig && mt.type->sig) {
      out.push_back(cmi::cmiw::sig_modtype(mt.name, cmi_sig_to_items(*mt.type->sig, origin, &stack)));
      out.back().loc = rloc_to_loc(mt.loc);
    }
  // Primitive values take no field either; emit before the field-takers.
  for (auto& v : sig.values) {
    if (v.prim.empty()) continue;
    std::unordered_map<const cmi::TypeExpr*, int> vars; int nv = 0;
    std::unordered_map<const cmi::TypeExpr*, cmi::cmiw::TyPtr> nodes;
    auto se = cmi::cmiw::sig_external(
        v.name, conv_cmi_ty(v.type, vars, nv, &nodes), v.prim, v.prim_native);
    se.prim_alloc = v.prim_alloc;
    se.prim_reprs = v.prim_reprs;
    se.prim_repr_res = v.prim_repr_res;
    se.loc = rloc_to_loc(v.loc);
    out.push_back(std::move(se));
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
      std::unordered_map<const cmi::TypeExpr*, cmi::cmiw::TyPtr> nodes;
      out.push_back(cmi::cmiw::sig_value(fn, conv_cmi_ty(it->second->type, vars, nv, &nodes)));
      out.back().loc = rloc_to_loc(it->second->loc);
    } else if (auto it = mmap.find(fn); it != mmap.end()) {
      auto mit = cmi_module_to_item(fn, *it->second, origin, &stack);
      mit.loc = rloc_to_loc(it->second->loc);
      out.push_back(std::move(mit));
    } else if (auto it = xmap.find(fn); it != xmap.end()) {
      out.push_back(cmi_typext_to_item(*it->second));
    }
  }
  return out;
}
static cmi::cmiw::SigItem cmi_type_to_item(const cmi::TypeDecl& td) {
  std::unordered_map<const cmi::TypeExpr*, int> vars; int nv = 0;
  // ONE sharing map across the decl: a constrained param (stored as its
  // bound, not a var) cited from a label/manifest converts to the same
  // writer node, so Printtyp prints the `constraint 'a = ..` clause back.
  std::unordered_map<const cmi::TypeExpr*, cmi::cmiw::TyPtr> nodes;
  std::vector<cmi::cmiw::TyPtr> params;
  for (auto& p : td.params) params.push_back(conv_cmi_ty(p, vars, nv, &nodes));
  cmi::cmiw::SigItem si;
  if (td.kind == cmi::TypeDecl::Record) {
    std::vector<cmi::cmiw::Label> ls;
    for (auto& l : td.labels) {
      cmi::cmiw::Label lw{l.name, l.mutable_, false, conv_cmi_ty(l.type, vars, nv, &nodes)};
      lw.loc = rloc_to_loc(l.loc);
      ls.push_back(std::move(lw));
    }
    si = cmi::cmiw::sig_record(td.name, std::move(params), std::move(ls));
  } else if (td.kind == cmi::TypeDecl::Variant) {
    std::vector<cmi::cmiw::Ctor> cs;
    for (auto& c : td.ctors) {
      cmi::cmiw::Ctor cw;
      cw.name = c.name;
      cw.loc = rloc_to_loc(c.loc);
      for (auto& a : c.args) cw.args.push_back(conv_cmi_ty(a, vars, nv, &nodes));
      for (auto& l : c.inline_record) {
        cmi::cmiw::Label lw{l.name, l.mutable_, false, conv_cmi_ty(l.type, vars, nv, &nodes)};
        lw.loc = rloc_to_loc(l.loc);
        cw.inline_record.push_back(std::move(lw));
      }
      // GADT return (`Element : 'a lr1state * .. -> element`): dropping cd_res
      // here turned the ctor into a PLAIN one, so a spliced functor-result sig
      // no longer matched the engine's own decl (parser.mli's MenhirInterpreter
      // include -- oracle: "is not included in").
      if (c.res) cw.res = conv_cmi_ty(c.res, vars, nv, &nodes);
      cs.push_back(std::move(cw));
    }
    si = cmi::cmiw::sig_variant(td.name, std::move(params), std::move(cs));
  } else {
    cmi::cmiw::TyPtr man = td.manifest ? conv_cmi_ty(td.manifest, vars, nv, &nodes) : nullptr;
    si = cmi::cmiw::sig_type(td.name, std::move(params), man);
  }
  // A re-exported datatype (List's `type 'a t = 'a list = [] | ..`) keeps its
  // manifest ALONGSIDE the kind (Printtyp renders the `= 'a list =` link).
  if (!si.manifest && td.manifest &&
      (td.kind == cmi::TypeDecl::Record || td.kind == cmi::TypeDecl::Variant))
    si.manifest = conv_cmi_ty(td.manifest, vars, nv, &nodes);
  si.type_private = td.priv;
  si.type_immediate = td.immediate;
  si.type_variances = td.variances;
  si.loc = rloc_to_loc(td.loc);
  return si;
}
static cmi::cmiw::SigItem cmi_module_to_item(const std::string& name,
                                             const cmi::ModuleDecl& md,
                                             const std::string& origin,
                                             const std::vector<const StampScope*>* outer_stamps) {
  if (md.type && md.type->kind == cmi::ModuleType::Sig && md.type->sig)
    return cmi::cmiw::sig_module(name, cmi_sig_to_items(*md.type->sig, origin, outer_stamps));
  if (md.type && md.type->kind == cmi::ModuleType::Alias && md.type->path)
    return cmi::cmiw::sig_module_alias(name, bare_cmi_path(*md.type->path));
  if (md.type && md.type->kind == cmi::ModuleType::Ident && md.type->path) {
    // `module Config : Opt.Config` -- a NAMED modtype reference; keep the
    // Mty_ident (the writer's modtype_path resolves it), not an empty sig.
    auto si = cmi::cmiw::sig_module(name, {});
    si.modtype_ref = bare_cmi_path(*md.type->path);
    if (!origin.empty() && si.modtype_ref.find('.') == std::string::npos)
      si.modtype_ref = origin + "." + si.modtype_ref;
    return si;
  }
  if (md.type && md.type->kind == cmi::ModuleType::Functor) {
    // A FUNCTOR member (Hashtbl's Make): rebuild the curried parameter chain
    // and result so a spliced signature round-trips it (previously an opaque
    // `sig end` -- same field count, but the printed sig lost the functor).
    // A bare Mty_ident param ref ("HashedType") stays bare: the sibling
    // modtype item travels in the same spliced signature.
    struct P {
      std::string name, ref;
      std::vector<cmi::cmiw::SigItem> sig;
      bool unit = false;
    };
    std::vector<P> ps;
    const cmi::ModuleType* cur = md.type.get();
    while (cur && cur->kind == cmi::ModuleType::Functor) {
      P p;
      p.unit = cur->functor_unit;
      if (cur->functor_param) p.name = *cur->functor_param;
      if (const cmi::ModuleType* pt = cur->functor_param_type.get()) {
        if (pt->kind == cmi::ModuleType::Sig && pt->sig)
          p.sig = cmi_sig_to_items(*pt->sig, origin);
        else if (pt->kind == cmi::ModuleType::Ident && pt->path)
          p.ref = bare_cmi_path(*pt->path);
      }
      ps.push_back(std::move(p));
      cur = cur->functor_body.get();
    }
    std::vector<cmi::cmiw::SigItem> result;
    std::string result_ref;
    if (cur) {
      if (cur->kind == cmi::ModuleType::Sig && cur->sig)
        result = cmi_sig_to_items(*cur->sig, origin);
      else if (cur->kind == cmi::ModuleType::Ident && cur->path)
        result_ref = bare_cmi_path(*cur->path);
    }
    auto item = cmi::cmiw::sig_module_functor(name, ps[0].name,
                                              std::move(ps[0].sig),
                                              std::move(result));
    item.functor_unit = ps[0].unit;
    item.functor_param_ref = std::move(ps[0].ref);
    item.functor_result_ref = std::move(result_ref);
    for (std::size_t i = 1; i < ps.size(); ++i) {
      item.more_param_names.push_back(std::move(ps[i].name));
      item.more_param_sigs.push_back(std::move(ps[i].sig));
      item.more_param_units.push_back(ps[i].unit ? 1 : 0);
      item.more_param_refs.push_back(std::move(ps[i].ref));
    }
    return item;
  }
  return cmi::cmiw::sig_module(name, {});  // opaque, but keeps the field
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

// The most recent external in `out` named `name` (reverse scan = shadowing).
static const cmi::cmiw::SigItem* find_prim_alias_target(
    const std::vector<cmi::cmiw::SigItem>& out, const std::string& name) {
  for (auto it = out.rbegin(); it != out.rend(); ++it)
    if (it->k == cmi::cmiw::SigItem::Value && it->name == name &&
        (it->prim_external || !it->prim.empty()))
      return &*it;
  return nullptr;
}

// `external f [: t] = g` (Pprim_alias): ocamlc copies g's ENTIRE primitive
// description -- prim name(s), alloc + native reprs -- and keeps f's explicit
// type when given, else g's.  The decl's own [@@noalloc]/[@unboxed] attrs are
// IGNORED (they trigger warning 53), so we do NOT run apply_prim_attrs here.
// Returns nullopt when the target isn't a resolvable local external (dotted
// aliases aren't supported), so the item is left dropped as before.
static std::optional<cmi::cmiw::SigItem> emit_prim_alias(
    Checker& ck, const ast::PrimitiveDescription& pd,
    const std::vector<cmi::cmiw::SigItem>& out) {
  if (!pd.alias || pd.alias->txt.find('.') != std::string::npos) return std::nullopt;
  const cmi::cmiw::SigItem* tgt = find_prim_alias_target(out, pd.alias->txt);
  if (!tgt) return std::nullopt;
  cmi::cmiw::TyPtr ty;
  if (pd.type) {
    std::unordered_map<std::string, TypePtr> tvars;
    std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
    ty = bridge_ty_named(ck.from_coretype(*pd.type, tvars), bvars, nextvar, tvars);
  } else {
    ty = tgt->ty;  // no annotation: inherit the aliased primitive's type
  }
  cmi::cmiw::SigItem item =
      cmi::cmiw::sig_external(pd.name.txt, ty, tgt->prim, tgt->prim_native);
  item.prim_alloc = tgt->prim_alloc;
  item.prim_reprs = tgt->prim_reprs;
  item.prim_repr_res = tgt->prim_repr_res;
  return item;
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
        for (auto& la : f.attrs) if (la.name == "atomic" || la.name == "ocaml.atomic") lab.atomic = true;
      lab.ty = bridge_label_ty(ck, *f.type, tvars, bvars, nextvar);
      labels.push_back(std::move(lab));
    }
    item = cmi::cmiw::sig_exception_record(name, std::move(labels));
  } else {
    std::vector<cmi::cmiw::TyPtr> args;
    if (auto* tup = std::get_if<Pcstr_tuple>(&pd.args))
      for (auto& a : tup->elems)
        args.push_back(bridge_ty_named(ck.from_coretype(*a, tvars), bvars, nextvar, tvars));
    item = cmi::cmiw::sig_exception(name, std::move(args));
  }
  if (pd.res)
    item.ext_ret = bridge_ty_named(ck.from_coretype(**pd.res, tvars), bvars, nextvar, tvars);
  if (ext) {
    item.ext_path = typext_path(ck, ext->path.txt);
    item.ext_params = typext_param_names(*ext);
    item.text_kind = first ? 0 : 1;  // Text_first / Text_next
    // `type exn += private In_context of error` (env.mli): ext_private.
    item.type_private = ext->priv == PrivateFlag::Private;
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
// Apply the `with type ...` refinements of a Pmty_with chain onto resolved
// signature items (body_sig / qual_modtype_items strip the chain, but ocamlc
// records the CONSTRAINED decl: `S with type in_t = T0.t` stores in_t's
// manifest).  `with type t := ..` (destructive) erases the decl instead --
// types take no runtime field, so erasure can't shift the value layout.
// The ENCLOSING structure's already-emitted items (set by infer_signature
// around its emission loop; restored on return).  Lets a SIGNATURE body
// resolve `module type of Foo` where Foo is a local struct module (t02's
// `module type Gee = sig module M : module type of Foo .. end`).
static const std::vector<cmi::cmiw::SigItem>* g_enclosing_struct_items = nullptr;

// Enclosing-scope `open M` module paths active where a submodule is emitted, so
// its re-inference (a fresh Checker) sees the outer file's opens.  Set around
// the submodule's infer_signature call; restored so recursion nests correctly.
static const std::vector<const ast::Longident*>* g_inherited_opens = nullptr;

// `module type of <path>` resolution (Typemod.type_module_type_of +
// Mtype.remove_aliases): the path's DECLARED module type with aliases
// expanded.  The result is strengthened at the NORMALIZED path exactly when
// resolution passed through an alias binding -- a plain local module or a
// direct -I unit stays abstract, while `module Alias = M`, an aliased member
// of an opened module, or a stdlib unit (reached through the `Stdlib.X`
// alias member) all strengthen.
struct TypeofResolved {
  std::vector<cmi::cmiw::SigItem> items;
  std::string norm;           // normalized path (strengthening base)
  bool through_alias = false; // resolution crossed an alias -> strengthen
  bool ok = false;
};
static TypeofResolved resolve_typeof_path(
    const std::vector<cmi::cmiw::SigItem>* scope, const std::string& dotted,
    int depth = 0);

static void rewrite_item_ty_names(std::vector<cmi::cmiw::SigItem>& items,
                                  const std::function<void(std::string&)>& fn);
static void rewrite_item_ty_nodes(std::vector<cmi::cmiw::SigItem>& items,
                                  const std::function<void(cmi::cmiw::TyPtr&)>& fn);
static void subst_type_citations(std::vector<cmi::cmiw::SigItem>& items,
                                 const std::string& name,
                                 const std::string& newname, int d,
                                 bool shadowed);
static void qualify_enclosing_types(std::vector<cmi::cmiw::SigItem>& result,
                                    const cmi::Signature& outer_sig,
                                    const std::string& unit_path,
                                    const std::set<std::string>* unit_submods = nullptr,
                                    const std::string& unit_name = "");
static void annot_modtype_items(Checker* ckp, const ast::ModuleType& mt,
                                std::string& ref,
                                std::vector<cmi::cmiw::SigItem>& sig);
static std::vector<std::string> split_dotted(const std::string& s);
static std::vector<cmi::cmiw::SigItem> cmi_modtype_items(
    const std::vector<std::string>& comps);
static void strengthen_abstract(std::vector<cmi::cmiw::SigItem>& items,
                                const std::string& app,
                                bool aliasable = false);
static std::optional<cmi::cmiw::SigItem> module_binding_sigitem(
    const std::string& name, const ast::ModuleExpr& me,
    const std::vector<cmi::cmiw::SigItem>* prior, Checker* ckp);
// depth_bias: how many scope levels the `items` vector itself sits BELOW the
// scope where the constraint was written -- 1 when they become a module's sub
// (`module Map : Map.S with type ..`), 0 when spliced flat by an include.
static void apply_with_constraints(Checker& ck, const ast::ModuleType& mt,
                                   std::vector<cmi::cmiw::SigItem>& items,
                                   int depth_bias = 1) {
  const ast::ModuleType* m = &mt;
  while (auto* pw = std::get_if<Pmty_with>(&m->desc)) {
    for (auto& c : pw->constraints) {
      const ast::TypeDeclaration* td = nullptr;
      const LongidentLoc* lid = nullptr;
      bool subst = false;
      if (auto* wt = std::get_if<Pwith_type>(&c)) { td = &*wt->td; lid = &wt->lid; }
      else if (auto* ws = std::get_if<Pwith_typesubst>(&c)) {
        td = &*ws->td; lid = &ws->lid; subst = true;
      } else if (auto* wm = std::get_if<Pwith_modsubst>(&c)) {
        // `with module X := Y` substitutes Y for X everywhere: references
        // `X.t` in the remaining items become `Y.t` (the X item itself is
        // dropped by the caller via drop_modsubst).
        std::string from = lid_full(wm->lid1.txt) + ".";
        std::string to = lid_full(wm->lid2.txt) + ".";
        rewrite_item_ty_names(items, [&](std::string& n) {
          if (n.rfind(from, 0) == 0) n = to + n.substr(from.size());
        });
        continue;
      } else if (auto* wmo = std::get_if<Pwith_module>(&c)) {
        // `with module X = Y`: ocamlc replaces X's decl with Y's signature
        // strengthened at Y (`module Endpoint : sig type t = Endpoint.t
        // end`).  Best effort: materialize X's OWN sig (a named cross-unit
        // ref resolves through its cmi) and strengthen its abstract types at
        // Y -- equivalent whenever Y was ascribed that same modtype, the
        // common source shape (range.ml's `with module Endpoint = Endpoint`).
        std::vector<std::string> mcomps = split_dotted(lid_full(wmo->lid1.txt));
        std::vector<cmi::cmiw::SigItem>* mcur = &items;
        cmi::cmiw::SigItem* mitem = nullptr;
        for (std::size_t i = 0; i < mcomps.size() && mcur; ++i) {
          mitem = nullptr;
          for (auto& si : *mcur)
            if (si.k == cmi::cmiw::SigItem::Module && si.name == mcomps[i]) {
              mitem = &si; break;
            }
          mcur = mitem ? &mitem->sub : nullptr;
        }
        // When Y is a LOCAL module of the enclosing structure (`with module
        // M = M` where a sibling `module M = struct type t = int * .. end`
        // exists), ocamlc takes Y's OWN signature -- carrying Y's manifests --
        // strengthened at Y, not X's abstract face strengthened.  Seeding
        // mitem->sub from Y keeps `type t = int * (<m:'a> as 'a)` instead of
        // degrading to `type t = M.t` (pr6371).
        if (mitem && g_enclosing_struct_items) {
          std::vector<std::string> ycomps = split_dotted(lid_full(wmo->lid2.txt));
          const std::vector<cmi::cmiw::SigItem>* ycur = g_enclosing_struct_items;
          const cmi::cmiw::SigItem* yitem = nullptr;
          for (std::size_t i = 0; i < ycomps.size() && ycur; ++i) {
            yitem = nullptr;
            for (auto& si : *ycur)
              if (si.k == cmi::cmiw::SigItem::Module && si.name == ycomps[i]) {
                yitem = &si; break;
              }
            ycur = yitem ? &yitem->sub : nullptr;
          }
          if (yitem && !yitem->sub.empty()) {
            mitem->sub = yitem->sub;
            mitem->modtype_ref.clear();
            strengthen_abstract(mitem->sub, lid_full(wmo->lid2.txt));
            continue;
          }
        }
        if (mitem) {
          if (mitem->sub.empty() && !mitem->modtype_ref.empty() &&
              mitem->modtype_ref.find('.') != std::string::npos) {
            auto mats = cmi_modtype_items(split_dotted(mitem->modtype_ref));
            if (!mats.empty()) mitem->sub = std::move(mats);
          }
          if (!mitem->sub.empty()) {
            mitem->modtype_ref.clear();
            strengthen_abstract(mitem->sub, lid_full(wmo->lid2.txt));
          }
        }
        continue;
      } else if (auto* wmt = std::get_if<Pwith_modtype>(&c)) {
        // `with module type MT = AS`: replace the abstract `module type MT`
        // decl with a manifest.  A named RHS (`= AS`, `= M.T`) sets the
        // modtype_ref so the writer emits Some(Mty_ident) (shape-index's MSA);
        // an inline `sig .. end` RHS is left best-effort (rare).
        std::vector<std::string> ncomps = split_dotted(lid_full(wmt->lid.txt));
        std::vector<cmi::cmiw::SigItem>* ncur = &items;
        cmi::cmiw::SigItem* nitem = nullptr;
        for (std::size_t i = 0; i < ncomps.size() && ncur; ++i) {
          nitem = nullptr;
          for (auto& si : *ncur)
            if ((i + 1 < ncomps.size() ? si.k == cmi::cmiw::SigItem::Module
                                       : si.k == cmi::cmiw::SigItem::Modtype) &&
                si.name == ncomps[i]) { nitem = &si; break; }
          ncur = nitem ? &nitem->sub : nullptr;
        }
        if (nitem)
          if (auto* pid = std::get_if<Pmty_ident>(&wmt->mty->desc);
              pid && !std::holds_alternative<Lapply>(pid->id.txt.v)) {
            nitem->modtype_abstract = false;
            nitem->modtype_ref = lid_full(pid->id.txt);
          }
        continue;
      } else continue;
      // Descend a dotted `with type M.t = ..` through submodule items.
      std::string full = lid_full(lid->txt);
      std::vector<std::string> comps;
      for (std::size_t p = 0, d; p <= full.size(); p = d + 1) {
        d = full.find('.', p);
        if (d == std::string::npos) d = full.size();
        comps.push_back(full.substr(p, d - p));
      }
      std::vector<cmi::cmiw::SigItem>* cur = &items;
      for (std::size_t i = 0; i + 1 < comps.size() && cur; ++i) {
        std::vector<cmi::cmiw::SigItem>* next = nullptr;
        for (auto& si : *cur)
          if (si.k == cmi::cmiw::SigItem::Module && si.name == comps[i]) {
            // A submodule held as a NAMED modtype ref (`module Endpoint :
            // Range_intf.Endpoint_intf`): the refinement forces it inline --
            // ocamlc stores `sig type t = Endpoint.t end` -- so materialize
            // the referenced modtype's items and drop the ref.
            if (si.sub.empty() && !si.modtype_ref.empty() &&
                si.modtype_ref.find('.') != std::string::npos) {
              auto mats = cmi_modtype_items(split_dotted(si.modtype_ref));
              if (!mats.empty()) { si.sub = std::move(mats); si.modtype_ref.clear(); }
            }
            next = &si.sub; break;
          }
        cur = next;
      }
      if (!cur) continue;
      auto tgt = std::find_if(cur->begin(), cur->end(), [&](const cmi::cmiw::SigItem& si) {
        return si.k == cmi::cmiw::SigItem::Type && si.name == comps.back();
      });
      if (tgt == cur->end()) continue;
      if (subst) {
        cur->erase(tgt);
        // `type t := u` (destructive): besides erasing `type t`, substitute
        // t -> u in the remaining items, so a later `val x : t` becomes
        // `val x : u` instead of degrading to a fresh var (mirrors the
        // Pwith_modsubst rewrite above; shape-index's MSB include).  Only a
        // path (Constr) RHS -- the `t := u` / `t := M.t` shape.
        if (td->manifest) {
          if (auto* pc = std::get_if<Ptyp_constr>(&(*td->manifest)->desc)) {
            std::string newname = lid_full(pc->id.txt);
            // A BARE RHS written in the constraint's outer scope resolves
            // there (diffing.mli's `Parameters with type update_result :=
            // state` under `open D` means D.state): prefer the checker's
            // resolution when it only QUALIFIES the written name -- an
            // unresolvable bare citation degrades to a fresh var in the
            // emitted items.
            if (std::holds_alternative<Lident>(pc->id.txt.v)) {
              std::unordered_map<std::string, TypePtr> tv2;
              TypePtr r = I::Engine::repr(ck.from_coretype(**td->manifest, tv2));
              if (r->kind == I::Type::Kind::Constr &&
                  r->path.size() > newname.size() &&
                  r->path.compare(r->path.size() - newname.size() - 1,
                                  newname.size() + 1, "." + newname) == 0)
                newname = r->path;
            }
            if (full.find('.') == std::string::npos)
              // Bare erased name: referent-aware (a SHADOWING inner decl's
              // own citations stay; a skip-spliced manifest reaching the
              // subst level is rewritten) -- see subst_type_citations.
              subst_type_citations(items, full, newname, 0, false);
            else
              rewrite_item_ty_names(items, [&](std::string& n) {
                if (n == full) n = newname;
              });
          } else if (td->params.empty() &&
                     full.find('.') == std::string::npos) {
            // A NON-path RHS (`update_result := state * left array`):
            // replace each citing Constr NODE with a fresh bridge of the
            // manifest (a name rewrite can't represent a tuple).  Only for
            // an unparameterized bare subst, and only when the bridged tree
            // is var-free (a leaked var would collide with the item's own
            // numbering).
            std::function<bool(const cmi::cmiw::TyPtr&)> has_var =
                [&](const cmi::cmiw::TyPtr& t) -> bool {
              if (!t) return false;
              if (t->k == cmi::cmiw::Ty::Var) return true;
              for (auto& a : t->args) if (has_var(a)) return true;
              return false;
            };
            rewrite_item_ty_nodes(items, [&](cmi::cmiw::TyPtr& t) {
              if (!t || t->k != cmi::cmiw::Ty::Constr || t->name != full ||
                  !t->args.empty())
                return;
              std::unordered_map<std::string, TypePtr> tv2;
              std::unordered_map<const I::Type*, int> bv2;
              int nv2 = 0;
              auto rep = bridge_ty_named(
                  ck.from_coretype(**td->manifest, tv2), bv2, nv2, tv2);
              if (rep && !has_var(rep)) t = rep;
            });
          }
        }
        continue;
      }
      if (!td->manifest) continue;
      // Params first, then the manifest, sharing the var table -- so `'a` in
      // `with type 'a t = 'a list` is the same Var node in both.
      std::unordered_map<std::string, TypePtr> tvars;
      std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
      std::vector<cmi::cmiw::TyPtr> params;
      for (auto& p : td->params) {
        auto pv = bridge_ty_named(ck.from_coretype(*p, tvars), bvars, nextvar, tvars);
        if (pv->k == cmi::cmiw::Ty::Var)
          if (auto* v = std::get_if<Ptyp_var>(&p->desc)) pv->var_name = v->name;
        params.push_back(std::move(pv));
      }
      if (params.size() != tgt->params.size()) continue;  // arity mismatch: keep as-is
      tgt->params = std::move(params);
      // `with type a = private [> `A]`: a polyvariant manifest keeps its row
      // (the generic bridge degrades an open/upper row to a bare var).
      tgt->manifest = pv_row_manifest(ck, **td->manifest, tvars, bvars, nextvar);
      if (!tgt->manifest)
        tgt->manifest = bridge_ty_named(ck.from_coretype(**td->manifest, tvars),
                                        bvars, nextvar, tvars);
      tgt->type_private = (td->priv == PrivateFlag::Private);
      // The refined type is re-declared AT the `with type X = ..` clause, so its
      // location becomes that clause's (OCaml: `Map.key` in `Map : Map.S with
      // type key = t` carries the local `type key = t` loc, not map.mli's).
      tgt->loc = conv_loc(td->loc);
      // The RHS was written OUTSIDE the refined signature: bare names in the
      // manifest must not be captured by the refined sig's own decls (`Map.S
      // with type key = t` cites the enclosing t, not Map.S's t).  The item
      // sits comps.size()-1 levels below `items` plus the caller's bias.
      tgt->with_scope_skip = (int)comps.size() - 1 + depth_bias;
    }
    m = pw->mt.get();
  }
}

// ast::Location -> the cmi writer's Loc (file_id resolved to pos_fname later, in
// write_cmi via its filename table).
static cmi::cmiw::Loc conv_loc(const ast::Location& l) {
  cmi::cmiw::Loc r;
  r.ghost = l.ghost;
  r.start = {l.start.lnum, l.start.bol, l.start.cnum, l.start.file_id};
  r.end = {l.end.lnum, l.end.bol, l.end.cnum, l.end.file_id};
  return r;
}

// A `with` refinement that carries a DESTRUCTIVE substitution (`:=`) forces
// OCaml's Subst.signature over the ascribed module type, which relocates every
// resulting declaration to the ascription's location (printtyp.mli's
// `module Doc : Printers with type 'a printer := ..` -> all of Doc's members
// carry line 100's loc, not Printers' original positions).  A plain `=`
// refinement does NOT relocate (the members keep their source cmi locations).
static bool with_has_destructive_subst(const ast::ModuleType& mt) {
  auto* pw = std::get_if<Pmty_with>(&mt.desc);
  if (!pw) return false;
  for (auto& c : pw->constraints)
    if (std::holds_alternative<Pwith_typesubst>(c) ||
        std::holds_alternative<Pwith_modsubst>(c) ||
        std::holds_alternative<Pwith_modtypesubst>(c))
      return true;
  return false;
}
static void relocate_sig_locs(std::vector<cmi::cmiw::SigItem>& items,
                              const cmi::cmiw::Loc& loc);
static void relocate_sig_item(cmi::cmiw::SigItem& si, const cmi::cmiw::Loc& loc) {
  si.loc = loc;
  for (auto& c : si.ctors) {
    c.loc = loc;
    for (auto& l : c.inline_record) l.loc = loc;
  }
  for (auto& l : si.labels) l.loc = loc;
  relocate_sig_locs(si.sub, loc);
}
static void relocate_sig_locs(std::vector<cmi::cmiw::SigItem>& items,
                              const cmi::cmiw::Loc& loc) {
  for (auto& si : items) relocate_sig_item(si, loc);
}

static std::vector<cmi::cmiw::SigItem> signature_to_cmi_i(
    const ast::Signature& s,
    const std::unordered_map<std::string, const ast::Signature*>* outer,
    const std::unordered_map<std::string, const ast::Signature*>* outer_mods,
    const Checker* outer_ck) {
  Checker ck;
  ck.record_kinds_ = true;
  ck.keep_local_abbrevs_ = true;  // verbatim path: `t` stays `t`, not its manifest
  // Keep CROSS-MODULE source abbreviations as written too: a declared
  // `val to_seq : t -> float Seq.t` stores Seq.t, not its manifest expansion
  // `unit -> float Seq.node` (floatarray's module type S).
  ck.fold_abbrevs_ = true;
  ck.eng.lenient = true;  // fold_abbrevs_ requires best-effort unification
  // A nested signature (`module Lazy : sig .. end` after a top-level
  // `open Types`) sees the enclosing scope's opens: without inheriting them a
  // nested `Uid.t` was written as a bare GLOBAL `Uid` (no such unit) instead of
  // `Types.Uid.t` (subst.mli's mdl_uid, data_types.mli's cstr_uid).  Local
  // decls in the nested sig still shadow: the pre-pass below erases entries.
  if (outer_ck) {
    ck.opened_type_quals_ = outer_ck->opened_type_quals_;
    ck.opened_submod_quals_ = outer_ck->opened_submod_quals_;
    ck.opened_modtype_quals_ = outer_ck->opened_modtype_quals_;
    ck.type_substs_ = outer_ck->type_substs_;
    // SHARED decl-position qual snapshots: a sibling module's manifest
    // (sibling_type_manifest_) expands under the snapshot its own
    // sub-checker recorded.
    ck.manifest_decl_quals_ = outer_ck->manifest_decl_quals_;
  }
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
  // Row inherits citing a sibling module's type (`[ Simple.view | .. ]`)
  // expand through the sibling's AST manifest (see sibling_type_manifest_).
  ck.sibling_type_manifest_ = [&module_sigs](const std::string& m,
                                             const std::string& t)
      -> const ast::CoreType* {
    auto it2 = module_sigs.find(m);
    if (it2 == module_sigs.end()) return nullptr;
    for (auto& sit : *it2->second)
      if (auto* pt2 = std::get_if<Psig_type>(&sit.desc))
        for (auto& d2 : pt2->decls)
          if (d2.name.txt == t && d2.manifest) return d2.manifest->get();
    return nullptr;
  };
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
      for (auto& d : pt->decls) {
        ck.register_type_decl(d);
        // NOTE: the opened_type_quals_ shadow-erase for a local decl happens
        // in the EMISSION loop (decl-order-aware), not here: erasing in this
        // pre-pass made an EARLIER item's citation of the opened name degrade
        // to a fresh var (patterns.mli's Simple.view row cites `pattern` =
        // Typedtree.pattern BEFORE the local `type pattern` decl).
        ck.type_substs_.erase(d.name.txt);  // a real decl shadows a `:=`
      }
    // Same shadowing rule for a locally-declared module vs an opened (or
    // inherited) submodule qual: `module Uid : sig .. end` here must not have
    // `Uid.t` requalify through an earlier `open Types`.
    if (auto* pmd = std::get_if<Psig_module>(&it.desc))
      if (pmd->md.name.txt) ck.opened_submod_quals_.erase(*pmd->md.name.txt);
    // `type t := rhs`: register the substitution (citations expand to rhs;
    // the item itself is erased from the emitted signature).
    if (auto* pts = std::get_if<Psig_typesubst>(&it.desc))
      for (auto& d : pts->decls)
        if (d.manifest) {
          Checker::Alias al;
          for (auto& p : d.params)
            al.params.push_back(std::holds_alternative<Ptyp_var>(p->desc)
                                    ? std::get<Ptyp_var>(p->desc).name : "");
          al.manifest = d.manifest->get();
          ck.type_substs_[d.name.txt] = std::move(al);
          ck.opened_type_quals_.erase(d.name.txt);
        }
    if (auto* pmt = std::get_if<Psig_modtype>(&it.desc))
      if (pmt->type) {
        if (auto* ps = std::get_if<Pmty_signature>(&pmt->type->desc))
          modtypes[pmt->name.txt] = &ps->items;
        // `module type S2 = S1`: the alias resolves to the target's items.
        else if (auto* pid = std::get_if<Pmty_ident>(&pmt->type->desc))
          if (auto* l = std::get_if<Lident>(&pid->id.txt.v))
            if (auto f = modtypes.find(l->name); f != modtypes.end())
              modtypes[pmt->name.txt] = f->second;
      }
    // `module type S := sig .. end` (a destructive modtype SUBSTITUTION): S takes
    // no field and is erased from the output, but a later `include S with ..` must
    // still expand to its members.  printtyp.mli declares `module type Printers :=`
    // then `include Printers with type 'a printer := ..`; without registering the
    // subst, body_sig couldn't resolve the include and 22 items (ident/path/
    // type_expr/..) were dropped -> Printtyp.* read wrong fields.
    if (auto* pms = std::get_if<Psig_modtypesubst>(&it.desc))
      if (auto* ps = std::get_if<Pmty_signature>(&pms->type.desc))
        modtypes[pms->name.txt] = &ps->items;
    if (auto* po = std::get_if<Psig_open>(&it.desc)) {
      auto* l = std::get_if<Lident>(&po->id.txt.v);
      // `open A` of a LOCAL module of the enclosing structure (A emitted just
      // above): register each of A's type names `t` -> `A.t` so a val
      // annotation `val c : t` in this sig resolves to `A.t` instead of a
      // fresh var (shape-index's `module C : sig open A val c : t end`).
      if (l && g_enclosing_struct_items)
        for (auto& si : *g_enclosing_struct_items)
          if (si.k == cmi::cmiw::SigItem::Module && si.name == l->name) {
            for (auto& msi : si.sub)
              if (msi.k == cmi::cmiw::SigItem::Type)
                ck.opened_type_quals_[msi.name] = l->name + "." + msi.name;
            break;
          }
      if (l && module_sigs.count(l->name)) {
        import_modtypes_of(*module_sigs.at(l->name));
        // Its TYPE decls too: `open D` of a module known by its AST sig (a
        // functor parameter, or a local module) must qualify bare type cites
        // (`left` -> `D.left`) or every use in a following val/decl degrades
        // to a fresh var (diffing.mli's `Define(D:Defs): sig open D ..`).
        // Includes are flattened transitively: menhirLib's `open I` where
        // I : EVERYTHING = `include INCREMENTAL_ENGINE; include INSPECTION`
        // -- xsymbol/element live behind the includes.
        std::function<void(const ast::Signature&, int)> seed_types =
            [&](const ast::Signature& sg2, int depth) {
              if (depth > 6) return;
              for (auto& msi : sg2) {
                if (auto* pt2 = std::get_if<Psig_type>(&msi.desc))
                  for (auto& d2 : pt2->decls)
                    ck.opened_type_quals_[d2.name.txt] =
                        l->name + "." + d2.name.txt;
                if (auto* pi2 = std::get_if<Psig_include>(&msi.desc)) {
                  const ast::ModuleType* m2 = &pi2->mt;
                  while (auto* pw2 = std::get_if<Pmty_with>(&m2->desc))
                    m2 = pw2->mt.get();
                  if (auto* pid2 = std::get_if<Pmty_ident>(&m2->desc)) {
                    const ast::Signature* inc = nullptr;
                    if (auto* il = std::get_if<Lident>(&pid2->id.txt.v)) {
                      if (auto mf = modtypes.find(il->name);
                          mf != modtypes.end())
                        inc = mf->second;
                      // a modtype declared inside a SIBLING module (EVERYTHING
                      // includes INCREMENTAL_ENGINE, both members of
                      // IncrementalEngine -- the bare name is in scope THERE)
                      if (!inc)
                        for (auto& [mn2, msig2] : module_sigs) {
                          for (auto& mit3 : *msig2)
                            if (auto* pmt3 =
                                    std::get_if<Psig_modtype>(&mit3.desc))
                              if (pmt3->name.txt == il->name && pmt3->type)
                                if (auto* ps3 = std::get_if<Pmty_signature>(
                                        &pmt3->type->desc))
                                  inc = &ps3->items;
                          if (inc) break;
                        }
                    } else if (auto* dd2 = std::get_if<Ldot>(&pid2->id.txt.v)) {
                      if (auto* pl2 = std::get_if<Lident>(&dd2->prefix->v))
                        if (auto f2 = module_sigs.find(pl2->name);
                            f2 != module_sigs.end())
                          for (auto& mit2 : *f2->second)
                            if (auto* pmt2 =
                                    std::get_if<Psig_modtype>(&mit2.desc))
                              if (pmt2->name.txt == dd2->name && pmt2->type)
                                if (auto* ps2 = std::get_if<Pmty_signature>(
                                        &pmt2->type->desc))
                                  inc = &ps2->items;
                    }
                    if (inc) seed_types(*inc, depth + 1);
                  }
                }
              }
            };
        seed_types(*module_sigs.at(l->name), 0);
      } else if (!std::holds_alternative<Lapply>(po->id.txt.v)) {
        // `open Terms` of a separately-compiled unit (or dotted submodule):
        // its bare type names must resolve qualified (`term` -> `Terms.term`)
        // or every declared use degrades to a fresh var (misc-kb .mli files).
        ck.load_open_type_quals(po->id.txt);
        // Its submodules too: `open Types` then `Uid.t` must cite
        // `Types.Uid.t`, not a bare global `Uid` (data_types.mli).
        ck.load_open_submod_quals(po->id.txt);
      }
    }
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
    const std::string mtname = comps.back();  // by value: comps may re-root
    // The head may be a LOCAL submodule (`IncrementalEngine.INCREMENTAL_ENGINE`
    // where IncrementalEngine is a sibling, not a separate cmi): find its
    // module-type decl in the threaded module signatures and emit its items.
    if (comps.size() == 2)
      if (auto f = module_sigs.find(comps[0]); f != module_sigs.end())
        for (auto& mit : *f->second)
          if (auto* pmt = std::get_if<Psig_modtype>(&mit.desc))
            if (pmt->name.txt == mtname && pmt->type)
              if (auto* ps = std::get_if<Pmty_signature>(&pmt->type->desc)) {
                // MT's body may cite SIBLING modtypes declared in the head module
                // (`EngineTypes.TABLE`'s `module Log : LOG with ..` refers to the
                // sibling `EngineTypes.LOG`) -- pulling MT out via the dotted path
                // loses that scope, so import the head's own modtype decls before
                // recursing.  Without them body_sig can't resolve `LOG`, Log's sig
                // comes out empty, and the .cmi records an empty submodule; a
                // consumer/functor coercing to that sig then builds an empty block,
                // so `Log.state` reads field 0 of an atom -> segfault (menhir
                // MakeEngineTable's result TABLE, bootstrap bug #3).
                auto sib = modtypes;
                for (auto& hit : *f->second)
                  if (auto* hmt = std::get_if<Psig_modtype>(&hit.desc))
                    if (hmt->type)
                      if (auto* hps = std::get_if<Pmty_signature>(&hmt->type->desc))
                        sib[hmt->name.txt] = &hps->items;
                // The head module's own TOPLEVEL types are in scope inside
                // MT's body and must splice QUALIFIED: INCREMENTAL_ENGINE's
                // `val offer : .. token * position * position ..` cites the
                // enclosing IncrementalEngine's `type position = Lexing.
                // position` -- without the qual it degraded to fresh vars
                // and ocamlc rejected camlinternalMenhirLib.ml against the
                // cmi.  Seed the quals for the sub-conversion (restored
                // after; MT's own same-named decls still shadow via the
                // emission-loop erase).
                std::vector<std::pair<std::string, std::optional<std::string>>>
                    saved_q;
                auto seed = [&](const std::string& n2, const std::string& q) {
                  auto old = ck.opened_type_quals_.find(n2);
                  saved_q.emplace_back(
                      n2, old != ck.opened_type_quals_.end()
                              ? std::optional<std::string>(old->second)
                              : std::nullopt);
                  ck.opened_type_quals_[n2] = q;
                };
                for (auto& hit : *f->second) {
                  if (auto* hpt = std::get_if<Psig_type>(&hit.desc))
                    for (auto& d2 : hpt->decls)
                      seed(d2.name.txt, comps[0] + "." + d2.name.txt);
                  // an `open General` at the head module's top level is in
                  // scope inside MT too (INCREMENTAL_ENGINE's `type stack =
                  // element stream` means General.stream)
                  if (auto* hop = std::get_if<Psig_open>(&hit.desc))
                    if (auto* ol = std::get_if<Lident>(&hop->id.txt.v))
                      if (auto of2 = module_sigs.find(ol->name);
                          of2 != module_sigs.end())
                        for (auto& oit : *of2->second)
                          if (auto* opt2 = std::get_if<Psig_type>(&oit.desc))
                            for (auto& d3 : opt2->decls)
                              seed(d3.name.txt, ol->name + "." + d3.name.txt);
                }
                auto r =
                    signature_to_cmi_i(ps->items, &sib, &module_sigs, &ck);
                for (auto& [nm2, q2] : saved_q) {
                  if (q2) ck.opened_type_quals_[nm2] = *q2;
                  else ck.opened_type_quals_.erase(nm2);
                }
                return r;
              }
    // Cross-module, possibly DEEP (`CamlinternalMenhirLib.IncrementalEngine.
    // INCREMENTAL_ENGINE`): load the head cmi, navigate intermediate submodules,
    // then read the module-type's signature.  Previously only a single-component
    // prefix (`A.MT`) resolved -- a deeper prefix was a `Ldot`, not a `Lident`,
    // so the whole `include` was dropped (the parser's MenhirInterpreter lost the
    // 23 INCREMENTAL_ENGINE values -> a short module block -> a wild call at parse).
    try {
      // An intermediate component may be an ALIAS member (`Stdlib.Set.S`:
      // stdlib.cmi's Set = Stdlib__Set, Mp_absent) -- re-root the remaining
      // path at the target and walk again (ident.mli's `module Set :
      // Stdlib.Set.S with type elt = t` emitted an EMPTY sig without this,
      // and the bootstrapped ocamlc then read garbage Set fields).
      for (int guard = 0; guard < 8; ++guard) {
      const auto& cmi = cmi::CmiFile::load(head_cmi(comps[0]));
      const cmi::Signature* sig = &cmi.sig();
      bool rerooted = false;
      for (size_t i = 1; i + 1 < comps.size() && !rerooted; ++i) {
        const cmi::Signature* next = nullptr;
        for (auto& md : sig->modules)
          if (md.name == comps[i] && md.type) {
            if (md.type->kind == cmi::ModuleType::Alias && md.type->path) {
              std::string tgt = cmi_path_str(*md.type->path);
              if (!tgt.empty()) {
                auto nc = split_dotted(tgt);
                nc.insert(nc.end(), comps.begin() + i + 1, comps.end());
                comps = std::move(nc);
                rerooted = true;
              }
              break;
            }
            if (md.type->kind == cmi::ModuleType::Sig && md.type->sig)
              next = md.type->sig.get();
            break;
          }
        if (rerooted) break;
        if (!next) return {};
        sig = next;
      }
      if (rerooted) continue;
      for (auto& md : sig->modtypes)
        if (md.name == mtname && md.type &&
            md.type->kind == cmi::ModuleType::Sig && md.type->sig) {
          std::string origin;
          for (std::size_t i = 0; i + 1 < comps.size(); ++i)
            origin += (i ? "." : "") + comps[i];
          auto result = cmi_sig_to_items(*md.type->sig, origin);
          std::string unit_path = cmi.module_name();
          for (std::size_t i = 1; i + 1 < comps.size(); ++i)
            unit_path += "." + comps[i];
          // Sibling submodules of the UNIT (`General` alongside
          // IncrementalEngine) must requalify with the bare unit name, not the
          // deeper unit_path, when cited by a bare path in the spliced modtype.
          std::set<std::string> unit_submods;
          for (auto& um : cmi.sig().modules) unit_submods.insert(um.name);
          qualify_enclosing_types(result, *sig, unit_path, &unit_submods,
                                  cmi.module_name());
          return result;
        }
      return {};
      }
    } catch (...) {}
    return {};
  };
  // Build a functor SigItem (is_functor) from a `functor (P : _) -> ..` module
  // type.  Shared by a functor MODULE decl (`module Make (Ord : _) : S`) and a
  // functor MODULE TYPE decl (`module type S1 = (S0 -> S0') -> S0`); the caller
  // sets .k (Module vs Modtype) afterwards.
  std::function<cmi::cmiw::SigItem(const std::string&, const Pmty_functor&)>
      build_functor_item =
      [&](const std::string& name, const Pmty_functor& pf) -> cmi::cmiw::SigItem {
    std::string param, param_ref;
    std::vector<cmi::cmiw::SigItem> param_sig;
    if (auto* fn = std::get_if<Functor_named>(&pf.param)) {
      if (fn->name.txt) param = *fn->name.txt;
      if (fn->type) {
        if (const ast::Signature* psg = body_sig(*fn->type))
          param_sig = signature_to_cmi_i(*psg, &modtypes, &module_sigs, &ck);
        else
          param_sig = qual_modtype_items(*fn->type);
        apply_with_constraints(ck, *fn->type, param_sig);
        if (auto* pid = std::get_if<Pmty_ident>(&fn->type->desc))
          param_ref = lid_full(pid->id.txt);
      }
    }
    // A first parameter that is ITSELF a functor (`(S0 -> S0')`): carry it as a
    // param_functor Module item (the writer reuses functor-module emission).
    std::vector<cmi::cmiw::SigItem> param_functor;
    if (auto* fn = std::get_if<Functor_named>(&pf.param))
      if (fn->type)
        if (auto* pf1 = std::get_if<Pmty_functor>(&fn->type->desc)) {
          auto sub = build_functor_item("", *pf1);
          sub.k = cmi::cmiw::SigItem::Module;
          param_functor.push_back(std::move(sub));
        }
    struct P { std::string name, ref; std::vector<cmi::cmiw::SigItem> sig; bool unit = false;
               std::vector<cmi::cmiw::SigItem> pfunc; };
    std::vector<P> more;
    const ast::ModuleType* body = pf.body.get();
    while (auto* pf2 = std::get_if<Pmty_functor>(&body->desc)) {
      P p;
      p.unit = std::holds_alternative<Functor_unit>(pf2->param);
      if (auto* fn2 = std::get_if<Functor_named>(&pf2->param)) {
        if (fn2->name.txt) p.name = *fn2->name.txt;
        if (fn2->type) {
          if (const ast::Signature* psg2 = body_sig(*fn2->type))
            p.sig = signature_to_cmi_i(*psg2, &modtypes, &module_sigs, &ck);
          else
            p.sig = qual_modtype_items(*fn2->type);
          apply_with_constraints(ck, *fn2->type, p.sig);
          if (auto* pid2 = std::get_if<Pmty_ident>(&fn2->type->desc))
            p.ref = lid_full(pid2->id.txt);
        }
      }
      more.push_back(std::move(p));
      body = pf2->body.get();
    }
    // The BODY sees each NAMED param as a module (its AST sig): `open D`
    // inside the result signature then qualifies D's types (diffing.mli's
    // `module Define(D:Defs): sig open D  type nonrec change = (left,..)
    // change .. end` -- without it left/right/eq/diff degraded to fresh
    // vars and ocamlc rejected diffing.ml against the cmi).
    std::unordered_map<std::string, const ast::Signature*> body_mods =
        module_sigs;
    // A param's modtype may be a DOTTED sibling-module member
    // (`(I : IncrementalEngine.EVERYTHING)`): body_sig only resolves bare
    // names, so look the member up in the sibling's AST sig too -- the
    // body's `open I` then seeds I's types (menhirLib's Printers.Make).
    auto param_sig_ast = [&](const ast::ModuleType& mt) -> const ast::Signature* {
      if (const ast::Signature* s2 = body_sig(mt)) return s2;
      const ast::ModuleType* m2 = &mt;
      while (auto* pw2 = std::get_if<Pmty_with>(&m2->desc)) m2 = pw2->mt.get();
      if (auto* pi2 = std::get_if<Pmty_ident>(&m2->desc))
        if (auto* dd2 = std::get_if<Ldot>(&pi2->id.txt.v))
          if (auto* pl2 = std::get_if<Lident>(&dd2->prefix->v))
            if (auto f2 = module_sigs.find(pl2->name); f2 != module_sigs.end())
              for (auto& mit2 : *f2->second)
                if (auto* pmt2 = std::get_if<Psig_modtype>(&mit2.desc))
                  if (pmt2->name.txt == dd2->name && pmt2->type)
                    if (auto* ps2 =
                            std::get_if<Pmty_signature>(&pmt2->type->desc))
                      return &ps2->items;
      return nullptr;
    };
    if (auto* fn = std::get_if<Functor_named>(&pf.param))
      if (fn->name.txt && fn->type)
        if (const ast::Signature* psg = param_sig_ast(*fn->type))
          body_mods[*fn->name.txt] = psg;
    {
      const ast::ModuleType* b2 = pf.body.get();
      while (auto* pf2 = std::get_if<Pmty_functor>(&b2->desc)) {
        if (auto* fn2 = std::get_if<Functor_named>(&pf2->param))
          if (fn2->name.txt && fn2->type)
            if (const ast::Signature* psg2 = param_sig_ast(*fn2->type))
              body_mods[*fn2->name.txt] = psg2;
        b2 = pf2->body.get();
      }
    }
    std::vector<cmi::cmiw::SigItem> result;
    if (const ast::Signature* rs = body_sig(*body))
      result = signature_to_cmi_i(*rs, &modtypes, &body_mods, &ck);
    else
      result = qual_modtype_items(*body);
    apply_with_constraints(ck, *body, result);
    drop_modsubst(result, with_modsubst_names(*body));
    auto fitem = cmi::cmiw::sig_module_functor(name, param,
                    std::move(param_sig), std::move(result));
    fitem.functor_unit = std::holds_alternative<Functor_unit>(pf.param);
    fitem.functor_param_ref = std::move(param_ref);
    fitem.param_functor = std::move(param_functor);
    if (auto* rid = std::get_if<Pmty_ident>(&body->desc))
      fitem.functor_result_ref = lid_full(rid->id.txt);
    for (auto& p : more) {
      fitem.more_param_names.push_back(std::move(p.name));
      fitem.more_param_sigs.push_back(std::move(p.sig));
      fitem.more_param_units.push_back(p.unit ? 1 : 0);
      fitem.more_param_refs.push_back(std::move(p.ref));
    }
    return fitem;
  };
  std::vector<cmi::cmiw::SigItem> out;
  // `open F(X)` in this signature: the applied path, so a following nonrec
  // self-manifest can qualify through it.
  std::string sig_open_app;
  for (auto& it : s) {
    if (auto* po = std::get_if<Psig_open>(&it.desc)) {
      if (std::holds_alternative<Lapply>(po->id.txt.v))
        sig_open_app = lid_full(po->id.txt);
    }
    if (auto* pv = std::get_if<Psig_value>(&it.desc)) {
      if (!pv->vd.type) continue;
      std::unordered_map<std::string, TypePtr> tvars;
      std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
      out.push_back(cmi::cmiw::sig_value(pv->vd.name.txt,
                      bridge_ty_named(ck.from_coretype(*pv->vd.type, tvars), bvars, nextvar, tvars)));
      out.back().loc = conv_loc(pv->vd.loc);
    } else if (auto* pr = std::get_if<Psig_primitive>(&it.desc)) {
      if (pr->pd.type && !pr->pd.prims.empty()) {
        std::unordered_map<std::string, TypePtr> tvars;
        std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
        auto ty = bridge_ty_named(ck.from_coretype(*pr->pd.type, tvars), bvars, nextvar, tvars);
        std::string native = pr->pd.prims.size() > 1 ? pr->pd.prims[1] : "";
        auto item = cmi::cmiw::sig_external(pr->pd.name.txt, ty, pr->pd.prims[0], native);
        apply_prim_attrs(pr->pd, out, item);
        out.push_back(std::move(item));
        out.back().loc = conv_loc(pr->pd.loc);
      } else if (pr->pd.alias) {  // `external f [: t] = g`: copy g's primitive
        if (auto item = emit_prim_alias(ck, pr->pd, out)) {
          out.push_back(std::move(*item));
          out.back().loc = conv_loc(pr->pd.loc);
        }
      }
    } else if (auto* pt = std::get_if<Psig_type>(&it.desc)) {
      bool nonrec_ = pt->rf == RecFlag::Nonrecursive;
      std::size_t before = out.size();
      // A local decl SHADOWS a same-named type brought in by an earlier
      // `open M` (longident.mli's recursive `open Location  type t = ..
      // Ldot of t loc ..` must NOT qualify its own t to Location.t).  The
      // erase is decl-order-aware: an ORDINARY group's names shadow from
      // the group itself (recursion included), a `nonrec` group's only
      // AFTER it (its RHS still sees the open).  Earlier items citing the
      // opened name keep the qual (patterns.mli's Simple.view row cites
      // `pattern` = Typedtree.pattern BEFORE the local decl).
      if (!nonrec_)
        for (auto& d : pt->decls) ck.opened_type_quals_.erase(d.name.txt);
      emit_type_decls(ck, pt->decls, out, nonrec_);
      // emit_type_decls appends one Type SigItem per declaration, in order.
      for (std::size_t j = 0; before + j < out.size() && j < pt->decls.size(); ++j)
        out[before + j].loc = conv_loc(pt->decls[j].loc);
      if (nonrec_)
        for (auto& d : pt->decls) ck.opened_type_quals_.erase(d.name.txt);
      // Under `open F(X)` in this signature, a nonrec self-manifest
      // (`type nonrec t = t`) resolves the RHS through the OPEN, not the
      // decl itself: qualify it with the applied path (accepted_batch).
      if (nonrec_ && !sig_open_app.empty())
        for (std::size_t i = before; i < out.size(); ++i)
          if (out[i].k == cmi::cmiw::SigItem::Type && out[i].manifest &&
              out[i].manifest->k == cmi::cmiw::Ty::Constr &&
              out[i].manifest->name == out[i].name)
            out[i].manifest->name = sig_open_app + "." + out[i].manifest->name;
    } else if (auto* prm = std::get_if<Psig_recmodule>(&it.desc)) {
      // `module rec A : (FOO with type t = ..) and B : FOO` in a SIGNATURE:
      // each decl emits like Psig_module (ident kept as Mty_ident, `with`
      // expanded), marked Trec_first/Trec_next so Printtyp prints the group
      // back as one `module rec .. and ..`.
      int rs = 1;
      for (auto& md : prm->decls) {
        if (!md.name.txt || !md.type) continue;
        std::vector<cmi::cmiw::SigItem> items;
        if (const ast::Signature* bs = body_sig(*md.type))
          items = signature_to_cmi_i(*bs, &modtypes, &module_sigs, &ck);
        else
          items = qual_modtype_items(*md.type);
        apply_with_constraints(ck, *md.type, items);
        drop_modsubst(items, with_modsubst_names(*md.type));
        auto mi = cmi::cmiw::sig_module(*md.name.txt, std::move(items));
        if (auto* pid = std::get_if<Pmty_ident>(&md.type->desc))
          mi.modtype_ref = lid_full(pid->id.txt);
        mi.rec_status = rs; rs = 2;
        out.push_back(std::move(mi));
      }
    } else if (auto* pm = std::get_if<Psig_module>(&it.desc)) {
      std::size_t mbefore = out.size();
      if (pm->md.name.txt && pm->md.type) {
        if (auto* ps = std::get_if<Pmty_signature>(&pm->md.type->desc))
          out.push_back(cmi::cmiw::sig_module(*pm->md.name.txt,
                                              signature_to_cmi_i(ps->items, &modtypes, &module_sigs, &ck)));
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
          out.push_back(build_functor_item(*pm->md.name.txt, *pf));
        } else if (const ast::Signature* bs = body_sig(*pm->md.type)) {
          // `module MD5 : S` (a NAMED module type) or `S with ...`: emit the
          // submodule with S's resolved signature inline, so a consumer can
          // resolve `Digest.MD5.bytes` to its field (else the module is dropped).
          auto items = signature_to_cmi_i(*bs, &modtypes, &module_sigs, &ck);
          apply_with_constraints(ck, *pm->md.type, items);
          drop_modsubst(items, with_modsubst_names(*pm->md.type));
          auto mitem = cmi::cmiw::sig_module(*pm->md.name.txt, std::move(items));
          // A plain NAMED modtype (`module MD5 : S`, no `with`) is stored by
          // ocamlc as Mty_ident(S); the resolved sig stays the fallback.
          if (auto* pid = std::get_if<Pmty_ident>(&pm->md.type->desc))
            mitem.modtype_ref = lid_full(pid->id.txt);
          out.push_back(std::move(mitem));
        } else if (auto items = qual_modtype_items(*pm->md.type); !items.empty()) {
          // `module Set : Set.S with ..` (a qualified functor-result module type).
          apply_with_constraints(ck, *pm->md.type, items);
          drop_modsubst(items, with_modsubst_names(*pm->md.type));
          auto mitem = cmi::cmiw::sig_module(*pm->md.name.txt, std::move(items));
          // A plain DOTTED named modtype (`module M : Original.T`, no `with`)
          // stays Mty_ident like ocamlc; the resolved sig is the fallback.
          if (auto* pid = std::get_if<Pmty_ident>(&pm->md.type->desc))
            mitem.modtype_ref = lid_full(pid->id.txt);
          out.push_back(std::move(mitem));
        } else {
          // Any other module type (`module Consistbl : module type of struct ..
          // end`): emit an opaque submodule so it still TAKES A FIELD -- else the
          // surrounding value layout is short of the .cmo and every following
          // member (Persistent_env.empty) resolves to the wrong slot.
          auto mitem = cmi::cmiw::sig_module(*pm->md.name.txt, {});
          // `module M : module type of Foo` where Foo is a local struct
          // module: its already-emitted items are the signature (t02's Gee).
          if (auto* pto = std::get_if<Pmty_typeof>(&pm->md.type->desc)) {
            if (auto* pi2 = std::get_if<Pmod_ident>(&pto->me->desc))
              if (auto* l2 = std::get_if<Lident>(&pi2->id.txt.v)) {
                if (g_enclosing_struct_items)
                  for (auto& si : *g_enclosing_struct_items)
                    if (si.k == cmi::cmiw::SigItem::Module &&
                        si.name == l2->name && !si.is_functor &&
                        si.alias.empty()) {
                      mitem.sub = si.sub;
                      break;
                    }
              }
            // `module Consistbl : module type of struct include Consistbl.Make
            // (Misc.Stdlib.String) end` (persistent_env.mli): the signature is
            // the functor APPLICATION's, which module_binding_sigitem already
            // builds for the `.ml` binding form (result items from the
            // functor's cmi, parameter substitution, path strengthening).
            // Without it the submodule stayed an EMPTY sig, so the .cmo's
            // coercion built an empty block for it and a consumer reading
            // `Persistent_env.Consistbl.Inconsistency` (topeval) read past its
            // end.
            if (mitem.sub.empty()) {
              const ast::ModuleExpr* app = pto->me.get();
              if (auto* ms2 = std::get_if<Pmod_structure>(&app->desc);
                  ms2 && ms2->items.size() == 1)
                if (auto* inc2 = std::get_if<Pstr_include>(&ms2->items[0].desc))
                  app = &inc2->expr;
              if (std::holds_alternative<Pmod_apply>(app->desc) ||
                  std::holds_alternative<Pmod_apply_unit>(app->desc))
                if (auto r = module_binding_sigitem(*pm->md.name.txt, *app,
                                                    g_enclosing_struct_items,
                                                    &ck);
                    r && !r->is_functor)
                  mitem.sub = std::move(r->sub);
            }
          }
          // `module X : T` where T is a local ABSTRACT modtype (no items to
          // resolve): the Mty_ident reference is still kept (pr6651).
          if (auto* pid = std::get_if<Pmty_ident>(&pm->md.type->desc))
            mitem.modtype_ref = lid_full(pid->id.txt);
          out.push_back(std::move(mitem));
        }
      }
      if (out.size() > mbefore) {
        out.back().loc = conv_loc(it.loc);
        // `module M : S with type t := ..`: the destructive subst relocates
        // every member of M to the ascription's location.
        if (pm->md.type && with_has_destructive_subst(*pm->md.type))
          relocate_sig_locs(out.back().sub, conv_loc(pm->md.type->loc));
      }
    } else if (auto* pmt = std::get_if<Psig_modtype>(&it.desc)) {
      std::size_t mtbefore = out.size();
      // `module type S = sig .. end`: emit it so a functor parameter typed by S
      // (`Make (H : Hashtbl.HashedType)`) can resolve H's members to fields.
      if (!pmt->type)  // ABSTRACT `module type S` in a signature
        out.push_back(cmi::cmiw::sig_modtype_abstract(pmt->name.txt));
      else if (auto* ps = std::get_if<Pmty_signature>(&pmt->type->desc))
        out.push_back(cmi::cmiw::sig_modtype(pmt->name.txt,
                                             signature_to_cmi_i(ps->items, &modtypes, &module_sigs, &ck)));
      else if (auto* pf = std::get_if<Pmty_functor>(&pmt->type->desc)) {
        // `module type F = functor (X : _) -> ..` (w53's TestInlineSig): the body
        // is a functor type -- emit Mty_functor, not a signature.
        auto s = build_functor_item(pmt->name.txt, *pf);
        s.k = cmi::cmiw::SigItem::Modtype;
        out.push_back(std::move(s));
      } else if (auto* pid = std::get_if<Pmty_ident>(&pmt->type->desc)) {
        // `module type S2 = S1` / `= M.T`: an ALIAS -- mtd_type stays
        // Mty_ident like ocamlc; the resolved items are the fallback layout.
        if (!std::holds_alternative<Lapply>(pid->id.txt.v)) {
          std::vector<cmi::cmiw::SigItem> sub;
          if (const ast::Signature* bs = body_sig(*pmt->type))
            sub = signature_to_cmi_i(*bs, &modtypes, &module_sigs, &ck);
          else
            sub = qual_modtype_items(*pmt->type);
          auto s = cmi::cmiw::sig_modtype(pmt->name.txt, std::move(sub));
          s.modtype_ref = lid_full(pid->id.txt);
          out.push_back(std::move(s));
        }
      }
      if (out.size() > mtbefore) {
        out.back().loc = conv_loc(it.loc);
        if (pmt->type && with_has_destructive_subst(*pmt->type))
          relocate_sig_locs(out.back().sub, conv_loc(pmt->type->loc));
      }
    } else if (std::get_if<Psig_class>(&it.desc) ||
               std::get_if<Psig_class_type>(&it.desc)) {
      // `class c : <arrows> -> object .. end` / `class type ct = object .. end`
      // in a SIGNATURE: member types come straight from the written coretypes
      // (a class takes a runtime field, so dropping it would shift the layout).
      auto* pcd = std::get_if<Psig_class>(&it.desc);
      const std::vector<ClassTypeDeclaration>& decls =
          pcd ? pcd->decls : std::get_if<Psig_class_type>(&it.desc)->decls;
      int crs = 1;
      for (auto& d : decls) {
        if (!d.params.empty()) continue;  // ['a] not representable yet
        cmi::cmiw::SigItem ci;
        ci.k = cmi::cmiw::SigItem::Class;
        ci.class_is_type = !pcd;
        ci.name = d.name.txt;
        ci.rec_status = crs; crs = 2;
        ci.class_virtual = (d.virt == VirtualFlag::Virtual);
        std::unordered_map<const I::Type*, int> cvars; int cnext = 0;
        std::unordered_map<std::string, TypePtr> tv;
        const ast::ClassType* ct = &d.expr;
        bool ok = true;
        while (auto* pa = std::get_if<Pcty_arrow>(&ct->desc)) {  // class c : t -> ...
          auto [lk, lb] = arglabel(pa->label);
          ci.class_arrow_doms.push_back(
              bridge_ty_named(ck.from_coretype(*pa->dom, tv), cvars, cnext, tv));
          ci.class_arrow_lks.push_back(lk);
          ci.class_arrow_lbls.push_back(lb);
          ct = pa->cod.get();
        }
        auto* cs = std::get_if<Pcty_signature>(&ct->desc);
        if (!cs) continue;
        std::unordered_set<std::string> own_meths, own_vals;
        std::vector<std::string> sig_inherits;  // `inherit ct` parents to splice
        for (auto& cf : cs->cs.fields) {
          if (auto* pv = std::get_if<Pctf_val>(&cf.desc)) {
            cmi::cmiw::ClassField f;
            f.name = pv->name.txt;
            f.mut = (pv->mut == MutableFlag::Mutable);
            f.virt = (pv->virt == VirtualFlag::Virtual);
            f.ty = bridge_ty_named(ck.from_coretype(*pv->type, tv), cvars, cnext, tv);
            own_vals.insert(f.name);
            ci.class_fields.push_back(std::move(f));
          } else if (auto* pm = std::get_if<Pctf_method>(&cf.desc)) {
            cmi::cmiw::ClassField f;
            f.name = pm->name.txt;
            f.is_method = true;
            f.priv = (pm->priv == PrivateFlag::Private);
            f.virt = (pm->virt == VirtualFlag::Virtual);
            f.ty = bridge_ty_named(ck.from_coretype(*pm->type, tv), cvars, cnext, tv);
            own_meths.insert(f.name);
            ci.class_fields.push_back(std::move(f));
          } else if (auto* inh = std::get_if<Pctf_inherit>(&cf.desc)) {
            // `class dt : object inherit ct end`: splice ct's members (a local
            // same-signature class type).  Bail only if the parent isn't a plain
            // class-type name we can resolve.
            const ast::ClassType* pct2 = inh->ct.get();
            if (auto* cc = std::get_if<Pcty_constr>(&pct2->desc)) sig_inherits.push_back(lid_last(cc->id.txt));
            else { ok = false; break; }
          } else if (std::holds_alternative<Pctf_constraint>(cf.desc)) {
            ok = false;
            break;
          }
        }
        for (auto& pname : sig_inherits) {
          bool found = false;
          for (auto& pit : out) {
            if (pit.k != cmi::cmiw::SigItem::Class || pit.name != pname) continue;
            for (auto& pf : pit.class_fields) {
              auto& seen = pf.is_method ? own_meths : own_vals;
              if (!seen.insert(pf.name).second) continue;
              ci.class_fields.push_back(pf);
            }
            found = true;
            break;
          }
          if (!found) ok = false;  // unresolved parent -> don't emit a partial class
        }
        if (ok) out.push_back(std::move(ci));
      }
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
      std::size_t incbefore = out.size();
      // `include module type of M`: splice M's compiled cmi signature here so the
      // .cmi records M's values (with prim flags) and types -- otherwise the
      // included members are absent and the field layout is short of the .cmo.
      if (auto* pto = std::get_if<Pmty_typeof>(&pinc->mt.desc)) {
        // `include module type of struct include List end` (the classic
        // de-aliasing idiom, misc.mli's Stdlib.List): resolve the inner
        // include's target like a plain `module type of List`, but WITHOUT
        // strengthening -- the struct-include re-binds the types as their
        // own decls (`type 'a t = 'a list` stays the source manifest).
        const ast::Pmod_ident* pi = std::get_if<Pmod_ident>(&pto->me->desc);
        bool via_struct_include = false;
        if (!pi)
          if (auto* ms = std::get_if<Pmod_structure>(&pto->me->desc);
              ms && ms->items.size() == 1)
            if (auto* inc2 = std::get_if<Pstr_include>(&ms->items[0].desc))
              if ((pi = std::get_if<Pmod_ident>(&inc2->expr.desc)))
                via_struct_include = true;
        if (pi)
          if (!std::holds_alternative<Lapply>(pi->id.txt.v)) {
            // A LOCAL struct module shadows a compilation unit of the same
            // name: splice its already-emitted items (t02's Gee).
            bool local = false;
            if (auto* l = std::get_if<Lident>(&pi->id.txt.v);
                l && g_enclosing_struct_items)
              for (auto& si : *g_enclosing_struct_items)
                if (si.k == cmi::cmiw::SigItem::Module && si.name == l->name &&
                    !si.is_functor && si.alias.empty()) {
                  for (auto& s2 : si.sub) out.push_back(s2);
                  local = true;
                  break;
                }
            // Otherwise resolve the path like `module type of` everywhere:
            // through opened local modules, alias chains (strengthening at
            // the normalized path), or a unit's cmi (a stdlib unit routes
            // through the Stdlib alias member, so it strengthens too --
            // `include module type of Hashtbl` records `= ('a, 'b)
            // Stdlib__Hashtbl.t` manifests; gatien_baron's Hash2).
            if (!local) {
              auto t = resolve_typeof_path(g_enclosing_struct_items,
                                           lid_full(pi->id.txt));
              if (t.ok) {
                if (t.through_alias && !via_struct_include)
                  strengthen_abstract(t.items, t.norm, false);
                for (auto& si : t.items) out.push_back(std::move(si));
              }
            }
          }
      } else if (const ast::Signature* bs = body_sig(pinc->mt)) {
        // `include S` (named local modtype) / `include sig .. end`
        auto items = signature_to_cmi_i(*bs, &modtypes, &module_sigs, &ck);
        // Type-level `with` refinements only: module substitutions in an
        // include are left to the existing layout handling (lambda.cpp's AST
        // walk doesn't apply them, and cmi/cmo layouts must agree).
        apply_with_constraints(ck, pinc->mt, items, /*depth_bias=*/0);
        for (auto& si : items) out.push_back(std::move(si));
      } else if (auto items = qual_modtype_items(pinc->mt); !items.empty()) {
        // `include Identifiable.S with type t = int` (a QUALIFIED cross-module
        // module type): splice its members from the head module's cmi, so the
        // .cmi records the included values/submodules (Numbers.Int gets Map/Set/
        // compare).  Without it a downstream `include Numbers.Int` builds a short
        // block missing Key.Map -> Arg_helper.Make's parsed record is garbage.
        // qual_modtype_items STRIPS the Pmty_with chain, so apply the type
        // refinements here: `with type t := t` must ERASE S's abstract t
        // (ident.mli -- without the erasure the dedup pass replaced the
        // enclosing record decl with S's abstract one, dropping the labels).
        // Module substitutions stay layout-handled elsewhere (see above).
        apply_with_constraints(ck, pinc->mt, items, /*depth_bias=*/0);
        for (auto& si : items) out.push_back(std::move(si));
      }
      // `include S with type t := ..`: the destructive subst relocates every
      // spliced member to the included module-type expression's location
      // (identifiable.mli's `include Hashtbl.HashedType with type t := t`).
      if (with_has_destructive_subst(pinc->mt)) {
        cmi::cmiw::Loc rl = conv_loc(pinc->mt.loc);
        for (std::size_t i = incbefore; i < out.size(); ++i)
          relocate_sig_item(out[i], rl);
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
  // TYPE declarations too (no runtime field, but one namespace per sig): two
  // `include module type of ..` both carrying `type 'a t` keep one (pr5164).
  {
    std::unordered_map<std::string, std::size_t> lastt;
    bool tdup = false;
    for (std::size_t i = 0; i < out.size(); ++i)
      if (out[i].k == cmi::cmiw::SigItem::Type) {
        if (lastt.count(out[i].name)) tdup = true;
        lastt[out[i].name] = i;
      }
    if (tdup) {
      std::vector<cmi::cmiw::SigItem> ded; ded.reserve(out.size());
      for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i].k != cmi::cmiw::SigItem::Type || lastt[out[i].name] == i)
          ded.push_back(std::move(out[i]));
      out = std::move(ded);
    }
  }
  return out;
}

std::vector<cmi::cmiw::SigItem> signature_to_cmi(
    const ast::Signature& s,
    const std::unordered_map<std::string, const ast::Signature*>* outer,
    const std::unordered_map<std::string, const ast::Signature*>* outer_mods) {
  return signature_to_cmi_i(s, outer, outer_mods, nullptr);
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

// ---- functor application results ------------------------------------------
// `module IntSet = Set.Make(..)`: ocamlc records the functor's RESULT signature
// with the parameter substituted away.  We rebuild that: the result sig comes
// from the functor's own .cmi (or a local functor's already-emitted SigItem),
// and every constr path headed by the parameter name is rewritten -- to the
// argument's path (`Ord.t` -> `String.t`) or, for an anonymous struct argument,
// to the struct's own manifest (`Ord.t` -> `int`).
struct FunctorArgSubst {
  std::string param;     // parameter name ("Ord"); empty = nothing to rewrite
  std::string arg_path;  // path argument ("String", "Digest.MD5")
  std::unordered_map<std::string, cmi::cmiw::TyPtr> manifests;  // struct argument
  // struct argument, PHANTOM decl (`type 'a t = 'a ..`: manifest == params[i]):
  // an application substitutes to its i-th argument (pr4775's `'a A.t` -> 'a)
  std::unordered_map<std::string, int> phantoms;
};
// Does `name` contain `comp` as a whole PATH COMPONENT?  Components are
// delimited by '.', '(' and ')' -- so "Ord" is found in "Ord.t" and in
// "Map.Make(Ord).t" but not in "Order.t".
static bool path_has_component(const std::string& name, const std::string& comp) {
  for (std::size_t p = name.find(comp); p != std::string::npos;
       p = name.find(comp, p + 1)) {
    bool lok = p == 0 || name[p - 1] == '.' || name[p - 1] == '(';
    std::size_t e = p + comp.size();
    bool rok = e == name.size() || name[e] == '.' || name[e] == ')' || name[e] == '(';
    if (lok && rok) return true;
  }
  return false;
}
// Rewrite every whole-component occurrence of `comp` in a path to `repl`.
static std::string path_replace_component(const std::string& name,
                                          const std::string& comp,
                                          const std::string& repl) {
  std::string out;
  for (std::size_t p = 0; p < name.size();) {
    if (name.compare(p, comp.size(), comp) == 0) {
      bool lok = p == 0 || name[p - 1] == '.' || name[p - 1] == '(';
      std::size_t e = p + comp.size();
      bool rok = e == name.size() || name[e] == '.' || name[e] == ')' || name[e] == '(';
      if (lok && rok) { out += repl; p = e; continue; }
    }
    out += name[p++];
  }
  return out;
}
// Does the type still reference any parameter that had NO path to rewrite to
// (an anonymous struct argument)?  Such a reference cannot appear in the
// recorded signature -- ocamlc erases the manifest (nondep) instead.
static bool ty_mentions_unsubst_param(const cmi::cmiw::TyPtr& t,
                                      const std::vector<FunctorArgSubst>& subs) {
  if (!t) return false;
  if (t->k == cmi::cmiw::Ty::Constr || t->k == cmi::cmiw::Ty::Package)
    for (auto& s : subs)
      if (!s.param.empty() && s.arg_path.empty() &&
          path_has_component(t->name, s.param))
        return true;
  for (auto& a : t->args)
    if (ty_mentions_unsubst_param(a, subs)) return true;
  return false;
}
static cmi::cmiw::TyPtr subst_param_ty(
    const cmi::cmiw::TyPtr& t, const std::vector<FunctorArgSubst>& subs,
    std::unordered_map<const cmi::cmiw::Ty*, cmi::cmiw::TyPtr>& memo) {
  if (!t) return t;
  // Memoized per item: a node cited twice (a constrained param shared into
  // the manifest) must substitute to ONE node, or Printtyp loses the `'a`
  // aliasing (pr4775's `type ('a,'b) t = 'a`); also terminates cyclic types.
  if (auto it = memo.find(t.get()); it != memo.end()) return it->second;
  auto r = std::make_shared<cmi::cmiw::Ty>(*t);  // never mutate shared nodes
  memo[t.get()] = r;
  for (auto& a : r->args) a = subst_param_ty(a, subs, memo);
  for (auto& a : r->row_name_args) a = subst_param_ty(a, subs, memo);
  if (r->k == cmi::cmiw::Ty::Constr || r->k == cmi::cmiw::Ty::Package) {
    for (auto& s : subs) {
      if (s.param.empty() || !path_has_component(r->name, s.param)) continue;
      if (r->k == cmi::cmiw::Ty::Constr && r->args.empty() &&
          r->name.rfind(s.param + ".", 0) == 0)
        if (auto m = s.manifests.find(r->name.substr(s.param.size() + 1));
            m != s.manifests.end())
          return memo[t.get()] = m->second;
      if (r->k == cmi::cmiw::Ty::Constr && !r->args.empty() &&
          r->name.rfind(s.param + ".", 0) == 0)
        if (auto ph = s.phantoms.find(r->name.substr(s.param.size() + 1));
            ph != s.phantoms.end() &&
            (std::size_t)ph->second < r->args.size())
          // args already substituted above
          return memo[t.get()] = r->args[ph->second];
      if (!s.arg_path.empty())
        r->name = path_replace_component(r->name, s.param, s.arg_path);
      break;
    }
  }
  return r;
}
static void subst_param_items(std::vector<cmi::cmiw::SigItem>& items,
                              const std::vector<FunctorArgSubst>& subs) {
  for (auto& si : items) {
    std::unordered_map<const cmi::cmiw::Ty*, cmi::cmiw::TyPtr> memo;
    si.ty = subst_param_ty(si.ty, subs, memo);
    // A manifest that STILL mentions an anonymous-argument parameter after
    // substitution can't be expressed (`'a t = 'a Map.Make(Ord).t`): erase it,
    // leaving the decl abstract with its recorded variance -- as ocamlc does.
    si.manifest = subst_param_ty(si.manifest, subs, memo);
    if (si.k == cmi::cmiw::SigItem::Type && si.manifest &&
        ty_mentions_unsubst_param(si.manifest, subs))
      si.manifest = nullptr;
    for (auto& p : si.params) p = subst_param_ty(p, subs, memo);
    for (auto& c : si.ctors) {
      for (auto& a : c.args) a = subst_param_ty(a, subs, memo);
      for (auto& l : c.inline_record) l.ty = subst_param_ty(l.ty, subs, memo);
      c.res = subst_param_ty(c.res, subs, memo);
    }
    for (auto& l : si.labels) l.ty = subst_param_ty(l.ty, subs, memo);
    si.ext_ret = subst_param_ty(si.ext_ret, subs, memo);
    for (auto& f : si.class_fields) f.ty = subst_param_ty(f.ty, subs, memo);
    for (auto& d : si.class_arrow_doms) d = subst_param_ty(d, subs, memo);
    subst_param_items(si.sub, subs);
    subst_param_items(si.param_sig, subs);
    for (auto& ps : si.more_param_sigs) subst_param_items(ps, subs);
  }
}

// The emitting file's `module type` ASTs and opened-modtype qualifications,
// visible to nested functor-body inference (set by infer_signature around its
// emission loop; restored on return so recursion nests correctly).
static const std::unordered_map<std::string, const ast::Signature*>*
    g_outer_modtype_asts = nullptr;
static const std::unordered_map<std::string, std::string>*
    g_outer_modtype_quals = nullptr;
// The emitting file's module value exports (Checker::modenv, flat by simple
// name), seeded into a submodule's re-inference so references to ENCLOSING
// local modules don't degrade to fresh vars -- `module Test = struct open
// ExtUnix.All; let module B = BigEndian in B.get_uint8 x 0 end` types int
// only if the sub-checker can resolve the outer file's modules (pr6726).
// The main pass types submodules in the SAME checker, so it never loses
// them; only the emission re-inference (a fresh Checker) did.
static const std::unordered_map<std::string,
                                std::unordered_map<std::string, TypePtr>>*
    g_outer_modenv = nullptr;
// The emitting file's top-level VALUE scopes (Checker::venv), seeded the same
// way: a functor body calling an enclosing let (`print (module P) x` inside
// PList, typing-modular-explicits/compiling) types with the real scheme
// instead of degrading everything it touches to fresh vars.
static const std::vector<std::unordered_map<std::string, TypePtr>>*
    g_outer_venv = nullptr;
// The emitting file's top-level CONSTRUCTOR scopes (Checker::cenv), seeded the
// same way: a module body's pattern/expression citing an ENCLOSING type's ctor
// (`type t = A;; module B = struct type t = B let f A = B end` -- pr4791)
// types with the OUTER t instead of degrading the argument to a fresh var.
static const std::vector<std::unordered_map<std::string, TypePtr>>*
    g_outer_cenv = nullptr;
// The ENCLOSING module's already-emitted items, visible to functor-BODY
// inference so `module Y = G(X)` inside a functor body resolves the sibling
// functor G declared in the outer scope (set around the body's
// infer_signature call; restored so recursion nests correctly).
static const std::vector<cmi::cmiw::SigItem>* g_outer_prior = nullptr;

// Strengthen a functor-application result whose arguments are all PATHS: every
// abstract type gets the applied-functor manifest ocamlc records
// (`module S = Set.Make(Loc)` gives `type t = Set.Make(Loc).t`), recursively
// through plain submodules.  `app` is the applied path ("Set.Make(Loc)").
static void strengthen_abstract(std::vector<cmi::cmiw::SigItem>& items,
                                const std::string& app, bool aliasable) {
  for (auto& si : items) {
    // Like Mtype.strengthen: abstract AND datatype (record/variant) decls
    // gain the `= path.t` manifest alongside their kind (`type t = A.t =
    // { x : int; }`); the params are SHARED into the manifest args so
    // `type 'a t = 'a A.t = ..` prints one var.
    if (si.k == cmi::cmiw::SigItem::Type && !si.manifest &&
        !si.type_open && !si.type_empty_variant) {
      std::vector<cmi::cmiw::TyPtr> as(si.params.begin(), si.params.end());
      si.manifest = cmi::cmiw::ty_constr(app + "." + si.name, std::move(as));
    } else if (si.k == cmi::cmiw::SigItem::Module && !si.is_functor &&
               si.alias.empty()) {
      // With `aliasable` (a nameable module path -- `include M`), a
      // submodule strengthens to Mty_alias(M.Set) like Mtype.strengthen's
      // strengthen_decl ~aliasable:true; a functor-application path can't
      // be aliased, so those callers recurse instead.
      if (aliasable) {
        si.alias = app + "." + si.name;
        si.sub.clear();
        si.modtype_ref.clear();
      } else {
        strengthen_abstract(si.sub, app + "." + si.name, false);
      }
    } else if (si.k == cmi::cmiw::SigItem::Module && si.is_functor &&
               si.alias.empty() && !si.functor_unit && !aliasable) {
      // An APPLICATIVE functor member: Mtype.strengthen strengthens its
      // RESULT at Papply(p.F, param) -- `type 'a t = 'a Stdlib__Hashtbl.
      // Make(H).t`.  A generative `()` param doesn't strengthen; an anonymous
      // param is skipped (the writer binds only NAMED params, so a synthetic
      // "Arg" head would emit an unresolvable path).
      std::string ap = app + "." + si.name;
      ap += si.functor_param.empty() ? "" : "(" + si.functor_param + ")";
      bool strengthenable = !si.functor_param.empty();
      for (std::size_t i = 0;
           strengthenable && i < si.more_param_names.size(); ++i) {
        if ((i < si.more_param_units.size() && si.more_param_units[i]) ||
            si.more_param_names[i].empty())
          strengthenable = false;
        else
          ap += "(" + si.more_param_names[i] + ")";
      }
      if (strengthenable) strengthen_abstract(si.sub, ap, false);
    }
  }
}

// Walk every type in a SigItem tree, applying `fn` to each Constr/Package
// path name (in place -- nodes here are freshly built per item, and prefix
// rewrites are idempotent under the sharing a local-functor copy introduces).
static void rewrite_ty_names(const cmi::cmiw::TyPtr& t,
                             const std::function<void(std::string&)>& fn) {
  if (!t) return;
  if (t->k == cmi::cmiw::Ty::Constr || t->k == cmi::cmiw::Ty::Package) fn(t->name);
  for (auto& a : t->args) rewrite_ty_names(a, fn);
  if (!t->row_name.empty()) fn(t->row_name);
  for (auto& a : t->row_name_args) rewrite_ty_names(a, fn);
}
static void rewrite_item_ty_names(std::vector<cmi::cmiw::SigItem>& items,
                                  const std::function<void(std::string&)>& fn) {
  for (auto& si : items) {
    rewrite_ty_names(si.ty, fn);
    rewrite_ty_names(si.manifest, fn);
    for (auto& p : si.params) rewrite_ty_names(p, fn);
    for (auto& c : si.ctors) {
      for (auto& a : c.args) rewrite_ty_names(a, fn);
      for (auto& l : c.inline_record) rewrite_ty_names(l.ty, fn);
      rewrite_ty_names(c.res, fn);
    }
    for (auto& l : si.labels) rewrite_ty_names(l.ty, fn);
    rewrite_ty_names(si.ext_ret, fn);
    for (auto& f : si.class_fields) rewrite_ty_names(f.ty, fn);
    for (auto& d : si.class_arrow_doms) rewrite_ty_names(d, fn);
    rewrite_item_ty_names(si.sub, fn);
    rewrite_item_ty_names(si.param_sig, fn);
    for (auto& ps : si.more_param_sigs) rewrite_item_ty_names(ps, fn);
  }
}
// NODE-level rewrite: fn may REPLACE the pointed-to node wholesale (used by
// `with type t := <non-path>` substitution, where a tuple RHS can't be
// expressed as a name rewrite).  fn is applied to each node BEFORE its args
// are walked; a replaced node's args are not re-walked.
static void rewrite_ty_nodes(cmi::cmiw::TyPtr& t,
                             const std::function<void(cmi::cmiw::TyPtr&)>& fn) {
  if (!t) return;
  auto* before = t.get();
  fn(t);
  if (t.get() != before) return;  // replaced: the new tree is final
  for (auto& a : t->args) rewrite_ty_nodes(a, fn);
  for (auto& a : t->row_name_args) rewrite_ty_nodes(a, fn);
}
static void rewrite_item_ty_nodes(std::vector<cmi::cmiw::SigItem>& items,
                                  const std::function<void(cmi::cmiw::TyPtr&)>& fn) {
  for (auto& si : items) {
    rewrite_ty_nodes(si.ty, fn);
    rewrite_ty_nodes(si.manifest, fn);
    for (auto& p : si.params) rewrite_ty_nodes(p, fn);
    for (auto& c : si.ctors) {
      for (auto& a : c.args) rewrite_ty_nodes(a, fn);
      for (auto& l : c.inline_record) rewrite_ty_nodes(l.ty, fn);
      rewrite_ty_nodes(c.res, fn);
    }
    for (auto& l : si.labels) rewrite_ty_nodes(l.ty, fn);
    rewrite_ty_nodes(si.ext_ret, fn);
    for (auto& f : si.class_fields) rewrite_ty_nodes(f.ty, fn);
    for (auto& d : si.class_arrow_doms) rewrite_ty_nodes(d, fn);
    rewrite_item_ty_nodes(si.sub, fn);
    rewrite_item_ty_nodes(si.param_sig, fn);
    for (auto& ps : si.more_param_sigs) rewrite_item_ty_nodes(ps, fn);
  }
}

// `with type t := u` citation rewrite, referent-aware (Identifiable.S in
// ident.mli/identifiable.mli's Make): a PLAIN bare citation of `name` refers
// to the erased decl only while NO intervening level declares its own `name`
// (Set's `val empty : t` means Set's t -- untouched); a `with type`-spliced
// item whose with_scope_skip reaches EXACTLY the subst level refers to the
// erased decl THROUGH the shadow (S's `module T : Thing with type t = t`) and
// is rewritten regardless.
static void subst_type_citations(std::vector<cmi::cmiw::SigItem>& items,
                                 const std::string& name,
                                 const std::string& newname, int d,
                                 bool shadowed) {
  bool here = false;
  for (auto& si : items)
    if (si.k == cmi::cmiw::SigItem::Type && si.name == name) here = true;
  bool active = !shadowed && !(d > 0 && here);
  auto fn = [&](std::string& n) { if (n == name) n = newname; };
  for (auto& si : items) {
    bool skip_hit = si.k == cmi::cmiw::SigItem::Type && d > 0 &&
                    si.with_scope_skip == d;
    if (active || skip_hit) {
      rewrite_ty_names(si.manifest, fn);
      for (auto& p : si.params) rewrite_ty_names(p, fn);
    }
    if (active) {
      rewrite_ty_names(si.ty, fn);
      for (auto& c : si.ctors) {
        for (auto& a : c.args) rewrite_ty_names(a, fn);
        for (auto& l : c.inline_record) rewrite_ty_names(l.ty, fn);
        rewrite_ty_names(c.res, fn);
      }
      for (auto& l : si.labels) rewrite_ty_names(l.ty, fn);
      rewrite_ty_names(si.ext_ret, fn);
      for (auto& f : si.class_fields) rewrite_ty_names(f.ty, fn);
      for (auto& d2 : si.class_arrow_doms) rewrite_ty_names(d2, fn);
    }
    bool child_shadowed = shadowed || (d > 0 && here);
    subst_type_citations(si.sub, name, newname, d + 1, child_shadowed);
    subst_type_citations(si.param_sig, name, newname, d + 1, child_shadowed);
    for (auto& ps : si.more_param_sigs)
      subst_type_citations(ps, name, newname, d + 1, child_shadowed);
  }
}

// Deep-copy the Ty trees hanging off SigItems (memo preserves node sharing,
// so `as 'a` rows survive).  Needed before an IN-PLACE rewrite of items copied
// from another SigItem -- the TyPtr nodes are shared with the original.
static cmi::cmiw::TyPtr ty_deep_copy(
    const cmi::cmiw::TyPtr& t,
    std::unordered_map<const cmi::cmiw::Ty*, cmi::cmiw::TyPtr>& memo) {
  if (!t) return nullptr;
  if (auto it = memo.find(t.get()); it != memo.end()) return it->second;
  auto c = std::make_shared<cmi::cmiw::Ty>(*t);
  memo[t.get()] = c;
  for (auto& a : c->args) a = ty_deep_copy(a, memo);
  return c;
}
static void items_deep_copy_tys(
    std::vector<cmi::cmiw::SigItem>& items,
    std::unordered_map<const cmi::cmiw::Ty*, cmi::cmiw::TyPtr>& memo) {
  for (auto& si : items) {
    auto cp = [&](cmi::cmiw::TyPtr& p) { p = ty_deep_copy(p, memo); };
    cp(si.ty); cp(si.manifest);
    for (auto& p : si.params) cp(p);
    for (auto& c : si.ctors) {
      for (auto& a : c.args) cp(a);
      for (auto& l : c.inline_record) cp(l.ty);
      cp(c.res);
    }
    for (auto& l : si.labels) cp(l.ty);
    cp(si.ext_ret);
    for (auto& f : si.class_fields) cp(f.ty);
    for (auto& d : si.class_arrow_doms) cp(d);
    items_deep_copy_tys(si.sub, memo);
    items_deep_copy_tys(si.param_sig, memo);
    for (auto& ps : si.more_param_sigs) items_deep_copy_tys(ps, memo);
  }
}

static std::vector<std::string> split_dotted(const std::string& s) {
  std::vector<std::string> out;
  for (std::size_t p = 0, d; p <= s.size(); p = d + 1) {
    d = s.find('.', p);
    if (d == std::string::npos) d = s.size();
    out.push_back(s.substr(p, d - p));
  }
  return out;
}

// A value inside a resolved modtype may cite a type declared at the ENCLOSING
// unit level (Hashtbl's `statistics`, cited bare by `SeededS.stats`): its cmi
// Pident decodes to a bare `statistics` that no longer resolves once the
// modtype is spliced into a functor result, degrading to a fresh var
// (`stats : 'a t -> 'b`).  Requalify such refs to the enclosing unit's real
// path (`Stdlib__Hashtbl.statistics`).  Only names the modtype does NOT itself
// declare are rewritten, so its own abstract `t`/`key` (which shadow any
// same-named unit type) stay bare.  `outer_sig` is the signature that CONTAINS
// the modtype decl; `unit_path` the real compiled path of that signature.
static void qualify_enclosing_types(std::vector<cmi::cmiw::SigItem>& result,
                                    const cmi::Signature& outer_sig,
                                    const std::string& unit_path,
                                    const std::set<std::string>* unit_submods,
                                    const std::string& unit_name) {
  std::set<std::string> outer;
  for (auto& td : outer_sig.types) outer.insert(td.name);
  bool have_submods = unit_submods && !unit_submods->empty() && !unit_name.empty();
  if (outer.empty() && !have_submods) return;
  std::set<std::string> shadow;
  std::function<void(const std::vector<cmi::cmiw::SigItem>&)> collect =
      [&](const std::vector<cmi::cmiw::SigItem>& its) {
        for (auto& si : its) {
          if (si.k == cmi::cmiw::SigItem::Type) shadow.insert(si.name);
          collect(si.sub);
        }
      };
  collect(result);
  rewrite_item_ty_names(result, [&](std::string& n) {
    auto dot = n.find('.');
    if (dot == std::string::npos) {
      // A BARE type name owned by the enclosing module -> qualify with the
      // full unit_path (`position` -> `CamlinternalMenhirLib.IncrementalEngine.
      // position`).
      if (outer.count(n) && !shadow.count(n)) n = unit_path + "." + n;
    } else if (have_submods) {
      // A path whose HEAD is a SIBLING submodule of the unit (a top-level
      // module of the loaded cmi, not in scope in the including unit):
      // `General.stream` -> `CamlinternalMenhirLib.General.stream`.  The modtype
      // was written with `General` in scope (an `open General` / sibling ref);
      // spliced into another unit that bare path resolves to nothing and ocamlc
      // reports `General.stream is abstract, no cmi found` -> segfault here.
      std::string head = n.substr(0, dot);
      if (unit_submods->count(head) && !shadow.count(head) && head != unit_name)
        n = unit_name + "." + n;
    }
  });
}

// Signature items of a QUALIFIED named module type ("Set.S",
// "Pqueue.OrderedType"), resolved through the head module's compiled cmi.
static std::vector<cmi::cmiw::SigItem> cmi_modtype_items(
    const std::vector<std::string>& comps0) {
  if (comps0.size() < 2) return {};
  try {
    // A component may be an ALIAS member (`Stdlib.Set.S`: stdlib.cmi's Set is
    // `module Set = Stdlib__Set`, Mp_absent) -- re-root the remaining path at
    // the alias target and walk again (ident.mli's `module Set :
    // Stdlib.Set.S with type elt = t` wrote an EMPTY sig without this).
    std::vector<std::string> comps = comps0;
    for (int guard = 0; guard < 8; ++guard) {
    const auto& cmif = cmi::CmiFile::load(head_cmi(comps[0]));
    const cmi::Signature* sig = &cmif.sig();
    bool rerooted = false;
    for (std::size_t i = 1; i + 1 < comps.size() && sig; ++i) {
      const cmi::ModuleDecl* md = nullptr;
      for (auto& mm : sig->modules) if (mm.name == comps[i]) { md = &mm; break; }
      if (md && md->type && md->type->kind == cmi::ModuleType::Alias &&
          md->type->path) {
        std::string tgt = cmi_path_str(*md->type->path);
        if (!tgt.empty()) {
          std::vector<std::string> nc = split_dotted(tgt);
          nc.insert(nc.end(), comps.begin() + i + 1, comps.end());
          comps = std::move(nc);
          rerooted = true;
          break;
        }
      }
      sig = (md && md->type && md->type->kind == cmi::ModuleType::Sig)
                ? md->type->sig.get() : nullptr;
    }
    if (rerooted) continue;
    if (!sig) return {};
    for (auto& mtd : sig->modtypes)
      if (mtd.name == comps.back() && mtd.type &&
          mtd.type->kind == cmi::ModuleType::Sig && mtd.type->sig) {
        auto result = cmi_sig_to_items(*mtd.type->sig, [&] {
          std::string o;
          for (std::size_t i = 0; i + 1 < comps.size(); ++i) o += (i ? "." : "") + comps[i];
          return o;
        }());
        std::string unit_path = cmif.module_name();
        for (std::size_t i = 1; i + 1 < comps.size(); ++i)
          unit_path += "." + comps[i];
        std::set<std::string> unit_submods;
        for (auto& um : cmif.sig().modules) unit_submods.insert(um.name);
        qualify_enclosing_types(result, *sig, unit_path, &unit_submods,
                                cmif.module_name());
        return result;
      }
    return {};  // modtype not found in the resolved signature
    }           // guard: re-rooted alias walk
  } catch (...) {}
  return {};
}

// Resolve a module-type ANNOTATION position (a struct-side functor param or
// result) into (ref, sig): a literal signature converts verbatim; a named ref
// is kept as Mty_ident (a local decl's / dotted cmi's items stay the fallback
// layout); `S with ..` resolves the base and grafts the refinements (ocamlc
// stores those expanded -- a `with` can't stay a plain Mty_ident).
static void annot_modtype_items(Checker* ckp, const ast::ModuleType& mt,
                                std::string& ref,
                                std::vector<cmi::cmiw::SigItem>& sig) {
  if (auto* psg = std::get_if<Pmty_signature>(&mt.desc)) {
    sig = signature_to_cmi(psg->items);
  } else if (auto* pid = std::get_if<Pmty_ident>(&mt.desc)) {
    ref = lid_full(pid->id.txt);
    if (ckp && ref.find('.') == std::string::npos) {
      if (auto q = ckp->opened_modtype_quals_.find(ref);
          q != ckp->opened_modtype_quals_.end())
        ref = q->second;
      else if (auto a = ckp->modtype_sig_asts_.find(ref);
               a != ckp->modtype_sig_asts_.end())
        sig = signature_to_cmi(*a->second);
    }
    if (sig.empty() && ref.find('.') != std::string::npos)
      sig = cmi_modtype_items(split_dotted(ref));
  } else if (ckp && std::holds_alternative<Pmty_with>(mt.desc)) {
    const ast::ModuleType* base = &mt;
    while (auto* pw = std::get_if<Pmty_with>(&base->desc)) base = pw->mt.get();
    if (auto* bid = std::get_if<Pmty_ident>(&base->desc)) {
      std::string bref = lid_full(bid->id.txt);
      if (bref.find('.') == std::string::npos) {
        if (auto q = ckp->opened_modtype_quals_.find(bref);
            q != ckp->opened_modtype_quals_.end())
          bref = q->second;
        else if (auto a = ckp->modtype_sig_asts_.find(bref);
                 a != ckp->modtype_sig_asts_.end())
          sig = signature_to_cmi(*a->second);
      }
      if (sig.empty() && bref.find('.') != std::string::npos)
        sig = cmi_modtype_items(split_dotted(bref));
    } else if (auto* bsg = std::get_if<Pmty_signature>(&base->desc)) {
      sig = signature_to_cmi(bsg->items);
    }
    if (!sig.empty()) {
      apply_with_constraints(*ckp, mt, sig);
      drop_modsubst(sig, with_modsubst_names(mt));
    }
  }
}

// The Sig_module item for one module binding, by shape: a structure body is
// inferred recursively; `module M : sig .. end = ..` takes the CONSTRAINT
// signature verbatim (it is authoritative, like a .mli); a functor emits
// Mty_functor with its named param's signature and its body's; a functor
// APPLICATION emits the substituted result signature (above).  `prior` is the
// surrounding module's already-emitted items (local-functor lookup); `ckp` the
// surrounding checker (opened-submodule heads: `open MoreLabels` + Map.Make
// must take the LABELLED Map).  Null when the shape isn't representable yet
// (unpack, unresolvable application).
static TypeofResolved resolve_typeof_path(
    const std::vector<cmi::cmiw::SigItem>* scope, const std::string& dotted,
    int depth) {
  TypeofResolved r;
  if (depth > 8 || dotted.empty() ||
      dotted.find('(') != std::string::npos)  // no functor-application paths
    return r;
  std::vector<std::string> comps = split_dotted(dotted);
  std::vector<cmi::cmiw::SigItem> items;  // current module's members
  std::string norm;                       // its normalized path so far
  bool ta = false;
  std::size_t next = 1;  // first comps index still to descend
  // HEAD: an emitted local module -- directly in scope, or a member of an
  // `open`ed local module (gatien_baron's `open Std; .. module type of Hash`).
  const cmi::cmiw::SigItem* head = nullptr;
  if (scope)
    for (auto& si : *scope)
      if (si.k == cmi::cmiw::SigItem::Module && si.name == comps[0]) {
        head = &si;
        break;
      }
  cmi::cmiw::SigItem opened_head;  // owns a member found through an open
  if (!head && scope && g_inherited_opens)
    for (auto oi = g_inherited_opens->rbegin();
         oi != g_inherited_opens->rend() && !head; ++oi) {
      auto op = resolve_typeof_path(scope, lid_full(**oi), depth + 1);
      if (!op.ok) continue;
      for (auto& si : op.items)
        if (si.k == cmi::cmiw::SigItem::Module && si.name == comps[0]) {
          opened_head = std::move(si);
          head = &opened_head;
          break;
        }
    }
  if (head) {
    if (!head->alias.empty()) {
      // Chase the alias: re-resolve its target plus our remaining components
      // from the top scope (best effort -- the target was written in the
      // aliasing module's own context, but a local sibling or unit name
      // resolves the same from here).
      std::string rest = head->alias;
      for (std::size_t i = 1; i < comps.size(); ++i) rest += "." + comps[i];
      auto t = resolve_typeof_path(scope, rest, depth + 1);
      if (t.ok) t.through_alias = true;
      return t;
    }
    if (head->is_functor) return r;
    items = head->sub;
    norm = comps[0];
  } else {
    // A compilation unit.  A bare stdlib name resolves through the `Stdlib.X`
    // alias member, so it normalizes to the MANGLED unit and strengthens; a
    // direct -I unit is a plain persistent signature (no alias, abstract).
    std::string ucmi = head_cmi(comps[0]);
    if (!std::filesystem::exists(ucmi)) return r;
    try {
      const auto& cmif = cmi::CmiFile::load(ucmi);
      items = cmi_sig_to_items(cmif.sig(), comps[0]);
      norm = cmif.module_name().empty() ? comps[0] : cmif.module_name();
    } catch (...) {
      return r;
    }
    std::string base = ucmi.substr(ucmi.rfind('/') + 1);
    ta = base.rfind("stdlib__", 0) == 0 &&
         comps[0].rfind("Stdlib__", 0) != 0;
  }
  for (; next < comps.size(); ++next) {
    const cmi::cmiw::SigItem* m = nullptr;
    for (auto& si : items)
      if (si.k == cmi::cmiw::SigItem::Module && si.name == comps[next]) {
        m = &si;
        break;
      }
    if (!m) return r;
    if (!m->alias.empty()) {
      std::string rest = m->alias;
      for (std::size_t i = next + 1; i < comps.size(); ++i)
        rest += "." + comps[i];
      auto t = resolve_typeof_path(scope, rest, depth + 1);
      if (t.ok) t.through_alias = true;
      return t;
    }
    if (m->is_functor) return r;
    auto sub = m->sub;
    items = std::move(sub);
    norm += "." + comps[next];
  }
  r.items = std::move(items);
  r.norm = std::move(norm);
  r.through_alias = ta;
  r.ok = true;
  return r;
}

static std::optional<cmi::cmiw::SigItem> module_binding_sigitem(
    const std::string& name, const ast::ModuleExpr& me,
    const std::vector<cmi::cmiw::SigItem>* prior = nullptr,
    Checker* ckp = nullptr) {
  if (auto* ms = std::get_if<Pmod_structure>(&me.desc))
  {
    // The enclosing scope's items travel as g_outer_prior so a nested
    // `module R = M(T)` resolves an OUTER local functor M (pr5469).
    auto* saved_prior = g_outer_prior;
    if (prior) g_outer_prior = prior;
    auto items = infer_signature(ms->items);
    g_outer_prior = saved_prior;
    return cmi::cmiw::sig_module(name, std::move(items));
  }
  if (auto* pi = std::get_if<Pmod_ident>(&me.desc)) {
    // `module MP = Gc.Memprof` / `module Alias = A`: a module alias binding.
    // ocamlc records Mty_alias(<target path>) (Mp_absent -- transparent, takes
    // no runtime field), printed `module MP = Gc.Memprof`.  A functor
    // application (Lapply) has no path form -- leave it dropped.
    if (!std::holds_alternative<Lapply>(pi->id.txt.v))
      return cmi::cmiw::sig_module_alias(name, lid_full(pi->id.txt));
    return std::nullopt;
  }
  if (auto* mu = std::get_if<Pmod_unpack>(&me.desc)) {
    // `module X = (val x)` with NO annotation: x's inferred package type
    // (engine Constr "(module S)") names the modtype; ocamlc stores
    // Mty_ident S (index_aliases).
    if (auto* ui = std::get_if<Pexp_ident>(&mu->e->desc); ui && ckp)
      if (auto* l = std::get_if<Lident>(&ui->id.txt.v);
          l && !ckp->venv.empty()) {
        auto f = ckp->venv.back().find(l->name);
        if (f != ckp->venv.back().end()) {
          TypePtr t = I::Engine::repr(f->second);
          if (t->kind == I::Type::Kind::Constr &&
              t->path.rfind("(module ", 0) == 0 && t->path.back() == ')') {
            auto si = cmi::cmiw::sig_module(name, {});
            si.modtype_ref = t->path.substr(8, t->path.size() - 9);
            return si;
          }
        }
      }
    // `module T = (val e : S with type t = ..)`: ocamlc types T at the
    // package modtype EXPANDED, with-refinements grafted onto the items.
    if (auto* uc = std::get_if<Pexp_constraint>(&mu->e->desc))
      if (auto* pk = std::get_if<Ptyp_package>(&uc->t->desc)) {
        std::string ref = lid_full(pk->path.txt);
        std::vector<cmi::cmiw::SigItem> items;
        if (ckp && ref.find('.') == std::string::npos) {
          if (auto q = ckp->opened_modtype_quals_.find(ref);
              q != ckp->opened_modtype_quals_.end())
            ref = q->second;
          else if (auto a = ckp->modtype_sig_asts_.find(ref);
                   a != ckp->modtype_sig_asts_.end())
            items = signature_to_cmi(*a->second);
        }
        if (items.empty() && ref.find('.') != std::string::npos)
          items = cmi_modtype_items(split_dotted(ref));
        if (!items.empty() && ckp) {
          for (auto& [clid, cty] : pk->constraints) {
            auto comps = split_dotted(lid_full(clid.txt));
            std::vector<cmi::cmiw::SigItem>* cur = &items;
            for (std::size_t i = 0; i + 1 < comps.size() && cur; ++i) {
              std::vector<cmi::cmiw::SigItem>* next = nullptr;
              for (auto& si : *cur)
                if (si.k == cmi::cmiw::SigItem::Module && si.name == comps[i]) {
                  next = &si.sub; break;
                }
              cur = next;
            }
            if (!cur) continue;
            for (auto& si : *cur)
              if (si.k == cmi::cmiw::SigItem::Type && si.name == comps.back() &&
                  si.params.empty()) {
                std::unordered_map<std::string, TypePtr> tvars;
                std::unordered_map<const I::Type*, int> bvars; int nv = 0;
                si.manifest = bridge_ty_named(ckp->from_coretype(*cty, tvars),
                                              bvars, nv, tvars);
                break;
              }
          }
          return cmi::cmiw::sig_module(name, std::move(items));
        }
      }
    return std::nullopt;
  }
  if (auto* mc = std::get_if<Pmod_constraint>(&me.desc)) {
    if (mc->mt) {
      if (auto* ps = std::get_if<Pmty_signature>(&mc->mt->desc))
        return cmi::cmiw::sig_module(name, signature_to_cmi(ps->items));
      // `module M : S with type t = u = ..`: ocamlc records the constrained
      // signature EXPANDED (the ascription is authoritative; a `with` can't
      // stay a plain Mty_ident).  Resolve the base modtype (local AST or the
      // head module's cmi) and graft the refinements.
      if (ckp && std::holds_alternative<Pmty_with>(mc->mt->desc)) {
        const ast::ModuleType* base = mc->mt.get();
        while (auto* pw = std::get_if<Pmty_with>(&base->desc)) base = pw->mt.get();
        std::vector<cmi::cmiw::SigItem> items;
        if (auto* pid = std::get_if<Pmty_ident>(&base->desc)) {
          std::string ref = lid_full(pid->id.txt);
          if (ref.find('.') == std::string::npos) {
            if (auto q = ckp->opened_modtype_quals_.find(ref);
                q != ckp->opened_modtype_quals_.end())
              ref = q->second;
            else if (auto a = ckp->modtype_sig_asts_.find(ref);
                     a != ckp->modtype_sig_asts_.end())
              items = signature_to_cmi(*a->second);
          }
          if (items.empty() && ref.find('.') != std::string::npos)
            items = cmi_modtype_items(split_dotted(ref));
        } else if (auto* psg = std::get_if<Pmty_signature>(&base->desc)) {
          items = signature_to_cmi(psg->items);
        }
        if (!items.empty()) {
          apply_with_constraints(*ckp, *mc->mt, items);
          drop_modsubst(items, with_modsubst_names(*mc->mt));
          return cmi::cmiw::sig_module(name, std::move(items));
        }
      }
      if (auto* pto = std::get_if<Pmty_typeof>(&mc->mt->desc)) {
        // `module Hash1 : module type of Hash = Hash` / `module M' : module
        // type of Std'.M = Std2.M`: the ascription is authoritative -- ocamlc
        // records the typeof signature (strengthened iff the path crossed an
        // alias), NOT an alias to the RHS (gatien_baron).
        if (auto* pi2 = std::get_if<Pmod_ident>(&pto->me->desc);
            pi2 && !std::holds_alternative<Lapply>(pi2->id.txt.v)) {
          auto t = resolve_typeof_path(prior ? prior : g_enclosing_struct_items,
                                       lid_full(pi2->id.txt));
          if (t.ok) {
            if (t.through_alias) strengthen_abstract(t.items, t.norm, false);
            return cmi::cmiw::sig_module(name, std::move(t.items));
          }
        }
      }
      if (auto* pid = std::get_if<Pmty_ident>(&mc->mt->desc)) {
        // `module M : S = struct .. end`: ocamlc records Mty_ident(S); the
        // inferred structure stays the fallback layout.
        auto inner = module_binding_sigitem(name, *mc->me, prior, ckp);
        if (!inner) inner = cmi::cmiw::sig_module(name, {});
        if (inner->k == cmi::cmiw::SigItem::Module && !inner->is_functor &&
            inner->alias.empty())
          inner->modtype_ref = lid_full(pid->id.txt);
        return inner;
      }
      if (std::holds_alternative<Pmty_functor>(mc->mt->desc)) {
        // `module F2 : S1 -> S1 -> T = functor (X : S) -> ..`: ocamlc stores
        // the ANNOTATION (an anonymous `S1 ->` param = Named(None, Mty_ident
        // S1)), not the expression's named params / inferred body.
        struct P { std::string name, ref; std::vector<cmi::cmiw::SigItem> sig;
                   bool unit = false; };
        std::vector<P> ps;
        auto conv = [&](const ast::ModuleType& mt, std::string& ref,
                        std::vector<cmi::cmiw::SigItem>& sig) {
          annot_modtype_items(ckp, mt, ref, sig);
        };
        const ast::ModuleType* body = mc->mt.get();
        while (auto* f = std::get_if<Pmty_functor>(&body->desc)) {
          P p;
          p.unit = std::holds_alternative<Functor_unit>(f->param);
          if (auto* fn = std::get_if<Functor_named>(&f->param)) {
            if (fn->name.txt) p.name = *fn->name.txt;
            if (fn->type) conv(*fn->type, p.ref, p.sig);
          }
          ps.push_back(std::move(p));
          body = f->body.get();
        }
        std::vector<cmi::cmiw::SigItem> result;
        std::string result_ref;
        conv(*body, result_ref, result);
        auto item = cmi::cmiw::sig_module_functor(name, ps[0].name,
                                                  std::move(ps[0].sig),
                                                  std::move(result));
        item.functor_result_ref = std::move(result_ref);
        item.functor_unit = ps[0].unit;
        item.functor_param_ref = std::move(ps[0].ref);
        for (std::size_t i = 1; i < ps.size(); ++i) {
          item.more_param_names.push_back(std::move(ps[i].name));
          item.more_param_sigs.push_back(std::move(ps[i].sig));
          item.more_param_units.push_back(ps[i].unit ? 1 : 0);
          item.more_param_refs.push_back(std::move(ps[i].ref));
        }
        return item;
      }
    }
    return module_binding_sigitem(name, *mc->me, prior, ckp);
  }
  if (std::holds_alternative<Pmod_functor>(me.desc)) {
    // Collect the whole CURRIED parameter chain (`(X : S) (Y : T) -> ..` --
    // each parameter after the first lives in a nested Pmod_functor body).
    struct P { std::string name, ref; std::vector<cmi::cmiw::SigItem> sig;
               std::vector<cmi::cmiw::SigItem> fsig;  // higher-order param
               bool unit = false; const ast::ModuleType* mt_ast = nullptr; };
    std::vector<P> ps;
    const ast::ModuleExpr* cur = &me;
    while (auto* f = std::get_if<Pmod_functor>(&cur->desc)) {
      P p;
      p.unit = std::holds_alternative<Functor_unit>(f->param);  // generative `()`
      if (auto* fn = std::get_if<Functor_named>(&f->param)) {
        if (fn->name.txt) p.name = *fn->name.txt;
        if (fn->type) {
          p.mt_ast = fn->type.get();
          if (auto* psg = std::get_if<Pmty_signature>(&fn->type->desc))
            p.sig = signature_to_cmi(psg->items);
          // A HIGHER-ORDER parameter (`(MakeDiet : functor (X : ORD) -> SET
          // with ..)`, t17ok): describe the param as a functor Module item;
          // the emitter reuses functor emission for its module type.
          if (auto* hpf = std::get_if<Pmty_functor>(&fn->type->desc)) {
            std::string ipname, ipref, iresref;
            std::vector<cmi::cmiw::SigItem> ipsig, ires;
            bool iunit = std::holds_alternative<Functor_unit>(hpf->param);
            if (auto* ifn = std::get_if<Functor_named>(&hpf->param)) {
              if (ifn->name.txt) ipname = *ifn->name.txt;
              if (ifn->type) annot_modtype_items(ckp, *ifn->type, ipref, ipsig);
            }
            const ast::ModuleType* ibody = hpf->body.get();
            annot_modtype_items(ckp, *ibody, iresref, ires);
            if (!std::holds_alternative<Pmty_ident>(ibody->desc)) iresref.clear();
            auto fit = cmi::cmiw::sig_module_functor(p.name, ipname,
                                                     std::move(ipsig),
                                                     std::move(ires));
            fit.functor_unit = iunit;
            fit.functor_param_ref = std::move(ipref);
            fit.functor_result_ref = std::move(iresref);
            p.fsig.push_back(std::move(fit));
          }
          if (auto* pid = std::get_if<Pmty_ident>(&fn->type->desc)) {
            p.ref = lid_full(pid->id.txt);
            // A bare ref declared by an opened module cites the QUALIFIED
            // name (`open Globroots` + `(G : GLOBREF)` -> Globroots.GLOBREF).
            if (ckp && p.ref.find('.') == std::string::npos)
              if (auto q = ckp->opened_modtype_quals_.find(p.ref);
                  q != ckp->opened_modtype_quals_.end())
                p.ref = q->second;
          }
          // `(Bar : S with type a = private [> `A])`: resolve S and graft the
          // refinements (ocamlc stores the expanded, refined param sig).
          if (std::holds_alternative<Pmty_with>(fn->type->desc))
            annot_modtype_items(ckp, *fn->type, p.ref, p.sig);
          // `(Baz : module type of struct include Bar end)` where Bar is an
          // EARLIER param: Bar's sig strengthened through Bar -- every type
          // gets `= Bar.t` (private dropped: ocamlc's Mtype.strengthen makes
          // a strengthened decl public).  pr6985.
          if (auto* ptf = std::get_if<Pmty_typeof>(&fn->type->desc))
            if (auto* tms = std::get_if<Pmod_structure>(&ptf->me->desc))
              for (auto& sit : tms->items)
                if (auto* pin = std::get_if<Pstr_include>(&sit.desc))
                  if (auto* imi = std::get_if<Pmod_ident>(&pin->expr.desc))
                    if (auto* il = std::get_if<Lident>(&imi->id.txt.v))
                      for (auto& prev : ps)
                        if (prev.name == il->name && prev.mt_ast) {
                          std::vector<cmi::cmiw::SigItem> inc = prev.sig;
                          if (inc.empty())
                            if (auto* pbs = std::get_if<Pmty_signature>(
                                    &prev.mt_ast->desc))
                              inc = signature_to_cmi(pbs->items);
                          for (auto& si : inc) {
                            if (si.k == cmi::cmiw::SigItem::Type &&
                                si.ctors.empty() && si.labels.empty() &&
                                !si.type_open && !si.type_empty_variant) {
                              std::vector<cmi::cmiw::TyPtr> as(
                                  si.params.begin(), si.params.end());
                              si.manifest = cmi::cmiw::ty_constr(
                                  prev.name + "." + si.name, std::move(as));
                              si.type_private = false;
                            } else if (si.k == cmi::cmiw::SigItem::Module &&
                                       !si.is_functor && si.alias.empty()) {
                              strengthen_abstract(si.sub,
                                                  prev.name + "." + si.name);
                            }
                          }
                          for (auto& si : inc) p.sig.push_back(std::move(si));
                          break;
                        }
        }
      }
      ps.push_back(std::move(p));
      cur = f->body.get();
    }
    std::vector<cmi::cmiw::SigItem> result;
    std::string result_ref;
    if (auto* bs = std::get_if<Pmod_structure>(&cur->desc)) {
      std::vector<std::pair<std::string, const ast::ModuleType*>> fps;
      for (auto& p : ps)
        if (!p.name.empty() && p.mt_ast) fps.emplace_back(p.name, p.mt_ast);
      auto* saved_prior = g_outer_prior;
      g_outer_prior = prior;
      result = infer_signature(bs->items, fps.empty() ? nullptr : &fps);
      g_outer_prior = saved_prior;
    }
    else if (auto* bi = std::get_if<Pmod_ident>(&cur->desc)) {
      // `module Id (S : S) = S` (an identity functor): ocamlc's result is the
      // PARAMETER's signature strengthened through the param path
      // (`sig type 'a t = 'a S.t end`), not an empty sig.  A DOTTED body
      // `module F (X : S) = X.X` projects the param's submodule: a named
      // submodule modtype keeps its ref qualified through the param
      // (`(X : S) -> X.T`, pr6651); inline items strengthen through `X.X`.
      std::string hd;
      std::vector<std::string> rest;
      if (auto* l = std::get_if<Lident>(&bi->id.txt.v)) hd = l->name;
      else if (!std::holds_alternative<Lapply>(bi->id.txt.v)) {
        rest = split_dotted(lid_full(bi->id.txt));
        hd = rest.front();
        rest.erase(rest.begin());
      }
      for (auto& p : ps)
        if (!hd.empty() && p.name == hd) {
          std::vector<cmi::cmiw::SigItem> psig;
          if (!p.sig.empty())
            psig = p.sig;
          else if (!p.ref.empty()) {
            if (p.ref.find('.') == std::string::npos) {
              if (ckp)
                if (auto a = ckp->modtype_sig_asts_.find(p.ref);
                    a != ckp->modtype_sig_asts_.end())
                  psig = signature_to_cmi(*a->second);
            } else {
              psig = cmi_modtype_items(split_dotted(p.ref));
            }
          }
          if (rest.empty()) {
            result = std::move(psig);
            strengthen_abstract(result, p.name);
          } else {
            // descend the param's sig through the dotted components
            std::string path = p.name;
            const std::vector<cmi::cmiw::SigItem>* cur_items = &psig;
            const cmi::cmiw::SigItem* found = nullptr;
            for (std::size_t i = 0; i < rest.size() && cur_items; ++i) {
              found = nullptr;
              for (auto& si : *cur_items)
                if (si.k == cmi::cmiw::SigItem::Module && si.name == rest[i]) {
                  found = &si; break;
                }
              if (!found) break;
              path += "." + rest[i];
              cur_items = &found->sub;
            }
            if (found) {
              if (!found->modtype_ref.empty() &&
                  found->modtype_ref.find('.') == std::string::npos)
                // a BARE ref names a sibling inside the param's sig: X.T
                result_ref = p.name + "." + found->modtype_ref;
              result = found->sub;
              strengthen_abstract(result, path);
            }
          }
          break;
        }
    }
    else if (auto* bc = std::get_if<Pmod_constraint>(&cur->desc)) {
      if (bc->mt) {
        if (auto* psg = std::get_if<Pmty_signature>(&bc->mt->desc))
          result = signature_to_cmi(psg->items);
        else if (auto* rid = std::get_if<Pmty_ident>(&bc->mt->desc)) {
          // `module F () : Ret = struct .. end`: Mty_ident(Ret) result; the
          // local modtype's items stay the fallback layout.
          result_ref = lid_full(rid->id.txt);
          if (ckp && result_ref.find('.') == std::string::npos) {
            if (auto q = ckp->opened_modtype_quals_.find(result_ref);
                q != ckp->opened_modtype_quals_.end())
              result_ref = q->second;
            else if (auto a = ckp->modtype_sig_asts_.find(result_ref);
                     a != ckp->modtype_sig_asts_.end())
              result = signature_to_cmi(*a->second);
          }
        }
        else if (std::holds_alternative<Pmty_with>(bc->mt->desc)) {
          // `module Apply (Arg : ..) : S with type elt = Arg.t = struct ..`:
          // resolve S and graft the refinements (ocamlc stores them expanded).
          std::string wref;
          annot_modtype_items(ckp, *bc->mt, wref, result);
        }
      }
    }
    auto item = cmi::cmiw::sig_module_functor(name, ps[0].name,
                                              std::move(ps[0].sig),
                                              std::move(result));
    item.functor_result_ref = std::move(result_ref);
    item.functor_unit = ps[0].unit;
    item.functor_param_ref = std::move(ps[0].ref);
    item.param_functor = std::move(ps[0].fsig);
    for (std::size_t i = 1; i < ps.size(); ++i) {
      item.more_param_names.push_back(std::move(ps[i].name));
      item.more_param_sigs.push_back(std::move(ps[i].sig));
      item.more_param_units.push_back(ps[i].unit ? 1 : 0);
      item.more_param_refs.push_back(std::move(ps[i].ref));
    }
    return item;
  }
  if (std::holds_alternative<Pmod_apply>(me.desc) ||
      std::holds_alternative<Pmod_apply_unit>(me.desc)) {
    // Peel the (possibly curried) application chain: F(A)(B)() -> head F,
    // args [A, B, null] (null = a generative `()` application).
    std::vector<const ast::ModuleExpr*> args;
    const ast::ModuleExpr* h = &me;
    while (true) {
      if (auto* a = std::get_if<Pmod_apply>(&h->desc)) {
        args.push_back(a->arg.get()); h = a->f.get();
      } else if (auto* au = std::get_if<Pmod_apply_unit>(&h->desc)) {
        args.push_back(nullptr); h = au->f.get();
      } else break;
    }
    std::reverse(args.begin(), args.end());
    auto* fi = std::get_if<Pmod_ident>(&h->desc);
    if (!fi || std::holds_alternative<Lapply>(fi->id.txt.v)) return std::nullopt;
    std::string fpath = lid_full(fi->id.txt);
    std::vector<std::string> comps;
    for (std::size_t p = 0, d; p <= fpath.size(); p = d + 1) {
      d = fpath.find('.', p);
      if (d == std::string::npos) d = fpath.size();
      comps.push_back(fpath.substr(p, d - p));
    }
    // Resolve the functor: one param name per application level + result items.
    std::vector<std::string> pnames;
    std::vector<cmi::cmiw::SigItem> result;
    std::string result_ref;  // a local functor's Mty_ident result (`: Priv`)
    bool resolved = false;
    // Chase local ALIAS bindings first (`module F' = F; module C = F'(A)`):
    // a bare alias item redirects the head to its target (index_aliases).
    for (int hop = 0; hop < 4 && comps.size() == 1; ++hop) {
      bool changed = false;
      for (auto* scope : {prior, g_outer_prior}) {
        if (!scope || changed) continue;
        for (auto& si : *scope)
          if (si.k == cmi::cmiw::SigItem::Module && !si.is_functor &&
              si.name == comps[0] && !si.alias.empty() &&
              si.alias.find('.') == std::string::npos) {
            comps[0] = si.alias;
            changed = true;
            break;
          }
      }
      if (!changed) break;
    }
    if (comps.size() == 1) {  // a local functor emitted earlier
      // An EMPTY result sig is legitimate (`module F (A : ..) = struct let _ =
      // .. end; module B = F(X)` -- ocamlc emits `module B : sig end`);
      // requiring items here dropped B entirely (a layout shift, B takes a
      // runtime field).  Inside a functor body, a sibling functor from the
      // ENCLOSING scope (g_outer_prior) resolves too.
      for (auto* scope : {prior, g_outer_prior}) {
        if (!scope || resolved) continue;
        for (auto& si : *scope)
          if (si.k == cmi::cmiw::SigItem::Module && si.is_functor &&
              si.name == comps[0] &&
              args.size() == 1 + si.more_param_names.size()) {
            pnames.push_back(si.functor_param);
            for (auto& n : si.more_param_names) pnames.push_back(n);
            result = si.sub;  // fresh item; subst clones every node it touches
            result_ref = si.functor_result_ref;
            resolved = true;
            break;
          }
      }
    }
    // A DOTTED head whose module chain is LOCAL (`module StrM =
    // Msg.Define(..)` with Msg bound above): descend the already-emitted
    // items to the functor.  Bare refs in its result that name the enclosing
    // module's members requalify through it (`tag` -> `Msg.tag`) -- outside
    // Msg they no longer resolve locally.
    if (!resolved && comps.size() >= 2) {
      // Keeps sigs MATERIALIZED during the descent alive (a chain component
      // held as a named modtype ref -- `module X : S` with the functor F
      // declared inside S -- has no inline sub; resolve the ref instead).
      std::vector<std::unique_ptr<std::vector<cmi::cmiw::SigItem>>> mat_store;
      for (auto* scope : {prior, g_outer_prior}) {
        if (!scope || resolved) continue;
        const std::vector<cmi::cmiw::SigItem>* cur = scope;
        for (std::size_t i = 0; i + 1 < comps.size() && cur; ++i) {
          const std::vector<cmi::cmiw::SigItem>* next = nullptr;
          for (auto& si : *cur)
            if (si.k == cmi::cmiw::SigItem::Module && !si.is_functor &&
                si.name == comps[i]) {
              if (!si.sub.empty()) { next = &si.sub; break; }
              if (!si.modtype_ref.empty()) {
                std::vector<cmi::cmiw::SigItem> m;
                if (si.modtype_ref.find('.') == std::string::npos) {
                  if (ckp)
                    if (auto a = ckp->modtype_sig_asts_.find(si.modtype_ref);
                        a != ckp->modtype_sig_asts_.end())
                      m = signature_to_cmi(*a->second);
                } else {
                  m = cmi_modtype_items(split_dotted(si.modtype_ref));
                }
                if (!m.empty()) {
                  mat_store.push_back(
                      std::make_unique<std::vector<cmi::cmiw::SigItem>>(
                          std::move(m)));
                  next = mat_store.back().get();
                }
              }
              break;
            }
          cur = next;
        }
        if (!cur) continue;
        for (auto& si : *cur)
          if (si.k == cmi::cmiw::SigItem::Module && si.is_functor &&
              si.name == comps.back() &&
              args.size() == 1 + si.more_param_names.size()) {
            pnames.push_back(si.functor_param);
            for (auto& n : si.more_param_names) pnames.push_back(n);
            result = si.sub;
            result_ref = si.functor_result_ref;
            {  // unshare before the in-place qual rewrite below
              std::unordered_map<const cmi::cmiw::Ty*, cmi::cmiw::TyPtr> memo;
              items_deep_copy_tys(result, memo);
            }
            std::string prefix = comps[0];
            for (std::size_t i = 1; i + 1 < comps.size(); ++i)
              prefix += "." + comps[i];
            std::set<std::string> encl, own;
            for (auto& x : *cur)
              if (x.k == cmi::cmiw::SigItem::Type ||
                  x.k == cmi::cmiw::SigItem::Module) encl.insert(x.name);
            for (auto& x : result)
              if (x.k == cmi::cmiw::SigItem::Type ||
                  x.k == cmi::cmiw::SigItem::Module) own.insert(x.name);
            auto qual = [&](std::string& n) {
              std::string head = n.substr(0, n.find('.'));
              if (encl.count(head) && !own.count(head)) n = prefix + "." + n;
            };
            rewrite_item_ty_names(result, qual);
            for (auto& x : result)
              if (x.k == cmi::cmiw::SigItem::Exception && !x.ext_path.empty())
                qual(x.ext_path);
            resolved = true;
            break;
          }
      }
    }
    // An opened module's submodule head (`open MoreLabels` then `Map.Make`):
    // requalify so the LABELLED Map/Set/Hashtbl is the one loaded.
    if (!resolved && ckp)
      if (auto q = ckp->opened_submod_quals_.find(comps[0]);
          q != ckp->opened_submod_quals_.end()) {
        std::vector<std::string> qc;
        for (std::size_t p = 0, d; p <= q->second.size(); p = d + 1) {
          d = q->second.find('.', p);
          if (d == std::string::npos) d = q->second.size();
          qc.push_back(q->second.substr(p, d - p));
        }
        qc.insert(qc.end(), comps.begin() + 1, comps.end());
        comps = std::move(qc);
      }
    bool stdlib_unit_head = false;  // strengthen with the mangled unit head
    // Remaining parameters of a PARTIAL application (empty = fully applied).
    struct PartParam { std::string name, ref;
                       std::vector<cmi::cmiw::SigItem> sig; bool unit = false; };
    std::vector<PartParam> part_params;
    if (!resolved && comps.size() >= 2) {  // a functor from a compiled .cmi
      try {
        std::string hc = head_cmi(comps[0]);
        std::string base = hc.substr(hc.rfind('/') + 1);
        stdlib_unit_head = base.rfind("stdlib__", 0) == 0;
        const auto& cmif = cmi::CmiFile::load(hc);
        const cmi::Signature* sig = &cmif.sig();
        const cmi::ModuleType* mt = nullptr;
        for (std::size_t i = 1; i < comps.size(); ++i) {
          if (!sig) { mt = nullptr; break; }
          const cmi::ModuleDecl* md = nullptr;
          for (auto& mm : sig->modules)
            if (mm.name == comps[i]) { md = &mm; break; }
          if (!md || !md->type) { mt = nullptr; break; }
          mt = md->type.get();
          sig = mt->kind == cmi::ModuleType::Sig ? mt->sig.get() : nullptr;
        }
        const cmi::ModuleType* cur = mt;
        for (std::size_t i = 0; i < args.size() && cur; ++i) {
          if (cur->kind != cmi::ModuleType::Functor) { cur = nullptr; break; }
          pnames.push_back(cur->functor_param.value_or(""));
          cur = cur->functor_body.get();
        }
        // Splice origin: the functor's PARENT module path (bare same-unit
        // modtype refs requalify as Opt.Config / Outcome.Allow).
        std::string origin;
        for (std::size_t i = 0; i + 1 < comps.size(); ++i)
          origin += (i ? "." : "") + comps[i];
        // The enclosing unit signature + its real path, so a functor RESULT's
        // bare refs to a unit-level type (Hashtbl's `statistics`) requalify --
        // see qualify_enclosing_types (MakeSeeded(SS)'s `stats : .. -> 'b`).
        const cmi::Signature* encl = &cmif.sig();
        std::string unit_path = cmif.module_name();
        for (std::size_t i = 1; i + 1 < comps.size() && encl; ++i) {
          const cmi::ModuleDecl* emd = nullptr;
          for (auto& mm : encl->modules)
            if (mm.name == comps[i]) { emd = &mm; break; }
          encl = (emd && emd->type && emd->type->kind == cmi::ModuleType::Sig)
                     ? emd->type->sig.get() : nullptr;
          unit_path += "." + comps[i];
        }
        if (cur && cur->kind == cmi::ModuleType::Sig && cur->sig) {
          result = cmi_sig_to_items(*cur->sig, origin);
          if (encl) qualify_enclosing_types(result, *encl, unit_path);
          resolved = true;
        } else if (cur && cur->kind == cmi::ModuleType::Functor) {
          // PARTIAL application (`Outcome.Make(IntT)(IntT)` of a 4-param
          // functor): the binding is itself a functor over the remaining
          // parameters, its result the final signature.
          const cmi::ModuleType* w = cur;
          while (w && w->kind == cmi::ModuleType::Functor) {
            PartParam r;
            r.unit = w->functor_unit;
            if (w->functor_param) r.name = *w->functor_param;
            if (w->functor_param_type) {
              if (w->functor_param_type->kind == cmi::ModuleType::Ident &&
                  w->functor_param_type->path) {
                r.ref = bare_cmi_path(*w->functor_param_type->path);
                if (!origin.empty() && r.ref.find('.') == std::string::npos)
                  r.ref = origin + "." + r.ref;
              } else if (w->functor_param_type->kind == cmi::ModuleType::Sig &&
                         w->functor_param_type->sig) {
                r.sig = cmi_sig_to_items(*w->functor_param_type->sig, origin);
              }
            }
            part_params.push_back(std::move(r));
            w = w->functor_body.get();
          }
          if (w && w->kind == cmi::ModuleType::Sig && w->sig) {
            result = cmi_sig_to_items(*w->sig, origin);
            if (encl) qualify_enclosing_types(result, *encl, unit_path);
            resolved = true;
          } else {
            part_params.clear();
          }
        }
      } catch (...) {}
    }
    if (!resolved) return std::nullopt;
    std::vector<FunctorArgSubst> subs;
    for (std::size_t i = 0; i < args.size() && i < pnames.size(); ++i) {
      FunctorArgSubst s;
      s.param = pnames[i];
      const ast::ModuleExpr* am = args[i];
      while (am) {
        if (auto* amc = std::get_if<Pmod_constraint>(&am->desc)) am = amc->me.get();
        else break;
      }
      if (am && !s.param.empty()) {
        if (auto* api = std::get_if<Pmod_ident>(&am->desc)) {
          if (!std::holds_alternative<Lapply>(api->id.txt.v)) {
            s.arg_path = lid_full(api->id.txt);
            // An opened submodule argument cites its qualified path
            // (`open Globroots` + Test(Classic) -> Globroots.Classic).
            if (ckp) {
              std::string head = s.arg_path.substr(0, s.arg_path.find('.'));
              if (auto q = ckp->opened_submod_quals_.find(head);
                  q != ckp->opened_submod_quals_.end())
                s.arg_path = q->second + s.arg_path.substr(head.size());
            }
          }
        } else if (auto* ast_ = std::get_if<Pmod_structure>(&am->desc)) {
          // An anonymous struct argument: its manifests eliminate the
          // parameter (`Ord.t` -> `int`).  Arity-0 substitution, plus
          // PHANTOM decls (manifest == a param: `'a t = 'a`) which
          // substitute an application to its argument.
          for (auto& ai : infer_signature(ast_->items))
            if (ai.k == cmi::cmiw::SigItem::Type && ai.manifest) {
              if (ai.params.empty()) {
                s.manifests[ai.name] = ai.manifest;
              } else {
                for (std::size_t pi = 0; pi < ai.params.size(); ++pi)
                  if (ai.params[pi].get() == ai.manifest.get()) {
                    s.phantoms[ai.name] = (int)pi;
                    break;
                  }
              }
            }
        }
      }
      if (!s.param.empty() &&
          (!s.arg_path.empty() || !s.manifests.empty() || !s.phantoms.empty()))
        subs.push_back(std::move(s));
    }
    // Build the bound item: a plain module, or (partial application) a
    // functor over the remaining parameters.
    cmi::cmiw::SigItem item;
    if (part_params.empty()) {
      item = cmi::cmiw::sig_module(name, std::move(result));
      // `module A = Make(..)` where Make's result is a NAMED modtype (`: Priv`)
      // that doesn't cite a parameter: ocamlc keeps Mty_ident(Priv) as A's
      // modtype; the substituted items stay the fallback layout.
      if (!result_ref.empty()) {
        std::string head = result_ref.substr(0, result_ref.find('.'));
        bool param_rel = false;
        for (auto& pn : pnames) if (!pn.empty() && pn == head) param_rel = true;
        if (!param_rel) item.modtype_ref = result_ref;
      }
    } else {
      item = cmi::cmiw::sig_module_functor(name, part_params[0].name,
                                           std::move(part_params[0].sig),
                                           std::move(result));
      item.functor_unit = part_params[0].unit;
      item.functor_param_ref = part_params[0].ref;
      for (std::size_t i = 1; i < part_params.size(); ++i) {
        item.more_param_names.push_back(std::move(part_params[i].name));
        item.more_param_sigs.push_back(std::move(part_params[i].sig));
        item.more_param_units.push_back(part_params[i].unit ? 1 : 0);
        item.more_param_refs.push_back(std::move(part_params[i].ref));
      }
    }
    if (!subs.empty()) {  // substitute through result AND remaining param sigs
      std::vector<cmi::cmiw::SigItem> tmp;
      tmp.push_back(std::move(item));
      subst_param_items(tmp, subs);
      item = std::move(tmp[0]);
    }
    // All-path arguments (no anonymous struct, no generative `()`): ocamlc
    // strengthens the result's abstract types with the applied-path manifest.
    bool all_paths = !args.empty();
    std::vector<std::string> argpaths;
    for (auto* a : args) {
      const ast::ModuleExpr* am = a;
      while (am) {
        if (auto* amc = std::get_if<Pmod_constraint>(&am->desc)) am = amc->me.get();
        else break;
      }
      auto* api = am ? std::get_if<Pmod_ident>(&am->desc) : nullptr;
      if (api && !std::holds_alternative<Lapply>(api->id.txt.v)) {
        std::string ap = lid_full(api->id.txt);
        if (ckp) {  // opened submodule argument -> qualified path
          std::string head = ap.substr(0, ap.find('.'));
          if (auto q = ckp->opened_submod_quals_.find(head);
              q != ckp->opened_submod_quals_.end())
            ap = q->second + ap.substr(head.size());
        }
        argpaths.push_back(std::move(ap));
      } else { all_paths = false; break; }
    }
    // A partial application strengthens with the remaining params applied too
    // (`Outcome.Make(IntT)(IntT)(N)(A).t`) -- only when they're all named.
    for (std::size_t i = 0; i < item.more_param_names.size() + 1 && all_paths &&
                            item.is_functor; ++i) {
      const std::string& pn = i == 0 ? item.functor_param
                                     : item.more_param_names[i - 1];
      bool un = i == 0 ? item.functor_unit
                       : (i - 1 < item.more_param_units.size() &&
                          item.more_param_units[i - 1]);
      if (un || pn.empty()) all_paths = false;
    }
    if (all_paths) {
      // ocamlc's strengthening cites the RAW compilation unit
      // (Stdlib__Set.Make(X).t) -- unlike a source-written path, which keeps
      // the pervasive alias form.  The writer keys the raw form off the
      // explicit Stdlib__ prefix.
      std::string app;
      for (auto& cp : comps) { if (!app.empty()) app += '.'; app += cp; }
      if (stdlib_unit_head) app = "Stdlib__" + app;
      for (auto& ap : argpaths) app += "(" + ap + ")";
      if (item.is_functor) {
        app += "(" + item.functor_param + ")";
        for (auto& pn : item.more_param_names) app += "(" + pn + ")";
      }
      strengthen_abstract(item.sub, app);
    }
    return item;
  }
  return std::nullopt;
}

// Does object type `r` appear within its own method args (a recursive self
// type, as produced by a `method m = {< >}`)?  Such a method's type IS the
// class' self, which the writer emits as the shared csig_self node.
static bool object_cites_self(const TypePtr& t) {
  TypePtr r = I::Engine::repr(t);
  if (!r || r->kind != I::Type::Kind::Object) return false;
  std::unordered_set<const I::Type*> seen;
  std::function<bool(const TypePtr&)> dfs = [&](const TypePtr& x) -> bool {
    TypePtr xr = I::Engine::repr(x);
    if (!xr) return false;
    if (xr.get() == r.get()) return true;
    if (!seen.insert(xr.get()).second) return false;
    for (auto& a : xr->args) if (dfs(a)) return true;
    if (xr->dom && dfs(xr->dom)) return true;
    if (xr->cod && dfs(xr->cod)) return true;
    return false;
  };
  for (auto& a : r->args) if (dfs(a)) return true;
  return false;
}

std::vector<cmi::cmiw::SigItem> infer_signature(
    const ast::Structure& s,
    const std::vector<std::pair<std::string, const ast::ModuleType*>>* fparams) {
  Checker ck;
  ck.record_kinds_ = true;
  // Mirror the sig-display pass: ocamlc's .cmi stores SOURCE abbreviations
  // (Scanf.Scanning.in_channel, not its expansion), which only the folded
  // pass keeps; folding skips from_cmi abbreviation expansion, so unification
  // must be best-effort like the display pass (see infer_structure_types).
  ck.eng.lenient = true;
  ck.fold_abbrevs_ = true;
  // Enclosing-scope opens (this file is a re-inferred submodule): replay them so
  // its value inference matches the main pass, which typed it with those opens
  // in scope.  `active` accumulates them plus this level's own opens, for
  // deeper submodules.
  std::vector<const ast::Longident*> active;
  if (g_inherited_opens) active = *g_inherited_opens;
  ck.replay_opens_ = active;
  // Enclosing functor parameters: bind each param's value members with their
  // REAL declared types (same registration the main pass does at its functor
  // harvest), so body exports don't degrade to fresh vars.  The outer file's
  // `module type` ASTs come along (g_outer_modtype_asts) so a `(H : S)` with a
  // LOCAL S resolves -- the body structure itself doesn't contain S.
  // Enclosing-scope module exports: seed them so a submodule body's reference
  // to an outer local module (opened, aliased with `let module`, or dotted)
  // resolves instead of degrading to a fresh var.  emplace, not assignment:
  // run_checker's own (re-)bindings overwrite local names later anyway, and
  // functor params seeded below must win over an outer module of the same name.
  if (g_outer_modenv)
    for (auto& [n, ex] : *g_outer_modenv) ck.modenv.emplace(n, ex);
  if (g_outer_venv && !g_outer_venv->empty() && !ck.venv.empty())
    for (auto& [n, v] : g_outer_venv->front()) ck.venv.front().emplace(n, v);
  if (g_outer_cenv && !g_outer_cenv->empty() && !ck.cenv.empty())
    for (auto& [n, v] : g_outer_cenv->front()) ck.cenv.front().emplace(n, v);
  if (fparams) {
    if (g_outer_modtype_asts) ck.modtype_sig_asts_ = *g_outer_modtype_asts;
    if (g_outer_modtype_quals) ck.opened_modtype_quals_ = *g_outer_modtype_quals;
    for (auto& [pn, psig] : *fparams) {
      ck.bound_module_names_.insert(pn);
      if (psig) {
        ck.modenv[pn] = ck.param_sig_value_schemes(*psig, {}, pn);
        ck.register_param_sig_members(pn, *psig);
      }
    }
  }
  run_checker(ck, s);  // leaves top-level bindings in venv.back()
  // Expose this file's modtype ASTs / opened-modtype quals to nested
  // functor-body inference (saved / restored: infer_signature recurses
  // through submodule structures).
  auto* saved_mt_asts = g_outer_modtype_asts;
  auto* saved_mt_quals = g_outer_modtype_quals;
  auto* saved_enclosing = g_enclosing_struct_items;
  auto* saved_modenv = g_outer_modenv;
  auto* saved_venv = g_outer_venv;
  auto* saved_cenv = g_outer_cenv;
  g_outer_modtype_asts = &ck.modtype_sig_asts_;
  g_outer_modtype_quals = &ck.opened_modtype_quals_;
  g_outer_modenv = &ck.modenv;
  g_outer_venv = &ck.venv;
  g_outer_cenv = &ck.cenv;
  // Emission phase: checking is DONE, every from_coretype below only converts
  // declaration types for the .cmi -- keep local abbreviations as written
  // (`startDate : (int, message) fieldStatus` stores `message`, not string).
  ck.keep_local_abbrevs_ = true;
  // An expression-local module escaping into an inferred val type is stored
  // by ocamlc as its definition path with RAW unit heads (`let module N =
  // Map.Make(S) in .. : int N.t` -> int Stdlib__Map.Make(Stdlib__String).t;
  // the display pass alias-routes it back).  A name also bound by a
  // top-level module stays in scope and is NOT rewritten (pr6944).
  std::unordered_map<std::string, std::string> local_mod_defs;
  {
    std::set<std::string> toplevel_mods;
    for (auto& it : s)
      if (auto* mb = std::get_if<Pstr_module>(&it.desc))
        if (mb->binding.name.txt) toplevel_mods.insert(*mb->binding.name.txt);
    auto stdlib_unit = [](const std::string& m) {
      if (m.empty() || m.rfind("Stdlib__", 0) == 0) return false;
      std::string hc = head_cmi(m);
      std::string base = hc.substr(hc.rfind('/') + 1);
      return base.rfind("stdlib__", 0) == 0 && std::filesystem::exists(hc);
    };
    auto raw_applied = [&](const std::string& p) {
      std::string r;
      for (std::size_t i = 0; i < p.size();) {
        if (i == 0 || p[i - 1] == '(') {  // a path head: start or functor arg
          std::size_t j = i;
          while (j < p.size() &&
                 (std::isalnum((unsigned char)p[j]) || p[j] == '_')) ++j;
          std::string id = p.substr(i, j - i);
          r += stdlib_unit(id) ? "Stdlib__" + id : id;
          i = j;
        } else {
          r += p[i++];
        }
      }
      return r;
    };
    for (auto& [nm, path] : ck.local_module_paths_)
      if (!path.empty() && path.find('(') != std::string::npos &&
          !toplevel_mods.count(nm))
        local_mod_defs[nm] = raw_applied(path);
  }
  std::vector<cmi::cmiw::SigItem> out;
  // `include <functor param>` re-exports the param's types locally
  // (`type t = Analysis.t`): later inferred vals citing the param-qualified
  // path are rewritten to the LOCAL re-export, like ocamlc's strengthened
  // include (pr7601's Make).
  std::unordered_map<std::string, std::string> include_requal;
  g_enclosing_struct_items = &out;  // sig-side `module type of <local module>`
  // A submodule emitted below is re-inferred with a fresh checker; hand it the
  // opens active here (inherited + this level's, accumulated in source order).
  auto* saved_inherited_opens = g_inherited_opens;
  g_inherited_opens = &active;
  // `include <compilation unit>` splices, recorded for the end-of-emission
  // requalification pass: (emitted size after the splice, unit name as
  // written, the spliced top-level type names).
  std::vector<std::tuple<std::size_t, std::string, std::set<std::string>>>
      unit_includes;
  for (auto& it : s) {
    if (auto* op = std::get_if<Pstr_open>(&it.desc)) {
      if (auto* pi = std::get_if<Pmod_ident>(&op->expr.desc);
          pi && !std::holds_alternative<Lapply>(pi->id.txt.v))
        active.push_back(&pi->id.txt);
    }
    if (auto* sv = std::get_if<Pstr_value>(&it.desc)) {
      for (auto& b : sv->bindings) {
        std::vector<std::string> names;
        toplevel_pat_vars(b.pat, names);  // every binder, incl. destructuring lets
        for (auto& nm : names) {
          auto f = ck.venv.back().find(nm);
          if (f == ck.venv.back().end()) continue;
          std::unordered_map<const I::Type*, int> vars; int nextvar = 0;
          if (getenv("BRIDGEDBG")) fprintf(stderr, "[bridge-val] %s\n", nm.c_str());
          auto ty = bridge_ty(f->second, vars, nextvar);
          if (!local_mod_defs.empty())
            rewrite_ty_names(ty, [&](std::string& n) {
              std::string head = n.substr(0, n.find('.'));
              if (auto d = local_mod_defs.find(head); d != local_mod_defs.end())
                n = d->second + n.substr(head.size());
            });
          if (!include_requal.empty())
            rewrite_ty_names(ty, [&](std::string& n) {
              if (auto q = include_requal.find(n); q != include_requal.end())
                n = q->second;
            });
          out.push_back(cmi::cmiw::sig_value(nm, std::move(ty)));
        }
      }
    } else if (auto* pr = std::get_if<Pstr_primitive>(&it.desc)) {
      // `external f : t = "prim"`: a Val_prim value (typed from the annotation).
      if (pr->prim.type && !pr->prim.prims.empty()) {
        std::unordered_map<std::string, TypePtr> tvars;
        std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
        auto ty = bridge_ty_named(ck.from_coretype(*pr->prim.type, tvars), bvars, nextvar, tvars);
        std::string native = pr->prim.prims.size() > 1 ? pr->prim.prims[1] : "";
        auto item = cmi::cmiw::sig_external(pr->prim.name.txt, ty, pr->prim.prims[0], native);
        apply_prim_attrs(pr->prim, out, item);
        out.push_back(std::move(item));
      } else if (pr->prim.alias) {  // `external f [: t] = g`: copy g's primitive
        if (auto item = emit_prim_alias(ck, pr->prim, out)) out.push_back(std::move(*item));
      }
    } else if (auto* ty = std::get_if<Pstr_type>(&it.desc)) {
      emit_type_decls(ck, ty->decls, out, ty->rf == RecFlag::Nonrecursive);
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
      for (auto& ec : px->ext.ctors) {
        if (ec.name.txt.empty()) continue;
        if (auto* pd = std::get_if<Pext_decl>(&ec.kind)) {
          out.push_back(exn_sigitem(ck, ec.name.txt, *pd, &px->ext, first));
          first = false;
        } else if (auto* rb = std::get_if<Pext_rebind>(&ec.kind)) {
          // `type 'a Msg.tag += String = StrM.C`: clone the target ctor's
          // typext item (args/return travel along) under the new name.
          auto comps = split_dotted(lid_full(rb->id.txt));
          const std::vector<cmi::cmiw::SigItem>* cur = &out;
          for (std::size_t i = 0; i + 1 < comps.size() && cur; ++i) {
            const std::vector<cmi::cmiw::SigItem>* next = nullptr;
            for (auto& si : *cur)
              if (si.k == cmi::cmiw::SigItem::Module && !si.is_functor &&
                  si.name == comps[i]) { next = &si.sub; break; }
            cur = next;
          }
          if (!cur) continue;
          for (auto& si : *cur)
            if (si.k == cmi::cmiw::SigItem::Exception &&
                si.name == comps.back()) {
              cmi::cmiw::SigItem ni = si;
              std::vector<cmi::cmiw::SigItem> one{std::move(ni)};
              std::unordered_map<const cmi::cmiw::Ty*, cmi::cmiw::TyPtr> memo;
              items_deep_copy_tys(one, memo);
              one[0].name = ec.name.txt;
              one[0].ext_path = lid_full(px->ext.path.txt);
              one[0].ext_params = typext_param_names(px->ext);
              one[0].text_kind = first ? 0 : 1;
              out.push_back(std::move(one[0]));
              first = false;
              break;
            }
        }
      }
    } else if (auto* pmt = std::get_if<Pstr_modtype>(&it.desc)) {
      // `module type S = sig .. end` in a .ml (no .mli): emit Sig_modtype so the
      // inferred .cmi carries it (a modtype takes no runtime field, so it never
      // shifts the value layout).
      // Resolve a modtype BODY to items: a literal signature; a bare ident
      // through an earlier Modtype item (covers `= module type of ..` bodies
      // annot_modtype_items can't see) or the checker's signature ASTs; a
      // `module type of M` through M's already-emitted item or a literal
      // struct; `S with ..` resolves the base then grafts.
      std::function<std::optional<std::vector<cmi::cmiw::SigItem>>(
          const ast::ModuleType&)> mt_items =
          [&](const ast::ModuleType& mt)
              -> std::optional<std::vector<cmi::cmiw::SigItem>> {
        if (auto* ps = std::get_if<Pmty_signature>(&mt.desc))
          return signature_to_cmi(ps->items);
        if (auto* pid = std::get_if<Pmty_ident>(&mt.desc)) {
          if (auto* l = std::get_if<Lident>(&pid->id.txt.v)) {
            for (auto& si : out)
              if (si.k == cmi::cmiw::SigItem::Modtype && si.name == l->name &&
                  !si.modtype_abstract)
                return si.sub;
            if (auto a = ck.modtype_sig_asts_.find(l->name);
                a != ck.modtype_sig_asts_.end())
              return signature_to_cmi(*a->second);
          }
          return std::nullopt;
        }
        if (auto* pt = std::get_if<Pmty_typeof>(&mt.desc)) {
          if (auto* ms = std::get_if<Pmod_structure>(&pt->me->desc))
            return infer_signature(ms->items);
          if (auto* mi = std::get_if<Pmod_ident>(&pt->me->desc))
            if (auto* l = std::get_if<Lident>(&mi->id.txt.v))
              for (auto& si : out)
                if (si.k == cmi::cmiw::SigItem::Module && si.name == l->name &&
                    !si.is_functor && si.alias.empty())
                  return si.sub;
          return std::nullopt;
        }
        if (auto* pw = std::get_if<Pmty_with>(&mt.desc)) {
          auto base = mt_items(*pw->mt);
          if (!base) return std::nullopt;
          apply_with_constraints(ck, mt, *base);
          drop_modsubst(*base, with_modsubst_names(mt));
          return base;
        }
        return std::nullopt;
      };
      if (!pmt->type)  // ABSTRACT `module type S` in a struct (pr7112)
        out.push_back(cmi::cmiw::sig_modtype_abstract(pmt->name.txt));
      else if (auto* ps = std::get_if<Pmty_signature>(&pmt->type->desc))
        // Earlier top-level modtypes travel along so a nested `FOO with ..`
        // (e.g. a `module rec` decl's annotation, pr7082) resolves FOO.
        out.push_back(cmi::cmiw::sig_modtype(
            pmt->name.txt, signature_to_cmi(ps->items, &ck.modtype_sig_asts_)));
      else if (auto* pid = std::get_if<Pmty_ident>(&pmt->type->desc)) {
        // `module type S2 = S1` / `= M.T` in a struct: an ALIAS -- mtd_type
        // stays Mty_ident; the resolved items are the fallback layout.
        if (!std::holds_alternative<Lapply>(pid->id.txt.v)) {
          std::string ref;
          std::vector<cmi::cmiw::SigItem> sub;
          annot_modtype_items(&ck, *pmt->type, ref, sub);
          if (sub.empty())
            if (auto r = mt_items(*pmt->type)) sub = std::move(*r);
          auto s = cmi::cmiw::sig_modtype(pmt->name.txt, std::move(sub));
          s.modtype_ref = std::move(ref);
          out.push_back(std::move(s));
        }
      } else if (std::holds_alternative<Pmty_typeof>(pmt->type->desc) ||
                 std::holds_alternative<Pmty_with>(pmt->type->desc)) {
        // `module type TFoo = module type of Foo` / `= TFoo with type u := ..`
        // (t02): resolved to the expanded signature (ocamlc stores it too).
        if (auto r = mt_items(*pmt->type))
          out.push_back(cmi::cmiw::sig_modtype(pmt->name.txt, std::move(*r)));
      }
    } else if (auto* pc = std::get_if<Pstr_class>(&it.desc)) {
      // `class c [params] = object .. end`: emit Sig_class (the writer adds
      // the two ghost companions).  Member TYPES come from the checker
      // (class_types_ / class_ctor_types_ hold the generalized object /
      // constructor arrow; class_instvars_ the vals); member FLAGS
      // (mutable/private/virtual) from the AST fields.  Type-parameterized
      // classes (['a] c) aren't representable yet -- skipped.
      int class_rs = 1;  // Trec_first, then Trec_next for the `and` members
      for (auto& d : pc->decls) {
        const ast::ClassExpr* ce = &d.expr;
        std::vector<const Pcl_fun*> cparams;
        // `class b : B.a = object .. end`: a NAMED class-type annotation is
        // stored by ocamlc as Cty_constr(B.a, args, inner) and printed
        // `class b : B.a` -- capture the name while unwrapping.
        std::string annot_ref;
        for (;;) {
          if (auto* pf = std::get_if<Pcl_fun>(&ce->desc)) { cparams.push_back(pf); ce = pf->body.get(); }
          else if (auto* pl = std::get_if<Pcl_let>(&ce->desc)) ce = pl->body.get();
          else if (auto* pcn = std::get_if<Pcl_constraint>(&ce->desc)) {
            if (auto* cc = std::get_if<Pcty_constr>(&pcn->ct->desc))
              annot_ref = lid_full(cc->id.txt);
            ce = pcn->ce.get();
          }
          else if (auto* po = std::get_if<Pcl_open>(&ce->desc)) ce = po->body.get();
          else break;
        }
        auto* pst = std::get_if<Pcl_structure>(&ce->desc);
        if (!pst) {
          // An ALIAS class (`class c = with_param args`, recorded by the
          // checker): emit Cty_constr(target) with the target's methods as
          // the inner signature.
          auto ar = ck.class_alias_refs_.find(&d);
          if (ar == ck.class_alias_refs_.end()) { continue; }
          cmi::cmiw::SigItem ci;
          ci.k = cmi::cmiw::SigItem::Class;
          ci.name = d.name.txt;
          ci.rec_status = class_rs; class_rs = 2;
          ci.class_virtual = (d.virt == VirtualFlag::Virtual);
          ci.class_constr_ref = ar->second;
          std::unordered_map<const I::Type*, int> cvars; int cnext = 0;
          BridgeCtx cctx;
          if (auto f = ck.class_node_types_.find(&d); f != ck.class_node_types_.end()) {
            TypePtr obj = I::Engine::repr(f->second);
            if (obj->kind == I::Type::Kind::Object)
              for (std::size_t m = 0; m < obj->labels.size() && m < obj->args.size(); ++m) {
                cmi::cmiw::ClassField cf2;
                cf2.name = obj->labels[m];
                cf2.is_method = true;
                cf2.ty = bridge_ty_rec(obj->args[m], cvars, cnext, cctx);
                ci.class_fields.push_back(std::move(cf2));
              }
          }
          out.push_back(std::move(ci));
          continue;
        }
        cmi::cmiw::SigItem ci;
        ci.k = cmi::cmiw::SigItem::Class;
        ci.name = d.name.txt;
        ci.rec_status = class_rs; class_rs = 2;
        ci.class_virtual = (d.virt == VirtualFlag::Virtual);
        ci.class_constr_ref = annot_ref;
        std::unordered_map<const I::Type*, int> cvars; int cnext = 0;
        // ONE bridge context spans the whole class: fields citing the self
        // object (or any shared row) bridge to the SAME Ty node, so the
        // writer sharing (csig_self, `as 'a`) survives.
        BridgeCtx cctx;
        auto cbridge = [&](const TypePtr& t) {
          return bridge_ty_rec(t, cvars, cnext, cctx);
        };
        // The inferred self node maps to the writer csig_self.
        if (auto sf = ck.class_self_types_.find(&d);
            sf != ck.class_self_types_.end())
          ci.class_self = cbridge(sf->second);
        // constructor arrows + the final object type.  A paramless class lives
        // in class_types_; one with value OR type params in class_ctor_types_
        // (a `['a] c` with no value params still has a ctor scheme carrying its
        // tvars), so consult both.
        TypePtr ct;
        // The node-keyed entry is exact (a later shadowing `class c` inside an
        // `open struct` clobbers the flat name maps); fall back to the names.
        if (auto f = ck.class_node_types_.find(&d); f != ck.class_node_types_.end()) ct = f->second;
        else if (auto f = ck.class_types_.find(d.name.txt); f != ck.class_types_.end()) ct = f->second;
        else if (auto f = ck.class_ctor_types_.find(d.name.txt); f != ck.class_ctor_types_.end())
          ct = f->second;
        TypePtr obj = ct ? I::Engine::repr(ct) : nullptr;
        while (obj && obj->kind == I::Type::Kind::Arrow) {
          ci.class_arrow_doms.push_back(cbridge(obj->dom));
          ci.class_arrow_lks.push_back(obj->arrow_label);
          ci.class_arrow_lbls.push_back(obj->arrow_lbl);
          obj = I::Engine::repr(obj->cod);
        }
        // method name -> engine type (from the object row)
        std::unordered_map<std::string, TypePtr> mtypes;
        if (obj && obj->kind == I::Type::Kind::Object)
          for (std::size_t m = 0; m < obj->labels.size() && m < obj->args.size(); ++m)
            mtypes[obj->labels[m]] = obj->args[m];
        // `['a, _] c` type params: the checker stashes the class' tvars on the
        // inferred object's abbrev_args.  Bridge them FIRST (sharing cvars) so a
        // method's `'a` resolves to the same printed var as the class param.
        if (!d.params.empty() && obj && obj->kind == I::Type::Kind::Object &&
            obj->abbrev_args.size() == d.params.size())
          for (std::size_t pi = 0; pi < obj->abbrev_args.size(); ++pi) {
            auto pv = cbridge(obj->abbrev_args[pi]);
            // An anonymous param `_` is stored as Tvar(Some "_") so Printtyp
            // renders it `_` (`['a, _] c`) rather than naming it 'b.
            if (std::holds_alternative<Ptyp_any>(d.params[pi]->desc))
              pv->var_name = "_";
            ci.class_params.push_back(pv);
          }
        // `object (self : 'a)` or `(self : T as 'a)` where 'a is a class param:
        // the self type IS that param, so record its index -- the writer then
        // shares csig_self with cty_params[idx] and Printtyp shows `object ('a)
        // constraint 'a = <..>` (pr4766, pr5156).
        if (!ci.class_params.empty()) {
          const ast::Pattern* sp = &pst->cs.self;
          const ast::CoreType* sct = nullptr;
          while (auto* pcn = std::get_if<Ppat_constraint>(&sp->desc)) {
            sct = pcn->t.get();
            sp = pcn->p.get();
          }
          std::string selfvar;
          if (sct) {
            if (auto* pv = std::get_if<Ptyp_var>(&sct->desc)) selfvar = pv->name;
            else if (auto* pa = std::get_if<Ptyp_alias>(&sct->desc)) selfvar = pa->name;
          }
          if (!selfvar.empty())
            for (std::size_t pi = 0; pi < d.params.size(); ++pi)
              if (auto* pv = std::get_if<Ptyp_var>(&d.params[pi]->desc); pv && pv->name == selfvar)
                ci.class_self_param = (int)pi;
        }
        std::unordered_map<std::string, TypePtr> vtypes;
        if (auto f = ck.class_instvars_.find(d.name.txt); f != ck.class_instvars_.end())
          for (auto& [vn, vt] : f->second) vtypes[vn] = vt;
        std::unordered_set<std::string> own_meths, own_vals;
        for (auto& cf : pst->cs.fields) {
          if (auto* pv = std::get_if<Pcf_val>(&cf.desc)) {
            cmi::cmiw::ClassField f;
            f.name = pv->name.txt;
            f.mut = (pv->mut == MutableFlag::Mutable);
            if (auto* cv = std::get_if<Cfk_virtual>(&pv->kind)) {
              f.virt = true;
              std::unordered_map<std::string, TypePtr> tv;
              f.ty = cbridge(ck.from_coretype(*cv->type, tv));
            } else if (auto v = vtypes.find(f.name); v != vtypes.end()) {
              f.ty = cbridge(v->second);
            } else {
              f.ty = cmi::cmiw::ty_var(cnext++);
            }
            own_vals.insert(f.name);
            ci.class_fields.push_back(std::move(f));
          } else if (auto* pm = std::get_if<Pcf_method>(&cf.desc)) {
            cmi::cmiw::ClassField f;
            f.name = pm->name.txt;
            f.is_method = true;
            f.priv = (pm->priv == PrivateFlag::Private);
            if (auto* cv = std::get_if<Cfk_virtual>(&pm->kind)) {
              f.virt = true;
              std::unordered_map<std::string, TypePtr> tv;
              f.ty = cbridge(ck.from_coretype(*cv->type, tv));
            } else if (auto m = mtypes.find(f.name); m != mtypes.end()) {
              // `method m = {< >}` returns self: its type is the recursive self
              // object.  Flag it so the writer emits the shared csig_self node
              // (Printtyp then prints `object ('a) .. method m : 'a end`).
              f.self_ref = object_cites_self(m->second);
              f.ty = cbridge(m->second);
            } else {
              f.ty = cmi::cmiw::ty_var(cnext++);
            }
            own_meths.insert(f.name);
            ci.class_fields.push_back(std::move(f));
          }
        }
        // `inherit P [args]`: P's members are part of this class' signature too.
        // Splice the (already-emitted) parent SigItem's fields that this class
        // does not itself override (intext's `class bar = object inherit foo ..`
        // carries foo's data1..3 / test1..4).  Best-effort: only a same-signature
        // local parent, found by name in `out`.
        for (auto& cf : pst->cs.fields) {
          auto* inh = std::get_if<Pcf_inherit>(&cf.desc);
          if (!inh) continue;
          const ast::ClassExpr* pce = inh->ce.get();
          while (auto* ap = std::get_if<Pcl_apply>(&pce->desc)) pce = ap->ce.get();
          auto* pc = std::get_if<Pcl_constr>(&pce->desc);
          if (!pc) continue;
          std::string pname = lid_last(pc->id.txt);
          for (auto& pit : out) {
            if (pit.k != cmi::cmiw::SigItem::Class || pit.name != pname) continue;
            for (auto& pf : pit.class_fields) {
              auto& seen = pf.is_method ? own_meths : own_vals;
              if (!seen.insert(pf.name).second) continue;  // overridden / already have
              ci.class_fields.push_back(pf);  // copy the parent's typed field
            }
            break;
          }
        }
        out.push_back(std::move(ci));
      }
    } else if (auto* pct = std::get_if<Pstr_class_type>(&it.desc)) {
      // `class type ct = object .. end`: Sig_class_type + its ghost Sig_type.
      // Member types come straight from the written coretypes.
      int ct_rs = 1;
      for (auto& d : pct->decls) {
        auto* cs = std::get_if<Pcty_signature>(&d.expr.desc);
        if (!cs) continue;
        cmi::cmiw::SigItem ci;
        ci.k = cmi::cmiw::SigItem::Class;
        ci.class_is_type = true;
        ci.name = d.name.txt;
        ci.rec_status = ct_rs; ct_rs = 2;
        ci.class_virtual = (d.virt == VirtualFlag::Virtual);
        std::unordered_map<const I::Type*, int> cvars; int cnext = 0;
        std::unordered_map<std::string, TypePtr> tv;  // shared 'a across members
        // `['a] o2` class-type params: pre-bind each so members and cty_params
        // share the same tvar node (Printtyp names them consistently).
        std::vector<TypePtr> ctparams;
        for (auto& p : d.params) {
          TypePtr v = ck.eng.fresh_var();
          if (auto* pv = std::get_if<Ptyp_var>(&p->desc)) tv[pv->name] = v;
          ctparams.push_back(v);
        }
        bool ok = true;
        for (auto& cf : cs->cs.fields) {
          if (auto* pv = std::get_if<Pctf_val>(&cf.desc)) {
            cmi::cmiw::ClassField f;
            f.name = pv->name.txt;
            f.mut = (pv->mut == MutableFlag::Mutable);
            f.virt = (pv->virt == VirtualFlag::Virtual);
            f.ty = bridge_ty_named(ck.from_coretype(*pv->type, tv), cvars, cnext, tv);
            ci.class_fields.push_back(std::move(f));
          } else if (auto* pm = std::get_if<Pctf_method>(&cf.desc)) {
            cmi::cmiw::ClassField f;
            f.name = pm->name.txt;
            f.is_method = true;
            f.priv = (pm->priv == PrivateFlag::Private);
            f.virt = (pm->virt == VirtualFlag::Virtual);
            f.ty = bridge_ty_named(ck.from_coretype(*pm->type, tv), cvars, cnext, tv);
            ci.class_fields.push_back(std::move(f));
          } else if (std::holds_alternative<Pctf_inherit>(cf.desc) ||
                     std::holds_alternative<Pctf_constraint>(cf.desc)) {
            ok = false;  // inherited/constrained bodies aren't representable yet
            break;
          }
        }
        // cty_params (after members, so their tvars already have numbers), plus
        // the `object ('a)` self-param index (its self type IS a class param).
        for (std::size_t pi = 0; pi < d.params.size(); ++pi) {
          auto pv = bridge_ty(ctparams[pi], cvars, cnext);
          if (std::holds_alternative<Ptyp_any>(d.params[pi]->desc)) pv->var_name = "_";
          ci.class_params.push_back(pv);
        }
        if (!ci.class_params.empty())
          if (auto* sv = std::get_if<Ptyp_var>(&cs->cs.self->desc); sv && !sv->name.empty())
            for (std::size_t pi = 0; pi < d.params.size(); ++pi)
              if (auto* pv = std::get_if<Ptyp_var>(&d.params[pi]->desc); pv && pv->name == sv->name)
                ci.class_self_param = (int)pi;
        if (ok) out.push_back(std::move(ci));
      }
    } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
      // A submodule: emit Sig_module so the oracle can resolve `Outer.Inner.x`
      // and so the submodule's runtime field keeps the surrounding value layout
      // aligned.  Structures are inferred recursively (self-contained
      // submodules; outer refs not yet); constrained/functor shapes via
      // module_binding_sigitem.
      if (!mb->binding.name.txt) continue;  // `module _ = ...`
      // `module V0 = V0.U` whose HEAD is bound by an `open struct .. end` in
      // THIS structure: the target has no nameable path (the opened struct is
      // anonymous), so ocamlc stores the target's SIGNATURE, not an alias
      // (clambda_optim).  Resolve the dotted path through the opened struct's
      // (possibly nested) structure bindings and emit that module expression.
      {
        const ast::ModuleExpr* anon_tgt = nullptr;
        if (auto* pi = std::get_if<Pmod_ident>(&mb->binding.expr.desc);
            pi && !std::holds_alternative<Lapply>(pi->id.txt.v)) {
          std::vector<std::string> comps = split_dotted(lid_full(pi->id.txt));
          for (auto& it2 : s) {
            auto* op2 = std::get_if<Pstr_open>(&it2.desc);
            if (!op2) continue;
            auto* os = std::get_if<Pmod_structure>(&op2->expr.desc);
            if (!os) continue;
            const ast::Structure* scope = &os->items;
            const ast::ModuleExpr* tgt = nullptr;
            for (std::size_t ci = 0; ci < comps.size(); ++ci) {
              tgt = nullptr;
              if (!scope) break;
              for (auto& it3 : *scope)
                if (auto* mb3 = std::get_if<Pstr_module>(&it3.desc))
                  if (mb3->binding.name.txt &&
                      *mb3->binding.name.txt == comps[ci])
                    tgt = &mb3->binding.expr;
              scope = nullptr;
              if (tgt)
                if (auto* ts = std::get_if<Pmod_structure>(&tgt->desc))
                  scope = &ts->items;
            }
            if (tgt) { anon_tgt = tgt; break; }
          }
        }
        if (anon_tgt) {
          if (auto item = module_binding_sigitem(*mb->binding.name.txt,
                                                 *anon_tgt, &out, &ck))
            out.push_back(std::move(*item));
          continue;
        }
      }
      // `module Y = X` where X is a FUNCTOR PARAMETER (possibly `= X.M`):
      // a parameter path is not aliasable -- ocamlc stores the param's
      // signature EXPANDED (named-modtype submodules materialized, like
      // Mtype.strengthen's scrape) and strengthened at the path
      // (index_functor's `module M = X` / G's `module Y = X`).
      if (fparams) {
        auto* pi2 = std::get_if<Pmod_ident>(&mb->binding.expr.desc);
        std::vector<std::string> acomps;
        if (pi2 && !std::holds_alternative<Lapply>(pi2->id.txt.v))
          acomps = split_dotted(lid_full(pi2->id.txt));
        const ast::ModuleType* pmt = nullptr;
        if (!acomps.empty())
          for (auto& fp : *fparams)
            if (fp.first == acomps[0] && fp.second) pmt = fp.second;
        if (pmt) {
          std::function<void(std::vector<cmi::cmiw::SigItem>&)> mat_refs =
              [&](std::vector<cmi::cmiw::SigItem>& items) {
                for (auto& si : items) {
                  if (si.k != cmi::cmiw::SigItem::Module || si.is_functor ||
                      !si.alias.empty())
                    continue;
                  if (si.sub.empty() && !si.modtype_ref.empty()) {
                    std::vector<cmi::cmiw::SigItem> m;
                    if (si.modtype_ref.find('.') == std::string::npos) {
                      if (auto a = ck.modtype_sig_asts_.find(si.modtype_ref);
                          a != ck.modtype_sig_asts_.end())
                        m = signature_to_cmi(*a->second);
                    } else {
                      m = cmi_modtype_items(split_dotted(si.modtype_ref));
                    }
                    if (!m.empty()) { si.sub = std::move(m); si.modtype_ref.clear(); }
                  }
                  mat_refs(si.sub);
                }
              };
          std::string ref;
          std::vector<cmi::cmiw::SigItem> sub;
          annot_modtype_items(&ck, *pmt, ref, sub);
          mat_refs(sub);
          // Descend a dotted target (`= X.M`) to the cited submodule.
          bool ok = true;
          std::string path = acomps[0];
          for (std::size_t i = 1; i < acomps.size() && ok; ++i) {
            ok = false;
            for (auto& si : sub)
              if (si.k == cmi::cmiw::SigItem::Module && !si.is_functor &&
                  si.name == acomps[i]) {
                auto inner = std::move(si.sub);
                sub = std::move(inner);
                path += "." + acomps[i];
                ok = true;
                break;
              }
          }
          if (ok) {
            strengthen_abstract(sub, path);
            out.push_back(
                cmi::cmiw::sig_module(*mb->binding.name.txt, std::move(sub)));
            continue;
          }
        }
      }
      if (auto item = module_binding_sigitem(*mb->binding.name.txt,
                                             mb->binding.expr, &out, &ck))
        out.push_back(std::move(*item));
    } else if (auto* po2 = std::get_if<Pstr_open>(&it.desc)) {
      // `open M` where M is a LOCAL module already emitted above (`module
      // FArg = X.F(Arg); open FArg; type u = t`): register its type members
      // as open-quals so a following bare `t` that resolves to nothing local
      // renders qualified (`FArg.t`), like ocamlc stores it.  The
      // from_coretype consumer only fires when the bare name has no local
      // stamp, so genuinely local types keep priority (index_aliases).
      if (auto* omi = std::get_if<Pmod_ident>(&po2->expr.desc))
        if (auto* ol = std::get_if<Lident>(&omi->id.txt.v))
          for (auto& si : out)
            if (si.k == cmi::cmiw::SigItem::Module && !si.is_functor &&
                si.name == ol->name) {
              for (auto& m : si.sub)
                if (m.k == cmi::cmiw::SigItem::Type)
                  ck.opened_type_quals_.emplace(m.name,
                                                si.name + "." + m.name);
              break;
            }
    } else if (auto* mr = std::get_if<Pstr_recmodule>(&it.desc)) {
      // `module rec A .. and B ..`: each binding like Pstr_module, marked
      // Trec_first/Trec_next so ocamlc prints the group as one `module rec`.
      int rs = 1;
      for (auto& b : mr->bindings) {
        if (!b.name.txt) continue;
        if (auto item = module_binding_sigitem(*b.name.txt, b.expr, &out, &ck)) {
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
      std::string inc_path;  // non-empty = `include <local path>`: strengthen
      if (auto* ms = std::get_if<Pmod_structure>(&in->expr.desc))
        inc = &ms->items;                              // include (struct .. end)
      else if (auto* mi = std::get_if<Pmod_ident>(&in->expr.desc)) {
        std::string nm = lid_last(mi->id.txt);         // include LocalModule
        // Chase local ALIAS bindings (`module A_alias = A; include A_alias`):
        // ocamlc strengthens through the RESOLVED path (A, not A_alias).
        for (int hops = 0; hops < 8 && !inc; ++hops) {
          const ast::ModuleExpr* tgt = nullptr;
          for (auto& it2 : s)
            if (auto* mb2 = std::get_if<Pstr_module>(&it2.desc))
              if (mb2->binding.name.txt && *mb2->binding.name.txt == nm) {
                tgt = &mb2->binding.expr;
                break;
              }
          if (!tgt) break;
          if (auto* ms2 = std::get_if<Pmod_structure>(&tgt->desc)) {
            inc = &ms2->items;
            inc_path = nm;
          } else if (auto* mi2 = std::get_if<Pmod_ident>(&tgt->desc)) {
            if (auto* l2 = std::get_if<Lident>(&mi2->id.txt.v)) nm = l2->name;
            else break;
          } else {
            break;
          }
        }
        // `include X` where X is a module of the ENCLOSING structure
        // (`module Y = struct include X end` -- X outer): splice X's
        // already-emitted items, strengthened through X (fstclassmod).
        // Alias items are chased to their target (`module A_alias = A;
        // .. include A_alias` strengthens through A, pr6982).
        if (!inc && saved_enclosing &&
            std::holds_alternative<Lident>(mi->id.txt.v)) {
          std::string tnm = lid_last(mi->id.txt);
          for (int hops = 0; hops < 8; ++hops) {
            const cmi::cmiw::SigItem* found = nullptr;
            for (auto& si : *saved_enclosing)
              if (si.k == cmi::cmiw::SigItem::Module && si.name == tnm &&
                  !si.is_functor) {
                found = &si;
                break;
              }
            if (!found) break;
            if (!found->alias.empty()) {
              if (found->alias.find('.') != std::string::npos) break;
              tnm = found->alias;
              continue;
            }
            auto items = found->sub;
            strengthen_abstract(items, tnm);
            for (auto& s2 : items) {
              if (s2.k == cmi::cmiw::SigItem::Modtype) {
                s2.modtype_ref = tnm + "." + s2.name;
                s2.modtype_abstract = false;
              }
              out.push_back(std::move(s2));
            }
            inc_path = tnm;  // mark handled (skip the fparam fallback)
            break;
          }
        }
        // `include X` of a FUNCTOR PARAMETER: splice the param's signature
        // items, strengthened through the param path (ocamlc records
        // `type t = X.t` and X's vals as this module's own fields).
        if (!inc && inc_path.empty() && fparams &&
            std::holds_alternative<Lident>(mi->id.txt.v))
          for (auto& fp : *fparams)
            if (fp.first == lid_last(mi->id.txt) && fp.second) {
              std::string ref;
              std::vector<cmi::cmiw::SigItem> sub;
              annot_modtype_items(&ck, *fp.second, ref, sub);
              strengthen_abstract(sub, lid_last(mi->id.txt));
              for (auto& si : sub) {
                if (si.k == cmi::cmiw::SigItem::Type)
                  include_requal[fp.first + "." + si.name] = si.name;
                out.push_back(std::move(si));
              }
              break;
            }
        // `include N` where N's binding wasn't a plain struct (a functor
        // APPLICATION, `module N = F(..)`): splice N's already-EMITTED
        // items, alias-strengthened at N -- `module M = N.M`
        // (index_functor).
        if (!inc && inc_path.empty() &&
            std::holds_alternative<Lident>(mi->id.txt.v))
          for (auto& si : out)
            if (si.k == cmi::cmiw::SigItem::Module && !si.is_functor &&
                si.name == lid_last(mi->id.txt) && !si.sub.empty()) {
              std::vector<cmi::cmiw::SigItem> sub = si.sub;
              strengthen_abstract(sub, si.name, /*aliasable=*/true);
              for (auto& s2 : sub) out.push_back(std::move(s2));
              inc_path = si.name;  // mark handled
              break;
            }
        // `include Queue` of a COMPILATION UNIT (stdlib or a -I dir), bound
        // by nothing local: splice its compiled cmi signature strengthened at
        // the unit path -- ocamlc records `type 'a t = 'a Stdlib__Queue.t`,
        // `exception Empty`, and every included val citing the spliced LOCAL
        // t (lib-queue, lib-stack).  Members inferred AFTER the include cite
        // that local t too (ocamlc's env binds the included ident), so the
        // spliced type names are requalified over the tail of this module
        // once emission finishes (unit_includes).
        if (!inc && inc_path.empty() &&
            std::holds_alternative<Lident>(mi->id.txt.v)) {
          std::string unm = lid_last(mi->id.txt);
          bool bound_local = false;
          for (auto& it2 : s)
            if (auto* mb2 = std::get_if<Pstr_module>(&it2.desc))
              if (mb2->binding.name.txt && *mb2->binding.name.txt == unm)
                bound_local = true;
          if (fparams)
            for (auto& fp : *fparams)
              if (fp.first == unm) bound_local = true;
          std::string ucmi = head_cmi(unm);
          if (!bound_local && std::filesystem::exists(ucmi)) try {
            const auto& cmif = cmi::CmiFile::load(ucmi);
            auto items = cmi_sig_to_items(cmif.sig(), unm);
            // Strengthen at the unit's MANGLED name: ocamlc's manifests keep
            // the raw path (`type 'a t = 'a Stdlib__Queue.t`).
            strengthen_abstract(items, cmif.module_name().empty()
                                           ? unm
                                           : cmif.module_name(),
                                /*aliasable=*/true);
            std::set<std::string> tnames;
            for (auto& si : items)
              if (si.k == cmi::cmiw::SigItem::Type) tnames.insert(si.name);
            for (auto& si : items) out.push_back(std::move(si));
            unit_includes.push_back({out.size(), unm, std::move(tnames)});
            inc_path = unm;  // mark handled
          } catch (...) {}
        }
      } else if (auto* pc = std::get_if<Pmod_constraint>(&in->expr.desc)) {
        // `include (A : S)` / `include (struct .. end : S)`: ocamlc splices
        // S's items verbatim -- the constrained expression is not a path, so
        // nothing strengthens (`type t` stays abstract; includestruct).
        std::string cref;
        std::vector<cmi::cmiw::SigItem> csub;
        if (pc->mt) annot_modtype_items(&ck, *pc->mt, cref, csub);
        for (auto& si : csub) out.push_back(std::move(si));
      } else if (std::holds_alternative<Pmod_apply>(in->expr.desc)) {
        // `include F(struct end)`: the application's result items, computed by
        // the same machinery as `module N = F(..)`; no strengthening (an
        // application with a struct argument is not a path; includestruct's D).
        if (auto item = module_binding_sigitem("", in->expr, &out, &ck))
          for (auto& si : item->sub) out.push_back(std::move(si));
      }
      if (inc) {
        auto items = infer_signature(*inc);
        if (!inc_path.empty()) {
          // `include A` (a module PATH): ocamlc strengthens the spliced items
          // -- abstract types get `= A.t` manifests, submodules become
          // ALIASES `module Set = A.Set` (offset.ml), and MODTYPE decls
          // become aliases `module type S = A.S` (pr6982, fstclassmod).
          strengthen_abstract(items, inc_path, /*aliasable=*/true);
          for (auto& si : items)
            if (si.k == cmi::cmiw::SigItem::Modtype) {
              si.modtype_ref = inc_path + "." + si.name;
              si.modtype_abstract = false;
            }
        }
        for (auto& si : items) out.push_back(std::move(si));
      }
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
  // `include <unit>` requalification: members emitted after the splice cite
  // the spliced LOCAL types (`val to_list : 'a t`, not 'a Stdlib__Queue.t --
  // ocamlc's env binds the included type ident); the strengthening manifests
  // and pre-include members keep the qualified path.
  for (auto& [istart, iunit, itnames] : unit_includes) {
    if (istart >= out.size()) continue;
    std::vector<cmi::cmiw::SigItem> tail(
        std::make_move_iterator(out.begin() + (std::ptrdiff_t)istart),
        std::make_move_iterator(out.end()));
    out.resize(istart);
    rewrite_item_ty_names(tail, [&](std::string& nm) {
      auto d = nm.rfind('.');
      if (d == std::string::npos) return;
      std::string t = nm.substr(d + 1);
      if (!itnames.count(t)) return;
      std::string head = nm.substr(0, d);
      if (head == iunit || head == "Stdlib__" + iunit) nm = t;
    });
    for (auto& si : tail) out.push_back(std::move(si));
  }
  // `module MP = Gc.Memprof` aliases: ocamlc records value types THROUGH the
  // alias (MP.allocation, a Local-ident head), so rewrite each aliased prefix
  // back onto the emitted constr paths.  Longest target first (nested aliases).
  if (!ck.module_aliases_.empty()) {
    auto aliases = ck.module_aliases_;
    std::sort(aliases.begin(), aliases.end(),
              [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
    rewrite_item_ty_names(out, [&](std::string& nm) {
      for (auto& [tgt, al] : aliases)
        if (nm.rfind(tgt + ".", 0) == 0) { nm = al + nm.substr(tgt.size()); break; }
    });
  }
  out = cmi::cmiw::dedup_shadowed_fields(std::move(out));
  g_outer_modtype_asts = saved_mt_asts;
  g_outer_modtype_quals = saved_mt_quals;
  g_enclosing_struct_items = saved_enclosing;
  g_inherited_opens = saved_inherited_opens;
  g_outer_modenv = saved_modenv;
  g_outer_venv = saved_venv;
  g_outer_cenv = saved_cenv;
  return out;
}

}  // namespace cppcaml
