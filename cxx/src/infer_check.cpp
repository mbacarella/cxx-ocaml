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
  std::set<std::string> expanding_;  // guard against cyclic abbreviations
  // match-expression node -> is-partial (the result we route back to the dump)
  std::unordered_map<const Expression*, bool> match_partial;
  // local module name -> its exported value schemes (so open/include/M.x resolve)
  std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>> modenv;
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
        std::vector<TypePtr> as;
        for (auto& a : n->args) as.push_back(from_cmi(a, memo));
        return eng.constr(n->path ? cmi_path_str(*n->path) : "?", std::move(as));
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

  // Resolve a (possibly qualified) module path to its exported value schemes:
  // a local top-level module from modenv, else a stdlib module/submodule walked
  // through nested signatures (open Effect.Deep -> stdlib__Effect.cmi -> Deep).
  std::unordered_map<std::string, TypePtr> resolve_module_values(const Longident& m) {
    auto comps = mod_components(m);
    if (comps.empty()) return {};
    if (comps.size() == 1) {
      auto it = modenv.find(comps[0]);
      if (it != modenv.end()) return it->second;
    }
    std::unordered_map<std::string, TypePtr> out;
    try {
      const std::string& head = comps[0];
      auto cmi = (head == "Stdlib")
                     ? cmi::CmiFile::load("stdlib/stdlib.cmi")
                     : cmi::CmiFile::load("stdlib/stdlib__" + head + ".cmi");
      const cmi::Signature* sig = &cmi.sig();
      for (size_t i = 1; i < comps.size() && sig; ++i) {
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules)
          if (mm.name == comps[i]) { md = &mm; break; }
        if (md && md->type && md->type->kind == cmi::ModuleType::Sig && md->type->sig)
          sig = md->type->sig.get();
        else { sig = nullptr; }
      }
      if (sig)
        for (auto& v : sig->values) {
          std::unordered_map<cmi::TypeExpr*, TypePtr> memo;
          out[v.name] = from_cmi(v.type, memo);
        }
    } catch (...) {}
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
    for (auto& c : v->ctors) names.push_back(c.name.txt);
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
      TypePtr sch = eng.instantiate(it->second);
      if (k->arg) {
        // peel one or more arrow args against the (possibly tuple) sub-pattern
        if (auto* tup = std::get_if<Ppat_tuple>(&(*k->arg)->desc)) {
          for (auto& el : tup->elems) {
            TypePtr s = I::Engine::repr(sch);
            if (s->kind == I::Type::Kind::Arrow) { try_unify(s->dom, infer_pat(*el)); sch = s->cod; }
            else infer_pat(*el);
          }
        } else {
          TypePtr s = I::Engine::repr(sch);
          if (s->kind == I::Type::Kind::Arrow) { try_unify(s->dom, infer_pat(**k->arg)); sch = s->cod; }
          else infer_pat(**k->arg);
        }
      }
      return I::Engine::repr(sch);
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
    if (t->kind != I::Type::Kind::Constr) return false;
    auto dot = t->path.rfind('.');
    std::string base = dot == std::string::npos ? t->path : t->path.substr(dot + 1);
    return base == "format" || base == "format4" || base == "format6";
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
      TypePtr sch = eng.instantiate(it->second);
      if (k->arg) {
        std::vector<const Expression*> args;
        if (auto* tup = std::get_if<Pexp_tuple>(&(*k->arg)->desc))
          for (auto& el : tup->elems) args.push_back(el.get());
        else args.push_back(k->arg->get());
        for (auto* ae : args) {
          TypePtr s = I::Engine::repr(sch);
          if (s->kind == I::Type::Kind::Arrow) { try_unify(s->dom, infer_expr(*ae)); sch = s->cod; }
          else infer_expr(*ae);
        }
      }
      return I::Engine::repr(sch);
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
      TypePtr rt = eng.fresh_var();
      for (auto& c : m->cases) {
        venv.emplace_back();
        try_unify(infer_pat(c.lhs), se);
        if (c.guard) infer_expr(**c.guard);
        try_unify(infer_expr(*c.rhs), rt);
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
      venv.emplace_back();
      if (auto* op = std::get_if<Pstr_open>(&sti->item->desc))
        if (auto* mi = std::get_if<Pmod_ident>(&op->expr.desc)) open_into(mi->id.txt);
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
  TypePtr infer_apply(const Pexp_apply& a) {
    TypePtr ft = infer_expr(*a.fn);
    bool any_labelled = false;
    for (auto& [lbl, arg] : a.args)
      if (!std::holds_alternative<Nolabel>(lbl)) any_labelled = true;

    if (any_labelled) {
      // Collect the known arrow spine of the function type.
      std::vector<TypePtr> spine;
      TypePtr cur = I::Engine::repr(ft);
      while (cur->kind == I::Type::Kind::Arrow) {
        spine.push_back(cur);
        cur = I::Engine::repr(cur->cod);
      }
      TypePtr tail = cur;
      std::vector<bool> used(spine.size(), false);
      bool clean = true;
      for (auto& [lbl, arg] : a.args) {
        auto [lk, nm] = arglabel(lbl);
        int idx = -1;
        for (size_t i = 0; i < spine.size(); ++i) {
          if (used[i]) continue;
          if (lk == 0 ? spine[i]->arrow_label == 0
                      : (spine[i]->arrow_label != 0 && spine[i]->arrow_lbl == nm)) {
            idx = (int)i;
            break;
          }
        }
        if (idx < 0) { clean = false; break; }
        used[idx] = true;
        TypePtr at = infer_expr_expected(*arg, spine[idx]->dom);
        try_unify(spine[idx]->dom, at);
      }
      if (clean) {
        // result = the unconsumed params (in order) chained onto the tail
        TypePtr res = tail;
        for (int i = (int)spine.size() - 1; i >= 0; --i)
          if (!used[i])
            res = eng.arrow(spine[i]->dom, res, spine[i]->arrow_label, spine[i]->arrow_lbl);
        return res;
      }
      // fall through to positional peeling on partial/unknown match
    }

    for (auto& [lbl, arg] : a.args) {
      // Peel the function's expected domain first, so we can type-direct the
      // argument (bidirectional checking) rather than inferring it blindly.
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
    struct Param { TypePtr ty; int lk; std::string nm; };
    std::vector<Param> params;
    for (auto& fp : f.params) {
      // (type a) introduces a locally-abstract type, not a value argument, so it
      // contributes no arrow to the function's type.
      if (auto* pv = std::get_if<Pparam_val>(&fp.desc)) {
        auto [lk, nm] = arglabel(pv->label);
        params.push_back({infer_pat(pv->pat), lk, nm});
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
    if (auto* mc = std::get_if<Pmod_constraint>(&me.desc))
      return module_exports(*mc->me);  // ignore the constraint sig for now
    return {};  // functor / apply: deferred
  }

  // Process structure items into the current scope, populating modenv for
  // submodules.  Per-item best-effort (a bad item doesn't abort the rest).
  void process_items(const ast::Structure& items) {
    for (auto& it : items) {
      try {
        if (auto* sv = std::get_if<Pstr_value>(&it.desc))
          infer_bindings(sv->rf, sv->bindings);
        else if (auto* ev = std::get_if<Pstr_eval>(&it.desc))
          infer_expr(*ev->e);
        else if (auto* op = std::get_if<Pstr_open>(&it.desc)) {
          if (auto* mi = std::get_if<Pmod_ident>(&op->expr.desc))
            open_into(mi->id.txt);
        } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
          if (mb->binding.name.txt)
            modenv[*mb->binding.name.txt] = module_exports(mb->binding.expr);
        } else if (auto* in = std::get_if<Pstr_include>(&it.desc)) {
          for (auto& [k, v] : module_exports(in->expr)) venv.back()[k] = v;
        } else if (auto* pr = std::get_if<Pstr_primitive>(&it.desc)) {
          if (pr->prim.type) {  // external f : t = "..." binds f : t
            std::unordered_map<std::string, TypePtr> vars;
            venv.back()[pr->prim.name.txt] = from_coretype(*pr->prim.type, vars);
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
