// Port of typing/typeclass.ml: typing of class types, class expressions
// (class structures and their fields), class declarations / descriptions /
// class type declarations, immediate objects, and the recursive-module
// checks on class types.  Warnings are not emitted; the warning-only
// computations are left out.  Cmt_format.add_saved_type (`rc`) is not ported.
#include "cppcaml/typing/cmt_format.hpp"
#include <set>

#include "ast_helper.hpp"
#include "cppcaml/typing/includeclass.hpp"
#include "cppcaml/typing/subst.hpp"
#include "cppcaml/typing/typeclass.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/printtyp.hpp"
#include "cppcaml/typing/typedecl.hpp"
#include "cppcaml/typing/typedecl_variance.hpp"
#include "typecore_class.hpp"
#include "typecore_exp.hpp"

namespace cppcaml::typing::typeclass {

using namespace types;
using namespace btype;
using pt::as;
namespace tc = typecore;
namespace ah = ast_helper;
using TK = tt::ClassExprDesc::Kind;
using EK = Error::K;
using MethSet = std::set<std::string_view>;

std::function<std::pair<const tt::OpenDescription*, env::t>(std::shared_ptr<bool>, env::t, const pt::OpenDescription*)>
    type_open_descr;

namespace {

// 'a full_class
template <class B>
struct FullClass {
  Ident::t id;
  pt::StrLoc id_loc;
  const ClassDeclaration* clty;
  Ident::t ty_id;
  const ClassTypeDeclaration* cltydef;
  Ident::t obj_id;
  const TypeDeclaration* obj_abbr;
  long arity;
  std::vector<std::string_view> pub_meths;
  std::vector<Location> coe;
  const tt::ClassInfos<B>* req;
};

enum class Final { Final, Not_final };

Kind kind_of_final(Final f) { return f == Final::Final ? Kind::Object : Kind::Class; }

[[noreturn]] void raise_error(const Error& e) { typing_recovery::log_and_raise(e); }
Error err(const Location& loc, env::t env, EK k) { return Error(loc, env, k); }

template <class D>
const D* mkd(D d) {
  return make<D>(std::move(d));
}

tt::CoreType* ctyp(const tt::CoreTypeDesc* desc, TypeExpr* typ, env::t env, const Location& loc) {
  return make<tt::CoreType>(desc, typ, env, loc, tt::Attributes{});
}

// Path associated to the temporary class type of a class being typed (its
// constructor is not available); a module-initialization value
Path::t unbound_class() {
  static Path::t p = [] {
    ZoneScope perm(permanent_zone());
    return Path::pident(Ident::create_local(OCAML_LIT("*undef*")));
  }();
  return p;
}

const ClassType* cty_signature(ClassSignature* sign) {
  ClassType* c = make<ClassType>();
  c->kind = ClassType::Kind::Cty_signature;
  c->sign = sign;
  return c;
}
const ClassType* cty_arrow(const ArgLabel& l, TypeExpr* ty, const ClassType* cty) {
  ClassType* c = make<ClassType>();
  c->kind = ClassType::Kind::Cty_arrow;
  c->label = l;
  c->arg = ty;
  c->cty = cty;
  return c;
}
const ClassType* cty_constr(Path::t p, Slice<TypeExpr*> args, const ClassType* cty) {
  ClassType* c = make<ClassType>();
  c->kind = ClassType::Kind::Cty_constr;
  c->path = p;
  c->args = args;
  c->cty = cty;
  return c;
}

// ---- some operations on class types ----------------------------------------------------
struct Constraints {
  Slice<std::string_view> vals, meths, concrs;
};
Constraints extract_constraints(const ClassType* cty) {
  ClassSignature* sign = signature_of_class_type(cty);
  std::vector<std::string_view> v = instance_vars(sign);
  std::vector<std::string_view> m = methods(sign);
  MethSet c = concrete_methods(sign);
  return {slice(v), slice(m), slice(std::vector<std::string_view>(c.begin(), c.end()))};
}

void update_class_signature(const Location& loc, env::t env, VirtualFlag virt, Kind kind, ClassSignature* sign,
                            bool warn_implicit_public = false) {
  auto [implicit_public, implicit_declared] = ctype::update_class_signature(env, sign);
  if (!implicit_declared.empty() && virt == VirtualFlag::Concrete) {
    Error e = err(loc, env, EK::Undeclared_methods);
    e.class_kind = kind;
    e.names = implicit_declared;
    raise_error(e);
  }
  if (warn_implicit_public && !implicit_public.empty()) {
    std::vector<std::string> l(implicit_public.begin(), implicit_public.end());
    location::prerr_warning(loc, warnings::Warning::with_l(warnings::Warning::K::Implicit_public_methods, l));
  }
}

void complete_class_signature(const Location& loc, env::t env, VirtualFlag virt, Kind kind, ClassSignature* sign) {
  update_class_signature(loc, env, virt, kind, sign, false);
  ctype::hide_private_methods(env, sign);
}

void complete_class_type(const Location& loc, env::t env, VirtualFlag virt, Kind kind, const ClassType* typ) {
  ClassSignature* sign = signature_of_class_type(typ);
  complete_class_signature(loc, env, virt, kind, sign);
}

void check_virtual(const Location& loc, env::t env, VirtualFlag virt, Kind kind, const ClassSignature* sign) {
  if (virt == VirtualFlag::Virtual) return;
  // (Btype.virtual_methods sign, Btype.virtual_instance_vars sign): a `match` scrutinee tuple, evaluated left to right (Translcore binds its components in order)
  std::vector<std::string_view> meths = virtual_methods(sign);
  std::vector<std::string_view> vars = virtual_instance_vars(sign);
  if (meths.empty() && vars.empty()) return;
  Error e = err(loc, env, EK::Virtual_class);
  e.class_kind = kind;
  e.names = meths;
  e.names2 = vars;
  raise_error(e);
}

void check_virtual_clty(const Location& loc, env::t env, VirtualFlag virt, Kind kind, const ClassType* clty) {
  for (;;) {
    if (clty->kind == ClassType::Kind::Cty_signature) {
      check_virtual(loc, env, virt, kind, clty->sign);
      return;
    }
    clty = clty->cty;
  }
}

// Return the constructor type associated to a class type
TypeExpr* constructor_type(TypeExpr* constr, const ClassType* cty) {
  switch (cty->kind) {
    case ClassType::Kind::Cty_constr: return constructor_type(constr, cty->cty);
    case ClassType::Kind::Cty_signature: return constr;
    case ClassType::Kind::Cty_arrow: {
      TypeExpr* ty = ctype::newmono(cty->arg);
      TypeExpr* r = constructor_type(constr, cty->cty);
      return ctype::newty(tarrow(cty->label, ty, r, commu_ok()));
    }
  }
  throw std::logic_error("constructor_type");
}

// ---- primitives for typing classes ------------------------------------------------------
[[noreturn]] void raise_add_method_failure(const Location& loc, env::t env, std::string_view label,
                                           const ClassSignature* sign, const ctype::AddMethodFailed& f) {
  if (f.unexpected_method) {
    Error e = err(loc, env, EK::Unexpected_field);
    e.ty = sign->csig_self;
    e.name = std::string(label);
    raise_error(e);
  }
  Error e = err(loc, env, EK::Field_type_mismatch);
  e.name = "method";
  e.name2 = std::string(label);
  e.trace = f.err;
  raise_error(e);
}
[[noreturn]] void raise_add_instance_variable_failure(const Location& loc, env::t env, std::string_view label,
                                                      const ctype::AddInstanceVariableFailed& f) {
  if (f.mutability_mismatch) {
    Error e = err(loc, env, EK::Mutability_mismatch);
    e.name = std::string(label);
    e.mut = f.mut;
    raise_error(e);
  }
  Error e = err(loc, env, EK::Field_type_mismatch);
  e.name = "instance variable";
  e.name2 = std::string(label);
  e.trace = f.err;
  raise_error(e);
}

void add_method(const Location& loc, env::t env, std::string_view label, PrivateFlag priv, VirtualFlag virt,
                TypeExpr* ty, ClassSignature* sign) {
  try {
    ctype::add_method(env, label, priv, virt, ty, sign);
  } catch (const ctype::AddMethodFailed& f) {
    raise_add_method_failure(loc, env, label, sign, f);
  }
}
void add_instance_variable(bool strict, const Location& loc, env::t env, std::string_view label, MutableFlag mut,
                           VirtualFlag virt, TypeExpr* ty, ClassSignature* sign) {
  try {
    ctype::add_instance_variable(strict, env, label, mut, virt, ty, sign);
  } catch (const ctype::AddInstanceVariableFailed& f) {
    raise_add_instance_variable_failure(loc, env, label, f);
  }
}
void inherit_class_signature(bool strict, const Location& loc, env::t env, ClassSignature* sign1,
                             const ClassSignature* sign2) {
  try {
    ctype::inherit_class_signature(strict, env, sign1, sign2);
  } catch (const ctype::InheritClassSignatureFailed& f) {
    using IK = ctype::InheritClassSignatureFailed::Kind;
    switch (f.kind) {
      case IK::Self_type_mismatch: {
        Error e = err(loc, env, EK::Self_clash);
        e.trace = f.err;
        raise_error(e);
      }
      case IK::Method: raise_add_method_failure(loc, env, f.label, sign1, *f.method);
      case IK::Instance_variable: raise_add_instance_variable_failure(loc, env, f.label, *f.ivar);
    }
    throw;
  }
}
void inherit_class_type(bool strict, const Location& loc, env::t env, ClassSignature* sign1, const ClassType* cty2) {
  const ClassType* s = scrape_class_type(cty2);
  if (s->kind != ClassType::Kind::Cty_signature) {
    Error e = err(loc, env, EK::Structure_expected);
    e.cty = cty2;
    raise_error(e);
  }
  inherit_class_signature(strict, loc, env, sign1, s->sign);
}

void unify_delayed_method_type(const Location& loc, env::t env, std::string_view label, TypeExpr* ty,
                               TypeExpr* expected_ty) {
  try {
    ctype::unify(env, ty, expected_ty);
  } catch (const ctype::Unify& u) {
    Error e = err(loc, env, EK::Field_type_mismatch);
    e.name = "method";
    e.name2 = std::string(label);
    e.trace = u.err;
    raise_error(e);
  }
}

std::pair<const tt::CoreType*, const tt::CoreType*> type_constraint(env::t val_env, const pt::CoreType* sty,
                                                                    const pt::CoreType* sty2, const Location& loc) {
  const tt::CoreType* cty = typetexp::transl_simple_type(val_env, nullptr, false, sty);
  TypeExpr* ty = cty->ctyp_type;
  const tt::CoreType* cty2 = typetexp::transl_simple_type(val_env, nullptr, false, sty2);
  TypeExpr* ty2 = cty2->ctyp_type;
  try {
    ctype::unify(val_env, ty, ty2);
  } catch (const ctype::Unify& u) {
    Error e = err(loc, val_env, EK::Unconsistent_constraint);
    e.trace = u.err;
    raise_error(e);
  }
  return {cty, cty2};
}

// fun (self-* as self-<cl_num>) -> expr
const pt::Expression* make_method(const Location& loc, std::string_view cl_num, const pt::Expression* expr) {
  const pt::Pattern* var = ah::pat_mk(
      mkd(pt::Ppat_var{{pt::PatternDesc::Kind::Ppat_var}, pt::StrLoc{OCAML_LIT("self-*"), loc}}), loc);
  const pt::Pattern* pat = ah::pat_mk(
      mkd(pt::Ppat_alias{{pt::PatternDesc::Kind::Ppat_alias}, var,
                         pt::StrLoc{zborrow(std::string("self-") + std::string(cl_num)), loc}}),
      loc);
  pt::FunctionParamDesc pd{pt::FunctionParamDesc::Kind::Pparam_val};
  pd.label = ArgLabel::nolabel();
  pd.pat = pat;
  std::vector<const pt::FunctionParam*> params{make<pt::FunctionParam>(pat->ppat_loc, pd)};
  auto* body = make<pt::FunctionBody>();
  body->kind = pt::FunctionBody::Kind::Pfunction_body;
  body->body = expr;
  return ah::exp_mk(mkd(pt::Pexp_function{{pt::ExpressionDesc::Kind::Pexp_function}, slice(params), nullptr, body}),
                    expr->pexp_loc);
}

// Ast_helper.Typ.force_poly
const pt::CoreType* force_poly(const pt::CoreType* t) {
  if (t->ptyp_desc->kind == pt::CoreTypeDesc::Kind::Ptyp_poly) return t;
  return ah::typ_poly(t->ptyp_loc, {}, t);
}

// ---- class types --------------------------------------------------------------------------
std::vector<std::function<void()>> delayed_meth_specs;  // in order of creation

const tt::ClassType* class_type_rec(env::t env, VirtualFlag virt, long self_scope, const pt::ClassType* scty);

const tt::ClassTypeField* class_type_field(env::t env, ClassSignature* sign, long self_scope,
                                           const pt::ClassTypeField* ctf) {
  const Location& loc = ctf->pctf_loc;
  auto mkctf = [&](const tt::ClassTypeFieldDesc* desc) {
    return static_cast<const tt::ClassTypeField*>(make<tt::ClassTypeField>(desc, loc, ctf->pctf_attributes));
  };
  using FK = tt::ClassTypeFieldDesc::Kind;
  const pt::ClassTypeFieldDesc* d = ctf->pctf_desc;
  switch (d->kind) {
    case pt::ClassTypeFieldDesc::Kind::Pctf_inherit:
      return builtin_attributes::warning_scope(ctf->pctf_attributes, [&] {
        const tt::ClassType* parent =
            class_type_rec(env, VirtualFlag::Virtual, self_scope, as<pt::Pctf_inherit>(d)->cty);
        complete_class_type(parent->cltyp_loc, env, VirtualFlag::Virtual, Kind::Class_type, parent->cltyp_type);
        inherit_class_type(false, loc, env, sign, parent->cltyp_type);
        return mkctf(mkd(tt::Tctf_inherit{{FK::Tctf_inherit}, parent}));
      });
    case pt::ClassTypeFieldDesc::Kind::Pctf_val: {
      auto* v = as<pt::Pctf_val>(d);
      return builtin_attributes::warning_scope(ctf->pctf_attributes, [&] {
        const tt::CoreType* cty = typetexp::transl_simple_type(env, nullptr, false, v->ty);
        TypeExpr* ty = cty->ctyp_type;
        add_instance_variable(false, loc, env, v->label.txt, v->mut, v->virt, ty, sign);
        return mkctf(mkd(tt::Tctf_val{{FK::Tctf_val}, v->label.txt, v->mut, v->virt, cty}));
      });
    }
    case pt::ClassTypeFieldDesc::Kind::Pctf_method: {
      auto* m = as<pt::Pctf_method>(d);
      return builtin_attributes::warning_scope(ctf->pctf_attributes, [&] {
        std::string_view lab = m->label.txt;
        const pt::CoreType* sty = force_poly(m->ty);
        auto* p = as<pt::Ptyp_poly>(sty->ptyp_desc);
        if (p && p->vars.empty() && m->priv == PrivateFlag::Public) {
          const pt::CoreType* sty2 = p->ty;
          TypeExpr* expected_ty = ctype::newvar();
          add_method(loc, env, lab, m->priv, m->virt, expected_ty, sign);
          tt::CoreType* returned_cty =
              ctyp(mkd(tt::Ttyp_any{{tt::CoreTypeDesc::Kind::Ttyp_any}}), ctype::newty(tnil()), env, loc);
          delayed_meth_specs.push_back([=] {
            const tt::CoreType* cty = typetexp::transl_simple_type_univars(env, sty2);
            TypeExpr* ty = cty->ctyp_type;
            unify_delayed_method_type(loc, env, lab, ty, expected_ty);
            returned_cty->ctyp_desc = mkd(tt::Ttyp_poly{{tt::CoreTypeDesc::Kind::Ttyp_poly}, {}, cty});
            returned_cty->ctyp_type = ty;
          });
          return mkctf(mkd(tt::Tctf_method{{FK::Tctf_method}, lab, m->priv, m->virt, returned_cty}));
        }
        const tt::CoreType* cty = typetexp::transl_simple_type(env, nullptr, false, sty);
        TypeExpr* ty = cty->ctyp_type;
        add_method(loc, env, lab, m->priv, m->virt, ty, sign);
        return mkctf(mkd(tt::Tctf_method{{FK::Tctf_method}, lab, m->priv, m->virt, cty}));
      });
    }
    case pt::ClassTypeFieldDesc::Kind::Pctf_constraint: {
      auto* c = as<pt::Pctf_constraint>(d);
      return builtin_attributes::warning_scope(ctf->pctf_attributes, [&] {
        auto [cty, cty2] = type_constraint(env, c->t1, c->t2, ctf->pctf_loc);
        return mkctf(mkd(tt::Tctf_constraint{{FK::Tctf_constraint}, cty, cty2}));
      });
    }
    case pt::ClassTypeFieldDesc::Kind::Pctf_attribute:
      builtin_attributes::warning_attribute(as<pt::Pctf_attribute>(d)->attr);
      return mkctf(mkd(tt::Tctf_attribute{{FK::Tctf_attribute}, as<pt::Pctf_attribute>(d)->attr}));
    case pt::ClassTypeFieldDesc::Kind::Pctf_extension: throw ErrorForward(as<pt::Pctf_extension>(d)->ext);
  }
  throw std::logic_error("class_type_field");
}

const tt::TClassSignature* class_signature(VirtualFlag virt, env::t env, const pt::ClassSignature* pcsig,
                                           long self_scope, const Location& loc) {
  const pt::CoreType* sty = pcsig->pcsig_self;
  ClassSignature* sign = ctype::new_class_signature();
  // Introduce a dummy method preventing self type from being closed.
  ctype::add_dummy_method(env, self_scope, sign);
  const tt::CoreType* self_cty = typetexp::transl_simple_type(env, nullptr, false, sty);
  TypeExpr* self_type = self_cty->ctyp_type;
  try {
    ctype::unify(env, self_type, sign->csig_self);
  } catch (const ctype::Unify&) {
    Error e = err(sty->ptyp_loc, env, EK::Pattern_type_clash);
    e.ty = self_type;
    raise_error(e);
  }
  // Class type fields
  std::vector<const tt::ClassTypeField*> fields;
  for (auto* f : pcsig->pcsig_fields) fields.push_back(class_type_field(env, sign, self_scope, f));
  check_virtual(loc, env, virt, Kind::Class_type, sign);
  return make<tt::TClassSignature>(self_cty, slice(fields), sign);
}

const tt::ClassType* class_type_aux(env::t env, VirtualFlag virt, long self_scope, const pt::ClassType* scty) {
  auto cltyp = [&](const tt::ClassTypeDesc* desc, const ClassType* typ) {
    return static_cast<const tt::ClassType*>(
        make<tt::ClassType>(desc, typ, env, scty->pcty_loc, scty->pcty_attributes));
  };
  using CK = tt::ClassTypeDesc::Kind;
  const pt::ClassTypeDesc* d = scty->pcty_desc;
  switch (d->kind) {
    case pt::ClassTypeDesc::Kind::Pcty_constr: {
      auto* c = as<pt::Pcty_constr>(d);
      auto [path, decl] = env::lookup_cltype(true, scty->pcty_loc, c->lid.txt, env);
      if (path::same(decl->clty_path, unbound_class())) {
        Error e = err(scty->pcty_loc, env, EK::Unbound_class_type_2);
        e.lid = c->lid.txt;
        raise_error(e);
      }
      auto [params, clty] = ctype::instance_class(decl->clty_params, decl->clty_type);
      // Adding a dummy method to the self type prevents it from being closed
      // / escaping.
      ctype::add_dummy_method(env, self_scope, signature_of_class_type(clty));
      if (params.size() != c->args.size()) {
        Error e = err(scty->pcty_loc, env, EK::Parameter_arity_mismatch);
        e.lid = c->lid.txt;
        e.n1 = static_cast<long>(params.size());
        e.n2 = static_cast<long>(c->args.size());
        raise_error(e);
      }
      std::vector<const tt::CoreType*> ctys;
      for (std::size_t k = 0; k < params.size(); ++k) {
        const pt::CoreType* sty = c->args[k];
        const tt::CoreType* cty2 = typetexp::transl_simple_type(env, nullptr, false, sty);
        TypeExpr* ty2 = cty2->ctyp_type;
        try {
          ctype::unify(env, ty2, params[k]);
        } catch (const ctype::Unify& u) {
          Error e = err(sty->ptyp_loc, env, EK::Parameter_mismatch);
          e.trace = u.err;
          raise_error(e);
        }
        ctys.push_back(cty2);
      }
      const ClassType* typ = cty_constr(path, slice(params), clty);
      // Check for unexpected virtual methods
      check_virtual_clty(scty->pcty_loc, env, virt, Kind::Class_type, typ);
      return cltyp(mkd(tt::Tcty_constr{{CK::Tcty_constr}, path, c->lid, slice(ctys)}), typ);
    }
    case pt::ClassTypeDesc::Kind::Pcty_signature: {
      const tt::TClassSignature* clsig =
          class_signature(virt, env, as<pt::Pcty_signature>(d)->sign, self_scope, scty->pcty_loc);
      return cltyp(mkd(tt::Tcty_signature{{CK::Tcty_signature}, clsig}), cty_signature(const_cast<ClassSignature*>(clsig->csig_type)));
    }
    case pt::ClassTypeDesc::Kind::Pcty_arrow: {
      auto* a = as<pt::Pcty_arrow>(d);
      const tt::CoreType* cty = typetexp::transl_simple_type(env, nullptr, false, a->ty);
      TypeExpr* ty = cty->ctyp_type;
      if (is_optional(a->label)) {
        std::vector<TypeExpr*> args{ty};
        ty = ctype::newty(tconstr(predef::paths().option, slice(args), make<MemoRef>(mnil())));
      }
      const tt::ClassType* clty = class_type_rec(env, virt, self_scope, a->cty);
      const ClassType* typ = cty_arrow(a->label, ty, clty->cltyp_type);
      return cltyp(mkd(tt::Tcty_arrow{{CK::Tcty_arrow}, a->label, cty, clty}), typ);
    }
    case pt::ClassTypeDesc::Kind::Pcty_open: {
      auto* o = as<pt::Pcty_open>(d);
      auto [od, newenv] = type_open_descr(nullptr, env, o->od);
      const tt::ClassType* clty = class_type_rec(newenv, virt, self_scope, o->cty);
      return cltyp(mkd(tt::Tcty_open{{CK::Tcty_open}, od, clty}), clty->cltyp_type);
    }
    case pt::ClassTypeDesc::Kind::Pcty_extension: throw ErrorForward(as<pt::Pcty_extension>(d)->ext);
  }
  throw std::logic_error("class_type_aux");
}

const tt::ClassType* class_type_rec(env::t env, VirtualFlag virt, long self_scope, const pt::ClassType* scty) {
  return builtin_attributes::warning_scope(scty->pcty_attributes,
                                           [&] { return class_type_aux(env, virt, self_scope, scty); });
}

const tt::ClassType* class_type(env::t env, VirtualFlag virt, long self_scope, const pt::ClassType* scty) {
  delayed_meth_specs.clear();
  const tt::ClassType* cty = class_type_rec(env, virt, self_scope, scty);
  std::vector<std::function<void()>> specs = std::move(delayed_meth_specs);
  delayed_meth_specs.clear();
  for (auto& f : specs) f();  // List.iter Lazy.force (List.rev !delayed_meth_specs)
  delayed_meth_specs.clear();
  return cty;
}

// ---- environments of class bodies ----------------------------------------------------------
env::t enter_ancestor_val(std::string_view name, env::t val_env) {
  return env::enter_unbound_value(name, env::ValueUnboundReason{env::ValueUnboundReason::Kind::Val_unbound_ancestor},
                                  val_env);
}
env::t enter_self_val(std::string_view name, env::t val_env) {
  return env::enter_unbound_value(name, env::ValueUnboundReason{env::ValueUnboundReason::Kind::Val_unbound_self},
                                  val_env);
}
env::t enter_instance_var_val(std::string_view name, env::t val_env) {
  return env::enter_unbound_value(
      name, env::ValueUnboundReason{env::ValueUnboundReason::Kind::Val_unbound_instance_variable}, val_env);
}

std::pair<Ident::t, env::t> enter_ancestor_met(const Location& loc, std::string_view name, ClassSignature* sign,
                                               StrMap<Ident::t>* meths, std::string_view cl_num, TypeExpr* ty,
                                               Attributes attrs, env::t met_env) {
  ValueKind kind{ValueKind::Kind::Val_anc};
  kind.obj = fresh_identity();
  kind.sign = sign;
  kind.meths = meths;
  kind.cl_num = cl_num;
  auto* desc = make<ValueDescription>(ty, kind, loc, attrs, uid::mk(env::get_current_unit()));
  return env::enter_value(name, desc, met_env, [](std::string s) {
    return warnings::Warning::with_s(warnings::Warning::K::Unused_ancestor, s);
  });
}

env::t add_self_met(const Location& loc, Ident::t id, ClassSignature* sign, bool self_virtual,
                    StrMap<Ident::t>* meths, const StrMap<Ident::t>& vars, std::string_view cl_num, bool as_var,
                    TypeExpr* ty, Attributes attrs, env::t met_env) {
  warnings::Warning::K wk = as_var ? warnings::Warning::K::Unused_var : warnings::Warning::K::Unused_var_strict;
  ValueKind kind{ValueKind::Kind::Val_self};
  kind.obj = fresh_identity();
  kind.sign = sign;
  kind.self_virtual = self_virtual;
  kind.meths = meths;
  kind.vars = vars;
  kind.cl_num = cl_num;
  auto* desc = make<ValueDescription>(ty, kind, loc, attrs, uid::mk(env::get_current_unit()));
  return env::add_value(id, desc, met_env, [wk](std::string s) { return warnings::Warning::with_s(wk, s); });
}

env::t add_instance_var_met(const Location& loc, std::string_view label, Ident::t id, const ClassSignature* sign,
                            std::string_view cl_num, Attributes attrs, env::t met_env) {
  const VarEntry* v = sign->csig_vars.find_opt(label);
  if (!v) throw std::logic_error("add_instance_var_met");
  ValueKind kind{ValueKind::Kind::Val_ivar};
  kind.obj = fresh_identity();
  kind.ivar_mut = v->mut;
  kind.ivar_name = cl_num;
  auto* desc = make<ValueDescription>(v->ty, kind, loc, attrs, uid::mk(env::get_current_unit()));
  return env::add_value(id, desc, met_env);
}

env::t add_instance_vars_met(const Location& loc, Slice<std::pair<std::string_view, Ident::t>> vars,
                             const ClassSignature* sign, std::string_view cl_num, env::t met_env) {
  for (auto& [label, id] : vars) met_env = add_instance_var_met(loc, label, id, sign, cl_num, {}, met_env);
  return met_env;
}


// ---- class structures -----------------------------------------------------------------------
struct IntermediateClassField {
  enum class Kind { Inherit, Virtual_val, Concrete_val, Virtual_method, Concrete_method, Constraint, Initializer,
                    Attribute };
  Kind kind;
  OverrideFlag override_ = OverrideFlag::Fresh;
  const tt::ClassExpr* parent = nullptr;                    // Inherit
  OptStr super;                                             // Inherit
  std::vector<std::pair<std::string_view, Ident::t>> inherited_vars;  // Inherit (list order)
  std::vector<std::pair<std::string_view, Ident::t>> super_meths;     // Inherit (list order)
  pt::StrLoc label;                                         // vals / methods
  MutableFlag mut = MutableFlag::Immutable;
  Ident::t id = nullptr;
  const tt::CoreType* cty = nullptr;                        // Virtual_val / Virtual_method
  const tt::Expression* definition = nullptr;               // Concrete_val
  bool already_declared = false;
  PrivateFlag priv = PrivateFlag::Public;
  const pt::Expression* sdefinition = nullptr;              // Concrete_method / Initializer (sexpr)
  const tt::CoreType* cty1 = nullptr;                       // Constraint
  const tt::CoreType* cty2 = nullptr;
  const pt::Attribute* attribute = nullptr;                 // Attribute
  Location loc;
  pt::Attributes attributes;
  warnings::State warning_state;  // Concrete_method / Initializer
};

struct FirstPassAcc {
  std::vector<IntermediateClassField> rev_fields;  // in order of creation
  env::t val_env;
  env::t par_env;
  MethSet concrete_meths;
  MethSet concrete_vals;
  MethSet local_meths;
  MethSet local_vals;
  StrMap<Ident::t> vars;
};

const tt::ClassExpr* class_expr(std::string_view cl_num, env::t val_env, env::t met_env,
                                VirtualFlag virt, long self_scope, const pt::ClassExpr* scl);

MethSet set_inter(const MethSet& a, const MethSet& b) {
  MethSet r;
  for (auto& x : a)
    if (b.count(x)) r.insert(x);
  return r;
}

// (already_declared, val_env, par_env, id, vars) for an instance variable
struct DeclaredVar {
  bool already_declared;
  env::t val_env, par_env;
  Ident::t id;
  StrMap<Ident::t> vars;
};
DeclaredVar declare_var(std::string_view name, env::t val_env, env::t par_env, const StrMap<Ident::t>& vars) {
  if (const Ident::t* id = vars.find_opt(name)) return {true, val_env, par_env, *id, vars};
  val_env = enter_instance_var_val(name, val_env);
  par_env = enter_instance_var_val(name, par_env);
  Ident::t id = Ident::create_local(name);
  return {false, val_env, par_env, id, vars.add(name, id)};
}

void class_field_first_pass(const Location& self_loc, std::string_view cl_num, ClassSignature* sign,
                            long self_scope, FirstPassAcc& acc, const pt::ClassField* cf) {
  const Location& loc = cf->pcf_loc;
  const pt::Attributes& attributes = cf->pcf_attributes;
  const pt::ClassFieldDesc* d = cf->pcf_desc;
  using FK = pt::ClassFieldDesc::Kind;
  using IK = IntermediateClassField::Kind;
  switch (d->kind) {
    case FK::Pcf_inherit: {
      auto* in = as<pt::Pcf_inherit>(d);
      builtin_attributes::warning_scope(attributes, [&] {
        const tt::ClassExpr* parent =
            class_expr(cl_num, acc.val_env, acc.par_env, VirtualFlag::Virtual, self_scope, in->ce);
        complete_class_type(parent->cl_loc, acc.par_env, VirtualFlag::Virtual, Kind::Class, parent->cl_type);
        inherit_class_type(true, loc, acc.val_env, sign, parent->cl_type);
        ClassSignature* parent_sign = signature_of_class_type(parent->cl_type);
        MethSet new_concrete_meths = concrete_methods(parent_sign);
        MethSet new_concrete_vals = concrete_instance_vars(parent_sign);
        MethSet over_meths = set_inter(new_concrete_meths, acc.concrete_meths);
        MethSet over_vals = set_inter(new_concrete_vals, acc.concrete_vals);
        if (in->ovr == OverrideFlag::Fresh) {
          std::string cname = "inherited";
          if (parent->cl_type->kind == ClassType::Kind::Cty_constr) cname = path::name(parent->cl_type->path);
          if (!over_meths.empty()) {
            std::vector<std::string> l{cname};
            for (auto m : over_meths) l.push_back(std::string(m));
            location::prerr_warning(loc, warnings::Warning::with_l(warnings::Warning::K::Method_override, l));
          }
          if (!over_vals.empty()) {
            std::vector<std::string> l{cname};
            for (auto m : over_vals) l.push_back(std::string(m));
            location::prerr_warning(loc,
                                    warnings::Warning::with_l(warnings::Warning::K::Instance_variable_override, l));
          }
        }
        if (in->ovr == OverrideFlag::Override && over_meths.empty() && over_vals.empty()) {
          Error e = err(loc, acc.val_env, EK::No_overriding);
          raise_error(e);
        }
        MethSet concrete_vals = new_concrete_vals;
        concrete_vals.insert(acc.concrete_vals.begin(), acc.concrete_vals.end());
        MethSet concrete_meths = new_concrete_meths;
        concrete_meths.insert(acc.concrete_meths.begin(), acc.concrete_meths.end());
        env::t val_env = acc.val_env, par_env = acc.par_env;
        std::vector<std::pair<std::string_view, Ident::t>> inherited_vars;  // list order
        StrMap<Ident::t> vars = acc.vars;
        parent_sign->csig_vars.iter([&](std::string_view label, const VarEntry&) {
          val_env = enter_instance_var_val(label, val_env);
          par_env = enter_instance_var_val(label, par_env);
          Ident::t id = Ident::create_local(label);
          inherited_vars.insert(inherited_vars.begin(), {label, id});
          vars = vars.add(label, id);
        });
        // Methods available through super
        std::vector<std::pair<std::string_view, Ident::t>> super_meths;
        for (auto& label : new_concrete_meths) super_meths.insert(super_meths.begin(), {label, Ident::create_local(label)});
        // Super
        OptStr super;
        if (in->as) {
          val_env = enter_ancestor_val(in->as->txt, val_env);
          par_env = enter_ancestor_val(in->as->txt, par_env);
          super = OptStr{true, in->as->txt};
        }
        IntermediateClassField field{IK::Inherit};
        field.override_ = in->ovr;
        field.parent = parent;
        field.super = super;
        field.inherited_vars = inherited_vars;
        field.super_meths = super_meths;
        field.loc = loc;
        field.attributes = attributes;
        acc.rev_fields.push_back(field);
        acc.val_env = val_env;
        acc.par_env = par_env;
        acc.concrete_meths = concrete_meths;
        acc.concrete_vals = concrete_vals;
        acc.vars = vars;
        return 0;
      });
      return;
    }
    case FK::Pcf_val: {
      auto* v = as<pt::Pcf_val>(d);
      const pt::StrLoc& label = v->label;
      if (v->kind_.kind == pt::ClassFieldKind::Kind::Cfk_virtual) {
        builtin_attributes::warning_scope(attributes, [&] {
          const tt::CoreType* cty = ctype::with_local_level_generalize_structure_if_principal(
              [&] { return typetexp::transl_simple_type(acc.val_env, nullptr, false, v->kind_.ty); });
          add_instance_variable(true, loc, acc.val_env, label.txt, v->mut, VirtualFlag::Virtual, cty->ctyp_type, sign);
          DeclaredVar dv = declare_var(label.txt, acc.val_env, acc.par_env, acc.vars);
          IntermediateClassField field{IK::Virtual_val};
          field.label = label;
          field.mut = v->mut;
          field.id = dv.id;
          field.cty = cty;
          field.already_declared = dv.already_declared;
          field.loc = loc;
          field.attributes = attributes;
          acc.rev_fields.push_back(field);
          acc.val_env = dv.val_env;
          acc.par_env = dv.par_env;
          acc.vars = dv.vars;
          return 0;
        });
        return;
      }
      builtin_attributes::warning_scope(attributes, [&] {
        if (acc.local_vals.count(label.txt)) {
          Error e = err(loc, acc.val_env, EK::Duplicate);
          e.name = "instance variable";
          e.name2 = std::string(label.txt);
          raise_error(e);
        }
        if (acc.concrete_vals.count(label.txt)) {
          if (v->kind_.ovr == OverrideFlag::Fresh)
            location::prerr_warning(label.loc, warnings::Warning::with_l(warnings::Warning::K::Instance_variable_override,
                                                                         {std::string(label.txt)}));
        } else if (v->kind_.ovr == OverrideFlag::Override) {
          Error e = err(loc, acc.val_env, EK::No_overriding);
          e.name = "instance variable";
          e.name2 = std::string(label.txt);
          raise_error(e);
        }
        const tt::Expression* definition = ctype::with_local_level_generalize_structure_if_principal(
            [&] { return tc::type_exp(acc.val_env, v->kind_.exp); });
        add_instance_variable(true, loc, acc.val_env, label.txt, v->mut, VirtualFlag::Concrete, definition->exp_type,
                              sign);
        DeclaredVar dv = declare_var(label.txt, acc.val_env, acc.par_env, acc.vars);
        IntermediateClassField field{IK::Concrete_val};
        field.label = label;
        field.mut = v->mut;
        field.id = dv.id;
        field.override_ = v->kind_.ovr;
        field.definition = definition;
        field.already_declared = dv.already_declared;
        field.loc = loc;
        field.attributes = attributes;
        acc.rev_fields.push_back(field);
        acc.val_env = dv.val_env;
        acc.par_env = dv.par_env;
        acc.concrete_vals.insert(label.txt);
        acc.local_vals.insert(label.txt);
        acc.vars = dv.vars;
        return 0;
      });
      return;
    }
    case FK::Pcf_method: {
      auto* m = as<pt::Pcf_method>(d);
      const pt::StrLoc& label = m->label;
      if (m->kind_.kind == pt::ClassFieldKind::Kind::Cfk_virtual) {
        builtin_attributes::warning_scope(attributes, [&] {
          const pt::CoreType* sty = force_poly(m->kind_.ty);
          const tt::CoreType* cty = typetexp::transl_simple_type(acc.val_env, nullptr, false, sty);
          TypeExpr* ty = cty->ctyp_type;
          add_method(loc, acc.val_env, label.txt, m->priv, VirtualFlag::Virtual, ty, sign);
          IntermediateClassField field{IK::Virtual_method};
          field.label = label;
          field.priv = m->priv;
          field.cty = cty;
          field.loc = loc;
          field.attributes = attributes;
          acc.rev_fields.push_back(field);
          return 0;
        });
        return;
      }
      builtin_attributes::warning_scope(attributes, [&] {
        if (acc.local_meths.count(label.txt)) {
          Error e = err(loc, acc.val_env, EK::Duplicate);
          e.name = "method";
          e.name2 = std::string(label.txt);
          raise_error(e);
        }
        if (acc.concrete_meths.count(label.txt)) {
          if (m->kind_.ovr == OverrideFlag::Fresh)
            location::prerr_warning(loc, warnings::Warning::with_l(warnings::Warning::K::Method_override,
                                                                   {std::string(label.txt)}));
        } else if (m->kind_.ovr == OverrideFlag::Override) {
          Error e = err(loc, acc.val_env, EK::No_overriding);
          e.name = "method";
          e.name2 = std::string(label.txt);
          raise_error(e);
        }
        const pt::Expression* expr = m->kind_.exp;
        if (expr->pexp_desc->kind != pt::ExpressionDesc::Kind::Pexp_poly)
          expr = ah::exp_mk(mkd(pt::Pexp_poly{{pt::ExpressionDesc::Kind::Pexp_poly}, expr, nullptr}), expr->pexp_loc);
        auto* pp = as<pt::Pexp_poly>(expr->pexp_desc);
        const pt::Expression* sbody = pp->exp;
        TypeExpr* ty;
        if (!pp->ty) {
          ty = ctype::newvar();
        } else {
          const pt::CoreType* sty = force_poly(pp->ty);
          ty = typetexp::transl_simple_type(acc.val_env, nullptr, false, sty)->ctyp_type;
        }
        add_method(loc, acc.val_env, label.txt, m->priv, VirtualFlag::Concrete, ty, sign);
        try {
          const TypeDesc* td = get_desc(ty);
          if (td->kind == DescKind::Tvar) {
            TypeExpr* ty2 = ctype::newvar();
            ctype::unify(acc.val_env, ctype::newmono(ty2), ty);
            tc::type_approx(acc.val_env, sbody, ty2);
          } else if (auto* p = as<Tpoly>(td)) {
            TypeExpr* ty1 = ctype::instance_poly(p->vars, p->body);
            tc::type_approx(acc.val_env, sbody, ty1);
          } else {
            throw std::logic_error("class_field_first_pass: method type");
          }
        } catch (const ctype::Unify& u) {
          Error e = err(loc, acc.val_env, EK::Field_type_mismatch);
          e.name = "method";
          e.name2 = std::string(label.txt);
          e.trace = u.err;
          raise_error(e);
        }
        const pt::Expression* sdefinition = make_method(self_loc, cl_num, expr);
        IntermediateClassField field{IK::Concrete_method};
        field.label = label;
        field.priv = m->priv;
        field.override_ = m->kind_.ovr;
        field.sdefinition = sdefinition;
        field.warning_state = warnings::backup();
        field.loc = loc;
        field.attributes = attributes;
        acc.rev_fields.push_back(field);
        acc.concrete_meths.insert(label.txt);
        acc.local_meths.insert(label.txt);
        return 0;
      });
      return;
    }
    case FK::Pcf_constraint: {
      auto* c = as<pt::Pcf_constraint>(d);
      builtin_attributes::warning_scope(attributes, [&] {
        auto [cty1, cty2] = type_constraint(acc.val_env, c->t1, c->t2, loc);
        IntermediateClassField field{IK::Constraint};
        field.cty1 = cty1;
        field.cty2 = cty2;
        field.loc = loc;
        field.attributes = attributes;
        acc.rev_fields.push_back(field);
        return 0;
      });
      return;
    }
    case FK::Pcf_initializer: {
      builtin_attributes::warning_scope(attributes, [&] {
        const pt::Expression* sexpr = make_method(self_loc, cl_num, as<pt::Pcf_initializer>(d)->exp);
        IntermediateClassField field{IK::Initializer};
        field.sdefinition = sexpr;
        field.warning_state = warnings::backup();
        field.loc = loc;
        field.attributes = attributes;
        acc.rev_fields.push_back(field);
        return 0;
      });
      return;
    }
    case FK::Pcf_attribute: {
      builtin_attributes::warning_attribute(as<pt::Pcf_attribute>(d)->attr);
      IntermediateClassField field{IK::Attribute};
      field.attribute = as<pt::Pcf_attribute>(d)->attr;
      field.loc = loc;
      field.attributes = attributes;
      acc.rev_fields.push_back(field);
      return;
    }
    case FK::Pcf_extension: throw ErrorForward(as<pt::Pcf_extension>(d)->ext);
  }
}

std::pair<std::vector<IntermediateClassField>, StrMap<Ident::t>> class_fields_first_pass(
    const Location& self_loc, std::string_view cl_num, ClassSignature* sign, long self_scope,
    env::t val_env, env::t par_env, Slice<const pt::ClassField*> cfs) {
  FirstPassAcc acc{{}, val_env, par_env, {}, {}, {}, {}, {}};
  for (auto* cf : cfs) class_field_first_pass(self_loc, cl_num, sign, self_scope, acc, cf);
  return {acc.rev_fields, acc.vars};
}

std::pair<env::t, const tt::ClassField*> class_field_second_pass(std::string_view cl_num, ClassSignature* sign,
                                                                 env::t met_env, const IntermediateClassField& field) {
  auto mkcf = [](const tt::ClassFieldDesc* desc, const Location& loc, const pt::Attributes& attrs) {
    return static_cast<const tt::ClassField*>(make<tt::ClassField>(desc, loc, attrs));
  };
  using IK = IntermediateClassField::Kind;
  using CFK = tt::ClassFieldDesc::Kind;
  switch (field.kind) {
    case IK::Inherit: {
      met_env = add_instance_vars_met(field.loc, slice(field.inherited_vars), sign, cl_num, met_env);
      if (field.super.some) {
        auto* meths = make<StrMap<Ident::t>>();
        for (auto& [label, id] : field.super_meths) *meths = meths->add(label, id);
        TypeExpr* ty = self_type(field.parent->cl_type);
        met_env = enter_ancestor_met(field.loc, field.super.v, sign, meths, cl_num, ty, {}, met_env).second;
      }
      auto* desc = mkd(tt::Tcf_inherit{{CFK::Tcf_inherit}, field.override_, field.parent, field.super,
                                       slice(field.inherited_vars), slice(field.super_meths)});
      return {met_env, mkcf(desc, field.loc, field.attributes)};
    }
    case IK::Virtual_val:
    case IK::Concrete_val: {
      if (!field.already_declared)
        met_env = add_instance_var_met(field.loc, field.label.txt, field.id, sign, cl_num,
                                       pt::types_attributes(field.attributes), met_env);
      tt::ClassFieldKind kind;
      if (field.kind == IK::Virtual_val) kind = tt::ClassFieldKind{true, field.cty};
      else kind = tt::ClassFieldKind{false, nullptr, field.override_, field.definition};
      auto* desc = mkd(tt::Tcf_val{{CFK::Tcf_val}, field.label, field.mut, field.id, kind, field.already_declared});
      return {met_env, mkcf(desc, field.loc, field.attributes)};
    }
    case IK::Virtual_method: {
      auto* desc = mkd(tt::Tcf_method{{CFK::Tcf_method}, field.label, field.priv, tt::ClassFieldKind{true, field.cty}});
      return {met_env, mkcf(desc, field.loc, field.attributes)};
    }
    case IK::Concrete_method: return warnings::with_state(field.warning_state, [&]() -> std::pair<env::t, const tt::ClassField*> {
      TypeExpr* ty = method_type(field.label.txt, sign);
      TypeExpr* self_param_type = newgenmono(sign->csig_self);
      tc::TypeExpected meth_type =
          tc::mk_expected(newgenty(tarrow(ArgLabel::nolabel(), self_param_type, ty, commu_ok())));
      const tt::Expression* texp =
          ctype::with_raised_nongen_level([&] { return tc::type_expect(met_env, field.sdefinition, meth_type); });
      auto* desc = mkd(tt::Tcf_method{{CFK::Tcf_method}, field.label, field.priv,
                                      tt::ClassFieldKind{false, nullptr, field.override_, texp}});
      return {met_env, mkcf(desc, field.loc, field.attributes)};
    });
    case IK::Constraint:
      return {met_env, mkcf(mkd(tt::Tcf_constraint{{CFK::Tcf_constraint}, field.cty1, field.cty2}), field.loc,
                            field.attributes)};
    case IK::Initializer: return warnings::with_state(field.warning_state, [&]() -> std::pair<env::t, const tt::ClassField*> {
      TypeExpr* unit_type = ctype::instance(predef::type_unit());
      TypeExpr* self_param_type = ctype::newmono(sign->csig_self);
      tc::TypeExpected meth_type =
          tc::mk_expected(ctype::newty(tarrow(ArgLabel::nolabel(), self_param_type, unit_type, commu_ok())));
      const tt::Expression* texp =
          ctype::with_raised_nongen_level([&] { return tc::type_expect(met_env, field.sdefinition, meth_type); });
      return {met_env, mkcf(mkd(tt::Tcf_initializer{{CFK::Tcf_initializer}, texp}), field.loc, field.attributes)};
    });
    case IK::Attribute:
      return {met_env, mkcf(mkd(tt::Tcf_attribute{{CFK::Tcf_attribute}, field.attribute}), field.loc,
                            field.attributes)};
  }
  throw std::logic_error("class_field_second_pass");
}

std::vector<const tt::ClassField*> class_fields_second_pass(std::string_view cl_num, ClassSignature* sign,
                                                            env::t met_env,
                                                            const std::vector<IntermediateClassField>& fields) {
  std::vector<const tt::ClassField*> cfs;
  for (auto& f : fields) {
    auto [me, cf] = class_field_second_pass(cl_num, sign, met_env, f);
    met_env = me;
    cfs.push_back(cf);
  }
  return cfs;
}

// N.B. the self type of a final object type doesn't contain a dummy method
// in the beginning (see typeclass.ml).
const tt::ClassStructure* class_structure(std::string_view cl_num, VirtualFlag virt, long self_scope, Final final,
                                          env::t val_env, env::t met_env, const Location& loc,
                                          const pt::ClassStructure* cstr) {
  const pt::Pattern* spat = cstr->pcstr_self;
  // Environment for substructures
  env::t par_env = met_env;
  // Location of self. Used for locations of self arguments
  Location self_loc = spat->ppat_loc;
  self_loc.loc_ghost = true;
  self_loc = location::distinct_record(self_loc);  // {spat.ppat_loc with ...}: a record of its own
  ClassSignature* sign = ctype::new_class_signature();
  // Adding a dummy method to the signature prevents it from being closed /
  // escaping. That isn't needed for objects though.
  if (final == Final::Not_final) ctype::add_dummy_method(val_env, self_scope, sign);
  // Self binder
  auto [self_pat, self_pat_vars] = tc::type_self_pattern(val_env, spat);
  for (std::size_t k = self_pat_vars.size(); k-- > 0;) {  // List.fold_right
    std::string_view name = ident::name(self_pat_vars[k].pv_id);
    val_env = enter_self_val(name, val_env);
    par_env = enter_self_val(name, par_env);
  }
  // Check that the binder has a correct type
  try {
    ctype::unify(val_env, self_pat->pat_type, sign->csig_self);
  } catch (const ctype::Unify&) {
    Error e = err(spat->ppat_loc, val_env, EK::Pattern_type_clash);
    e.ty = self_pat->pat_type;
    raise_error(e);
  }
  // Typing of class fields
  auto [fields, vars] = class_fields_first_pass(self_loc, cl_num, sign, self_scope, val_env, par_env,
                                                cstr->pcstr_fields);
  Kind kind = kind_of_final(final);
  // Check for unexpected virtual methods
  check_virtual(loc, val_env, virt, kind, sign);
  // Update the class signature
  update_class_signature(loc, val_env, virt, kind, sign);
  StrMap<Ident::t> meths;
  sign->csig_meths.iter([&](std::string_view label, const MethEntry&) {
    meths = meths.add(label, Ident::create_local(label));
  });
  // Close the signature if it is final
  if (final == Final::Final && !ctype::close_class_signature(val_env, sign)) {
    Error e = err(loc, val_env, EK::Closing_self_type);
    e.sign = sign;
    raise_error(e);
  }
  // Typing of method bodies
  ctype::generalize_class_signature_spine(sign);
  bool self_virtual = virt == VirtualFlag::Virtual;
  auto* meths_ref = make<StrMap<Ident::t>>(meths);  // Self_virtual (ref meths) | Self_concrete meths
  for (std::size_t k = self_pat_vars.size(); k-- > 0;) {  // List.fold_right
    const tc::PatternVariable& pv = self_pat_vars[k];
    met_env = add_self_met(pv.pv_loc, pv.pv_id, sign, self_virtual, meths_ref, vars, cl_num,
                           pv.pv_kind == tc::PatternVariableKind::As_var, pv.pv_type,
                           pt::types_attributes(pv.pv_attributes), met_env);
  }
  std::vector<const tt::ClassField*> tfields = class_fields_second_pass(cl_num, sign, met_env, fields);
  // Update the class signature and warn about public methods made private
  update_class_signature(loc, val_env, virt, kind, sign, true);
  return make<tt::ClassStructure>(self_pat, slice(tfields), sign, *meths_ref);
}

tt::ClassExpr* mk_cl(const tt::ClassExprDesc* desc, const Location& loc, const ClassType* ty, env::t env,
                     const pt::Attributes& attrs) {
  tt::ClassExpr* c = make<tt::ClassExpr>(desc, loc, ty, env, attrs);
  cmt_format::add_saved_type({cmt_format::BinaryPart::Kind::Partial_class_expr, false, c});  // rc
  return c;
}

const tt::ClassExpr* class_expr_aux(std::string_view cl_num, env::t val_env, env::t met_env,
                                    VirtualFlag virt, long self_scope, const pt::ClassExpr* scl) {
  const pt::ClassExprDesc* d = scl->pcl_desc;
  using PK = pt::ClassExprDesc::Kind;
  switch (d->kind) {
    case PK::Pcl_constr: {
      auto* c = as<pt::Pcl_constr>(d);
      auto [path, decl] = env::lookup_class(true, scl->pcl_loc, c->lid.txt, val_env);
      if (path::same(decl->cty_path, unbound_class())) {
        Error e = err(scl->pcl_loc, val_env, EK::Unbound_class_2);
        e.lid = c->lid.txt;
        raise_error(e);
      }
      std::vector<const tt::CoreType*> tyl;
      for (auto* sty : c->args) tyl.push_back(typetexp::transl_simple_type(val_env, nullptr, false, sty));
      auto [params, clty] = ctype::instance_class(decl->cty_params, decl->cty_type);
      const ClassType* clty2 = abbreviate_class_type(path, slice(params), clty);
      // Adding a dummy method to the self type prevents it from being closed
      // / escaping.
      ctype::add_dummy_method(val_env, self_scope, signature_of_class_type(clty2));
      if (params.size() != tyl.size()) {
        Error e = err(scl->pcl_loc, val_env, EK::Parameter_arity_mismatch);
        e.lid = c->lid.txt;
        e.n1 = static_cast<long>(params.size());
        e.n2 = static_cast<long>(tyl.size());
        raise_error(e);
      }
      for (std::size_t k = 0; k < tyl.size(); ++k) {
        try {
          ctype::unify(val_env, tyl[k]->ctyp_type, params[k]);
        } catch (const ctype::Unify& u) {
          Error e = err(tyl[k]->ctyp_loc, val_env, EK::Parameter_mismatch);
          e.trace = u.err;
          raise_error(e);
        }
      }
      // Check for unexpected virtual methods
      check_virtual_clty(scl->pcl_loc, val_env, virt, Kind::Class, clty2);
      tt::ClassExpr* cl = mk_cl(mkd(tt::Tcl_ident{{TK::Tcl_ident}, path, c->lid, slice(tyl)}), scl->pcl_loc, clty2,
                                val_env, scl->pcl_attributes);
      Constraints cs = extract_constraints(clty);
      // attributes are kept on the inner cl node
      return mk_cl(mkd(tt::Tcl_constraint{{TK::Tcl_constraint}, cl, nullptr, cs.vals, cs.meths, cs.concrs}),
                   scl->pcl_loc, clty2, val_env, {});
    }
    case PK::Pcl_structure: {
      const tt::ClassStructure* desc =
          class_structure(cl_num, virt, self_scope, Final::Not_final, val_env, met_env, scl->pcl_loc, as<pt::Pcl_structure>(d)->cs);
      return mk_cl(mkd(tt::Tcl_structure{{TK::Tcl_structure}, desc}), scl->pcl_loc,
                   cty_signature(const_cast<ClassSignature*>(desc->cstr_type)), val_env, scl->pcl_attributes);
    }
    case PK::Pcl_fun: {
      auto* f = as<pt::Pcl_fun>(d);
      const pt::Pattern* spat = f->pat;
      if (tc::has_poly_constraint(spat)) raise_error(err(spat->ppat_loc, val_env, EK::Polymorphic_class_parameter));
      if (f->default_) {
        const pt::Expression* dflt = f->default_;
        const Location& loc = dflt->pexp_loc;
        auto predef_lid = [](std::string_view n) {
          return pt::LidLoc{Longident::ldot(Longident::lident(OCAML_LIT("*predef*")), location::none(), n, location::none()),
                            location::none()};
        };
        auto pat_var = [&](std::string_view n) {
          return ah::pat_mk(mkd(pt::Ppat_var{{pt::PatternDesc::Kind::Ppat_var}, pt::StrLoc{n, location::none()}}), loc);
        };
        auto exp_ident = [&](std::string_view n) {
          return ah::exp_ident(loc, pt::LidLoc{Longident::lident(n), location::none()});
        };
        const pt::Pattern* some_pat = ah::pat_mk(
            mkd(pt::Ppat_construct{{pt::PatternDesc::Kind::Ppat_construct}, predef_lid(OCAML_LIT("Some")),
                                   make<pt::ConstructArg>(pt::ConstructArg{{}, pat_var(OCAML_LIT("*sth*"))})}),
            loc);
        const pt::Pattern* none_pat = ah::pat_mk(
            mkd(pt::Ppat_construct{{pt::PatternDesc::Kind::Ppat_construct}, predef_lid(OCAML_LIT("None")), nullptr}), loc);
        std::vector<const pt::Case*> scases{make<pt::Case>(some_pat, nullptr, exp_ident(OCAML_LIT("*sth*"))),
                                            make<pt::Case>(none_pat, nullptr, dflt)};
        const pt::Expression* smatch = ah::exp_mk(
            mkd(pt::Pexp_match{{pt::ExpressionDesc::Kind::Pexp_match}, exp_ident(OCAML_LIT("*opt*")), slice(scases)}), loc);
        std::vector<const pt::ValueBinding*> vbs{
            make<pt::ValueBinding>(spat, smatch, nullptr, pt::Attributes{}, location::none())};
        auto* slet = make<pt::ClassExpr>(
            mkd(pt::Pcl_let{{pt::ClassExprDesc::Kind::Pcl_let}, RecFlag::Nonrecursive, slice(vbs), f->body}),
            scl->pcl_loc, pt::Attributes{});
        // Note: we don't put the '#default' attribute, as it is not detected
        // for class-level let bindings.  See #5975.
        auto* sfun = make<pt::ClassExpr>(
            mkd(pt::Pcl_fun{{pt::ClassExprDesc::Kind::Pcl_fun}, f->label, nullptr, pat_var(OCAML_LIT("*opt*")), slet}),
            scl->pcl_loc, pt::Attributes{});
        return class_expr(cl_num, val_env, met_env, virt, self_scope, sfun);
      }
      tc::ClassArgPatternResult r = ctype::with_local_level_generalize_structure_if_principal(
          [&] { return tc::type_class_arg_pattern(cl_num, val_env, met_env, f->label, spat); });
      const tt::Pattern* pat = r.pat;
      env::t val_env2 = r.val_env;
      std::vector<tt::IdentExpression> pv;
      for (auto& v : r.pv) {
        Path::t path = Path::pident(v.id);
        // do not mark the value as being used
        const ValueDescription* vd = env::find_value(path, val_env2);
        TypeExpr* ty = ctype::instance(vd->val_type);
        auto* e = make<tt::Expression>(
            mkd(tt::Texp_ident{{tt::ExpressionDesc::Kind::Texp_ident}, path,
                               pt::LidLoc{Longident::lident(ident::name(v.id2)), location::none()}, vd}),
            location::none(), Slice<tt::ExpExtraItem>{}, ty, val_env2, tt::Attributes{});
        pv.push_back({v.id2, e});
      }
      tt::Partial partial;
      {
        const tt::Expression* dummy = tc::type_exp(
            val_env, ah::exp_mk(mkd(pt::Pexp_unreachable{{pt::ExpressionDesc::Kind::Pexp_unreachable}}),
                                location::none()));
        auto* c = make<tt::Case>(pat, nullptr, nullptr, dummy);
        std::vector<parmatch::TypedCase> cases{parmatch::typed_case(c)};
        partial = tc::check_partial(ctype::get_current_level(), val_env, pat->pat_type, pat->pat_loc, cases);
      }
      const tt::ClassExpr* cl = ctype::with_raised_nongen_level(
          [&] { return class_expr(cl_num, val_env2, r.met_env, virt, self_scope, f->body); });
      auto not_nolabel_function = [](const ClassType* t) {
        for (; t->kind == ClassType::Kind::Cty_arrow; t = t->cty)
          if (t->label.kind == ArgLabel::Kind::Nolabel) return false;
        return true;
      };
      if (is_optional(f->label) && not_nolabel_function(cl->cl_type))
        location::prerr_warning(pat->pat_loc, warnings::Warning::make(warnings::Warning::K::Unerasable_optional_argument));
      TypeExpr* ity = ctype::instance(pat->pat_type);
      return mk_cl(mkd(tt::Tcl_fun{{TK::Tcl_fun}, f->label, pat, slice(pv), cl, partial}), scl->pcl_loc,
                   cty_arrow(f->label, ity, cl->cl_type), val_env, scl->pcl_attributes);
    }
    case PK::Pcl_apply: {
      auto* a = as<pt::Pcl_apply>(d);
      if (a->args.empty()) throw std::logic_error("class_expr: Pcl_apply []");
      std::vector<std::pair<ArgLabel, const pt::Expression*>> sargs0;
      for (auto& x : a->args) sargs0.push_back({x.label, x.exp});
      const tt::ClassExpr* cl = ctype::with_local_level_generalize_structure_if_principal(
          [&] { return class_expr(cl_num, val_env, met_env, virt, self_scope, a->ce); });
      bool ignore_labels = clflags::classic;
      if (!ignore_labels) {
        std::vector<ArgLabel> labels;  // nonopt_labels [] cl.cl_type (reversed)
        for (const ClassType* t = cl->cl_type; t->kind == ClassType::Kind::Cty_arrow; t = t->cty)
          if (!is_optional(t->label)) labels.insert(labels.begin(), t->label);
        bool all_nolabel = true;
        for (auto& s : sargs0)
          if (s.first.kind != ArgLabel::Kind::Nolabel) all_nolabel = false;
        bool any_label = false;
        for (auto& l : labels)
          if (l.kind != ArgLabel::Kind::Nolabel) any_label = true;
        ignore_labels = labels.size() == sargs0.size() && all_nolabel && any_label;
        if (ignore_labels) {
          std::vector<std::string> ls;
          for (auto& l : labels)
            if (l.kind != ArgLabel::Kind::Nolabel) ls.push_back(string_of_label(l));
          location::prerr_warning(cl->cl_loc, warnings::Warning::with_l(warnings::Warning::K::Labels_omitted, ls));
        }
      }
      std::vector<tt::LabeledArg> args;
      std::vector<std::pair<ArgLabel, TypeExpr*>> omitted;  // head first
      const ClassType* ty_fun = cl->cl_type;
      const ClassType* ty_fun0 = ctype::instance_class({}, cl->cl_type).second;
      std::vector<std::pair<ArgLabel, const pt::Expression*>> sargs = sargs0;
      for (;;) {
        if (ty_fun->kind == ClassType::Kind::Cty_arrow && ty_fun0->kind == ClassType::Kind::Cty_arrow &&
            !sargs.empty()) {
          const ArgLabel& l = ty_fun->label;
          TypeExpr* ty = ty_fun->arg;
          TypeExpr* ty0 = ty_fun0->arg;
          std::string_view name = label_name(l);
          bool optional = is_optional(l);
          auto use_arg = [&](const pt::Expression* sarg, const ArgLabel& l2) {
            if (!optional || is_optional(l2)) return tc::type_argument(val_env, sarg, ty, ty0);
            TypeExpr* ty2 = tc::extract_option_type(val_env, ty);
            TypeExpr* ty02 = tc::extract_option_type(val_env, ty0);
            const tt::Expression* arg = tc::type_argument(val_env, sarg, ty2, ty02);
            return tc::option_some(val_env, arg);
          };
          auto eliminate_optional_arg = [&] { return tc::option_none(val_env, ty0, location::none()); };
          std::vector<std::pair<ArgLabel, const pt::Expression*>> remaining_sargs;
          tt::ApplyArg arg{true, nullptr};  // Omitted ()
          if (ignore_labels) {
            auto [l2, sarg] = sargs[0];
            std::vector<std::pair<ArgLabel, const pt::Expression*>> rem(sargs.begin() + 1, sargs.end());
            bool in_rem = false;
            for (auto& x : rem)
              if (name == label_name(x.first)) in_rem = true;
            if (name == label_name(l2) || (!optional && l2.kind == ArgLabel::Kind::Nolabel)) {
              arg = tt::ApplyArg{false, use_arg(sarg, l2)};
              remaining_sargs = rem;
            } else if (optional && !in_rem) {
              arg = tt::ApplyArg{false, eliminate_optional_arg()};
              remaining_sargs = sargs;
            } else {
              Error e = err(sarg->pexp_loc, val_env, EK::Apply_wrong_label);
              e.label = l2;
              raise_error(e);
            }
          } else if (auto x = extract_label(name, sargs)) {
            auto& [l2, sarg, commuted, rem] = *x;
            (void)commuted;
            if (!optional && is_optional(l2))
              location::prerr_warning(sarg->pexp_loc, warnings::Warning::with_s(warnings::Warning::K::Nonoptional_label,
                                                                                string_of_label(l)));
            arg = tt::ApplyArg{false, use_arg(sarg, l2)};
            remaining_sargs = rem;
          } else {
            remaining_sargs = sargs;
            bool has_nolabel = false;
            for (auto& s : sargs)
              if (s.first.kind == ArgLabel::Kind::Nolabel) has_nolabel = true;
            if (is_optional(l) && has_nolabel) arg = tt::ApplyArg{false, eliminate_optional_arg()};
          }
          if (arg.omitted) omitted.insert(omitted.begin(), {l, ty0});
          args.push_back({l, arg});
          ty_fun = ty_fun->cty;
          ty_fun0 = ty_fun0->cty;
          sargs = remaining_sargs;
          continue;
        }
        if (!sargs.empty()) {
          if (!omitted.empty()) {
            Error e = err(sargs[0].second->pexp_loc, val_env, EK::Apply_wrong_label);
            e.label = sargs[0].first;
            raise_error(e);
          }
          Error e = err(cl->cl_loc, val_env, EK::Cannot_apply);
          e.cty = cl->cl_type;
          raise_error(e);
        }
        break;
      }
      const ClassType* cty = ty_fun0;
      for (auto& [l, ty] : omitted) cty = cty_arrow(l, ty, cty);
      return mk_cl(mkd(tt::Tcl_apply{{TK::Tcl_apply}, cl, slice(args)}), scl->pcl_loc, cty, val_env,
                   scl->pcl_attributes);
    }
    case PK::Pcl_let: {
      auto* l = as<pt::Pcl_let>(d);
      tc::TypeBindingResult tb = tc::type_let(tc::ExistentialRestriction::In_class_def, val_env, l->rec, l->vbs);
      env::t val_env2 = tb.env;
      std::vector<tt::IdentExpression> vals;
      std::vector<tt::BoundIdent> bound = tt::let_bound_idents_full(tb.vbs);
      for (std::size_t k = bound.size(); k-- > 0;) {  // List.fold_right
        Ident::t id = bound[k].id;
        Path::t path = Path::pident(id);
        // do not mark the value as used
        const ValueDescription* vd = env::find_value(path, val_env2);
        TypeExpr* ty = ctype::with_local_level_generalize([&] { return ctype::instance(vd->val_type); });
        auto* expr = make<tt::Expression>(
            mkd(tt::Texp_ident{{tt::ExpressionDesc::Kind::Texp_ident}, path,
                               pt::LidLoc{Longident::lident(ident::name(id)), location::none()}, vd}),
            location::none(), Slice<tt::ExpExtraItem>{}, ty, val_env2, tt::Attributes{});
        ValueKind kind{ValueKind::Kind::Val_ivar};
        kind.obj = fresh_identity();
        kind.ivar_mut = MutableFlag::Immutable;
        kind.ivar_name = cl_num;
        auto* desc = make<ValueDescription>(expr->exp_type, kind, vd->val_loc, Attributes{}, vd->val_uid);
        Ident::t id2 = Ident::create_local(ident::name(id));
        met_env = env::add_value(id2, desc, met_env);
        vals.insert(vals.begin(), tt::IdentExpression{id2, expr});
      }
      const tt::ClassExpr* cl = class_expr(cl_num, val_env2, met_env, virt, self_scope, l->body);
      Slice<const tt::ValueBinding*> defs = tb.vbs;
      if (l->rec == RecFlag::Recursive) defs = tc::annotate_recursive_bindings(val_env2, defs);
      return mk_cl(mkd(tt::Tcl_let{{TK::Tcl_let}, l->rec, defs, slice(vals), cl}), scl->pcl_loc, cl->cl_type,
                   val_env2, scl->pcl_attributes);
    }
    case PK::Pcl_constraint: {
      auto* c = as<pt::Pcl_constraint>(d);
      auto [cl, clty] = ctype::with_local_level_for_class([&] {
        long self_scope2 = ctype::get_current_level();
        const tt::ClassExpr* cl1 = typetexp::ty_var_env::with_local_scope([&] {
          const tt::ClassExpr* cl0 = class_expr(cl_num, val_env, met_env, virt, self_scope2, c->ce);
          complete_class_type(cl0->cl_loc, val_env, virt, Kind::Class_type, cl0->cl_type);
          return cl0;
        });
        const tt::ClassType* clty1 = typetexp::ty_var_env::with_local_scope([&] {
          const tt::ClassType* clty0 = class_type(val_env, virt, self_scope2, c->cty);
          complete_class_type(clty0->cltyp_loc, val_env, virt, Kind::Class, clty0->cltyp_type);
          return clty0;
        });
        return std::make_pair(cl1, clty1);
      });
      // ~post
      ctype::limited_generalize_class_type(self_type_row(cl->cl_type), cl->cl_type);
      ctype::limited_generalize_class_type(self_type_row(clty->cltyp_type), clty->cltyp_type);
      std::vector<ctype::ClassMatchFailure> error = includeclass::class_types(val_env, cl->cl_type, clty->cltyp_type);
      if (!error.empty()) {
        Error e = err(cl->cl_loc, val_env, EK::Class_match_failure);
        e.failures = error;
        raise_error(e);
      }
      Constraints cs = extract_constraints(clty->cltyp_type);
      const ClassType* ty = ctype::instance_class({}, clty->cltyp_type).second;
      // Adding a dummy method to the self type prevents it from being closed
      // / escaping.
      ctype::add_dummy_method(val_env, self_scope, signature_of_class_type(ty));
      return mk_cl(mkd(tt::Tcl_constraint{{TK::Tcl_constraint}, cl, clty, cs.vals, cs.meths, cs.concrs}),
                   scl->pcl_loc, ty, val_env, scl->pcl_attributes);
    }
    case PK::Pcl_open: {
      auto* o = as<pt::Pcl_open>(d);
      auto used_slot = std::make_shared<bool>(false);
      auto [od, new_val_env] = type_open_descr(used_slot, val_env, o->od);
      env::t new_met_env = type_open_descr(used_slot, met_env, o->od).second;
      const tt::ClassExpr* cl = class_expr(cl_num, new_val_env, new_met_env, virt, self_scope, o->ce);
      return mk_cl(mkd(tt::Tcl_open{{TK::Tcl_open}, od, cl}), scl->pcl_loc, cl->cl_type, val_env, scl->pcl_attributes);
    }
    case PK::Pcl_extension: throw ErrorForward(as<pt::Pcl_extension>(d)->ext);
  }
  throw std::logic_error("class_expr_aux");
}

const tt::ClassExpr* class_expr(std::string_view cl_num, env::t val_env, env::t met_env,
                                VirtualFlag virt, long self_scope, const pt::ClassExpr* scl) {
  return builtin_attributes::warning_scope(scl->pcl_attributes, [&] {
    return class_expr_aux(cl_num, val_env, met_env, virt, self_scope, scl);
  });
}

// ---- approximations of the constructor types, for recursive uses ------------------------
// a module-initialization value
TypeExpr* var_option() {
  static TypeExpr* t = [] {
    ZoneScope perm(permanent_zone());
    return predef::type_option(newgenvar());
  }();
  return t;
}

TypeExpr* approx_declaration(const pt::ClassExpr* cl) {
  const pt::ClassExprDesc* d = cl->pcl_desc;
  if (auto* f = as<pt::Pcl_fun>(d)) {
    TypeExpr* arg = is_optional(f->label) ? ctype::instance(var_option()) : ctype::newvar();
    arg = ctype::newmono(arg);
    TypeExpr* r = approx_declaration(f->body);
    return ctype::newty(tarrow(f->label, arg, r, commu_ok()));
  }
  if (auto* l = as<pt::Pcl_let>(d)) return approx_declaration(l->body);
  if (auto* c = as<pt::Pcl_constraint>(d)) return approx_declaration(c->ce);
  return ctype::newvar();
}

TypeExpr* approx_description(const pt::ClassType* ct) {
  if (auto* a = as<pt::Pcty_arrow>(ct->pcty_desc)) {
    TypeExpr* arg = is_optional(a->label) ? ctype::instance(var_option()) : ctype::newvar();
    arg = ctype::newmono(arg);
    TypeExpr* r = approx_description(a->cty);
    return ctype::newty(tarrow(a->label, arg, r, commu_ok()));
  }
  return ctype::newvar();
}

// ---- class declarations -----------------------------------------------------------------------
// typeclass.ml's `Type_abstract Definition` literal
const TypeKind* abstract_definition_kind() { return TYPE_ABSTRACT_LIT(Definition); }
Slice<Separability> default_separability(long arity) {
  // Types.Separability.default_signature (Config.flat_float_array)
  return slice(std::vector<Separability>(static_cast<std::size_t>(arity), Separability::Deepsep));
}

struct TempAbbrev {
  Slice<TypeExpr*> params;  // the declaration's type_params list
  TypeExpr* ty;
  const TypeDeclaration* td;
};
TempAbbrev temp_abbrev(const Location& loc, long arity, Uid uid) {
  std::vector<TypeExpr*> params;  // !params: the last created first
  for (long i = 1; i <= arity; ++i) params.insert(params.begin(), ctype::newvar());
  TypeExpr* ty = ctype::newobj(ctype::newvar());
  auto* td = make<TypeDeclaration>(slice(params), arity, abstract_definition_kind(), PrivateFlag::Public, ty,
                                   slice(variance::unknown_signature(false, arity)), default_separability(arity),
                                   false, lowest_level, loc, Attributes{}, TypeImmediacy::Unknown, false, uid);
  return {td->type_params, ty, td};
}

template <class S>
struct ClassEntry {  // (cl, id, ty_id, obj_id, uid)
  const pt::ClassInfos<S>* cl;
  Ident::t id, ty_id, obj_id;
  Uid uid;
};
template <class S>
struct Pending {
  const pt::ClassInfos<S>* cl;
  Ident::t id, ty_id, obj_id;
  Slice<TypeExpr*> obj_params;  // the temporary abbreviations' type_params lists
  TypeExpr* obj_ty;
  Slice<TypeExpr*> cl_params;
  TypeExpr* cl_ty;
  const TypeDeclaration* cl_td;
  TypeExpr* constr_type;
  ClassDeclaration* dummy_class;
};
template <class S, class T>
struct Infos {
  const pt::ClassInfos<S>* cl;
  Ident::t id;
  const ClassDeclaration* clty;
  Ident::t ty_id;
  const ClassTypeDeclaration* cltydef;
  Ident::t obj_id;
  const TypeDeclaration* obj_abbr;
  std::vector<tt::TypeParam> ci_params;
  long arity;
  std::vector<std::string_view> pub_meths;
  std::vector<Location> coe;
  T expr;
};

template <class S>
using Approx = std::function<TypeExpr*(S)>;
template <class S, class T>
using KindFn = std::function<std::pair<T, const ClassType*>(env::t, VirtualFlag, S)>;

template <class S>
env::t initial_env(bool define_class, const Approx<S>& approx, std::vector<Pending<S>>& res, env::t env,
                   const ClassEntry<S>& e) {
  const pt::ClassInfos<S>* cl = e.cl;
  // Temporary abbreviations
  long arity = static_cast<long>(cl->pci_params.size());
  TempAbbrev obj = temp_abbrev(cl->pci_loc, arity, e.uid);
  env = env::add_type(true, e.obj_id, obj.td, env);
  TempAbbrev cla = temp_abbrev(cl->pci_loc, arity, e.uid);
  // Temporary type for the class constructor
  TypeExpr* constr_type =
      ctype::with_local_level_generalize_structure_if_principal([&] { return approx(cl->pci_expr); });
  const ClassType* dummy_cty = cty_signature(ctype::new_class_signature());
  auto* dummy_class = make<ClassDeclaration>(Slice<TypeExpr*>{}, dummy_cty, unbound_class(),
                                             cl->pci_virt == VirtualFlag::Virtual ? nullptr : constr_type,
                                             Slice<variance::t>{}, location::none(), Attributes{}, e.uid);
  if (define_class) env = env::add_class(e.id, dummy_class, env);
  auto* dummy_cltype = make<ClassTypeDeclaration>(Slice<TypeExpr*>{}, dummy_cty, unbound_class(), cla.td,
                                                  Slice<variance::t>{}, location::none(), Attributes{}, e.uid);
  env = env::add_cltype(e.ty_id, dummy_cltype, env);
  res.insert(res.begin(), Pending<S>{cl, e.id, e.ty_id, e.obj_id, obj.params, obj.ty, cla.params, cla.ty, cla.td,
                                     constr_type, dummy_class});
  return env;
}

void unify_params(env::t env, const std::vector<TypeExpr*>& a, const std::vector<TypeExpr*>& b) {
  if (a.size() != b.size()) throw std::invalid_argument("List.iter2");
  for (std::size_t k = 0; k < a.size(); ++k) ctype::unify(env, a[k], b[k]);
}

template <class S, class T>
env::t class_infos_(bool define_class, const KindFn<S, T>& kind, const Pending<S>& p,
                    std::vector<Infos<S, T>>& res, env::t env) {
  const pt::ClassInfos<S>* cl = p.cl;
  struct R {
    std::vector<tt::TypeParam> ci_params;
    std::vector<TypeExpr*> params;
    std::shared_ptr<std::vector<Location>> coercion_locs;
    T expr;
    const ClassType* typ;
    ClassSignature* sign;
  };
  R r = ctype::with_local_level_for_class([&] {
    typetexp::ty_var_env::reset();
    // Introduce class parameters
    std::vector<tt::TypeParam> ci_params;
    for (auto& prm : cl->pci_params) {
      try {
        ci_params.push_back({typetexp::transl_type_param(env, prm.ty), prm.variance, prm.injectivity});
      } catch (const typetexp::AlreadyBound&) {
        raise_error(err(prm.ty->ptyp_loc, env, EK::Repeated_parameter));
      }
    }
    std::vector<TypeExpr*> params;
    for (auto& c : ci_params) params.push_back(c.ty->ctyp_type);
    // Allow self coercions (only for class declarations)
    auto coercion_locs = std::make_shared<std::vector<Location>>();
    // Type the class expression
    std::pair<T, const ClassType*> et;
    try {
      tc::self_coercion.insert(tc::self_coercion.begin(), {Path::pident(p.obj_id), coercion_locs});
      et = kind(env, cl->pci_virt, cl->pci_expr);
      tc::self_coercion.erase(tc::self_coercion.begin());
    } catch (...) {
      tc::self_coercion.clear();
      throw;
    }
    ClassSignature* sign = signature_of_class_type(et.second);
    return R{ci_params, params, coercion_locs, et.first, et.second, sign};
  });
  // ~post: Generalize the row variable
  for (TypeExpr* inside : r.params) ctype::limited_generalize(r.sign->csig_self_row, inside);
  ctype::limited_generalize_class_type(r.sign->csig_self_row, r.typ);
  // one list, as class_infos' `params`: the temporary cltydef / clty and the
  // self object's name (`rv :: params`) share it
  Slice<TypeExpr*> params = slice(r.params);
  const ClassType* typ = r.typ;
  // Check the abbreviation for the object type
  auto [obj_params2, obj_type] = ctype::instance_class(params, typ);
  TypeExpr* constr = ctype::newconstr(Path::pident(p.obj_id), p.obj_params);
  {
    TypeExpr* row = self_type_row(obj_type);
    ctype::unify(env, row, ctype::newty(tnil()));
    try {
      unify_params(env, std::vector<TypeExpr*>(p.obj_params.begin(), p.obj_params.end()), obj_params2);
    } catch (const ctype::Unify&) {
      Error e = err(cl->pci_loc, env, EK::Bad_parameters);
      e.id = p.obj_id;
      e.tys.assign(p.obj_params.begin(), p.obj_params.end());
      e.tys2 = obj_params2;
      raise_error(e);
    }
    TypeExpr* ty = self_type(obj_type);
    try {
      ctype::unify(env, ty, constr);
    } catch (const ctype::Unify&) {
      TypeExpr* exp = ctype::expand_head(env, constr);
      Error e = err(cl->pci_loc, env, EK::Abbrev_type_clash);
      e.ty = constr;
      e.ty2 = ty;
      e.ty3 = exp;
      raise_error(e);
    }
  }
  ctype::set_object_name(p.obj_id, params, self_type(typ));
  // Check the other temporary abbreviation (#-type)
  {
    auto [cl_params2, cl_type] = ctype::instance_class(params, typ);
    TypeExpr* ty = self_type(cl_type);
    try {
      unify_params(env, std::vector<TypeExpr*>(p.cl_params.begin(), p.cl_params.end()), cl_params2);
    } catch (const ctype::Unify&) {
      Error e = err(cl->pci_loc, env, EK::Bad_class_type_parameters);
      e.id = p.ty_id;
      e.tys.assign(p.cl_params.begin(), p.cl_params.end());
      e.tys2 = cl_params2;
      raise_error(e);
    }
    try {
      ctype::unify(env, ty, p.cl_ty);
    } catch (const ctype::Unify&) {
      TypeExpr* ty_expanded = ctype::object_fields(ty);
      Error e = err(cl->pci_loc, env, EK::Abbrev_type_clash);
      e.ty = ty;
      e.ty2 = ty_expanded;
      e.ty3 = p.cl_ty;
      raise_error(e);
    }
  }
  // Type of the class constructor
  try {
    TypeExpr* ic = ctype::instance(p.constr_type);
    TypeExpr* ct = constructor_type(constr, obj_type);
    ctype::unify(env, ct, ic);
  } catch (const ctype::Unify& u) {
    Error e = err(cl->pci_loc, env, EK::Constructor_type_mismatch);
    e.name = std::string(cl->pci_name.txt);
    e.trace = u.err;
    raise_error(e);
  }
  // Class and class type temporary definitions
  Slice<variance::t> cty_variance = slice(variance::unknown_signature(false, static_cast<long>(params.size())));
  // each `Path.Pident obj_id` of typeclass.ml is its own block
  TypeExpr* cty_new = cl->pci_virt == VirtualFlag::Virtual ? nullptr : p.constr_type;
  auto* cltydef0 = make<ClassTypeDeclaration>(params, class_body(typ), Path::pident(p.obj_id), p.cl_td, cty_variance,
                                              cl->pci_loc, pt::types_attributes(cl->pci_attributes),
                                              p.dummy_class->cty_uid);
  auto* clty0 = make<ClassDeclaration>(params, typ, Path::pident(p.obj_id), cty_new, cty_variance, cl->pci_loc,
                                       pt::types_attributes(cl->pci_attributes), p.dummy_class->cty_uid);
  p.dummy_class->cty_type = typ;
  if (define_class) env = env::add_class(p.id, clty0, env);
  env = env::add_cltype(p.ty_id, cltydef0, env);
  // Misc.
  long arity = class_type_arity(typ);
  std::vector<std::string_view> pub_meths = public_methods(r.sign);
  // Final definitions
  auto [params2_v, typ2] = ctype::instance_class(params, typ);
  Slice<TypeExpr*> params2 = slice(params2_v);  // params': clty's and cltydef's one list
  TypeExpr* cty_new2 = cl->pci_virt == VirtualFlag::Virtual ? nullptr : ctype::instance(p.constr_type);
  auto* clty = make<ClassDeclaration>(params2, typ2, Path::pident(p.obj_id), cty_new2, cty_variance, cl->pci_loc,
                                      pt::types_attributes(cl->pci_attributes), p.dummy_class->cty_uid);
  long oarity = static_cast<long>(p.obj_params.size());
  auto* obj_abbr = make<TypeDeclaration>(p.obj_params, oarity, abstract_definition_kind(), PrivateFlag::Public,
                                         p.obj_ty, slice(variance::unknown_signature(false, oarity)),
                                         default_separability(oarity), false, lowest_level, cl->pci_loc, Attributes{},
                                         TypeImmediacy::Unknown, false, p.dummy_class->cty_uid);
  auto [cl_params, cl_ty] = ctype::instance_parameterized_type(params, self_type(typ));
  // one list: the object name's `rv :: cl_params` shares it with cl_abbr
  Slice<TypeExpr*> cl_params_l = slice(cl_params);
  ctype::set_object_name(p.obj_id, cl_params_l, cl_ty);
  auto* cl_abbr = make<TypeDeclaration>(*p.cl_td);
  cl_abbr->type_params = cl_params_l;
  cl_abbr->type_manifest = cl_ty;
  cl_abbr->manifest_obj.reset();  // a new Some block
  auto* cltydef = make<ClassTypeDeclaration>(params2, class_body(typ2), Path::pident(p.obj_id), cl_abbr, cty_variance,
                                             cl->pci_loc, pt::types_attributes(cl->pci_attributes),
                                             p.dummy_class->cty_uid);
  // List.rev !coercion_locs (the head of the OCaml list is the vector's front)
  std::vector<Location> coe(r.coercion_locs->rbegin(), r.coercion_locs->rend());
  res.insert(res.begin(), Infos<S, T>{cl, p.id, clty, p.ty_id, cltydef, p.obj_id, obj_abbr, r.ci_params, arity,
                                      pub_meths, coe, r.expr});
  return env;
}

template <class S, class T>
env::t class_infos(bool define_class, const KindFn<S, T>& kind, const Pending<S>& p, std::vector<Infos<S, T>>& res,
                   env::t env) {
  return builtin_attributes::warning_scope(p.cl->pci_attributes,
                                           [&] { return class_infos_(define_class, kind, p, res, env); });
}

template <class S, class T>
void collapse_conj_class_params(env::t env, const Infos<S, T>& i) {
  try {
    ctype::collapse_conj_params(env, i.clty->cty_params);
  } catch (const ctype::Unify& u) {
    Error e = err(i.cl->pci_loc, env, EK::Non_collapsable_conjunction);
    e.id = i.id;
    e.clty = i.clty;
    e.trace = u.err;
    raise_error(e);
  }
}

template <class S, class T>
FullClass<T> final_decl(env::t env, bool define_class, const Infos<S, T>& i) {
  if (std::optional<TypeSet> vars = ctype::nongen_vars_in_class_declaration(i.clty)) {
    Error e = err(i.cl->pci_loc, env, EK::Non_generalizable_class);
    e.id = i.id;
    e.clty = i.clty;
    e.tys = vars->elements();
    raise_error(e);
  }
  if (auto reason = ctype::closed_class(i.clty->cty_params, signature_of_class_type(i.clty->cty_type))) {
    Error e = err(i.cl->pci_loc, env, EK::Unbound_type_var);
    // the declaration, printed when the error is raised
    if (define_class)
      e.decl_doc = format_doc::doc_printf(
          "%a", [&](format_doc::Formatter& f) { printtyp::class_declaration(i.id, f, i.clty); });
    else
      e.decl_doc = format_doc::doc_printf(
          "%a", [&](format_doc::Formatter& f) { printtyp::cltype_declaration(i.id, f, i.cltydef); });
    e.closed_failure = reason;
    e.clty = i.clty;
    raise_error(e);
  }
  auto* req = make<tt::ClassInfos<T>>();
  req->ci_loc = i.cl->pci_loc;
  req->ci_virt = i.cl->pci_virt;
  req->ci_params = slice(i.ci_params);
  // TODO : check that we have the correct use of identifiers (typeclass.ml)
  req->ci_id_name = i.cl->pci_name;
  req->ci_id_class = i.id;
  req->ci_id_class_type = i.ty_id;
  req->ci_id_object = i.obj_id;
  req->ci_expr = i.expr;
  req->ci_decl = i.clty;
  req->ci_type_decl = i.cltydef;
  req->ci_attributes = i.cl->pci_attributes;
  return FullClass<T>{i.id, i.cl->pci_name, i.clty, i.ty_id, i.cltydef, i.obj_id, i.obj_abbr, i.arity,
                      i.pub_meths, i.coe, req};
}

template <class T>
env::t final_env(bool define_class, env::t env, const FullClass<T>& c) {
  // Add definitions after cleaning them: the arguments are evaluated right
  // to left
  if (define_class) {
    const ClassDeclaration* cd = subst::class_declaration(subst::identity(), c.clty);
    env = env::add_class(c.id, cd, env);
  }
  const ClassTypeDeclaration* ctd = subst::cltype_declaration(subst::identity(), c.cltydef);
  env = env::add_cltype(c.ty_id, ctd, env);
  const TypeDeclaration* td = subst::type_declaration(subst::identity(), c.obj_abbr);
  return env::add_type(true, c.obj_id, td, env);
}

// Check that #c is coercible to c if there is a self-coercion
template <class T>
ClassInfo<const tt::ClassInfos<T>*> check_coercions(env::t env, const FullClass<T>& c) {
  const TypeDeclaration* cl_abbr = c.cltydef->clty_hash_type;
  if (!c.coe.empty()) {
    const Location& loc = c.coe[0];
    if (!cl_abbr->type_manifest || !c.obj_abbr->type_manifest) throw std::logic_error("check_coercions");
    auto [cl_params, cl_ty] = ctype::instance_parameterized_type(cl_abbr->type_params, cl_abbr->type_manifest);
    auto [obj_params, obj_ty] =
        ctype::instance_parameterized_type(c.obj_abbr->type_params, c.obj_abbr->type_manifest);
    unify_params(env, cl_params, obj_params);
    try {
      ctype::subtype(env, cl_ty, obj_ty)();
    } catch (const ctype::Subtype& s) {
      tc::Error e(loc, env, tc::Error::Kind::Not_subtype);
      e.subtype = s.err;
      typing_recovery::log_and_raise(e);
    }
    if (!ctype::opened_object(cl_ty)) {
      Error e = err(loc, env, EK::Cannot_coerce_self);
      e.ty = obj_ty;
      raise_error(e);
    }
  }
  return ClassInfo<const tt::ClassInfos<T>*>{c.id,        c.id_loc,    c.clty,  c.ty_id,     c.cltydef, c.obj_id,
                                             c.obj_abbr, cl_abbr,     c.arity, c.pub_meths, c.req};
}

template <class S, class T>
std::pair<std::vector<ClassInfo<const tt::ClassInfos<T>*>>, env::t> type_classes(
    bool define_class, const Approx<S>& approx, const KindFn<S, T>& kind, env::t env,
    Slice<const pt::ClassInfos<S>*> cls0) {
  long scope = ctype::create_scope();
  std::vector<ClassEntry<S>> cls;
  for (auto* cl : cls0) {
    // (cl, create_scoped .., create_scoped .., create_scoped .., Uid.mk ..):
    // right to left
    Uid uid = uid::mk(env::get_current_unit());
    Ident::t obj_id = Ident::create_scoped(static_cast<int>(scope), cl->pci_name.txt);
    Ident::t ty_id = Ident::create_scoped(static_cast<int>(scope), cl->pci_name.txt);
    Ident::t id = Ident::create_scoped(static_cast<int>(scope), cl->pci_name.txt);
    cls.push_back({cl, id, ty_id, obj_id, uid});
  }
  auto [infos, env2] = ctype::with_local_level_generalize_for_class([&] {
    std::vector<Pending<S>> pend;  // fold_left accumulation: the last class first
    env::t e = env;
    for (auto& c : cls) e = initial_env(define_class, approx, pend, e, c);
    std::vector<Infos<S, T>> res;  // fold_right over pend (from its end): the last class first again
    for (std::size_t k = pend.size(); k-- > 0;) e = class_infos(define_class, kind, pend[k], res, e);
    for (auto& i : res) collapse_conj_class_params(e, i);
    return std::make_pair(res, e);
  });
  env = env2;
  // List.rev_map (final_decl env define_class) res: applied in list order
  std::vector<FullClass<T>> res;
  for (auto& i : infos) res.insert(res.begin(), final_decl(env, define_class, i));
  std::vector<typedecl_variance::ClassDeclInput> decls;
  for (auto& c : res)
    decls.push_back({c.obj_id, c.obj_abbr, c.clty, c.cltydef, c.req->ci_params, c.req->ci_loc});
  std::vector<typedecl_variance::ClassDeclOutput> decls2;
  try {
    decls2 = typedecl_variance::update_class_decls(env, decls);
  } catch (const typedecl_variance::Error& ve) {
    typedecl::Error e(ve.loc, typedecl::Error::Kind::Variance);
    e.variance = ve;
    typing_recovery::log_and_raise(e);
  }
  if (decls2.size() != res.size()) throw std::invalid_argument("List.map2");
  for (std::size_t k = 0; k < res.size(); ++k) {
    res[k].obj_abbr = decls2[k].decl;
    res[k].clty = decls2[k].cl;
    res[k].cltydef = decls2[k].cltype;
  }
  for (auto& c : res) env = final_env(define_class, env, c);
  std::vector<ClassInfo<const tt::ClassInfos<T>*>> out;
  for (auto& c : res) out.push_back(check_coercions(env, c));
  return {out, env};
}

long class_num = 0;

std::pair<const tt::ClassExpr*, const ClassType*> class_declaration(env::t env, VirtualFlag virt,
                                                                    const pt::ClassExpr* sexpr) {
  ++class_num;
  long self_scope = ctype::get_current_level();
  const tt::ClassExpr* expr = class_expr(zborrow(std::to_string(class_num)), env, env, virt, self_scope, sexpr);
  complete_class_type(expr->cl_loc, env, virt, Kind::Class, expr->cl_type);
  return {expr, expr->cl_type};
}

std::pair<const tt::ClassType*, const ClassType*> class_description(env::t env, VirtualFlag virt,
                                                                    const pt::ClassType* sexpr) {
  long self_scope = ctype::get_current_level();
  const tt::ClassType* expr = class_type(env, virt, self_scope, sexpr);
  complete_class_type(expr->cltyp_loc, env, virt, Kind::Class_type, expr->cltyp_type);
  return {expr, expr->cltyp_type};
}

std::pair<const tt::ClassStructure*, std::vector<std::string_view>> type_object(env::t env, const Location& loc,
                                                                                const pt::ClassStructure* s) {
  ++class_num;
  const tt::ClassStructure* desc = class_structure(zborrow(std::to_string(class_num)), VirtualFlag::Concrete,
                                                   lowest_level, Final::Final, env, env, loc, s);
  complete_class_signature(loc, env, VirtualFlag::Concrete, Kind::Object,
                           const_cast<ClassSignature*>(desc->cstr_type));
  std::vector<std::string_view> meths = public_methods(desc->cstr_type);
  return {desc, meths};
}

// ---- recursive modules ----------------------------------------------------------------------
// Check that there is no references through recursive modules (GPR#6491)
void check_recmod_class_sig(env::t env, const pt::StrLoc& name, const pt::ClassSignature* csig);
void check_recmod_class_type(env::t env, const pt::StrLoc& name, const pt::ClassType* cty) {
  const pt::ClassTypeDesc* d = cty->pcty_desc;
  switch (d->kind) {
    case pt::ClassTypeDesc::Kind::Pcty_constr: {
      auto* c = as<pt::Pcty_constr>(d);
      try {
        env::lookup_cltype(false, c->lid.loc, c->lid.txt, env);
      } catch (const env::Error& e) {
        if (!(e.kind == env::Error::Kind::Lookup_error &&
              e.err.kind == env::LookupError::Kind::Illegal_reference_to_recursive_module))
          throw;
        env::Error e2(env::Error::Kind::Lookup_error);
        e2.loc = e.loc;
        e2.env = e.env;
        e2.err = env::LookupError{env::LookupError::Kind::Illegal_reference_to_recursive_class_type};
        e2.err.container = e.err.container;
        e2.err.unbound = e.err.unbound;
        e2.err.unbound_class_type = c->lid.txt;
        e2.err.container_class_type = name.txt;
        throw e2;
      }
      return;
    }
    case pt::ClassTypeDesc::Kind::Pcty_extension: return;
    case pt::ClassTypeDesc::Kind::Pcty_arrow: check_recmod_class_type(env, name, as<pt::Pcty_arrow>(d)->cty); return;
    case pt::ClassTypeDesc::Kind::Pcty_open: {
      auto* o = as<pt::Pcty_open>(d);
      env::t env2 = type_open_descr(nullptr, env, o->od).second;
      check_recmod_class_type(env2, name, o->cty);
      return;
    }
    case pt::ClassTypeDesc::Kind::Pcty_signature: check_recmod_class_sig(env, name, as<pt::Pcty_signature>(d)->sign); return;
  }
}
void check_recmod_class_sig(env::t env, const pt::StrLoc& name, const pt::ClassSignature* csig) {
  for (auto* ctf : csig->pcsig_fields)
    if (auto* i = as<pt::Pctf_inherit>(ctf->pctf_desc)) check_recmod_class_type(env, name, i->cty);
}

// Approximate the class declaration as class ['params] id = object end
const pt::ClassDescription* approx_class(const pt::ClassDescription* sdecl) {
  const pt::CoreType* self2 = ah::typ_mk(mkd(pt::Ptyp_any{{pt::CoreTypeDesc::Kind::Ptyp_any}}), location::none());
  auto* csig = make<pt::ClassSignature>(self2, Slice<const pt::ClassTypeField*>{});
  auto* clty2 = make<pt::ClassType>(mkd(pt::Pcty_signature{{pt::ClassTypeDesc::Kind::Pcty_signature}, csig}),
                                    sdecl->pci_expr->pcty_loc, pt::Attributes{});
  auto* d = make<pt::ClassDescription>(*sdecl);
  d->pci_expr = clty2;
  return d;
}

}  // namespace

// the module-initialization values of typeclass.ml, created at startup (see
// typemod::install_forward_refs); var_option's type ids are module-init ids,
// allocated before the first Types reset
void module_init() {
  (void)unbound_class();
  (void)var_option();
}

std::pair<std::vector<ClassInfo<const tt::TClassDeclaration*>>, env::t> class_declarations(
    env::t env, Slice<const pt::ClassDeclaration*> cls) {
  auto [info, env2] = type_classes<const pt::ClassExpr*, const tt::ClassExpr*>(
      true, approx_declaration, class_declaration, env, cls);
  std::vector<Ident::t> ids;
  std::vector<const tt::ClassExpr*> exprs;
  for (auto& ci : info) {
    ids.push_back(ci.cls_id);
    exprs.push_back(ci.cls_info->ci_expr);
  }
  tc::check_recursive_class_bindings(env2, ids, exprs);
  return {info, env2};
}

std::pair<std::vector<ClassInfo<const tt::TClassDescription*>>, env::t> class_descriptions(
    env::t env, Slice<const pt::ClassDescription*> cls) {
  return type_classes<const pt::ClassType*, const tt::ClassType*>(true, approx_description, class_description, env,
                                                                  cls);
}

std::pair<std::vector<ClassTypeInfo>, env::t> class_type_declarations(env::t env,
                                                                      Slice<const pt::ClassDescription*> cls) {
  auto [decls, env2] = type_classes<const pt::ClassType*, const tt::ClassType*>(false, approx_description,
                                                                                class_description, env, cls);
  std::vector<ClassTypeInfo> out;
  for (auto& d : decls)
    out.push_back({d.cls_ty_id, d.cls_id_loc, d.cls_ty_decl, d.cls_obj_id, d.cls_obj_abbr, d.cls_abbr, d.cls_info});
  return {out, env2};
}

std::pair<std::vector<ClassTypeInfo>, env::t> approx_class_declarations(env::t env,
                                                                        Slice<const pt::ClassDescription*> sdecls) {
  std::vector<const pt::ClassDescription*> approx;
  for (auto* s : sdecls) approx.push_back(approx_class(s));
  auto r = class_type_declarations(env, slice(approx));
  for (auto* s : sdecls) check_recmod_class_type(r.second, s->pci_name, s->pci_expr);
  return r;
}

void install_forward_refs() { tc::type_object = type_object; }
}  // namespace cppcaml::typing::typeclass
