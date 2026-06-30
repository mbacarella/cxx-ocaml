#include "cppcaml/infer_check.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
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
  // type abbreviations: name -> (param var names, manifest core_type) so that a
  // `type ('a,..) t = <manifest>` can be expanded when t is used in annotations.
  struct Alias { std::vector<std::string> params; const CoreType* manifest; };
  std::unordered_map<std::string, Alias> type_aliases;
  // GADT type names (a constructor has an explicit result type): matching one
  // refines types per branch, so branch results must not be cross-unified.
  std::set<std::string> gadt_types;
  std::set<std::string> gadt_ctors;  // constructor names belonging to a GADT
  std::unordered_map<std::string, int> type_arity;  // type name -> param count
  // Type identity: each opaque (non-alias) local type declaration gets a unique
  // stamp; tenv is the scoped type-name -> stamp environment (mirrors module
  // scopes), so a shadowed `type t` resolves to the right identity.
  int next_type_stamp_ = 1;
  std::unordered_map<const TypeDeclaration*, int> type_stamp_;
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
  // Scoped, ordered constructor resolution (mirrors tenv for types): a ctor name
  // resolves to the in-scope declaration, so a name reused across several local
  // types is disambiguated by position instead of collapsed to ambiguous.
  std::unordered_map<const ConstructorDecl*, TypePtr> ctor_scheme_;
  std::vector<std::unordered_map<std::string, TypePtr>> cenv{{}};
  // names that are also predefined or exception constructors: when one of these
  // is reused by a variant, OCaml disambiguates by expected type (which we lack),
  // so we keep them unknown rather than resolve to the wrong kind.
  std::set<std::string> predef_ctors_;
  std::set<std::string> exn_ctors_;
  // For the Lambda back end: record inferred types of let/param patterns and
  // function bodies, so value kinds can be read off after inference (additive;
  // off by default so the soundness/completeness passes are unaffected).
  bool record_kinds_ = false;
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
  // Optional-argument erasure: an expression of type `?l:.. -> ..` used where a
  // non-optional arrow is expected is eta-expanded with None for each erased
  // optional.  The bool vector is the application's argument slots in order
  // (true = a None for an erased optional, false = an eta-expansion parameter);
  // the Lambda back end builds `(let (arg = e) (function eta.. (apply arg ..)))`.
  std::unordered_map<const Expression*, std::vector<bool>> erasures_;
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
  // locally-abstract types `(type a)`: bound to a fresh (flexible) var so that
  // annotations mentioning `a` unify rather than clashing as an opaque constr.
  std::unordered_map<std::string, TypePtr> newtype_vars;
  std::set<std::string> expanding_;  // guard against cyclic abbreviations
  // match-expression node -> is-partial (the result we route back to the dump)
  std::unordered_map<const Expression*, bool> match_partial;
  // Pexp_apply node -> reconstructed argument slots (callee-param order, omitted
  // optionals filled), for the dump.  Only stored when non-trivial (see infer_apply).
  std::unordered_map<const Expression*, std::vector<applymatch::Slot>> apply_plans;
  // Pexp_construct / Ppat_construct nodes whose resolved constructor has arity>1
  // and is applied to a matching tuple -> the dump flattens the tuple into the
  // constructor's arguments.  Covers cmi constructors the transcriber can't see.
  std::unordered_set<const void*> flatten_construct;
  // Functional record-update nodes (`{ ext_record with .. }`) whose base resolves
  // to an EXTERNAL record type -> its full ordered field list, so the dump can
  // emit the omitted fields as <kept> (the transcriber's registry has only local
  // records).  Keyed by the Pexp_record node.
  std::unordered_map<const Expression*, std::vector<std::string>> record_fields;
  // local module name -> its exported value schemes (so open/include/M.x resolve)
  std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>> modenv;
  // local functor name -> its body's exported value schemes (F(X) result)
  std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>> functor_env;
  // A parameterless class's object type, so `new c` yields it (non-strict only).
  std::unordered_map<std::string, TypePtr> class_types_;
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
  // Stdlib value schemes (loaded once, lazily).
  bool stdlib_ready_ = false;
  std::unordered_map<std::string, TypePtr> stdlib_;
  // Strict mode: record definite type errors instead of swallowing them.
  bool strict = false;
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
        if (is_format_base(p)) { std::vector<TypePtr> fa; for (auto& a : n->args) fa.push_back(from_cmi(a, memo)); return eng.constr("format6", std::move(fa)); }
        // expand a same-module type abbreviation (Float.t = float, Int.t = int) --
        // but NOT in a functor result, where `elt = Ord.t` stays the abstract,
        // binding-qualified name (`IntSet.elt`), not its expansion.
        if (!func_result_mode_ && cmi_types_ctx_ && n->path && n->path->kind == cmi::Path::Pident &&
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
        // qualify a same-unit (Pident) type with its owning module
        if (cmi_types_ctx_ && !cmi_mod_prefix_.empty() && n->path &&
            n->path->kind == cmi::Path::Pident)
          for (auto& td : *cmi_types_ctx_)
            if (td.name == n->path->id.name) { p = cmi_mod_prefix_ + "." + p; break; }
        std::vector<TypePtr> as;
        for (auto& a : n->args) as.push_back(from_cmi(a, memo));
        return eng.constr(std::move(p), std::move(as));
      }
      case cmi::TypeExpr::Tpoly:
        return from_cmi(n->link, memo);
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
  // (constraints `with type ..` are dropped -- best effort).
  TypePtr package_type(const Ptyp_package& pk) {
    return eng.constr("(module " + lid_full(pk.path.txt) + ")");
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
    // A closed all-constant polymorphic variant is an immediate (its values are
    // tag hashes) -- type it int so the [int] value kind flows.  Payload-carrying
    // or open/inherited rows stay opaque.
    if (auto* pvr = std::get_if<Ptyp_variant>(&t.desc)) {
      bool all_const = pvr->closed == ClosedFlag::Closed && !pvr->rows.empty();
      for (auto& r : pvr->rows) {
        auto* rt = std::get_if<Rtag>(&r);
        if (!rt || !rt->constant) { all_const = false; break; }
      }
      if (all_const) return eng.constr("int");
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
      if (is_format_base(lid_full(c->id.txt))) return eng.constr("format6");
      std::vector<TypePtr> as;
      for (auto& a : c->args) as.push_back(from_coretype(*a, vars));
      // Pad an under-applied type (e.g. an existential GADT's `_ raw_arity`
      // written with fewer wildcards than the type's arity) with Any, so it
      // unifies with the fully-applied form instead of clashing on arity.
      auto ar = type_arity.find(lid_last(c->id.txt));
      if (ar != type_arity.end())
        while ((int)as.size() < ar->second) as.push_back(eng.any());
      // Expand a known type abbreviation (type (params) name = manifest), with a
      // recursion guard so a cyclic/recursive abbreviation falls back to opaque.
      std::string nm = lid_last(c->id.txt);
      auto ai = type_aliases.find(nm);
      if (ai != type_aliases.end() && ai->second.params.size() == as.size() &&
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
      if (std::holds_alternative<Ldot>(c->id.txt.v))
        if (TypePtr r = expand_qualified_abbrev(c->id.txt, as)) return r;
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
      if (std::holds_alternative<Ldot>(c->id.txt.v)) {
        auto dot = path.find('.');
        if (dot != std::string::npos)
          if (auto q = opened_submod_quals_.find(path.substr(0, dot));
              q != opened_submod_quals_.end())
            path = q->second + path.substr(dot);
      }
      return eng.constr(std::move(path), std::move(as), stamp);
    }
    if (auto* pk = std::get_if<Ptyp_package>(&t.desc)) return package_type(*pk);
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
        if (f != it->end()) return eng.instantiate(f->second);
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
        return eng.any();   // else stay dynamic (a module we just can't load)
      }
      if (auto f = ex.find(d->name); f != ex.end())
        return eng.instantiate(f->second);  // present: use its real type
      if (strict) note_error("Unbound value " + lid_full(lid));  // genuinely absent
      return eng.any();
    }
    return eng.any();
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
        // A submodule `Array1` of an opened `Bigarray`: a `Array1.t` annotation
        // qualifies to `Bigarray.Array1.t`.
        for (auto& mm : sig->modules) opened_submod_quals_[mm.name] = full + "." + mm.name;
      }
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
    if (loaded_field_mods_.insert(key).second) load_module_record_fields(m);
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
      if (p[0] >= 'A' && p[0] <= 'Z') p[0] += 32;  // file is first-char-lowercased
      try {
        loaded.push_back(cmi::CmiFile::load(stdpath(p + ".cmi")));
        return &loaded.back().sig();
      } catch (...) { return nullptr; }
    }
    return nullptr;
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
    std::unordered_map<std::string, TypePtr> out;
    try {
      const std::string& head = comps[0];
      // cmis stay alive for the whole walk; sig points into the last one.
      std::vector<cmi::CmiFile> loaded;
      loaded.push_back(cmi::CmiFile::load(head_cmi(head)));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig) {
        cmi_types_ctx_ = &sig->types;  // enable same-module abbreviation expansion
        std::string pfx;
        for (auto& cmp : comps) { if (!pfx.empty()) pfx += '.'; pfx += cmp; }
        cmi_mod_prefix_ = pfx;
        for (auto& v : sig->values) {
          std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
          out[v.name] = from_cmi(v.type, memo);
        }
        cmi_types_ctx_ = nullptr;
        cmi_mod_prefix_.clear();
      }
    } catch (...) { cmi_types_ctx_ = nullptr; cmi_mod_prefix_.clear(); }
    return out;
  }

  // Load the record-field schemes of every record type in module `m` (navigated
  // through the cmis) into ext_fields_, qualified (`Effect.Deep.handler`).  So a
  // construction `{ retc; exnc; effc }` after `open Effect.Deep` resolves to the
  // handler record and its result type flows (match_with's `'c` -> unit).
  void load_module_record_fields(const Longident& m) {
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
                                                                 int napp = 1) {
    auto comps = mod_components(fpath);
    if (comps.empty()) return {};
    if (comps.size() == 1) {  // a local functor's recorded (fully-applied) body
      auto it = functor_env.find(comps[0]);
      if (it != functor_env.end()) return it->second;
    }
    std::unordered_map<std::string, TypePtr> out;
    try {
      const std::string& head = comps[0];
      auto cmi = cmi::CmiFile::load(head_cmi(head));
      const cmi::Signature* sig = &cmi.sig();
      const cmi::ModuleType* mt = nullptr;
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        if (!md || !md->type) { sig = nullptr; break; }
        mt = md->type.get();
        sig = (mt->kind == cmi::ModuleType::Sig) ? mt->sig.get() : nullptr;
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
        if (!ambiguous.count(name)) ctors[name] = ty;
    } catch (...) {}
  }

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
    std::set<std::string> seen, seen_class, seen_classty;
    for (auto& it : items) {
      if (auto* mt = std::get_if<Psig_modtype>(&it.desc)) {
        if (!seen.insert(mt->name.txt).second)
          note_error("Multiple definition of the module type name " + mt->name.txt);
        if (mt->type) check_dup_modtypes_mt(*mt->type);
      } else if (auto* md = std::get_if<Psig_module>(&it.desc)) {
        check_dup_modtypes_mt(*md->md.type);
      } else if (auto* rm = std::get_if<Psig_recmodule>(&it.desc)) {
        for (auto& d : rm->decls) check_dup_modtypes_mt(*d.type);
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
  void check_dup_modtypes_struct(const ast::Structure& items) {
    if (!strict) return;
    std::set<std::string> seen, seen_class, seen_classty;
    for (auto& it : items) {
      if (auto* mt = std::get_if<Pstr_modtype>(&it.desc)) {
        if (!seen.insert(mt->name.txt).second)
          note_error("Multiple definition of the module type name " + mt->name.txt);
        if (mt->type) check_dup_modtypes_mt(*mt->type);
      } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
        check_dup_modtypes_me(mb->binding.expr);
      } else if (auto* rm = std::get_if<Pstr_recmodule>(&it.desc)) {
        for (auto& b : rm->bindings) check_dup_modtypes_me(b.expr);
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
    // Opaque types (variant/record/abstract-without-manifest) have a distinct
    // identity; pure abbreviations are transparent (expanded), so unstamped.
    bool opaque = std::holds_alternative<Ptype_variant>(d.kind) ||
                  std::holds_alternative<Ptype_record>(d.kind) ||
                  (std::holds_alternative<Ptype_abstract>(d.kind) && !d.manifest);
    if (opaque) type_stamp_[&d] = next_type_stamp_++;
    if (d.manifest) {  // `type (params) t = <manifest>`: a type abbreviation
      std::vector<std::string> ps;
      for (auto& p : d.params)
        ps.push_back(std::holds_alternative<Ptyp_var>(p->desc)
                         ? std::get<Ptyp_var>(p->desc).name : "");
      type_aliases[d.name.txt] = {std::move(ps), d.manifest->get()};
    }
    auto* v = std::get_if<Ptype_variant>(&d.kind);
    if (!v) return;
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
      for (auto& c : v->ctors) gadt_ctors.insert(c.name.txt);
    }
    type_ctors[d.name.txt] = std::move(names);
    for (auto& c : v->ctors) {
      std::unordered_map<std::string, TypePtr> vars;
      std::vector<TypePtr> params;
      for (auto& p : d.params) params.push_back(from_coretype(*p, vars));
      TypePtr result = eng.constr(d.name.txt, params, type_stamp_[&d]);
      TypePtr scheme = result;
      if (auto* tup = std::get_if<Pcstr_tuple>(&c.args)) {
        for (auto it = tup->elems.rbegin(); it != tup->elems.rend(); ++it)
          scheme = eng.arrow(from_coretype(**it, vars), scheme);
      }
      register_inline_record(c.args, result, vars);  // `C of { f : t }`
      if (ctors.count(c.name.txt)) ambiguous_ctors_.insert(c.name.txt);
      ctors[c.name.txt] = scheme;
      ctor_scheme_[&c] = scheme;  // for scoped (in-order) resolution via cenv
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
      // a universally-quantified field (`{ f : 'a. ... }`) is polymorphic per use;
      // a single monomorphic scheme would clash, so leave it to Any -- EXCEPT, in
      // the KIND pass only, a format-typed field (`{ pf : 'a. ('a,..) format ->
      // 'a }`): its uses must type string literals at format type or they stay
      // unlowered (= segfault); cross-use clashes are soft there.  The strict
      // pass keeps the skip (a monomorphic scheme false-rejects valid reuses).
      if (std::holds_alternative<Ptyp_poly>(f.type->desc) &&
          !(record_kinds_ && mentions_format(*f.type)))
        continue;
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
    // (an incorrect pick could false-reject).
    if (ambiguous_ctors_.count(name) && (strict || !exn_ctors_.count(name)))
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
    } catch (...) {}
    return 0;
  }

  // The ordered field names of an external record type named by a dotted path
  // ("Gc.Memprof.tracker"), found by navigating the cmis (head cmi then nested
  // submodule signatures).  Empty when not a cmi-resolvable record.
  std::vector<std::string> cmi_record_fields(const std::string& path) {
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
          for (auto& l : td.labels) fs.push_back(l.name);
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
    TypePtr s = I::Engine::repr(scrut);
    if (s->kind != I::Type::Kind::Constr) return false;  // unknown type
    static const std::set<std::string> inf = {
        "int", "char", "string", "float", "int32", "int64", "nativeint", "bytes"};
    if (inf.count(s->path)) return true;  // infinite type, no catch-all
    auto it = type_ctors.find(s->path);
    if (it == type_ctors.end()) return false;  // unknown variant
    std::set<std::string> covered;
    for (auto& c : cases) if (!c.guard) collect_ctors(c.lhs, covered);
    for (auto& ctor : it->second) if (!covered.count(ctor)) return true;  // missing
    return false;  // covers all top-level ctors => Total (conservative)
  }

  TypePtr constant_type(const Constant& c) {
    if (auto* i = std::get_if<Pconst_integer>(&c.desc)) {
      if (i->suffix == 'l') return eng.constr("int32");
      if (i->suffix == 'L') return eng.constr("int64");
      if (i->suffix == 'n') return eng.constr("nativeint");
      return eng.constr("int");  // unsuffixed (or user-defined suffix => int)
    }
    if (std::holds_alternative<Pconst_char>(c.desc)) return eng.constr("char");
    if (std::holds_alternative<Pconst_string>(c.desc)) return eng.constr("string");
    return eng.constr("float");
  }

  // Bind all variables of a pattern to Any (used for patterns in an unknown
  // context, e.g. record fields we don't type).
  void bind_pat_any(const Pattern& p) {
    if (auto* v = std::get_if<Ppat_var>(&p.desc)) { venv.back()[v->name.txt] = eng.any(); return; }
    if (auto* al = std::get_if<Ppat_alias>(&p.desc)) {
      venv.back()[al->name.txt] = eng.any(); bind_pat_any(*al->p); return;
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

  TypePtr infer_pat(const Pattern& p) {
    TypePtr t = infer_pat_impl(p);
    if (record_kinds_) rec_pat_[&p] = t;  // record every pattern's kind (params incl.)
    if (as_map_) (*as_map_)[&p] = t;      // side record for build_as_type under an alias
    return t;
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
    return fallback();
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
      if (!sch) {
        // A qualified `M.C` not in scope: flatten its tuple by the cmi arity.
        if (auto* tup = k->arg ? std::get_if<Ppat_tuple>(&(*k->arg)->desc) : nullptr) {
          int ar = qualified_ctor_arity(k->id.txt);
          if (ar > 1 && (size_t)ar == tup->elems.size()) flatten_construct.insert(&p);
        }
        if (k->arg) infer_pat(**k->arg);
        // In the kind pass, return a fresh var (not Any) so unification against
        // the scrutinee binds it to the constructor's real type -- the back end
        // then resolves the unqualified constructor through that type
        // (type-directed disambiguation: Visible/Hidden : Load_path.visibility
        // matched without `open Load_path`).  The strict pass keeps Any.
        if (record_kinds_) return eng.fresh_var();
        return eng.any();  // unknown/ambiguous constructor: dynamic
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
          try_unify(ps[0], infer_pat(**k->arg));
        } else {
          infer_pat(**k->arg);
        }
      }
      return result;
    }
    if (auto* ct = std::get_if<Ppat_constraint>(&p.desc)) {
      TypePtr pt = infer_pat(*ct->p);
      std::unordered_map<std::string, TypePtr> vars;
      TypePtr at = from_coretype(*ct->t, vars);
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
          bind_pat_any(*sub); continue;
        }
        TypePtr s = I::Engine::repr(eng.instantiate(it->second));
        try_unify(infer_pat(*sub), s->cod);
        if (recTy) try_unify(recTy, s->dom); else recTy = s->dom;
      }
      return recTy ? recTy : eng.any();
    }
    if (auto* a = std::get_if<Ppat_array>(&p.desc)) {
      for (auto& el : a->elems) infer_pat(*el);
      return eng.fresh_var();
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
      if (pv->arg) infer_pat(**pv->arg);  // bind arg vars; poly-variant type unknown
      return eng.fresh_var();
    }
    if (auto* ex = std::get_if<Ppat_exception>(&p.desc)) {
      infer_pat(*ex->p);  // binds vars; matches an exn, independent of scrutinee
      return eng.fresh_var();
    }
    if (auto* op = std::get_if<Ppat_open>(&p.desc)) {
      venv.emplace_back();
      open_into(op->mod_.txt);
      std::set<std::string> opened;  // names introduced by the open, not by the pat
      for (auto& [k, v] : venv.back()) opened.insert(k);
      TypePtr t = infer_pat(*op->p);
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
      return up->pkg ? package_type(*up->pkg) : eng.fresh_var();
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
  TypePtr format_arrow(const std::string& s, const TypePtr& result) {
    auto isdig = [](char c) { return c >= '0' && c <= '9'; };
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
        case 'a': add(eng.any()); add(eng.any()); break;  // fn + value
        case 't': add(eng.any()); break;
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
        default: add(eng.any()); break;
      }
    }
    TypePtr r = result;  // the printf function's result is the format's result param
    for (auto it = args.rbegin(); it != args.rend(); ++it) r = eng.arrow(*it, r);
    return r;
  }

  // Infer an expression with an expected type pushed down (bidirectional).  A
  // string literal expected at a format type is accepted as that format (OCaml's
  // type_format), with its argument arrow filled in so the consuming application
  // (printf/sprintf/...) flows argument value-kinds.
  TypePtr infer_expr_expected(const Expression& e, const TypePtr& expected) {
    if (auto* c = std::get_if<Pexp_constant>(&e.desc))
      if (auto* s = std::get_if<Pconst_string>(&c->c.desc); s && is_format_constr(expected)) {
        if (record_kinds_) fmt_lits_.insert(&e);  // Lambda lowers it as a format
        auto er = I::Engine::repr(expected);  // format6's arg0 ('a) is the args function
        if (er->kind == I::Type::Kind::Constr && !er->args.empty()) {
          std::vector<TypePtr> a = er->args; a[0] = format_arrow(s->s, a.back());
          return eng.constr(er->path, std::move(a));
        }
        return expected;
      }
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
    if (record_kinds_) {
      std::vector<bool> slots;
      bool erased = false;
      TypePtr a = I::Engine::repr(t), ex = I::Engine::repr(expected);
      while (a->kind == I::Type::Kind::Arrow) {
        bool ex_arrow = ex->kind == I::Type::Kind::Arrow;
        if (a->arrow_label == 2 &&
            !(ex_arrow && ex->arrow_label == 2 && ex->arrow_lbl == a->arrow_lbl)) {
          slots.push_back(true);  // erase this optional -> None
          erased = true;
          a = I::Engine::repr(a->cod);
          continue;
        }
        if (!ex_arrow) break;
        slots.push_back(false);  // a kept parameter -> eta param
        a = I::Engine::repr(a->cod);
        ex = I::Engine::repr(ex->cod);
      }
      // Need at least one erased optional and at least one kept (eta) parameter:
      // a trailing-only optional with nothing after it isn't eta-expandable here.
      if (erased)
        for (bool none_slot : slots)
          if (!none_slot) { erasures_[&e] = slots; break; }
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
      newtype_vars[nt->name.txt] = eng.fresh_var();
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
      TypePtr* sch = find_ctor(lid_last(k->id.txt));
      if (!sch) {
        // A qualified `M.C` whose bare name isn't in scope: recover its variant
        // type from M's cmi (so an optional-arg default fixes the param type).
        if (auto* tup = k->arg ? std::get_if<Pexp_tuple>(&(*k->arg)->desc) : nullptr) {
          int ar = qualified_ctor_arity(k->id.txt);
          if (ar > 1 && (size_t)ar == tup->elems.size()) flatten_construct.insert(&e);
        }
        if (TypePtr qt = qualified_ctor_type(k->id.txt)) {
          if (k->arg) infer_expr(**k->arg);
          return qt;
        }
        if (k->arg) infer_expr(**k->arg);
        return eng.any();
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
      bool window = gadt && !record_kinds_;
      TypePtr rt = eng.fresh_var();
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
        if (window) eng.undo_to(wm);
        venv.pop_back();
      }
      match_partial[&e] = compute_partial(se, m->cases);  // for the dump (Slice 3)
      return rt;
    }
    if (auto* ct = std::get_if<Pexp_constraint>(&e.desc)) {
      TypePtr et = infer_expr(*ct->e);
      std::unordered_map<std::string, TypePtr> vars;
      TypePtr at = from_coretype(*ct->t, vars);
      if (strict && expected_clash(et, at))  // (e : T) with e of a clashing type
        note_error("expression does not match the type constraint");
      // Pin the inner expression's type for the value-kind pass (`(x:int)` ->
      // x:int); not in the strict reject pass, where an incomplete unify can
      // propagate a spurious clash and cost a false-rejection.
      if (record_kinds_) try_unify(et, at);
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
      if (pk->pkg) return package_type(*pk->pkg);
      return eng.any();  // unconstrained pack: type unknown without the sig
    }
    if (auto* nw = std::get_if<Pexp_new>(&e.desc)) {
      // `new c` for a parameterless local class is its object type.
      if (!strict)
        if (auto it = class_types_.find(lid_last(nw->id.txt)); it != class_types_.end())
          return eng.instantiate(it->second);
      return eng.any();
    }
    if (auto* lz = std::get_if<Pexp_lazy>(&e.desc)) {
      // `lazy e` : e Lazy.t -- only in the value-kinds pass (a concrete type here
      // can clash downstream in an incomplete strict pass and false-reject).
      // NB: rendering this as the predefined `lazy_t` (which is what ocamlc -i
      // shows for a constructed lazy value) was tried and REVERTED: lazy_t vs the
      // cmi-loaded Lazy.t don't unify by name, regressing reject 0.0%->0.1% and
      // sig (a lazy value flowing into a Lazy.t context clashed).
      TypePtr inner = infer_expr(*lz->e);
      return strict ? eng.any() : eng.constr("Lazy.t", {inner});
    }
    if (auto* as = std::get_if<Pexp_assert>(&e.desc)) {
      infer_expr(*as->e);  // infer the condition (flows operand kinds, e.g. x:int)
      // `assert false` is bottom ('a, never returns): a fresh var, so the
      // surrounding result type is decided by the other branches -- it neither
      // pins a polymorphic result (a fold accumulator) nor absorbs a concrete one
      // (`try (..; assert false) with _ -> 0` is int, from the handler).
      if (auto* ctr = std::get_if<Pexp_construct>(&as->e->desc))
        if (lid_last(ctr->id.txt) == "false") return generic_var();
      // `assert e` (e != false) is unit.  A concrete unit can cause our
      // incomplete strict pass to false-reject, so there alone we keep Any; the
      // value-kinds and signature passes commit to unit.
      return strict ? eng.any() : eng.constr("unit");
    }
    // Records, via the unique-label registry (ambiguous labels -> Any).
    if (auto* fld = std::get_if<Pexp_field>(&e.desc)) {
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
      return eng.any();
    }
    if (auto* rc = std::get_if<Pexp_record>(&e.desc)) {
      // Record update `{ e with ... }` flows the base record through incomplete
      // inference (e.g. recursive maps over a record tree) and clashes; its
      // soundness value is low, so type only plain construction.
      if (rc->base) {
        bool sv = strict; strict = false;
        TypePtr bt = infer_expr(**rc->base);
        for (auto& [lbl, val] : rc->fields) infer_expr(*val);
        strict = sv;
        // For the dump's `<kept>` fields, resolve an EXTERNAL record type's full
        // ordered field list from the cmis (local records use the transcriber's
        // own field registry).
        { TypePtr rb = I::Engine::repr(bt);
          if (rb->kind == I::Type::Kind::Constr) {
            auto fs = cmi_record_fields(rb->path);
            if (!fs.empty()) record_fields[&e] = std::move(fs);
          } }
        // The update's type IS the base record's type; returning it (instead of
        // `any`) lets a field read on the result (`let it = {super with ..} in
        // it.it_module_type`) resolve its label through that record type.
        return bt;
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
      if (strict && recTy) {
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
      return recTy ? recTy : eng.any();
    }
    if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) {
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
      // A LOCAL exception / type extension is only seen here (the top-level
      // register_types_rec pass that registers exception/typext ctors never
      // descends into expressions), so register its ctors now -- otherwise
      // `let exception E of t in E x` leaves `E x` as Any instead of exn.
      if (auto* ex = std::get_if<Pstr_exception>(&sti->item->desc))
        register_exception(ex->exn.ctor);
      else if (auto* tx = std::get_if<Pstr_typext>(&sti->item->desc))
        register_typext(tx->ext);
      process_item(*sti->item);
      TypePtr bt = infer_expr(*sti->body);
      venv.pop_back();
      return bt;
    }
    if (auto* sd = std::get_if<Pexp_send>(&e.desc)) {  // o#m: the method's type
      TypePtr ot = I::Engine::repr(infer_expr(*sd->obj));
      if (!strict && ot->kind == I::Type::Kind::Object)
        for (size_t i = 0; i < ot->labels.size(); ++i)
          if (ot->labels[i] == sd->meth.txt) return ot->args[i];
      return eng.any();
    }
    if (auto* si = std::get_if<Pexp_setinstvar>(&e.desc)) {  // n <- e: e has n's type
      TypePtr vt = infer_expr(*si->value);
      for (auto it = venv.rbegin(); it != venv.rend(); ++it)
        if (auto f = it->find(si->name.txt); f != it->end()) { try_unify(f->second, vt); break; }
      return eng.any();
    }
    if (auto* pv = std::get_if<Pexp_variant>(&e.desc)) {
      // A constructed polymorphic variant `` `A [e] `` has the open row type
      // `[> `A [of t]]`; rows merge through unification (if/match branches).  The
      // strict pass stays dynamic (matched-only variants are conjunctive -- a
      // naive row there is unsound; see the reverted attempt).
      if (strict) { if (pv->arg) infer_expr(**pv->arg); return eng.any(); }
      TypePtr at = pv->arg ? infer_expr(**pv->arg) : eng.fresh_var();
      return eng.variant_type({pv->label}, {at}, {(char)(pv->arg ? 1 : 0)});
    }
    if (auto* ob = std::get_if<Pexp_object>(&e.desc)) {
      // Non-strict passes type the method bodies and build the object type
      // `< m : t; .. >` (the signature pass renders it; the value-kind pass needs
      // the bodies for kinds/format literals).  The strict pass stays dynamic --
      // its self/instance-var model is incomplete (avoid false-rejects).
      if (!strict) return infer_object_body(*ob->cs);
      return eng.any();
    }
    return eng.any();  // records/fields/objects/etc. unhandled: dynamic, no clash
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
      for (size_t i = 0; i < m.slots.size(); ++i)
        if (m.slots[i].omitted || m.slots[i].some_wrap || m.slots[i].arg_index != (int)i) {
          trivial = false;
          break;
        }
    if (!trivial) apply_plans[&enode] = std::move(m.slots);
  }
  TypePtr infer_apply(const Pexp_apply& a, const Expression& enode) {
    TypePtr ft = infer_expr(*a.fn);
    record_apply_plan(a, enode, ft);
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
      for (auto& [lbl, arg] : a.args) {
        auto [lk, nm] = arglabel(lbl);
        int idx = match_param(spine, used, lk, nm);
        used[idx] = true;
        if (lk == 0 && idx > maxc) maxc = idx;
        TypePtr at = infer_expr_expected(*arg, spine[idx]->dom);
        // Restrict to a literal-constant argument: its type is certain, whereas a
        // GADT/abstract-typed expression argument may be mis-inferred.
        if (strict && reliable_callee && std::holds_alternative<Pexp_constant>(arg->desc) &&
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
      return res;
    }

    // Fallback: spine unknown/insufficient -- peel positionally (bidirectional
    // on each argument), so a var-typed callee never false-rejects.
    for (auto& [lbl, arg] : a.args) {
      auto [lk, nm] = arglabel(lbl);
      (void)nm;
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
      TypePtr dom = eng.fresh_var(), r = eng.fresh_var();
      soft_unify(ft, eng.arrow(dom, r));
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
                         const std::vector<const ast::Pcl_let*>* cl_lets = nullptr) {
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
      }
    // `class c = let .. in object`: the local bindings, before the fields.
    if (cl_lets) for (auto* lg : *cl_lets) infer_bindings(lg->rf, lg->bindings);
    // Pre-create a type variable per concrete method and bind `self` to the
    // object type built from them, so a method body's `self#other` resolves to
    // the (possibly forward-declared) sibling method's var, which a later
    // unification ties to that method's body type.  Without this, `self` was
    // Any and every self-method-call returned Any.
    std::unordered_map<std::string, TypePtr> mvar;
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
      if (auto* sv = std::get_if<Ppat_var>(&cs.self.desc)) venv.back()[sv->name.txt] = selfTy;
    }
    for (auto& f : cs.fields)
      if (auto* v = std::get_if<Pcf_val>(&f.desc))
        if (auto* cc = std::get_if<Cfk_concrete>(&v->kind))
          venv.back()[v->name.txt] = infer_expr(*cc->e);
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
    return eng.object_type(std::move(mnames), std::move(mtypes));
  }

  TypePtr infer_function(const Pexp_function& f) {
    venv.emplace_back();
    // Bind all (type a) params to flexible vars first, so value-param
    // annotations mentioning them resolve regardless of order.
    for (auto& fp : f.params)
      if (auto* nt = std::get_if<Pparam_newtype>(&fp.desc))
        newtype_vars[nt->name.txt] = eng.fresh_var();
    struct Param { TypePtr ty; int lk; std::string nm; };
    std::vector<Param> params;
    for (auto& fp : f.params) {
      // (type a) introduces a locally-abstract type, not a value argument, so it
      // contributes no arrow to the function's type.
      if (auto* pv = std::get_if<Pparam_val>(&fp.desc)) {
        auto [lk, nm] = arglabel(pv->label);
        TypePtr pt = infer_pat(pv->pat);
        // an optional parameter's type is its default's type: `?(c = 100)` => int
        if (pv->default_) try_unify(pt, infer_expr(**pv->default_));
        params.push_back({pt, lk, nm});
      }
    }
    TypePtr body;
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
      bool window = gadt && !record_kinds_;
      for (auto& c : fc.cases) {
        venv.emplace_back();
        size_t wm = window ? eng.mark() : 0;
        if (window) {
          soft_unify(infer_pat(c.lhs), arg);
          if (c.guard) infer_expr(**c.guard);
          soft_unify(infer_expr(*c.rhs), rt);
          eng.undo_to(wm);
        } else {
          try_unify(infer_pat(c.lhs), arg);
          if (c.guard) infer_expr(**c.guard);  // flows operand kinds; not bool-constrained
          try_unify(infer_expr(*c.rhs), rt);
        }
        venv.pop_back();
      }
      params.push_back({arg, 0, ""});
      body = rt;
    }
    // A return-type annotation (`fun .. : t -> e`) pins the body's type to t --
    // resolving fresh/Any results in the signature.  Skipped in the strict pass
    // (our incomplete inference could make a valid body clash with t and
    // false-reject); soft elsewhere so a stray clash can't abort the pass.
    if (f.constraint_ && !strict)
      if (auto* pc = std::get_if<Pconstraint>(&*f.constraint_)) {
        std::unordered_map<std::string, TypePtr> vars;
        soft_unify(body, from_coretype(*pc->type, vars));
      }
    if (record_kinds_) rec_ret_[&f] = body;
    TypePtr t = body;
    for (auto it = params.rbegin(); it != params.rend(); ++it)
      t = eng.arrow(it->ty, t, it->lk, it->nm);
    venv.pop_back();
    return t;
  }

  // Bind a let group (generalizing each RHS at the outer level).
  void infer_bindings(RecFlag rf, const std::vector<ValueBinding>& bs) {
    if (rf == RecFlag::Recursive) {
      // Pre-bind each name; a `let rec f : type a. T = ...` annotation makes f
      // polymorphic-recursive -- bind it to the (generic) annotation so recursive
      // calls instantiate fresh, rather than forcing one monomorphic type (which
      // for a GADT recursion yields a spurious occurs-check).  Plain bindings get
      // a monomorphic var unified with the inferred body.
      check_letrec(bs);  // the value-recursion restriction
      std::vector<TypePtr> tv(bs.size(), nullptr);
      for (size_t i = 0; i < bs.size(); ++i) {
        const ValueBinding& b = bs[i];
        const Pvc_constraint* pc =
            b.constraint_ ? std::get_if<Pvc_constraint>(&*b.constraint_) : nullptr;
        if (pc && !pc->univars.empty()) {
          for (auto& u : pc->univars) newtype_vars[u.txt] = generic_var();
          std::unordered_map<std::string, TypePtr> vars;
          bind_pattern_scheme(b.pat, from_coretype(*pc->typ, vars));
        } else if (pc && !strict) {
          // A plain declared type `let rec x : T = e` pins x to T -- bind the
          // name to the annotation rather than the (possibly Any) body, so a
          // body that infers Any (`(module struct end)`) or an under-determined
          // value (`[||]`) doesn't erase the declared type.  tv stays null: the
          // body is still inferred below (effects/kinds) but not unified back.
          std::unordered_map<std::string, TypePtr> vars;
          bind_pattern_scheme(b.pat, from_coretype(*pc->typ, vars));
        } else {
          tv[i] = infer_pat(b.pat);
        }
      }
      for (size_t i = 0; i < bs.size(); ++i) {
        TypePtr te = infer_expr(*bs[i].expr);  // check body (best-effort)
        if (tv[i]) try_unify(tv[i], te);
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
      return;
    }
    for (auto& b : bs) {
      eng.enter_level();
      TypePtr te = infer_expr(*b.expr);
      // A declared type `let f : T = e`: check the inferred type's identities
      // against T (a distinct local type used where another is declared is an
      // error).  We only flag identity (stamp) clashes, not structural ones --
      // structural inference is still incomplete, so unifying T into te would
      // false-reject (e.g. array vs iarray); the identity layer is reliable.
      if (b.constraint_)
        if (auto* pc = std::get_if<Pvc_constraint>(&*b.constraint_)) {
          for (auto& u : pc->univars) newtype_vars[u.txt] = generic_var();
          std::unordered_map<std::string, TypePtr> vars;
          if (strict && expected_clash(te, from_coretype(*pc->typ, vars)))
            note_error("type mismatch against declared type");
          // Flow the declared type `let x : T = e` into the inferred one (pins
          // an under-determined result, e.g. `let why : unit -> unit = fun () ->
          // raise Exit`).  Non-strict only (the strict pass keeps the inferred
          // type so an incomplete-inference clash can't false-reject); soft so a
          // stray clash can't abort the pass.
          if (!strict) soft_unify(te, from_coretype(*pc->typ, vars));
        }
      eng.leave_level();
      // Value restriction: generalise only a non-expansive (syntactic-value) RHS,
      // so `ref []` stays weak and is pinned by later use (`int list ref`, not
      // `'a list ref`).  Strict pass keeps generalising everything (an
      // over-eager weak var could false-reject a valid polymorphic use).
      if (strict || non_expansive(*b.expr)) eng.generalize(te);
      bind_pattern_scheme(b.pat, te);
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
        return inner;
      }
      return modtype_values(*mc->mt);  // e.g. `(val e : S)` parsed as a constraint
    }
    if (auto* mu = std::get_if<Pmod_unpack>(&me.desc)) {
      // (val e : S ...): resolve S's value names from the expression's package type
      const Expression* ie = mu->e.get();
      if (auto* ct = std::get_if<Pexp_constraint>(&ie->desc))
        if (auto* pk = std::get_if<Ptyp_package>(&ct->t->desc))
          return modtype_values_of(pk->path.txt);
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
        return functor_result_values(fi->id.txt, napp);
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
        if (auto* pi = std::get_if<Pmod_ident>(&in->expr.desc))
          for (auto& s : module_submodule_names(pi->id.txt)) opened_submodules_.insert(s);
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
        } else if (auto* sv = std::get_if<Pstr_value>(&it.desc))
          infer_bindings(sv->rf, sv->bindings);
        else if (auto* pc = std::get_if<Pstr_class>(&it.desc)) {
          if (!strict)  // value kinds + signature: class method bodies (Pexp_object)
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
                TypePtr ot = infer_object_body(ps->cs, params.empty() ? nullptr : &params,
                                               lets.empty() ? nullptr : &lets);
                // A parameterless class: `new c` is its object type.  Generalise
                // so each `new c` instantiates fresh.
                if (params.empty()) { eng.generalize(ot); class_types_[d.name.txt] = ot; }
              }
            }
        } else if (auto* ev = std::get_if<Pstr_eval>(&it.desc))
          infer_expr(*ev->e);
        else if (auto* op = std::get_if<Pstr_open>(&it.desc)) {
          for (auto& [k, v] : module_exports(op->expr)) venv.back()[k] = v;
          if (auto* pi = std::get_if<Pmod_ident>(&op->expr.desc)) {  // open M -> M's submodules
            for (auto& s : module_submodule_names(pi->id.txt)) opened_submodules_.insert(s);
            if (!strict) load_open_type_quals(pi->id.txt);  // bare type -> M.t (display)
          }
        } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
          if (mb->binding.name.txt) {
            // A functor: record its body's exports as the application result.
            const ModuleExpr* me = &mb->binding.expr;
            while (auto* mc = std::get_if<Pmod_constraint>(&me->desc)) me = mc->me.get();
            if (std::holds_alternative<Pmod_functor>(me->desc)) {
              while (auto* mf = std::get_if<Pmod_functor>(&me->desc)) me = mf->body.get();
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
              for (auto& [k, v] : ex) v = generic_var();
              functor_env[*mb->binding.name.txt] = std::move(ex);
            } else {
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
            auto ex = module_exports(b.expr);
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
            venv.back()[pr->prim.name.txt] = ty;
          }
        } else if (auto* mt = std::get_if<Pstr_modtype>(&it.desc)) {
          if (mt->type)  // record a signature module type's value names for unpacks
            if (auto* sg = std::get_if<Pmty_signature>(&mt->type->desc))
              collect_sig_values(sg->items, modtype_env[mt->name.txt]);
        }
      } catch (const I::TypeError&) {
      }
    }
  }
};

}  // namespace

// Register variant constructors from type decls, recursing into local module
// structures (a flat ctor namespace — best-effort, so `open M; A` resolves).
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
      }
    }
  }
}

// Shared setup: register constructors, then run best-effort inference over the
// structure (populating ck.match_partial and ck.errors as it traverses).
static void run_checker(Checker& ck, const ast::Structure& s) {
  ck.register_predef_ctors();
  ck.register_stdlib_ctors();
  ck.collect_bound_modules(s);  // pre-collect bound module names (before type checks)
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
  run_checker(ck, s);
  DumpAux out;
  out.match_partial = std::move(ck.match_partial);
  out.apply_plans = std::move(ck.apply_plans);
  out.flatten_construct = std::move(ck.flatten_construct);
  out.record_fields = std::move(ck.record_fields);
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
  run_checker(ck, s);
  std::vector<std::pair<std::string, std::string>> all;
  auto emit = [&](const std::string& nm) {
    auto f = ck.venv.back().find(nm);
    if (f != ck.venv.back().end()) all.emplace_back(nm, I::show(f->second));
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
        if (const ast::Structure* inc = incstruct(in->expr)) walk(*inc);
      }
    }
  };
  walk(s);
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
static cmi::cmiw::TyPtr bridge_ty(const TypePtr& t0,
                                  std::unordered_map<const I::Type*, int>& vars, int& nextvar) {
  TypePtr t = I::Engine::repr(t0);
  using K = I::Type::Kind;
  switch (t->kind) {
    case K::Var: {
      auto it = vars.find(t.get());
      if (it != vars.end()) return cmi::cmiw::ty_var(it->second);
      int id = nextvar++; vars[t.get()] = id; return cmi::cmiw::ty_var(id);
    }
    case K::Any: return cmi::cmiw::ty_var(nextvar++);
    case K::Object: return cmi::cmiw::ty_var(nextvar++);  // opaque in the .cmi for now
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
      // to "format6" by from_coretype) lives in CamlinternalFormatBasics.  Qualify
      // it so the cmi writer emits a real Tconstr (Pdot) instead of degrading the
      // bare name to a Tvar -- otherwise a `val printf : (...) format -> 'a`
      // records as `'_ -> '_`, the reader can't see the format type, and format
      // string literals aren't lowered to fmt values (Printf then gets a raw
      // string and dies in make_printf).
      if (path == "format6") path = "CamlinternalFormatBasics.format6";
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
  for (auto& d : decls) {
    std::unordered_map<std::string, TypePtr> tvars;        // param name -> engine var
    std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;  // shared across params+manifest
    std::vector<cmi::cmiw::TyPtr> params;
    for (auto& p : d.params) params.push_back(bridge_ty(ck.from_coretype(*p, tvars), bvars, nextvar));
    // A variant type: emit its constructors (Cstr_tuple args OR an inline record
    // `Ctor of {l;..}`; GADT results are dropped to a no-arg ctor for now).
    if (auto* var = std::get_if<Ptype_variant>(&d.kind)) {
      std::vector<cmi::cmiw::Ctor> ctors;
      for (auto& c : var->ctors) {
        cmi::cmiw::Ctor cc; cc.name = c.name.txt;
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
      out.push_back(cmi::cmiw::sig_variant(d.name.txt, std::move(params), std::move(ctors)));
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
      out.push_back(cmi::cmiw::sig_record(d.name.txt, std::move(params), std::move(labels)));
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
    out.push_back(cmi::cmiw::sig_type(d.name.txt, std::move(params), manifest));
  }
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
static cmi::cmiw::SigItem exn_sigitem(Checker& ck, const std::string& name,
                                      const ast::Pext_decl& pd) {
  std::unordered_map<std::string, TypePtr> tvars;
  std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
  if (auto* rec = std::get_if<Pcstr_record>(&pd.args)) {
    std::vector<cmi::cmiw::Label> labels;
    for (auto& f : rec->fields) {
      cmi::cmiw::Label lab;
      lab.name = f.name.txt;
      lab.mut = (f.mut == MutableFlag::Mutable);
      lab.ty = bridge_ty(ck.from_coretype(*f.type, tvars), bvars, nextvar);
      labels.push_back(std::move(lab));
    }
    return cmi::cmiw::sig_exception_record(name, std::move(labels));
  }
  std::vector<cmi::cmiw::TyPtr> args;
  if (auto* tup = std::get_if<Pcstr_tuple>(&pd.args))
    for (auto& a : tup->elems)
      args.push_back(bridge_ty(ck.from_coretype(*a, tvars), bvars, nextvar));
  return cmi::cmiw::sig_exception(name, std::move(args));
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
        out.push_back(cmi::cmiw::sig_external(pr->pd.name.txt, ty, pr->pd.prims[0], native));
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
      for (auto& ec : px->ext.ctors) {
        if (ec.name.txt.empty()) continue;
        if (auto* pd = std::get_if<Pext_decl>(&ec.kind))
          out.push_back(exn_sigitem(ck, ec.name.txt, *pd));
        else
          out.push_back(cmi::cmiw::sig_exception(ec.name.txt, {}));  // rebind `+= C = D`
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

std::vector<cmi::cmiw::SigItem> infer_signature(const ast::Structure& s) {
  Checker ck;
  ck.record_kinds_ = true;
  run_checker(ck, s);  // leaves top-level bindings in venv.back()
  std::vector<cmi::cmiw::SigItem> out;
  for (auto& it : s) {
    if (auto* sv = std::get_if<Pstr_value>(&it.desc)) {
      for (auto& b : sv->bindings)
        if (auto* v = std::get_if<Ppat_var>(&b.pat.desc)) {  // single-var top-level lets
          auto f = ck.venv.back().find(v->name.txt);
          if (f == ck.venv.back().end()) continue;
          std::unordered_map<const I::Type*, int> vars; int nextvar = 0;
          out.push_back(cmi::cmiw::sig_value(v->name.txt, bridge_ty(f->second, vars, nextvar)));
        }
    } else if (auto* pr = std::get_if<Pstr_primitive>(&it.desc)) {
      // `external f : t = "prim"`: a Val_prim value (typed from the annotation).
      if (pr->prim.type && !pr->prim.prims.empty()) {
        std::unordered_map<std::string, TypePtr> tvars;
        std::unordered_map<const I::Type*, int> bvars; int nextvar = 0;
        auto ty = bridge_ty(ck.from_coretype(*pr->prim.type, tvars), bvars, nextvar);
        std::string native = pr->prim.prims.size() > 1 ? pr->prim.prims[1] : "";
        out.push_back(cmi::cmiw::sig_external(pr->prim.name.txt, ty, pr->prim.prims[0], native));
      }
    } else if (auto* ty = std::get_if<Pstr_type>(&it.desc)) {
      emit_type_decls(ck, ty->decls, out);
    } else if (auto* pe = std::get_if<Pstr_exception>(&it.desc)) {
      // `exception E [of ..]` in a .ml without a .mli: emit the Sig_typext so
      // the inferred .cmi carries the exception (it takes a runtime field, and a
      // qualified `M.E` use must resolve it -- otherwise the match collapses).
      const ExtensionConstructor& ec = pe->exn.ctor;
      if (!ec.name.txt.empty())
        if (auto* pd = std::get_if<Pext_decl>(&ec.kind))
          out.push_back(exn_sigitem(ck, ec.name.txt, *pd));
    } else if (auto* px = std::get_if<Pstr_typext>(&it.desc)) {
      for (auto& ec : px->ext.ctors)
        if (!ec.name.txt.empty())
          if (auto* pd = std::get_if<Pext_decl>(&ec.kind))
            out.push_back(exn_sigitem(ck, ec.name.txt, *pd));
    } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
      // A submodule `module Inner = struct ... end`: emit Sig_module so the
      // oracle can resolve `Outer.Inner.x` and so the submodule's runtime field
      // keeps the surrounding value layout aligned.  Inner structures are
      // inferred recursively (self-contained submodules; outer refs not yet).
      if (!mb->binding.name.txt) continue;  // `module _ = ...`
      if (auto* ms = std::get_if<Pmod_structure>(&mb->binding.expr.desc))
        out.push_back(cmi::cmiw::sig_module(*mb->binding.name.txt, infer_signature(ms->items)));
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
