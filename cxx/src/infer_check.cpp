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
  // Stdlib value schemes (loaded once, lazily).
  bool stdlib_ready_ = false;
  std::unordered_map<std::string, TypePtr> stdlib_;
  // Strict mode: record definite type errors instead of swallowing them.
  bool strict = false;
  std::vector<std::string> errors;
  void note_error(const std::string& m) { if (errors.size() < 100) errors.push_back(m); }

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
        if (is_format_base(p)) return eng.constr("format6");
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
      return eng.constr(lid_full(c->id.txt), std::move(as));
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
    // Qualified (M.x): submodule cmis not loaded yet — can't resolve, so we must
    // NOT reject (a known gap, not a type error).  Best-effort fresh var.
    return eng.fresh_var();
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
      if (sig)
        for (auto& v : sig->values) {
          std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
          out[v.name] = from_cmi(v.type, memo);
        }
    } catch (...) {}
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
    type_ctors["bool"] = {"false", "true"};
    type_ctors["option"] = {"None", "Some"};
    type_ctors["list"] = {"[]", "::"};
    type_ctors["unit"] = {"()"};
  }

  // Register a user variant: A of t1*..*tn -> scheme t1->..->tn->(params) name.
  void register_type_decl(const TypeDeclaration& d) {
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
    bool is_gadt = false;
    for (auto& c : v->ctors) { names.push_back(c.name.txt); if (c.res) is_gadt = true; }
    if (is_gadt) gadt_types.insert(d.name.txt);
    type_ctors[d.name.txt] = std::move(names);
    for (auto& c : v->ctors) {
      std::unordered_map<std::string, TypePtr> vars;
      std::vector<TypePtr> params;
      for (auto& p : d.params) params.push_back(from_coretype(*p, vars));
      TypePtr result = eng.constr(d.name.txt, params);
      TypePtr scheme = result;
      if (auto* tup = std::get_if<Pcstr_tuple>(&c.args)) {
        for (auto it = tup->elems.rbegin(); it != tup->elems.rend(); ++it)
          scheme = eng.arrow(from_coretype(**it, vars), scheme);
      }
      ctors[c.name.txt] = scheme;
    }
  }

  // Split a (instantiated) constructor scheme into its argument types and result.
  static std::vector<TypePtr> ctor_params(const TypePtr& sch, TypePtr& result) {
    std::vector<TypePtr> ps;
    TypePtr c = I::Engine::repr(sch);
    while (c->kind == I::Type::Kind::Arrow) { ps.push_back(c->dom); c = I::Engine::repr(c->cod); }
    result = c;
    return ps;
  }

  TypePtr try_(std::function<TypePtr()> f) {
    try { return f(); } catch (const I::TypeError&) { return eng.fresh_var(); }
  }
  void try_unify(const TypePtr& a, const TypePtr& b) {
    try { eng.unify(a, b); }
    catch (const I::TypeError& e) { if (strict) note_error(e.what()); }
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

  TypePtr infer_pat(const Pattern& p) {
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
      auto it = ctors.find(lid_last(k->id.txt));
      if (it == ctors.end()) {
        if (k->arg) infer_pat(**k->arg);
        return eng.fresh_var();
      }
      TypePtr result;
      auto ps = ctor_params(eng.instantiate(it->second), result);
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
      for (auto& [lid, sub] : r->fields) infer_pat(*sub);  // bind field vars
      return eng.fresh_var();
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

  // Infer an expression with an expected type pushed down (bidirectional).  The
  // only type-directed rule so far: a string literal expected at a format type
  // is accepted as that format (OCaml's type_format).  Otherwise it's ordinary
  // inference; the caller still unifies the result against the expected type.
  TypePtr infer_expr_expected(const Expression& e, const TypePtr& expected) {
    if (auto* c = std::get_if<Pexp_constant>(&e.desc))
      if (std::holds_alternative<Pconst_string>(c->c.desc) &&
          is_format_constr(expected))
        return expected;
    return infer_expr(e);
  }

  TypePtr infer_expr(const Expression& e) {
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
      auto it = ctors.find(lid_last(k->id.txt));
      if (it == ctors.end()) { if (k->arg) infer_expr(**k->arg); return eng.fresh_var(); }
      TypePtr result;
      auto ps = ctor_params(eng.instantiate(it->second), result);
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
    if (auto* m = std::get_if<Pexp_match>(&e.desc)) {
      TypePtr se = infer_expr(*m->e);
      TypePtr sr = I::Engine::repr(se);
      // Matching a GADT refines types per branch (e.g. Int -> int, Ptr ->
      // int list at result type 'a); cross-unifying the branch results would be
      // an unsound clash, so leave the result type open for GADT scrutinees.
      bool gadt = sr->kind == I::Type::Kind::Constr && gadt_types.count(sr->path);
      TypePtr rt = eng.fresh_var();
      for (auto& c : m->cases) {
        venv.emplace_back();
        try_unify(infer_pat(c.lhs), se);
        if (c.guard) infer_expr(**c.guard);
        TypePtr br = infer_expr(*c.rhs);
        if (!gadt) try_unify(br, rt);
        venv.pop_back();
      }
      match_partial[&e] = compute_partial(se, m->cases);  // for the dump (Slice 3)
      return rt;
    }
    if (auto* ct = std::get_if<Pexp_constraint>(&e.desc)) {
      infer_expr(*ct->e);
      std::unordered_map<std::string, TypePtr> vars;
      TypePtr at = from_coretype(*ct->t, vars);
      return at;
    }
    // record / field / variant / etc.: best-effort (record-field types need
    // type-directed disambiguation of shared labels; without it, a flat label
    // registry resolves to the wrong record type and clashes -- left opaque).
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

  TypePtr infer_apply(const Pexp_apply& a) {
    TypePtr ft = infer_expr(*a.fn);
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
        try_unify(spine[idx]->dom, at);
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
      try_unify(ft, eng.arrow(dom, r));
      TypePtr at = infer_expr_expected(*arg, dom);
      try_unify(dom, at);
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
        try_unify(infer_expr(*c.rhs), rt);
        venv.pop_back();
      }
      params.push_back({arg, 0, ""});
      body = rt;
    }
    TypePtr t = body;
    for (auto it = params.rbegin(); it != params.rend(); ++it)
      t = eng.arrow(it->ty, t, it->lk, it->nm);
    venv.pop_back();
    return t;
  }

  // Bind a let group (generalizing each RHS at the outer level).
  void infer_bindings(RecFlag rf, const std::vector<ValueBinding>& bs) {
    if (rf == RecFlag::Recursive) {
      // pre-bind monomorphic vars, infer, then nothing fancy (no generalization
      // of rec bindings in this best-effort pass)
      std::vector<TypePtr> tv;
      for (auto& b : bs) tv.push_back(infer_pat(b.pat));
      for (size_t i = 0; i < bs.size(); ++i)
        try_unify(tv[i], infer_expr(*bs[i].expr));
      return;
    }
    for (auto& b : bs) {
      eng.enter_level();
      TypePtr te = infer_expr(*b.expr);
      eng.leave_level();
      eng.generalize(te);
      bind_pattern_scheme(b.pat, te);
    }
  }

  // Bind a let pattern's variables to a (generalized) type.
  void bind_pattern_scheme(const Pattern& p, const TypePtr& te) {
    if (auto* v = std::get_if<Ppat_var>(&p.desc)) {
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
      process_items(ms->items);
      auto exports = std::move(venv.back());
      venv.pop_back();
      return exports;
    }
    if (auto* mi = std::get_if<Pmod_ident>(&me.desc))
      return resolve_module_values(mi->id.txt);  // local alias or stdlib (sub)module
    if (auto* mc = std::get_if<Pmod_constraint>(&me.desc)) {
      auto inner = module_exports(*mc->me);
      if (!inner.empty()) return inner;
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
        if (auto* sv = std::get_if<Pstr_value>(&it.desc))
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
    if (auto* ty = std::get_if<Pstr_type>(&it.desc))
      for (auto& d : ty->decls) ck.register_type_decl(d);
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
  register_types_rec(ck, s);
  ck.process_items(s);
}

std::unordered_map<const ast::Expression*, bool> infer_match_partiality(
    const ast::Structure& s) {
  Checker ck;
  run_checker(ck, s);
  return std::move(ck.match_partial);
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
