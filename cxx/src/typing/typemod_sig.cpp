// Port of typing/typemod.ml, part 2: module types and signatures
// (transl_modtype, `with` constraints, transl_signature, module type
// declarations, recursive module types).
#include "cppcaml/typing/cmt_format.hpp"
#include "typemod_internal.hpp"

namespace cppcaml::typing::typemod {

using namespace types;
using pt::as;
using TK = tt::ModuleTypeDesc::Kind;
using TSK = tt::SignatureItemDesc::Kind;

const ModuleType* transl_modtype_longident_(const Location& loc, env::t env, Longident::t lid) {
  return mty_ident(env::lookup_modtype_path(true, loc, lid, env));
}
static Path::t transl_module_alias(const Location& loc, env::t env, Longident::t lid) {
  return env::lookup_module_path(true, loc, false, lid, env);
}

static const tt::ModuleType* mkmty(const tt::ModuleTypeDesc* desc, const ModuleType* typ, env::t env,
                                   const Location& loc, pt::Attributes attrs) {
  const tt::ModuleType* mty = make<tt::ModuleType>(desc, typ, env, loc, attrs);
  cmt_format::add_saved_type({cmt_format::BinaryPart::Kind::Partial_module_type, false, mty});
  return mty;
}
static const tt::SignatureItem* mksig(const tt::SignatureItemDesc* desc, env::t env, const Location& loc) {
  const tt::SignatureItem* sg = make<tt::SignatureItem>(desc, env, loc);
  cmt_format::add_saved_type({cmt_format::BinaryPart::Kind::Partial_signature_item, false, sg});
  return sg;
}
template <class D>
static const D* mkd(D d) {
  return make<D>(std::move(d));
}

static const tt::ModuleType* transl_modtype_aux(env::t env, const pt::ModuleType* smty);

const tt::ModuleType* transl_modtype(env::t env, const pt::ModuleType* smty) {
  return builtin_attributes::warning_scope(smty->pmty_attributes, [&] { return transl_modtype_aux(env, smty); });
}

static const tt::ModuleType* transl_modtype_functor_arg(env::t env, const pt::ModuleType* sarg) {
  const tt::ModuleType* mty = transl_modtype(env, sarg);
  auto* m2 = make<tt::ModuleType>(*mty);
  m2->mty_type = mtype::scrape_for_functor_arg(env, mty->mty_type);
  return m2;
}

static std::pair<std::vector<tt::WithConstraintItem>, Signature> transl_with(
    const Location& loc, env::t env, bool remove_aliases, std::vector<tt::WithConstraintItem> rev_tconstraints,
    Signature sg, const pt::WithConstraint* constr) {
  bool destructive = merge::is_destructive(constr);
  using K = pt::WithConstraint::Kind;
  tt::WithConstraint c{};
  Path::t path;
  pt::LidLoc lid;
  switch (constr->kind) {
    case K::Pwith_type:
    case K::Pwith_typesubst: {
      merge::TypeResult r = merge::merge_type(destructive, env, loc, sg, constr->lid, constr->decl);
      c.kind = destructive ? tt::WithConstraint::Kind::Twith_typesubst : tt::WithConstraint::Kind::Twith_type;
      c.decl = r.tdecl;
      path = r.path;
      lid = r.lid;
      sg = r.sg;
      break;
    }
    case K::Pwith_module:
    case K::Pwith_modsubst: {
      auto [p, md] = env::lookup_module(true, loc, constr->lid2.txt, env);
      c.kind = destructive ? tt::WithConstraint::Kind::Twith_modsubst : tt::WithConstraint::Kind::Twith_module;
      c.path = p;
      c.lid = constr->lid2;
      merge::Result r = merge::merge_module(false, destructive, env, loc, sg, constr->lid, md, p, remove_aliases);
      path = r.path;
      lid = r.lid;
      sg = r.sg;
      break;
    }
    case K::Pwith_modtype:
    case K::Pwith_modtypesubst: {
      const tt::ModuleType* tmty = transl_modtype(env, constr->mty);
      c.kind = destructive ? tt::WithConstraint::Kind::Twith_modtypesubst : tt::WithConstraint::Kind::Twith_modtype;
      c.mty = tmty;
      merge::Result r = merge::merge_modtype(false, destructive, env, loc, sg, constr->lid, tmty->mty_type);
      path = r.path;
      lid = r.lid;
      sg = r.sg;
      break;
    }
  }
  rev_tconstraints.insert(rev_tconstraints.begin(), tt::WithConstraintItem{path, lid, c});
  return {rev_tconstraints, sg};
}

static const tt::ModuleType* transl_modtype_aux(env::t env, const pt::ModuleType* smty) {
  const Location& loc = smty->pmty_loc;
  const pt::ModuleTypeDesc* d = smty->pmty_desc;
  using K = pt::ModuleTypeDesc::Kind;
  switch (d->kind) {
    case K::Pmty_ident: {
      const pt::LidLoc& lid = as<pt::Pmty_ident>(d)->lid;
      const ModuleType* m = transl_modtype_longident_(loc, env, lid.txt);
      return mkmty(mkd(tt::Tmty_ident{{TK::Tmty_ident}, m->path, lid}), m, env, loc, smty->pmty_attributes);
    }
    case K::Pmty_alias: {
      const pt::LidLoc& lid = as<pt::Pmty_alias>(d)->lid;
      Path::t path = transl_module_alias(loc, env, lid.txt);
      return mkmty(mkd(tt::Tmty_alias{{TK::Tmty_alias}, path, lid}), mty_alias(path), env, loc, smty->pmty_attributes);
    }
    case K::Pmty_signature: {
      const tt::Signature* sg = transl_signature(env, as<pt::Pmty_signature>(d)->sg);
      return mkmty(mkd(tt::Tmty_signature{{TK::Tmty_signature}, sg}), mty_signature(sg->sig_type), env, loc,
                   smty->pmty_attributes);
    }
    case K::Pmty_functor: {
      auto* f = as<pt::Pmty_functor>(d);
      tt::FunctorParameter t_arg{true};
      FunctorParameter ty_arg;
      env::t newenv = env;
      if (!f->param.is_unit) {
        const tt::ModuleType* arg = transl_modtype_functor_arg(env, f->param.mty);
        Ident::t id = nullptr;
        if (f->param.name.txt.some) {
          long scope = ctype::create_scope();
          auto* arg_md = make<ModuleDeclaration>(arg->mty_type, Attributes{}, f->param.name.loc,
                                                 uid::mk(env::get_current_unit()));
          auto [i, e2] = env::enter_module_declaration(static_cast<int>(scope), f->param.name.txt.v,
                                                       ModulePresence::Mp_present, arg_md, env, true);
          id = i;
          newenv = e2;
        }
        t_arg = tt::FunctorParameter{false, id, f->param.name, arg};
        ty_arg.is_unit = false;
        ty_arg.named_obj = fresh_identity();
        ty_arg.id = id;
        ty_arg.some_obj = id ? fresh_identity() : nullptr;
        t_arg.some_obj = ty_arg.some_obj;  // Named (id, ...), Types.Named (id, ...): one `Some id`
        ty_arg.mty = arg->mty_type;
      }
      const tt::ModuleType* res = transl_modtype(newenv, f->body);
      return mkmty(mkd(tt::Tmty_functor{{TK::Tmty_functor}, t_arg, res}), mty_functor(ty_arg, res->mty_type), env, loc,
                   smty->pmty_attributes);
    }
    case K::Pmty_with: {
      auto* w = as<pt::Pmty_with>(d);
      const tt::ModuleType* body = transl_modtype(env, w->mty);
      Signature init_sg = extract_sig(env, w->mty->pmty_loc, body->mty_type);
      bool remove_aliases = builtin_attributes::has_attribute("remove_aliases", smty->pmty_attributes);
      std::vector<tt::WithConstraintItem> rev;
      Signature final_sg = init_sg;
      for (auto* c : w->cstrs) std::tie(rev, final_sg) = transl_with(smty->pmty_loc, env, remove_aliases, rev, final_sg, c);
      long scope = ctype::create_scope();
      std::vector<tt::WithConstraintItem> cs(rev.rbegin(), rev.rend());
      return mkmty(mkd(tt::Tmty_with{{TK::Tmty_with}, body, slice(cs)}),
                   mtype::freshen(static_cast<int>(scope), mty_signature(final_sg)), env, loc, smty->pmty_attributes);
    }
    case K::Pmty_typeof: {
      env::t env2 = env::in_signature(false, env);
      auto [tmty, mty] = type_module_type_of_fwd(env2, as<pt::Pmty_typeof>(d)->me);
      return mkmty(mkd(tt::Tmty_typeof{{TK::Tmty_typeof}, tmty}), mty, env2, loc, smty->pmty_attributes);
    }
    case K::Pmty_extension: throw ErrorForward(as<pt::Pmty_extension>(d)->ext);
  }
  throw std::logic_error("transl_modtype");
}

TranslModtypeDecl transl_modtype_decl(env::t env, const pt::ModuleTypeDeclaration* pmtd) {
  return builtin_attributes::warning_scope(pmtd->pmtd_attributes, [&] {
    const tt::ModuleType* tmty = pmtd->pmtd_type ? transl_modtype(env::in_signature(true, env), pmtd->pmtd_type) : nullptr;
    auto* decl = make<ModtypeDeclaration>(tmty ? tmty->mty_type : nullptr,
                                          parsetree::types_attributes(pmtd->pmtd_attributes), pmtd->pmtd_loc,
                                          uid::mk(env::get_current_unit()));
    long scope = ctype::create_scope();
    auto [id, newenv] = env::enter_modtype(static_cast<int>(scope), pmtd->pmtd_name.txt, decl, env);
    auto* mtd = make<tt::TModuleTypeDeclaration>(id, pmtd->pmtd_name, decl->mtd_uid, tmty, pmtd->pmtd_attributes,
                                                 pmtd->pmtd_loc);
    return TranslModtypeDecl{mtd, decl, newenv};
  });
}


TranslRecmodule transl_recmodule_modtypes(env::t env, Slice<const pt::ModuleDeclaration*> sdecls) {
  struct Cur {
    Ident::t id;  // id_shape: None when id is null
    pt::OptStrLoc id_loc;
    const ModuleDeclaration* md;
    const tt::ModuleType* tmty;
    shape::t shape;  // id_shape's shape
  };
  auto make_env = [&](const std::vector<Cur>& curr) {
    env::t e = env;
    for (auto& c : curr)
      if (c.id) e = env::add_module_declaration(true, c.id, ModulePresence::Mp_present, c.md, e, true, c.shape);
    return e;
  };
  auto transition = [&](env::t env_c, const std::vector<Cur>& curr) {
    std::vector<Cur> out;
    for (std::size_t k = 0; k < sdecls.size(); ++k) {
      const tt::ModuleType* tmty = builtin_attributes::warning_scope(
          sdecls[k]->pmd_attributes, [&] { return transl_modtype(env_c, sdecls[k]->pmd_type); });
      auto* md = make<ModuleDeclaration>(*curr[k].md);
      md->md_type = tmty->mty_type;
      out.push_back({curr[k].id, curr[k].id_loc, md, tmty, curr[k].shape});
    }
    return out;
  };
  auto map_mtys = [](const std::vector<Cur>& curr) {
    std::vector<std::pair<Ident::t, const ModuleDeclaration*>> out;
    for (auto& c : curr)
      if (c.id) out.push_back({c.id, c.md});
    return out;
  };
  long scope = ctype::create_scope();
  std::vector<Ident::t> ids;
  for (auto* x : sdecls) ids.push_back(x->pmd_name.txt.some ? Ident::create_scoped(static_cast<int>(scope), x->pmd_name.txt.v) : nullptr);
  auto approx_env = [&](OptStr container) {
    env::t e = env;
    for (Ident::t id : ids)
      if (id)  // cf #5965
        e = env::enter_unbound_module(ident::name(id), env::ModuleUnboundReason{container, ident::name(id)}, e);
    return e;
  };
  std::vector<Cur> init;
  for (std::size_t k = 0; k < sdecls.size(); ++k) {
    const pt::ModuleDeclaration* pmd = sdecls[k];
    Uid md_uid = uid::mk(env::get_current_unit());
    auto* md = make<ModuleDeclaration>(approx_modtype(approx_env(pmd->pmd_name.txt), pmd->pmd_type),
                                       parsetree::types_attributes(pmd->pmd_attributes), pmd->pmd_loc, md_uid);
    // id_shape: Shape.var md_uid id, the shape make_env's environments
    // give the module (Env.add_module_declaration ~shape)
    shape::t id_shape = ids[k] ? shape::var(md_uid, ids[k]) : nullptr;
    init.push_back({ids[k], pmd->pmd_name, md, nullptr, id_shape});
  }
  env::t abs_env = make_env(init);
  std::vector<Cur> dcl1 = warnings::without_warnings([&] { return transition(abs_env, init); });
  env::t env1 = make_env(dcl1);
  check_recmod_typedecls(env1, map_mtys(dcl1));
  std::vector<Cur> dcl2 = transition(env1, dcl1);
  env::t env2 = make_env(dcl2);
  check_recmod_typedecls(env2, map_mtys(dcl2));
  std::vector<const tt::TModuleDeclaration*> out;
  for (std::size_t k = 0; k < sdecls.size(); ++k)
    out.push_back(make<tt::TModuleDeclaration>(dcl2[k].id, dcl2[k].id_loc, dcl2[k].md->md_uid,
                                               ModulePresence::Mp_present, dcl2[k].tmty, sdecls[k]->pmd_attributes,
                                               sdecls[k]->pmd_loc));
  return {out, env2};
}

const tt::Signature* transl_signature_(env::t env0, pt::Signature ssg) {
  SignatureNames* names = create_signature_names();
  struct ItemResult {
    const tt::SignatureItem* item;
    std::vector<const SignatureItem*> sg;
    env::t env;
  };
  auto transl_sig_item = [&](env::t env, const pt::SignatureItem* item) -> ItemResult {
    const Location& loc = item->psig_loc;
    const pt::SignatureItemDesc* d = item->psig_desc;
    using K = pt::SignatureItemDesc::Kind;
    switch (d->kind) {
      case K::Psig_value: {
        auto [tdesc, newenv] = typedecl::transl_value_decl(env, item->psig_loc, as<pt::Psig_value>(d)->vd);
        check_value(names, tdesc->val_loc, tdesc->val_id);
        return {mksig(mkd(tt::Tsig_value{{TSK::Tsig_value}, tdesc}), env, loc),
                {sig_value(tdesc->val_id, tdesc->val_val, Visibility::Exported)},
                newenv};
      }
      case K::Psig_primitive: {
        auto [tdesc, newenv] = typedecl::transl_prim_desc(env, item->psig_loc, as<pt::Psig_primitive>(d)->pd);
        check_value(names, tdesc->prim_loc, tdesc->prim_id);
        return {mksig(mkd(tt::Tsig_primitive{{TSK::Tsig_primitive}, tdesc}), env, loc),
                {sig_value(tdesc->prim_id, tdesc->prim_val, Visibility::Exported)},
                newenv};
      }
      case K::Psig_type: {
        auto* t = as<pt::Psig_type>(d);
        typedecl::TranslTypeDeclResult r = typedecl::transl_type_decl(env, t->rec, t->decls);
        for (auto* td : r.decls) check_type(names, td->typ_loc, td->typ_id);
        return {mksig(mkd(tt::Tsig_type{{TSK::Tsig_type}, t->rec, slice(r.decls)}), env, loc),
                map_rec_type_with_row_types(t->rec, r.decls), r.env};
      }
      case K::Psig_typesubst: {
        typedecl::TranslTypeDeclResult r =
            typedecl::transl_type_decl(env, RecFlag::Nonrecursive, as<pt::Psig_typesubst>(d)->decls);
        for (auto* td : r.decls) {
          if (td->typ_kind.kind != tt::TTypeKind::Kind::Ttype_abstract || !td->typ_manifest ||
              td->typ_private == PrivateFlag::Private)
            raise_error(err(td->typ_loc, env, EK::Invalid_type_subst_rhs));
          Slice<TypeExpr*> params = td->typ_type->type_params;
          if (params_are_constrained(params)) raise_error(err(loc, env, EK::With_cannot_remove_constrained_type));
          if (!td->typ_type->type_manifest) throw std::logic_error("Option.get");
          subst::t s = subst::unsafe::add_type_function(Path::pident(td->typ_id), params, td->typ_type->type_manifest,
                                                        subst::identity());
          check_type(names, td->typ_loc, td->typ_id, NameInfo{NameInfo::Kind::Substituted_away, {}, s});
        }
        return {mksig(mkd(tt::Tsig_typesubst{{TSK::Tsig_typesubst}, slice(r.decls)}), env, loc), {}, r.env};
      }
      case K::Psig_typext: {
        auto [tyext, newenv, shapes_] = typedecl::transl_type_extension(false, env, item->psig_loc, as<pt::Psig_typext>(d)->ext);
        for (auto* ext : tyext->tyext_constructors) check_typext(names, ext->ext_loc, ext->ext_id);
        std::vector<const SignatureItem*> sg;
        for (std::size_t k = 0; k < tyext->tyext_constructors.size(); ++k) {
          auto* ext = tyext->tyext_constructors[k];
          sg.push_back(sig_typext(ext->ext_id, ext->ext_type, k == 0 ? ExtStatus::Text_first : ExtStatus::Text_next,
                                  Visibility::Exported));
        }
        return {mksig(mkd(tt::Tsig_typext{{TSK::Tsig_typext}, tyext}), env, loc), sg, newenv};
      }
      case K::Psig_exception: {
        auto [ext, newenv, shape_] = typedecl::transl_type_exception(env, as<pt::Psig_exception>(d)->exn);
        const tt::TExtensionConstructor* c = ext->tyexn_constructor;
        check_typext(names, c->ext_loc, c->ext_id);
        return {mksig(mkd(tt::Tsig_exception{{TSK::Tsig_exception}, ext}), env, loc),
                {sig_typext(c->ext_id, c->ext_type, ExtStatus::Text_exception, Visibility::Exported)},
                newenv};
      }
      case K::Psig_module: {
        const pt::ModuleDeclaration* pmd = as<pt::Psig_module>(d)->md;
        long scope = ctype::create_scope();
        const tt::ModuleType* tmty =
            builtin_attributes::warning_scope(pmd->pmd_attributes, [&] { return transl_modtype(env, pmd->pmd_type); });
        ModulePresence pres = ModulePresence::Mp_present;
        if (tmty->mty_type->kind == ModuleType::Kind::Mty_alias) {
          if (!env::is_aliasable(tmty->mty_type->path, env)) {
            Error e = err(pmd->pmd_loc, env, EK::Cannot_alias);
            e.path = tmty->mty_type->path;
            raise_error(e);
          }
          pres = ModulePresence::Mp_absent;
        }
        auto* md = make<ModuleDeclaration>(tmty->mty_type, parsetree::types_attributes(pmd->pmd_attributes),
                                           pmd->pmd_loc, uid::mk(env::get_current_unit()));
        Ident::t id = nullptr;
        env::t newenv = env;
        if (pmd->pmd_name.txt.some) {
          auto [i, e2] = env::enter_module_declaration(static_cast<int>(scope), pmd->pmd_name.txt.v, pres, md, env);
          check_module(names, pmd->pmd_name.loc, i);
          id = i;
          newenv = e2;
        }
        auto* tmd = make<tt::TModuleDeclaration>(id, pmd->pmd_name, md->md_uid, pres, tmty, pmd->pmd_attributes,
                                                 pmd->pmd_loc);
        std::vector<const SignatureItem*> sg;
        if (id) sg.push_back(sig_module(id, pres, md, RecStatus::Trec_not, Visibility::Exported));
        return {mksig(mkd(tt::Tsig_module{{TSK::Tsig_module}, tmd}), env, loc), sg, newenv};
      }
      case K::Psig_modsubst: {
        const pt::ModuleSubstitution* pms = as<pt::Psig_modsubst>(d)->ms;
        long scope = ctype::create_scope();
        auto [path, md0] = env::lookup_module(true, pms->pms_manifest.loc, pms->pms_manifest.txt, env);
        bool aliasable = env::is_aliasable(path, env);
        const ModuleDeclaration* md;
        if (!aliasable) {
          auto* m2 = make<ModuleDeclaration>(*md0);
          m2->md_loc = pms->pms_loc;
          m2->md_uid = uid::mk(env::get_current_unit());
          md = m2;
        } else {
          md = make<ModuleDeclaration>(mty_alias(path), parsetree::types_attributes(pms->pms_attributes), pms->pms_loc,
                                       uid::mk(env::get_current_unit()));
        }
        ModulePresence pres =
            md->md_type->kind == ModuleType::Kind::Mty_alias ? ModulePresence::Mp_absent : ModulePresence::Mp_present;
        auto [id, newenv] = env::enter_module_declaration(static_cast<int>(scope), pms->pms_name.txt, pres, md, env);
        check_module(names, pms->pms_name.loc, id,
                     NameInfo{NameInfo::Kind::Substituted_away, {}, subst::add_module(id, path, subst::identity())});
        auto* ms = make<tt::TModuleSubstitution>(id, pms->pms_name, md->md_uid, path, pms->pms_manifest,
                                                 pms->pms_attributes, pms->pms_loc);
        return {mksig(mkd(tt::Tsig_modsubst{{TSK::Tsig_modsubst}, ms}), env, loc), {}, newenv};
      }
      case K::Psig_recmodule: {
        TranslRecmodule r = transl_recmodule_modtypes(env, as<pt::Psig_recmodule>(d)->mds);
        std::vector<const tt::TModuleDeclaration*> decls;
        for (auto* md : r.decls)
          if (md->md_id) decls.push_back(md);
        for (auto* md : decls) check_module(names, md->md_loc, md->md_id);
        std::vector<const SignatureItem*> sg;
        for (std::size_t k = 0; k < decls.size(); ++k) {
          auto* md = decls[k];
          auto* dd = make<ModuleDeclaration>(md->md_type->mty_type, parsetree::types_attributes(md->md_attributes),
                                             md->md_loc, md->md_uid);
          sg.push_back(sig_module(md->md_id, ModulePresence::Mp_present, dd,
                                  k == 0 ? RecStatus::Trec_first : RecStatus::Trec_next, Visibility::Exported));
        }
        return {mksig(mkd(tt::Tsig_recmodule{{TSK::Tsig_recmodule}, slice(r.decls)}), env, loc), sg, r.env};
      }
      case K::Psig_modtype: {
        const pt::ModuleTypeDeclaration* pmtd = as<pt::Psig_modtype>(d)->mtd;
        auto [mtd, decl, newenv] = transl_modtype_decl(env, pmtd);
        check_modtype(names, pmtd->pmtd_loc, mtd->mtd_id);
        return {mksig(mkd(tt::Tsig_modtype{{TSK::Tsig_modtype}, mtd}), env, loc),
                {sig_modtype(mtd->mtd_id, decl, Visibility::Exported)},
                newenv};
      }
      case K::Psig_modtypesubst: {
        const pt::ModuleTypeDeclaration* pmtd = as<pt::Psig_modtypesubst>(d)->mtd;
        auto [mtd, decl_, newenv] = transl_modtype_decl(env, pmtd);
        (void)decl_;
        // (parsetree invariant, see Ast_invariants)
        if (!mtd->mtd_type) throw std::logic_error("Psig_modtypesubst");
        subst::t s = subst::unsafe::add_modtype(mtd->mtd_id, mtd->mtd_type->mty_type, subst::identity());
        check_modtype(names, pmtd->pmtd_loc, mtd->mtd_id, NameInfo{NameInfo::Kind::Substituted_away, {}, s});
        return {mksig(mkd(tt::Tsig_modtypesubst{{TSK::Tsig_modtypesubst}, mtd}), env, loc), {}, newenv};
      }
      case K::Psig_open: {
        auto [od, newenv] = type_open_descr(nullptr, false, env, as<pt::Psig_open>(d)->od);
        return {mksig(mkd(tt::Tsig_open{{TSK::Tsig_open}, od}), env, loc), {}, newenv};
      }
      case K::Psig_include: {
        const pt::IncludeDescription* sincl = as<pt::Psig_include>(d)->incl;
        const pt::ModuleType* smty = sincl->pincl_mod;
        const tt::ModuleType* tmty =
            builtin_attributes::warning_scope(sincl->pincl_attributes, [&] { return transl_modtype(env, smty); });
        long scope = ctype::create_scope();
        auto [sg, newenv] =
            env::enter_signature(static_cast<int>(scope), extract_sig(env, smty->pmty_loc, tmty->mty_type), env);
        signature_group::iter([&](const signature_group::RecGroup& g) { check_sig_item(names, item->psig_loc, g); },
                              sg);
        auto* incl = make<tt::IncludeDescription>(tmty, sg, sincl->pincl_loc, sincl->pincl_attributes);
        return {mksig(mkd(tt::Tsig_include{{TSK::Tsig_include}, incl}), env, loc),
                std::vector<const SignatureItem*>(sg.begin(), sg.end()), newenv};
      }
      case K::Psig_class: {
        auto [classes, newenv] = typeclass::class_descriptions(env, as<pt::Psig_class>(d)->decls);
        for (auto& cls : classes) {
          const Location& l = cls.cls_id_loc.loc;
          check_type(names, l, cls.cls_obj_id);
          check_class(names, l, cls.cls_id);
          check_class_type(names, l, cls.cls_ty_id);
        }
        std::vector<const SignatureItem*> sg;
        std::vector<const tt::TClassDescription*> infos;
        for (std::size_t k = 0; k < classes.size(); ++k) {
          auto& cls = classes[k];
          RecStatus rs = k == 0 ? RecStatus::Trec_first : RecStatus::Trec_next;
          sg.push_back(sig_class(cls.cls_id, cls.cls_decl, rs, Visibility::Exported));
          sg.push_back(sig_class_type(cls.cls_ty_id, cls.cls_ty_decl, rs, Visibility::Exported));
          sg.push_back(sig_type(cls.cls_obj_id, cls.cls_obj_abbr, rs, Visibility::Exported));
          infos.push_back(cls.cls_info);
        }
        return {mksig(mkd(tt::Tsig_class{{TSK::Tsig_class}, slice(infos)}), env, loc), sg, newenv};
      }
      case K::Psig_class_type: {
        auto [classes, newenv] = typeclass::class_type_declarations(env, as<pt::Psig_class_type>(d)->decls);
        for (auto& decl : classes) {
          const Location& l = decl.clsty_id_loc.loc;
          check_class_type(names, l, decl.clsty_ty_id);
          check_type(names, l, decl.clsty_obj_id);
        }
        std::vector<const SignatureItem*> sg;
        std::vector<const tt::TClassTypeDeclaration*> infos;
        for (std::size_t k = 0; k < classes.size(); ++k) {
          auto& decl = classes[k];
          RecStatus rs = k == 0 ? RecStatus::Trec_first : RecStatus::Trec_next;
          sg.push_back(sig_class_type(decl.clsty_ty_id, decl.clsty_ty_decl, rs, Visibility::Exported));
          sg.push_back(sig_type(decl.clsty_obj_id, decl.clsty_obj_abbr, rs, Visibility::Exported));
          infos.push_back(decl.clsty_info);
        }
        return {mksig(mkd(tt::Tsig_class_type{{TSK::Tsig_class_type}, slice(infos)}), env, loc), sg, newenv};
      }
      case K::Psig_attribute:
        builtin_attributes::warning_attribute(as<pt::Psig_attribute>(d)->attr);
        return {mksig(mkd(tt::Tsig_attribute{{TSK::Tsig_attribute}, as<pt::Psig_attribute>(d)->attr}), env, loc), {},
                env};
      case K::Psig_extension: throw ErrorForward(as<pt::Psig_extension>(d)->ext);
    }
    throw std::logic_error("transl_sig_item");
  };
  // (Typing_recovery_state.with_saved_types / warning_scope [])
  env::t env = env::in_signature(true, env0);
  std::vector<const tt::SignatureItem*> trem;
  std::vector<const SignatureItem*> rem;
  for (auto* item : ssg) {
    ItemResult r = transl_sig_item(env, item);
    trem.push_back(r.item);
    rem.insert(rem.end(), r.sg.begin(), r.sg.end());
    env = r.env;
  }
  Signature rem2 = simplify(env, names, slice(rem));
  return make<tt::Signature>(slice(trem), rem2, env);
}

const tt::Signature* transl_signature(env::t env, pt::Signature ssg) {
  return cmt_format::with_saved_types(
      [&] { return builtin_attributes::warning_scope(pt::Attributes{}, [&] { return transl_signature_(env, ssg); }); },
      [](const tt::Signature* sg) {
        return cmt_format::BinaryPart{cmt_format::BinaryPart::Kind::Partial_signature, false, sg};
      });
}

}  // namespace cppcaml::typing::typemod
