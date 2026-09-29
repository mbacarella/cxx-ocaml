// Port of typing/typemod.ml, part 3: generalization checks, recursive module
// inclusion, packages, module expressions (type_module, functor
// applications, opens) and structures (type_structure / type_str_item).
//
// Shapes: the Env port keeps no shape map, but Includemod's functor case
// creates an ident when the shape of a module is not an abstraction, so the
// shapes of modules are tracked here (a table of the module idents' shapes
// stands for Env's: idents are unique).
#include "cppcaml/typing/cmt_format.hpp"
#include <unordered_map>

#include "cppcaml/typing/includemod_errorprinter.hpp"
#include "cppcaml/typing/typedecl_unboxed.hpp"
#include "typecore_app.hpp"
#include "typemod_internal.hpp"

namespace cppcaml::typing::typemod {

using namespace types;
using pt::as;
using SK = SignatureItem::Kind;
using MK = tt::ModuleExprDesc::Kind;
using STK = tt::StructureItemDesc::Kind;
namespace tc = typecore;
namespace et = errortrace;



// ---- check that all core type schemes do not contain non-generalized type variables ------
struct NongenFound {
  std::vector<TypeExpr*> vars;
  const ValueDescription* item;
};
static std::optional<NongenFound> nongen_modtype(env::t env, const ModuleType* mty);
static std::optional<NongenFound> nongen_signature_item(env::t env, const SignatureItem* it) {
  if (it->kind == SK::Sig_value) {
    if (auto vars = ctype::nongen_vars_in_schema(env, it->value->val_type)) return NongenFound{vars->elements(), it->value};
    return std::nullopt;
  }
  if (it->kind == SK::Sig_module) return nongen_modtype(env, it->md->md_type);
  return std::nullopt;
}
static std::optional<NongenFound> nongen_modtype(env::t env, const ModuleType* mty) {
  switch (mty->kind) {
    case ModuleType::Kind::Mty_ident:
    case ModuleType::Kind::Mty_alias: return std::nullopt;
    case ModuleType::Kind::Mty_signature: {
      env::t e = env::add_signature(mty->sign, env);
      for (auto* it : mty->sign)
        if (auto r = nongen_signature_item(e, it)) return r;
      return std::nullopt;
    }
    case ModuleType::Kind::Mty_functor: {
      env::t e = env;
      if (!mty->param.is_unit && mty->param.id)
        e = env::add_module(mty->param.id, ModulePresence::Mp_present, mty->param.mty, env, true);
      return nongen_modtype(e, mty->res);
    }
  }
  return std::nullopt;
}

void check_nongen_modtype(env::t env, const Location& loc, const ModuleType* mty) {
  if (auto r = nongen_modtype(env, mty)) {
    Error e = err(loc, env, EK::Non_generalizable_module);
    e.vars = r->vars;
    e.item = r->item;
    e.mty = mty;
    raise_error(e);
  }
}

static void check_nongen_signature_item(env::t env, const SignatureItem* it) {
  if (it->kind == SK::Sig_value) {
    if (auto vars = ctype::nongen_vars_in_schema(env, it->value->val_type)) {
      Error e = err(it->value->val_loc, env, EK::Non_generalizable);
      e.vars = vars->elements();
      e.ty = it->value->val_type;
      raise_error(e);
    }
  } else if (it->kind == SK::Sig_module) {
    check_nongen_modtype(env, it->md->md_loc, it->md->md_type);
  }
}
void check_nongen_signature_(env::t env, Signature sg) {
  for (auto* it : sg) check_nongen_signature_item(env, it);
}
void check_nongen_signature(env::t env, Signature sg) { check_nongen_signature_(env, sg); }

// ---- helpers for typing recursive modules -----------------------------------------------
static Path::t anchor_submodule(const OptStr& name, Path::t anchor) {
  if (!anchor || !name.some) return nullptr;
  return Path::pdot(anchor, name.v);
}
static Path::t anchor_recmodule(Ident::t id) { return id ? Path::pident(id) : nullptr; }

env::t enrich_type_decls(Path::t anchor, const std::vector<const tt::TTypeDeclaration*>& decls, env::t oldenv,
                         env::t newenv) {
  if (!anchor) return newenv;
  env::t e = oldenv;
  for (auto* info : decls) {
    Ident::t id = info->typ_id;
    const TypeDeclaration* info2 = mtype::enrich_typedecl(oldenv, Path::pdot(anchor, ident::name(id)), id, info->typ_type);
    e = env::add_type(true, id, info2, e);
  }
  return e;
}

const ModuleType* enrich_module_type(Path::t anchor, const OptStr& name, const ModuleType* mty, env::t env) {
  if (!anchor || !name.some) return mty;
  return mtype::enrich_modtype(env, Path::pdot(anchor, name.v), mty);
}

namespace {
struct RecBinding {  // (id, name, mty_decl, modl, mty_actual, attrs, loc, shape, uid)
  Ident::t id;
  pt::OptStrLoc name;
  const tt::ModuleType* mty_decl;
  const tt::ModuleExpr* modl;
  const ModuleType* mty_actual;
  pt::Attributes attrs;
  Location loc;
  shape::t shape;
  Uid uid;
};
struct RecChecked {
  const tt::ModuleBinding* mb;
  shape::t shape;
  Uid uid;
};
}  // namespace

// PR#4450, PR#4470: the inclusion of recursive modules is checked after
// "unrolling away" the potential circularities N times (N = the number of
// mutually recursive declarations); see typemod.ml.
static std::vector<RecChecked> check_recmodule_inclusion(env::t env0, const std::vector<RecBinding>& bindings) {
  auto subst_and_strengthen = [](env::t env, long scope, subst::t s, Ident::t id, const ModuleType* mty) {
    const ModuleType* m = subst::modtype(subst::Scoping::rescope(static_cast<int>(scope)), s, mty);
    if (!id) return m;
    return mtype::strengthen(false, env, m, subst::module_path(s, Path::pident(id)));
  };
  std::function<std::vector<RecChecked>(bool, long, env::t, subst::t)> check_incl = [&](bool first_time, long n,
                                                                                       env::t env, subst::t s) {
    long scope = ctype::create_scope();
    if (n > 0) {
      // Generate fresh names Y_i for the rec. bound module idents X_i
      struct B1 {
        Ident::t id, id2;
        const ModuleType* mty_actual;
        shape::t shape;
      };
      std::vector<B1> bindings1;
      for (auto& b : bindings) {
        Ident::t id2 = b.id ? Ident::create_scoped(static_cast<int>(scope), ident::name(b.id)) : nullptr;
        bindings1.push_back({b.id, id2, b.mty_actual, b.shape});
      }
      // Enter the Y_i in the environment with their actual types substituted
      // by the input substitution s
      env::t env2 = env;
      for (auto& b : bindings1) {
        if (!b.id) continue;
        const ModuleType* mty_actual2 =
            first_time ? b.mty_actual : subst_and_strengthen(env2, scope, s, b.id, b.mty_actual);
        env2 = env::add_module(b.id2, ModulePresence::Mp_present, mty_actual2, env2, false, b.shape);
      }
      // Build the output substitution Y_i <- X_i
      subst::t s2 = subst::identity();
      for (auto& b : bindings1)
        if (b.id) s2 = subst::add_module(b.id, Path::pident(b.id2), s2);
      // Recurse with env' and s'
      return check_incl(false, n - 1, env2, s2);
    }
    // Base case: check inclusion of s(mty_actual) in s(mty_decl) and insert
    // coercion if needed
    std::vector<RecChecked> out;
    for (auto& b : bindings) {
      // (let mty_decl' = .. and mty_actual' = ..: left to right)
      const ModuleType* mty_decl2 =
          subst::modtype(subst::Scoping::rescope(static_cast<int>(scope)), s, b.mty_decl->mty_type);
      const ModuleType* mty_actual2 = subst_and_strengthen(env, scope, s, b.id, b.mty_actual);
      const tt::ModuleCoercion* coercion;
      shape::t shape = b.shape;
      try {
        auto r = includemod::modtypes_constraint(b.shape, b.modl->mod_loc, env, true, mty_actual2, mty_decl2);
        coercion = r.first;
        shape = r.second;
      } catch (const includemod::Error& ie) {
        Error e = err(b.modl->mod_loc, env, EK::Not_included);
        e.explanation = ie.expl;
        raise_error(e);  // (log_or_raise)
      }
      auto* modl2 = make<tt::ModuleExpr>(
          make<tt::Tmod_constraint>(
              tt::Tmod_constraint{{MK::Tmod_constraint}, b.modl, b.mty_decl->mty_type, b.mty_decl, coercion}),
          b.modl->mod_loc, b.mty_decl->mty_type, env, pt::Attributes{});
      auto* mb = make<tt::ModuleBinding>(b.id, b.name, b.uid, ModulePresence::Mp_present, modl2, b.attrs, b.loc);
      out.push_back({mb, shape, b.uid});
    }
    return out;
  };
  return check_incl(true, static_cast<long>(bindings.size()), env0, subst::identity());
}

// ---- helper for unpack ------------------------------------------------------------------
using Constrs = std::vector<std::pair<std::vector<std::string_view>, TypeExpr*>>;
static const ModuleType* package_constraints(env::t env, const Location& loc, const ModuleType* mty,
                                             const Constrs& constrs);
static Signature package_constraints_sig(env::t env, const Location& loc, Signature sg, const Constrs& constrs) {
  std::vector<const SignatureItem*> out;
  for (auto* it : sg) {
    if (it->kind == SK::Sig_type && it->type->type_params.empty()) {
      std::string_view name = ident::name(it->id);
      TypeExpr* ty = nullptr;
      for (auto& [l, t] : constrs)
        if (l.size() == 1 && l[0] == name) {
          ty = t;
          break;
        }
      if (ty) {
        auto* td = make<TypeDeclaration>(*it->type);
        td->type_manifest = ty;
        td->manifest_obj.reset();  // a new Some block
        td->type_immediate = typedecl_immediacy::compute_decl(env, td);
        out.push_back(sig_type(it->id, td, it->rec, it->vis));
        continue;
      }
    }
    if (it->kind == SK::Sig_module) {
      Constrs sub;
      for (auto& [l, t] : constrs)
        if (l.size() >= 2 && l[0] == ident::name(it->id)) sub.push_back({std::vector<std::string_view>(l.begin() + 1, l.end()), t});
      auto* md = make<ModuleDeclaration>(*it->md);
      md->md_type = package_constraints(env, loc, it->md->md_type, sub);
      out.push_back(sig_module(it->id, it->presence, md, it->rec, it->vis));
      continue;
    }
    out.push_back(it);
  }
  return slice(out);
}
static const ModuleType* package_constraints(env::t env, const Location& loc, const ModuleType* mty,
                                             const Constrs& constrs) {
  if (constrs.empty()) return mty;
  const ModuleType* m = mtype::scrape(env, mty);
  switch (m->kind) {
    case ModuleType::Kind::Mty_signature: return mty_signature(package_constraints_sig(env, loc, m->sign, constrs));
    case ModuleType::Kind::Mty_ident: {
      Error e = err(loc, env, EK::Cannot_scrape_package_type);
      e.path = m->path;
      raise_error(e);
    }
    default: throw std::logic_error("package_constraints");
  }
}

const ModuleType* modtype_of_package_(env::t env, const Location& loc, const Package* pack) {
  // Ctype.duplicate_type ensures that the types being added to the module
  // type are at generic_level.
  Constrs constrs;
  for (auto& c : pack->pack_constraints)
    constrs.push_back({std::vector<std::string_view>(c.path.begin(), c.path.end()), ctype::duplicate_type(c.ty)});
  const ModuleType* mty = package_constraints(env, loc, mty_ident(pack->pack_path), constrs);
  return subst::modtype(subst::Scoping::keep(), subst::identity(), mty);
}
const ModuleType* modtype_of_package(env::t env, const Location& loc, const Package* pack) {
  return modtype_of_package_(env, loc, pack);
}

static ctype::PackageSubtypeResult package_subtype(env::t env, const Package* pack1, const Package* pack2) {
  auto mkmty = [&](const Package* pack) {
    std::vector<PackConstraint> fl;
    for (auto& c : pack->pack_constraints)
      if (ctype::closed_type_expr(c.ty)) fl.push_back(c);
    auto* p = make<Package>(pack->pack_path, slice(fl));
    return modtype_of_package_(env, location::none(), p);
  };
  const ModuleType *mty1, *mty2;
  try {
    // (mkmty pack1, mkmty pack2): a `match` scrutinee tuple, evaluated left to right (Translcore binds its components in order)
    mty1 = mkmty(pack1);
    mty2 = mkmty(pack2);
  } catch (const Error& e) {
    if (e.kind != EK::Cannot_scrape_package_type) throw;
    et::FirstClassModule f{et::FirstClassModule::Kind::Package_cannot_scrape};
    f.path = e.path;
    return {false, f};
  }
  try {
    const tt::ModuleCoercion* c = includemod::modtypes(location::none(), env, true, mty1, mty2);
    if (c->kind == tt::ModuleCoercion::Kind::Tcoerce_none) return {true, {}};
    et::FirstClassModule f{et::FirstClassModule::Kind::Package_coercion};
    f.doc = includemod_errorprinter::coercion_in_package_subtype(env, mty1, c);
    return {false, f};
  } catch (const includemod::Error& e) {
    et::FirstClassModule f{et::FirstClassModule::Kind::Package_inclusion};
    f.doc = format_doc::doc_printf("%a", [&](format_doc::Formatter& ff) { includemod_errorprinter::err_msgs(ff, e.expl); });
    return {false, f};
  }
}

static const tt::ModuleExpr* wrap_constraint_package(env::t env, bool mark, const tt::ModuleExpr* arg,
                                                     const ModuleType* mty, const tt::ModuleType* explicit_) {
  const ModuleType* mty1 = subst::modtype(subst::Scoping::keep(), subst::identity(), arg->mod_type);
  const ModuleType* mty2 = subst::modtype(subst::Scoping::keep(), subst::identity(), mty);
  const tt::ModuleCoercion* coercion;
  try {
    coercion = includemod::modtypes(arg->mod_loc, env, mark, mty1, mty2);
  } catch (const includemod::Error& ie) {
    Error e = err(arg->mod_loc, env, EK::Not_included);
    e.explanation = ie.expl;
    raise_error(e);  // (log_or_raise)
  }
  return make<tt::ModuleExpr>(
      make<tt::Tmod_constraint>(tt::Tmod_constraint{{MK::Tmod_constraint}, arg, mty, explicit_, coercion}),
      arg->mod_loc, mty, env, pt::Attributes{});
}

struct Typed {  // Typedtree.module_expr * Shape.t
  const tt::ModuleExpr* me;
  shape::t shape;
};

static Typed wrap_constraint_with_shape(env::t env, bool mark, const tt::ModuleExpr* arg, const ModuleType* mty,
                                        shape::t shape, const tt::ModuleType* explicit_) {
  std::pair<const tt::ModuleCoercion*, shape::t> r;
  try {
    r = includemod::modtypes_constraint(shape, arg->mod_loc, env, mark, arg->mod_type, mty);
  } catch (const includemod::Error& ie) {
    Error e = err(arg->mod_loc, env, EK::Not_included);
    e.explanation = ie.expl;
    raise_error(e);
  }
  auto* me = make<tt::ModuleExpr>(
      make<tt::Tmod_constraint>(tt::Tmod_constraint{{MK::Tmod_constraint}, arg, mty, explicit_, r.first}),
      arg->mod_loc, mty, env, pt::Attributes{});
  return {me, r.second};
}

// ---- type a module value expression ------------------------------------------------------
namespace {
// the X in [F(X)] (which might be missing, for [F ()])
struct ArgumentSummary {
  bool is_syntactic_unit;
  const tt::ModuleExpr* arg;
  Path::t path;  // option
  shape::t shape;
};
struct ApplicationSummary {
  Location loc;
  pt::Attributes attributes;
  Location f_loc;  // loc for F
  std::optional<ArgumentSummary> arg;  // None for ()
};
std::pair<includemod::error::FunctorArgDescr, const ModuleType*> simplify_app_summary(const ApplicationSummary& app) {
  using FK = includemod::error::FunctorArgDescr::Kind;
  if (!app.arg) return {{FK::Unit}, mty_signature({})};
  const ModuleType* mty = app.arg->arg->mod_type;
  if (app.arg->is_syntactic_unit) return {{FK::Empty_struct}, mty};
  if (app.arg->path) return {{FK::Named, app.arg->path}, mty};
  return {{FK::Anonymous}, mty};
}
}  // namespace

static void check_package_closed(const Location& loc, env::t env, TypeExpr* typ,
                                 const std::vector<std::pair<std::vector<std::string_view>, TypeExpr*>>& fl) {
  for (auto& [n, t] : fl)
    if (!ctype::closed_type_expr(t)) {
      Error e = err(loc, env, EK::Incomplete_packed_module);
      e.ty = typ;
      raise_error(e);
    }
}

static Typed type_module_s(bool alias, bool strengthen, bool funct_body, Path::t anchor, env::t env,
                           const pt::ModuleExpr* smod);
static Typed type_application(const Location& loc, bool strengthen, bool funct_body, env::t env,
                              const pt::ModuleExpr* smod);
struct StructureTyped {
  const tt::Structure* str;
  Signature sg;
  SignatureNames* names;
  shape::t shape;
  env::t env;
};
static StructureTyped type_structure_s(bool toplevel, bool funct_body, Path::t anchor, env::t env, pt::Structure sstr);

static const tt::ModuleExpr* mkmod(const tt::ModuleExprDesc* d, const pt::ModuleExpr* smod, const ModuleType* mty,
                                   env::t env) {
  return make<tt::ModuleExpr>(d, smod->pmod_loc, mty, env, smod->pmod_attributes);
}

static Typed type_module_aux(bool alias, bool strengthen, bool funct_body, Path::t anchor, env::t env,
                             const pt::ModuleExpr* smod) {
  const pt::ModuleExprDesc* d = smod->pmod_desc;
  using K = pt::ModuleExprDesc::Kind;
  switch (d->kind) {
    case K::Pmod_ident: {
      const pt::LidLoc& lid = as<pt::Pmod_ident>(d)->lid;
      Path::t path = env::lookup_module_path(true, smod->pmod_loc, !alias, lid.txt, env);
      const tt::ModuleExpr* md =
          mkmod(make<tt::Tmod_ident>(tt::Tmod_ident{{MK::Tmod_ident}, path, lid}), smod, mty_alias(path), env);
      bool aliasable = env::is_aliasable(path, env);
      shape::t shape = env::shape_of_path(SigComponentKind::Module, env, path);
      if (alias && aliasable) {
        shape = shape::alias(nullptr, shape);
        env::add_required_global(path::head(path));
        return {md, shape};
      }
      const ModuleType* mty = strengthen ? env::find_strengthened_module(aliasable, path, env)
                                         : env::find_module(path, env)->md_type;
      if (mty->kind == ModuleType::Kind::Mty_alias && !alias) {
        Path::t p1 = env::normalize_module_path(&smod->pmod_loc, env, mty->path);
        const ModuleType* mty2 = includemod::expand_module_alias(strengthen, env, p1);
        auto* alias_cc = make<tt::ModuleCoercion>(tt::ModuleCoercion{tt::ModuleCoercion::Kind::Tcoerce_alias});
        alias_cc->alias_env = env;
        alias_cc->alias_path = path;
        alias_cc->alias_coercion = tt::tcoerce_none();
        auto* m2 = make<tt::ModuleExpr>(*md);
        m2->mod_desc = make<tt::Tmod_constraint>(tt::Tmod_constraint{{MK::Tmod_constraint}, md, mty2, nullptr, alias_cc});
        m2->mod_type = mty2;
        return {m2, shape};
      }
      auto* m2 = make<tt::ModuleExpr>(*md);
      m2->mod_type = mty;
      return {m2, shape};
    }
    case K::Pmod_structure: {
      StructureTyped st = type_structure_s(false, funct_body, anchor, env, as<pt::Pmod_structure>(d)->str);
      const tt::ModuleExpr* md =
          mkmod(make<tt::Tmod_structure>(tt::Tmod_structure{{MK::Tmod_structure}, st.str}), smod, mty_signature(st.sg), env);
      Signature sg2 = simplify(st.env, st.names, st.sg);
      if (sg2.size() == st.sg.size()) return {md, st.shape};
      return wrap_constraint_with_shape(env, false, md, mty_signature(sg2), st.shape, nullptr);
    }
    case K::Pmod_functor: {
      auto* f = as<pt::Pmod_functor>(d);
      tt::FunctorParameter t_arg{true};
      FunctorParameter ty_arg;
      env::t newenv = env;
      Ident::t funct_shape_param;
      bool funct_body2;
      if (f->param.is_unit) {
        funct_shape_param = shape::for_unnamed_functor_param();
        funct_body2 = false;
      } else {
        const tt::ModuleType* mty = builtin_attributes::warning_scope(f->param.mty->pmty_attributes, [&] {
          const tt::ModuleType* m = transl_modtype(env, f->param.mty);  // transl_modtype_functor_arg
          auto* m2 = make<tt::ModuleType>(*m);
          m2->mty_type = mtype::scrape_for_functor_arg(env, m->mty_type);
          return static_cast<const tt::ModuleType*>(m2);
        });
        long scope = ctype::create_scope();
        Ident::t id = nullptr;
        if (!f->param.name.txt.some) {
          funct_shape_param = shape::for_unnamed_functor_param();
        } else {
          Uid md_uid = uid::mk(env::get_current_unit());
          auto* arg_md = make<ModuleDeclaration>(mty->mty_type, Attributes{}, f->param.name.loc, md_uid);
          id = Ident::create_scoped(static_cast<int>(scope), f->param.name.txt.v);
          shape::t shape = shape::var(md_uid, id);
          newenv = env::add_module_declaration(true, id, ModulePresence::Mp_present, arg_md, env, true, shape);
          funct_shape_param = id;
        }
        t_arg = tt::FunctorParameter{false, id, f->param.name, mty};
        ty_arg.is_unit = false;
        ty_arg.id = id;
        ty_arg.some_obj = id ? fresh_identity() : nullptr;
        t_arg.some_obj = ty_arg.some_obj;  // Named (id, ...), Types.Named (id, ...): one `Some id`
        ty_arg.mty = mty->mty_type;
        funct_body2 = true;
      }
      Typed body = type_module_s(false, true, funct_body2, nullptr, newenv, f->body);
      const tt::ModuleExpr* me = mkmod(make<tt::Tmod_functor>(tt::Tmod_functor{{MK::Tmod_functor}, t_arg, body.me}),
                                       smod, mty_functor(ty_arg, body.me->mod_type), env);
      return {me, shape::abs(nullptr, funct_shape_param, body.shape)};
    }
    case K::Pmod_apply:
    case K::Pmod_apply_unit: return type_application(smod->pmod_loc, strengthen, funct_body, env, smod);
    case K::Pmod_constraint: {
      auto* c = as<pt::Pmod_constraint>(d);
      Typed arg = type_module_s(alias, true, funct_body, anchor, env, c->me);
      const tt::ModuleType* mty = transl_modtype(env, c->mty);
      Typed r = wrap_constraint_with_shape(env, true, arg.me, mty->mty_type, arg.shape, mty);
      auto* m2 = make<tt::ModuleExpr>(*r.me);
      m2->mod_loc = smod->pmod_loc;
      m2->mod_attributes = smod->pmod_attributes;
      return {m2, r.shape};
    }
    case K::Pmod_unpack: {
      const tt::Expression* exp = ctype::with_local_level_generalize_structure_if_principal(
          [&] { return tc::type_exp(env, as<pt::Pmod_unpack>(d)->exp); });
      const TypeDesc* td = get_desc(ctype::expand_head(env, exp->exp_type));
      const ModuleType* mty;
      if (auto* p = as<Tpackage>(td)) {
        std::vector<std::pair<std::vector<std::string_view>, TypeExpr*>> fl;
        for (auto& c : p->pack->pack_constraints) fl.push_back({std::vector<std::string_view>(c.path.begin(), c.path.end()), c.ty});
        check_package_closed(smod->pmod_loc, env, exp->exp_type, fl);
        // (the -principal warning's check)
        if (clflags::principal) tc::generalizable(btype::generic_level - 1, exp->exp_type);
        mty = modtype_of_package_(env, smod->pmod_loc, p->pack);
      } else if (td->kind == DescKind::Tvar) {
        throw tc::Error(smod->pmod_loc, env, tc::Error::Kind::Cannot_infer_signature);
      } else {
        Error e = err(smod->pmod_loc, env, EK::Not_a_packed_module);
        e.ty = exp->exp_type;
        raise_error(e);
      }
      if (funct_body && mtype::contains_type(env, mty))
        raise_error(err(smod->pmod_loc, env, EK::Not_allowed_in_functor_body));
      const tt::ModuleExpr* me =
          mkmod(make<tt::Tmod_unpack>(tt::Tmod_unpack{{MK::Tmod_unpack}, exp, mty}), smod, mty, env);
      return {me, shape::leaf_for_unpack()};
    }
    case K::Pmod_extension: throw ErrorForward(as<pt::Pmod_extension>(d)->ext);
    case K::Pmod_hole: raise_error(err(smod->pmod_loc, env, EK::Unexpected_hole));
  }
  throw std::logic_error("type_module_aux");
}

static Typed type_module_s(bool alias, bool strengthen, bool funct_body, Path::t anchor, env::t env,
                           const pt::ModuleExpr* smod) {
  // (typing recovery is off)
  return builtin_attributes::warning_scope(smod->pmod_attributes, [&] {
    return type_module_aux(alias, strengthen, funct_body, anchor, env, smod);
  });
}

static Typed type_one_application(const Location& apply_loc, const pt::ModuleExpr* sfunct, const tt::ModuleExpr* md_f,
                                  const std::vector<ApplicationSummary>& args, bool funct_body, env::t env,
                                  Typed acc, const ApplicationSummary& app_view) {
  const tt::ModuleExpr* funct = acc.me;
  const ModuleType* mt = env::scrape_alias(env, funct->mod_type);
  auto apply_error = [&] {
    includemod::ApplyError e;
    e.loc = apply_loc;
    e.env = env;
    if (auto* i = as<pt::Pmod_ident>(sfunct->pmod_desc))
      e.app_name = {includemod::ApplicationNameKind::Named_leftmost_functor, i->lid.txt};
    else
      e.app_name = {includemod::ApplicationNameKind::Anonymous_functor};
    e.mty_f = md_f->mod_type;
    for (auto& a : args) e.args.push_back(simplify_app_summary(a));
    return e;
  };
  if (mt->kind == ModuleType::Kind::Mty_functor && mt->param.is_unit) {
    if (app_view.arg) {
      if (app_view.arg->is_syntactic_unit) {
        // this call to warning_scope allows e.g. [F (struct end [@warning "-73"])]
        // not to warn
        builtin_attributes::warning_scope(app_view.arg->arg->mod_attributes, [&] {
          location::prerr_warning(app_view.arg->arg->mod_loc,
                                  warnings::Warning::make(warnings::Warning::K::Generative_application_expects_unit));
        });
      } else {
        raise_error(err(app_view.f_loc, env, EK::Apply_generative));
      }
    }
    if (funct_body && mtype::contains_type(env, funct->mod_type))
      raise_error(err(apply_loc, env, EK::Not_allowed_in_functor_body));
    auto* me = make<tt::ModuleExpr>(make<tt::Tmod_apply_unit>(tt::Tmod_apply_unit{{MK::Tmod_apply_unit}, funct}),
                                    funct->mod_loc, mt->res, env, app_view.attributes);
    return {me, shape::app(nullptr, acc.shape, shape::dummy_mod())};
  }
  if (mt->kind == ModuleType::Kind::Mty_functor) {
    Ident::t param = mt->param.id;
    const ModuleType* mty_param = mt->param.mty;
    const ModuleType* mty_res = mt->res;
    if (!app_view.arg) throw apply_error();
    const ArgumentSummary& a = *app_view.arg;
    const tt::ModuleCoercion* coercion;
    try {
      coercion = includemod::modtypes(a.arg->mod_loc, env, true, a.arg->mod_type, mty_param);
    } catch (const includemod::Error&) {
      throw apply_error();
    }
    const ModuleType* mty_appl;
    if (a.path) {
      long scope = ctype::create_scope();
      subst::t s = param ? subst::add_module(param, a.path, subst::identity()) : subst::identity();
      mty_appl = subst::modtype(subst::Scoping::rescope(static_cast<int>(scope)), s, mty_res);
    } else {
      env::t env2 = env;
      const ModuleType* nondep_mty = mty_res;
      if (param) {
        env2 = env::add_module(param, ModulePresence::Mp_present, a.arg->mod_type, env, true);
        check_well_formed_module(env2, app_view.loc, "the signature of this functor application", mty_res);
        try {
          nondep_mty = mtype::nondep_supertype(env2, {param}, mty_res);
        } catch (const ctype::NondepCannotErase&) {
          Error e = err(app_view.loc, env2, EK::Cannot_eliminate_dependency);
          e.mty = mt;
          raise_error(e);
        }
      }
      try {
        const tt::ModuleCoercion* c = includemod::modtypes(app_view.loc, env2, false, mty_res, nondep_mty);
        if (c->kind != tt::ModuleCoercion::Kind::Tcoerce_none)
          throw std::logic_error("unexpected coercion from original module type to nondep_supertype one");
      } catch (const includemod::Error&) {
        throw std::logic_error("nondep_supertype not included in original module type");
      }
      mty_appl = nondep_mty;
    }
    check_well_formed_module(env, apply_loc, "the signature of this functor application", mty_appl);
    auto* me = make<tt::ModuleExpr>(make<tt::Tmod_apply>(tt::Tmod_apply{{MK::Tmod_apply}, funct, a.arg, coercion}),
                                    app_view.loc, mty_appl, env, app_view.attributes);
    return {me, shape::app(nullptr, acc.shape, a.shape)};
  }
  if (mt->kind == ModuleType::Kind::Mty_alias) {
    Error e = err(app_view.f_loc, env, EK::Cannot_scrape_alias);
    e.path = mt->path;
    raise_error(e);
  }
  throw apply_error();
}

static Typed type_application(const Location& loc, bool strengthen, bool funct_body, env::t env,
                              const pt::ModuleExpr* smod) {
  // extract_application: the arguments are typed from the outermost
  std::vector<ApplicationSummary> args;
  const pt::ModuleExpr* sfunct = smod;
  for (;;) {
    const pt::ModuleExprDesc* d = sfunct->pmod_desc;
    if (auto* a = as<pt::Pmod_apply>(d)) {
      Typed arg = type_module_s(false, true, funct_body, nullptr, env, a->arg);
      bool unit = false;
      if (auto* s = as<pt::Pmod_structure>(a->arg->pmod_desc)) unit = s->str.empty();
      args.insert(args.begin(), ApplicationSummary{sfunct->pmod_loc, sfunct->pmod_attributes, a->fn->pmod_loc,
                                                   ArgumentSummary{unit, arg.me, tt::path_of_module(arg.me), arg.shape}});
      sfunct = a->fn;
      continue;
    }
    if (auto* a = as<pt::Pmod_apply_unit>(d)) {
      args.insert(args.begin(), ApplicationSummary{sfunct->pmod_loc, sfunct->pmod_attributes, a->fn->pmod_loc, std::nullopt});
      sfunct = a->fn;
      continue;
    }
    break;
  }
  bool all_paths = true;
  for (auto& a : args) all_paths = all_paths && a.arg && a.arg->path;
  Typed funct = type_module_s(false, strengthen && all_paths, funct_body, nullptr, env, sfunct);
  Typed acc = funct;
  for (auto& app : args) acc = type_one_application(loc, sfunct, funct.me, args, funct_body, env, acc, app);
  return acc;
}

TypeOpenDeclResult type_open_decl_(std::shared_ptr<bool> used_slot, bool toplevel, bool funct_body, SignatureNames* names, env::t env,
                                   const pt::OpenDeclaration* od) {
  return builtin_attributes::warning_scope(od->popen_attributes, [&]() -> TypeOpenDeclResult {
    const Location& loc = od->popen_loc;
    if (auto* i = as<pt::Pmod_ident>(od->popen_expr->pmod_desc)) {
      auto [path, newenv] = type_open_(used_slot, toplevel, od->popen_override, env, loc, i->lid);
      auto* md = make<tt::ModuleExpr>(make<tt::Tmod_ident>(tt::Tmod_ident{{MK::Tmod_ident}, path, i->lid}),
                                      od->popen_expr->pmod_loc, mty_alias(path), env, od->popen_expr->pmod_attributes);
      auto* descr =
          make<tt::OpenDeclaration>(md, Signature{}, od->popen_override, newenv, loc, od->popen_attributes);
      return {descr, {}, newenv};
    }
    Typed md = type_module_s(false, true, funct_body, nullptr, env, od->popen_expr);
    long scope = ctype::create_scope();
    auto [sg0, newenv] = env::enter_signature(static_cast<int>(scope),
                                              extract_sig_open(env, md.me->mod_loc, md.me->mod_type), env, md.shape);
    std::optional<NameInfo> info;
    Visibility visibility = Visibility::Exported;
    if (!toplevel) {
      info = NameInfo{NameInfo::Kind::From_open};
      visibility = Visibility::Hidden;
    }
    signature_group::iter([&](const signature_group::RecGroup& g) { check_sig_item(names, loc, g, info); }, sg0);
    std::vector<const SignatureItem*> sg;
    for (auto* it : sg0) {
      auto* it2 = make<SignatureItem>(*it);
      it2->vis = visibility;
      sg.push_back(it2);
    }
    auto* descr = make<tt::OpenDeclaration>(md.me, slice(sg), od->popen_override, newenv, loc, od->popen_attributes);
    return {descr, slice(sg), newenv};
  });
}

// ---- structures ------------------------------------------------------------------------------
namespace {
struct ItemTyped {
  const tt::StructureItem* item;
  std::vector<const SignatureItem*> sg;
  shape::ItemMap shape_map;
  env::t env;
};
template <class D>
const D* mkd(D d) {
  return make<D>(std::move(d));
}
}  // namespace

static ItemTyped type_str_item(SignatureNames* names, bool toplevel, bool funct_body, Path::t anchor, env::t env,
                               shape::ItemMap shape_map, const pt::StructureItem* sitem) {
  const Location& loc = sitem->pstr_loc;
  const pt::StructureItemDesc* d = sitem->pstr_desc;
  using K = pt::StructureItemDesc::Kind;
  auto mk = [&](const tt::StructureItemDesc* desc, std::vector<const SignatureItem*> sg, shape::ItemMap sm,
                env::t newenv) { return ItemTyped{make<tt::StructureItem>(desc, loc, env), std::move(sg), sm, newenv}; };
  switch (d->kind) {
    case K::Pstr_eval: {
      auto* e = as<pt::Pstr_eval>(d);
      const tt::Expression* expr = builtin_attributes::warning_scope(e->attrs, [&] { return tc::type_expression(env, e->exp); });
      return mk(mkd(tt::Tstr_eval{{STK::Tstr_eval}, expr, e->attrs}), {}, shape_map, env);
    }
    case K::Pstr_value: {
      auto* v = as<pt::Pstr_value>(d);
      tc::TypeBindingResult r = tc::type_binding(env, v->rec, v->vbs);
      Slice<const tt::ValueBinding*> defs = r.vbs;
      if (v->rec == RecFlag::Recursive) defs = tc::annotate_recursive_bindings(env, defs);
      std::vector<const SignatureItem*> items;
      for (auto& b : tt::let_bound_idents_full(defs)) {
        check_value(names, b.name.loc, b.id);
        const ValueDescription* vd = env::find_value(Path::pident(b.id), r.env);
        items.push_back(sig_value(b.id, vd, Visibility::Exported));
        shape_map = shape::map::add_value(shape_map, b.id, vd->val_uid);
      }
      return mk(mkd(tt::Tstr_value{{STK::Tstr_value}, v->rec, defs}), items, shape_map, r.env);
    }
    case K::Pstr_val: raise_error(err(as<pt::Pstr_val>(d)->vd->pval_loc, env, EK::Val_in_structure));
    case K::Pstr_primitive: {
      auto [desc, newenv] = typedecl::transl_prim_desc(env, loc, as<pt::Pstr_primitive>(d)->pd);
      check_value(names, desc->prim_loc, desc->prim_id);
      shape_map = shape::map::add_value(shape_map, desc->prim_id, desc->prim_val->val_uid);
      return mk(mkd(tt::Tstr_primitive{{STK::Tstr_primitive}, desc}),
                {sig_value(desc->prim_id, desc->prim_val, Visibility::Exported)}, shape_map, newenv);
    }
    case K::Pstr_type: {
      auto* t = as<pt::Pstr_type>(d);
      typedecl::TranslTypeDeclResult r = typedecl::transl_type_decl(env, t->rec, t->decls);
      for (auto* td : r.decls) check_type(names, td->typ_loc, td->typ_id);
      std::vector<const SignatureItem*> items = map_rec_type_with_row_types(t->rec, r.decls);
      for (std::size_t k = 0; k < r.decls.size(); ++k)
        shape_map = shape::map::add_type(shape_map, r.decls[k]->typ_id, r.shapes[k]);
      return mk(mkd(tt::Tstr_type{{STK::Tstr_type}, t->rec, slice(r.decls)}), items, shape_map,
                enrich_type_decls(anchor, r.decls, env, r.env));
    }
    case K::Pstr_typext: {
      auto [tyext, newenv, shapes] = typedecl::transl_type_extension(true, env, loc, as<pt::Pstr_typext>(d)->ext);
      std::vector<const SignatureItem*> sg;
      for (std::size_t k = 0; k < tyext->tyext_constructors.size(); ++k) {
        auto* ext = tyext->tyext_constructors[k];
        check_typext(names, ext->ext_loc, ext->ext_id);
        shape_map = shape::map::add_extcons(shape_map, ext->ext_id, shapes[k]);
      }
      for (std::size_t k = 0; k < tyext->tyext_constructors.size(); ++k) {
        auto* ext = tyext->tyext_constructors[k];
        sg.push_back(sig_typext(ext->ext_id, ext->ext_type, k == 0 ? ExtStatus::Text_first : ExtStatus::Text_next,
                                Visibility::Exported));
      }
      return mk(mkd(tt::Tstr_typext{{STK::Tstr_typext}, tyext}), sg, shape_map, newenv);
    }
    case K::Pstr_exception: {
      auto [ext, newenv, shape] = typedecl::transl_type_exception(env, as<pt::Pstr_exception>(d)->exn);
      const tt::TExtensionConstructor* c = ext->tyexn_constructor;
      check_typext(names, c->ext_loc, c->ext_id);
      shape_map = shape::map::add_extcons(shape_map, c->ext_id, shape);
      return mk(mkd(tt::Tstr_exception{{STK::Tstr_exception}, ext}),
                {sig_typext(c->ext_id, c->ext_type, ExtStatus::Text_exception, Visibility::Exported)}, shape_map,
                newenv);
    }
    case K::Pstr_module: {
      const pt::ModuleBinding* mb = as<pt::Pstr_module>(d)->mb;
      long outer_scope = ctype::get_current_level();
      long scope = ctype::create_scope();
      Typed modl = builtin_attributes::warning_scope(mb->pmb_attributes, [&] {
        return type_module_s(true, true, funct_body, anchor_submodule(mb->pmb_name.txt, anchor), env, mb->pmb_expr);
      });
      ModulePresence pres = modl.me->mod_type->kind == ModuleType::Kind::Mty_alias ? ModulePresence::Mp_absent
                                                                                   : ModulePresence::Mp_present;
      Uid md_uid = uid::mk(env::get_current_unit());
      auto* md = make<ModuleDeclaration>(enrich_module_type(anchor, mb->pmb_name.txt, modl.me->mod_type, env),
                                         parsetree::types_attributes(mb->pmb_attributes), mb->pmb_loc, md_uid);
      shape::t md_shape = shape::set_uid_if_none(modl.shape, md_uid);
      mtype::lower_nongen(outer_scope, md->md_type);
      Ident::t id = nullptr;
      env::t newenv = env;
      std::vector<const SignatureItem*> sg;
      if (mb->pmb_name.txt.some) {
        auto [i, e] =
            env::enter_module_declaration(static_cast<int>(scope), mb->pmb_name.txt.v, pres, md, env, false, md_shape);
        check_module(names, mb->pmb_loc, i);
        id = i;
        newenv = e;
        auto* md2 = make<ModuleDeclaration>(modl.me->mod_type, parsetree::types_attributes(mb->pmb_attributes),
                                            mb->pmb_loc, md_uid);
        sg.push_back(sig_module(i, pres, md2, RecStatus::Trec_not, Visibility::Exported));
        shape_map = shape::map::add_module(shape_map, i, md_shape);
      }
      auto* tmb = make<tt::ModuleBinding>(id, mb->pmb_name, md->md_uid, pres, modl.me, mb->pmb_attributes, mb->pmb_loc);
      return mk(mkd(tt::Tstr_module{{STK::Tstr_module}, tmb}), sg, shape_map, newenv);
    }
    case K::Pstr_recmodule: {
      struct SB {
        pt::OptStrLoc name;
        const pt::ModuleType* smty;
        const pt::ModuleExpr* smodl;
        pt::Attributes attrs;
        Location loc;
      };
      std::vector<SB> sbind;
      for (auto* mb : as<pt::Pstr_recmodule>(d)->mbs) {
        auto* c = as<pt::Pmod_constraint>(mb->pmb_expr->pmod_desc);
        if (!c) raise_error(err(mb->pmb_expr->pmod_loc, env, EK::Recursive_module_require_explicit_type));
        sbind.push_back({mb->pmb_name, c->mty, c->me, mb->pmb_attributes, mb->pmb_loc});
      }
      std::vector<const pt::ModuleDeclaration*> pmds;
      for (auto& b : sbind) pmds.push_back(make<pt::ModuleDeclaration>(b.name, b.smty, b.attrs, b.loc));
      TranslRecmodule tr = transl_recmodule_modtypes(env, slice(pmds));
      for (auto* md : tr.decls)
        if (md->md_id) check_module(names, md->md_loc, md->md_id);
      std::vector<RecBinding> bindings1;
      for (std::size_t k = 0; k < tr.decls.size(); ++k) {
        const tt::TModuleDeclaration* md = tr.decls[k];
        Typed modl = builtin_attributes::warning_scope(sbind[k].attrs, [&] {
          return type_module_s(false, true, funct_body, anchor_recmodule(md->md_id), tr.env, sbind[k].smodl);
        });
        const ModuleType* mty2 = enrich_module_type(anchor, sbind[k].name.txt, modl.me->mod_type, tr.env);
        includemod::modtypes_consistency(modl.me->mod_loc, tr.env, mty2, md->md_type->mty_type);
        bindings1.push_back({md->md_id, sbind[k].name, md->md_type, modl.me, mty2, sbind[k].attrs, sbind[k].loc,
                             modl.shape, md->md_uid});
      }
      // allow aliasing recursive modules from outside
      env::t newenv = env;
      for (auto& b : bindings1) {
        if (!b.id) continue;
        auto* mdecl = make<ModuleDeclaration>(b.mty_decl->mty_type, parsetree::types_attributes(b.attrs), b.loc, b.uid);
        newenv = env::add_module_declaration(true, b.id, ModulePresence::Mp_present, mdecl, newenv, false, b.shape);
      }
      std::vector<RecChecked> bindings2 = check_recmodule_inclusion(newenv, bindings1);
      std::vector<const tt::ModuleBinding*> mbs_all;
      std::vector<const SignatureItem*> sg;
      std::vector<RecChecked> mbs;
      for (auto& b : bindings2) {
        mbs_all.push_back(b.mb);
        if (b.mb->mb_id) mbs.push_back(b);
      }
      for (auto& b : mbs) shape_map = shape::map::add_module(shape_map, b.mb->mb_id, b.shape);
      for (std::size_t k = 0; k < mbs.size(); ++k) {
        auto* mb = mbs[k].mb;
        auto* dd = make<ModuleDeclaration>(mb->mb_expr->mod_type, parsetree::types_attributes(mb->mb_attributes),
                                           mb->mb_loc, mbs[k].uid);
        sg.push_back(sig_module(mb->mb_id, ModulePresence::Mp_present, dd,
                                k == 0 ? RecStatus::Trec_first : RecStatus::Trec_next, Visibility::Exported));
      }
      return mk(mkd(tt::Tstr_recmodule{{STK::Tstr_recmodule}, slice(mbs_all)}), sg, shape_map, newenv);
    }
    case K::Pstr_modtype: {
      const pt::ModuleTypeDeclaration* pmtd = as<pt::Pstr_modtype>(d)->mtd;
      auto [mtd, decl, newenv] = transl_modtype_decl(env, pmtd);
      check_modtype(names, pmtd->pmtd_loc, mtd->mtd_id);
      shape_map = shape::map::add_module_type(shape_map, mtd->mtd_id, decl->mtd_uid);
      return mk(mkd(tt::Tstr_modtype{{STK::Tstr_modtype}, mtd}),
                {sig_modtype(mtd->mtd_id, decl, Visibility::Exported)}, shape_map, newenv);
    }
    case K::Pstr_open: {
      TypeOpenDeclResult r = type_open_decl_(nullptr, toplevel, funct_body, names, env, as<pt::Pstr_open>(d)->od);
      return mk(mkd(tt::Tstr_open{{STK::Tstr_open}, r.od}), std::vector<const SignatureItem*>(r.sg.begin(), r.sg.end()),
                shape_map, r.env);
    }
    case K::Pstr_class: {
      auto [classes, new_env] = typeclass::class_declarations(env, as<pt::Pstr_class>(d)->decls);
      for (auto& cls : classes) {
        const Location& l = cls.cls_id_loc.loc;
        check_class(names, l, cls.cls_id);
        check_class_type(names, l, cls.cls_ty_id);
        check_type(names, l, cls.cls_obj_id);
        Uid uid = cls.cls_decl->cty_uid;
        shape_map = shape::map::add_class(shape_map, cls.cls_id, uid);
        shape_map = shape::map::add_class_type(shape_map, cls.cls_ty_id, uid);
        shape_map = shape::map::add_type(shape_map, cls.cls_obj_id, shape::leaf(uid));
      }
      std::vector<tt::ClassDeclarationItem> items;
      std::vector<const SignatureItem*> sg;
      for (std::size_t k = 0; k < classes.size(); ++k) {
        auto& cls = classes[k];
        items.push_back({cls.cls_info, slice(cls.cls_pub_methods)});
        RecStatus rs = k == 0 ? RecStatus::Trec_first : RecStatus::Trec_next;
        sg.push_back(sig_class(cls.cls_id, cls.cls_decl, rs, Visibility::Exported));
        sg.push_back(sig_class_type(cls.cls_ty_id, cls.cls_ty_decl, rs, Visibility::Exported));
        sg.push_back(sig_type(cls.cls_obj_id, cls.cls_obj_abbr, rs, Visibility::Exported));
      }
      return mk(mkd(tt::Tstr_class{{STK::Tstr_class}, slice(items)}), sg, shape_map, new_env);
    }
    case K::Pstr_class_type: {
      auto [classes, new_env] = typeclass::class_type_declarations(env, as<pt::Pstr_class_type>(d)->decls);
      for (auto& decl : classes) {
        const Location& l = decl.clsty_id_loc.loc;
        check_class_type(names, l, decl.clsty_ty_id);
        check_type(names, l, decl.clsty_obj_id);
        Uid uid = decl.clsty_ty_decl->clty_uid;
        shape_map = shape::map::add_class_type(shape_map, decl.clsty_ty_id, uid);
        shape_map = shape::map::add_type(shape_map, decl.clsty_obj_id, shape::leaf(uid));
      }
      std::vector<tt::ClassTypeDeclarationItem> items;
      std::vector<const SignatureItem*> sg;
      for (std::size_t k = 0; k < classes.size(); ++k) {
        auto& decl = classes[k];
        items.push_back({decl.clsty_ty_id, decl.clsty_id_loc, decl.clsty_info});
        RecStatus rs = k == 0 ? RecStatus::Trec_first : RecStatus::Trec_next;
        sg.push_back(sig_class_type(decl.clsty_ty_id, decl.clsty_ty_decl, rs, Visibility::Exported));
        sg.push_back(sig_type(decl.clsty_obj_id, decl.clsty_obj_abbr, rs, Visibility::Exported));
      }
      return mk(mkd(tt::Tstr_class_type{{STK::Tstr_class_type}, slice(items)}), sg, shape_map, new_env);
    }
    case K::Pstr_include: {
      const pt::IncludeDeclaration* sincl = as<pt::Pstr_include>(d)->incl;
      const pt::ModuleExpr* smodl = sincl->pincl_mod;
      Typed modl = builtin_attributes::warning_scope(sincl->pincl_attributes, [&] {
        return type_module_s(false, true, funct_body, nullptr, env, smodl);
      });
      long scope = ctype::create_scope();
      // Rename all identifiers bound by this signature to avoid clashes
      // (Env.enter_signature_and_shape)
      auto [sg, shape_map2, new_env] = env::enter_signature_and_shape(
          static_cast<int>(scope), shape_map, modl.shape, extract_sig_open(env, smodl->pmod_loc, modl.me->mod_type), env);
      shape_map = shape_map2;

      signature_group::iter([&](const signature_group::RecGroup& g) { check_sig_item(names, loc, g); }, sg);
      auto* incl = make<tt::IncludeDeclaration>(modl.me, sg, sincl->pincl_loc, sincl->pincl_attributes);
      return mk(mkd(tt::Tstr_include{{STK::Tstr_include}, incl}), std::vector<const SignatureItem*>(sg.begin(), sg.end()),
                shape_map, new_env);
    }
    case K::Pstr_extension: throw ErrorForward(as<pt::Pstr_extension>(d)->ext);
    case K::Pstr_attribute:
      builtin_attributes::warning_attribute(as<pt::Pstr_attribute>(d)->attr);
      return mk(mkd(tt::Tstr_attribute{{STK::Tstr_attribute}, as<pt::Pstr_attribute>(d)->attr}), {}, shape_map, env);
  }
  throw std::logic_error("type_str_item");
}

static StructureTyped type_structure_s(bool toplevel, bool funct_body, Path::t anchor, env::t env0,
                                       pt::Structure sstr) {
  SignatureNames* names = create_signature_names();
  env::t env = env0;
  shape::ItemMap shape_map = shape::map::empty();
  std::vector<const tt::StructureItem*> items;
  std::vector<const SignatureItem*> sg;
  // (typing recovery is not ported)
  using cmt_format::BinaryPart;
  cmt_format::saved_types_t saved = cmt_format::get_saved_types();  // with_saved_types ~save_part
  cmt_format::set_saved_types(nullptr);
  auto delayed = [&] {
    for (auto* item : sstr) {
      cmt_format::saved_types_t previous_saved_types = cmt_format::get_saved_types();
      ItemTyped r = type_str_item(names, toplevel, funct_body, anchor, env, shape_map, item);
      cmt_format::set_saved_types(cmt_format::cons_saved_type(
          BinaryPart{BinaryPart::Kind::Partial_structure_item, false, r.item}, previous_saved_types));
      items.push_back(r.item);
      sg.insert(sg.end(), r.sg.begin(), r.sg.end());
      shape_map = r.shape_map;
      env = r.env;
    }
    return 0;
  };
  if (toplevel) delayed();
  else builtin_attributes::warning_scope(pt::Attributes{}, delayed);
  Signature sgs = slice(sg);  // one list: str_type and the module type's
  auto* str = make<tt::Structure>(slice(items), sgs, env);
  cmt_format::set_saved_types(
      cmt_format::cons_saved_type(BinaryPart{BinaryPart::Kind::Partial_structure, false, str}, saved));
  return {str, sgs, names, shape::str(nullptr, shape_map), env};
}

// ---- entry points ---------------------------------------------------------------------------
const tt::ModuleExpr* type_module_(bool alias, bool strengthen, bool funct_body, Path::t anchor, env::t env,
                                   const pt::ModuleExpr* smod) {
  return type_module_s(alias, strengthen, funct_body, anchor, env, smod).me;
}
const tt::ModuleExpr* type_module(env::t env, const pt::ModuleExpr* smod) {
  return type_module_s(false, true, false, nullptr, env, smod).me;
}
TypeStructureResult type_structure_(bool toplevel, bool funct_body, Path::t anchor, env::t env, pt::Structure sstr) {
  StructureTyped st = type_structure_s(toplevel, funct_body, anchor, env, sstr);
  return {st.str, st.sg, st.names, st.env, st.shape};
}
TypeStructureResult type_structure(env::t env, pt::Structure sstr) {
  return type_structure_(false, false, nullptr, env, sstr);
}
TypeStructureResult type_toplevel_phrase(env::t env, pt::Structure sstr) {
  env::reset_required_globals();
  return type_structure_(true, false, nullptr, env, sstr);
}
std::pair<const tt::ModuleExpr*, shape::t> type_module_alias_with_shape(env::t env, const pt::ModuleExpr* smod) {
  Typed t = type_module_s(true, true, false, nullptr, env, smod);
  return {t.me, t.shape};
}
std::pair<const tt::StructureItem*, env::t> type_str_item_fwd(env::t env, const pt::StructureItem* item) {
  ItemTyped r = type_str_item(create_signature_names(), false, false, nullptr, env, shape::map::empty(), item);
  return {r.item, r.env};
}
const tt::ModuleExpr* wrap_constraint_package_(env::t env, bool mark, const tt::ModuleExpr* arg, const ModuleType* mty,
                                               const tt::ModuleType* explicit_) {
  return wrap_constraint_package(env, mark, arg, mty, explicit_);
}
void check_package_closed_(const Location& loc, env::t env, TypeExpr* typ,
                           const std::vector<std::pair<std::vector<std::string_view>, TypeExpr*>>& fl) {
  check_package_closed(loc, env, typ, fl);
}
ctype::PackageSubtypeResult package_subtype_(env::t env, const Package* p1, const Package* p2) {
  return package_subtype(env, p1, p2);
}

}  // namespace cppcaml::typing::typemod
