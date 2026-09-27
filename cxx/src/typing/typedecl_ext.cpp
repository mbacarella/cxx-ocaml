// Port of typing/typedecl.ml, part 2: type extensions and exceptions, value
// and primitive descriptions (native representations), `with type`
// constraints, package constraints and approximate declarations.
#include "typedecl_internal.hpp"

namespace cppcaml::typing::typedecl {

using namespace types;
using pt::as;

// ---- translating type extensions -------------------------------------------------------------
static const tt::TExtensionConstructor* transl_extension_constructor(long scope, env::t env, Path::t type_path,
                                                                     Slice<TypeExpr*> type_params,
                                                                     Slice<TypeExpr*> typext_params,
                                                                     PrivateFlag priv,
                                                                     const pt::ExtensionConstructor* sext) {
  return builtin_attributes::warning_scope(sext->pext_attributes, [&]() -> const tt::TExtensionConstructor* {
    Ident::t id = Ident::create_scoped(static_cast<int>(scope), sext->pext_name.txt);
    ConstructorArguments args;
    TypeExpr* ret_type = nullptr;
    tt::TExtensionConstructorKind kind{};
    const pt::ExtensionConstructorKind& sk = sext->pext_kind;
    if (sk.kind == pt::ExtensionConstructorKind::Kind::Pext_decl) {
      MadeConstructor mc = make_constructor(env, sext->pext_loc, type_path,
                                            std::vector<TypeExpr*>(typext_params.begin(), typext_params.end()), sk.vars, sk.args, sk.res);
      args = mc.args;
      ret_type = mc.ret_type;
      kind.kind = tt::TExtensionConstructorKind::Kind::Text_decl;
      kind.vars = sk.vars;
      kind.args = mc.targs;
      kind.res = mc.tret_type;
    } else {
      const pt::LidLoc& lid = sk.rebind;
      env::ConstructorUsage usage =
          priv == PrivateFlag::Public ? env::ConstructorUsage::Exported : env::ConstructorUsage::Exported_private;
      const ConstructorDescription* cdescr = env::lookup_constructor(true, lid.loc, usage, lid.txt, env);
      ctype::InstancedConstructor ic = ctype::instance_constructor(ctype::ExistentialTreatment{}, cdescr);
      TypeExpr* res;
      if (cdescr->cstr_generalized) {
        std::vector<TypeExpr*> tp(type_params.begin(), type_params.end());
        std::vector<TypeExpr*> params = ctype::instance_list(tp);
        res = ctype::newconstr(type_path, slice(params));
        ret_type = ctype::newconstr(type_path, slice(params));
      } else {
        res = ctype::newconstr(type_path, typext_params);
      }
      try {
        ctype::unify(env, ic.res, res);
      } catch (const ctype::Unify& u) {
        Error e(lid.loc, EK::Rebind_wrong_type);
        e.lid = lid.txt;
        e.env = env;
        e.trace = u.err;
        raise_error(e);
      }
      // Remove "_" names from parameters used in the constructor
      if (!cdescr->cstr_generalized) {
        std::vector<TypeExpr*> vars = ctype::free_variables_list(ic.args);
        for (TypeExpr* ty : typext_params) {
          auto* v = as<Tvar>(get_desc(ty));
          if (!(v && v->name.some && v->name.v == "_")) continue;
          bool used = false;
          for (TypeExpr* x : vars) used = used || eq_type(ty, x);
          if (used) set_type_desc(ty, TVAR_NONE_LIT());
        }
      }
      // Ensure that constructor's type matches the type being extended
      Path::t cstr_res_type_path = data_types::cstr_res_type_path(cdescr);
      Slice<TypeExpr*> cstr_res_type_params = env::find_type(cstr_res_type_path, env)->type_params;
      std::vector<TypeExpr*> cstr_types{
          btype::newgenty(tconstr(cstr_res_type_path, cstr_res_type_params, make<MemoRef>(mnil())))};
      cstr_types.insert(cstr_types.end(), cstr_res_type_params.begin(), cstr_res_type_params.end());
      std::vector<TypeExpr*> ext_types{btype::newgenty(tconstr(type_path, type_params, make<MemoRef>(mnil())))};
      ext_types.insert(ext_types.end(), type_params.begin(), type_params.end());
      if (!ctype::is_equal(env, true, slice(cstr_types), slice(ext_types))) {
        Error e(lid.loc, EK::Rebind_mismatch);
        e.lid = lid.txt;
        e.path = cstr_res_type_path;
        e.path2 = type_path;
        raise_error(e);
      }
      // Disallow rebinding private constructors to non-private
      if (cdescr->cstr_private == PrivateFlag::Private && priv == PrivateFlag::Public) {
        Error e(lid.loc, EK::Rebind_private);
        e.lid = lid.txt;
        raise_error(e);
      }
      if (cdescr->cstr_tag.kind != ConstructorTag::Kind::Cstr_extension)
        throw std::logic_error("transl_extension_constructor: rebind");
      Path::t path = cdescr->cstr_tag.ext_path;
      if (!cdescr->cstr_inlined) {
        args = ConstructorArguments{ConstructorArguments::Kind::Cstr_tuple, slice(ic.args)};
      } else {
        if (ic.args.size() != 1) throw std::logic_error("transl_extension_constructor: inlined");
        auto* tc = as<Tconstr>(get_desc(ic.args[0]));
        if (!tc) throw std::logic_error("transl_extension_constructor: inlined");
        const TypeDeclaration* decl = ctype::instance_declaration(cdescr->cstr_inlined);
        if (decl->type_params.size() != tc->args.size()) throw std::logic_error("transl_extension_constructor");
        for (std::size_t k = 0; k < tc->args.size(); ++k) ctype::unify(env, decl->type_params[k], tc->args[k]);
        if (!(decl->type_kind->kind == TypeKind::Kind::Type_record &&
              decl->type_kind->record_repr.kind == RecordRepresentation::Kind::Record_extension))
          throw std::logic_error("transl_extension_constructor: record");
        args = ConstructorArguments{ConstructorArguments::Kind::Cstr_record, {}, decl->type_kind->labels};
      }
      kind.kind = tt::TExtensionConstructorKind::Kind::Text_rebind;
      kind.path = path;
      kind.lid = lid;
    }
    auto* ext = make<ExtensionConstructor>(type_path, typext_params, args, ret_type, priv, sext->pext_loc,
                                           parsetree::types_attributes(sext->pext_attributes),
                                           uid::mk(env::get_current_unit()));
    // (the shape is cmt-only)
    return make<tt::TExtensionConstructor>(id, sext->pext_name, ext, kind, sext->pext_loc, sext->pext_attributes);
  });
}

static bool is_rebind(const tt::TExtensionConstructor* ext) {
  return ext->ext_kind.kind == tt::TExtensionConstructorKind::Kind::Text_rebind;
}

std::pair<const tt::TTypeExtension*, env::t> transl_type_extension(bool extend, env::t env, const Location& loc,
                                                                  const pt::TypeExtension* styext) {
  return builtin_attributes::warning_scope(styext->ptyext_attributes, [&] {
    const pt::LidLoc& lid = styext->ptyext_path;
    auto [type_path, type_decl] = env::lookup_type(true, lid.loc, lid.txt, env);
    if (type_decl->type_kind->kind == TypeKind::Kind::Type_open) {
      if (type_decl->type_private == PrivateFlag::Private && extend) {
        for (auto* c : styext->ptyext_constructors)
          if (c->pext_kind.kind == pt::ExtensionConstructorKind::Kind::Pext_decl) {
            Error e(c->pext_loc, EK::Cannot_extend_private_type);
            e.path = type_path;
            raise_error(e);
          }
      }
    } else {
      Error e(loc, EK::Not_extensible_type);
      e.path = type_path;
      raise_error(e);
    }
    typedecl_variance::Req type_variance;
    for (variance::t v : type_decl->type_variance) {
      bool co = variance::mem(variance::F::May_pos, v), cn = variance::mem(variance::F::May_neg, v);
      type_variance.push_back({!cn, !co, false});
    }
    std::optional<includecore::TypeMismatch> err;
    if (type_decl->type_arity != static_cast<long>(styext->ptyext_params.size())) {
      err = includecore::TypeMismatch{includecore::TypeMismatch::Kind::Arity};
    } else {
      auto req = typedecl_variance::variance_of_params(styext->ptyext_params);
      if (req.size() != type_variance.size()) throw std::invalid_argument("List.for_all2");
      bool ok = true;
      for (std::size_t k = 0; k < req.size() && ok; ++k)
        ok = (!req[k].co || type_variance[k].co) && (!req[k].cn || type_variance[k].cn);
      if (!ok) err = includecore::TypeMismatch{includecore::TypeMismatch::Kind::Variance};
    }
    if (err) {
      Error e(loc, EK::Extension_mismatch);
      e.path = type_path;
      e.env = env;
      e.mismatch = err;
      raise_error(e);
    }
    struct R {
      std::vector<tt::TypeParam> ttype_params;
      std::vector<const tt::TExtensionConstructor*> constructors;
    };
    // Note: it would be incorrect to call [create_scope] *after*
    // [TyVarEnv.reset] or after [with_local_level] (see #10010).
    long scope = ctype::create_scope();
    R r = ctype::with_local_level_generalize([&] {
      typetexp::ty_var_env::reset();
      std::vector<tt::TypeParam> ttype_params = make_params(env, styext->ptyext_params);
      std::vector<TypeExpr*> type_params;
      for (auto& p : ttype_params) type_params.push_back(p.ty->ctyp_type);
      std::vector<TypeExpr*> dp(type_decl->type_params.begin(), type_decl->type_params.end());
      std::vector<TypeExpr*> inst = ctype::instance_list(dp);
      if (inst.size() != type_params.size()) throw std::invalid_argument("List.iter2");
      for (std::size_t k = 0; k < inst.size(); ++k) ctype::unify_var(env, inst[k], type_params[k]);
      std::vector<const tt::TExtensionConstructor*> constructors;
      // one list: every constructor's ext_type_params is typext_params
      Slice<TypeExpr*> typext_params = slice(type_params);
      for (auto* c : styext->ptyext_constructors)
        constructors.push_back(transl_extension_constructor(scope, env, type_path, type_decl->type_params, typext_params,
                                                            styext->ptyext_private, c));
      return R{ttype_params, constructors};
    });
    // Check variances are correct ([loc] is the location of the extension)
    for (auto* ext : r.constructors) {
      try {
        typedecl_variance::check_variance_extension(env, type_decl, ext, type_variance, loc);
      } catch (const typedecl_variance::Error& v) {
        Error e(v.loc, EK::Variance);
        e.variance = v;
        raise_error(e);
      }
    }
    // Add extension constructors to the environment
    env::t newenv = env;
    for (auto* ext : r.constructors) newenv = env::add_extension(true, is_rebind(ext), ext->ext_id, ext->ext_type, newenv);
    auto* tyext = make<tt::TTypeExtension>(type_path, styext->ptyext_path, slice(r.ttype_params), slice(r.constructors),
                                           styext->ptyext_private, styext->ptyext_loc, styext->ptyext_attributes);
    return std::make_pair(static_cast<const tt::TTypeExtension*>(tyext), newenv);
  });
}

std::pair<const tt::TExtensionConstructor*, env::t> transl_exception(env::t env, const pt::ExtensionConstructor* sext) {
  long scope = ctype::create_scope();
  const tt::TExtensionConstructor* ext = ctype::with_local_level_generalize([&] {
    typetexp::ty_var_env::reset();
    return transl_extension_constructor(scope, env, predef::paths().exn, {}, {}, PrivateFlag::Public, sext);
  });
  env::t newenv = env::add_extension(true, is_rebind(ext), ext->ext_id, ext->ext_type, env);
  return {ext, newenv};
}

std::pair<const tt::TTypeException*, env::t> transl_type_exception(env::t env, const pt::TypeException* t) {
  auto [c, newenv] = builtin_attributes::warning_scope(t->ptyexn_attributes,
                                                       [&] { return transl_exception(env, t->ptyexn_constructor); });
  return {make<tt::TTypeException>(c, t->ptyexn_loc, t->ptyexn_attributes), newenv};
}

// ---- native representations of external declarations ---------------------------------------
namespace {
struct NativeReprAttribute {  // Native_repr_attr_absent | Native_repr_attr_present of native_repr_kind
  bool present;
  NativeReprKind kind = NativeReprKind::Unboxed;
};

NativeReprAttribute get_native_repr_attribute(const pt::Attributes& attrs, std::optional<NativeReprKind> global_repr) {
  // (get_no_payload_attribute "unboxed", get_no_payload_attribute "untagged", global_repr): right to left
  std::optional<pt::StrLoc> untagged = attr_helper::get_no_payload_attribute("untagged", attrs);
  std::optional<pt::StrLoc> unboxed = attr_helper::get_no_payload_attribute("unboxed", attrs);
  if (!unboxed && !untagged) {
    if (!global_repr) return {false};
    return {true, *global_repr};
  }
  if (unboxed && !untagged && !global_repr) return {true, NativeReprKind::Unboxed};
  if (!unboxed && untagged && !global_repr) return {true, NativeReprKind::Untagged};
  raise_error(Error(unboxed ? unboxed->loc : untagged->loc, EK::Multiple_native_repr_attributes));
}

std::optional<NativeRepr> native_repr_of_type(env::t env, NativeReprKind kind, TypeExpr* ty) {
  auto* tc = as<Tconstr>(get_desc(ctype::expand_head_opt(env, ty)));
  if (!tc) return std::nullopt;
  if (kind == NativeReprKind::Untagged) {
    if (typeopt::maybe_pointer_type(env, ty) == typeopt::ImmediateOrPointer::Immediate)
      return NativeRepr{NativeRepr::Kind::Untagged_immediate};
    return std::nullopt;
  }
  const predef::Paths& p = predef::paths();
  if (path::same(tc->path, p.float_)) return NativeRepr{NativeRepr::Kind::Unboxed_float};
  if (path::same(tc->path, p.int32)) { static const char k = 0; return NativeRepr{NativeRepr::Kind::Unboxed_integer, BoxedInteger::Pint32, &k}; }
  if (path::same(tc->path, p.int64)) { static const char k = 0; return NativeRepr{NativeRepr::Kind::Unboxed_integer, BoxedInteger::Pint64, &k}; }
  if (path::same(tc->path, p.nativeint))
    { static const char k = 0; return NativeRepr{NativeRepr::Kind::Unboxed_integer, BoxedInteger::Pnativeint, &k}; }
  return std::nullopt;
}

// Raises an error when [core_type] contains an [@unboxed] or [@untagged]
// attribute in a strict sub-term (Ast_iterator's default traversal order).
void error_if_has_deep_native_repr_attributes(const pt::CoreType* core_type) {
  std::function<void(const pt::CoreType*)> typ;
  std::function<void(const pt::CoreType*)> children = [&](const pt::CoreType* t) {
    // sub.attributes: the payload types of the attributes
    for (auto* a : t->ptyp_attributes)
      if (a->attr_payload.kind == pt::Payload::Kind::PTyp && a->attr_payload.typ) typ(a->attr_payload.typ);
    using K = pt::CoreTypeDesc::Kind;
    const pt::CoreTypeDesc* d = t->ptyp_desc;
    auto package = [&](const pt::PackageType* p) {
      for (auto& c : p->ppt_constraints) typ(c.second);
    };
    switch (d->kind) {
      case K::Ptyp_arrow: typ(as<pt::Ptyp_arrow>(d)->t1); typ(as<pt::Ptyp_arrow>(d)->t2); break;
      case K::Ptyp_tuple:
        for (auto& x : as<pt::Ptyp_tuple>(d)->tl) typ(x.ty);
        break;
      case K::Ptyp_constr:
        for (auto* x : as<pt::Ptyp_constr>(d)->args) typ(x);
        break;
      case K::Ptyp_object:
        for (auto* f : as<pt::Ptyp_object>(d)->fields) {
          if (auto* o = as<pt::Otag>(f->pof_desc)) typ(o->ty);
          else typ(as<pt::Oinherit>(f->pof_desc)->ty);
        }
        break;
      case K::Ptyp_class:
        for (auto* x : as<pt::Ptyp_class>(d)->args) typ(x);
        break;
      case K::Ptyp_alias: typ(as<pt::Ptyp_alias>(d)->ty); break;
      case K::Ptyp_variant:
        for (auto* f : as<pt::Ptyp_variant>(d)->fields) {
          if (auto* r = as<pt::Rtag>(f->prf_desc)) {
            for (auto* x : r->types) typ(x);
          } else {
            typ(as<pt::Rinherit>(f->prf_desc)->ty);
          }
        }
        break;
      case K::Ptyp_poly: typ(as<pt::Ptyp_poly>(d)->ty); break;
      case K::Ptyp_package: package(as<pt::Ptyp_package>(d)->pack); break;
      case K::Ptyp_open: typ(as<pt::Ptyp_open>(d)->ty); break;
      case K::Ptyp_functor:
        package(as<pt::Ptyp_functor>(d)->pack);
        typ(as<pt::Ptyp_functor>(d)->ty);
        break;
      default: break;
    }
  };
  typ = [&](const pt::CoreType* t) {
    NativeReprAttribute a = get_native_repr_attribute(t->ptyp_attributes, std::nullopt);
    if (a.present) {
      Error e(t->ptyp_loc, EK::Deep_unbox_or_untag_attribute);
      e.repr = a.kind;
      raise_error(e);
    }
    children(t);
  };
  children(core_type);
}

NativeRepr make_native_repr(env::t env, const pt::CoreType* core_type, TypeExpr* ty,
                            std::optional<NativeReprKind> global_repr) {
  error_if_has_deep_native_repr_attributes(core_type);
  NativeReprAttribute a = get_native_repr_attribute(core_type->ptyp_attributes, global_repr);
  if (!a.present) return NativeRepr{};
  std::optional<NativeRepr> r = native_repr_of_type(env, a.kind, ty);
  if (!r) {
    Error e(core_type->ptyp_loc, EK::Cannot_unbox_or_untag_type);
    e.repr = a.kind;
    raise_error(e);
  }
  return *r;
}

std::pair<std::vector<NativeRepr>, NativeRepr> parse_native_repr_attributes(
    env::t env, const pt::CoreType* core_type, TypeExpr* ty, std::optional<NativeReprKind> global_repr) {
  NativeReprAttribute attr = get_native_repr_attribute(core_type->ptyp_attributes, std::nullopt);
  const TypeDesc* td = get_desc(ty);
  using K = pt::CoreTypeDesc::Kind;
  const pt::CoreTypeDesc* d = core_type->ptyp_desc;
  if (d->kind == K::Ptyp_arrow && td->kind == DescKind::Tarrow && attr.present) {
    Error e(core_type->ptyp_loc, EK::Cannot_unbox_or_untag_type);
    e.repr = attr.kind;
    raise_error(e);
  }
  if (d->kind == K::Ptyp_arrow && td->kind == DescKind::Tarrow) {
    auto* pa = as<pt::Ptyp_arrow>(d);
    auto* ta = as<Tarrow>(td);
    TypeExpr* t1 = btype::tpoly_get_poly(ta->t1).first;
    NativeRepr repr_arg = make_native_repr(env, pa->t1, t1, global_repr);
    auto [repr_args, repr_res] = parse_native_repr_attributes(env, pa->t2, ta->t2, global_repr);
    repr_args.insert(repr_args.begin(), repr_arg);
    return {repr_args, repr_res};
  }
  if (d->kind == K::Ptyp_functor && td->kind == DescKind::Tfunctor) {
    Error e(core_type->ptyp_loc, EK::Type_cannot_be_external);
    e.ty = ty;
    raise_error(e);
  }
  if (auto* p = as<pt::Ptyp_poly>(d)) return parse_native_repr_attributes(env, p->ty, ty, global_repr);
  if (auto* a = as<pt::Ptyp_alias>(d)) return parse_native_repr_attributes(env, a->ty, ty, global_repr);
  if (d->kind == K::Ptyp_arrow || d->kind == K::Ptyp_functor) throw std::logic_error("parse_native_repr_attributes");
  if (td->kind == DescKind::Tarrow || td->kind == DescKind::Tfunctor)
    raise_error(Error(core_type->ptyp_loc, EK::External_with_non_syntactic_arity));
  return {{}, make_native_repr(env, core_type, ty, global_repr)};
}

// (warning 61, Unboxable_type_in_prim_decl, is not emitted; the expansions
// are kept)
void check_unboxable(env::t env, TypeExpr* ty) {
  std::function<void(TypeExpr*)> check_type = [&](TypeExpr* t) {
    t = ctype::expand_head_opt(env, t);
    const TypeDesc* d = get_desc(t);
    if (auto* tc = as<Tconstr>(d)) {
      try {
        env::find_type(tc->path, env);
      } catch (const env::NotFound&) {
      }
    } else if (auto* p = as<Tpoly>(d); p && p->vars.empty()) {
      check_type(p->body);
    }
  };
  btype::iter_type_expr(check_type, ty);
}
}  // namespace

// Translate a value declaration
std::pair<const tt::TValueDescription*, env::t> transl_value_decl(env::t env, const Location& loc,
                                                                  const pt::ValueDescription* valdecl) {
  return builtin_attributes::warning_scope(valdecl->pval_attributes, [&] {
    const tt::CoreType* cty = typetexp::transl_type_scheme(env, valdecl->pval_type);
    auto* v = make<ValueDescription>(cty->ctyp_type, ValueKind{}, loc,
                                     parsetree::types_attributes(valdecl->pval_attributes),
                                     uid::mk(env::get_current_unit()));
    auto [id, newenv] = env::enter_value(valdecl->pval_name.txt, v, env, [](std::string s) { return warnings::Warning::with_s(warnings::Warning::K::Unused_value_declaration, s); });
    auto* desc = make<tt::TValueDescription>(id, valdecl->pval_name, cty, v, valdecl->pval_loc, valdecl->pval_attributes);
    return std::make_pair(static_cast<const tt::TValueDescription*>(desc), newenv);
  });
}

// Translate a primitive description
std::pair<const tt::TPrimitiveDescription*, env::t> transl_prim_desc(env::t env, const Location& loc,
                                                                     const pt::PrimitiveDescription* primdesc) {
  return builtin_attributes::warning_scope(primdesc->pprim_attributes, [&]() -> std::pair<const tt::TPrimitiveDescription*, env::t> {
    const pt::PrimitiveKind& pk = primdesc->pprim_kind;
    if (pk.kind == pt::PrimitiveKind::Kind::Pprim_decl) {
      const pt::CoreType* pprim_type = pk.ty;
      const tt::CoreType* cty = typetexp::transl_type_scheme(env, pprim_type);
      TypeExpr* ty = cty->ctyp_type;
      std::optional<NativeReprKind> global_repr;
      NativeReprAttribute g = get_native_repr_attribute(primdesc->pprim_attributes, std::nullopt);
      if (g.present) global_repr = g.kind;
      auto [native_repr_args, native_repr_res] = parse_native_repr_attributes(env, pprim_type, ty, global_repr);
      const PrimitiveDescription* prim = primitive::parse_description(
          native_repr_args, native_repr_res, pk.prims, primdesc->pprim_attributes, primdesc->pprim_loc);
      if (prim->prim_arity == 0 && (prim->prim_name.empty() || prim->prim_name[0] != '%'))
        raise_error(Error(pprim_type->ptyp_loc, EK::Null_arity_external));
      if (clflags::native_code && prim->prim_arity > 5 && prim->prim_native_name.empty())
        raise_error(Error(pprim_type->ptyp_loc, EK::Missing_native_external));
      check_unboxable(env, ty);
      ValueKind vk{ValueKind::Kind::Val_prim};
      vk.prim = prim;
      auto* v = make<ValueDescription>(ty, vk, loc, parsetree::types_attributes(primdesc->pprim_attributes),
                                       uid::mk(env::get_current_unit()));
      auto [id, newenv] = env::enter_value(primdesc->pprim_name.txt, v, env, [](std::string s) { return warnings::Warning::with_s(warnings::Warning::K::Unused_value_declaration, s); });
      tt::PrimitiveKind tk{tt::PrimitiveKind::Kind::Tprim_decl, cty, pk.prims};
      return {make<tt::TPrimitiveDescription>(id, primdesc->pprim_name, tk, v, primdesc->pprim_loc,
                                              primdesc->pprim_attributes),
              newenv};
    }
    const pt::LidLoc& pprim_ident = pk.alias;
    auto [path, v0] = env::lookup_value(true, pprim_ident.loc, pprim_ident.txt, env);
    if (v0->val_kind.kind != ValueKind::Kind::Val_prim) {
      Error e(pprim_ident.loc, EK::Primitive_alias_does_not_refer_to_primitive);
      e.value_kind = v0->val_kind;
      raise_error(e);
    }
    const tt::CoreType* cty = nullptr;
    const ValueDescription* v = v0;
    if (pk.ty) {
      cty = typetexp::transl_type_scheme(env, pk.ty);
      // When the alias has a type ascription, we check that it is no more
      // general than the type of the aliased declaration.
      try {
        ctype::matches(true, env, cty->ctyp_type, v0->val_type);
      } catch (const ctype::MatchesFailure& m) {
        Error e(cty->ctyp_loc, EK::Primitive_type_mismatch);
        e.env = m.env;
        e.trace = m.err;
        raise_error(e);
      }
      auto* v2 = make<ValueDescription>(*v0);
      v2->val_type = cty->ctyp_type;
      v2->val_loc = loc;
      v = v2;
    }
    auto [id, newenv] = env::enter_value(primdesc->pprim_name.txt, v, env, [](std::string s) { return warnings::Warning::with_s(warnings::Warning::K::Unused_value_declaration, s); });
    tt::PrimitiveKind tk{tt::PrimitiveKind::Kind::Tprim_alias, cty, {}, path, pprim_ident};
    return {make<tt::TPrimitiveDescription>(id, primdesc->pprim_name, tk, v, primdesc->pprim_loc,
                                            primdesc->pprim_attributes),
            newenv};
  });
}

// Translate a "with" constraint -- much simplified version of
// transl_type_decl.  [sig_decl] is the declaration of [t] in [Sig], in
// [sig_env]; [sdecl] is typed in the outer environment.
const tt::TTypeDeclaration* transl_with_constraint(Ident::t id, Path::t fixed_row_path, env::t sig_env,
                                                   const TypeDeclaration* sig_decl0, env::t outer_env,
                                                   const pt::TypeDeclaration* sdecl) {
  // (Env.mark_type_used: usage marking only feeds warnings)
  return ctype::with_local_level_generalize([&]() -> const tt::TTypeDeclaration* {
    typetexp::ty_var_env::reset();
    // In the first part, we typecheck the syntactic declaration in the
    // outer environment.
    env::t env = outer_env;
    const Location& loc = sdecl->ptype_loc;
    std::vector<tt::TypeParam> tparams = make_params(env, sdecl->ptype_params);
    std::vector<TypeExpr*> params;
    for (auto& p : tparams) params.push_back(p.ty->ctyp_type);
    long arity = static_cast<long>(params.size());
    std::vector<tt::TypeConstraintItem> constraints;
    for (auto& c : sdecl->ptype_constraints) {
      const tt::CoreType* cty = typetexp::transl_simple_type(env, nullptr, false, c.t1);
      const tt::CoreType* cty2 = typetexp::transl_simple_type(env, nullptr, false, c.t2);
      // (the unification is delayed after the parameters')
      constraints.push_back({cty, cty2, c.loc});
    }
    bool no_row = !is_fixed_type(sdecl);
    if (!sdecl->ptype_manifest) throw std::logic_error("Typedecl.transl_with_constraint: no manifest");
    const tt::CoreType* tman = typetexp::transl_simple_type(env, nullptr, no_row, sdecl->ptype_manifest);
    TypeExpr* man = tman->ctyp_type;
    // In the second part, we check the consistency between the two
    // declarations and compute a "merged" declaration, in [sig_env].
    env = sig_env;
    const TypeDeclaration* sig_decl = ctype::instance_declaration(sig_decl0);
    bool arity_ok = arity == sig_decl->type_arity;
    if (arity_ok) {
      if (tparams.size() != sig_decl->type_params.size()) throw std::invalid_argument("List.iter2");
      for (std::size_t k = 0; k < tparams.size(); ++k) {
        try {
          ctype::unify_var(env, tparams[k].ty->ctyp_type, sig_decl->type_params[k]);
        } catch (const ctype::Unify& u) {
          Error e(tparams[k].ty->ctyp_loc, EK::Inconsistent_constraint);
          e.env = env;
          e.trace = u.err;
          raise_error(e);
        }
      }
    }
    for (auto& c : constraints) {
      // constraints must also be enforced in [sig_env]
      try {
        ctype::unify(env, c.t1->ctyp_type, c.t2->ctyp_type);
      } catch (const ctype::Unify& u) {
        Error e(c.loc, EK::Inconsistent_constraint);
        e.env = env;
        e.trace = u.err;
        raise_error(e);
      }
    }
    bool sig_decl_abstract = btype::type_kind_is_abstract(sig_decl);
    PrivateFlag priv = sdecl->ptype_private == PrivateFlag::Private ? PrivateFlag::Private
                       : (arity_ok && !sig_decl_abstract)          ? sig_decl->type_private
                                                                   : sdecl->ptype_private;
    // (the "spurious use of private" alert is not emitted)
    const TypeKind* type_kind;
    bool type_unboxed_default;
    if (arity_ok) {
      type_kind = sig_decl->type_kind;
      type_unboxed_default = sig_decl->type_unboxed_default;
    } else {
      type_kind = TYPE_ABSTRACT_LIT(Definition);
      type_unboxed_default = false;
    }
    auto* new_sig_decl = make<TypeDeclaration>();
    new_sig_decl->type_params = slice(params);
    new_sig_decl->type_arity = arity;
    new_sig_decl->type_kind = type_kind;
    new_sig_decl->type_private = priv;
    new_sig_decl->type_manifest = man;
    new_sig_decl->type_variance = {};
    new_sig_decl->type_separability = slice(default_separability(arity));
    new_sig_decl->type_is_newtype = false;
    new_sig_decl->type_expansion_scope = btype::lowest_level;
    new_sig_decl->type_loc = loc;
    new_sig_decl->type_attributes = parsetree::types_attributes(sdecl->ptype_attributes);
    new_sig_decl->type_immediate = TypeImmediacy::Unknown;
    new_sig_decl->type_unboxed_default = type_unboxed_default;
    new_sig_decl->type_uid = uid::mk(env::get_current_unit());
    if (fixed_row_path) set_private_row(env, sdecl->ptype_loc, fixed_row_path, new_sig_decl);
    if (TypeExpr* var = ctype::closed_type_decl(new_sig_decl)) {
      Error e(loc, EK::Unbound_type_var);
      e.ty = var;
      e.params = params;
      e.decl = new_sig_decl;
      raise_error(e);
    }
    const TypeDeclaration* nsd = name_recursion(sdecl, id, new_sig_decl);
    typedecl_variance::Prop new_type_variance;
    try {
      new_type_variance =
          typedecl_variance::compute_decl(env, id, nsd, typedecl_variance::variance_of_sdecl(sdecl));
    } catch (const typedecl_variance::Error& v) {
      Error e(v.loc, EK::Variance);
      e.variance = v;
      raise_error(e);
    }
    // Typedecl_immediacy.compute_decl never raises
    TypeImmediacy new_type_immediate = typedecl_immediacy::compute_decl(env, nsd);
    std::vector<Separability> new_type_separability;
    try {
      new_type_separability = typedecl_separability::compute_decl(env, nsd);
    } catch (const typedecl_separability::Error& s) {
      Error e(s.loc, EK::Separability);
      e.name = s.evar.some ? std::string(s.evar.v) : std::string();
      raise_error(e);
    }
    auto* final_decl = make<TypeDeclaration>(*nsd);
    final_decl->type_variance = slice(new_type_variance);
    final_decl->type_immediate = new_type_immediate;
    final_decl->type_separability = slice(new_type_separability);
    tt::TTypeKind tkind{tt::TTypeKind::Kind::Ttype_abstract};
    return make<tt::TTypeDeclaration>(id, sdecl->ptype_name, slice(tparams), final_decl, slice(constraints), tkind,
                                      sdecl->ptype_private, tman, loc, sdecl->ptype_attributes);
  });
}

// A simplified version of [transl_with_constraint], for packages.
const TypeDeclaration* transl_package_constraint(const Location& loc, env::t env, TypeExpr* ty) {
  auto* d = make<TypeDeclaration>();
  d->type_arity = 0;
  d->type_kind = TYPE_ABSTRACT_LIT(Definition);
  d->type_private = PrivateFlag::Public;
  d->type_manifest = ty;
  d->type_is_newtype = false;
  d->type_expansion_scope = btype::lowest_level;
  d->type_loc = loc;
  d->type_immediate = TypeImmediacy::Unknown;
  d->type_unboxed_default = false;
  d->type_uid = uid::mk(env::get_current_unit());
  // Typedecl_immediacy.compute_decl never raises
  d->type_immediate = typedecl_immediacy::compute_decl(env, d);
  return d;
}

// Approximate a type declaration: just make all types abstract
const TypeDeclaration* abstract_type_decl(bool injective, TypeOrigin explanation, long arity) {
  return ctype::with_local_level_generalize([&] {
    // Ctype.newvar () :: make_params (n-1): the last parameter is created
    // first
    std::vector<TypeExpr*> params(static_cast<std::size_t>(arity > 0 ? arity : 0));
    for (std::size_t k = params.size(); k-- > 0;) params[k] = ctype::newvar();
    auto* kind = make<TypeKind>();
    kind->origin = explanation;
    auto* d = make<TypeDeclaration>();
    d->type_params = slice(params);
    d->type_arity = arity;
    d->type_kind = kind;
    d->type_private = PrivateFlag::Public;
    d->type_manifest = nullptr;
    d->type_variance = slice(variance::unknown_signature(injective, arity));
    d->type_separability = slice(default_separability(arity));
    d->type_is_newtype = false;
    d->type_expansion_scope = btype::lowest_level;
    d->type_loc = location::none();
    d->type_immediate = TypeImmediacy::Unknown;
    d->type_unboxed_default = false;
    d->type_uid = uid::internal_not_actually_unique();
    return static_cast<const TypeDeclaration*>(d);
  });
}

std::vector<std::pair<Ident::t, const TypeDeclaration*>> approx_type_decl(TypeOrigin explanation,
                                                                          Slice<const pt::TypeDeclaration*> sdecls) {
  long scope = ctype::create_scope();
  std::vector<std::pair<Ident::t, const TypeDeclaration*>> out;
  for (auto* sdecl : sdecls) {
    bool injective = sdecl->ptype_kind.kind != pt::TypeKind::Kind::Ptype_abstract;
    // (Ident.create_scoped .., abstract_type_decl ..): right to left
    const TypeDeclaration* d =
        abstract_type_decl(injective, explanation, static_cast<long>(sdecl->ptype_params.size()));
    out.push_back({Ident::create_scoped(static_cast<int>(scope), sdecl->ptype_name.txt), d});
  }
  return out;
}

}  // namespace cppcaml::typing::typedecl
