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
      param = Ident::create_scoped(path::scope(p), OCAML_LIT("Arg"));
    }
    auto* r = make<lz::Modtype>(MK::MtyL_functor);
    r->param.is_unit = false;
    r->param.named_obj = fresh_identity();
    r->param.id = param;
    r->param.some_obj = fresh_identity();  // Named (Some param, arg): a new block
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
          d->manifest_obj.reset();  // a new Some block
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

// ---- the rest of mtype.ml ---------------------------------------------------------------
namespace cppcaml::typing::mtype {

using namespace types;
using namespace btype;
using MK = ModuleType::Kind;
using SK = SignatureItem::Kind;

static const SignatureItem* with_md(const SignatureItem* it, ModulePresence pres, const ModuleType* mty) {
  auto* md = make<ModuleDeclaration>(*it->md);
  md->md_type = mty;
  auto* n = make<SignatureItem>(*it);
  n->presence = pres;
  n->md = md;
  return n;
}

static std::pair<ModulePresence, const ModuleType*> make_aliases_absent(ModulePresence pres, const ModuleType* mty);
static Signature make_aliases_absent_sig(Signature sg) {
  std::vector<const SignatureItem*> out;
  for (auto* it : sg) {
    if (it->kind == SK::Sig_module) {
      auto [pres, mty] = make_aliases_absent(it->presence, it->md->md_type);
      out.push_back(with_md(it, pres, mty));
    } else {
      out.push_back(it);
    }
  }
  return slice(out);
}
static std::pair<ModulePresence, const ModuleType*> make_aliases_absent(ModulePresence pres, const ModuleType* mty) {
  switch (mty->kind) {
    case MK::Mty_alias: return {ModulePresence::Mp_absent, mty};
    case MK::Mty_signature: {
      auto* r = make<ModuleType>(*mty);
      r->sign = make_aliases_absent_sig(mty->sign);
      return {pres, r};
    }
    case MK::Mty_functor: {
      auto res = make_aliases_absent(ModulePresence::Mp_present, mty->res).second;
      auto* r = make<ModuleType>(*mty);
      r->res = res;
      return {pres, r};
    }
    default: return {pres, mty};
  }
}

static std::pair<ModulePresence, const ModuleType*> scrape_for_type_of_(env::t env, ModulePresence pres,
                                                                        const ModuleType* mty0) {
  std::function<const ModuleType*(Path::t, const ModuleType*)> loop = [&](Path::t path,
                                                                           const ModuleType* mty) -> const ModuleType* {
    if (mty->kind == MK::Mty_alias) {
      try {
        const ModuleDeclaration* md = env::find_module(mty->path, env);
        return loop(mty->path, md->md_type);
      } catch (const env::NotFound&) {
        return mty;
      }
    }
    if (path) return strengthen(false, env, mty, path);
    return mty;
  };
  return make_aliases_absent(pres, loop(nullptr, mty0));
}

// In nondep_supertype, env is only used for the type it assigns to id.
// Hence there is no need to keep env up-to-date by adding the bindings
// traversed.
enum class Va { Co, Contra, Strict };
static const ModuleType* nondep_mty(env::t env, Va va, const std::vector<Ident::t>& ids, const ModuleType* mty);
static Signature nondep_sig(env::t env, Va va, const std::vector<Ident::t>& ids, Signature sg);
static const ModtypeDeclaration* nondep_modtype_decl(env::t env, const std::vector<Ident::t>& ids,
                                                     const ModtypeDeclaration* mtd);

static std::pair<ModulePresence, const ModuleType*> nondep_mty_with_presence(env::t env, Va va,
                                                                             const std::vector<Ident::t>& ids,
                                                                             ModulePresence pres,
                                                                             const ModuleType* mty) {
  switch (mty->kind) {
    case MK::Mty_ident: {
      if (auto id = path::find_free_opt(ids, mty->path)) {
        const ModuleType* expansion;
        try {
          expansion = env::find_modtype_expansion(mty->path, env);
        } catch (const env::NotFound&) {
          throw ctype::NondepCannotErase{*id};
        }
        return nondep_mty_with_presence(env, va, ids, pres, expansion);
      }
      return {pres, mty};
    }
    case MK::Mty_alias: {
      if (auto id = path::find_free_opt(ids, mty->path)) {
        const ModuleDeclaration* expansion;
        try {
          expansion = env::find_module(mty->path, env);
        } catch (const env::NotFound&) {
          throw ctype::NondepCannotErase{*id};
        }
        return nondep_mty_with_presence(env, va, ids, ModulePresence::Mp_present, expansion->md_type);
      }
      return {pres, mty};
    }
    case MK::Mty_signature: {
      auto* r = make<ModuleType>(*mty);
      r->sign = nondep_sig(env, va, ids, mty->sign);
      return {pres, r};
    }
    case MK::Mty_functor: {
      if (mty->param.is_unit) {
        auto* r = make<ModuleType>(*mty);
        r->res = nondep_mty(env, va, ids, mty->res);
        return {pres, r};
      }
      Va var_inv = va == Va::Co ? Va::Contra : va == Va::Contra ? Va::Co : Va::Strict;
      env::t res_env = env;
      if (mty->param.id) res_env = env::add_module(mty->param.id, ModulePresence::Mp_present, mty->param.mty, env, true);
      // Mty_functor(Named (param, nondep_mty env var_inv ids arg), nondep_mty res_env va ids res):
      // right to left
      const ModuleType* res = nondep_mty(res_env, va, ids, mty->res);
      const ModuleType* arg = nondep_mty(env, var_inv, ids, mty->param.mty);
      auto* r = make<ModuleType>(*mty);
      r->param.named_obj = fresh_identity();
      r->param.mty = arg;
      r->res = res;
      return {pres, r};
    }
  }
  return {pres, mty};
}
static const ModuleType* nondep_mty(env::t env, Va va, const std::vector<Ident::t>& ids, const ModuleType* mty) {
  return nondep_mty_with_presence(env, va, ids, ModulePresence::Mp_present, mty).second;
}
static const SignatureItem* nondep_sig_item_(env::t env, Va va, const std::vector<Ident::t>& ids,
                                             const SignatureItem* it) {
  auto* n = make<SignatureItem>(*it);
  switch (it->kind) {
    case SK::Sig_value: {
      auto* d = make<ValueDescription>(*it->value);
      d->val_type = ctype::nondep_type(env, ids, it->value->val_type);
      n->value = d;
      break;
    }
    case SK::Sig_type: n->type = ctype::nondep_type_decl(env, ids, va == Va::Co, it->type); break;
    case SK::Sig_typext: n->ext = ctype::nondep_extension_constructor(env, ids, it->ext); break;
    case SK::Sig_module: {
      auto [pres, mty] = nondep_mty_with_presence(env, va, ids, it->presence, it->md->md_type);
      return with_md(it, pres, mty);
    }
    case SK::Sig_modtype: n->mtd = nondep_modtype_decl(env, ids, it->mtd); break;
    case SK::Sig_class: n->cls = ctype::nondep_class_declaration(env, ids, it->cls); break;
    case SK::Sig_class_type: n->clty = ctype::nondep_cltype_declaration(env, ids, it->clty); break;
  }
  return n;
}
static Signature nondep_sig(env::t env, Va va, const std::vector<Ident::t>& ids, Signature sg0) {
  long scope = ctype::create_scope();
  auto [sg, env2] = env::enter_signature(static_cast<int>(scope), sg0, env);
  std::vector<const SignatureItem*> out;
  for (auto* it : sg) out.push_back(nondep_sig_item_(env2, va, ids, it));
  return slice(out);
}
static const ModtypeDeclaration* nondep_modtype_decl(env::t env, const std::vector<Ident::t>& ids,
                                                     const ModtypeDeclaration* mtd) {
  auto* r = make<ModtypeDeclaration>(*mtd);
  if (mtd->mtd_type) r->mtd_type = nondep_mty(env, Va::Strict, ids, mtd->mtd_type);
  return r;
}
const ModuleType* nondep_supertype(env::t env, const std::vector<Ident::t>& ids, const ModuleType* mty) {
  return nondep_mty(env, Va::Co, ids, mty);
}
const SignatureItem* nondep_sig_item(env::t env, const std::vector<Ident::t>& ids, const SignatureItem* item) {
  return nondep_sig_item_(env, Va::Co, ids, item);
}

const TypeDeclaration* enrich_typedecl(env::t env, Path::t p, Ident::t id, const TypeDeclaration* decl) {
  if (decl->type_manifest) return decl;
  const TypeDeclaration* orig_decl;
  try {
    orig_decl = env::find_type(p, env);
  } catch (const env::NotFound&) {
    // Type which was not present in the signature, so we don't have
    // anything to do.
    return decl;
  }
  if (decl->type_arity != orig_decl->type_arity) return decl;
  TypeExpr* orig_ty =
      ctype::reify_univars(env, newgenty(tconstr(p, orig_decl->type_params, make<MemoRef>(mnil()))));
  TypeExpr* new_ty =
      ctype::reify_univars(env, newgenty(tconstr(Path::pident(id), decl->type_params, make<MemoRef>(mnil()))));
  env::t env2 = env::add_type(false, id, decl, env);
  try {
    ctype::mcomp(env2, orig_ty, new_ty);
  } catch (const ctype::Incompatible&) {
    // The current declaration is not compatible with the one we got from
    // the signature.
    return decl;
  }
  TypeExpr* orig_ty2 = newgenty(tconstr(p, decl->type_params, make<MemoRef>(mnil())));
  auto* r = make<TypeDeclaration>(*decl);
  r->type_manifest = orig_ty2;
  r->manifest_obj.reset();  // a new Some block
  return r;
}

const ModuleType* enrich_modtype(env::t env, Path::t p, const ModuleType* mty) {
  if (mty->kind != MK::Mty_signature) return mty;
  std::vector<const SignatureItem*> out;
  for (auto* it : mty->sign) {
    if (it->kind == SK::Sig_type) {
      auto* n = make<SignatureItem>(*it);
      n->type = enrich_typedecl(env, Path::pdot(p, ident::name(it->id)), it->id, it->type);
      out.push_back(n);
    } else if (it->kind == SK::Sig_module) {
      auto* md = make<ModuleDeclaration>(*it->md);
      md->md_type = enrich_modtype(env, Path::pdot(p, ident::name(it->id)), it->md->md_type);
      auto* n = make<SignatureItem>(*it);
      n->md = md;
      out.push_back(n);
    } else {
      out.push_back(it);
    }
  }
  auto* r = make<ModuleType>(*mty);
  r->sign = slice(out);
  return r;
}

static std::vector<Path::t> type_paths_sig(env::t env, Path::t p, Signature sg, std::size_t k);
std::vector<Path::t> type_paths(env::t env, Path::t p, const ModuleType* mty) {
  const ModuleType* m = scrape(env, mty);
  if (m->kind == MK::Mty_signature) return type_paths_sig(env, p, m->sign, 0);
  return {};
}
static std::vector<Path::t> type_paths_sig(env::t env, Path::t p, Signature sg, std::size_t k) {
  for (; k < sg.size(); ++k) {
    const SignatureItem* it = sg[k];
    switch (it->kind) {
      case SK::Sig_type: {
        std::vector<Path::t> r{Path::pdot(p, ident::name(it->id))};
        auto rest = type_paths_sig(env, p, sg, k + 1);
        r.insert(r.end(), rest.begin(), rest.end());
        return r;
      }
      case SK::Sig_module: {
        // type_paths env (Pdot ..) md.md_type @ type_paths_sig (Env.add_module_declaration ..) p rem:
        // right to left
        auto rest = type_paths_sig(env::add_module_declaration(false, it->id, it->presence, it->md, env), p, sg, k + 1);
        auto r = type_paths(env, Path::pdot(p, ident::name(it->id)), it->md->md_type);
        r.insert(r.end(), rest.begin(), rest.end());
        return r;
      }
      case SK::Sig_modtype: env = env::add_modtype(it->id, it->mtd, env); break;
      default: break;
    }
  }
  return {};
}

static bool no_code_needed_mod(env::t env, ModulePresence pres, const ModuleType* mty) {
  if (pres == ModulePresence::Mp_absent) return true;
  const ModuleType* m = scrape(env, mty);
  if (m->kind == MK::Mty_signature) return no_code_needed_sig(env, m->sign);
  return false;
}
bool no_code_needed_sig(env::t env, Signature sg) {
  for (auto* it : sg) {
    switch (it->kind) {
      case SK::Sig_value:
        if (it->value->val_kind.kind != ValueKind::Kind::Val_prim) return false;
        break;
      case SK::Sig_module:
        if (!no_code_needed_mod(env, it->presence, it->md->md_type)) return false;
        env = env::add_module_declaration(false, it->id, it->presence, it->md, env);
        break;
      case SK::Sig_type:
      case SK::Sig_modtype:
      case SK::Sig_class_type: break;
      case SK::Sig_typext:
      case SK::Sig_class: return false;
    }
  }
  return true;
}
bool no_code_needed(env::t env, const ModuleType* mty) {
  return no_code_needed_mod(env, ModulePresence::Mp_present, mty);
}

// Check whether a module type may return types
namespace {
struct ExitC {};
void contains_type_(env::t env, const ModuleType* mty);
void contains_type_sig(env::t env, Signature sg) {
  for (auto* it : sg) {
    switch (it->kind) {
      case SK::Sig_type:
        if (!it->type->type_manifest ||
            (it->type->type_kind->kind == TypeKind::Kind::Type_abstract &&
             it->type->type_private == PrivateFlag::Private))
          throw ExitC{};
        break;
      case SK::Sig_modtype: throw ExitC{};
      case SK::Sig_typext:
        // We consider that extension constructors with an inlined record
        // create a type (the inlined record).
        if (it->ext->ext_args.kind == ConstructorArguments::Kind::Cstr_record) throw ExitC{};
        break;
      case SK::Sig_module: contains_type_(env, it->md->md_type); break;
      default: break;
    }
  }
}
void contains_type_(env::t env, const ModuleType* mty) {
  switch (mty->kind) {
    case MK::Mty_ident: {
      const ModtypeDeclaration* d;
      try {
        d = env::find_modtype(mty->path, env);
      } catch (const env::NotFound&) {
        throw ExitC{};
      }
      if (!d->mtd_type) throw ExitC{};  // PR#6427
      contains_type_(env, d->mtd_type);
      return;
    }
    case MK::Mty_signature: contains_type_sig(env, mty->sign); return;
    case MK::Mty_functor: contains_type_(env, mty->res); return;
    case MK::Mty_alias: return;
  }
}
}  // namespace
bool contains_type(env::t env, const ModuleType* mty) {
  try {
    contains_type_(env, mty);
    return false;
  } catch (const ExitC&) {
    return true;
  }
}

// ---- remove module aliases from a signature ----------------------------------------------
namespace {
using PathSet = std::vector<Path::t>;  // Path.Set: only membership / union matter
void path_set_add(PathSet& s, Path::t p) {
  for (auto* q : s)
    if (path::compare(q, p) == 0) return;
  s.push_back(p);
}
PathSet get_prefixes(Path::t p) {
  PathSet s;
  while (p->kind != Path::Kind::Pident) {
    path_set_add(s, p->p1);
    p = p->p1;
  }
  return s;
}
PathSet get_arg_paths(Path::t p) {
  switch (p->kind) {
    case Path::Kind::Pident: return {};
    case Path::Kind::Pdot:
    case Path::Kind::Pextra_ty: return get_arg_paths(p->p1);
    case Path::Kind::Papply: {
      PathSet s{p->p2};
      for (auto* q : get_prefixes(p->p2)) path_set_add(s, q);
      for (auto* q : get_arg_paths(p->p1)) path_set_add(s, q);
      for (auto* q : get_arg_paths(p->p2)) path_set_add(s, q);
      return s;
    }
  }
  return {};
}
using PathIdMap = PathMap<Ident::t>;
Path::t rollback_path(const PathIdMap& subst, Path::t p) {
  if (const Ident::t* id = subst.find_opt(p)) return Path::pident(*id);
  switch (p->kind) {
    case Path::Kind::Pident:
    case Path::Kind::Papply: return p;
    case Path::Kind::Pdot: {
      Path::t p1 = rollback_path(subst, p->p1);
      if (path::same(p->p1, p1)) return p;
      return rollback_path(subst, Path::pdot(p1, p->s));
    }
    case Path::Kind::Pextra_ty: {
      Path::t p1 = rollback_path(subst, p->p1);
      if (path::same(p->p1, p1)) return p;
      return rollback_path(subst, Path::pextra_ty(p1, p->extra, p->s));
    }
  }
  return p;
}
void collect_ids(const PathIdMap& subst, const ident::Tbl<Path::t>& bindings, Path::t p,
                 std::vector<Ident::t>& out) {
  Path::t r = rollback_path(subst, p);
  if (r->kind != Path::Kind::Pident) return;
  if (const Path::t* q = bindings.find_same_opt(r->id)) collect_ids(subst, bindings, *q, out);
  out.push_back(r->id);
}
std::vector<Ident::t> collect_arg_paths(const ModuleType* mty) {
  PathSet paths;
  PathIdMap subst;
  ident::Tbl<Path::t> bindings;
  with_type_mark([&](TypeMark& mark) {
    TypeIterators super = type_iterators(mark);
    TypeIterators it = super;
    it.it_path = [&](Path::t p) {
      for (auto* q : get_arg_paths(p)) path_set_add(paths, q);
    };
    it.it_signature_item = [&, super](TypeIterators& self, const SignatureItem* si) {
      super.it_signature_item(self, si);
      if (si->kind == SK::Sig_module) {
        const ModuleType* m = si->md->md_type;
        if (m->kind == MK::Mty_alias) {
          bindings = bindings.add(si->id, m->path);
        } else if (m->kind == MK::Mty_signature) {
          for (auto* it2 : m->sign)
            if (it2->kind == SK::Sig_module)
              subst = subst.add(Path::pdot(Path::pident(si->id), ident::name(it2->id)), it2->id);
        }
      }
    };
    it.it_module_type(it, mty);
  });
  std::vector<Ident::t> out;
  for (auto* p : paths) collect_ids(subst, bindings, p, out);
  return out;
}

struct RemoveAliasArgs {
  bool modified;
  std::function<bool(Ident::t, Path::t)> exclude;
  std::function<const ModuleType*(env::t, const ModuleType*)> scrape;
};
Signature remove_aliases_sig(env::t env, RemoveAliasArgs& args, Signature sg);
std::pair<ModulePresence, const ModuleType*> remove_aliases_mty(env::t env, RemoveAliasArgs& args,
                                                                ModulePresence pres, const ModuleType* mty) {
  RemoveAliasArgs args2{false, args.exclude, args.scrape};
  std::pair<ModulePresence, const ModuleType*> res;
  const ModuleType* m = args.scrape(env, mty);
  if (m->kind == MK::Mty_signature) {
    auto* r = make<ModuleType>(*m);
    r->sign = remove_aliases_sig(env, args2, m->sign);
    res = {ModulePresence::Mp_present, r};
  } else if (m->kind == MK::Mty_alias) {
    const ModuleType* m2 = env::scrape_alias(env, m);
    // mty' = mty: scrape_alias returns its argument when it cannot scrape
    bool same = m2 == m || (m2->kind == MK::Mty_alias && path::same(m2->path, m->path));
    if (same) {
      res = {pres, m};
    } else {
      args2.modified = true;
      res = remove_aliases_mty(env, args2, ModulePresence::Mp_present, m2);
    }
  } else {
    res = {ModulePresence::Mp_present, m};
  }
  if (args2.modified) {
    args.modified = true;
    return res;
  }
  return {pres, mty};
}
Signature remove_aliases_sig(env::t env, RemoveAliasArgs& args, Signature sg) {
  std::vector<const SignatureItem*> out;
  for (auto* it : sg) {
    if (it->kind == SK::Sig_module) {
      ModulePresence pres = it->presence;
      const ModuleType* mty = it->md->md_type;
      if (!(mty->kind == MK::Mty_alias && args.exclude(it->id, mty->path)))
        std::tie(pres, mty) = remove_aliases_mty(env, args, pres, mty);
      out.push_back(with_md(it, pres, mty));
      env = env::add_module(it->id, pres, mty, env);
    } else if (it->kind == SK::Sig_modtype) {
      out.push_back(it);
      env = env::add_modtype(it->id, it->mtd, env);
    } else {
      out.push_back(it);
    }
  }
  return slice(out);
}
}  // namespace

const ModuleType* scrape_for_functor_arg(env::t env, const ModuleType* mty) {
  RemoveAliasArgs args{false,
                       [env](Ident::t, Path::t p) {
                         try {
                           env::find_module(p, env);
                           return true;
                         } catch (const env::NotFound&) {
                           return false;
                         }
                       },
                       [](env::t e, const ModuleType* m) { return scrape(e, m); }};
  return remove_aliases_mty(env, args, ModulePresence::Mp_present, mty).second;
}

const ModuleType* scrape_for_type_of(bool remove_aliases, env::t env, const ModuleType* mty) {
  if (remove_aliases) {
    std::vector<Ident::t> excl = collect_arg_paths(mty);
    RemoveAliasArgs args{false,
                         [excl](Ident::t id, Path::t) {
                           for (auto* e : excl)
                             if (ident::compare(e, id) == 0) return true;
                           return false;
                         },
                         [](env::t, const ModuleType* m) { return m; }};
    return remove_aliases_mty(env, args, ModulePresence::Mp_present, mty).second;
  }
  return scrape_for_type_of_(env, ModulePresence::Mp_present, mty).second;
}

// Lower non-generalizable type variables
void lower_nongen(long nglev, const ModuleType* mty) {
  with_type_mark([&](TypeMark& mark) {
    TypeIterators super = type_iterators(mark);
    TypeIterators it = super;
    it.it_do_type_expr = [nglev, super](TypeIterators& self, TypeExpr* ty) {
      if (get_desc(ty)->kind == DescKind::Tvar) {
        long level = get_level(ty);
        if (level < generic_level && level > nglev) set_level(ty, nglev);
      } else {
        super.it_do_type_expr(self, ty);
      }
    };
    it.it_module_type(it, mty);
  });
}

}  // namespace cppcaml::typing::mtype
