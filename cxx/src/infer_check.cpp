#include "cppcaml/infer_check.hpp"

#include <functional>
#include <optional>
#include <set>
#include <unordered_map>

#include "cppcaml/cmi.hpp"

namespace cppcaml {
namespace {

namespace I = infer;
using namespace ast;
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
  std::set<const Expression*> fmt_lits_;  // string literals inferred at format type
  // Local variant types whose constructors are all constant (nullary): these have
  // an immediate (int) runtime representation, so a value of such a type gets the
  // [int] value kind in the Lambda dump.
  std::set<std::string> immediate_types_;
  // record fields with a UNIQUE label across all record types: label -> generic
  // scheme arrow(recordType, fieldType).  Ambiguous labels are omitted (type-
  // directed disambiguation needed) and left to Any, so this can't pick wrong.
  std::unordered_map<std::string, TypePtr> fields_;
  std::unordered_map<std::string, std::vector<TypePtr>> field_candidates_;
  // locally-abstract types `(type a)`: bound to a fresh (flexible) var so that
  // annotations mentioning `a` unify rather than clashing as an opaque constr.
  std::unordered_map<std::string, TypePtr> newtype_vars;
  std::set<std::string> expanding_;  // guard against cyclic abbreviations
  // match-expression node -> is-partial (the result we route back to the dump)
  std::unordered_map<const Expression*, bool> match_partial;
  // local module name -> its exported value schemes (so open/include/M.x resolve)
  std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>> modenv;
  // local functor name -> its body's exported value schemes (F(X) result)
  std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>> functor_env;
  // local module-type name -> its signature's value names (first-class modules):
  // (val e : S) unpacks bring S's values into scope.
  std::unordered_map<std::string, std::vector<std::string>> modtype_env;
  // When loading a module's values from a cmi, its own type abbreviations so
  // from_cmi can expand them (e.g. Float.t = float, so `min : t -> t -> t`
  // becomes float -> float -> float instead of clashing t vs float).
  const std::vector<cmi::TypeDecl>* cmi_types_ctx_ = nullptr;
  std::set<std::string> cmi_expanding_;
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
        // expand a same-module type abbreviation (Float.t = float, Int.t = int)
        if (cmi_types_ctx_ && n->path && n->path->kind == cmi::Path::Pident &&
            !cmi_expanding_.count(n->path->id.name))
          for (auto& td : *cmi_types_ctx_)
            if (td.name == n->path->id.name && td.manifest &&
                td.params.size() == n->args.size()) {
              std::unordered_map<cmi::TypeExpr*, TypePtr> m2;
              for (size_t i = 0; i < td.params.size(); ++i)
                m2[td.params[i].get()] = from_cmi(n->args[i], memo);
              cmi_expanding_.insert(td.name);
              TypePtr r = from_cmi(td.manifest, m2);
              cmi_expanding_.erase(td.name);
              return r;
            }
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
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
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
      // A bare reference to a local opaque type carries its identity stamp.
      int stamp = 0;
      if (auto* l = std::get_if<Lident>(&c->id.txt.v)) stamp = tenv_lookup(l->name);
      return eng.constr(lid_full(c->id.txt), std::move(as), stamp);
    }
    return eng.fresh_var();
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
      if (ex.empty()) return eng.any();   // module unresolvable: stay dynamic
      if (auto f = ex.find(d->name); f != ex.end())
        return eng.instantiate(f->second);  // present: use its real type
      if (strict) note_error("Unbound value " + lid_full(lid));  // genuinely absent
      return eng.any();
    }
    return eng.any();
  }

  // resolve_module_values memoized by module path (cmi loads are expensive and a
  // file may reference M.x many times).
  std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>> modvals_cache_;
  const std::unordered_map<std::string, TypePtr>& module_values_cached(const Longident& m) {
    std::string key = lid_full(m);
    auto it = modvals_cache_.find(key);
    if (it != modvals_cache_.end()) return it->second;
    return modvals_cache_.emplace(key, resolve_module_values(m)).first->second;
  }

  // Stdlib top-level value schemes, loaded once from stdlib.cmi.
  const std::unordered_map<std::string, TypePtr>& stdlib_schemes() {
    if (stdlib_ready_) return stdlib_;
    stdlib_ready_ = true;
    try {
      auto cmi = cmi::CmiFile::load("stdlib/stdlib.cmi");
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
        loaded.push_back(cmi::CmiFile::load("stdlib/" + p + ".cmi"));
        return &loaded.back().sig();
      } catch (...) { return nullptr; }
    }
    return nullptr;
  }

  // Resolve a (possibly qualified) module path to its exported value schemes:
  // a local top-level module from modenv, else a stdlib module/submodule walked
  // through nested signatures (open Effect.Deep -> stdlib__Effect.cmi -> Deep).
  std::unordered_map<std::string, TypePtr> resolve_module_values(const Longident& m) {
    auto comps = mod_components(m);
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
      loaded.push_back(head == "Stdlib"
                           ? cmi::CmiFile::load("stdlib/stdlib.cmi")
                           : cmi::CmiFile::load("stdlib/stdlib__" + head + ".cmi"));
      const cmi::Signature* sig = &loaded.back().sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        sig = md ? module_sig(md->type, loaded) : nullptr;
      }
      if (sig) {
        cmi_types_ctx_ = &sig->types;  // enable same-module abbreviation expansion
        for (auto& v : sig->values) {
          std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
          out[v.name] = from_cmi(v.type, memo);
        }
        cmi_types_ctx_ = nullptr;
      }
    } catch (...) { cmi_types_ctx_ = nullptr; }
    return out;
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
      auto cmi = (head == "Stdlib")
                     ? cmi::CmiFile::load("stdlib/stdlib.cmi")
                     : cmi::CmiFile::load("stdlib/stdlib__" + head + ".cmi");
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
      if (cur && cur->kind == cmi::ModuleType::Sig && cur->sig)
        for (auto& v : cur->sig->values) out[v.name] = generic_var();
    } catch (...) {}
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
  }

  // Register top-level Stdlib variant CONSTANT constructors (e.g. fpclass's
  // FP_normal) so an unqualified use gets its real type (and an all-constant type
  // is marked immediate -> the [int] value kind).  Skips names already known
  // (predef wins) or ambiguous across stdlib types; constant ctors only (block
  // ctors need parameter handling and are left to Any).
  void register_stdlib_ctors() {
    try {
      auto cmi = cmi::CmiFile::load("stdlib/stdlib.cmi");
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
    if (all_const && !is_gadt) immediate_types_.insert(d.name.txt);
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
    TypePtr recTy = eng.constr(d.name.txt, params, type_stamp_[&d]);
    for (auto& f : rec->fields) {
      // a universally-quantified field (`{ f : 'a. ... }`) is polymorphic per use;
      // a single monomorphic scheme would clash, so leave it to Any.
      if (std::holds_alternative<Ptyp_poly>(f.type->desc)) continue;
      field_candidates_[f.name.txt].push_back(eng.arrow(recTy, from_coretype(*f.type, vars)));
    }
  }
  // Keep only labels unique across all record types (others need type-direction).
  void finalize_fields() {
    for (auto& [k, v] : field_candidates_)
      if (v.size() == 1) fields_[k] = v[0];
  }

  // Register an exception/extension constructor: A of t1*..*tn => t1->..->tn->exn.
  // Participates in ambiguity detection so `exception E` + `type t = E` makes E
  // ambiguous (type-directed disambiguation, approximated as unknown).
  void register_exception(const ExtensionConstructor& ec) {
    TypePtr scheme = eng.constr("exn");
    if (auto* d = std::get_if<Pext_decl>(&ec.kind))
      if (auto* tup = std::get_if<Pcstr_tuple>(&d->args))
        for (auto it = tup->elems.rbegin(); it != tup->elems.rend(); ++it) {
          std::unordered_map<std::string, TypePtr> vars;
          scheme = eng.arrow(from_coretype(**it, vars), scheme);
        }
    if (ctors.count(ec.name.txt)) ambiguous_ctors_.insert(ec.name.txt);
    ctors[ec.name.txt] = scheme;
    exn_ctors_.insert(ec.name.txt);
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
    if (ambiguous_ctors_.count(name)) return nullptr;
    auto it = ctors.find(name);
    return it == ctors.end() ? nullptr : &it->second;
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
    return t;
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
        if (k->arg) infer_pat(**k->arg);
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
      TypePtr t = infer_pat(*al->p);
      venv.back()[al->name.txt] = t;
      return t;
    }
    if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
      // Type fields via the unique-label registry; an ambiguous/unknown label's
      // sub-pattern vars bind to Any (a polymorphic field used at several types
      // must not clash through one monomorphic var).
      TypePtr recTy = nullptr;
      for (auto& [lid, sub] : r->fields) {
        auto it = fields_.find(lid_last(lid.txt));
        if (it == fields_.end()) { bind_pat_any(*sub); continue; }
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
    if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) return infer_pat(*lz->p);
    if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      // Both branches bind the same variables; type them and unify.
      TypePtr lt = infer_pat(*o->l), rt = infer_pat(*o->r);
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
      char len = 0;
      if (s[i] == 'l' || s[i] == 'n' || s[i] == 'L') { len = s[i]; if (++i >= n) break; }
      char c = s[i]; ++i;
      switch (c) {
        case 'd': case 'i': case 'x': case 'X': case 'o': case 'u':
          add(eng.constr(len == 'l' ? "int32" : len == 'n' ? "nativeint"
                         : len == 'L' ? "int64" : "int")); break;
        case 's': case 'S': add(eng.constr("string")); break;
        case 'c': case 'C': add(eng.constr("char")); break;
        case 'f': case 'e': case 'E': case 'g': case 'G': case 'F': case 'h': case 'H':
          add(eng.constr("float")); break;
        case 'b': case 'B': add(eng.constr("bool")); break;
        case 'a': add(eng.any()); add(eng.any()); break;  // fn + value
        case 't': add(eng.any()); break;
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
    return infer_expr(e);
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
      return infer_apply(*a);
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
    if (auto* t = std::get_if<Pexp_tuple>(&e.desc)) {
      std::vector<TypePtr> es;
      for (auto& el : t->elems) es.push_back(infer_expr(*el));
      return eng.tuple(std::move(es));
    }
    if (auto* k = std::get_if<Pexp_construct>(&e.desc)) {
      TypePtr* sch = find_ctor(lid_last(k->id.txt));
      if (!sch) { if (k->arg) infer_expr(**k->arg); return eng.any(); }
      TypePtr result;
      auto ps = ctor_params(eng.instantiate(*sch), result);
      if (k->arg) {
        auto* tup = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
        if (ps.size() > 1 && tup && tup->elems.size() == ps.size()) {
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
      if (it->else_) try_unify(tt, infer_expr(**it->else_));
      return tt;
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
      TypePtr t = infer_expr(*tr->e);  // body and every handler share the result type
      for (auto& c : tr->cases) {
        venv.emplace_back();
        infer_pat(c.lhs);  // an exception pattern (binds exn-typed vars)
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
      TypePtr rt = eng.fresh_var();
      for (auto& c : m->cases) {
        venv.emplace_back();
        TypePtr pt = infer_pat(c.lhs);
        if (!gadt) try_unify(pt, se);
        if (c.guard) infer_expr(**c.guard);
        TypePtr br = infer_expr(*c.rhs);
        if (!gadt) try_unify(br, rt);
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
      return at;
    }
    if (auto* as = std::get_if<Pexp_assert>(&e.desc)) {
      infer_expr(*as->e);  // infer the condition (flows operand kinds, e.g. x:int)
      // `assert false` is bottom ('a, never returns): the type-checker leaves the
      // surrounding result type open, so don't constrain it (would wrongly pin a
      // polymorphic result -- e.g. a fold's accumulator -- to unit's [int] kind).
      if (auto* ctr = std::get_if<Pexp_construct>(&as->e->desc))
        if (lid_last(ctr->id.txt) == "false") return eng.any();
      // `assert e` (e != false) is unit (-> the [int] value kind), but a concrete
      // unit clashes in strict checking, so only commit to unit for kinds.
      return record_kinds_ ? eng.constr("unit") : eng.any();
    }
    // Records, via the unique-label registry (ambiguous labels -> Any).
    if (auto* fld = std::get_if<Pexp_field>(&e.desc)) {
      auto it = fields_.find(lid_last(fld->field.txt));
      if (it == fields_.end()) { infer_expr(*fld->e); return eng.any(); }
      TypePtr s = I::Engine::repr(eng.instantiate(it->second));  // recTy -> fldTy
      try_unify(infer_expr(*fld->e), s->dom);
      return s->cod;
    }
    if (auto* rc = std::get_if<Pexp_record>(&e.desc)) {
      // Record update `{ e with ... }` flows the base record through incomplete
      // inference (e.g. recursive maps over a record tree) and clashes; its
      // soundness value is low, so type only plain construction.
      if (rc->base) {
        bool sv = strict; strict = false;
        infer_expr(**rc->base);
        for (auto& [lbl, val] : rc->fields) infer_expr(*val);
        strict = sv;
        return eng.any();
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
        auto it = fields_.find(lid_last(lbl.txt));
        if (it == fields_.end()) continue;
        TypePtr s = I::Engine::repr(eng.instantiate(it->second));
        try_unify(vt, s->cod);
        if (recTy) try_unify(recTy, s->dom); else recTy = s->dom;
      }
      return recTy ? recTy : eng.any();
    }
    if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) {
      auto it = fields_.find(lid_last(sf->field.txt));
      if (it != fields_.end()) {
        TypePtr s = I::Engine::repr(eng.instantiate(it->second));
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
      process_item(*sti->item);
      TypePtr bt = infer_expr(*sti->body);
      venv.pop_back();
      return bt;
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

  TypePtr infer_apply(const Pexp_apply& a) {
    TypePtr ft = infer_expr(*a.fn);
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
      int maxc = -1;
      for (auto& [lbl, arg] : a.args) {
        auto [lk, nm] = arglabel(lbl);
        int idx = match_param(spine, used, lk, nm);
        used[idx] = true;
        if (idx > maxc) maxc = idx;
        TypePtr at = infer_expr_expected(*arg, spine[idx]->dom);
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
      TypePtr dom = eng.fresh_var(), r = eng.fresh_var();
      soft_unify(ft, eng.arrow(dom, r));
      TypePtr at = infer_expr_expected(*arg, dom);
      soft_unify(dom, at);
      ft = I::Engine::repr(r);
    }
    return ft;
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
      for (auto& c : fc.cases) {
        venv.emplace_back();
        try_unify(infer_pat(c.lhs), arg);
        if (c.guard) infer_expr(**c.guard);  // infer (flows operand kinds); not bool-constrained
        try_unify(infer_expr(*c.rhs), rt);
        venv.pop_back();
      }
      params.push_back({arg, 0, ""});
      body = rt;
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
        } else {
          tv[i] = infer_pat(b.pat);
        }
      }
      for (size_t i = 0; i < bs.size(); ++i) {
        TypePtr te = infer_expr(*bs[i].expr);  // check body (best-effort)
        if (tv[i]) try_unify(tv[i], te);
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
        }
      eng.leave_level();
      eng.generalize(te);
      bind_pattern_scheme(b.pat, te);
    }
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
  bool type_decls_clash(const TypeDeclaration& impl, const TypeDeclaration& spec) {
    if (std::holds_alternative<Ptype_abstract>(spec.kind) && !spec.manifest)
      return false;  // spec abstract: any implementation is fine
    if (impl.manifest && spec.manifest) {
      std::unordered_map<std::string, TypePtr> v1, v2;
      return expected_clash(from_coretype(*impl.manifest->get(), v1),
                            from_coretype(*spec.manifest->get(), v2));
    }
    auto* iv = std::get_if<Ptype_variant>(&impl.kind);
    auto* sv = std::get_if<Ptype_variant>(&spec.kind);
    if (iv && sv) {
      std::set<std::string> si, ss;
      for (auto& c : iv->ctors) { if (c.res) return false; si.insert(c.name.txt); }
      for (auto& c : sv->ctors) { if (c.res) return false; ss.insert(c.name.txt); }
      return si != ss;
    }
    auto* ir = std::get_if<Ptype_record>(&impl.kind);
    auto* sr = std::get_if<Ptype_record>(&spec.kind);
    if (ir && sr) {
      std::set<std::string> fi, fs;
      for (auto& f : ir->fields) fi.insert(f.name.txt);
      for (auto& f : sr->fields) fs.insert(f.name.txt);
      return fi != fs;
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
  static bool expected_clash(const TypePtr& a0, const TypePtr& b0) {
    TypePtr a = I::Engine::repr(a0), b = I::Engine::repr(b0);
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
    if (auto* mi = std::get_if<Pmod_ident>(&me.desc))
      return resolve_module_values(mi->id.txt);  // local alias or stdlib (sub)module
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
      if (auto* fi = std::get_if<Pmod_ident>(&h->desc))
        return functor_result_values(fi->id.txt, napp);
      return {};
    }
    return {};  // functor definition itself: no values
  }

  // Process structure items into the current scope, populating modenv for
  // submodules.  Per-item best-effort (a bad item doesn't abort the rest).
  void process_items(const ast::Structure& items) {
    for (auto& it : items) process_item(it);
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
        else if (auto* ev = std::get_if<Pstr_eval>(&it.desc))
          infer_expr(*ev->e);
        else if (auto* op = std::get_if<Pstr_open>(&it.desc)) {
          for (auto& [k, v] : module_exports(op->expr)) venv.back()[k] = v;
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
              modenv[*mb->binding.name.txt] = module_exports(mb->binding.expr);
            }
          }
        } else if (auto* in = std::get_if<Pstr_include>(&it.desc)) {
          for (auto& [k, v] : module_exports(in->expr)) venv.back()[k] = v;
        } else if (auto* pr = std::get_if<Pstr_primitive>(&it.desc)) {
          if (pr->prim.type) {  // external f : t = "..." binds f : t
            std::unordered_map<std::string, TypePtr> vars;
            venv.back()[pr->prim.name.txt] = from_coretype(*pr->prim.type, vars);
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
    else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
      const ModuleExpr* me = &mb->binding.expr;
      while (auto* mc = std::get_if<Pmod_constraint>(&me->desc)) me = mc->me.get();
      if (auto* ms = std::get_if<Pmod_structure>(&me->desc))
        register_types_rec(ck, ms->items);
    }
  }
}

// Shared setup: register constructors, then run best-effort inference over the
// structure (populating ck.match_partial and ck.errors as it traverses).
static void run_checker(Checker& ck, const ast::Structure& s) {
  ck.register_predef_ctors();
  ck.register_stdlib_ctors();
  register_types_rec(ck, s);
  ck.finalize_fields();
  ck.check_cyclic_aliases();
  ck.process_items(s);
}

std::unordered_map<const ast::Expression*, bool> infer_match_partiality(
    const ast::Structure& s) {
  Checker ck;
  run_checker(ck, s);
  return std::move(ck.match_partial);
}

// The Lambda value_kind of an inferred type, as -dlambda spells it.
static std::string kind_str(const TypePtr& t0, const std::set<std::string>& imm) {
  TypePtr t = I::Engine::repr(t0);
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
  ValueKinds vk;
  for (auto& [p, t] : ck.rec_pat_) vk.pat[p] = kind_str(t, ck.immediate_types_);
  for (auto& [f, t] : ck.rec_ret_) vk.fn_ret[f] = kind_str(t, ck.immediate_types_);
  for (auto& [e, t] : ck.rec_expr_) {
    vk.expr[e] = kind_str(t, ck.immediate_types_);
    std::string ek;
    if (array_elem_str(t, ck.immediate_types_, ek)) vk.array_elem[e] = ek;  // "" = gen element
  }
  vk.format_lits = std::move(ck.fmt_lits_);
  return vk;
}

// Strict type-check: returns the definite type errors found (empty => accepted).
// Conservative — only DEFINITE errors (unqualified unbound value, type clash);
// unknown/unsupported constructs and qualified names are assumed OK so that
// engine incompleteness shows up as false-rejections to be driven out, not as
// spurious accepts.
std::vector<std::string> structure_typecheck(const ast::Structure& s) {
  Checker ck;
  ck.strict = true;
  run_checker(ck, s);
  return std::move(ck.errors);
}

std::vector<std::pair<std::string, std::string>> infer_structure_types(
    const ast::Structure& s) {
  Checker ck;
  ck.register_predef_ctors();
  // Pre-register variant constructors from all type decls.
  for (auto& it : s)
    if (auto* ty = std::get_if<Pstr_type>(&it.desc))
      for (auto& d : ty->decls) ck.register_type_decl(d);

  std::vector<std::pair<std::string, std::string>> out;
  for (auto& it : s) {
    auto* sv = std::get_if<Pstr_value>(&it.desc);
    if (!sv) continue;
    try {
      ck.infer_bindings(sv->rf, sv->bindings);
      // report the type bound to each simple var name
      for (auto& b : sv->bindings)
        if (auto* v = std::get_if<Ppat_var>(&b.pat.desc)) {
          auto f = ck.venv.back().find(v->name.txt);
          if (f != ck.venv.back().end())
            out.emplace_back(v->name.txt, I::show(f->second));
        }
    } catch (const I::TypeError&) {
    }
  }
  return out;
}

}  // namespace cppcaml
