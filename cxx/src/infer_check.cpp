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
  // match-expression node -> is-partial (the result we route back to the dump)
  std::unordered_map<const Expression*, bool> match_partial;
  // Stdlib value schemes (loaded once, lazily).
  bool stdlib_ready_ = false;
  std::unordered_map<std::string, TypePtr> stdlib_;

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
        return eng.arrow(from_cmi(n->dom, memo), from_cmi(n->cod, memo));
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
    if (auto* a = std::get_if<Ptyp_arrow>(&t.desc))
      return eng.arrow(from_coretype(*a->dom, vars), from_coretype(*a->cod, vars));
    if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
      std::vector<TypePtr> es;
      for (auto& e : tu->elems) es.push_back(from_coretype(*e, vars));
      return eng.tuple(std::move(es));
    }
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      std::vector<TypePtr> as;
      for (auto& a : c->args) as.push_back(from_coretype(*a, vars));
      return eng.constr(lid_full(c->id.txt), std::move(as));
    }
    return eng.fresh_var();
  }

  TypePtr lookup_value(const std::string& name) {
    for (auto it = venv.rbegin(); it != venv.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return eng.instantiate(f->second);
    }
    auto& s = stdlib_schemes();
    auto f = s.find(name);
    if (f != s.end()) return eng.instantiate(f->second);
    return eng.fresh_var();  // unbound / qualified: best-effort
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
    if (std::holds_alternative<Pconst_integer>(c.desc)) return eng.constr("int");
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
    return eng.fresh_var();
  }

  TypePtr infer_expr(const Expression& e) {
    if (auto* c = std::get_if<Pexp_constant>(&e.desc)) return constant_type(c->c);
    if (auto* id = std::get_if<Pexp_ident>(&e.desc))
      return lookup_value(lid_last(id->id.txt));
    if (auto* a = std::get_if<Pexp_apply>(&e.desc)) {
      TypePtr ft = infer_expr(*a->fn);
      for (auto& [lbl, arg] : a->args) {
        TypePtr at = infer_expr(*arg);
        TypePtr r = eng.fresh_var();
        try_unify(ft, eng.arrow(at, r));
        ft = I::Engine::repr(r);
      }
      return ft;
    }
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
    // record / field / variant / etc.: best-effort
    if (auto* a = std::get_if<Pexp_array>(&e.desc)) {
      TypePtr el = eng.fresh_var();
      for (auto& x : a->elems) try_unify(el, infer_expr(*x));
      return eng.constr("array", {el});
    }
    return eng.fresh_var();
  }

  TypePtr infer_function(const Pexp_function& f) {
    venv.emplace_back();
    std::vector<TypePtr> params;
    for (auto& fp : f.params) {
      if (auto* pv = std::get_if<Pparam_val>(&fp.desc)) params.push_back(infer_pat(pv->pat));
      else params.push_back(eng.fresh_var());
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
      params.push_back(arg);
      body = rt;
    }
    TypePtr t = body;
    for (auto it = params.rbegin(); it != params.rend(); ++it) t = eng.arrow(*it, t);
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
};

}  // namespace

// Shared setup: register predef + all type-decl constructors, then run
// best-effort inference over every top-level value binding / eval expression
// (this is what populates ck.match_partial as it traverses).
static void run_checker(Checker& ck, const ast::Structure& s) {
  ck.register_predef_ctors();
  for (auto& it : s)
    if (auto* ty = std::get_if<Pstr_type>(&it.desc))
      for (auto& d : ty->decls) ck.register_type_decl(d);
  for (auto& it : s) {
    try {
      if (auto* sv = std::get_if<Pstr_value>(&it.desc))
        ck.infer_bindings(sv->rf, sv->bindings);
      else if (auto* ev = std::get_if<Pstr_eval>(&it.desc))
        ck.infer_expr(*ev->e);
    } catch (const I::TypeError&) {
    }
  }
}

std::unordered_map<const ast::Expression*, bool> infer_match_partiality(
    const ast::Structure& s) {
  Checker ck;
  run_checker(ck, s);
  return std::move(ck.match_partial);
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
