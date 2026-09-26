// Port of typing/mtype.ml (scraping and strengthening).  See mtype.hpp.
#include "cppcaml/typing/mtype.hpp"
#include "cppcaml/typing/ctype.hpp"

namespace cppcaml::typing::mtype {

using namespace types;
using namespace btype;


const lz::Modtype* scrape_lazy(env::t env, const lz::Modtype* mty) {
  while (mty->kind == lz::Modtype::Kind::MtyL_ident) {
    try {
      mty = env::find_modtype_expansion_lazy(mty->path, env);
    } catch (const env::NotFound&) {
      return mty;
    }
  }
  return mty;
}

const ModuleType* scrape(env::t env, const ModuleType* mty) {
  if (mty->kind != ModuleType::Kind::Mty_ident) return mty;
  auto* l = make<lz::Modtype>(lz::Modtype::Kind::MtyL_ident);
  l->path = mty->path;
  return lz::force_modtype(scrape_lazy(env, l));
}

const ModuleType* freshen(int scope, const ModuleType* mty) {
  return subst::modtype(subst::Scoping::rescope(scope), subst::identity(), mty);
}

static lz::Signature strengthen_lazy_sig(bool aliasable, env::t env, lz::Signature sg, Path::t p);

const lz::Modtype* strengthen_lazy(bool aliasable, env::t env, const lz::Modtype* mty0, Path::t p) {
  using MK = lz::Modtype::Kind;
  const lz::Modtype* mty = scrape_lazy(env, mty0);
  if (mty->kind == MK::MtyL_signature) {
    auto* r = make<lz::Modtype>(MK::MtyL_signature);
    r->sign = strengthen_lazy_sig(aliasable, env, mty->sign, p);
    return r;
  }
  if (mty->kind == MK::MtyL_functor && !mty->param.is_unit && clflags::applicative_functors) {
    Ident::t param = mty->param.id;
    env::t env2 = env;
    if (param) {
      env2 = env::add_module_lazy(false, param, ModulePresence::Mp_present, mty->param.mty, env);
    } else {
      param = Ident::create_scoped(path::scope(p), "Arg");
    }
    auto* r = make<lz::Modtype>(MK::MtyL_functor);
    r->param.is_unit = false;
    r->param.id = param;
    r->param.mty = mty->param.mty;
    r->res = strengthen_lazy(false, env2, mty->res, Path::papply(p, Path::pident(param)));
    return r;
  }
  return mty;
}

const lz::ModuleDecl* strengthen_lazy_decl(bool aliasable, env::t env, const lz::ModuleDecl* md,
                                           Path::t p) {
  if (md->mdl_type->kind == lz::Modtype::Kind::MtyL_alias) return md;
  auto* r = make<lz::ModuleDecl>(*md);
  if (aliasable) {
    auto* a = make<lz::Modtype>(lz::Modtype::Kind::MtyL_alias);
    a->path = p;
    r->mdl_type = a;
  } else {
    r->mdl_type = strengthen_lazy(aliasable, env, md->mdl_type, p);
  }
  return r;
}

static std::vector<const lz::SignatureItem*> strengthen_lazy_sig2(
    bool aliasable, env::t env, Slice<const lz::SignatureItem*> sg, Path::t p) {
  using K = SignatureItem::Kind;
  std::vector<const lz::SignatureItem*> out;
  for (auto* item : sg) {
    switch (item->kind) {
      case K::Sig_value:
      case K::Sig_typext:
      case K::Sig_class:
      case K::Sig_class_type:
        out.push_back(item);
        break;
      case K::Sig_type: {
        const TypeDeclaration* decl = item->type;
        if (decl->type_kind->kind == TypeKind::Kind::Type_abstract &&
            is_row_name(ident::name(item->id)))
          break;  // dropped
        const TypeDeclaration* newdecl = decl;
        bool keep = decl->type_manifest &&
                    (decl->type_private == PrivateFlag::Public ||
                     decl->type_kind->kind == TypeKind::Kind::Type_record ||
                     decl->type_kind->kind == TypeKind::Kind::Type_variant);
        if (!keep) {
          TypeExpr* manif = newgenty(tconstr(Path::pdot(p, ident::name(item->id)),
                                             decl->type_params, make<MemoRef>(mnil())));
          auto* d = make<TypeDeclaration>(*decl);
          d->type_manifest = manif;
          if (type_kind_is_abstract(decl)) d->type_private = PrivateFlag::Public;
          newdecl = d;
        }
        auto* n = make<lz::SignatureItem>(*item);
        n->type = newdecl;
        out.push_back(n);
        break;
      }
      case K::Sig_module: {
        const lz::ModuleDecl* str =
            strengthen_lazy_decl(aliasable, env, item->md, Path::pdot(p, ident::name(item->id)));
        // Need to add the module in case it defines manifest module types
        env = env::add_module_declaration_lazy(false, item->id, item->presence, item->md, env);
        auto* n = make<lz::SignatureItem>(*item);
        n->md = str;
        out.push_back(n);
        break;
      }
      case K::Sig_modtype: {
        const lz::ModtypeDecl* decl = item->mtd;
        const lz::ModtypeDecl* newdecl = decl;
        if (!(decl->mtdl_type && !aliasable)) {
          // [not aliasable] condition needed because of recursive modules.
          auto* d = make<lz::ModtypeDecl>(*decl);
          auto* mt = make<lz::Modtype>(lz::Modtype::Kind::MtyL_ident);
          mt->path = Path::pdot(p, ident::name(item->id));
          d->mtdl_type = mt;
          newdecl = d;
        }
        // Need to add the module type in case it is manifest
        env = env::add_modtype_lazy(false, item->id, decl, env);
        auto* n = make<lz::SignatureItem>(*item);
        n->mtd = newdecl;
        out.push_back(n);
        break;
      }
    }
  }
  return out;
}

static lz::Signature strengthen_lazy_sig(bool aliasable, env::t env, lz::Signature sg, Path::t p) {
  Slice<const lz::SignatureItem*> items = lz::force_signature_once(sg);
  return lz::of_signature_items(slice(strengthen_lazy_sig2(aliasable, env, items, p)));
}

const ModuleType* strengthen(bool aliasable, env::t env, const ModuleType* mty, Path::t p) {
  return lz::force_modtype(strengthen_lazy(aliasable, env, lz::of_modtype(mty), p));
}

const ModuleDeclaration* strengthen_decl(bool aliasable, env::t env, const ModuleDeclaration* md,
                                         Path::t p) {
  return lz::force_module_decl(strengthen_lazy_decl(aliasable, env, lz::of_module_decl(md), p));
}

}  // namespace cppcaml::typing::mtype

namespace cppcaml::typing {
void install_forward_refs() {
  static bool done = false;
  if (done) return;
  done = true;
  env::strengthen = mtype::strengthen_lazy;
  ctype::forward_try_expand_safe = ctype::try_expand_safe_no_link;
  ctype::unify_var_ref = ctype::unify_var_uenv;
  ctype::nondep_type_ref = ctype::nondep_type;
  env::same_constr = ctype::same_constr;
}
}  // namespace cppcaml::typing
