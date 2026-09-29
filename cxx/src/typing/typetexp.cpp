// Port of typing/typetexp.ml: typechecking of type expressions for the core
// language.  See typetexp.hpp.
#include "cppcaml/typing/typetexp.hpp"

#include <algorithm>
#include <map>

#include "cppcaml/lexer.hpp"
#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/ocaml_list.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/typing_recovery.hpp"

namespace cppcaml::typing::typetexp {

using namespace types;
using namespace btype;
using EK = Error::Kind;
using pt::as;
using TK = tt::CoreTypeDesc::Kind;

// Pprintast.tyvar_of_name
std::string tyvar_of_name(std::string_view s) {
  if (s.size() >= 2 && s[1] == '\'') return "' " + std::string(s);
  if (cppcaml::is_ocaml_keyword(s)) return "'\\#" + std::string(s);
  if (s == "_") return std::string(s);
  return "'" + std::string(s);
}

[[noreturn]] static void raise_(const Error& e) { typing_recovery::log_and_raise(e); }
static Error err(const Location& loc, env::t env, EK k) { return Error(loc, env, k); }

// ---- TyVarEnv ------------------------------------------------------------------------------
namespace ty_var_env {

static bool not_generic(TypeExpr* v) { return get_level(v) != generic_level; }

// These are the "global" type variables: they were in scope before we
// started processing the current type.
static StrMap<std::pair<TypeExpr*, bool*>> type_variables;
// These are variables that have been used in the currently-being-checked type.
struct UsedVar {
  TypeExpr* ty;
  Location loc;
  bool* unused;
};
static StrMap<UsedVar> used_variables;
// Variables we expect to become univars (see typetexp.ml).
static PolyUnivars univars;  // head first
static void assert_univars(const PolyUnivars& uvs) {
  for (auto& [_, v] : uvs)
    if (!not_generic(v->univar)) throw std::logic_error("Typetexp.assert_univars");
}
// Variables that will become univars when we're done with the current type.
static std::vector<TypeExpr*> pre_univars;  // head first

void reset() {
  ctype::reset_global_level();
  type_variables = {};
}

bool is_in_scope(std::string_view name) { return type_variables.mem(name); }

static void add(std::string_view name, TypeExpr* v, bool* unused = nullptr) {
  if (!not_generic(v)) throw std::logic_error("Typetexp.TyVarEnv.add");
  if (!unused) unused = make<bool>(false);
  type_variables = type_variables.add(zborrow(name), {v, unused});
}

Context narrow() { return Context{ctype::increase_global_level(), type_variables}; }
void widen(const Context& c) {
  ctype::restore_global_level(c.gl);
  type_variables = c.tv;
}

// throws env::NotFound if the variable is not in scope
static TypeExpr* lookup_global_type_variable(std::string_view name) {
  const auto* p = type_variables.find_opt(name);
  if (!p) throw env::NotFound{};
  *p->second = false;
  return p->first;
}

static std::vector<std::string> get_in_scope_names() {
  std::vector<std::string> l;  // TyVarMap.fold ... [] : consed in increasing order
  type_variables.iter([&](std::string_view name, const std::pair<TypeExpr*, bool*>&) {
    if (name != "_") l.insert(l.begin(), tyvar_of_name(name));
  });
  return l;
}

template <class F>
static auto with_univars(const PolyUnivars& new_ones, F&& f) -> decltype(f()) {
  assert_univars(new_ones);
  PolyUnivars old = univars;
  PolyUnivars u = new_ones;
  u.insert(u.end(), univars.begin(), univars.end());
  univars = u;
  struct R {
    PolyUnivars old;
    ~R() { univars = std::move(old); }
  } r{std::move(old)};
  return f();
}

PolyUnivars make_poly_univars(const std::vector<std::string_view>& vars) {
  PolyUnivars out;
  for (auto name : vars)
    out.push_back({zborrow(name), make<PendingUnivar>(PendingUnivar{ctype::newvar(OptStr::of(name)), {}})});
  return out;
}

// fold_left: promoted variables are consed onto `promoted`
static std::vector<TypeExpr*> promote_generics_to_univars(std::vector<TypeExpr*> promoted,
                                                          const std::vector<TypeExpr*>& vars) {
  for (TypeExpr* v : vars) {
    if (auto* tv = as<Tvar>(get_desc(v)); tv && get_level(v) == generic_level) {
      set_type_desc(v, tunivar(tv->name));
      promoted.insert(promoted.begin(), v);
    }
  }
  return promoted;
}

std::vector<TypeExpr*> check_poly_univars(env::t env, const Location& loc, const PolyUnivars& vars) {
  std::vector<TypeExpr*> us;
  for (auto& [name, p] : vars) {
    TypeExpr* v = proxy(p->univar);
    if (auto* tv = as<Tvar>(get_desc(v)); tv && get_level(v) == generic_level) {
      set_type_desc(v, tunivar(tv->name));
    } else {
      Error e = err(loc, env, EK::Cannot_quantify);
      e.name = std::string(name);
      e.ty1 = v;
      raise_(e);
    }
    us.push_back(v);
  }
  // Since we are promoting variables to univars in
  // promote_generics_to_univars, even if a row variable is associated with
  // multiple univars we will promote it once (see typetexp.ml).
  for (auto& [_, p] : vars) {
    std::vector<TypeExpr*> enclosed_rows;
    for (TyOptRef* r : p->associated)
      if (r->contents) enclosed_rows.push_back(r->contents);
    us = promote_generics_to_univars(std::move(us), enclosed_rows);
  }
  return us;
}

std::vector<TypeExpr*> instance_poly_univars(env::t env, const Location& loc,
                                             const PolyUnivars& vars) {
  std::vector<TypeExpr*> vs = check_poly_univars(env, loc, vars);
  for (TypeExpr* v : vs) {
    auto* u = as<Tunivar>(get_desc(v));
    if (!u) throw std::logic_error("Typetexp.instance_poly_univars");
    set_type_desc(v, tvar(u->name));
  }
  return vs;
}

static void reset_locals(const PolyUnivars* uvs = nullptr) {
  PolyUnivars u = uvs ? *uvs : PolyUnivars{};
  assert_univars(u);
  univars = u;
  used_variables = {};
}

static void associate(const std::vector<TyOptRef*>& row_context, PendingUnivar* p) {
  for (TyOptRef* x : row_context)
    if (std::find(p->associated.begin(), p->associated.end(), x) == p->associated.end())
      p->associated.insert(p->associated.begin(), x);
}

// throws env::NotFound if the variable is not in scope
static TypeExpr* lookup_local(const std::vector<TyOptRef*>& row_context, std::string_view name) {
  for (auto& [n, p] : univars)
    if (n == name) {
      associate(row_context, p);
      return p->univar;
    }
  const UsedVar* u = used_variables.find_opt(name);
  if (!u) throw env::NotFound{};
  *u->unused = false;
  // This call to instance might be redundant (see typetexp.ml).
  return ctype::instance(u->ty);
}

// The `check` of an alias only schedules the unused-alias warning
// (warnings are not ported yet).
static void remember_used(std::string_view name, TypeExpr* v, const Location& loc) {
  if (!not_generic(v)) throw std::logic_error("Typetexp.remember_used");
  bool* unused = make<bool>(false);
  used_variables = used_variables.add(zborrow(name), UsedVar{v, loc, unused});
}

enum class Flavor { Unification, Universal };
enum class Extensibility { Extensible, Fixed };
struct Policy {
  Flavor flavor;
  Extensibility extensibility;
};
static constexpr Policy fixed_policy{Flavor::Unification, Extensibility::Fixed};
static constexpr Policy extensible_policy{Flavor::Unification, Extensibility::Extensible};
static constexpr Policy univars_policy{Flavor::Universal, Extensibility::Extensible};

static void add_pre_univar(TypeExpr* tv, const Policy& p) {
  if (p.flavor == Flavor::Universal) {
    if (!not_generic(tv)) throw std::logic_error("Typetexp.add_pre_univar");
    pre_univars.insert(pre_univars.begin(), tv);
  }
}

template <class F>
static auto collect_univars(F&& f) -> std::pair<decltype(f()), std::vector<TypeExpr*>> {
  pre_univars.clear();
  auto result = f();
  std::vector<TypeExpr*> univs = promote_generics_to_univars({}, pre_univars);
  return {result, univs};
}

static TypeExpr* new_var(const Policy& policy, OptStr name = OptStr::none()) {
  TypeExpr* tv = ctype::newvar(name);
  add_pre_univar(tv, policy);
  return tv;
}

static TypeExpr* new_any_var(const Location& loc, env::t env, const Policy& policy) {
  if (policy.extensibility == Extensibility::Fixed) raise_(err(loc, env, EK::No_type_wildcards));
  return new_var(policy);
}

static std::function<void()> globalize_used_variables(const Policy& policy, env::t env) {
  struct Pending {
    Location loc;
    TypeExpr* t1;
    TypeExpr* t2;
  };
  auto r = std::make_shared<std::vector<Pending>>();  // head first
  used_variables.iter([&](std::string_view name, const UsedVar& u) {
    if (!(policy.flavor == Flavor::Unification || is_in_scope(name))) return;
    TypeExpr* v = ctype::new_global_var();
    Snapshot snap = btype::snapshot();
    try {
      ctype::unify(env, v, u.ty);
    } catch (const ctype::Unify& e) {
      if (is_in_scope(name)) {
        Error er = err(u.loc, env, EK::Type_mismatch);
        er.trace = e.err;
        raise_(er);
      }
      btype::backtrack(snap);
      return;
    } catch (...) {
      btype::backtrack(snap);
      return;
    }
    TypeExpr* global_var;
    try {
      global_var = lookup_global_type_variable(name);
    } catch (const env::NotFound&) {
      if (policy.extensibility == Extensibility::Fixed && is_Tvar(u.ty)) {
        Error er = err(u.loc, env, EK::Unbound_type_variable);
        er.name = tyvar_of_name(name);
        er.names = get_in_scope_names();
        raise_(er);
      }
      TypeExpr* v2 = ctype::new_global_var();
      r->insert(r->begin(), Pending{u.loc, v, v2});
      add(name, v2, u.unused);
      return;
    }
    r->insert(r->begin(), Pending{u.loc, v, global_var});
    *u.unused = false;
  });
  used_variables = {};
  return [r, env] {
    for (auto& p : *r) {
      try {
        ctype::unify(env, p.t1, p.t2);
      } catch (const ctype::Unify& e) {
        Error er = err(p.loc, env, EK::Type_mismatch);
        er.trace = e.err;
        raise_(er);
      }
    }
  };
}

}  // namespace ty_var_env

using ty_var_env::Policy;
using RowContext = std::vector<TyOptRef*>;

// ---- support for first-class modules ---------------------------------------------------
std::function<std::pair<Path::t, env::t>(std::shared_ptr<bool>, OverrideFlag, env::t, const Location&,
                                         const pt::LidLoc&)>
    type_open;
std::function<Path::t(const Location&, env::t, Longident::t)> transl_modtype_longident;
std::function<const tt::ModuleType*(env::t, const pt::ModuleType*)> transl_modtype;
std::function<const ModuleType*(const Location&, env::t, const ModuleType*, bool,
                                Slice<std::pair<pt::LidLoc, const tt::CoreType*>>)>
    check_package_with_type_constraints;

using Constraint = std::pair<pt::LidLoc, const pt::CoreType*>;
static std::vector<Constraint> sort_constraints_no_duplicates(const Location& loc, env::t env,
                                                              Slice<Constraint> l) {
  std::vector<Constraint> v(l.begin(), l.end());
  return ocaml_list::stable_sort(
      [&](const Constraint& a, const Constraint& b) {
        if (longident::same(a.first.txt, b.first.txt)) {
          Error e = err(loc, env, EK::Multiple_constraints_on_type);
          e.lid = a.first.txt;
          raise_(e);
        }
        return longident::compare_poly(a.first.txt, b.first.txt);
      },
      v);
}

// ---- translation of type expressions ---------------------------------------------------
static bool strict_ident(char c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static OptStr validate_name(OptStr name) {
  if (name.some && !name.v.empty() && strict_ident(name.v[0])) return name;
  return OptStr::none();
}
static TypeExpr* new_global_var(OptStr name = OptStr::none()) {
  return ctype::new_global_var(validate_name(name));
}
static TypeExpr* newvar(OptStr name = OptStr::none()) { return ctype::newvar(validate_name(name)); }

bool valid_tyvar_name(std::string_view name) { return !name.empty() && name[0] != '_'; }

static void check_tyvar_name(env::t env, const Location& loc, std::string_view name) {
  if (!valid_tyvar_name(name)) {
    Error e = err(loc, env, EK::Invalid_variable_name);
    e.name = "'" + std::string(name);
    raise_(e);
  }
}

static tt::CoreType* mk_ctyp(const tt::CoreTypeDesc* d, TypeExpr* ty, env::t env,
                             const pt::CoreType* styp) {
  return make<tt::CoreType>(d, ty, env, styp->ptyp_loc, styp->ptyp_attributes);
}

static const tt::CoreType* transl_type_param_(env::t env, const pt::CoreType* styp) {
  const Location& loc = styp->ptyp_loc;
  const pt::CoreTypeDesc* d = styp->ptyp_desc;
  if (d->kind == pt::CoreTypeDesc::Kind::Ptyp_any) {
    // `new_global_var ~name:"_"`: `Some "_"` is one static constant
    static const OptStr underscore = [] {
      ZoneScope perm(permanent_zone());
      return OptStr::of("_");
    }();
    TypeExpr* ty = new_global_var(underscore);
    return make<tt::CoreType>(make<tt::Ttyp_any>(TK::Ttyp_any), ty, env, loc, styp->ptyp_attributes);
  }
  if (auto* v = as<pt::Ptyp_var>(d)) {
    check_tyvar_name(env::empty(), loc, v->name);
    if (ty_var_env::is_in_scope(v->name)) throw AlreadyBound{};
    TypeExpr* ty = new_global_var(OptStr::of(v->name));
    ty_var_env::add(v->name, ty);
    return make<tt::CoreType>(make<tt::Ttyp_var>(tt::Ttyp_var{{TK::Ttyp_var}, zborrow(v->name)}), ty, env,
                              loc, styp->ptyp_attributes);
  }
  throw std::logic_error("Typetexp.transl_type_param");
}

const tt::CoreType* transl_type_param(env::t env, const pt::CoreType* styp) {
  // Currently useless, since type parameters cannot hold attributes.
  return builtin_attributes::warning_scope(styp->ptyp_attributes,
                                           [&] { return transl_type_param_(env, styp); });
}

static const tt::CoreType* transl_type(env::t env, const Policy& policy, bool aliased,
                                       const RowContext& row_context, const pt::CoreType* styp);
static const tt::CoreType* transl_type_aux(env::t env, const RowContext& row_context, bool aliased,
                                           const Policy& policy, const pt::CoreType* styp);
static std::pair<TypeExpr*, Slice<const tt::ObjectField*>> transl_fields(
    env::t env, const Policy& policy, const RowContext& row_context, ClosedFlag o,
    Slice<const pt::ObjectField*> fields);
struct TranslatedPackage {
  const Package* pack;
  const ModuleType* mty;  // ComputeMType; nullptr for NoMType
  Slice<std::pair<pt::LidLoc, const tt::CoreType*>> ptys;
};
static TranslatedPackage transl_package(env::t env, const Policy& policy,
                                        const RowContext& row_context, bool compute,
                                        const pt::PackageType* ptyp);

static const tt::CoreType* transl_type(env::t env, const Policy& policy, bool aliased,
                                       const RowContext& row_context, const pt::CoreType* styp) {
  // (-typing-recovery is never on in batch ocamlc)
  return builtin_attributes::warning_scope(styp->ptyp_attributes, [&] {
    return transl_type_aux(env, row_context, aliased, policy, styp);
  });
}

static void type_mismatch_swapped(const Location& loc, env::t env, const ctype::Unify& u, EK k) {
  Error e = err(loc, env, k);
  e.trace = ctype::et::swap_unification_error(u.err);
  raise_(e);
}

static const RowField* rf_present_opt(TypeExpr* t) { return rf_present(t); }

static const tt::CoreType* transl_type_aux(env::t env, const RowContext& row_context, bool aliased,
                                           const Policy& policy, const pt::CoreType* styp) {
  const Location& loc = styp->ptyp_loc;
  auto ctyp = [&](const tt::CoreTypeDesc* d, TypeExpr* ty) { return mk_ctyp(d, ty, env, styp); };
  const pt::CoreTypeDesc* sd = styp->ptyp_desc;
  using PK = pt::CoreTypeDesc::Kind;
  switch (sd->kind) {
    case PK::Ptyp_any: {
      TypeExpr* ty = ty_var_env::new_any_var(styp->ptyp_loc, env, policy);
      return ctyp(make<tt::Ttyp_any>(TK::Ttyp_any), ty);
    }
    case PK::Ptyp_var: {
      std::string_view name = as<pt::Ptyp_var>(sd)->name;
      check_tyvar_name(env, styp->ptyp_loc, name);
      TypeExpr* ty;
      try {
        ty = ty_var_env::lookup_local(row_context, name);
      } catch (const env::NotFound&) {
        TypeExpr* v = ty_var_env::new_var(policy, OptStr::of(name));
        ty_var_env::remember_used(name, v, styp->ptyp_loc);
        ty = v;
      }
      return ctyp(make<tt::Ttyp_var>(tt::Ttyp_var{{TK::Ttyp_var}, zborrow(name)}), ty);
    }
    case PK::Ptyp_arrow: {
      auto* a = as<pt::Ptyp_arrow>(sd);
      const tt::CoreType* arg_cty = transl_type(env, policy, false, row_context, a->t1);
      const tt::CoreType* ret_cty = transl_type(env, policy, false, row_context, a->t2);
      TypeExpr* arg_ty = arg_cty->ctyp_type;
      arg_ty = is_Tpoly(arg_ty) ? arg_ty : ctype::newmono(arg_ty);
      if (a->label.kind == ArgLabel::Kind::Optional) {
        if (!tpoly_is_mono(arg_ty)) {
          Error e = err(a->t1->ptyp_loc, env, EK::Polymorphic_optional_param);
          e.name = std::string(a->label.name);
          raise_(e);
        }
        arg_ty = ctype::newmono(ctype::newconstr(predef::paths().option, slice({tpoly_get_mono(arg_ty)})));
      }
      TypeExpr* ty = ctype::newty(tarrow(a->label, arg_ty, ret_cty->ctyp_type, commu_ok()));
      return ctyp(make<tt::Ttyp_arrow>(tt::Ttyp_arrow{{TK::Ttyp_arrow}, a->label, arg_cty, ret_cty}), ty);
    }
    case PK::Ptyp_tuple: {
      auto* t = as<pt::Ptyp_tuple>(sd);
      if (t->tl.size() < 2) throw std::logic_error("Typetexp: tuple arity");
      {  // Misc.repeated_label
        std::vector<std::string_view> seen;
        for (auto& e : t->tl) {
          if (!e.label.some) continue;
          if (std::find(seen.begin(), seen.end(), e.label.v) != seen.end()) {
            Error er = err(loc, env, EK::Repeated_tuple_label);
            er.name = std::string(e.label.v);
            raise_(er);
          }
          seen.push_back(e.label.v);
        }
      }
      std::vector<tt::LabeledCoreType> ctys;
      for (auto& e : t->tl) ctys.push_back({e.label, transl_type(env, policy, false, row_context, e.ty)});
      std::vector<LabeledTy> tys;
      for (auto& c : ctys) tys.push_back({c.label, c.ty->ctyp_type});
      TypeExpr* ty = ctype::newty(ttuple(slice(tys)));
      return ctyp(make<tt::Ttyp_tuple>(tt::Ttyp_tuple{{TK::Ttyp_tuple}, slice(ctys)}), ty);
    }
    case PK::Ptyp_constr: {
      auto* c = as<pt::Ptyp_constr>(sd);
      auto [path, decl] = env::lookup_type(true, c->lid.loc, c->lid.txt, env);
      std::vector<const pt::CoreType*> stl(c->args.begin(), c->args.end());
      if (stl.size() == 1 && stl[0]->ptyp_desc->kind == PK::Ptyp_any && decl->type_arity > 1)
        stl.assign(decl->type_params.size(), stl[0]);
      if (static_cast<long>(stl.size()) != decl->type_arity) {
        Error e = err(styp->ptyp_loc, env, EK::Type_arity_mismatch);
        e.lid = c->lid.txt;
        e.expected = decl->type_arity;
        e.provided = static_cast<long>(stl.size());
        raise_(e);
      }
      std::vector<const tt::CoreType*> args;
      for (auto* st : stl) args.push_back(transl_type(env, policy, false, row_context, st));
      std::vector<TypeExpr*> params =
          ctype::instance_list(std::vector<TypeExpr*>(decl->type_params.begin(), decl->type_params.end()));
      bool use_unify_var = !decl->type_manifest || get_level(decl->type_manifest) == generic_level;
      if (stl.size() != params.size()) throw std::invalid_argument("List.iter2");
      for (std::size_t k = 0; k < stl.size(); ++k) {
        try {
          if (use_unify_var) ctype::unify_var(env, params[k], args[k]->ctyp_type);
          else ctype::unify(env, params[k], args[k]->ctyp_type);
        } catch (const ctype::Unify& u) {
          type_mismatch_swapped(stl[k]->ptyp_loc, env, u, EK::Type_mismatch);
        }
      }
      std::vector<TypeExpr*> tys;
      for (auto* a : args) tys.push_back(a->ctyp_type);
      TypeExpr* constr = ctype::newconstr(path, slice(tys));
      return ctyp(make<tt::Ttyp_constr>(tt::Ttyp_constr{{TK::Ttyp_constr}, path, c->lid, slice(args)}),
                  constr);
    }
    case PK::Ptyp_object: {
      auto* o = as<pt::Ptyp_object>(sd);
      auto [ty, fields] = transl_fields(env, policy, row_context, o->closed, o->fields);
      TypeExpr* obj = ctype::newobj(ty);
      return ctyp(make<tt::Ttyp_object>(tt::Ttyp_object{{TK::Ttyp_object}, fields, o->closed}), obj);
    }
    case PK::Ptyp_class: {
      auto* c = as<pt::Ptyp_class>(sd);
      auto [path, cdecl] = env::lookup_cltype(true, c->lid.loc, c->lid.txt, env);
      const TypeDeclaration* decl = cdecl->clty_hash_type;
      if (static_cast<long>(c->args.size()) != decl->type_arity) {
        Error e = err(styp->ptyp_loc, env, EK::Type_arity_mismatch);
        e.lid = c->lid.txt;
        e.expected = decl->type_arity;
        e.provided = static_cast<long>(c->args.size());
        raise_(e);
      }
      std::vector<const tt::CoreType*> args;
      for (auto* st : c->args) args.push_back(transl_type(env, policy, false, row_context, st));
      TypeExpr* body = decl->type_manifest;
      if (!body) throw std::invalid_argument("Option.get");
      auto [params, body2] = ctype::instance_parameterized_type(decl->type_params, body);
      if (c->args.size() != params.size()) throw std::invalid_argument("List.iter2");
      for (std::size_t k = 0; k < params.size(); ++k) {
        try {
          ctype::unify_var(env, params[k], args[k]->ctyp_type);
        } catch (const ctype::Unify& u) {
          type_mismatch_swapped(c->args[k]->ptyp_loc, env, u, EK::Type_mismatch);
        }
      }
      std::vector<TypeExpr*> ty_args;
      for (auto* a : args) ty_args.push_back(a->ctyp_type);
      TypeExpr* ty = ctype::apply(env, slice(params), body2, slice(ty_args), true);
      auto* ob = as<Tobject>(get_desc(ty));
      if (!ob) throw std::logic_error("Typetexp: Ptyp_class");
      auto [_, tv] = ctype::flatten_fields(ob->fields);
      ty_var_env::add_pre_univar(tv, policy);
      return ctyp(make<tt::Ttyp_class>(tt::Ttyp_class{{TK::Ttyp_class}, path, c->lid, slice(args)}), ty);
    }
    case PK::Ptyp_alias: {
      auto* a = as<pt::Ptyp_alias>(sd);
      const pt::StrLoc& alias = a->name;
      const tt::CoreType* cty;
      try {
        check_tyvar_name(env, alias.loc, alias.txt);
        TypeExpr* t = ty_var_env::lookup_local(row_context, alias.txt);
        const tt::CoreType* ty = transl_type(env, policy, true, row_context, a->ty);
        try {
          ctype::unify_var(env, t, ty->ctyp_type);
        } catch (const ctype::Unify& u) {
          type_mismatch_swapped(alias.loc, env, u, EK::Alias_type_mismatch);
        }
        cty = ty;
      } catch (const env::NotFound&) {
        auto [t0, ty] = ctype::with_local_level_generalize_structure_if_principal([&] {
          TypeExpr* t = newvar();
          // Use the whole location, which is used by [Type_mismatch].
          ty_var_env::remember_used(alias.txt, t, styp->ptyp_loc);
          const tt::CoreType* ty = transl_type(env, policy, false, row_context, a->ty);
          try {
            ctype::unify_var(env, t, ty->ctyp_type);
          } catch (const ctype::Unify& u) {
            type_mismatch_swapped(alias.loc, env, u, EK::Alias_type_mismatch);
          }
          return std::make_pair(t, ty);
        });
        TypeExpr* t = ctype::instance(t0);
        TypeExpr* px = proxy(t);
        const TypeDesc* pd = get_desc(px);
        if (auto* v = as<Tvar>(pd); v && !v->name.some) set_type_desc(px, tvar(OptStr::of(alias.txt)));
        else if (auto* u = as<Tunivar>(pd); u && !u->name.some)
          set_type_desc(px, tunivar(OptStr::of(alias.txt)));
        tt::CoreType* c = make<tt::CoreType>(*ty);
        c->ctyp_type = t;
        cty = c;
      }
      return ctyp(make<tt::Ttyp_alias>(tt::Ttyp_alias{{TK::Ttyp_alias}, cty, alias}), cty->ctyp_type);
    }
    case PK::Ptyp_variant: {
      auto* v = as<pt::Ptyp_variant>(sd);
      const PathArgs* name = nullptr;
      auto mkfield = [&](std::string_view l, const RowField* f) {
        TypeExpr* more = newvar();
        return ctype::newty(tvariant(create_row(slice({RowFieldEntry{l, f}}), more, true, nullptr, nullptr)));
      };
      // Using a reference to a map rather than a hash table gives us a
      // canonical order when iterating.
      std::map<long, std::pair<std::string_view, const RowField*>> hfields;
      auto add_typed_field = [&](const Location& floc, std::string_view l, const RowField* f) {
        long h = hash_variant(l);
        auto it = hfields.find(h);
        if (it == hfields.end()) {
          hfields.emplace(h, std::make_pair(zborrow(l), f));
          return;
        }
        auto [l2, f2] = it->second;
        // Check for tag conflicts
        if (l != l2) {
          Error e = err(styp->ptyp_loc, env, EK::Variant_tags);
          e.name = std::string(l);
          e.name2 = std::string(l2);
          raise_(e);
        }
        // `let ty = .. and ty' = ..`: left to right
        TypeExpr* ty = mkfield(l, f);
        TypeExpr* ty2 = mkfield(l, f2);
        if (ctype::is_equal(env, false, slice({ty}), slice({ty2}))) return;
        try {
          ctype::unify(env, ty, ty2);
        } catch (const ctype::Unify&) {
          Error e = err(floc, env, EK::Constructor_mismatch);
          e.ty1 = ty;
          e.ty2 = ty2;
          raise_(e);
        }
      };
      auto mem_present = [&](std::string_view l) {
        for (auto x : v->labels)
          if (x == l) return true;
        return false;
      };
      auto add_field = [&](const RowContext& rc, const pt::RowField* field) -> const tt::RowField* {
        tt::RowFieldDesc rf_desc{};
        if (auto* rt = as<pt::Rtag>(field->prf_desc)) {
          name = nullptr;
          std::vector<const tt::CoreType*> tl = builtin_attributes::warning_scope(field->prf_attributes, [&] {
            std::vector<const tt::CoreType*> out;
            for (auto* st : rt->types) out.push_back(transl_type(env, policy, false, rc, st));
            return out;
          });
          const RowField* f;
          if (v->has_labels && !mem_present(rt->label.txt)) {
            std::vector<TypeExpr*> ty_tl;
            for (auto* c : tl) ty_tl.push_back(c->ctyp_type);
            f = rf_either(nullptr, rt->constant, slice(ty_tl), false);
          } else {
            if (rt->types.size() > 1 || (rt->constant && !rt->types.empty())) {
              Error e = err(styp->ptyp_loc, env, EK::Present_has_conjunction);
              e.name = std::string(rt->label.txt);
              raise_(e);
            }
            f = tl.empty() ? RF_PRESENT_NONE_LIT() : rf_present_opt(tl[0]->ctyp_type);
          }
          add_typed_field(styp->ptyp_loc, rt->label.txt, f);
          rf_desc.is_tag = true;
          rf_desc.label = rt->label;
          rf_desc.constant = rt->constant;
          rf_desc.types = slice(tl);
        } else {
          const pt::CoreType* sty = as<pt::Rinherit>(field->prf_desc)->ty;
          const tt::CoreType* cty = transl_type(env, policy, false, rc, sty);
          TypeExpr* ty = cty->ctyp_type;
          const PathArgs* nm = nullptr;
          if (auto* c = as<Tconstr>(get_desc(cty->ctyp_type))) nm = make<PathArgs>(c->path, c->args);
          name = hfields.empty() ? nm : nullptr;
          Slice<RowFieldEntry> fl;
          const TypeDesc* ed = get_desc(ctype::expand_head(env, cty->ctyp_type));
          if (auto* vr = as<Tvariant>(ed); vr && static_row(vr->row)) {
            fl = row_fields(vr->row);
          } else if (ed->kind == DescKind::Tvar && nm) {
            Error e = err(sty->ptyp_loc, env, EK::Undefined_type_constructor);
            e.path = nm->path;
            raise_(e);
          } else {
            Error e = err(sty->ptyp_loc, env, EK::Not_a_variant);
            e.ty1 = ty;
            raise_(e);
          }
          for ([[maybe_unused]] auto& [l, f0, l_obj] : fl) {
            const RowField* f = f0;
            if (v->has_labels && !mem_present(l)) {
              RowFieldView fv = row_field_repr(f0);
              if (fv.kind != RowFieldView::Kind::Rpresent) throw std::logic_error("Typetexp: Rinherit");
              f = rf_either_of(fv.present);
            }
            add_typed_field(sty->ptyp_loc, l, f);
          }
          rf_desc.is_tag = false;
          rf_desc.inherit = cty;
        }
        return make<tt::RowField>(rf_desc, field->prf_loc, field->prf_attributes);
      };
      TyOptRef* more_slot = make<TyOptRef>(TyOptRef{nullptr});
      RowContext rc = row_context;
      if (!aliased) rc.insert(rc.begin(), more_slot);
      std::vector<const tt::RowField*> tfields;
      for (auto* f : v->fields) tfields.push_back(add_field(rc, f));
      // HMap.fold (fun _ p l -> p :: l): decreasing hash order
      std::vector<RowFieldEntry> fields;
      for (auto it = hfields.rbegin(); it != hfields.rend(); ++it)
        fields.push_back({it->second.first, it->second.second});
      if (v->has_labels) {
        for (auto l : v->labels) {
          bool found = false;
          for (auto& e : fields)
            if (e.label == l) found = true;
          if (!found) {
            Error e = err(styp->ptyp_loc, env, EK::Present_has_no_type);
            e.name = std::string(l);
            raise_(e);
          }
        }
      }
      auto make_row = [&](TypeExpr* more) {
        return create_row(slice(fields), more, v->closed == ClosedFlag::Closed, nullptr, name);
      };
      TypeExpr* more = static_row(make_row(newvar())) ? ctype::newty(tnil())
                                                      : ty_var_env::new_var(policy);
      more_slot->contents = more;
      TypeExpr* ty = ctype::newty(tvariant(make_row(more)));
      return ctyp(make<tt::Ttyp_variant>(tt::Ttyp_variant{{TK::Ttyp_variant}, slice(tfields), v->closed,
                                                          v->has_labels, v->labels}),
                  ty);
    }
    case PK::Ptyp_poly: {
      auto* p = as<pt::Ptyp_poly>(sd);
      std::vector<std::string_view> vars;
      for (auto& x : p->vars) vars.push_back(x.txt);
      auto [new_univars, cty] = ctype::with_local_level_generalize([&] {
        ty_var_env::PolyUnivars nu = ty_var_env::make_poly_univars(vars);
        const tt::CoreType* c = ty_var_env::with_univars(nu, [&] {
          return transl_type(env, policy, false, row_context, p->ty);
        });
        return std::make_pair(nu, c);
      });
      TypeExpr* ty = cty->ctyp_type;
      std::vector<TypeExpr*> ty_list = ty_var_env::check_poly_univars(env, styp->ptyp_loc, new_univars);
      std::vector<TypeExpr*> kept;
      for (TypeExpr* x : ty_list)
        if (deep_occur(x, ty)) kept.push_back(x);
      TypeExpr* ty2 = newgenty(tpoly(ty, slice(kept)));
      ctype::unify_var(env, newvar(), ty2);
      return ctyp(make<tt::Ttyp_poly>(tt::Ttyp_poly{{TK::Ttyp_poly}, slice(vars), cty}), ty2);
    }
    case PK::Ptyp_package: {
      const pt::PackageType* ptyp = as<pt::Ptyp_package>(sd)->pack;
      TranslatedPackage tp = transl_package(env, policy, row_context, false, ptyp);
      TypeExpr* ty = ctype::newty(tpackage(tp.pack));
      auto* pk = make<tt::PackageType>(tt::PackageType{tp.pack->pack_path, tp.ptys, tp.pack, ptyp->ppt_path});
      return ctyp(make<tt::Ttyp_package>(tt::Ttyp_package{{TK::Ttyp_package}, pk}), ty);
    }
    case PK::Ptyp_open: {
      auto* o = as<pt::Ptyp_open>(sd);
      auto [path, new_env] = type_open(nullptr, OverrideFlag::Fresh, env, loc, o->lid);
      const tt::CoreType* cty = transl_type(new_env, policy, false, row_context, o->ty);
      return ctyp(make<tt::Ttyp_open>(tt::Ttyp_open{{TK::Ttyp_open}, path, o->lid, cty}), cty->ctyp_type);
    }
    case PK::Ptyp_extension:
      throw ErrorForward(as<pt::Ptyp_extension>(sd)->ext);
    case PK::Ptyp_functor: {
      auto* f = as<pt::Ptyp_functor>(sd);
      const pt::PackageType* ptyp = f->pack;
      if (f->label.kind == ArgLabel::Kind::Optional) {
        Error e = err(ptyp->ppt_loc, env, EK::Functor_optional_param);
        e.name = std::string(f->label.name);
        raise_(e);
      }
      TranslatedPackage tp = transl_package(env, policy, row_context, true, ptyp);
      TypeExpr* t = newvar();
      ident::Unscoped* ident = ident::Unscoped::create(f->name.txt);
      struct R {
        Ident::t scoped_ident;
        const tt::CoreType* cty;
        TypeExpr* ty;
      };
      R r = ctype::with_local_level([&] {
        Ident::t scoped_ident =
            Ident::create_scoped(static_cast<int>(ctype::get_current_level()), f->name.txt);
        env::t env2 = env::add_module(scoped_ident, ModulePresence::Mp_present, tp.mty, env);
        const tt::CoreType* cty = transl_type(env2, policy, false, row_context, f->ty);
        TypeExpr* ctyp_type = ctype::instance_funct(scoped_ident, Path::pident(Ident::of_unscoped(ident)),
                                                    false, cty->ctyp_type);
        TypeExpr* ty = ctype::newty(tfunctor(f->label, ident, tp.pack, ctyp_type));
        // Here we reduce the level of [cty] before leaving the local level
        try {
          ctype::unify(env2, ty, t);
        } catch (const ctype::Unify& u) {
          Error e = err(loc, env2, EK::Type_mismatch);
          e.trace = u.err;
          raise_(e);
        }
        return R{scoped_ident, cty, ty};
      });
      auto* pk = make<tt::PackageType>(tt::PackageType{tp.pack->pack_path, tp.ptys, tp.pack, ptyp->ppt_path});
      return ctyp(make<tt::Ttyp_functor>(tt::Ttyp_functor{{TK::Ttyp_functor}, f->label, r.scoped_ident,
                                                          f->name.loc, pk, r.cty}),
                  r.ty);
    }
  }
  throw std::logic_error("Typetexp.transl_type_aux");
}

// Ast_helper.Typ.force_poly
static const pt::CoreType* force_poly(const pt::CoreType* t) {
  if (t->ptyp_desc->kind == pt::CoreTypeDesc::Kind::Ptyp_poly) return t;
  auto* d = make<pt::Ptyp_poly>(pt::Ptyp_poly{{pt::CoreTypeDesc::Kind::Ptyp_poly}, Slice<pt::StrLoc>{}, t});
  return make<pt::CoreType>(d, t->ptyp_loc, pt::LocationStack{}, pt::Attributes{});
}

static std::pair<TypeExpr*, Slice<const tt::ObjectField*>> transl_fields(
    env::t env, const Policy& policy, const RowContext& row_context, ClosedFlag o,
    Slice<const pt::ObjectField*> fields) {
  // Using a reference to a map rather than a hash table gives us a canonical
  // order when iterating.
  std::map<std::string_view, TypeExpr*> hfields;
  auto add_typed_field = [&](const Location& loc, std::string_view l, TypeExpr* ty) {
    auto it = hfields.find(l);
    if (it == hfields.end()) {
      hfields.emplace(zborrow(l), ty);
      return;
    }
    TypeExpr* ty2 = it->second;
    if (ctype::is_equal(env, false, slice({ty}), slice({ty2}))) return;
    try {
      ctype::unify(env, ty, ty2);
    } catch (const ctype::Unify&) {
      Error e = err(loc, env, EK::Method_mismatch);
      e.name = std::string(l);
      e.ty1 = ty;
      e.ty2 = ty2;
      raise_(e);
    }
  };
  auto add_field = [&](const pt::ObjectField* f) -> const tt::ObjectField* {
    tt::ObjectFieldDesc of_desc{};
    if (auto* ot = as<pt::Otag>(f->pof_desc)) {
      const tt::CoreType* ty1 = builtin_attributes::warning_scope(f->pof_attributes, [&] {
        return transl_type(env, policy, false, row_context, force_poly(ot->ty));
      });
      of_desc = tt::ObjectFieldDesc{true, ot->label, ty1};
      add_typed_field(ty1->ctyp_loc, ot->label.txt, ty1->ctyp_type);
    } else {
      const pt::CoreType* sty = as<pt::Oinherit>(f->pof_desc)->ty;
      const tt::CoreType* cty = transl_type(env, policy, false, row_context, sty);
      Path::t nm = nullptr;
      if (auto* c = as<Tconstr>(get_desc(cty->ctyp_type))) nm = c->path;
      TypeExpr* t = ctype::expand_head(env, cty->ctyp_type);
      auto* ob = as<Tobject>(get_desc(t));
      DescKind tfk = ob ? get_desc(ob->fields)->kind : DescKind::Tvar;
      if (ob && (tfk == DescKind::Tfield || tfk == DescKind::Tnil)) {
        if (ctype::opened_object(t)) {
          Error e = err(sty->ptyp_loc, env, EK::Opened_object);
          e.path = nm;
          raise_(e);
        }
        for (TypeExpr* ty = ob->fields;;) {
          const TypeDesc* d = get_desc(ty);
          if (auto* fl = as<Tfield>(d)) {
            add_typed_field(sty->ptyp_loc, fl->label, fl->ty);
            ty = fl->rest;
          } else if (d->kind == DescKind::Tnil) {
            break;
          } else {
            throw std::logic_error("Typetexp.transl_fields");
          }
        }
        of_desc = tt::ObjectFieldDesc{false, pt::StrLoc{}, cty};
      } else if (get_desc(t)->kind == DescKind::Tvar && nm) {
        Error e = err(sty->ptyp_loc, env, EK::Undefined_type_constructor);
        e.path = nm;
        raise_(e);
      } else {
        Error e = err(sty->ptyp_loc, env, EK::Not_an_object);
        e.ty1 = t;
        raise_(e);
      }
    }
    return make<tt::ObjectField>(of_desc, f->pof_loc, f->pof_attributes);
  };
  std::vector<const tt::ObjectField*> object_fields;
  for (auto* f : fields) object_fields.push_back(add_field(f));
  // HMap.fold (fun s ty l -> (s, ty) :: l): decreasing name order
  std::vector<std::pair<std::string_view, TypeExpr*>> fl(hfields.rbegin(), hfields.rend());
  TypeExpr* ty = o == ClosedFlag::Closed ? ctype::newty(tnil()) : ty_var_env::new_var(policy);
  for (auto& [s, ty2] : fl) ty = ctype::newty(tfield(s, field_public(), ty2, ty));
  return {ty, slice(object_fields)};
}

static TranslatedPackage transl_package(env::t env, const Policy& policy,
                                        const RowContext& row_context, bool compute,
                                        const pt::PackageType* ptyp) {
  const Location& loc = ptyp->ppt_loc;
  std::vector<Constraint> l = sort_constraints_no_duplicates(loc, env, ptyp->ppt_constraints);
  // Ast_helper.Mty.mk ~loc (Pmty_ident ptyp.ppt_path)
  auto* md = make<pt::Pmty_ident>(pt::Pmty_ident{{pt::ModuleTypeDesc::Kind::Pmty_ident}, ptyp->ppt_path});
  auto* smty = make<pt::ModuleType>(md, loc, pt::Attributes{});
  const tt::ModuleType* mty = ty_var_env::with_local_scope([&] { return transl_modtype(env, smty); });
  std::vector<std::pair<pt::LidLoc, const tt::CoreType*>> ptys;
  for (auto& [s, pty] : l) ptys.push_back({s, transl_type(env, policy, false, row_context, pty)});
  const ModuleType* mty2 = nullptr;
  if (!ptys.empty())
    mty2 = check_package_with_type_constraints(loc, env, mty->mty_type, compute, slice(ptys));
  else if (compute)
    mty2 = mty->mty_type;
  Path::t pack_path = transl_modtype_longident(loc, env, ptyp->ppt_path.txt);
  std::vector<PackConstraint> pack_constraints;
  for (auto& [s, cty] : ptys) {
    std::vector<std::string_view> fl = longident::flatten(s.txt);
    pack_constraints.push_back(PackConstraint{slice(fl), cty->ctyp_type});
  }
  return {make<Package>(Package{pack_path, slice(pack_constraints)}), mty2, slice(ptys)};
}

// Make the rows "fixed" in this type, to make universal check easier
static void make_fixed_univars_rec(TypeMark& mark, TypeExpr* ty) {
  if (!try_mark_node(mark, ty)) return;
  if (auto* v = as<Tvariant>(get_desc(ty))) {
    const RowDesc* row = v->row;
    RowDescRepr r = row_repr(row);
    if (is_Tunivar(r.more)) {
      std::vector<RowFieldEntry> fields;
      for ([[maybe_unused]] auto& [s, f, s_obj] : r.fields) {
        RowFieldView fv = row_field_repr(f);
        if (fv.kind == RowFieldView::Kind::Reither)
          fields.push_back({s, rf_either(f, fv.constant, slice(fv.arg_types), true)});
        else
          fields.push_back({s, f});
      }
      set_type_desc(ty, tvariant(create_row(slice(fields), r.more, r.closed,
                                            make<FixedExplanation>(FixedExplanation::Kind::Univar, r.more),
                                            r.name)));
    }
    iter_row([&](TypeExpr* t) { make_fixed_univars_rec(mark, t); }, row);
  } else {
    iter_type_expr([&](TypeExpr* t) { make_fixed_univars_rec(mark, t); }, ty);
  }
}
static void make_fixed_univars(TypeExpr* ty) {
  with_type_mark([&](TypeMark& mark) { make_fixed_univars_rec(mark, ty); });
}

static const tt::CoreType* transl_type_top(env::t env, const Policy& policy, const pt::CoreType* styp) {
  return transl_type(env, policy, false, {}, styp);
}

const tt::CoreType* transl_simple_type(env::t env, const ty_var_env::PolyUnivars* univars,
                                       bool closed, const pt::CoreType* styp) {
  ty_var_env::reset_locals(univars);
  Policy policy = closed ? ty_var_env::fixed_policy : ty_var_env::extensible_policy;
  const tt::CoreType* typ = transl_type_top(env, policy, styp);
  ty_var_env::globalize_used_variables(policy, env)();
  make_fixed_univars(typ->ctyp_type);
  return typ;
}

const tt::CoreType* transl_simple_type_univars(env::t env, const pt::CoreType* styp) {
  ty_var_env::reset_locals();
  auto [typ, univs] = ty_var_env::collect_univars([&] {
    return ctype::with_local_level_generalize([&] {
      Policy policy = ty_var_env::univars_policy;
      const tt::CoreType* t = transl_type_top(env, policy, styp);
      ty_var_env::globalize_used_variables(policy, env)();
      return t;
    });
  });
  make_fixed_univars(typ->ctyp_type);
  tt::CoreType* c = make<tt::CoreType>(*typ);
  c->ctyp_type = ctype::instance(newgenty(tpoly(typ->ctyp_type, slice(univs))));
  return c;
}

Delayed transl_simple_type_delayed(env::t env, const pt::CoreType* styp) {
  ty_var_env::reset_locals();
  auto [typ, force] = ctype::with_local_level_generalize([&] {
    Policy policy = ty_var_env::extensible_policy;
    const tt::CoreType* t = transl_type_top(env, policy, styp);
    make_fixed_univars(t->ctyp_type);
    // This brings the used variables to the global level, but doesn't link
    // them to their other occurrences just yet.  This will be done when
    // [force] is called.
    std::function<void()> f = ty_var_env::globalize_used_variables(policy, env);
    return std::make_pair(t, f);
  });
  return Delayed{typ, ctype::instance(typ->ctyp_type), force};
}

const tt::CoreType* transl_type_scheme(env::t env, const pt::CoreType* styp) {
  if (auto* p = as<pt::Ptyp_poly>(styp->ptyp_desc)) {
    std::vector<std::string_view> vars;
    for (auto& x : p->vars) vars.push_back(x.txt);
    auto [univars, typ] = ctype::with_local_level_generalize([&] {
      ty_var_env::reset();
      ty_var_env::PolyUnivars u = ty_var_env::make_poly_univars(vars);
      const tt::CoreType* t = transl_simple_type(env, &u, true, p->ty);
      return std::make_pair(u, t);
    });
    ty_var_env::instance_poly_univars(env, styp->ptyp_loc, univars);
    return make<tt::CoreType>(make<tt::Ttyp_poly>(tt::Ttyp_poly{{TK::Ttyp_poly}, slice(vars), typ}),
                              typ->ctyp_type, env, styp->ptyp_loc, styp->ptyp_attributes);
  }
  return ctype::with_local_level_generalize([&] {
    ty_var_env::reset();
    return transl_simple_type(env, nullptr, false, styp);
  });
}

}  // namespace cppcaml::typing::typetexp
