// Port of typing/typemod.ml, part 1: utilities (signature extraction, opens,
// recursive-module type checks, deep-substitution checks, well-formedness),
// the Merge module (`with` constraints), the approximation of module types
// for recursive modules, and Signature_names (name uniqueness and hiding).
#include <set>

#include "typemod_internal.hpp"

namespace cppcaml::typing::typemod {

using namespace types;
using pt::as;
using SK = SignatureItem::Kind;

[[noreturn]] void raise_error(const Error& e) { throw e; }

Path::t path_concat(Ident::t head, Path::t p) {
  switch (p->kind) {
    case Path::Kind::Pident: return Path::pdot(Path::pident(head), ident::name(p->id));
    case Path::Kind::Pdot: return Path::pdot(path_concat(head, p->p1), p->s);
    case Path::Kind::Papply: throw std::logic_error("path_concat");
    case Path::Kind::Pextra_ty: return Path::pextra_ty(path_concat(head, p->p1), p->extra, p->s);
  }
  throw std::logic_error("path_concat");
}

// Extract a signature from a module type
Signature extract_sig(env::t env, const Location& loc, const ModuleType* mty) {
  const ModuleType* m = env::scrape_alias(env, mty);
  if (m->kind == ModuleType::Kind::Mty_signature) return m->sign;
  if (m->kind == ModuleType::Kind::Mty_alias) {
    Error e = err(loc, env, EK::Cannot_scrape_alias);
    e.path = m->path;
    raise_error(e);
  }
  raise_error(err(loc, env, EK::Signature_expected));
}
Signature extract_sig_open(env::t env, const Location& loc, const ModuleType* mty) {
  const ModuleType* m = env::scrape_alias(env, mty);
  if (m->kind == ModuleType::Kind::Mty_signature) return m->sign;
  if (m->kind == ModuleType::Kind::Mty_alias) {
    Error e = err(loc, env, EK::Cannot_scrape_alias);
    e.path = m->path;
    raise_error(e);
  }
  Error e = err(loc, env, EK::Structure_expected);
  e.mty = m;
  raise_error(e);
}

// Compute the environment after opening a module
std::pair<Path::t, env::t> type_open_(std::shared_ptr<bool> used_slot, bool toplevel, OverrideFlag ovf, env::t env, const Location& loc,
                                      const pt::LidLoc& lid) {
  Path::t path = env::lookup_module_path(true, lid.loc, true, lid.txt, env);
  env::OpenResult r = env::open_signature(ovf, path, env, loc, toplevel, used_slot);
  if (r.kind == env::OpenResult::Kind::Ok) return {path, r.env};
  const ModuleDeclaration* md = env::find_module(path, env);
  extract_sig_open(env, lid.loc, md->md_type);
  throw std::logic_error("type_open_");
}

std::pair<const tt::OpenDescription*, env::t> type_open_descr(std::shared_ptr<bool> used_slot, bool toplevel, env::t env,
                                                              const pt::OpenDescription* sod) {
  auto [path, newenv] = builtin_attributes::warning_scope(sod->popen_attributes, [&] {
    return type_open_(used_slot, toplevel, sod->popen_override, env, sod->popen_loc, sod->popen_expr);
  });
  auto* od = make<tt::OpenDescription>(tt::PathLid{path, sod->popen_expr}, Signature{}, sod->popen_override, newenv,
                                       sod->popen_loc, sod->popen_attributes);
  return {od, newenv};
}

// Forward declaration, to be filled in by type_module_type_of
std::function<std::pair<const tt::ModuleExpr*, const ModuleType*>(env::t, const pt::ModuleExpr*)>
    type_module_type_of_fwd;

// ---- builders -------------------------------------------------------------------------------
const SignatureItem* sig_value(Ident::t id, const ValueDescription* v, Visibility vis) {
  auto* it = make<SignatureItem>();
  it->kind = SK::Sig_value;
  it->id = id;
  it->value = v;
  it->vis = vis;
  return it;
}
const SignatureItem* sig_type(Ident::t id, const TypeDeclaration* d, RecStatus rs, Visibility vis) {
  auto* it = make<SignatureItem>();
  it->kind = SK::Sig_type;
  it->id = id;
  it->type = d;
  it->rec = rs;
  it->vis = vis;
  return it;
}
const SignatureItem* sig_typext(Ident::t id, const ExtensionConstructor* e, ExtStatus es, Visibility vis) {
  auto* it = make<SignatureItem>();
  it->kind = SK::Sig_typext;
  it->id = id;
  it->ext = e;
  it->ext_status = es;
  it->vis = vis;
  return it;
}
const SignatureItem* sig_module(Ident::t id, ModulePresence pres, const ModuleDeclaration* md, RecStatus rs,
                                Visibility vis) {
  auto* it = make<SignatureItem>();
  it->kind = SK::Sig_module;
  it->id = id;
  it->presence = pres;
  it->md = md;
  it->rec = rs;
  it->vis = vis;
  return it;
}
const SignatureItem* sig_modtype(Ident::t id, const ModtypeDeclaration* mtd, Visibility vis) {
  auto* it = make<SignatureItem>();
  it->kind = SK::Sig_modtype;
  it->id = id;
  it->mtd = mtd;
  it->vis = vis;
  return it;
}
const SignatureItem* sig_class(Ident::t id, const ClassDeclaration* c, RecStatus rs, Visibility vis) {
  auto* it = make<SignatureItem>();
  it->kind = SK::Sig_class;
  it->id = id;
  it->cls = c;
  it->rec = rs;
  it->vis = vis;
  return it;
}
const SignatureItem* sig_class_type(Ident::t id, const ClassTypeDeclaration* c, RecStatus rs, Visibility vis) {
  auto* it = make<SignatureItem>();
  it->kind = SK::Sig_class_type;
  it->id = id;
  it->clty = c;
  it->rec = rs;
  it->vis = vis;
  return it;
}
const ModuleType* mty_signature(Signature sg) {
  auto* m = make<ModuleType>();
  m->kind = ModuleType::Kind::Mty_signature;
  m->sign = sg;
  return m;
}
const ModuleType* mty_ident(Path::t p) {
  auto* m = make<ModuleType>();
  m->kind = ModuleType::Kind::Mty_ident;
  m->path = p;
  return m;
}
const ModuleType* mty_alias(Path::t p) {
  auto* m = make<ModuleType>();
  m->kind = ModuleType::Kind::Mty_alias;
  m->path = p;
  return m;
}
const ModuleType* mty_functor(const FunctorParameter& param, const ModuleType* res) {
  auto* m = make<ModuleType>();
  m->kind = ModuleType::Kind::Mty_functor;
  m->param = param;
  m->res = res;
  return m;
}

Classified classify_signature_item(const SignatureItem* it) {
  switch (it->kind) {
    case SK::Sig_value: return {SigComponentKind::Value, it->id, it->value->val_loc};
    case SK::Sig_type: return {SigComponentKind::Type, it->id, it->type->type_loc};
    case SK::Sig_typext: return {SigComponentKind::Extension_constructor, it->id, it->ext->ext_loc};
    case SK::Sig_module: return {SigComponentKind::Module, it->id, it->md->md_loc};
    case SK::Sig_modtype: return {SigComponentKind::Module_type, it->id, it->mtd->mtd_loc};
    case SK::Sig_class: return {SigComponentKind::Class, it->id, it->cls->cty_loc};
    case SK::Sig_class_type: return {SigComponentKind::Class_type, it->id, it->clty->clty_loc};
  }
  throw std::logic_error("classify_signature_item");
}

// ---- additional validity checks on type definitions arising from recursive modules ------
static const TypeDeclaration* abstractify_type(const TypeDeclaration* ty) {
  long arity = ty->type_arity;
  auto* d = make<TypeDeclaration>(*ty);
  std::vector<TypeExpr*> params;
  for (std::size_t k = 0; k < ty->type_params.size(); ++k) params.push_back(btype::newgenvar());
  d->type_params = slice(params);
  d->type_kind = TYPE_ABSTRACT_LIT(Rec_check_regularity);
  d->type_manifest = nullptr;
  d->type_variance = slice(variance::unknown_signature(false, arity));
  d->type_separability =
      slice(std::vector<Separability>(static_cast<std::size_t>(arity > 0 ? arity : 0), Separability::Deepsep));
  d->type_is_newtype = false;
  d->type_expansion_scope = btype::lowest_level;
  d->type_immediate = TypeImmediacy::Unknown;
  d->type_unboxed_default = false;
  return d;
}

struct PathLess {
  bool operator()(Path::t a, Path::t b) const { return path::compare(a, b) < 0; }
};

void check_recmod_typedecls(env::t env, const std::vector<std::pair<Ident::t, const ModuleDeclaration*>>& decls) {
  std::vector<Ident::t> recmod_ids;
  for (auto& d : decls) recmod_ids.push_back(d.first);
  std::map<Path::t, const TypeDeclaration*, PathLess> type_to_abstract;
  for (auto& [id, md] : decls)
    for (Path::t path : mtype::type_paths(env, Path::pident(id), md->md_type)) {
      const TypeDeclaration* ty = env::find_type(path, env);
      type_to_abstract[path] = abstractify_type(ty);
    }
  env::t abs_env = env;
  for (auto& [path, ty] : type_to_abstract) abs_env = env::add_local_constraint(path, ty, abs_env);
  for (auto& [id, md] : decls)
    for (Path::t path : mtype::type_paths(env, Path::pident(id), md->md_type))
      typedecl::check_recmod_typedecl(abs_env, env, md->md_loc, recmod_ids, path, env::find_type(path, env));
}

// Merge one "with" constraint in a signature
static void check_type_decl(env::t env, Signature sg, const Location& loc, Ident::t id, Ident::t row_id,
                            const TypeDeclaration* newdecl, const TypeDeclaration* decl) {
  Ident::t fresh_id = ident::rename(id);
  Path::t path = Path::pident(fresh_id);
  subst::t sub = subst::add_type(id, path, subst::identity());
  Ident::t fresh_row_id = nullptr;
  if (row_id) {
    fresh_row_id = ident::rename(row_id);
    sub = subst::add_type(row_id, Path::pident(fresh_id), sub);
  }
  newdecl = subst::type_declaration(sub, newdecl);
  decl = subst::type_declaration(sub, decl);
  std::vector<const SignatureItem*> sg2;
  for (auto* it : sg) sg2.push_back(subst::signature_item(subst::Scoping::keep(), sub, it));
  env = env::add_type(false, fresh_id, newdecl, env);
  if (fresh_row_id) env = env::add_type(false, fresh_row_id, newdecl, env);
  env = env::add_signature(slice(sg2), env);
  env::t abs_env = env::add_local_constraint(path, abstractify_type(newdecl), env);
  // The type declarations input to the inclusion check must be
  // well-founded; otherwise the inclusion check may loop.
  typedecl::check_well_founded_decl(abs_env, env, [&](Path::t p) { return path::same(path, p); }, loc, path, newdecl);
  includemod::type_declarations(loc, env, true, fresh_id, newdecl, decl);
  typedecl::check_coherence(env, loc, path, newdecl);
}

static variance::t make_variance(bool p, bool n, bool i) {
  using F = variance::F;
  return variance::set_if(p, F::May_pos, variance::set_if(n, F::May_neg, variance::set_if(i, F::Inj, variance::null)));
}

static void iter_path_apply(Path::t p, const std::function<void(Path::t, Path::t)>& f) {
  switch (p->kind) {
    case Path::Kind::Pident: return;
    case Path::Kind::Pdot: iter_path_apply(p->p1, f); return;
    case Path::Kind::Papply:
      iter_path_apply(p->p1, f);
      iter_path_apply(p->p2, f);
      f(p->p1, p->p2);  // after recursing, so we know both paths are well typed
      return;
    case Path::Kind::Pextra_ty: throw std::logic_error("iter_path_apply");
  }
}

// Checks if a flat path [path] admits [prefix] as a prefix; [strict]
// rejects equal paths
static bool path_is_prefix(bool strict, Path::t path, Path::t prefix) {
  auto f1 = path::flatten(path);
  auto f2 = path::flatten(prefix);
  if (!f1 || !f2) return false;
  if (!ident::same(f1->first, f2->first)) return false;
  const auto& l = f1->second;
  const auto& pre = f2->second;
  if (pre.size() > l.size()) return false;
  for (std::size_t k = 0; k < pre.size(); ++k)
    if (l[k] != pre[k]) return false;
  if (pre.size() == l.size()) return !strict;
  return true;
}

// ---- iterators with an environment ----------------------------------------------------------
namespace {
struct LazyEnv {
  std::function<env::t()> thunk;
  env::t val = nullptr;
  bool forced = false;
  env::t force() {
    if (!forced) {
      val = thunk();
      forced = true;
    }
    return val;
  }
};
using LazyEnvP = std::shared_ptr<LazyEnv>;
LazyEnvP lazy_value(env::t e) {
  auto l = std::make_shared<LazyEnv>();
  l->val = e;
  l->forced = true;
  return l;
}
LazyEnvP lazy_of(std::function<env::t()> f) {
  auto l = std::make_shared<LazyEnv>();
  l->thunk = std::move(f);
  return l;
}
using EnvRef = std::shared_ptr<LazyEnvP>;

std::pair<EnvRef, btype::TypeIterators> iterator_with_env(const btype::TypeIterators& super, env::t env0) {
  EnvRef env = std::make_shared<LazyEnvP>(lazy_value(env0));
  btype::TypeIterators it = super;
  it.it_signature = [env, super](btype::TypeIterators& self, Signature sg) {
    // add all items to the env before recursing down, to handle recursive
    // definitions
    LazyEnvP env_before = *env;
    *env = lazy_of([env_before, sg] { return env::add_signature(sg, env_before->force()); });
    super.it_signature(self, sg);
    *env = env_before;
  };
  it.it_module_type = [env, super](btype::TypeIterators& self, const ModuleType* mty) {
    if (mty->kind != ModuleType::Kind::Mty_functor) {
      super.it_module_type(self, mty);
      return;
    }
    LazyEnvP env_before = *env;
    if (!mty->param.is_unit) {
      self.it_module_type(self, mty->param.mty);
      if (Ident::t id = mty->param.id) {
        const ModuleType* mty_arg = mty->param.mty;
        *env = lazy_of([env_before, id, mty_arg] {
          return env::add_module(id, ModulePresence::Mp_present, mty_arg, env_before->force(), true);
        });
      }
    }
    self.it_module_type(self, mty->res);
    *env = env_before;
  };
  return {env, it};
}
}  // namespace

static std::optional<includemod::Explanation> retype_applicative_functor_type(const Location& loc, env::t env,
                                                                              Path::t funct, Path::t arg) {
  const ModuleType* mty_functor = env::find_module(funct, env)->md_type;
  const ModuleType* mty_arg = env::find_module(arg, env)->md_type;
  const ModuleType* m = env::scrape_alias(env, mty_functor);
  if (!(m->kind == ModuleType::Kind::Mty_functor && !m->param.is_unit))
    throw std::logic_error("retype_applicative_functor_type");  // could trigger due to MPR#7611
  return includemod::check_modtype_inclusion(loc, env, mty_arg, arg, m->param.mty);
}

// When doing a deep destructive substitution with type M.N.t := .., check
// the uses of M and M.N other than component extraction (applicative
// functor types and aliases).
static btype::TypeIterators check_usage_of_path_of_substituted_item(const std::vector<Path::t>& paths,
                                                                    const Location& loc, const pt::LidLoc& lid,
                                                                    EnvRef env, const btype::TypeIterators& super) {
  if (paths.empty()) throw std::logic_error("check_usage_of_path_of_substituted_item");
  if (paths.size() == 1) return super;  // Shallow substitution, nothing to check
  // The last item is the one that's removed.
  std::vector<Path::t> rest(paths.rbegin() + 1, paths.rend());
  if (paths.back()->kind != Path::Kind::Pident) throw std::logic_error("check_usage_of_path_of_substituted_item");
  btype::TypeIterators it = super;
  it.it_signature_item = [rest, loc, lid, env, super](btype::TypeIterators& self, const SignatureItem* item) {
    if (item->kind == SK::Sig_module && item->md->md_type->kind == ModuleType::Kind::Mty_alias) {
      Path::t aliased_path = item->md->md_type->path;
      for (Path::t p : rest)
        if (path_is_prefix(true, p, aliased_path)) {
          Error e = err(loc, (*env)->force(), EK::With_changes_module_alias);
          e.lid = lid.txt;
          e.id = item->id;
          e.path = aliased_path;
          raise_error(e);
        }
    }
    super.it_signature_item(self, item);
  };
  it.it_path = [rest, loc, lid, env](Path::t referenced_path) {
    iter_path_apply(referenced_path, [&](Path::t funct, Path::t arg) {
      bool affected = false;
      for (Path::t p : rest) affected = affected || path_is_prefix(true, p, arg);
      if (!affected) return;
      env::t e2 = (*env)->force();
      if (auto explanation = retype_applicative_functor_type(loc, e2, funct, arg)) {
        Error e = err(loc, e2, EK::With_makes_applicative_functor_ill_typed);
        e.lid = lid.txt;
        e.path = referenced_path;
        e.explanation = explanation;
        raise_error(e);
      }
    });
  };
  return it;
}

// When doing destructive module substitutions [with module X = P] with a
// non-aliasable [P], module aliases to [X] or any suffix would become
// invalid.
static btype::TypeIterators check_invalid_aliases(const std::vector<Path::t>& paths, const Location& loc, EnvRef env,
                                                  Path::t invalid_alias, const btype::TypeIterators& super) {
  if (!invalid_alias) return super;
  btype::TypeIterators it = super;
  it.it_signature_item = [paths, loc, env, invalid_alias, super](btype::TypeIterators& self,
                                                                 const SignatureItem* item) {
    if (item->kind == SK::Sig_module && item->md->md_type->kind == ModuleType::Kind::Mty_alias) {
      Path::t aliased_path = item->md->md_type->path;
      bool invalid = false;
      for (Path::t p : paths) invalid = invalid || path_is_prefix(false, aliased_path, p);
      if (invalid) {
        Error e = err(loc, (*env)->force(), EK::With_creates_invalid_aliases);
        e.id = item->id;
        e.path = aliased_path;
        e.path2 = invalid_alias;
        raise_error(e);
      }
    }
    super.it_signature_item(self, item);
  };
  return it;
}

// the effect of destructive substitutions and the introduction of invalid
// aliases
static void check_usage_after_substitution(env::t env0, const Location& loc, const pt::LidLoc& lid,
                                           const std::vector<Path::t>& paths, Path::t invalid_alias, Signature sg) {
  if (paths.size() == 1 && !invalid_alias) return;
  with_type_mark([&](TypeMark& mark) {
    auto [env, base_iterator] = iterator_with_env(btype::type_iterators(mark), env0);
    btype::TypeIterators iterator = check_invalid_aliases(
        paths, loc, env, invalid_alias, check_usage_of_path_of_substituted_item(paths, loc, lid, env, base_iterator));
    iterator.it_signature(iterator, sg);
  });
}

// After substitution one also needs to re-check the well-foundedness of
// type declarations in recursive modules
static std::pair<std::vector<std::pair<Ident::t, const ModuleDeclaration*>>, Signature> extract_next_modules(
    Signature sg) {
  std::vector<std::pair<Ident::t, const ModuleDeclaration*>> l;
  std::size_t k = 0;
  while (k < sg.size() && sg[k]->kind == SK::Sig_module && sg[k]->rec == RecStatus::Trec_next) {
    l.push_back({sg[k]->id, sg[k]->md});
    ++k;
  }
  return {l, Signature(sg.begin() + k, sg.size() - k)};
}

void check_well_formed_module(env::t env0, const Location& loc, const std::string& context, const ModuleType* mty) {
  auto [envref, super] = iterator_with_env(btype::type_iterators_without_type_expr(), env0);
  std::function<void(LazyEnvP, Signature)> check_signature = [&](LazyEnvP env, Signature sg) {
    while (!sg.empty()) {
      const SignatureItem* it = sg[0];
      if (it->kind == SK::Sig_module && it->rec == RecStatus::Trec_first) {
        auto [id_mty_l, rem] = extract_next_modules(Signature(sg.begin() + 1, sg.size() - 1));
        try {
          env::t forced_env = env->force();
          std::vector<std::pair<Ident::t, const ModuleDeclaration*>> decls{{it->id, it->md}};
          decls.insert(decls.end(), id_mty_l.begin(), id_mty_l.end());
          check_recmod_typedecls(forced_env, decls);
        } catch (const typedecl::Error& te) {
          Error e = err(loc, env->force(), EK::Badly_formed_signature);
          e.name = context;
          e.typedecl_error = te;
          raise_error(e);
        }
        sg = rem;
        continue;
      }
      sg = Signature(sg.begin() + 1, sg.size() - 1);
    }
  };
  btype::TypeIterators iterator = super;
  iterator.it_signature = [envref = envref, super = super, &check_signature](btype::TypeIterators& self, Signature sg) {
    LazyEnvP env_before = *envref;
    LazyEnvP env = lazy_of([env_before, sg] { return env::add_signature(sg, env_before->force()); });
    check_signature(env, sg);
    super.it_signature(self, sg);
  };
  iterator.it_module_type(iterator, mty);
}

std::optional<pt::LidLoc> type_decl_is_alias(const pt::TypeDeclaration* sdecl) {
  // (assuming no explicit constraint)
  if (!sdecl->ptype_manifest) return std::nullopt;
  auto* c = as<pt::Ptyp_constr>(sdecl->ptype_manifest->ptyp_desc);
  if (!c || c->args.size() != sdecl->ptype_params.size()) return std::nullopt;
  for (std::size_t k = 0; k < c->args.size(); ++k) {
    auto* x = as<pt::Ptyp_var>(c->args[k]->ptyp_desc);
    auto* y = as<pt::Ptyp_var>(sdecl->ptype_params[k].ty->ptyp_desc);
    if (!(x && y && x->name == y->name)) return std::nullopt;
  }
  return c->lid;
}

bool params_are_constrained(Slice<TypeExpr*> l) {
  for (std::size_t k = 0; k < l.size(); ++k) {
    TypeExpr* hd = l[k];
    if (get_desc(hd)->kind != DescKind::Tvar) return true;
    for (std::size_t j = k + 1; j < l.size(); ++j)
      if (l[j] == hd) return true;  // List.memq
  }
  return false;
}

// map_rec_type_with_row_types: the #row ghost types are Trec_not
std::vector<const SignatureItem*> map_rec_type_with_row_types(RecFlag rec_flag,
                                                              const std::vector<const tt::TTypeDeclaration*>& decls) {
  std::vector<const SignatureItem*> out;
  std::size_t k = 0;
  while (k < decls.size() && btype::is_row_name(ident::name(decls[k]->typ_id))) {
    out.push_back(sig_type(decls[k]->typ_id, decls[k]->typ_type, RecStatus::Trec_not, Visibility::Exported));
    ++k;
  }
  // map_rec_type ~rec_flag
  for (std::size_t j = k; j < decls.size(); ++j) {
    RecStatus rs = j == k ? (rec_flag == RecFlag::Recursive ? RecStatus::Trec_first : RecStatus::Trec_not)
                          : RecStatus::Trec_next;
    out.push_back(sig_type(decls[j]->typ_id, decls[j]->typ_type, rs, Visibility::Exported));
  }
  return out;
}

// ---- module Merge: signature constraints -----------------------------------------------
namespace merge {

namespace {
struct Info {  // (path, path :: paths, late_typedtree)
  Path::t path;
  std::vector<Path::t> paths;
  const tt::TTypeDeclaration* late;
};
using Patched = std::optional<std::pair<Info, signature_group::InPlacePatch>>;
using Patch = std::function<Patched(const SignatureItem* item, std::string_view s, env::t sig_env,
                                    Signature sg_for_env, Signature ghosts)>;

Patched return_payload(Signature ghosts, const SignatureItem* replace_by, const tt::TTypeDeclaration* late,
                       Path::t path, const std::vector<Path::t>& paths = {}) {
  std::vector<Path::t> ps{path};
  ps.insert(ps.end(), paths.begin(), paths.end());
  return std::make_pair(Info{path, ps, late}, signature_group::InPlacePatch{ghosts, replace_by});
}

struct RowSplit {
  std::vector<const SignatureItem*> before;  // reversed, as the OCaml accumulator
  Ident::t row_id;
  std::vector<const SignatureItem*> rest;
};
RowSplit split_row_id(std::string_view s, Signature ghosts) {
  std::string srow = std::string(s) + "#row";
  RowSplit r{{}, nullptr, {}};
  for (std::size_t k = 0; k < ghosts.size(); ++k) {
    const SignatureItem* a = ghosts[k];
    if (a->kind == SK::Sig_type && ident::name(a->id) == srow) {
      r.row_id = a->id;
      r.rest.assign(ghosts.begin() + k + 1, ghosts.end());
      return r;
    }
    r.before.insert(r.before.begin(), a);
  }
  return r;
}
// List.rev_append before rest
Signature rev_append(const std::vector<const SignatureItem*>& before, std::vector<const SignatureItem*> rest) {
  std::vector<const SignatureItem*> out(before.rbegin(), before.rend());
  out.insert(out.end(), rest.begin(), rest.end());
  return slice(out);
}

Signature unsafe_signature_subst(env::t initial_env, const Location& loc, Signature sg, subst::t sub) {
  // (the result is always freshened by the caller)
  try {
    return subst::signature(subst::Scoping::make_local(), sub, sg);
  } catch (const subst::ModuleTypePathSubstitutedAway& s) {
    Error e = err(loc, initial_env, EK::With_cannot_remove_packed_modtype);
    e.path = s.path;
    e.mty = s.mty;
    raise_error(e);
  }
}

using Replace = std::function<subst::t(subst::t, Path::t)>;
// Called after an item has been patched (rewritten or removed)
Signature post_process(bool approx, const Replace* replace, Path::t invalid_alias, const Location& loc,
                       const pt::LidLoc& lid, env::t env, const std::vector<Path::t>& paths, Signature sg) {
  if (replace) {
    // Check that the substitution would not make the signature ill-formed
    if (!approx) check_usage_after_substitution(env, loc, lid, paths, invalid_alias, sg);
    // Actually remove the identifiers
    subst::t sub = subst::change_locs(subst::identity(), loc);
    for (Path::t p : paths) sub = (*replace)(sub, p);
    sg = unsafe_signature_subst(env, loc, sg, sub);
  }
  // check that the resulting signature is still wellformed
  if (!approx) check_well_formed_module(env, loc, "this instantiated signature", mty_signature(sg));
  return sg;
}

struct Merged {
  Path::t path;
  std::vector<Path::t> paths;
  const tt::TTypeDeclaration* late;
  Signature sg;
};
Merged merge_signature(env::t initial_env, env::t env, Signature sg, const std::vector<std::string_view>& namelist,
                       std::size_t k, const Location& loc, const pt::LidLoc& lid, const Patch& patch, bool destructive);

// Main recursive knot to handle deep merges
Patched patch_deep_item(const Patch& patch, bool destructive, const std::vector<std::string_view>& namelist,
                        std::size_t k, env::t initial_env, env::t env, Signature outer_sg, const Location& loc,
                        const pt::LidLoc& lid, Signature ghosts, const SignatureItem* item) {
  // Shallow constraints : call the patch function
  if (k + 1 == namelist.size()) return patch(item, namelist[k], env, outer_sg, ghosts);
  // Deep constraints
  if (item->kind == SK::Sig_module && ident::name(item->id) == namelist[k]) {
    const ModuleDeclaration* md = item->md;
    env::t sig_env = env::add_signature(outer_sg, env);
    Signature sg = extract_sig(sig_env, loc, md->md_type);
    Merged m = merge_signature(initial_env, sig_env, sg, namelist, k + 1, loc, lid, patch, destructive);
    Path::t path = path_concat(item->id, m.path);
    if (md->md_type->kind == ModuleType::Kind::Mty_alias && !destructive)
      // Deep non-destructive substitutions inside aliases are checked, but
      // do not change the resulting signature
      return return_payload(ghosts, item, m.late, path);
    auto* new_md = make<ModuleDeclaration>(*md);
    new_md->md_type = mty_signature(m.sg);
    const SignatureItem* new_item = sig_module(item->id, ModulePresence::Mp_present, new_md, item->rec, item->vis);
    return return_payload(ghosts, new_item, m.late, path, m.paths);
  }
  return std::nullopt;
}

Merged merge_signature(env::t initial_env, env::t env, Signature sg, const std::vector<std::string_view>& namelist,
                       std::size_t k, const Location& loc, const pt::LidLoc& lid, const Patch& patch, bool destructive) {
  std::optional<std::pair<Info, Signature>> r;
  try {
    r = signature_group::replace_in_place<Info>(
        [&](Signature ghosts, const SignatureItem* item) {
          return patch_deep_item(patch, destructive, namelist, k, initial_env, env, sg, loc, lid, ghosts, item);
        },
        sg);
  } catch (const includemod::Error& ie) {
    Error e = err(loc, initial_env, EK::With_mismatch);
    e.lid = lid.txt;
    e.explanation = ie.expl;
    raise_error(e);
  }
  if (!r) {
    Error e = err(loc, initial_env, EK::With_no_component);
    e.lid = lid.txt;
    raise_error(e);
  }
  return {r->first.path, r->first.paths, r->first.late, r->second};
}

// Entry point for merging
Merged merge(const Patch& patch, bool destructive, env::t env, Signature sg, const Location& loc,
             const pt::LidLoc& lid) {
  std::vector<std::string_view> names = longident::flatten(lid.txt);
  return merge_signature(env, env, sg, names, 0, loc, lid, patch, destructive);
}
}  // namespace

// Type constraint [sg with type lid = sdecl]: the typedtree for [sdecl] is
// built at the point of the constrained item
TypeResult merge_type(bool destructive, env::t env, const Location& loc, Signature sg, const pt::LidLoc& lid,
                      const pt::TypeDeclaration* sdecl) {
  Patch patch = [&](const SignatureItem* item, std::string_view s, env::t sig_env0, Signature sg_for_env,
                    Signature ghosts) -> Patched {
    if (!(item->kind == SK::Sig_type && ident::name(item->id) == s)) return std::nullopt;
    Ident::t id = item->id;
    const TypeDeclaration* decl = item->type;
    RecStatus rs = item->rec;
    Visibility priv = item->vis;
    if (sdecl->ptype_kind.kind == pt::TypeKind::Kind::Ptype_abstract && typedecl::is_fixed_type(sdecl)) {
      long arity = static_cast<long>(sdecl->ptype_params.size());
      // record fields right to left: type_uid first
      Uid uid = uid::mk(env::get_current_unit());
      std::vector<variance::t> vars;
      for (auto& p : sdecl->ptype_params) {
        bool c = false, n = false;
        switch (p.variance) {
          case pt::Variance::Covariant: c = true; break;
          case pt::Variance::Contravariant: n = true; break;
          case pt::Variance::NoVariance: break;
          case pt::Variance::Bivariant: c = n = true; break;
        }
        vars.push_back(make_variance(!n, !c, p.injectivity == pt::Injectivity::Injective));
      }
      std::vector<TypeExpr*> params;
      for (std::size_t j = 0; j < sdecl->ptype_params.size(); ++j) params.push_back(btype::newgenvar());
      auto* decl_row = make<TypeDeclaration>();
      decl_row->type_params = slice(params);
      decl_row->type_arity = arity;
      decl_row->type_kind = TYPE_ABSTRACT_LIT(Definition);
      decl_row->type_private = PrivateFlag::Private;
      decl_row->type_manifest = nullptr;
      decl_row->type_variance = slice(vars);
      decl_row->type_separability =
          slice(std::vector<Separability>(static_cast<std::size_t>(arity), Separability::Deepsep));
      decl_row->type_loc = sdecl->ptype_loc;
      decl_row->type_is_newtype = false;
      decl_row->type_expansion_scope = btype::lowest_level;
      decl_row->type_immediate = TypeImmediacy::Unknown;
      decl_row->type_unboxed_default = false;
      decl_row->type_uid = uid;
      Ident::t id_row = Ident::create_local(zborrow(std::string(s) + "#row"));
      env::t initial_env = env::add_type(false, id_row, decl_row, env);
      env::t sig_env = env::add_signature(sg_for_env, sig_env0);
      const tt::TTypeDeclaration* tdecl =
          typedecl::transl_with_constraint(id, Path::pident(id_row), sig_env, decl, initial_env, sdecl);
      const TypeDeclaration* newdecl = tdecl->typ_type;
      RowSplit rsp = split_row_id(s, ghosts);
      check_type_decl(sig_env, sg_for_env, sdecl->ptype_loc, id, rsp.row_id, newdecl, decl);
      auto* decl_row2 = make<TypeDeclaration>(*decl_row);
      decl_row2->type_params = newdecl->type_params;
      RecStatus rs2 = rs == RecStatus::Trec_first ? RecStatus::Trec_not : rs;
      std::vector<const SignatureItem*> after{sig_type(id_row, decl_row2, rs2, priv)};
      after.insert(after.end(), rsp.rest.begin(), rsp.rest.end());
      Signature ghosts2 = rev_append(rsp.before, after);
      return return_payload(ghosts2, sig_type(id, newdecl, rs, priv), tdecl, Path::pident(id));
    }
    env::t sig_env = env::add_signature(sg_for_env, sig_env0);
    const tt::TTypeDeclaration* tdecl = typedecl::transl_with_constraint(id, nullptr, sig_env, decl, env, sdecl);
    const TypeDeclaration* newdecl = tdecl->typ_type;
    const Location& newloc = sdecl->ptype_loc;
    RowSplit rsp = split_row_id(s, ghosts);
    Signature ghosts2 = rev_append(rsp.before, rsp.rest);
    check_type_decl(sig_env, sg_for_env, newloc, id, rsp.row_id, newdecl, decl);
    const SignatureItem* item_opt = destructive ? nullptr : sig_type(id, newdecl, rs, priv);
    return return_payload(ghosts2, item_opt, tdecl, Path::pident(id));
  };
  // Merging
  Merged m = merge(patch, destructive, env, sg, loc, lid);
  // Post processing
  std::optional<Replace> replace;
  if (destructive) {
    if (auto alias = type_decl_is_alias(sdecl)) {
      // if the type is an alias of [lid], replace by the definition
      Path::t replacement;
      try {
        replacement = env::find_type_by_name(alias->txt, env).first;
      } catch (const env::NotFound&) {
        throw std::logic_error("merge_type: alias");
      }
      replace = [replacement](subst::t s, Path::t path) { return subst::unsafe::add_type_path(path, replacement, s); };
    } else {
      // if the type is not an alias, try to inline it
      Slice<TypeExpr*> params = m.late->typ_type->type_params;
      if (params_are_constrained(params)) raise_error(err(loc, env, EK::With_cannot_remove_constrained_type));
      TypeExpr* body = m.late->typ_type->type_manifest;
      if (!body) throw std::logic_error("Option.get");
      replace = [params, body](subst::t s, Path::t path) {
        return subst::unsafe::add_type_function(path, params, body, s);
      };
    }
  }
  Signature sg2 = post_process(false, replace ? &*replace : nullptr, nullptr, loc, lid, env, m.paths, m.sg);
  return {m.late, m.path, lid, sg2};
}

// Approximated type constraint [sg with type lid = _]
Signature merge_type_approx(bool destructive, env::t env, const Location& loc, Signature sg, const pt::LidLoc& lid) {
  Patch patch = [&](const SignatureItem* item, std::string_view s, env::t, Signature, Signature ghosts) -> Patched {
    if (item->kind == SK::Sig_type && ident::name(item->id) == s)
      // An identity patch is applied (non-destructive)
      return return_payload(ghosts, destructive ? nullptr : item, nullptr, Path::pident(item->id));
    return std::nullopt;
  };
  Merged m = merge(patch, destructive, env, sg, loc, lid);
  // No replacement: all type fields are made abstract anyway; the approx
  // flag disables any wellformedness checks.
  return post_process(true, nullptr, nullptr, loc, lid, env, m.paths, m.sg);
}

// Module constraint [sg with module lid = path]
Result merge_module(bool approx, bool destructive, env::t env, const Location& loc, Signature sg,
                    const pt::LidLoc& lid, const ModuleDeclaration* md2, Path::t path, bool remove_aliases) {
  bool aliasable = env::is_aliasable(path, env);
  Patch patch = [&](const SignatureItem* item, std::string_view s, env::t sig_env0, Signature sg_for_env,
                    Signature ghosts) -> Patched {
    if (!(item->kind == SK::Sig_module && ident::name(item->id) == s)) return std::nullopt;
    env::t sig_env = env::add_signature(sg_for_env, sig_env0);
    Path::t real_path = Path::pident(item->id);
    if (destructive) {
      // Inclusion check with the strengthened definition
      if (!approx) includemod::strengthened_module_decl(loc, aliasable, sig_env, true, md2, path, item->md);
      return return_payload(ghosts, nullptr, nullptr, real_path);
    }
    const ModuleType* mty = mtype::scrape_for_type_of(remove_aliases, sig_env, md2->md_type);
    auto* md3 = make<ModuleDeclaration>(*md2);
    md3->md_type = mty;
    const ModuleDeclaration* newmd = mtype::strengthen_decl(false, sig_env, md3, path);
    // Inclusion check with the original signature
    if (!approx) includemod::modtypes(loc, sig_env, true, newmd->md_type, item->md->md_type);
    return return_payload(ghosts, sig_module(item->id, item->presence, newmd, item->rec, item->vis), nullptr,
                          real_path);
  };
  Merged m = merge(patch, destructive, env, sg, loc, lid);
  std::optional<Replace> replace;
  if (destructive) replace = [path](subst::t s, Path::t p) { return subst::unsafe::add_module_path(p, path, s); };
  Path::t invalid_alias = aliasable ? nullptr : path;
  Signature sg2 = post_process(approx, replace ? &*replace : nullptr, invalid_alias, loc, lid, env, m.paths, m.sg);
  return {m.path, lid, sg2};
}

// Module type constraint [sg with module type lid = mty]
Result merge_modtype(bool approx, bool destructive, env::t env, const Location& loc, Signature sg,
                     const pt::LidLoc& lid, const ModuleType* mty) {
  Patch patch = [&](const SignatureItem* item, std::string_view s, env::t sig_env0, Signature sg_for_env,
                    Signature ghosts) -> Patched {
    if (!(item->kind == SK::Sig_modtype && ident::name(item->id) == s)) return std::nullopt;
    // Check for equivalence if the previous module type was not abstract
    // (the check is ignored in approximation mode)
    if (item->mtd->mtd_type && !approx) {
      env::t sig_env = env::add_signature(sg_for_env, sig_env0);
      includemod::check_modtype_equiv(loc, sig_env, item->id, item->mtd->mtd_type, mty);
    }
    const SignatureItem* new_item = nullptr;
    if (!destructive) {
      auto* mtd2 = make<ModtypeDeclaration>(mty, Attributes{}, loc, uid::mk(env::get_current_unit()));
      new_item = sig_modtype(item->id, mtd2, item->vis);
    }
    return return_payload(ghosts, new_item, nullptr, Path::pident(item->id));
  };
  Merged m = merge(patch, destructive, env, sg, loc, lid);
  std::optional<Replace> replace;
  if (destructive) replace = [mty](subst::t s, Path::t p) { return subst::unsafe::add_modtype_path(p, mty, s); };
  Signature sg2 = post_process(approx, replace ? &*replace : nullptr, nullptr, loc, lid, env, m.paths, m.sg);
  return {m.path, lid, sg2};
}

// Type constraints inside a first class module type
static Signature merge_package(env::t env, const Location& loc, Signature sg, const pt::LidLoc& lid,
                               const tt::CoreType* cty) {
  Patch patch = [&](const SignatureItem* item, std::string_view s, env::t sig_env, Signature sg_for_env,
                    Signature ghosts) -> Patched {
    if (!(item->kind == SK::Sig_type && ident::name(item->id) == s)) return std::nullopt;
    if (TypeExpr* ty = item->type->type_manifest) {
      Error e = err(loc, sig_env, EK::With_package_manifest);
      e.lid = lid.txt;
      e.ty = ty;
      raise_error(e);
    }
    const TypeDeclaration* tdecl = typedecl::transl_package_constraint(loc, sig_env, cty->ctyp_type);
    check_type_decl(sig_env, sg_for_env, loc, item->id, nullptr, tdecl, item->type);
    auto* t2 = make<TypeDeclaration>(*tdecl);
    t2->type_manifest = nullptr;
    return return_payload(ghosts, sig_type(item->id, t2, item->rec, item->vis), nullptr, Path::pident(item->id));
  };
  return merge(patch, false, env, sg, loc, lid).sg;
}

const ModuleType* check_package_with_type_constraints(const Location& loc, env::t env, const ModuleType* mty,
                                                      bool compute,
                                                      Slice<std::pair<pt::LidLoc, const tt::CoreType*>> constraints) {
  Signature sg = extract_sig(env, loc, mty);
  for (auto& [lid, cty] : constraints) sg = merge_package(env, loc, sg, lid, cty);
  if (!compute) return nullptr;
  long scope = ctype::create_scope();
  return mtype::freshen(static_cast<int>(scope), mty_signature(sg));
}

bool is_destructive(const pt::WithConstraint* c) {
  using K = pt::WithConstraint::Kind;
  return c->kind == K::Pwith_typesubst || c->kind == K::Pwith_modtypesubst || c->kind == K::Pwith_modsubst;
}

}  // namespace merge

// ---- approximation of module types for recursive modules -----------------------------------
// Return a module type that approximates the shape of the given module type
// AST: only module, type and module type components of signatures; types
// keep their arity and are abstract.
static Signature approx_sig(env::t env, pt::Signature ssg, std::size_t k);
static const ModuleType* approx_modtype_(env::t env, const pt::ModuleType* smty);

static const ModuleDeclaration* approx_module_declaration(env::t env, const pt::ModuleDeclaration* pmd) {
  return make<ModuleDeclaration>(approx_modtype_(env, pmd->pmd_type), parsetree::types_attributes(pmd->pmd_attributes),
                                 pmd->pmd_loc, uid::internal_not_actually_unique());
}
static const ModtypeDeclaration* approx_modtype_info(env::t env, const pt::ModuleTypeDeclaration* sinfo) {
  return make<ModtypeDeclaration>(sinfo->pmtd_type ? approx_modtype_(env, sinfo->pmtd_type) : nullptr,
                                  parsetree::types_attributes(sinfo->pmtd_attributes), sinfo->pmtd_loc,
                                  uid::internal_not_actually_unique());
}

// constraints are first approximated then merged, disabling all
// equivalence and wellformedness checks
static Signature approx_constraint(env::t env, Signature body, const pt::WithConstraint* constr) {
  bool destructive = merge::is_destructive(constr);
  using K = pt::WithConstraint::Kind;
  switch (constr->kind) {
    case K::Pwith_type:
    case K::Pwith_typesubst:
      return merge::merge_type_approx(destructive, env, constr->decl->ptype_loc, body, constr->lid);
    case K::Pwith_modtype:
    case K::Pwith_modtypesubst: {
      const ModuleType* approx_smty = approx_modtype_(env, constr->mty);
      return merge::merge_modtype(true, destructive, env, constr->mty->pmty_loc, body, constr->lid, approx_smty).sg;
    }
    case K::Pwith_module:
    case K::Pwith_modsubst: {
      // Lookup the module to make sure that it is not recursive (GPR#1626)
      auto [path, approx_md] = env::lookup_module(false, constr->lid2.loc, constr->lid2.txt, env);
      return merge::merge_module(true, destructive, env, constr->lid2.loc, body, constr->lid, approx_md, path, false)
          .sg;
    }
  }
  throw std::logic_error("approx_constraint");
}

static const ModuleType* approx_modtype_(env::t env, const pt::ModuleType* smty) {
  const pt::ModuleTypeDesc* d = smty->pmty_desc;
  using K = pt::ModuleTypeDesc::Kind;
  switch (d->kind) {
    case K::Pmty_ident:
      return mty_ident(env::lookup_modtype_path(false, smty->pmty_loc, as<pt::Pmty_ident>(d)->lid.txt, env));
    case K::Pmty_alias:
      return mty_alias(env::lookup_module_path(false, smty->pmty_loc, false, as<pt::Pmty_alias>(d)->lid.txt, env));
    case K::Pmty_signature: return mty_signature(approx_sig(env, as<pt::Pmty_signature>(d)->sg, 0));
    case K::Pmty_functor: {
      auto* f = as<pt::Pmty_functor>(d);
      FunctorParameter param;
      env::t newenv = env;
      if (!f->param.is_unit) {
        const ModuleType* arg = approx_modtype_(env, f->param.mty);
        param.is_unit = false;
        param.named_obj = fresh_identity();
        param.mty = arg;
        if (f->param.name.txt.some) {
          const ModuleType* rarg = mtype::scrape_for_functor_arg(env, arg);
          long scope = ctype::create_scope();
          auto [id, e2] = env::enter_module(static_cast<int>(scope), f->param.name.txt.v, ModulePresence::Mp_present,
                                            rarg, env, true);
          param.id = id;
          param.some_obj = fresh_identity();
          newenv = e2;
        }
      }
      const ModuleType* res = approx_modtype_(newenv, f->body);
      return mty_functor(param, res);
    }
    case K::Pmty_with: {
      auto* w = as<pt::Pmty_with>(d);
      // the module type body is approximated and resolved to a signature,
      // then the constraints are approximated and merged
      const ModuleType* approx_body = approx_modtype_(env, w->mty);
      Signature sg = extract_sig(env, w->mty->pmty_loc, approx_body);
      for (auto* c : w->cstrs) sg = approx_constraint(env, sg, c);
      return mty_signature(sg);
    }
    case K::Pmty_typeof: return type_module_type_of_fwd(env, as<pt::Pmty_typeof>(d)->me).second;
    case K::Pmty_extension: throw ErrorForward(as<pt::Pmty_extension>(d)->ext);
  }
  throw std::logic_error("approx_modtype");
}

static Signature approx_sig(env::t env, pt::Signature ssg, std::size_t k) {
  if (k == ssg.size()) return {};
  const pt::SignatureItem* item = ssg[k];
  const pt::SignatureItemDesc* d = item->psig_desc;
  using K = pt::SignatureItemDesc::Kind;
  auto cat = [](std::vector<const SignatureItem*> a, Signature b) {
    a.insert(a.end(), b.begin(), b.end());
    return slice(a);
  };
  switch (d->kind) {
    case K::Psig_type: {
      auto* t = as<pt::Psig_type>(d);
      auto decls = typedecl::approx_type_decl(TypeOrigin{TypeOrigin::Kind::Approx_recmod}, t->decls);
      Signature rem = approx_sig(env, ssg, k + 1);
      // map_rec_type ~rec_flag
      std::vector<const SignatureItem*> out;
      for (std::size_t j = 0; j < decls.size(); ++j) {
        RecStatus rs = j == 0 ? (t->rec == RecFlag::Recursive ? RecStatus::Trec_first : RecStatus::Trec_not)
                              : RecStatus::Trec_next;
        out.push_back(sig_type(decls[j].first, decls[j].second, rs, Visibility::Exported));
      }
      return cat(out, rem);
    }
    case K::Psig_typesubst: return approx_sig(env, ssg, k + 1);
    case K::Psig_module: {
      const pt::ModuleDeclaration* pmd = as<pt::Psig_module>(d)->md;
      if (!pmd->pmd_name.txt.some) return approx_sig(env, ssg, k + 1);
      long scope = ctype::create_scope();
      const ModuleDeclaration* md = approx_module_declaration(env, pmd);
      ModulePresence pres =
          md->md_type->kind == ModuleType::Kind::Mty_alias ? ModulePresence::Mp_absent : ModulePresence::Mp_present;
      auto [id, newenv] = env::enter_module_declaration(static_cast<int>(scope), pmd->pmd_name.txt.v, pres, md, env);
      return cat({sig_module(id, pres, md, RecStatus::Trec_not, Visibility::Exported)}, approx_sig(newenv, ssg, k + 1));
    }
    case K::Psig_modsubst: {
      const pt::ModuleSubstitution* pms = as<pt::Psig_modsubst>(d)->ms;
      long scope = ctype::create_scope();
      auto [p, md] = env::lookup_module(false, pms->pms_manifest.loc, pms->pms_manifest.txt, env);
      (void)p;
      ModulePresence pres =
          md->md_type->kind == ModuleType::Kind::Mty_alias ? ModulePresence::Mp_absent : ModulePresence::Mp_present;
      auto [id, newenv] = env::enter_module_declaration(static_cast<int>(scope), pms->pms_name.txt, pres, md, env);
      (void)id;
      return approx_sig(newenv, ssg, k + 1);
    }
    case K::Psig_recmodule: {
      long scope = ctype::create_scope();
      std::vector<std::pair<Ident::t, const ModuleDeclaration*>> decls;
      for (auto* pmd : as<pt::Psig_recmodule>(d)->mds) {
        if (!pmd->pmd_name.txt.some) continue;
        // (Ident.create_scoped .., approx_module_declaration ..): right to left
        const ModuleDeclaration* md = approx_module_declaration(env, pmd);
        decls.push_back({Ident::create_scoped(static_cast<int>(scope), pmd->pmd_name.txt.v), md});
      }
      env::t newenv = env;
      for (auto& [id, md] : decls) newenv = env::add_module_declaration(false, id, ModulePresence::Mp_present, md, newenv);
      Signature rem = approx_sig(newenv, ssg, k + 1);
      return slice(map_rec<std::pair<Ident::t, const ModuleDeclaration*>>(
          [](RecStatus rs, const std::pair<Ident::t, const ModuleDeclaration*>& x) {
            return sig_module(x.first, ModulePresence::Mp_present, x.second, rs, Visibility::Exported);
          },
          decls, std::vector<const SignatureItem*>(rem.begin(), rem.end())));
    }
    case K::Psig_modtype:
    case K::Psig_modtypesubst: {
      const pt::ModuleTypeDeclaration* md =
          d->kind == K::Psig_modtype ? as<pt::Psig_modtype>(d)->mtd : as<pt::Psig_modtypesubst>(d)->mtd;
      const ModtypeDeclaration* info = approx_modtype_info(env, md);
      long scope = ctype::create_scope();
      auto [id, newenv] = env::enter_modtype(static_cast<int>(scope), md->pmtd_name.txt, info, env);
      if (d->kind == K::Psig_modtypesubst) return approx_sig(newenv, ssg, k + 1);
      return cat({sig_modtype(id, info, Visibility::Exported)}, approx_sig(newenv, ssg, k + 1));
    }
    case K::Psig_open: {
      auto [od, env2] = type_open_descr(nullptr, false, env, as<pt::Psig_open>(d)->od);
      (void)od;
      return approx_sig(env2, ssg, k + 1);
    }
    case K::Psig_include: {
      const pt::ModuleType* smty = as<pt::Psig_include>(d)->incl->pincl_mod;
      const ModuleType* mty = approx_modtype_(env, smty);
      long scope = ctype::create_scope();
      auto [sg, newenv] = env::enter_signature(static_cast<int>(scope), extract_sig(env, smty->pmty_loc, mty), env);
      return cat(std::vector<const SignatureItem*>(sg.begin(), sg.end()), approx_sig(newenv, ssg, k + 1));
    }
    case K::Psig_class:
    case K::Psig_class_type: {
      // (Psig_class sdecls | Psig_class_type sdecls: the same type)
      Slice<const pt::ClassTypeDeclaration*> sdecls =
          d->kind == K::Psig_class ? as<pt::Psig_class>(d)->decls : as<pt::Psig_class_type>(d)->decls;
      auto [decls, env2] = typeclass::approx_class_declarations(env, sdecls);
      Signature rem = approx_sig(env2, ssg, k + 1);
      std::vector<const SignatureItem*> out;
      for (std::size_t j = 0; j < decls.size(); ++j) {
        RecStatus rs = j == 0 ? RecStatus::Trec_first : RecStatus::Trec_next;
        out.push_back(sig_class_type(decls[j].clsty_ty_id, decls[j].clsty_ty_decl, rs, Visibility::Exported));
        out.push_back(sig_type(decls[j].clsty_obj_id, decls[j].clsty_obj_abbr, rs, Visibility::Exported));
      }
      return cat(out, rem);
    }
    default: return approx_sig(env, ssg, k + 1);
  }
}

const ModuleType* approx_modtype(env::t env, const pt::ModuleType* smty) {
  return warnings::without_warnings([&] { return approx_modtype_(env, smty); });
}

// ---- module Signature_names ----------------------------------------------------------------
enum class HideReason { From_open, Shadowed_by };
struct Hide {
  SigComponentKind kind;
  Location loc;
  HideReason reason;
  Ident::t shadower = nullptr;
  Location shadower_loc;
};
struct IdentLess {
  bool operator()(Ident::t a, Ident::t b) const { return ident::compare(a, b) < 0; }
};
struct SignatureNames {
  // names: the bound names of each component kind (`Exported | `Shadowable)
  std::map<std::string_view, NameInfo> values, types, modules, modtypes, typexts, classes, class_types;
  // to_be_removed
  subst::t subst;
  std::map<Ident::t, Hide, IdentLess> hide;
};

SignatureNames* create_signature_names() {
  auto* t = new SignatureNames();
  t->subst = subst::identity();
  return t;
}

static std::map<std::string_view, NameInfo>& table_for(SigComponentKind component, SignatureNames* names) {
  switch (component) {
    case SigComponentKind::Value: return names->values;
    case SigComponentKind::Type:
    case SigComponentKind::Label:
    case SigComponentKind::Constructor: return names->types;
    case SigComponentKind::Module: return names->modules;
    case SigComponentKind::Module_type: return names->modtypes;
    case SigComponentKind::Extension_constructor: return names->typexts;
    case SigComponentKind::Class: return names->classes;
    case SigComponentKind::Class_type: return names->class_types;
  }
  throw std::logic_error("table_for");
}

static void check(SigComponentKind cl, SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info) {
  switch (info.kind) {
    case NameInfo::Kind::Substituted_away: {
      try {
        t->subst = subst::compose(info.subst, t->subst);
      } catch (const subst::ModuleTypePathSubstitutedAway& s) {
        Error e = err(loc, env::empty(), EK::Non_packable_local_modtype_subst);
        e.path = s.path;
        raise_error(e);
      }
      return;
    }
    case NameInfo::Kind::From_open: t->hide[id] = Hide{cl, loc, HideReason::From_open}; return;
    default: break;
  }
  auto& tbl = table_for(cl, t);
  std::string_view name = ident::name(id);
  auto it = tbl.find(name);
  if (it == tbl.end()) {
    tbl.emplace(name, info);
    return;
  }
  if (it->second.kind == NameInfo::Kind::Shadowable) {
    Shadowable s = it->second.shadowable;
    it->second = info;
    for (Ident::t shadowed_id : s.group) t->hide[shadowed_id] = Hide{cl, s.loc, HideReason::Shadowed_by, id, loc};
    return;
  }
  Error e = err(loc, env::empty(), EK::Repeated_name);
  e.component = cl;
  e.name = std::string(name);
  raise_error(e);
}

void check_value(SignatureNames* t, const Location& loc, Ident::t id, const std::optional<NameInfo>& info) {
  NameInfo i = info ? *info : NameInfo{NameInfo::Kind::Shadowable, Shadowable{id, {id}, loc}};
  check(SigComponentKind::Value, t, loc, id, i);
}
void check_type(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info) {
  check(SigComponentKind::Type, t, loc, id, info);
}
void check_module(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info) {
  check(SigComponentKind::Module, t, loc, id, info);
}
void check_modtype(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info) {
  check(SigComponentKind::Module_type, t, loc, id, info);
}
void check_typext(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info) {
  check(SigComponentKind::Extension_constructor, t, loc, id, info);
}
void check_class(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info) {
  check(SigComponentKind::Class, t, loc, id, info);
}
void check_class_type(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info) {
  check(SigComponentKind::Class_type, t, loc, id, info);
}

void check_sig_item(SignatureNames* names, const Location& loc, const signature_group::RecGroup& item,
                    const std::optional<NameInfo>& info) {
  // we can ignore x.pre_ghosts: they are eliminated by strengthening, and
  // thus never appear in includes
  for (auto& si : signature_group::rec_items(item.group)) {
    std::vector<Classified> all;
    for (auto* it : signature_group::flatten(si)) all.push_back(classify_signature_item(it));
    std::vector<Ident::t> group;
    for (auto& c : all) group.push_back(c.id);
    for (auto& c : all) {
      NameInfo i = info ? *info : NameInfo{NameInfo::Kind::Shadowable, Shadowable{c.id, group, loc}};
      check(c.kind, names, loc, c.id, i);
    }
  }
}

// Keep only the last (rightmost) specification of a component with
// several ones, removing all references to the previous ones from the
// signature, or error out with [Cannot_hide_id].
Signature simplify(env::t env, SignatureNames* t, Signature sg) {
  // Ident.Map.fold (increasing order) consing: the ids in decreasing order
  std::vector<Ident::t> ids_to_remove;
  for (auto& [id, h] : t->hide)
    if (shape::can_appear_in_types(h.kind)) ids_to_remove.insert(ids_to_remove.begin(), id);
  std::vector<const SignatureItem*> out;
  for (const SignatureItem* component : sg) {
    Classified c = classify_signature_item(component);
    if (t->hide.count(c.id)) continue;
    if (t->subst != subst::identity()) {
      try {
        component = subst::signature_item(subst::Scoping::keep(), t->subst, component);
      } catch (const subst::ModuleTypePathSubstitutedAway& s) {
        Error e = err(c.loc, env, EK::Non_packable_local_modtype_subst);
        e.path = s.path;
        raise_error(e);
      }
    }
    if (!ids_to_remove.empty()) {
      try {
        component = mtype::nondep_sig_item(env, ids_to_remove, component);
      } catch (const ctype::NondepCannotErase& n) {
        const Hide& h = t->hide.at(n.id);
        HidingError he;
        Location err_loc;
        if (h.reason == HideReason::From_open) {
          err_loc = h.loc;
          he.kind = HidingError::Kind::Appears_in_signature;
          he.opened_item_kind = h.kind;
          he.opened_item_id = n.id;
        } else {
          err_loc = h.shadower_loc;
          he.kind = HidingError::Kind::Illegal_shadowing;
          he.shadowed_item_kind = h.kind;
          he.shadowed_item_id = n.id;
          he.shadowed_item_loc = h.loc;
          he.shadower_id = h.shadower;
        }
        he.user_id = c.id;
        he.user_kind = c.kind;
        he.user_loc = c.loc;
        Error e = err(err_loc, env, EK::Cannot_hide_id);
        e.hiding = he;
        raise_error(e);
      }
    }
    out.push_back(component);
  }
  return slice(out);
}

}  // namespace cppcaml::typing::typemod
