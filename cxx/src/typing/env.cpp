// Port of typing/env.ml.  See env.hpp for the deviations (no shapes; no
// warnings / alerts / usage tracking yet).  Sections follow env.ml.
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/utf8_lexeme.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/location.hpp"

#include <algorithm>

#include "cppcaml/typing/mtype.hpp"
#include "cppcaml/typing/predef.hpp"

namespace cppcaml::typing::env {

using namespace types;
using namespace btype;
namespace lz = subst::lazy;

// ---- forward references --------------------------------------------------------
std::function<void(bool, const Location&, Longident::t, Path::t,
                   const std::vector<std::pair<Path::t, const ModuleType*>>&, Path::t,
                   const ModuleType*, const ModuleType*, t)>
    check_functor_application;
std::function<const lz::Modtype*(bool, t, const lz::Modtype*, Path::t)> strengthen;
std::function<bool(t, TypeExpr*, TypeExpr*)> same_constr;
std::function<void(t, const Location&, const std::string&, const ModuleType*)>
    check_well_formed_module;

// ---- errors (Typing_recovery.log_and_raise: raise when not collecting) --------
[[noreturn]] static void lookup_error(const Location& loc, t env, LookupError err) {
  Error e(Error::Kind::Lookup_error);
  e.loc = loc;
  e.env = env;
  e.err = err;
  throw e;
}
[[noreturn]] static void may_lookup_error(bool errors, const Location& loc, t env,
                                          LookupError err) {
  if (errors) lookup_error(loc, env, err);
  throw NotFound{};
}
static LookupError lerr(LookupError::Kind k, Longident::t lid = nullptr) {
  LookupError e;
  e.kind = k;
  e.lid = lid;
  return e;
}

// ============================================================================
// IdTbl

template <class A, class B>
static IdTbl<A, B> idtbl_add(Ident::t id, const A& x, const IdTbl<A, B>& tbl) {
  IdTbl<A, B> r = tbl;
  r.current = tbl.current.add(id, x);
  return r;
}

template <class A, class B>
static IdTbl<A, B> idtbl_remove(Ident::t id, const IdTbl<A, B>& tbl) {
  IdTbl<A, B> r = tbl;
  r.current = tbl.current.remove(id);
  return r;
}

// add_open slot wrap root components next: `using` (the slot, applied to
// the component's kind) detects unused opens and shadowing
template <class A, class B>
static IdTbl<A, B> idtbl_add_open(Path::t root, const StrMap<B>& components, const IdTbl<A, B>& next,
                                  const UsingFn<A>* using_) {
  IdTbl<A, B> r;
  using L = typename IdTbl<A, B>::Layer;
  r.layer = make<L>(L{true, root, components, using_, {}, next});
  return r;
}

template <class A, class B>
static IdTbl<A, B> idtbl_remove_last_open(Path::t rt, const IdTbl<A, B>& tbl) {
  if (tbl.layer && tbl.layer->is_open && path::same(rt, tbl.layer->root)) {
    IdTbl<A, B> next = tbl.layer->next;
    ident::Tbl<A> cur = next.current;
    tbl.current.fold_all([&](Ident::t id, const A& d) { cur = cur.add(id, d); });
    next.current = cur;
    return next;
  }
  throw std::logic_error("Env.IdTbl.remove_last_open");
}

template <class A, class B>
static IdTbl<A, B> idtbl_map(std::function<A(A)> f, const IdTbl<A, B>& next) {
  IdTbl<A, B> r;
  using L = typename IdTbl<A, B>::Layer;
  r.layer = make<L>(L{false, nullptr, {}, nullptr, std::move(f), next});
  return r;
}

template <class A, class B>
static A idtbl_find_same(Ident::t id, const IdTbl<A, B>& tbl) {
  if (const A* x = tbl.current.find_same_opt(id)) return *x;
  if (!tbl.layer) throw NotFound{};
  if (tbl.layer->is_open) return idtbl_find_same(id, tbl.layer->next);
  return tbl.layer->f(idtbl_find_same(id, tbl.layer->next));
}
// the same, nullopt where it raises Not_found (for the callers that catch it
// at once: a persistent module, the common case, is found without a throw)
template <class A, class B>
static std::optional<A> idtbl_find_same_opt(Ident::t id, const IdTbl<A, B>& tbl) {
  if (const A* x = tbl.current.find_same_opt(id)) return *x;
  if (!tbl.layer) return std::nullopt;
  std::optional<A> r = idtbl_find_same_opt(id, tbl.layer->next);
  if (!r || tbl.layer->is_open) return r;
  return tbl.layer->f(*r);
}

// idtbl_find_name, nullopt where it raises Not_found: nothing observable
// happens on that path (wrap and the using callbacks run only on a find), so
// the callers that catch Not_found at once test the result instead -- a throw
// from the bottom of the layers unwinds one frame per `open`
template <class A, class B, class W>
static std::optional<std::pair<Path::t, A>> idtbl_find_name_opt(W&& wrap, bool mark, std::string_view name,
                                                                const IdTbl<A, B>& tbl) {
  if (auto [id, desc] = tbl.current.find_name_opt(name); desc)
    return std::pair<Path::t, A>{Path::pident(id), *desc};
  if (!tbl.layer) return std::nullopt;
  const auto* L = tbl.layer;
  if (L->is_open) {
    if (const B* c = L->components.find_opt(name)) {
      A descr = wrap(*c);
      std::pair<Path::t, A> res{Path::pdot(L->root, name), descr};
      if (mark && L->using_) {
        std::optional<std::pair<Path::t, A>> hidden = idtbl_find_name_opt(wrap, false, name, L->next);
        if (!hidden) {
          (*L->using_)(name, nullptr);
        } else {
          std::pair<A, A> both{hidden->second, descr};
          (*L->using_)(name, &both);
        }
      }
      return res;
    }
    return idtbl_find_name_opt(wrap, mark, name, L->next);
  }
  auto r = idtbl_find_name_opt(wrap, mark, name, L->next);
  if (!r) return std::nullopt;
  return std::pair<Path::t, A>{r->first, L->f(r->second)};
}
template <class A, class B, class W>
static std::pair<Path::t, A> idtbl_find_name(W&& wrap, bool mark, std::string_view name,
                                             const IdTbl<A, B>& tbl) {
  auto r = idtbl_find_name_opt(wrap, mark, name, tbl);
  if (!r) throw NotFound{};
  return *r;
}

template <class A, class B, class W>
static std::vector<std::pair<Path::t, A>> idtbl_find_all(W&& wrap, std::string_view name,
                                                         const IdTbl<A, B>& tbl) {
  std::vector<std::pair<Path::t, A>> out;
  for (auto& [id, d] : tbl.current.find_all(name)) out.emplace_back(Path::pident(id), *d);
  if (!tbl.layer) return out;
  const auto* L = tbl.layer;
  if (L->is_open) {
    if (const B* c = L->components.find_opt(name))
      out.emplace_back(Path::pdot(L->root, name), wrap(*c));
    auto rest = idtbl_find_all(wrap, name, L->next);
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
  }
  for (auto& [p, d] : idtbl_find_all(wrap, name, L->next)) out.emplace_back(p, L->f(d));
  return out;
}

// fold_name wrap f tbl acc, as the sequence of `f name (path, data)` calls
template <class A, class B, class W>
static void idtbl_fold_name(W&& wrap, const std::function<void(std::string_view, Path::t, const A&)>& f,
                            const IdTbl<A, B>& tbl) {
  tbl.current.fold_name(
      [&](Ident::t id, const A& d) { f(ident::name(id), Path::pident(id), d); });
  if (!tbl.layer) return;
  const auto* L = tbl.layer;
  if (L->is_open) {
    L->components.iter(
        [&](std::string_view name, const B& desc) { f(name, Path::pdot(L->root, name), wrap(desc)); });
    idtbl_fold_name(wrap, f, L->next);
    return;
  }
  idtbl_fold_name<A, B>(
      wrap,
      std::function<void(std::string_view, Path::t, const A&)>(
          [&](std::string_view name, Path::t p, const A& d) { f(name, p, L->f(d)); }),
      L->next);
}

template <class A, class B>
static void idtbl_local_keys(const IdTbl<A, B>& tbl, std::vector<Ident::t>& acc) {
  // Ident.fold_all (fun k _ accu -> k::accu): consing, so reversed
  tbl.current.fold_all([&](Ident::t k, const A&) { acc.insert(acc.begin(), k); });
  if (tbl.layer) idtbl_local_keys(tbl.layer->next, acc);
}

template <class A, class B>
static std::vector<Ident::t> idtbl_diff_keys(const IdTbl<A, B>& tbl1, const IdTbl<A, B>& tbl2) {
  std::vector<Ident::t> keys2;
  idtbl_local_keys(tbl2, keys2);
  std::vector<Ident::t> out;
  for (Ident::t id : keys2) {
    try {
      idtbl_find_same(id, tbl1);
    } catch (const NotFound&) {
      out.push_back(id);
    }
  }
  return out;
}

// ============================================================================
// TycompTbl

template <class A>
static TycompTbl<A> tycomp_add(Ident::t id, const A& x, const TycompTbl<A>& tbl) {
  TycompTbl<A> r = tbl;
  r.current = tbl.current.add(id, x);
  return r;
}

template <class A>
static TycompTbl<A> tycomp_add_open(Path::t root, const StrMap<Slice<A>>& components,
                                    const TycompTbl<A>& next, const UsingFn<A>* using_) {
  TycompTbl<A> r;
  using O = typename TycompTbl<A>::Opened;
  r.opened = make<O>(O{components, root, using_, next});
  return r;
}

template <class A>
static TycompTbl<A> tycomp_remove_last_open(Path::t rt, const TycompTbl<A>& tbl) {
  if (tbl.opened && path::same(rt, tbl.opened->root)) {
    TycompTbl<A> next = tbl.opened->next;
    ident::Tbl<A> cur = next.current;
    tbl.current.fold_all([&](Ident::t id, const A& d) { cur = cur.add(id, d); });
    next.current = cur;
    return next;
  }
  throw std::logic_error("Env.TycompTbl.remove_last_open");
}

template <class A>
static A tycomp_find_same(Ident::t id, const TycompTbl<A>& tbl) {
  if (const A* x = tbl.current.find_same_opt(id)) return *x;
  if (!tbl.opened) throw NotFound{};
  return tycomp_find_same(id, tbl.opened->next);
}

// find_all ~mark name tbl: (desc, use callback) list, locals first
template <class A>
static std::vector<std::pair<A, std::function<void()>>> tycomp_find_all(
    bool mark, std::string_view name, const TycompTbl<A>& tbl) {
  std::vector<std::pair<A, std::function<void()>>> out;
  for (auto& [id, d] : tbl.current.find_all(name)) out.emplace_back(*d, [] {});
  if (!tbl.opened) return out;
  auto rest = tycomp_find_all(mark, name, tbl.opened->next);
  const UsingFn<A>* using_ = mark ? tbl.opened->using_ : nullptr;
  if (const Slice<A>* opened = tbl.opened->components.find_opt(name))
    for (const A& desc : *opened) {
      // mk_callback rest name desc using
      std::function<void()> cb = [] {};
      if (using_) {
        std::optional<A> hidden;
        if (!rest.empty()) hidden = rest.front().first;
        std::string n(name);
        cb = [using_, n, desc, hidden] {
          if (!hidden) {
            (*using_)(n, nullptr);
          } else {
            std::pair<A, A> both{desc, *hidden};
            (*using_)(n, &both);
          }
        };
      }
      out.emplace_back(desc, cb);
    }
  out.insert(out.end(), rest.begin(), rest.end());
  return out;
}

template <class A, class F>
static void tycomp_fold_name(F&& f, const TycompTbl<A>& tbl) {
  tbl.current.fold_name([&](Ident::t, const A& d) { f(d); });
  if (!tbl.opened) return;
  // NameMap.fold (fun _name -> List.fold_right f): each list right to left
  tbl.opened->components.iter([&](std::string_view, const Slice<A>& l) {
    for (auto it = l.end(); it != l.begin();) f(*--it);
  });
  tycomp_fold_name<A>(f, tbl.opened->next);
}

template <class A>
static void tycomp_local_keys(const TycompTbl<A>& tbl, std::vector<Ident::t>& acc) {
  tbl.current.fold_all([&](Ident::t k, const A&) { acc.insert(acc.begin(), k); });
  if (tbl.opened) tycomp_local_keys(tbl.opened->next, acc);
}

template <class A, class P>
static std::vector<Ident::t> tycomp_diff_keys(P&& is_local, const TycompTbl<A>& tbl1,
                                              const TycompTbl<A>& tbl2) {
  std::vector<Ident::t> keys2;
  tycomp_local_keys(tbl2, keys2);
  std::vector<Ident::t> out;
  for (Ident::t id : keys2) {
    if (!is_local(tycomp_find_same(id, tbl2))) continue;
    try {
      tycomp_find_same(id, tbl1);
    } catch (const NotFound&) {
      out.push_back(id);
    }
  }
  return out;
}

// ============================================================================
// The environment record

static const Summary* summ(Summary s) { return make<Summary>(s); }

static EnvT* copy_env(t env) { return make<EnvT>(*env); }

// Env.empty: one value (Out_type compares the printing environment with it
// physically)
t empty() {
  static t e_empty = [] {
    ZoneScope perm(permanent_zone());
    const Summary* s_empty = make<Summary>(Summary{Summary::Kind::Env_empty});
    EnvT* e = make<EnvT>();
    e->summary = s_empty;
    return e;
  }();
  return e_empty;
}

bool is_empty(t env) {
  // env = Env.empty, structurally: nothing bound, no summary
  return env == empty() || (env->summary->kind == Summary::Kind::Env_empty && env->flags == 0 &&
                            env->local_constraints.is_empty());
}

t in_signature(bool b, t env) {
  EnvT* e = copy_env(env);
  e->flags = b ? (env->flags | in_signature_flag) : (env->flags & ~in_signature_flag);
  return e;
}
bool is_in_signature(t env) { return (env->flags & in_signature_flag) != 0; }
bool has_local_constraints(t env) { return !env->local_constraints.is_empty(); }

static bool is_ext(const ConstructorData* cda) {
  return cda->cda_description->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension;
}
static bool is_local_ext(const ConstructorData* cda) {
  const auto& tag = cda->cda_description->cstr_tag;
  return tag.kind == ConstructorTag::Kind::Cstr_extension &&
         tag.ext_path->kind == Path::Kind::Pident;
}

std::vector<Ident::t> diff(t env1, t env2) {
  auto out = idtbl_diff_keys(env1->values, env2->values);
  auto b = tycomp_diff_keys(is_local_ext, env1->constrs, env2->constrs);
  auto c = idtbl_diff_keys(env1->modules, env2->modules);
  auto d = idtbl_diff_keys(env1->classes, env2->classes);
  out.insert(out.end(), b.begin(), b.end());
  out.insert(out.end(), c.begin(), c.end());
  out.insert(out.end(), d.begin(), d.end());
  return out;
}

bool same_type_declarations(t e1, t e2) {
  return e1->types.current.same_as(e2->types.current) && e1->types.layer == e2->types.layer &&
         e1->modules.current.same_as(e2->modules.current) &&
         e1->modules.layer == e2->modules.layer &&
         e1->local_constraints.same_as(e2->local_constraints);
}

// wrap functions
static const ValueEntry* wrap_value(const ValueData* vda) {
  return make<ValueEntry>(true, vda);
}
static const ModuleEntry* wrap_module(const ModuleData* mda) {
  return make<ModuleEntry>(ModuleEntry::Kind::Mod_local, mda);
}
template <class X>
static X wrap_identity(X x) {
  return x;
}

static const ModuleDeclaration* md(const ModuleType* md_type) {
  return make<ModuleDeclaration>(md_type, Attributes{}, location::none(),
                                 uid::internal_not_actually_unique());
}

// ---- current unit ------------------------------------------------------------
static std::optional<UnitInfo> g_current_unit;
void set_current_unit(const UnitInfo& u) { g_current_unit = u; }
const UnitInfo* get_current_unit() { return g_current_unit ? &*g_current_unit : nullptr; }
std::string get_current_unit_name() { return g_current_unit ? g_current_unit->modname : ""; }
static bool current_unit_is(std::string_view name) { return get_current_unit_name() == name; }
static bool current_unit_is_ident(Ident::t id) {
  return ident::persistent(id) && current_unit_is(ident::name(id));
}
static bool current_unit_is_path(Path::t p) {
  return p->kind == Path::Kind::Pident && current_unit_is_ident(p->id);
}

static const ModuleEntry* mod_persistent() {
  static const ModuleEntry* m = [] {
    ZoneScope perm(permanent_zone());
    return make<ModuleEntry>(ModuleEntry::Kind::Mod_persistent);
  }();
  return m;
}

static const ModuleEntry* find_same_module(Ident::t id,
                                           const IdTbl<const ModuleEntry*, const ModuleData*>& tbl) {
  if (std::optional<const ModuleEntry*> e = idtbl_find_same_opt(id, tbl)) return *e;
  if (ident::persistent(id) && !current_unit_is_ident(id)) return mod_persistent();
  throw NotFound{};
}

static std::pair<Path::t, const ModuleEntry*> find_name_module(
    bool mark, std::string_view name, const IdTbl<const ModuleEntry*, const ModuleData*>& tbl) {
  if (auto r = idtbl_find_name_opt(wrap_module, mark, name, tbl)) return *r;
  if (current_unit_is(name)) throw NotFound{};
  return {Path::pident(Ident::create_persistent(name)), mod_persistent()};
}

static const bool& no_alias_deps = clflags::no_alias_deps;

t add_persistent_structure(Ident::t id, t env) {
  if (!ident::persistent(id)) throw std::invalid_argument("Env.add_persistent_structure");
  if (current_unit_is_ident(id)) return env;
  // This addition only observably changes the environment if it shadows a
  // non-persistent module already in the environment (PR#9345).
  bool material = false;
  if (auto r = idtbl_find_name_opt(wrap_module, false, ident::name(id), env->modules))
    material = r->second->kind != ModuleEntry::Kind::Mod_persistent;
  EnvT* e = copy_env(env);
  if (material) {
    Summary s{Summary::Kind::Env_persistent, env->summary};
    s.id = id;
    e->summary = summ(s);
  }
  if (material || !no_alias_deps) e->modules = idtbl_add(id, mod_persistent(), env->modules);
  return e;
}

static ModuleComponents* components_of_module(StrMap<std::string_view> alerts, const Uid& uid,
                                              t env, subst::t ps, Path::t path,
                                              AddressLazy* addr, const lz::Modtype* mty, shape::t shape) {
  return make<ModuleComponents>(
      alerts, uid,
      LazyBacktrack<ComponentsMaker, ComponentsResult>::create(
          ComponentsMaker{env, ps, path, addr, mty, shape}));
}

// ---- persistent structures ---------------------------------------------------
static persistent_env::PersistentEnv<const ModuleData*> g_persistent_env;

// the persistent ident's name: the cmi's own string (a fresh one per read,
// as OCaml's; kept for the whole process -- a cached lambda may hold the
// ident past the unit's zone)
static std::string_view persistent_name(std::string_view name) {
  ZoneScope perm(permanent_zone());
  return zborrow(name);
}

static const ModuleData* sign_of_cmi(bool freshen, const persistent_env::PersistentSignature& ps) {
  const auto& cmi = ps.cmi;
  Ident::t id = Ident::create_persistent(persistent_name(cmi.cmi_name));
  Path::t path = Path::pident(id);
  StrMap<std::string_view> alerts;
  for (auto& f : cmi.cmi_flags)
    if (f.kind == cmi_format::PersFlag::Kind::Alerts) alerts = f.alerts;
  auto* mt = make<ModuleType>(ModuleType::Kind::Mty_signature);
  mt->sign = cmi.cmi_sign;
  const ModuleDeclaration* md0 = make<ModuleDeclaration>(
      mt, Attributes{}, location::none(), uid::of_compilation_unit_id(ident::name(id)));
  AddressLazy* mda_address = AddressLazy::create_forced(make<Address>(true, id));
  const lz::ModuleDecl* mda_declaration =
      lz::module_decl(subst::Scoping::make_local(), subst::identity(), lz::of_module_decl(md0));
  shape::t mda_shape = shape::for_persistent_unit(cmi.cmi_name);
  const lz::Modtype* mty = lz::of_modtype(mt);
  if (freshen) mty = lz::modtype(subst::Scoping::rescope(path::scope(path)), subst::identity(), mty);
  ModuleComponents* comps = components_of_module(alerts, md0->md_uid, empty(), subst::identity(),
                                                 path, mda_address, mty, mda_shape);
  return make<ModuleData>(mda_declaration, comps, mda_address, mda_shape);
}

static const ModuleData* read_sign_of_cmi(const persistent_env::PersistentSignature& ps) {
  return sign_of_cmi(true, ps);
}

// Persistent_env.check_pers_struct: emits a warning if there is no valid cmi
// for [name]
static void check_pers_struct(bool allow_hidden, const Location& loc, const std::string& name) {
  using W = warnings::Warning;
  auto warn = [&](std::optional<std::string> msg) {
    W w = W::with_s(W::K::No_cmi_file, name);
    w.opt = std::move(msg);
    location::prerr_warning(loc, w);
  };
  try {
    g_persistent_env.find_pers_struct(allow_hidden, read_sign_of_cmi, false, name);
  } catch (const load_path::NotFound&) {
    warn(std::nullopt);
  } catch (const cmi_format::Error& e) {
    // Format.asprintf "%a" Cmi_format.report_error err
    auto qf = [f = e.filename](format_doc::Formatter& ff) { location::doc::quoted_filename(ff, f); };
    std::string msg;
    switch (e.kind) {
      case cmi_format::Error::Kind::Not_an_interface:
        msg = format_doc::asprintf("%a@ is not a compiled interface", qf);
        break;
      case cmi_format::Error::Kind::Wrong_version_interface:
        msg = format_doc::asprintf(
            "%a@ is not a compiled interface for this version of OCaml.@.It seems to be for %s version of OCaml.",
            qf, e.older_newer);
        break;
      case cmi_format::Error::Kind::Corrupted_interface:
        msg = format_doc::asprintf("Corrupted compiled interface@ %a", qf);
        break;
    }
    warn(msg);
  } catch (const persistent_env::Error& e) {
    using K = persistent_env::Error::Kind;
    std::string msg;
    switch (e.kind) {
      case K::Illegal_renaming:  // (name, ps_name, filename)
        msg = format_doc::asprintf(
            " %a@ contains the compiled interface for @ %a when %a was expected",
            [f = e.c](format_doc::Formatter& ff) { location::doc::quoted_filename(ff, f); },
            misc::style::code_str(e.b), misc::style::code_str(e.a));
        break;
      case K::Inconsistent_import: throw std::logic_error("Persistent_env.check_pers_struct");
      case K::Need_recursive_types:
        msg = format_doc::asprintf("%a uses recursive types", misc::style::code_str(e.a));
        break;
    }
    warn(msg);
  }
}

// Env.check_pers_mod = Persistent_env.check
static void check_pers_mod(bool allow_hidden, const Location& loc, std::string_view name) {
  if (g_persistent_env.mem(name)) return;
  // PR#6843: record the weak dependency ([add_import]) regardless of
  // whether the check succeeds, to help make builds more deterministic.
  g_persistent_env.add_import(name);
  using W = warnings::Warning;
  if (warnings::is_active(W::make(W::K::No_cmi_file)))
    add_delayed_check_forward(
        [allow_hidden, loc, n = std::string(name)] { check_pers_struct(allow_hidden, loc, n); });
}

// save_signature_with_transform cmi_transform ~alerts sg cmi_info
static cmi_format::CmiInfos save_signature_with_transform(
    const std::function<void(cmi_format::CmiInfos&)>& cmi_transform, StrMap<std::string_view> alerts, Signature sg,
    const std::string& modname, const std::string& filename) {
  btype::cleanup_abbrev_memo();
  subst::reset_for_saving();
  Signature ssg = subst::signature(subst::Scoping::make_local(), subst::for_saving(subst::identity()), sg);
  cmi_format::CmiInfos cmi = g_persistent_env.make_cmi(modname, ssg, alerts);
  cmi_transform(cmi);
  persistent_env::PersistentSignature pers_sig{filename, cmi, load_path::Visibility::Visible};
  const ModuleData* pm = sign_of_cmi(false, pers_sig);  // save_sign_of_cmi
  g_persistent_env.save_cmi(pers_sig, pm);
  return cmi;
}

cmi_format::CmiInfos save_signature(StrMap<std::string_view> alerts, Signature sg, const std::string& modname,
                                    const std::string& filename) {
  return save_signature_with_transform([](cmi_format::CmiInfos&) {}, alerts, sg, modname, filename);
}

cmi_format::CmiInfos save_signature_with_imports(
    StrMap<std::string_view> alerts, Signature sg, const std::string& modname, const std::string& filename,
    const std::vector<std::pair<std::string, std::optional<std::string>>>& imports) {
  return save_signature_with_transform([&](cmi_format::CmiInfos& cmi) { cmi.cmi_crcs = imports; }, alerts, sg,
                                       modname, filename);
}

const ModuleData* find_pers_mod(bool allow_hidden, std::string_view name) {
  try {
    return g_persistent_env.find(allow_hidden, read_sign_of_cmi, name);
  } catch (const load_path::NotFound&) {
    throw NotFound{};
  }
}

void without_cmis(const std::function<void()>& f) {
  g_persistent_env.without_cmis([&] {
    f();
    return 0;
  });
}

std::vector<std::pair<std::string, std::optional<std::string>>> imports() {
  return g_persistent_env.imports();
}

std::string_view import_name(std::string_view name) { return g_persistent_env.import_name(name); }
std::string crc_of_unit(const std::string& name) {
  return g_persistent_env.crc_of_unit(read_sign_of_cmi, name);
}
bool is_imported_opaque(const std::string& m) { return g_persistent_env.is_imported_opaque(m); }
void register_import_as_opaque(const std::string& m) {
  g_persistent_env.register_import_as_opaque(m);
}

void reset_cache() {
  g_current_unit.reset();
  g_persistent_env.clear();
  reset_declaration_caches();
}

// ---- get_components --------------------------------------------------------------
static ComponentsResult components_of_module_maker(ComponentsMaker cm);

ComponentsResult get_components_res(const ModuleComponents* c) {
  std::function<ComponentsResult(ComponentsMaker)> f = components_of_module_maker;
  if (LazyLog* log = g_persistent_env.cannot_load_log())
    return force_logged<ComponentsMaker, ComponentsResult>(
        *log, f, c->comps, [](const ComponentsResult& r) { return !r.ok; });
  return c->comps->force(f);
}

static const ModuleComponentsRepr* empty_structure() {
  auto* s = make<StructureComponents>();
  return make<ModuleComponentsRepr>(true, s);
}

const ModuleComponentsRepr* get_components(const ModuleComponents* c) {
  ComponentsResult r = get_components_res(c);
  if (!r.ok) return empty_structure();
  return r.repr;
}

// ---- module type of functor application ------------------------------------------
static const ModuleType* modtype_of_functor_appl(FunctorComponents* fcomp, Path::t p1,
                                                 Path::t p2) {
  if (fcomp->fcomp_res->kind == ModuleType::Kind::Mty_alias) return fcomp->fcomp_res;
  if (auto it = fcomp->fcomp_subst_cache.find(p2); it != fcomp->fcomp_subst_cache.end())
    return it->second;
  int scope = path::scope(Path::papply(p1, p2));
  subst::t sub = subst::identity();
  if (!fcomp->fcomp_arg.is_unit && fcomp->fcomp_arg.id)
    sub = subst::add_module(fcomp->fcomp_arg.id, p2, subst::identity());
  const ModuleType* mty = subst::modtype(subst::Scoping::rescope(scope), sub, fcomp->fcomp_res);
  fcomp->fcomp_subst_cache.emplace(p2, mty);
  return mty;
}

static void check_functor_appl(bool errors, const Location& loc, Longident::t lid_whole_app,
                               Path::t f0_path,
                               const std::vector<std::pair<Path::t, const ModuleType*>>& args,
                               FunctorComponents* f_comp, Path::t arg_path,
                               const ModuleType* arg_mty, const ModuleType* param_mty, t env) {
  if (!f_comp->fcomp_cache.count(arg_path) && check_functor_application)
    check_functor_application(errors, loc, lid_whole_app, f0_path, args, arg_path, arg_mty,
                              param_mty, env);
}

// ---- lookup by identifier ---------------------------------------------------------
static ModuleComponents* components_of_functor_appl(const Location& loc, Path::t f_path,
                                                    FunctorComponents* f_comp, Path::t arg,
                                                    t env);

static const ModuleData* find_ident_module(Ident::t id, t env) {
  const ModuleEntry* e = find_same_module(id, env->modules);
  switch (e->kind) {
    case ModuleEntry::Kind::Mod_local: return e->data;
    case ModuleEntry::Kind::Mod_unbound: throw NotFound{};
    case ModuleEntry::Kind::Mod_persistent: return find_pers_mod(true, ident::name(id));
  }
  throw NotFound{};
}

static StructureComponents* find_structure_components(Path::t p, t env);
static FunctorComponents* find_functor_components(Path::t p, t env);

static ModuleComponents* find_module_components(Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: return find_ident_module(p->id, env)->mda_components;
    case Path::Kind::Pdot: {
      StructureComponents* sc = find_structure_components(p->p1, env);
      auto* m = sc->comp_modules.find_opt(p->s);
      if (!m) throw NotFound{};
      return (*m)->mda_components;
    }
    case Path::Kind::Papply: {
      FunctorComponents* f_comp = find_functor_components(p->p1, env);
      // Location.(in_file !input_name)
      return components_of_functor_appl(location::none(), p->p1, f_comp, p->p2, env);
    }
    case Path::Kind::Pextra_ty: throw NotFound{};
  }
  throw NotFound{};
}

static StructureComponents* find_structure_components(Path::t p, t env) {
  const ModuleComponentsRepr* r = get_components(find_module_components(p, env));
  if (!r->is_structure) throw NotFound{};
  return r->structure;
}

static FunctorComponents* find_functor_components(Path::t p, t env) {
  const ModuleComponentsRepr* r = get_components(find_module_components(p, env));
  if (r->is_structure) throw NotFound{};
  return r->functor;
}

static const ModuleDeclaration* find_module_(bool alias, Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident:
      return lz::force_module_decl(find_ident_module(p->id, env)->mda_declaration);
    case Path::Kind::Pdot: {
      StructureComponents* sc = find_structure_components(p->p1, env);
      auto* m = sc->comp_modules.find_opt(p->s);
      if (!m) throw NotFound{};
      return lz::force_module_decl((*m)->mda_declaration);
    }
    case Path::Kind::Papply: {
      FunctorComponents* fc = find_functor_components(p->p1, env);
      if (alias) return md(fc->fcomp_res);
      return md(modtype_of_functor_appl(fc, p->p1, p->p2));
    }
    case Path::Kind::Pextra_ty: throw NotFound{};
  }
  throw NotFound{};
}

static const lz::ModuleDecl* find_module_lazy_(bool alias, Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: return find_ident_module(p->id, env)->mda_declaration;
    case Path::Kind::Pdot: {
      StructureComponents* sc = find_structure_components(p->p1, env);
      auto* m = sc->comp_modules.find_opt(p->s);
      if (!m) throw NotFound{};
      return (*m)->mda_declaration;
    }
    case Path::Kind::Papply: {
      FunctorComponents* fc = find_functor_components(p->p1, env);
      const ModuleDeclaration* d =
          alias ? md(fc->fcomp_res) : md(modtype_of_functor_appl(fc, p->p1, p->p2));
      return lz::of_module_decl(d);
    }
    case Path::Kind::Pextra_ty: throw NotFound{};
  }
  throw NotFound{};
}

const ModuleType* find_strengthened_module(bool aliasable, Path::t p, t env) {
  const lz::ModuleDecl* d = find_module_lazy_(true, p, env);
  const lz::Modtype* mty = strengthen(aliasable, env, d->mdl_type, p);
  return lz::force_modtype(mty);
}

static const ValueData* find_value_full(Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: {
      const ValueEntry* e = idtbl_find_same(p->id, env->values);
      if (!e->bound) throw NotFound{};
      return e->data;
    }
    case Path::Kind::Pdot: {
      StructureComponents* sc = find_structure_components(p->p1, env);
      auto* v = sc->comp_values.find_opt(p->s);
      if (!v) throw NotFound{};
      return *v;
    }
    default: throw NotFound{};
  }
}

const ConstructorData* find_extension_full(Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: return tycomp_find_same(p->id, env->constrs);
    case Path::Kind::Pdot: {
      StructureComponents* comps = find_structure_components(p->p1, env);
      auto* cstrs = comps->comp_constrs.find_opt(p->s);
      if (!cstrs) throw NotFound{};
      std::vector<const ConstructorData*> exts;
      for (auto* c : *cstrs)
        if (is_ext(c)) exts.push_back(c);
      if (exts.size() == 1) return exts[0];
      throw NotFound{};
    }
    default: throw NotFound{};
  }
}

static const TypeDescriptions* descriptions_record(Slice<const LabelDescription*> labels,
                                                   RecordRepresentation repr) {
  auto* d = make<TypeDescriptions>();
  d->kind = TypeKind::Kind::Type_record;
  d->labels = labels;
  d->record_repr = repr;
  return d;
}

static const TypeData* type_of_cstr(Path::t path, const ConstructorDescription* cstr) {
  const TypeDeclaration* decl = cstr->cstr_inlined;
  if (!decl || decl->type_kind->kind != TypeKind::Kind::Type_record)
    throw std::logic_error("Env.type_of_cstr");
  std::vector<const LabelDescription*> labels;
  for (auto& [id, l] : datarepr::labels_of_type(path, decl)) labels.push_back(l);
  return make<TypeData>(decl, descriptions_record(slice(labels), decl->type_kind->record_repr),
                        shape::leaf(decl->type_uid));
}

static const TypeDescriptions* abstract_descr(const TypeDeclaration* decl) {
  auto* d = make<TypeDescriptions>();
  d->kind = TypeKind::Kind::Type_abstract;
  d->origin = type_origin(decl);
  return d;
}

static const TypeData* find_type_data(Path::t p, t env);

const ConstructorDescription* find_cstr(Path::t p, std::string_view name, t env) {
  const TypeData* tda = find_type_data(p, env);
  if (tda->tda_descriptions->kind != TypeKind::Kind::Type_variant) throw NotFound{};
  for (auto* c : tda->tda_descriptions->constructors)
    if (c->cstr_name == name) return c;
  throw NotFound{};
}

static const TypeData* find_type_data(Path::t p, t env) {
  if (auto* decl = env->local_constraints.find_opt(p))
    return make<TypeData>(*decl, abstract_descr(*decl), shape::leaf((*decl)->type_uid));
  switch (p->kind) {
    case Path::Kind::Pident: return idtbl_find_same(p->id, env->types);
    case Path::Kind::Pdot: {
      StructureComponents* sc = find_structure_components(p->p1, env);
      auto* x = sc->comp_types.find_opt(p->s);
      if (!x) throw NotFound{};
      return *x;
    }
    case Path::Kind::Papply: throw NotFound{};
    case Path::Kind::Pextra_ty:
      if (p->extra == Path::Extra::Pcstr_ty) return type_of_cstr(p, find_cstr(p->p1, p->s, env));
      return type_of_cstr(p, find_extension_full(p->p1, env)->cda_description);
  }
  throw NotFound{};
}

const LabelDescription* find_label(Path::t p, std::string_view name, t env) {
  const TypeData* tda = find_type_data(p, env);
  if (tda->tda_descriptions->kind != TypeKind::Kind::Type_record) throw NotFound{};
  for (auto* l : tda->tda_descriptions->labels)
    if (l->lbl_name == name) return l;
  throw NotFound{};
}

const lz::ModtypeDecl* find_modtype_lazy(Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: return idtbl_find_same(p->id, env->modtypes)->mtda_declaration;
    case Path::Kind::Pdot: {
      StructureComponents* sc = find_structure_components(p->p1, env);
      auto* x = sc->comp_modtypes.find_opt(p->s);
      if (!x) throw NotFound{};
      return (*x)->mtda_declaration;
    }
    default: throw NotFound{};
  }
}

const ModtypeDeclaration* find_modtype(Path::t p, t env) {
  return lz::force_modtype_decl(find_modtype_lazy(p, env));
}

static const ClassData* find_class_full(Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: return idtbl_find_same(p->id, env->classes);
    case Path::Kind::Pdot: {
      StructureComponents* sc = find_structure_components(p->p1, env);
      auto* x = sc->comp_classes.find_opt(p->s);
      if (!x) throw NotFound{};
      return *x;
    }
    default: throw NotFound{};
  }
}

const ClassTypeDeclaration* find_cltype(Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: return idtbl_find_same(p->id, env->cltypes)->cltda_declaration;
    case Path::Kind::Pdot: {
      StructureComponents* sc = find_structure_components(p->p1, env);
      auto* x = sc->comp_cltypes.find_opt(p->s);
      if (!x) throw NotFound{};
      return (*x)->cltda_declaration;
    }
    default: throw NotFound{};
  }
}

const ValueDescription* find_value(Path::t p, t env) {
  return find_value_full(p, env)->vda_description;
}
const ClassDeclaration* find_class(Path::t p, t env) {
  return find_class_full(p, env)->clda_declaration;
}
const ConstructorDescription* find_ident_constructor(Ident::t id, t env) {
  return tycomp_find_same(id, env->constrs)->cda_description;
}
const LabelDescription* find_ident_label(Ident::t id, t env) {
  return tycomp_find_same(id, env->labels);
}
const TypeDeclaration* find_type(Path::t p, t env) { return find_type_data(p, env)->tda_declaration; }
const TypeDescriptions* find_type_descrs(Path::t p, t env) {
  return find_type_data(p, env)->tda_descriptions;
}

// ---- addresses --------------------------------------------------------------------
static const Address* get_address(AddressLazy* a);

const Address* find_module_address(Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: return get_address(find_ident_module(p->id, env)->mda_address);
    case Path::Kind::Pdot: {
      StructureComponents* c = find_structure_components(p->p1, env);
      auto* m = c->comp_modules.find_opt(p->s);
      if (!m) throw NotFound{};
      return get_address((*m)->mda_address);
    }
    default: throw NotFound{};
  }
}

static const Address* force_address(AddressUnforced a) {
  if (a.is_projection) return make<Address>(false, nullptr, get_address(a.parent), a.pos);
  return find_module_address(a.path, a.env);
}

static const Address* get_address(AddressLazy* a) { return a->force(force_address); }

// Lazy_backtrack.create_failed Not_found
static AddressLazy* failed_address() {
  auto* a = make<AddressLazy>(AddressLazy::Kind::Raise);
  a->exn = std::make_exception_ptr(NotFound{});
  return a;
}

const Address* find_value_address(Path::t p, t env) {
  return get_address(find_value_full(p, env)->vda_address);
}
const Address* find_class_address(Path::t p, t env) {
  return get_address(find_class_full(p, env)->clda_address);
}

const Address* find_constructor_address(Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: {
      const ConstructorData* cda = tycomp_find_same(p->id, env->constrs);
      if (!cda->cda_address) throw NotFound{};
      return get_address(cda->cda_address);
    }
    case Path::Kind::Pdot: {
      StructureComponents* c = find_structure_components(p->p1, env);
      auto* l = c->comp_constrs.find_opt(p->s);
      if (!l) throw NotFound{};
      for (auto* cda : *l)
        if (cda->cda_address) return get_address(cda->cda_address);
      throw NotFound{};
    }
    default: throw NotFound{};
  }
}

const TypeDeclaration* find_hash_type(Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: {
      auto r = idtbl_find_name(wrap_identity<const CltypeData*>, false, ident::name(p->id),
                               env->cltypes);
      return r.second->cltda_declaration->clty_hash_type;
    }
    case Path::Kind::Pdot: {
      StructureComponents* c = find_structure_components(p->p1, env);
      auto* x = c->comp_cltypes.find_opt(p->s);
      if (!x) throw NotFound{};
      return (*x)->cltda_declaration->clty_hash_type;
    }
    default: throw NotFound{};
  }
}

// ---- required globals -------------------------------------------------------------
static std::vector<Ident::t> g_required_globals;  // most recent first
void reset_required_globals() { g_required_globals.clear(); }
std::vector<Ident::t> get_required_globals() { return g_required_globals; }
void add_required_global(Ident::t id) {
  if (ident::global(id) && !no_alias_deps &&
      std::none_of(g_required_globals.begin(), g_required_globals.end(),
                   [&](Ident::t x) { return ident::same(id, x); }))
    g_required_globals.insert(g_required_globals.begin(), id);
}

// ---- normalization -----------------------------------------------------------------
static Path::t expand_module_path(bool lax, t env, Path::t path);

static Path::t normalize_module_path_(bool lax, t env, Path::t path) {
  switch (path->kind) {
    case Path::Kind::Pident:
      if (lax && ident::persistent(path->id)) return path;  // fast path (avoids lookup)
      return expand_module_path(lax, env, path);
    case Path::Kind::Pdot: {
      Path::t p2 = normalize_module_path_(lax, env, path->p1);
      if (path->p1 == p2) return expand_module_path(lax, env, path);
      return expand_module_path(lax, env, Path::pdot(p2, path->s));
    }
    case Path::Kind::Papply: {
      Path::t p1 = normalize_module_path_(lax, env, path->p1);
      Path::t p2 = normalize_module_path_(true, env, path->p2);
      if (path->p1 == p1 && path->p2 == p2) return expand_module_path(lax, env, path);
      return expand_module_path(lax, env, Path::papply(p1, p2));
    }
    case Path::Kind::Pextra_ty:
      throw std::logic_error("Env.normalize_module_path");
  }
  return path;
}

static Path::t expand_module_path(bool lax, t env, Path::t path) {
  try {
    const lz::ModuleDecl* d = find_module_lazy_(true, path, env);
    if (d->mdl_type->kind != lz::Modtype::Kind::MtyL_alias) return path;
    Path::t path2 = normalize_module_path_(lax, env, d->mdl_type->path);
    if (lax || no_alias_deps) return path2;
    Ident::t id = path::head(path);
    if (ident::global(id) && !ident::same(id, path::head(path2))) add_required_global(id);
    return path2;
  } catch (const NotFound&) {
    if (lax || !(path->kind == Path::Kind::Pident && ident::persistent(path->id)))
      return path;
    throw;
  }
}

Path::t normalize_module_path(const Location* oloc, t env, Path::t path) {
  try {
    return normalize_module_path_(oloc == nullptr, env, path);
  } catch (const NotFound&) {
    if (!oloc) throw std::logic_error("Env.normalize_module_path");
    Error e(Error::Kind::Missing_module);
    e.loc = *oloc;
    e.path1 = path;
    e.path2 = normalize_module_path_(true, env, path);
    throw e;
  }
}

static Path::t normalize_path_prefix(const Location* oloc, t env, Path::t path) {
  switch (path->kind) {
    case Path::Kind::Pdot: {
      Path::t p2 = normalize_module_path(oloc, env, path->p1);
      return path->p1 == p2 ? path : Path::pdot(p2, path->s);
    }
    case Path::Kind::Pident: return path;
    case Path::Kind::Pextra_ty: {
      Path::t p2 = normalize_path_prefix(oloc, env, path->p1);
      return path->p1 == p2 ? path : Path::pextra_ty(p2, path->extra, path->s);
    }
    case Path::Kind::Papply: throw std::logic_error("Env.normalize_path_prefix");
  }
  return path;
}

Path::t normalize_type_path(const Location* oloc, t env, Path::t p) {
  return normalize_path_prefix(oloc, env, p);
}
Path::t normalize_value_path(const Location* oloc, t env, Path::t p) {
  return normalize_path_prefix(oloc, env, p);
}

Path::t normalize_modtype_path(t env, Path::t path) {
  path = normalize_path_prefix(nullptr, env, path);
  // expand_modtype_path
  try {
    const lz::ModtypeDecl* d = find_modtype_lazy(path, env);
    if (d->mtdl_type && d->mtdl_type->kind == lz::Modtype::Kind::MtyL_ident)
      return normalize_modtype_path(env, d->mtdl_type->path);
  } catch (const NotFound&) {
  }
  return path;
}

Path::t try_normalize_type_path(const Location* oloc, t env, Path::t p) {
  Path::t p2 = normalize_type_path(oloc, env, p);
  return path::same(p, p2) ? nullptr : p2;
}
Path::t try_normalize_modtype_path(t env, Path::t p) {
  Path::t p2 = normalize_modtype_path(env, p);
  return path::same(p, p2) ? nullptr : p2;
}

const ModuleDeclaration* find_module(Path::t p, t env) { return find_module_(false, p, env); }
const lz::ModuleDecl* find_module_lazy(Path::t p, t env) {
  return find_module_lazy_(false, p, env);
}

// ---- type / modtype expansion ---------------------------------------------------------
bool find_type_expansion_into(Path::t p, t env, bool opt, TypeExpansion& out) {
  const TypeDeclaration* decl = find_type(p, env);
  if (decl->type_manifest &&
      (opt || decl->type_private == PrivateFlag::Public || !type_kind_is_abstract(decl) ||
       has_constr_row(decl->type_manifest))) {
    out = {decl->type_params, decl->type_manifest, decl->type_expansion_scope};
    return true;
  }
  // The manifest type of Private abstract data types without private row
  // are still considered unknown to the type system.
  return false;
}

TypeExpansion find_type_expansion(Path::t p, t env) {
  TypeExpansion x;
  if (!find_type_expansion_into(p, env, false, x)) throw NotFound{};
  return x;
}

TypeExpansion find_type_expansion_opt(Path::t p, t env) {
  TypeExpansion x;
  if (!find_type_expansion_into(p, env, true, x)) throw NotFound{};
  return x;
}

const lz::Modtype* find_modtype_expansion_lazy(Path::t p, t env) {
  const lz::ModtypeDecl* d = find_modtype_lazy(p, env);
  if (!d->mtdl_type) throw NotFound{};
  return d->mtdl_type;
}

const ModuleType* find_modtype_expansion(Path::t p, t env) {
  return lz::force_modtype(find_modtype_expansion_lazy(p, env));
}

bool is_aliasable(Path::t p, t env) {
  switch (p->kind) {
    case Path::Kind::Pident: return env->not_aliasable.find_same_opt(p->id) == nullptr;
    case Path::Kind::Pdot:
    case Path::Kind::Pextra_ty: return is_aliasable(p->p1, env);
    case Path::Kind::Papply: return false;
  }
  return false;
}

// ---- copying types associated with values ----------------------------------------------
std::function<t(t)> make_copy_of_types(t env0) {
  auto memo = std::make_shared<std::unordered_map<long, TypeExpr*>>();
  auto copy = [memo](TypeExpr* ty) {
    if (auto it = memo->find(get_id(ty)); it != memo->end()) return it->second;
    TypeExpr* t2 = subst::type_expr(subst::identity(), ty);
    memo->emplace(get_id(ty), t2);
    return t2;
  };
  std::function<const ValueEntry*(const ValueEntry*)> f = [copy](const ValueEntry* e) {
    if (!e->bound) return e;
    auto* desc = make<ValueDescription>(*e->data->vda_description);
    desc->val_type = copy(desc->val_type);
    return static_cast<const ValueEntry*>(
        make<ValueEntry>(true, make<ValueData>(desc, e->data->vda_address, e->data->vda_shape)));
  };
  auto values = idtbl_map<const ValueEntry*, const ValueData*>(f, env0->values);
  return [values](t env) -> t {
    EnvT* e = copy_env(env);
    e->values = values;
    e->summary = summ(Summary{Summary::Kind::Env_copy_types, env->summary});
    return e;
  };
}

// IdTbl.find_all_idents name tbl: the idents bound to the name, most
// recent first, None for an opened module's component
template <class A, class B>
static void idtbl_find_all_idents(std::string_view name, const IdTbl<A, B>& tbl, std::vector<Ident::t>& out) {
  for (auto& [id, d] : tbl.current.find_all(name)) out.push_back(id);
  if (!tbl.layer) return;
  const auto* L = tbl.layer;
  if (L->is_open && L->components.find_opt(name)) out.push_back(nullptr);
  idtbl_find_all_idents(name, L->next, out);
}

// find_index_tbl ident tbl: the ident's position among the bindings of its name
template <class A, class B>
static std::optional<long> find_index_tbl(Ident::t id, const IdTbl<A, B>& tbl) {
  std::vector<Ident::t> lbs;
  idtbl_find_all_idents(ident::name(id), tbl, lbs);
  for (std::size_t i = 0; i < lbs.size(); ++i)
    if (lbs[i] && ident::same(id, lbs[i])) return static_cast<long>(i);
  return std::nullopt;
}

std::optional<long> find_value_index(Ident::t id, t env) { return find_index_tbl(id, env->values); }
std::optional<long> find_type_index(Ident::t id, t env) { return find_index_tbl(id, env->types); }
std::optional<long> find_module_index(Ident::t id, t env) { return find_index_tbl(id, env->modules); }
std::optional<long> find_modtype_index(Ident::t id, t env) { return find_index_tbl(id, env->modtypes); }
std::optional<long> find_class_index(Ident::t id, t env) { return find_index_tbl(id, env->classes); }
std::optional<long> find_cltype_index(Ident::t id, t env) { return find_index_tbl(id, env->cltypes); }

bool same_types(t e1, t e2) {
  return e1->types.current.same_as(e2->types.current) && e1->types.layer == e2->types.layer &&
         e1->modules.current.same_as(e2->modules.current) &&
         e1->modules.layer == e2->modules.layer;
}

// ---- iterating on an environment (ignoring the body of functors and not yet
// evaluated structures): the short-paths search of Out_type ----------------

// iter_env_cont: the continuations the current level's iteration queued, as
// (path, cont), consed (run_iter_cont reverses them)
static std::vector<std::pair<Path::t, IterCont>> g_iter_env_cont;

static bool scrape_alias_for_visit(t env, const lz::Modtype* mty) {
  for (;;) {
    if (mty->kind != lz::Modtype::Kind::MtyL_alias) return true;
    Path::t path = mty->path;
    if (path->kind == Path::Kind::Pident && ident::persistent(path->id) &&
        !g_persistent_env.looked_up(std::string(ident::name(path->id))))
      return false;
    // PR#6600: find_module may raise Not_found
    try {
      mty = find_module_lazy(path, env)->mdl_type;
    } catch (const NotFound&) {
      return false;
    }
  }
}

// IdTbl.iter wrap f tbl
template <class A, class B, class W>
static void idtbl_iter(W&& wrap, const std::function<void(Ident::t, Path::t, const A&)>& f,
                       const IdTbl<A, B>& tbl) {
  tbl.current.iter([&](Ident::t id, const A& desc) { f(id, Path::pident(id), desc); });
  if (!tbl.layer) return;
  const auto* L = tbl.layer;
  if (L->is_open) {
    L->components.iter([&](std::string_view s, const B& x) {
      long root_scope = path::scope(L->root);
      f(Ident::create_scoped(static_cast<int>(root_scope), s), Path::pdot(L->root, s), wrap(x));
    });
    idtbl_iter(wrap, f, L->next);
    return;
  }
  idtbl_iter<A, B>(wrap,
                   std::function<void(Ident::t, Path::t, const A&)>(
                       [&](Ident::t id, Path::t p, const A& desc) { f(id, p, L->f(desc)); }),
                   L->next);
}

using TypeIterFn = std::function<void(Path::t, Path::t, const TypeDeclaration*)>;

// iter_env's iter_components path path' mcomps: queue the continuation that
// visits one module's components (and queues its submodules')
static void queue_components(const TypeIterFn& f, t env, Path::t path, Path::t path2,
                             ModuleComponents* mcomps) {
  IterCont cont = [f, env, path, path2, mcomps] {
    bool visit = true;
    if (const ComponentsMaker* cm = mcomps->comps->get_arg()) visit = scrape_alias_for_visit(env, cm->cm_mty);
    if (!visit) return;
    const ModuleComponentsRepr* r = get_components(mcomps);
    if (!r->is_structure) return;  // Functor_comps
    const StructureComponents* comps = r->structure;
    comps->comp_types.iter([&](std::string_view s, const TypeData* d) {
      f(Path::pdot(path, s), Path::pdot(path2, s), d->tda_declaration);
    });
    comps->comp_modules.iter([&](std::string_view s, const ModuleData* mda) {
      queue_components(f, env, Path::pdot(path, s), Path::pdot(path2, s), mda->mda_components);
    });
  };
  g_iter_env_cont.emplace_back(path, std::move(cont));
}

// iter_types f env: iter_env wrap_identity (fun env -> env.types)
// (fun sc -> sc.comp_types) ... env, the unit -> unit closure
IterCont iter_types(const TypeIterFn& f, t env) {
  return [f, env] {
    idtbl_iter<const TypeData*, const TypeData*>(
        wrap_identity<const TypeData*>,
        [&](Ident::t id, Path::t p2, const TypeData* const& tda) { f(Path::pident(id), p2, tda->tda_declaration); },
        env->types);
    idtbl_iter<const ModuleEntry*, const ModuleData*>(
        wrap_module,
               [&](Ident::t id, Path::t path, const ModuleEntry* const& entry) {
                 switch (entry->kind) {
                   case ModuleEntry::Kind::Mod_unbound: return;
                   case ModuleEntry::Kind::Mod_local:
                     queue_components(f, env, Path::pident(id), path, entry->data->mda_components);
                     return;
                   case ModuleEntry::Kind::Mod_persistent: {
                     const ModuleData* const* data =
                         g_persistent_env.find_in_cache(std::string(ident::name(id)));
                     if (!data) return;
                     queue_components(f, env, Path::pident(id), path, (*data)->mda_components);
                     return;
                   }
                 }
               },
               env->modules);
  };
}

std::vector<std::pair<Path::t, IterCont>> run_iter_cont(const std::vector<IterCont>& l) {
  g_iter_env_cont.clear();
  for (const IterCont& c : l) c();
  std::vector<std::pair<Path::t, IterCont>> cont = std::move(g_iter_env_cont);  // List.rev of the consed list
  g_iter_env_cont.clear();
  return cont;
}

std::set<std::string> used_persistent() {
  std::set<std::string> r;
  g_persistent_env.fold([&](const std::string& s, const ModuleData* const&) { r.insert(s); });
  return r;
}

// find_all_comps wrap proj s (p, mda)
template <class A, class Proj>
static std::vector<std::pair<Path::t, A>> find_all_comps(Proj&& proj, std::string_view s, Path::t p,
                                                        const ModuleData* mda) {
  const ModuleComponentsRepr* r = get_components(mda->mda_components);
  if (!r->is_structure) return {};
  if (const A* c = proj(r->structure).find_opt(s)) return {{Path::pdot(p, s), *c}};
  return {};
}

static std::vector<std::pair<Path::t, const ModuleData*>> find_shadowed_comps(Path::t path, t env) {
  switch (path->kind) {
    case Path::Kind::Pident: {
      std::vector<std::pair<Path::t, const ModuleData*>> out;
      for (auto& [p, data] : idtbl_find_all(wrap_module, ident::name(path->id), env->modules))
        if (data->kind == ModuleEntry::Kind::Mod_local) out.emplace_back(p, data->data);
      return out;
    }
    case Path::Kind::Pdot: {
      std::vector<std::pair<Path::t, const ModuleData*>> out;
      for (auto& [p, mda] : find_shadowed_comps(path->p1, env))
        for (auto& x : find_all_comps<const ModuleData*>(
                 [](const StructureComponents* c) -> const auto& { return c->comp_modules; }, path->s, p, mda))
          out.push_back(x);
      return out;
    }
    default: return {};
  }
}

std::vector<Path::t> find_shadowed_types(Path::t path, t env) {
  std::vector<Path::t> out;
  switch (path->kind) {
    case Path::Kind::Pident:
      for (auto& [p, d] : idtbl_find_all(wrap_identity<const TypeData*>, ident::name(path->id), env->types))
        out.push_back(p);
      return out;
    case Path::Kind::Pdot:
      for (auto& [p, mda] : find_shadowed_comps(path->p1, env))
        for (auto& x : find_all_comps<const TypeData*>(
                 [](const StructureComponents* c) -> const auto& { return c->comp_types; }, path->s, p, mda))
          out.push_back(x.first);
      return out;
    default: return out;
  }
}

// ============================================================================
// Expand manifest module type names at the top of the given module type

static const lz::Modtype* scrape_alias_(t env, const lz::Modtype* mty, Path::t path) {
  using MK = lz::Modtype::Kind;
  if (mty->kind == MK::MtyL_ident) {
    try {
      return scrape_alias_(env, find_modtype_expansion_lazy(mty->path, env), path);
    } catch (const NotFound&) {
      return mty;
    }
  }
  if (mty->kind == MK::MtyL_alias) {
    try {
      return scrape_alias_(env, find_module_lazy_(false, mty->path, env)->mdl_type, mty->path);
    } catch (const NotFound&) {
      return mty;
    }
  }
  if (path) return strengthen(true, env, mty, path);
  return mty;
}

const lz::Modtype* scrape_alias_lazy(t env, const lz::Modtype* mty) {
  return scrape_alias_(env, mty, nullptr);
}

// Non-lazy version of scrape_alias
const ModuleType* scrape_alias(t env, const ModuleType* mty) {
  return lz::force_modtype(scrape_alias_(env, lz::of_modtype(mty), nullptr));
}

// Given a signature and a root path, prefix all idents in the signature by
// the root path and build the corresponding substitution.
static std::pair<std::vector<std::pair<const lz::SignatureItem*, Path::t>>, subst::t> prefix_idents(
    Path::t root, subst::t prefixing_sub, lz::Signature sg) {
  using K = SignatureItem::Kind;
  auto items = lz::force_signature_once(sg);
  std::vector<std::pair<const lz::SignatureItem*, Path::t>> out;
  for (auto* item : items) {
    Path::t p = Path::pdot(root, ident::name(item->id));
    switch (item->kind) {
      case K::Sig_value: break;
      case K::Sig_type:
      case K::Sig_typext:  // extend the substitution in case of an inlined record
      case K::Sig_class:   // pretend this is a type, cf. PR#6650
      case K::Sig_class_type:
        prefixing_sub = subst::add_type(item->id, p, prefixing_sub);
        break;
      case K::Sig_module:
        prefixing_sub = subst::add_module(item->id, p, prefixing_sub);
        break;
      case K::Sig_modtype:
        prefixing_sub = subst::add_modtype(item->id, p, prefixing_sub);
        break;
    }
    out.emplace_back(item, p);
  }
  return {std::move(out), prefixing_sub};
}

// ---- compute structure descriptions ----------------------------------------------
template <class X>
static StrMap<Slice<X>> add_to_tbl(std::string_view id, X decl, const StrMap<Slice<X>>& tbl) {
  std::vector<X> decls{decl};
  if (const Slice<X>* l = tbl.find_opt(id)) decls.insert(decls.end(), l->begin(), l->end());
  return tbl.add(id, slice(decls));
}

static AddressLazy* value_declaration_address(Ident::t id, const ValueDescription* decl) {
  if (decl->val_kind.kind == ValueKind::Kind::Val_prim) return failed_address();
  return AddressLazy::create_forced(make<Address>(true, id));
}
static AddressLazy* ident_address(Ident::t id) {
  return AddressLazy::create_forced(make<Address>(true, id));
}
static AddressLazy* module_declaration_address(t env, Ident::t id, ModulePresence presence,
                                               const lz::ModuleDecl* d) {
  if (presence == ModulePresence::Mp_absent) {
    if (d->mdl_type->kind != lz::Modtype::Kind::MtyL_alias)
      throw std::logic_error("Env.module_declaration_address");
    return AddressLazy::create(AddressUnforced{false, nullptr, 0, env, d->mdl_type->path});
  }
  return ident_address(id);
}

// ---- tracking usage -----------------------------------------------------------------
// 'a usage_tbl: ('a -> unit) Types.Uid.Tbl.t -- a Hashtbl, whose add shadows
// and whose find returns the latest binding

std::function<void(std::function<void()>)> add_delayed_check_forward = [](std::function<void()>) {
  throw std::logic_error("Env.add_delayed_check_forward");
};

namespace {
using UidKey = std::tuple<int, std::string, long, int>;
UidKey uid_key(const Uid& u) {
  return UidKey{static_cast<int>(u.kind), std::string(u.comp_unit), u.id, static_cast<int>(u.from)};
}
template <class Fn>
struct UsageTbl {
  std::map<UidKey, std::vector<Fn>> m;
  bool mem(const Uid& u) const { return m.count(uid_key(u)) != 0; }
  void add(const Uid& u, Fn f) { m[uid_key(u)].push_back(std::move(f)); }
  const Fn* find(const Uid& u) const {
    auto it = m.find(uid_key(u));
    return it == m.end() ? nullptr : &it->second.back();
  }
  void replace(const Uid& u, Fn f) {
    std::vector<Fn>& v = m[uid_key(u)];
    if (v.empty()) v.push_back(std::move(f));
    else v.back() = std::move(f);
  }
  void clear() { m.clear(); }
};
UsageTbl<std::function<void()>> value_declarations, type_declarations, module_declarations;

struct ConstructorUsages {
  bool cu_positive = false, cu_pattern = false, cu_exported_private = false;
};
void add_constructor_usage(ConstructorUsages& cu, ConstructorUsage usage) {
  switch (usage) {
    case ConstructorUsage::Positive: cu.cu_positive = true; break;
    case ConstructorUsage::Pattern: cu.cu_pattern = true; break;
    case ConstructorUsage::Exported_private: cu.cu_exported_private = true; break;
    case ConstructorUsage::Exported: cu.cu_positive = cu.cu_pattern = cu.cu_exported_private = true; break;
  }
}
std::optional<warnings::ConstructorUsage> constructor_usage_complaint(bool rebind, PrivateFlag priv,
                                                                      const ConstructorUsages& cu) {
  using W = warnings::ConstructorUsage;
  if (priv == PrivateFlag::Private && !rebind) return std::nullopt;
  if (rebind) {
    if (cu.cu_positive || cu.cu_pattern || cu.cu_exported_private) return std::nullopt;
    return W::Unused;
  }
  if (cu.cu_positive) return std::nullopt;
  if (!cu.cu_pattern && !cu.cu_exported_private) return W::Unused;
  if (cu.cu_pattern) return W::Not_constructed;
  return W::Only_exported_private;
}
UsageTbl<std::function<void(ConstructorUsage)>> used_constructors;

struct LabelUsages {
  bool lu_projection = false, lu_mutation = false, lu_construct = false;
};
void add_label_usage(LabelUsages& lu, LabelUsage usage) {
  switch (usage) {
    case LabelUsage::Projection: lu.lu_projection = true; break;
    case LabelUsage::Mutation: lu.lu_mutation = true; break;
    case LabelUsage::Construct: lu.lu_construct = true; break;
    case LabelUsage::Exported_private: lu.lu_projection = true; break;
    case LabelUsage::Exported: lu.lu_projection = lu.lu_mutation = lu.lu_construct = true; break;
  }
}
bool is_mutating_label_usage(LabelUsage u) { return u == LabelUsage::Mutation; }
std::optional<warnings::FieldUsage> label_usage_complaint(PrivateFlag priv, MutableFlag mut, const LabelUsages& lu) {
  using W = warnings::FieldUsage;
  if (priv == PrivateFlag::Private) {
    if (lu.lu_projection) return std::nullopt;
    return W::Unused;
  }
  if (mut == MutableFlag::Immutable) {
    if (lu.lu_projection) return std::nullopt;
    if (!lu.lu_construct) return W::Unused;
    return W::Not_read;
  }
  if (lu.lu_projection && lu.lu_mutation) return std::nullopt;
  if (!lu.lu_projection && !lu.lu_mutation && !lu.lu_construct) return W::Unused;
  if (!lu.lu_projection) return W::Not_read;
  return W::Not_mutated;
}
UsageTbl<std::function<void(LabelUsage)>> used_labels;
}  // namespace

void reset_declaration_caches() {
  value_declarations.clear();
  type_declarations.clear();
  module_declarations.clear();
  used_constructors.clear();
  used_labels.clear();
}

// ---- insertion of bindings by identifier + path ----------------------------------


// check_usage loc id uid warn tbl
static void check_usage(const Location& loc, Ident::t id, const Uid& uid, const CheckFn& warn,
                        UsageTbl<std::function<void()>>& tbl) {
  if (!loc.loc_ghost && uid::for_actual_declaration(uid) && warnings::is_active(warn(""))) {
    std::string name(ident::name(id));
    if (tbl.mem(uid)) return;
    auto used = std::make_shared<bool>(false);
    tbl.add(uid, [used] { *used = true; });
    if (!(name.empty() || name[0] == '_' || name[0] == '#'))
      add_delayed_check_forward([used, loc, warn, name] {
        if (!*used) location::prerr_warning(loc, warn(name));
      });
  }
}

static void check_value_name(std::string_view name, const Location& loc) {
  if (name.empty() || utf8_lexeme::starts_like_a_valid_identifier(name)) return;
  for (std::size_t i = 1; i < name.size(); ++i)
    if (name[i] == '#') {
      Error e(Error::Kind::Illegal_value_name);
      e.loc = loc;
      e.name = std::string(name);
      throw e;
    }
}

static t store_value(const CheckFn& check, Ident::t id, AddressLazy* addr, const ValueDescription* decl,
                     shape::t shape, t env) {
  check_value_name(ident::name(id), decl->val_loc);
  builtin_attributes::mark_alerts_used(decl->val_attributes);
  if (check) check_usage(decl->val_loc, id, decl->val_uid, check, value_declarations);
  EnvT* e = copy_env(env);
  e->values = idtbl_add(id, wrap_value(make<ValueData>(decl, addr, shape)), env->values);
  Summary s{Summary::Kind::Env_value, env->summary};
  s.id = id;
  s.value = decl;
  e->summary = summ(s);
  return e;
}

static t store_constructor(bool check, const TypeDeclaration* type_decl, Ident::t type_id, Ident::t cstr_id,
                           const ConstructorDescription* cstr, t env) {
  builtin_attributes::warning_scope(builtin_attributes::ast_attributes(cstr->cstr_attributes), [&] {
    if (check && !type_decl->type_loc.loc_ghost && warnings::is_active(37)) {
      std::string ty_name(ident::name(type_id));
      std::string name(cstr->cstr_name);
      Location loc = cstr->cstr_loc;
      const Uid& k = cstr->cstr_uid;
      PrivateFlag priv = type_decl->type_private;
      if (!used_constructors.mem(k)) {
        auto used = std::make_shared<ConstructorUsages>();
        used_constructors.add(k, [used](ConstructorUsage u) { add_constructor_usage(*used, u); });
        if (!(ty_name.empty() || ty_name[0] == '_'))
          add_delayed_check_forward([used, loc, name, priv, env] {
            if (std::optional<warnings::ConstructorUsage> c = constructor_usage_complaint(false, priv, *used))
              if (!is_in_signature(env)) {
                warnings::Warning w = warnings::Warning::with_s(warnings::Warning::K::Unused_constructor, name);
                w.cusage = *c;
                location::prerr_warning(loc, w);
              }
          });
      }
    }
  });
  builtin_attributes::mark_alerts_used(cstr->cstr_attributes);
  builtin_attributes::mark_warn_on_literal_pattern_used(cstr->cstr_attributes);
  EnvT* e = copy_env(env);
  e->constrs =
      tycomp_add(cstr_id,
                 static_cast<const ConstructorData*>(make<ConstructorData>(cstr, nullptr, shape::leaf(cstr->cstr_uid))),
                 env->constrs);
  return e;
}

static t store_label(bool check, const TypeDeclaration* type_decl, Ident::t type_id, Ident::t lbl_id,
                     const LabelDescription* lbl, t env) {
  builtin_attributes::warning_scope(builtin_attributes::ast_attributes(lbl->lbl_attributes), [&] {
    if (check && !type_decl->type_loc.loc_ghost && warnings::is_active(69)) {
      std::string ty_name(ident::name(type_id));
      PrivateFlag priv = type_decl->type_private;
      std::string name(lbl->lbl_name);
      Location loc = lbl->lbl_loc;
      MutableFlag mut = lbl->lbl_mut;
      const Uid& k = lbl->lbl_uid;
      if (!used_labels.mem(k)) {
        auto used = std::make_shared<LabelUsages>();
        used_labels.add(k, [used](LabelUsage u) { add_label_usage(*used, u); });
        if (!(ty_name.empty() || ty_name[0] == '_' || name[0] == '_'))
          add_delayed_check_forward([used, loc, name, priv, mut, env] {
            if (std::optional<warnings::FieldUsage> c = label_usage_complaint(priv, mut, *used))
              if (!is_in_signature(env)) {
                warnings::Warning w = warnings::Warning::with_s(warnings::Warning::K::Unused_field, name);
                w.fusage = *c;
                location::prerr_warning(loc, w);
              }
          });
      }
    }
  });
  builtin_attributes::mark_alerts_used(lbl->lbl_attributes);
  if (lbl->lbl_mut == MutableFlag::Mutable) builtin_attributes::mark_deprecated_mutable_used(lbl->lbl_attributes);
  EnvT* e = copy_env(env);
  e->labels = tycomp_add(lbl_id, lbl, env->labels);
  return e;
}

static t store_type(bool check, Ident::t id, const TypeDeclaration* info, shape::t shape, t env) {
  if (check)
    check_usage(info->type_loc, id, info->type_uid,
                [](std::string s) {
                  warnings::Warning w = warnings::Warning::with_s(warnings::Warning::K::Unused_type_declaration, s);
                  w.tdusage = warnings::TypeDeclarationUsage::Declaration;
                  return w;
                },
                type_declarations);
  Path::t path = Path::pident(id);
  auto* descrs = make<TypeDescriptions>();
  descrs->kind = info->type_kind->kind;
  switch (info->type_kind->kind) {
    case TypeKind::Kind::Type_variant: {
      auto constructors = datarepr::constructors_of_type(get_current_unit(), path, info);
      std::vector<const ConstructorDescription*> cs;
      for (auto& [cid, c] : constructors) cs.push_back(c);
      descrs->constructors = slice(cs);
      descrs->variant_repr = info->type_kind->variant_repr;
      for (auto& [cid, c] : constructors) env = store_constructor(check, info, id, cid, c, env);
      break;
    }
    case TypeKind::Kind::Type_record: {
      auto labels = datarepr::labels_of_type(path, info);
      std::vector<const LabelDescription*> ls;
      for (auto& [lid, l] : labels) ls.push_back(l);
      descrs->labels = slice(ls);
      descrs->record_repr = info->type_kind->record_repr;
      for (auto& [lid, l] : labels) env = store_label(check, info, id, lid, l, env);
      break;
    }
    case TypeKind::Kind::Type_abstract: descrs->origin = info->type_kind->origin; break;
    case TypeKind::Kind::Type_open: break;
    case TypeKind::Kind::Type_external: descrs->external = info->type_kind->external; break;
  }
  builtin_attributes::mark_alerts_used(info->type_attributes);
  EnvT* e = copy_env(env);
  e->types = idtbl_add(id, static_cast<const TypeData*>(make<TypeData>(info, descrs, shape)), env->types);
  Summary s{Summary::Kind::Env_type, env->summary};
  s.id = id;
  s.type = info;
  e->summary = summ(s);
  return e;
}

// Simplified version of store_type that doesn't compute and store
// constructor and label infos (components_of_module keeps track of type
// abbreviations with it).
static t store_type_infos(shape::t tda_shape, Ident::t id, const TypeDeclaration* info, t env) {
  EnvT* e = copy_env(env);
  e->types = idtbl_add(id, static_cast<const TypeData*>(make<TypeData>(info, abstract_descr(info), tda_shape)),
                       env->types);
  Summary s{Summary::Kind::Env_type, env->summary};
  s.id = id;
  s.type = info;
  e->summary = summ(s);
  return e;
}

static t store_extension(bool check, bool rebind, Ident::t id, AddressLazy* addr, const ExtensionConstructor* ext,
                         shape::t shape, t env) {
  Location loc = ext->ext_loc;
  const ConstructorDescription* cstr =
      datarepr::extension_descr(get_current_unit(), Path::pident(id), ext);
  auto* cda = make<ConstructorData>(cstr, addr, shape);
  builtin_attributes::mark_alerts_used(ext->ext_attributes);
  builtin_attributes::mark_warn_on_literal_pattern_used(ext->ext_attributes);
  builtin_attributes::warning_scope(builtin_attributes::ast_attributes(ext->ext_attributes), [&] {
    if (check && !loc.loc_ghost && warnings::is_active(38)) {
      PrivateFlag priv = ext->ext_private;
      bool is_exception = path::same(ext->ext_type_path, predef::paths().exn);
      std::string name(cstr->cstr_name);
      const Uid& k = cstr->cstr_uid;
      if (!used_constructors.mem(k)) {
        auto used = std::make_shared<ConstructorUsages>();
        used_constructors.add(k, [used](ConstructorUsage u) { add_constructor_usage(*used, u); });
        add_delayed_check_forward([used, loc, name, priv, rebind, is_exception, env] {
          if (std::optional<warnings::ConstructorUsage> c = constructor_usage_complaint(rebind, priv, *used))
            if (!is_in_signature(env)) {
              warnings::Warning w = warnings::Warning::with_s(warnings::Warning::K::Unused_extension, name);
              w.b = is_exception;
              w.cusage = *c;
              location::prerr_warning(loc, w);
            }
        });
      }
    }
  });
  EnvT* e = copy_env(env);
  e->constrs = tycomp_add(id, static_cast<const ConstructorData*>(cda), env->constrs);
  Summary s{Summary::Kind::Env_extension, env->summary};
  s.id = id;
  s.ext = ext;
  e->summary = summ(s);
  return e;
}

static StrMap<std::string_view> alerts_of_attrs(Attributes l) { return builtin_attributes::alerts_of_attrs(l); }

static t store_module(bool update_summary, const CheckFn& check, Ident::t id, AddressLazy* addr,
                      ModulePresence presence, const lz::ModuleDecl* d, shape::t shape, t env) {
  if (check) check_usage(d->mdl_loc, id, d->mdl_uid, check, module_declarations);
  builtin_attributes::mark_alerts_used(d->mdl_attributes);
  StrMap<std::string_view> alerts = alerts_of_attrs(d->mdl_attributes);
  ModuleComponents* comps = components_of_module(alerts, d->mdl_uid, env, subst::identity(),
                                                 Path::pident(id), addr, d->mdl_type, shape);
  auto* mda = make<ModuleData>(d, comps, addr, shape);
  EnvT* e = copy_env(env);
  if (update_summary) {
    Summary s{Summary::Kind::Env_module, env->summary};
    s.id = id;
    s.presence = presence;
    s.md = lz::force_module_decl(d);
    e->summary = summ(s);
  }
  e->modules = idtbl_add(id, wrap_module(mda), env->modules);
  return e;
}

static t store_modtype(bool update_summary, Ident::t id, const lz::ModtypeDecl* info, shape::t shape, t env) {
  builtin_attributes::mark_alerts_used(info->mtdl_attributes);
  EnvT* e = copy_env(env);
  if (update_summary) {
    Summary s{Summary::Kind::Env_modtype, env->summary};
    s.id = id;
    s.mtd = lz::force_modtype_decl(info);
    e->summary = summ(s);
  }
  e->modtypes =
      idtbl_add(id, static_cast<const ModtypeData*>(make<ModtypeData>(info, shape)), env->modtypes);
  return e;
}

static t store_class(Ident::t id, AddressLazy* addr, const ClassDeclaration* desc, shape::t shape, t env) {
  builtin_attributes::mark_alerts_used(desc->cty_attributes);
  EnvT* e = copy_env(env);
  e->classes =
      idtbl_add(id, static_cast<const ClassData*>(make<ClassData>(desc, addr, shape)), env->classes);
  Summary s{Summary::Kind::Env_class, env->summary};
  s.id = id;
  s.cls = desc;
  e->summary = summ(s);
  return e;
}

static t store_cltype(Ident::t id, const ClassTypeDeclaration* desc, shape::t shape, t env) {
  builtin_attributes::mark_alerts_used(desc->clty_attributes);
  EnvT* e = copy_env(env);
  e->cltypes = idtbl_add(id, static_cast<const CltypeData*>(make<CltypeData>(desc, shape)), env->cltypes);
  Summary s{Summary::Kind::Env_cltype, env->summary};
  s.id = id;
  s.clty = desc;
  e->summary = summ(s);
  return e;
}

static ComponentsResult components_of_module_maker(ComponentsMaker cm) {
  using MK = lz::Modtype::Kind;
  const lz::Modtype* mty = scrape_alias_(cm.cm_env, cm.cm_mty, nullptr);
  switch (mty->kind) {
    case MK::MtyL_signature: {
      auto* c = make<StructureComponents>();
      auto [items_and_paths, sub] = prefix_idents(cm.cm_path, cm.cm_prefixing_subst, mty->sign);
      t env = cm.cm_env;
      long pos = 0;
      auto next_address = [&]() {
        AddressLazy* a = AddressLazy::create(AddressUnforced{true, cm.cm_addr, pos});
        ++pos;
        return a;
      };
      using K = SignatureItem::Kind;
      for (auto& [item, path] : items_and_paths) {
        Ident::t id = item->id;
        switch (item->kind) {
          case K::Sig_value: {
            const ValueDescription* decl2 = subst::value_description(sub, item->value);
            AddressLazy* addr = item->value->val_kind.kind == ValueKind::Kind::Val_prim
                                    ? failed_address()
                                    : next_address();
            shape::t vda_shape = shape::proj(nullptr, cm.cm_shape, shape::item::value(id));
            c->comp_values = c->comp_values.add(ident::name(id), make<ValueData>(decl2, addr, vda_shape));
            break;
          }
          case K::Sig_type: {
            const TypeDeclaration* decl = item->type;
            const TypeDeclaration* final_decl = subst::type_declaration(sub, decl);
            set_static_row_name(final_decl, subst::type_path(sub, Path::pident(id)));
            auto* descrs = make<TypeDescriptions>();
            descrs->kind = decl->type_kind->kind;
            switch (decl->type_kind->kind) {
              case TypeKind::Kind::Type_variant: {
                std::vector<const ConstructorDescription*> cstrs;
                for (auto& [cid, d] :
                     datarepr::constructors_of_type(get_current_unit(), path, final_decl))
                  cstrs.push_back(d);
                for (auto* d : cstrs)
                  c->comp_constrs = add_to_tbl<const ConstructorData*>(
                      d->cstr_name, make<ConstructorData>(d, nullptr, shape::leaf(d->cstr_uid)), c->comp_constrs);
                descrs->constructors = slice(cstrs);
                descrs->variant_repr = decl->type_kind->variant_repr;
                break;
              }
              case TypeKind::Kind::Type_record: {
                std::vector<const LabelDescription*> lbls;
                for (auto& [lid, d] : datarepr::labels_of_type(path, final_decl))
                  lbls.push_back(d);
                for (auto* d : lbls)
                  c->comp_labels = add_to_tbl<LabelData>(d->lbl_name, d, c->comp_labels);
                descrs->labels = slice(lbls);
                descrs->record_repr = decl->type_kind->record_repr;
                break;
              }
              case TypeKind::Kind::Type_abstract: descrs->origin = decl->type_kind->origin; break;
              case TypeKind::Kind::Type_open: break;
              case TypeKind::Kind::Type_external: descrs->external = decl->type_kind->external; break;
            }
            shape::t shape = shape::proj(nullptr, cm.cm_shape, shape::item::type_(id));
            c->comp_types = c->comp_types.add(ident::name(id), make<TypeData>(final_decl, descrs, shape));
            env = store_type_infos(shape, id, decl, env);
            break;
          }
          case K::Sig_typext: {
            const ExtensionConstructor* ext2 = subst::extension_constructor(sub, item->ext);
            const ConstructorDescription* descr =
                datarepr::extension_descr(get_current_unit(), path, ext2);
            AddressLazy* addr = next_address();
            shape::t cda_shape = shape::proj(nullptr, cm.cm_shape, shape::item::extension_constructor(id));
            c->comp_constrs = add_to_tbl<const ConstructorData*>(
                ident::name(id), make<ConstructorData>(descr, addr, cda_shape), c->comp_constrs);
            break;
          }
          case K::Sig_module: {
            // The prefixed items get the same scope as [cm_path], the prefix.
            const lz::ModuleDecl* md2 = lz::module_decl(
                subst::Scoping::rescope(path::scope(cm.cm_path)), sub, item->md);
            AddressLazy* addr;
            if (item->presence == ModulePresence::Mp_absent) {
              if (item->md->mdl_type->kind != MK::MtyL_alias)
                throw std::logic_error("Env.components_of_module_maker");
              addr = AddressLazy::create(
                  AddressUnforced{false, nullptr, 0, env, item->md->mdl_type->path});
            } else {
              addr = next_address();
            }
            StrMap<std::string_view> alerts = alerts_of_attrs(item->md->mdl_attributes);
            shape::t shape = shape::proj(nullptr, cm.cm_shape, shape::item::module_(id));
            ModuleComponents* comps = components_of_module(alerts, item->md->mdl_uid, env, sub,
                                                           path, addr, item->md->mdl_type, shape);
            c->comp_modules =
                c->comp_modules.add(ident::name(id), make<ModuleData>(md2, comps, addr, shape));
            env = store_module(false, nullptr, id, addr, item->presence, item->md, shape, env);
            break;
          }
          case K::Sig_modtype: {
            const lz::ModtypeDecl* final_decl = lz::modtype_decl(
                subst::Scoping::rescope(path::scope(cm.cm_path)), sub, item->mtd);
            shape::t shape = shape::proj(nullptr, cm.cm_shape, shape::item::module_type(id));
            c->comp_modtypes = c->comp_modtypes.add(ident::name(id), make<ModtypeData>(final_decl, shape));
            env = store_modtype(false, id, item->mtd, shape, env);
            break;
          }
          case K::Sig_class: {
            const ClassDeclaration* decl2 = subst::class_declaration(sub, item->cls);
            AddressLazy* addr = next_address();
            shape::t shape = shape::proj(nullptr, cm.cm_shape, shape::item::class_(id));
            c->comp_classes = c->comp_classes.add(ident::name(id), make<ClassData>(decl2, addr, shape));
            break;
          }
          case K::Sig_class_type: {
            const ClassTypeDeclaration* decl2 = subst::cltype_declaration(sub, item->clty);
            shape::t shape = shape::proj(nullptr, cm.cm_shape, shape::item::class_type(id));
            c->comp_cltypes = c->comp_cltypes.add(ident::name(id), make<CltypeData>(decl2, shape));
            break;
          }
        }
      }
      return ComponentsResult{true, make<ModuleComponentsRepr>(true, c)};
    }
    case MK::MtyL_functor: {
      subst::t sub = cm.cm_prefixing_subst;
      subst::Scoping scoping = subst::Scoping::rescope(path::scope(cm.cm_path));
      // fcomp_arg and fcomp_res must be prefixed eagerly, because they are
      // interpreted in the outer environment.  (record: fcomp_res is after
      // fcomp_arg in the definition, so it is computed first)
      const ModuleType* res = lz::force_modtype(lz::modtype(scoping, sub, mty->res));
      auto* f = make<FunctorComponents>();
      if (!mty->param.is_unit) {
        f->fcomp_arg.is_unit = false;
        f->fcomp_arg.id = mty->param.id;
        f->fcomp_arg.some_obj = mty->param.some_obj;
        f->fcomp_arg.mty = lz::force_modtype(lz::modtype(scoping, sub, mty->param.mty));
      }
      f->fcomp_res = res;
      f->fcomp_shape = cm.cm_shape;
      return ComponentsResult{true, make<ModuleComponentsRepr>(false, nullptr, f)};
    }
    case MK::MtyL_ident:
      return ComponentsResult{false};
    case MK::MtyL_alias:
      return ComponentsResult{false, nullptr, mty->path};
  }
  return ComponentsResult{false};
}

// Compute the components of a functor application in a path.
static ModuleComponents* components_of_functor_appl(const Location& loc, Path::t f_path,
                                                    FunctorComponents* f_comp, Path::t arg,
                                                    t env) {
  if (auto it = f_comp->fcomp_cache.find(arg); it != f_comp->fcomp_cache.end())
    return it->second;
  Path::t p = Path::papply(f_path, arg);
  subst::t sub = subst::identity();
  if (!f_comp->fcomp_arg.is_unit && f_comp->fcomp_arg.id)
    sub = subst::add_module(f_comp->fcomp_arg.id, arg, subst::identity());
  // we have to apply eagerly instead of passing sub to components_of_module
  // because of the call to check_well_formed_module.
  const ModuleType* mty = subst::modtype(subst::Scoping::rescope(path::scope(p)), sub,
                                         f_comp->fcomp_res);
  AddressLazy* addr = failed_address();
  if (check_well_formed_module)
    check_well_formed_module(env, loc, "the signature of " + path::name(p), mty);
  shape::t shape_arg = shape_of_path(shape::SigComponentKind::Module, env, arg);
  shape::t shape = shape::app(nullptr, f_comp->fcomp_shape, shape_arg);
  ModuleComponents* comps =
      components_of_module({}, uid::internal_not_actually_unique(), env, subst::identity(), p,
                           addr, lz::of_modtype(mty), shape);
  f_comp->fcomp_cache.emplace(arg, comps);
  return comps;
}

// ---- insertion of bindings by identifier -------------------------------------------
t mark_not_aliasable(Ident::t id, t env) {
  EnvT* e = copy_env(env);
  e->not_aliasable = env->not_aliasable.add(id, true);
  Summary s{Summary::Kind::Env_not_aliasable, env->summary};
  s.id = id;
  e->summary = summ(s);
  return e;
}

// shape_or_leaf uid shape
static shape::t shape_or_leaf(const Uid& uid, shape::t shape) { return shape ? shape : shape::leaf(uid); }

// add_value ?check ?shape (the exported add_value has no ?shape)
static t add_value_shape(Ident::t id, const ValueDescription* desc, t env, const CheckFn& check, shape::t shape) {
  AddressLazy* addr = value_declaration_address(id, desc);
  shape = shape_or_leaf(desc->val_uid, shape);
  return store_value(check, id, addr, desc, shape, env);
}

t add_value(Ident::t id, const ValueDescription* desc, t env, const CheckFn& check) {
  return add_value_shape(id, desc, env, check, nullptr);
}

t add_type(bool check, Ident::t id, const TypeDeclaration* info, t env, shape::t shape) {
  shape = shape_or_leaf(info->type_uid, shape);
  return store_type(check, id, info, shape, env);
}

t add_extension(bool check, bool rebind, Ident::t id, const ExtensionConstructor* ext, t env, shape::t shape) {
  AddressLazy* addr = ident_address(id);
  shape = shape_or_leaf(ext->ext_uid, shape);
  return store_extension(check, rebind, id, addr, ext, shape, env);
}

t add_module_declaration(bool check, Ident::t id, ModulePresence presence,
                         const ModuleDeclaration* md0, t env, bool noalias, shape::t shape) {
  CheckFn chk;
  if (!check) {
  } else if (noalias && is_in_signature(env)) {
    // While recursive modules are also added with the noalias flag when
    // typing the recursive definitions, they are then added back without the
    // flag (to be aliased from the outside), and therefore could not throw
    // the warning, leaving only functor parameters
    chk = [](std::string s) { return warnings::Warning::with_s(warnings::Warning::K::Unused_functor_parameter, s); };
  } else {
    chk = [](std::string s) { return warnings::Warning::with_s(warnings::Warning::K::Unused_module, s); };
  }
  const lz::ModuleDecl* d = lz::of_module_decl(md0);
  AddressLazy* addr = module_declaration_address(env, id, presence, d);
  shape = shape_or_leaf(d->mdl_uid, shape);
  env = store_module(true, chk, id, addr, presence, d, shape, env);
  return noalias ? mark_not_aliasable(id, env) : env;
}

t add_module_declaration_lazy(bool update_summary, Ident::t id, ModulePresence presence,
                              const lz::ModuleDecl* d, t env) {
  AddressLazy* addr = module_declaration_address(env, id, presence, d);
  shape::t shape = shape::leaf(d->mdl_uid);
  return store_module(update_summary, nullptr, id, addr, presence, d, shape, env);
}

// add_modtype ?shape / add_class ?shape / add_cltype ?shape (the exported
// ones have no ?shape)
static t add_modtype_shape(Ident::t id, const ModtypeDeclaration* info, t env, shape::t shape) {
  shape = shape_or_leaf(info->mtd_uid, shape);
  return store_modtype(true, id, lz::of_modtype_decl(info), shape, env);
}
static t add_class_shape(Ident::t id, const ClassDeclaration* ty, t env, shape::t shape) {
  AddressLazy* addr = ident_address(id);
  shape = shape_or_leaf(ty->cty_uid, shape);
  return store_class(id, addr, ty, shape, env);
}
static t add_cltype_shape(Ident::t id, const ClassTypeDeclaration* ty, t env, shape::t shape) {
  shape = shape_or_leaf(ty->clty_uid, shape);
  return store_cltype(id, ty, shape, env);
}

t add_modtype(Ident::t id, const ModtypeDeclaration* info, t env) { return add_modtype_shape(id, info, env, nullptr); }

t add_modtype_lazy(bool update_summary, Ident::t id, const lz::ModtypeDecl* info, t env) {
  shape::t shape = shape::leaf(info->mtdl_uid);
  return store_modtype(update_summary, id, info, shape, env);
}

t add_class(Ident::t id, const ClassDeclaration* ty, t env) { return add_class_shape(id, ty, env, nullptr); }

t add_cltype(Ident::t id, const ClassTypeDeclaration* ty, t env) { return add_cltype_shape(id, ty, env, nullptr); }

t add_module(Ident::t id, ModulePresence presence, const ModuleType* mty, t env, bool noalias, shape::t shape) {
  return add_module_declaration(false, id, presence, md(mty), env, noalias, shape);
}

t add_module_lazy(bool update_summary, Ident::t id, ModulePresence presence,
                  const lz::Modtype* mty, t env) {
  auto* d = make<lz::ModuleDecl>(mty, Attributes{}, location::none(),
                                 uid::internal_not_actually_unique());
  return add_module_declaration_lazy(update_summary, id, presence, d, env);
}

t add_local_constraint(Path::t path, const TypeDeclaration* info, t env) {
  EnvT* e = copy_env(env);
  e->local_constraints = env->local_constraints.add(path, info);
  return e;
}

// ---- insertion of bindings by name ---------------------------------------------------
std::pair<Ident::t, t> enter_value(std::string_view name, const ValueDescription* desc, t env,
                                   const CheckFn& check) {
  Ident::t id = Ident::create_local(name);
  AddressLazy* addr = value_declaration_address(id, desc);
  return {id, store_value(check, id, addr, desc, shape::leaf(desc->val_uid), env)};
}

std::pair<Ident::t, t> enter_type(int scope, std::string_view name, const TypeDeclaration* info,
                                  t env) {
  Ident::t id = Ident::create_scoped(scope, name);
  return {id, store_type(true, id, info, shape::leaf(info->type_uid), env)};
}

t reenter_type(Ident::t id, const TypeDeclaration* info, t env) {
  return store_type(true, id, info, shape::leaf(info->type_uid), env);
}

std::pair<Ident::t, t> enter_extension(int scope, bool rebind, std::string_view name,
                                       const ExtensionConstructor* ext, t env) {
  Ident::t id = Ident::create_scoped(scope, name);
  AddressLazy* addr = ident_address(id);
  shape::t shape = shape::leaf(ext->ext_uid);
  return {id, store_extension(true, rebind, id, addr, ext, shape, env)};
}

std::pair<Ident::t, t> enter_module_declaration(int scope, std::string_view name,
                                                ModulePresence presence,
                                                const ModuleDeclaration* md0, t env,
                                                bool noalias, shape::t shape) {
  Ident::t id = Ident::create_scoped(scope, name);
  return {id, add_module_declaration(true, id, presence, md0, env, noalias, shape)};
}

std::pair<Ident::t, t> enter_modtype(int scope, std::string_view name,
                                     const ModtypeDeclaration* mtd, t env) {
  Ident::t id = Ident::create_scoped(scope, name);
  shape::t shape = shape::leaf(mtd->mtd_uid);
  return {id, store_modtype(true, id, lz::of_modtype_decl(mtd), shape, env)};
}

std::pair<Ident::t, t> enter_class(int scope, std::string_view name, const ClassDeclaration* desc,
                                   t env) {
  Ident::t id = Ident::create_scoped(scope, name);
  AddressLazy* addr = ident_address(id);
  return {id, store_class(id, addr, desc, shape::leaf(desc->cty_uid), env)};
}

std::pair<Ident::t, t> enter_cltype(int scope, std::string_view name,
                                    const ClassTypeDeclaration* desc, t env) {
  Ident::t id = Ident::create_scoped(scope, name);
  return {id, store_cltype(id, desc, shape::leaf(desc->clty_uid), env)};
}

std::pair<Ident::t, t> enter_module(int scope, std::string_view name, ModulePresence presence,
                                    const ModuleType* mty, t env, bool noalias) {
  return enter_module_declaration(scope, name, presence, md(mty), env, noalias);
}

// ---- insertion of all components of a signature --------------------------------------
// add_item (map, mod_shape) comp env: with a module shape, each item's
// shape is its projection, recorded in the map too
static t add_item(shape::ItemMap& map, shape::t mod_shape, const SignatureItem* comp, t env) {
  using K = SignatureItem::Kind;
  auto proj_shape = [&](const shape::Item& item) -> shape::t {
    if (!mod_shape) return nullptr;
    shape::t s = shape::proj(nullptr, mod_shape, item);
    map = shape::map::add(map, item, s);
    return s;
  };
  switch (comp->kind) {
    case K::Sig_value: {
      shape::t sh = proj_shape(shape::item::value(comp->id));
      return add_value_shape(comp->id, comp->value, env, nullptr, sh);
    }
    case K::Sig_type: {
      shape::t sh = proj_shape(shape::item::type_(comp->id));
      return add_type(false, comp->id, comp->type, env, sh);
    }
    case K::Sig_typext: {
      shape::t sh = proj_shape(shape::item::extension_constructor(comp->id));
      return add_extension(false, false, comp->id, comp->ext, env, sh);
    }
    case K::Sig_module: {
      shape::t sh = proj_shape(shape::item::module_(comp->id));
      return add_module_declaration(false, comp->id, comp->presence, comp->md, env, false, sh);
    }
    case K::Sig_modtype: {
      shape::t sh = proj_shape(shape::item::module_type(comp->id));
      return add_modtype_shape(comp->id, comp->mtd, env, sh);
    }
    case K::Sig_class: {
      shape::t sh = proj_shape(shape::item::class_(comp->id));
      return add_class_shape(comp->id, comp->cls, env, sh);
    }
    case K::Sig_class_type: {
      shape::t sh = proj_shape(shape::item::class_type(comp->id));
      return add_cltype_shape(comp->id, comp->clty, env, sh);
    }
  }
  return env;
}

static t add_signature_shape(shape::ItemMap& map, shape::t mod_shape, Signature sg, t env) {
  for (auto* comp : sg) env = add_item(map, mod_shape, comp, env);
  return env;
}

t add_signature(Signature sg, t env) {
  shape::ItemMap map = shape::map::empty();
  return add_signature_shape(map, nullptr, sg, env);
}

std::pair<Signature, t> enter_signature(int scope, Signature sg, t env, shape::t mod_shape) {
  Signature sg2 = subst::signature(subst::Scoping::rescope(scope), subst::identity(), sg);
  shape::ItemMap map = shape::map::empty();
  t env2 = add_signature_shape(map, mod_shape, sg2, env);
  return {sg2, env2};
}

SignatureAndShape enter_signature_and_shape(int scope, shape::ItemMap parent_shape, shape::t mod_shape,
                                            Signature sg, t env) {
  Signature sg2 = subst::signature(subst::Scoping::rescope(scope), subst::identity(), sg);
  t env2 = add_signature_shape(parent_shape, mod_shape, sg2, env);
  return {sg2, parent_shape, env2};
}

// ---- shapes of the bindings ----------------------------------------------------------
shape::t find_shape(t env, shape::SigComponentKind ns, Ident::t id) {
  using NS = shape::SigComponentKind;
  switch (ns) {
    case NS::Type: return idtbl_find_same(id, env->types)->tda_shape;
    case NS::Constructor:
      return shape::leaf(tycomp_find_same(id, env->constrs)->cda_description->cstr_uid);
    case NS::Label: return shape::leaf(tycomp_find_same(id, env->labels)->lbl_uid);
    case NS::Extension_constructor: return tycomp_find_same(id, env->constrs)->cda_shape;
    case NS::Value: {
      const ValueEntry* v = idtbl_find_same(id, env->values);
      if (!v->bound) throw NotFound{};
      return v->data->vda_shape;
    }
    case NS::Module: {
      std::optional<const ModuleEntry*> found = idtbl_find_same_opt(id, env->modules);
      if (!found) {
        if (ident::persistent(id) && !current_unit_is_ident(id))
          return shape::for_persistent_unit(ident::name(id));
        throw NotFound{};
      }
      const ModuleEntry* m = *found;
      switch (m->kind) {
        case ModuleEntry::Kind::Mod_local: return m->data->mda_shape;
        case ModuleEntry::Kind::Mod_persistent: return shape::for_persistent_unit(ident::name(id));
        case ModuleEntry::Kind::Mod_unbound: throw std::logic_error("Env.find_shape");
      }
      throw std::logic_error("Env.find_shape");
    }
    case NS::Module_type: return idtbl_find_same(id, env->modtypes)->mtda_shape;
    case NS::Class: return idtbl_find_same(id, env->classes)->clda_shape;
    case NS::Class_type: return idtbl_find_same(id, env->cltypes)->cltda_shape;
  }
  throw std::logic_error("Env.find_shape");
}

shape::t shape_of_path(shape::SigComponentKind ns, t env, Path::t path) {
  return shape::of_path([env](shape::SigComponentKind k, Ident::t id) { return find_shape(env, k, id); }, ns,
                        path);
}

std::optional<Uid> find_uid(shape::SigComponentKind ns, Path::t path, t env) {
  using NS = shape::SigComponentKind;
  switch (ns) {
    case NS::Value: case NS::Class: path = normalize_value_path(nullptr, env, path); break;
    case NS::Type: case NS::Constructor: case NS::Label: case NS::Extension_constructor: case NS::Class_type:
      path = normalize_type_path(nullptr, env, path);
      break;
    case NS::Module: path = normalize_module_path(nullptr, env, path); break;
    case NS::Module_type: path = normalize_modtype_path(env, path); break;
  }
  // find_path_extra: Pextra_ty (ty, Pcstr_ty name) -> ty, name
  auto path_extra = [](Path::t p) -> std::pair<Path::t, std::string_view> {
    if (p->kind == Path::Kind::Pextra_ty && p->extra == Path::Extra::Pcstr_ty) return {p->p1, p->s};
    throw NotFound{};
  };
  try {
    switch (ns) {
      case NS::Value: return find_value(path, env)->val_uid;
      case NS::Extension_constructor: return find_extension_full(path, env)->cda_description->cstr_uid;
      case NS::Constructor: {
        auto [ty, cstr] = path_extra(path);
        return find_cstr(ty, cstr, env)->cstr_uid;
      }
      case NS::Label: {
        auto [ty, f] = path_extra(path);
        return find_label(ty, f, env)->lbl_uid;
      }
      case NS::Type: return find_type(path, env)->type_uid;
      case NS::Module: return find_module(path, env)->md_uid;
      case NS::Module_type: return find_modtype(path, env)->mtd_uid;
      case NS::Class: return find_class(path, env)->cty_uid;
      case NS::Class_type: return find_cltype(path, env)->clty_uid;
    }
  } catch (const NotFound&) {
  }
  return std::nullopt;
}

// ---- "unbound" bindings -----------------------------------------------------------
t enter_unbound_value(std::string_view name, ValueUnboundReason reason, t env) {
  Ident::t id = Ident::create_local(name);
  EnvT* e = copy_env(env);
  e->values = idtbl_add(id, static_cast<const ValueEntry*>(make<ValueEntry>(false, nullptr, reason)),
                        env->values);
  Summary s{Summary::Kind::Env_value_unbound, env->summary};
  s.name = zborrow(name);
  s.value_reason = reason;
  e->summary = summ(s);
  return e;
}

t enter_unbound_module(std::string_view name, ModuleUnboundReason reason, t env) {
  Ident::t id = Ident::create_local(name);
  EnvT* e = copy_env(env);
  e->modules = idtbl_add(
      id,
      static_cast<const ModuleEntry*>(
          make<ModuleEntry>(ModuleEntry::Kind::Mod_unbound, nullptr, reason)),
      env->modules);
  Summary s{Summary::Kind::Env_module_unbound, env->summary};
  s.name = zborrow(name);
  s.module_reason = reason;
  e->summary = summ(s);
  return e;
}

// ---- open a signature path ----------------------------------------------------------
// A slot: the component's name and, when it shadows a binding,
// check_shadowing's kind of the shadowed binding (None: no report)
using Slot = std::function<void(std::string_view, std::optional<std::string>)>;

// check_shadowing env: the kind of a shadowed binding worth reporting
template <class A>
static const UsingFn<A>* mk_using(const Slot& slot, std::function<std::optional<std::string>(const std::pair<A, A>&)> kind) {
  if (!slot) return nullptr;
  return make<UsingFn<A>>([slot, kind](std::string_view name, const std::pair<A, A>* both) {
    slot(name, both ? kind(*both) : std::nullopt);
  });
}

static t add_components(const Slot& slot, t env, Path::t root, t env0, const StructureComponents* comps) {
  EnvT* e = copy_env(env0);
  Summary s{Summary::Kind::Env_open, env0->summary};
  s.path = root;
  e->summary = summ(s);
  auto str = [](const char* k) { return std::optional<std::string>(k); };
  e->constrs = tycomp_add_open(
      root, comps->comp_constrs, env0->constrs,
      mk_using<const ConstructorData*>(slot, [env, str](const std::pair<const ConstructorData*, const ConstructorData*>& p)
                                             -> std::optional<std::string> {
        if (!same_constr(env, p.first->cda_description->cstr_res, p.second->cda_description->cstr_res))
          return str("constructor");
        return std::nullopt;
      }));
  e->labels = tycomp_add_open(root, comps->comp_labels, env0->labels,
                              mk_using<LabelData>(slot, [env, str](const std::pair<LabelData, LabelData>& p)
                                                      -> std::optional<std::string> {
                                if (!same_constr(env, p.first->lbl_res, p.second->lbl_res)) return str("label");
                                return std::nullopt;
                              }));
  e->values = idtbl_add_open(root, comps->comp_values, env0->values,
                             mk_using<const ValueEntry*>(slot, [str](const std::pair<const ValueEntry*, const ValueEntry*>& p)
                                                             -> std::optional<std::string> {
                               if (!p.first->bound) return std::nullopt;
                               return str("value");
                             }));
  e->types = idtbl_add_open(root, comps->comp_types, env0->types,
                            mk_using<const TypeData*>(slot, [str](const auto&) { return str("type"); }));
  e->modtypes = idtbl_add_open(root, comps->comp_modtypes, env0->modtypes,
                               mk_using<const ModtypeData*>(slot, [str](const auto&) { return str("module type"); }));
  e->classes = idtbl_add_open(root, comps->comp_classes, env0->classes,
                              mk_using<const ClassData*>(slot, [str](const auto&) { return str("class"); }));
  e->cltypes = idtbl_add_open(root, comps->comp_cltypes, env0->cltypes,
                              mk_using<const CltypeData*>(slot, [str](const auto&) { return str("class type"); }));
  e->modules = idtbl_add_open(root, comps->comp_modules, env0->modules,
                              mk_using<const ModuleEntry*>(slot, [str](const std::pair<const ModuleEntry*, const ModuleEntry*>& p)
                                                               -> std::optional<std::string> {
                                if (p.first->kind == ModuleEntry::Kind::Mod_unbound) return std::nullopt;
                                return str("module");
                              }));
  return e;
}

static OpenResult open_signature_(const Slot& slot, Path::t root, t env0) {
  ComponentsResult r;
  try {
    r = get_components_res(find_module_components(root, env0));
  } catch (const NotFound&) {
    return {OpenResult::Kind::Not_found};
  }
  if (!r.ok) return {OpenResult::Kind::Not_found};
  if (!r.repr->is_structure) return {OpenResult::Kind::Functor};
  return {OpenResult::Kind::Ok, add_components(slot, env0, root, env0, r.repr->structure)};
}

OpenResult open_signature(OverrideFlag ovf, Path::t root, t env, const Location& loc, bool toplevel,
                          std::shared_ptr<bool> used_slot) {
  if (!used_slot) used_slot = std::make_shared<bool>(false);
  warnings::Warning unused = warnings::Warning::with_s(
      ovf == OverrideFlag::Fresh ? warnings::Warning::K::Unused_open : warnings::Warning::K::Unused_open_bang,
      path::name(root));
  bool warn_unused = warnings::is_active(unused);
  bool warn_shadow_id = warnings::is_active(44);
  bool warn_shadow_lc = warnings::is_active(45);
  if (!toplevel && !loc.loc_ghost && (warn_unused || warn_shadow_id || warn_shadow_lc)) {
    std::shared_ptr<bool> used = used_slot;
    if (warn_unused)
      add_delayed_check_forward([used, loc, unused] {
        if (!*used) {
          *used = true;
          location::prerr_warning(loc, unused);
        }
      });
    auto shadowed = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
    Slot slot = [ovf, shadowed, loc, used](std::string_view s, std::optional<std::string> kind) {
      if (kind && ovf == OverrideFlag::Fresh &&
          std::find(shadowed->begin(), shadowed->end(), std::make_pair(*kind, std::string(s))) == shadowed->end()) {
        shadowed->insert(shadowed->begin(), {*kind, std::string(s)});
        warnings::Warning w = warnings::Warning::make(
            *kind == "label" || *kind == "constructor" ? warnings::Warning::K::Open_shadow_label_constructor
                                                       : warnings::Warning::K::Open_shadow_identifier);
        w.s = *kind;
        w.s2 = std::string(s);
        location::prerr_warning(loc, w);
      }
      *used = true;
    };
    // (the slot's check_shadowing reads the environment before the open)
    return open_signature_(slot, root, env);
  }
  return open_signature_(nullptr, root, env);
}

OpenResult open_pers_signature(std::string_view name, t env) {
  OpenResult r = open_signature_(nullptr, Path::pident(Ident::create_persistent(name)), env);
  if (r.kind == OpenResult::Kind::Functor)
    throw std::logic_error("Env.open_pers_signature: a compilation unit cannot be a functor");
  return r;
}

struct ExitEx {};

static const Summary* filter_open_summary(const Summary* s, Path::t root) {
  switch (s->kind) {
    case Summary::Kind::Env_empty: throw ExitEx{};
    case Summary::Kind::Env_open:
      if (path::same(s->path, root)) return s->next;
      throw ExitEx{};
    default: {
      Summary c = *s;
      c.next = filter_open_summary(s->next, root);
      return summ(c);
    }
  }
}

t remove_last_open(Path::t root, t env0) {
  const Summary* s;
  try {
    s = filter_open_summary(env0->summary, root);
  } catch (const ExitEx&) {
    return nullptr;
  }
  EnvT* e = copy_env(env0);
  e->summary = s;
  e->constrs = tycomp_remove_last_open(root, env0->constrs);
  e->labels = tycomp_remove_last_open(root, env0->labels);
  e->values = idtbl_remove_last_open(root, env0->values);
  e->types = idtbl_remove_last_open(root, env0->types);
  e->modtypes = idtbl_remove_last_open(root, env0->modtypes);
  e->classes = idtbl_remove_last_open(root, env0->classes);
  e->cltypes = idtbl_remove_last_open(root, env0->cltypes);
  e->modules = idtbl_remove_last_open(root, env0->modules);
  return e;
}

// Read a signature from a file (Persistent_env.read)
Signature read_signature_named(std::string_view modname, const std::string& filename) {
  const ModuleData* mda = g_persistent_env.read(read_sign_of_cmi, std::string(modname), filename, modname);
  const ModuleDeclaration* d = lz::force_module_decl(mda->mda_declaration);
  if (d->md_type->kind != ModuleType::Kind::Mty_signature)
    throw std::logic_error("Env.read_signature");
  return d->md_type->sign;
}
Signature read_signature(const std::string& modname, const std::string& filename) {
  const ModuleData* mda = g_persistent_env.read(read_sign_of_cmi, modname, filename);  // read_pers_mod
  const ModuleDeclaration* d = lz::force_module_decl(mda->mda_declaration);
  if (d->md_type->kind != ModuleType::Kind::Mty_signature)
    throw std::logic_error("Env.read_signature");
  return d->md_type->sign;
}

// Make the initial environment
// Env.initial is a value, computed once while env.ml initializes -- before
// the first Types.reset, so its declarations' type ids are module-init ids
// and the per-unit id counter restarts at -1 after them.  (Typemod's
// install_forward_refs forces it at the port's module-init time.)
t initial() {
  static const t initial_env = [] {
    install_forward_refs();
    predef::init();
    ZoneScope perm(permanent_zone());
    return predef::build_initial_env<t>(
        [](Ident::t id, const TypeDeclaration* d, t env) { return add_type(false, id, d, env); },
        [](Ident::t id, const ExtensionConstructor* e, t env) {
          return add_extension(false, false, id, e, env);
        },
        empty());
  }();
  return initial_env;
}

// ---- tracking usage: mark_*_used ----
void mark_module_used(const Uid& uid) {
  if (auto* f = module_declarations.find(uid)) (*f)();
}
void mark_modtype_used(const Uid&) {}
void mark_value_used(const Uid& uid) {
  if (auto* f = value_declarations.find(uid)) (*f)();
}
void mark_type_used(const Uid& uid) {
  if (auto* f = type_declarations.find(uid)) (*f)();
}
void mark_type_path_used(t env, Path::t path) {
  const TypeDeclaration* decl;
  try {
    decl = find_type(path, env);
  } catch (const NotFound&) {
    return;
  }
  mark_type_used(decl->type_uid);
}
void mark_constructor_used(ConstructorUsage usage, const Uid& uid) {
  if (auto* f = used_constructors.find(uid)) (*f)(usage);
}
void mark_extension_used(ConstructorUsage usage, const Uid& uid) {
  if (auto* f = used_constructors.find(uid)) (*f)(usage);
}
void mark_label_used(LabelUsage usage, const Uid& uid) {
  if (auto* f = used_labels.find(uid)) (*f)(usage);
}
static Path::t tconstr_path(TypeExpr* ty) {
  auto* c = as<Tconstr>(get_desc(ty));
  if (!c) throw std::logic_error("Env: not a Tconstr");
  return c->path;
}
void mark_constructor_description_used(ConstructorUsage usage, t env, const ConstructorDescription* cstr) {
  mark_type_path_used(env, tconstr_path(cstr->cstr_res));  // cstr_res_type_path
  if (auto* f = used_constructors.find(cstr->cstr_uid)) (*f)(usage);
}
void mark_label_description_used(LabelUsage usage, t env, const LabelDescription* lbl) {
  mark_type_path_used(env, tconstr_path(lbl->lbl_res));
  if (auto* f = used_labels.find(lbl->lbl_uid)) (*f)(usage);
}
void mark_class_used(const Uid& uid) {
  if (auto* f = type_declarations.find(uid)) (*f)();
}
void mark_cltype_used(const Uid& uid) {
  if (auto* f = type_declarations.find(uid)) (*f)();
}
void set_value_used_callback(const ValueDescription* vd, std::function<void()> callback) {
  value_declarations.add(vd->val_uid, std::move(callback));
}
void set_type_used_callback(const TypeDeclaration* td, std::function<void(std::function<void()>)> callback) {
  if (!uid::for_actual_declaration(td->type_uid)) return;
  std::function<void()> old = [] {};
  if (auto* f = type_declarations.find(td->type_uid)) old = *f;
  type_declarations.replace(td->type_uid, [callback, old] { callback(old); });
}

// ---- use_* (the marks and the alerts of a lookup) ----
static void use_module(bool use, const Location& loc, Path::t path, const ModuleData* mda) {
  if (!use) return;
  const ModuleComponents* comps = mda->mda_components;
  mark_module_used(comps->uid);
  comps->alerts.iter([&](std::string_view kind, std::string_view message) {
    std::string m = message.empty() ? std::string() : "\n" + std::string(message);
    location::alert(loc, std::string(kind), "module " + path::name(path) + m);
  });
}
static void use_value(bool use, const Location& loc, Path::t path, const ValueDescription* desc) {
  if (!use) return;
  mark_value_used(desc->val_uid);
  builtin_attributes::check_alerts(loc, desc->val_attributes, path::name(path));
}
static void use_type(bool use, const Location& loc, Path::t path, const TypeData* tda) {
  if (!use) return;
  const TypeDeclaration* decl = tda->tda_declaration;
  mark_type_used(decl->type_uid);
  builtin_attributes::check_alerts(loc, decl->type_attributes, path::name(path));
}
static void use_modtype(bool use, const Location& loc, Path::t path, const lz::ModtypeDecl* desc) {
  if (!use) return;
  mark_modtype_used(desc->mtdl_uid);
  builtin_attributes::check_alerts(loc, desc->mtdl_attributes, path::name(path));
}
static void use_class(bool use, const Location& loc, Path::t path, const ClassDeclaration* desc) {
  if (!use) return;
  mark_class_used(desc->cty_uid);
  builtin_attributes::check_alerts(loc, desc->cty_attributes, path::name(path));
}
static void use_cltype(bool use, const Location& loc, Path::t path, const ClassTypeDeclaration* desc) {
  if (!use) return;
  mark_cltype_used(desc->clty_uid);
  builtin_attributes::check_alerts(loc, desc->clty_attributes, path::name(path));
}
static void use_label(bool use, const Location& loc, LabelUsage usage, t env, const LabelDescription* lbl) {
  if (!use) return;
  mark_label_description_used(usage, env, lbl);
  builtin_attributes::check_alerts(loc, lbl->lbl_attributes, lbl->lbl_name);
  if (is_mutating_label_usage(usage))
    builtin_attributes::check_deprecated_mutable(loc, lbl->lbl_attributes, lbl->lbl_name);
}
static void use_constructor_desc(bool use, const Location& loc, ConstructorUsage usage, t env,
                                 const ConstructorDescription* cstr) {
  if (!use) return;
  mark_constructor_description_used(usage, env, cstr);
  builtin_attributes::check_alerts(loc, cstr->cstr_attributes, cstr->cstr_name);
}

// ============================================================================
// Lookup by name
// (use_* marking and alerts are not ported; `use` is carried for fidelity)

static void report_module_unbound(bool errors, const Location& loc, t env,
                                  const ModuleUnboundReason& reason) {
  // see #5965
  LookupError e = lerr(LookupError::Kind::Illegal_reference_to_recursive_module);
  e.container = reason.container;
  e.unbound = reason.unbound;
  may_lookup_error(errors, loc, env, e);
}

static void report_value_unbound(bool errors, const Location& loc, t env,
                                 const ValueUnboundReason& reason, Longident::t lid) {
  using RK = ValueUnboundReason::Kind;
  switch (reason.kind) {
    case RK::Val_unbound_instance_variable:
      may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Masked_instance_variable, lid));
    case RK::Val_unbound_self:
      may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Masked_self_variable, lid));
    case RK::Val_unbound_ancestor:
      may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Masked_ancestor_variable, lid));
    case RK::Val_unbound_ghost_recursive: {
      // Only display the "missing rec" hint for non-ghost code
      LookupError e = lerr(LookupError::Kind::Unbound_value, lid);
      e.missing_rec = !loc.loc_ghost && !reason.ghost_loc.loc_ghost;
      e.hint_loc = reason.ghost_loc;
      may_lookup_error(errors, loc, env, e);
    }
  }
}

// lookup_ident_module with `Load` (the module data) or `Don't_load` (nullptr)
static std::pair<Path::t, const ModuleData*> lookup_ident_module(bool load, bool errors, bool use,
                                                                 const Location& loc,
                                                                 std::string_view s, t env) {
  std::pair<Path::t, const ModuleEntry*> r;
  try {
    r = find_name_module(use, s, env->modules);
  } catch (const NotFound&) {
    may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Unbound_module, Longident::lident(s)));
  }
  auto [path, data] = r;
  switch (data->kind) {
    case ModuleEntry::Kind::Mod_local:
      use_module(use, loc, path, data->data);
      return {path, load ? data->data : nullptr};
    case ModuleEntry::Kind::Mod_unbound:
      report_module_unbound(errors, loc, env, data->reason);
      throw NotFound{};
    case ModuleEntry::Kind::Mod_persistent:
      if (!load) {
        check_pers_mod(false, loc, s);
        return {path, nullptr};
      }
      const ModuleData* mda;
      try {
        mda = find_pers_mod(false, s);
      } catch (const NotFound&) {
        may_lookup_error(errors, loc, env,
                         lerr(LookupError::Kind::Unbound_module, Longident::lident(s)));
      }
      use_module(use, loc, path, mda);
      return {path, mda};
  }
  throw NotFound{};
}

static std::pair<Path::t, const ValueDescription*> lookup_ident_value(bool errors, bool use,
                                                                      const Location& loc,
                                                                      std::string_view name,
                                                                      t env) {
  std::pair<Path::t, const ValueEntry*> r;
  if (auto f = idtbl_find_name_opt(wrap_value, use, name, env->values)) {
    r = *f;
  } else {
    LookupError e = lerr(LookupError::Kind::Unbound_value, Longident::lident(name));
    may_lookup_error(errors, loc, env, e);
  }
  if (r.second->bound) {
    use_value(use, loc, r.first, r.second->data->vda_description);
    return {r.first, r.second->data->vda_description};
  }
  report_value_unbound(errors, loc, env, r.second->reason, Longident::lident(name));
  throw NotFound{};
}

template <class A, class B>
static std::pair<Path::t, A> lookup_ident_generic(bool errors, bool use, const Location& loc,
                                                  std::string_view s, const IdTbl<A, B>& tbl,
                                                  t env, LookupError::Kind unbound) {
  if (auto r = idtbl_find_name_opt(wrap_identity<A>, use, s, tbl)) return *r;
  may_lookup_error(errors, loc, env, lerr(unbound, Longident::lident(s)));
}

static std::vector<std::pair<const LabelDescription*, std::function<void()>>>
lookup_all_ident_labels(bool errors, bool use, const Location& loc, LabelUsage usage, std::string_view s, t env) {
  auto lbls = tycomp_find_all(use, s, env->labels);
  if (lbls.empty())
    may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Unbound_label, Longident::lident(s)));
  std::vector<std::pair<const LabelDescription*, std::function<void()>>> out;
  for (auto& [l, fn] : lbls)
    out.emplace_back(l, [use, loc, usage, env, l = l, fn = fn] {
      use_label(use, loc, usage, env, l);
      fn();
    });
  return out;
}

static std::vector<std::pair<const ConstructorDescription*, std::function<void()>>>
lookup_all_ident_constructors(bool errors, bool use, const Location& loc, ConstructorUsage usage,
                              std::string_view s, t env) {
  auto cstrs = tycomp_find_all(use, s, env->constrs);
  if (cstrs.empty())
    may_lookup_error(errors, loc, env,
                     lerr(LookupError::Kind::Unbound_constructor, Longident::lident(s)));
  std::vector<std::pair<const ConstructorDescription*, std::function<void()>>> out;
  for (auto& [cda, fn] : cstrs)
    out.emplace_back(cda->cda_description, [use, loc, usage, env, c = cda->cda_description, fn = fn] {
      use_constructor_desc(use, loc, usage, env, c);
      fn();
    });
  return out;
}

struct ApplyResult {
  Path::t f_path;
  FunctorComponents* f_comp;
  Path::t arg;
};

static std::pair<Path::t, ModuleComponents*> lookup_module_components(bool errors, bool use,
                                                                      const Location& loc,
                                                                      Longident::t lid, t env);
static ApplyResult lookup_apply(bool errors, bool use, const Location& loc, Longident::t lid0,
                                t env);
static std::pair<Path::t, const ModuleData*> lookup_dot_module(bool errors, bool use,
                                                               const Location& loc,
                                                               Longident::t l, const Location& lloc,
                                                               std::string_view s, t env);
static std::pair<Path::t, const ModuleDeclaration*> lookup_module_(bool errors, bool use,
                                                                   const Location& loc,
                                                                   Longident::t lid, t env);

static std::pair<Path::t, StructureComponents*> lookup_structure_components(
    bool errors, bool use, Longident::t lid, const Location& loc, t env) {
  auto [path, comps] = lookup_module_components(errors, use, loc, lid, env);
  ComponentsResult r = get_components_res(comps);
  if (r.ok) {
    if (r.repr->is_structure) return {path, r.repr->structure};
    may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Functor_used_as_structure, lid));
  }
  if (!r.alias)
    may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Abstract_used_as_structure, lid));
  LookupError e = lerr(LookupError::Kind::Cannot_scrape_alias, lid);
  e.alias = r.alias;
  may_lookup_error(errors, loc, env, e);
}

static std::pair<FunctorComponents*, const ModuleType*> get_functor_components(
    bool errors, const Location& loc, Longident::t lid, t env, ModuleComponents* comps) {
  ComponentsResult r = get_components_res(comps);
  if (r.ok) {
    if (!r.repr->is_structure) {
      FunctorComponents* f = r.repr->functor;
      if (f->fcomp_arg.is_unit)  // PR#7611
        may_lookup_error(errors, loc, env,
                         lerr(LookupError::Kind::Generative_used_as_applicative, lid));
      return {f, f->fcomp_arg.mty};
    }
    may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Structure_used_as_functor, lid));
  }
  if (!r.alias)
    may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Abstract_used_as_functor, lid));
  LookupError e = lerr(LookupError::Kind::Cannot_scrape_alias, lid);
  e.alias = r.alias;
  may_lookup_error(errors, loc, env, e);
}

static std::pair<Path::t, ModuleComponents*> lookup_module_components(bool errors, bool use,
                                                                      const Location& loc,
                                                                      Longident::t lid, t env) {
  switch (lid->kind) {
    case Longident::Kind::Lident: {
      auto [path, data] = lookup_ident_module(true, errors, use, loc, lid->s, env);
      return {path, data->mda_components};
    }
    case Longident::Kind::Ldot: {
      auto [path, data] = lookup_dot_module(errors, use, loc, lid->l1, lid->l1_loc(), lid->s, env);
      return {path, data->mda_components};
    }
    case Longident::Kind::Lapply: {
      ApplyResult a = lookup_apply(errors, use, loc, lid, env);
      ModuleComponents* comps = components_of_functor_appl(loc, a.f_path, a.f_comp, a.arg, env);
      return {Path::papply(a.f_path, a.arg), comps};
    }
  }
  throw NotFound{};
}

struct ArgInfo {
  Longident::t f_lid;
  Location f_loc;
  Path::t arg_path;
  const ModuleType* arg_mty;
};

static ApplyResult lookup_apply(bool errors, bool use, const Location& loc, Longident::t lid0,
                                t env) {
  // lookup_all_args: walk down the applications collecting (f_lid, arg_path,
  // arg_mty), innermost application first
  std::vector<ArgInfo> args0;
  Longident::t f0_lid = lid0;
  while (f0_lid->kind == Longident::Kind::Lapply) {
    auto [arg_path, arg_md] = lookup_module_(errors, use, f0_lid->l2_loc(), f0_lid->l2, env);
    args0.insert(args0.begin(), ArgInfo{f0_lid->l1, f0_lid->l1_loc(), arg_path, arg_md->md_type});
    f0_lid = f0_lid->l1;
  }
  std::vector<std::pair<Path::t, const ModuleType*>> args_for_errors;
  for (auto& a : args0) args_for_errors.emplace_back(a.arg_path, a.arg_mty);
  auto [f0_path, f0_comp] = lookup_module_components(errors, use, loc, f0_lid, env);
  auto check_one_apply = [&](const ArgInfo& a, ModuleComponents* f_comp) {
    auto [fc, param_mty] = get_functor_components(errors, a.f_loc, a.f_lid, env, f_comp);
    check_functor_appl(errors, loc, lid0, f0_path, args_for_errors, fc, a.arg_path, a.arg_mty,
                       param_mty, env);
    return fc;
  };
  if (args0.empty()) throw std::invalid_argument("Env.lookup_apply: empty argument list");
  Path::t f_path = f0_path;
  ModuleComponents* f_comp = f0_comp;
  for (std::size_t k = 0;; ++k) {
    FunctorComponents* fc = check_one_apply(args0[k], f_comp);
    if (k + 1 == args0.size()) return {f_path, fc, args0[k].arg_path};
    f_comp = components_of_functor_appl(loc, f_path, fc, args0[k].arg_path, env);
    f_path = Path::papply(f_path, args0[k].arg_path);
  }
}

static std::pair<Path::t, const ModuleDeclaration*> lookup_module_(bool errors, bool use,
                                                                   const Location& loc,
                                                                   Longident::t lid, t env) {
  switch (lid->kind) {
    case Longident::Kind::Lident: {
      auto [path, data] = lookup_ident_module(true, errors, use, loc, lid->s, env);
      return {path, lz::force_module_decl(data->mda_declaration)};
    }
    case Longident::Kind::Ldot: {
      auto [path, data] = lookup_dot_module(errors, use, loc, lid->l1, lid->l1_loc(), lid->s, env);
      return {path, lz::force_module_decl(data->mda_declaration)};
    }
    case Longident::Kind::Lapply: {
      ApplyResult a = lookup_apply(errors, use, loc, lid, env);
      return {Path::papply(a.f_path, a.arg), md(modtype_of_functor_appl(a.f_comp, a.f_path, a.arg))};
    }
  }
  throw NotFound{};
}

static Longident::t dot_lid(Longident::t l, const Location& lloc, std::string_view s,
                            const Location& sloc) {
  return Longident::ldot(l, lloc, s, sloc);
}

static std::pair<Path::t, const ModuleData*> lookup_dot_module(bool errors, bool use,
                                                               const Location& loc,
                                                               Longident::t l, const Location& lloc,
                                                               std::string_view s, t env) {
  auto [p, comps] = lookup_structure_components(errors, use, l, lloc, env);
  if (auto* mda = comps->comp_modules.find_opt(s)) {
    Path::t path = Path::pdot(p, s);
    use_module(use, loc, path, *mda);
    return {path, *mda};
  }
  may_lookup_error(errors, loc, env,
                   lerr(LookupError::Kind::Unbound_module, dot_lid(l, lloc, s, loc)));
}

template <class X, class F>
static std::pair<Path::t, X> lookup_dot_generic(bool errors, bool use, const Location& loc,
                                                Longident::t lid, t env, F&& proj,
                                                LookupError::Kind unbound) {
  auto [p, comps] = lookup_structure_components(errors, use, lid->l1, lid->l1_loc(), env);
  if (auto* x = proj(comps).find_opt(lid->s)) return {Path::pdot(p, lid->s), *x};
  may_lookup_error(errors, loc, env, lerr(unbound, lid));
}

// ---- general forms -------------------------------------------------------------------
static Path::t lookup_module_path_(bool errors, bool use, const Location& loc, bool load,
                                   Longident::t lid, t env) {
  switch (lid->kind) {
    case Longident::Kind::Lident:
      return lookup_ident_module(!(no_alias_deps && !load), errors, use, loc, lid->s, env).first;
    case Longident::Kind::Ldot:
      return lookup_dot_module(errors, use, loc, lid->l1, lid->l1_loc(), lid->s, env).first;
    case Longident::Kind::Lapply: {
      ApplyResult a = lookup_apply(errors, use, loc, lid, env);
      return Path::papply(a.f_path, a.arg);
    }
  }
  throw NotFound{};
}

static std::pair<Path::t, const ValueDescription*> lookup_value_(bool errors, bool use,
                                                                 const Location& loc,
                                                                 Longident::t lid, t env) {
  if (lid->kind == Longident::Kind::Lident) return lookup_ident_value(errors, use, loc, lid->s, env);
  if (lid->kind == Longident::Kind::Ldot) {
    auto [p, comps] = lookup_structure_components(errors, use, lid->l1, lid->l1_loc(), env);
    if (auto* vda = comps->comp_values.find_opt(lid->s)) {
      Path::t path = Path::pdot(p, lid->s);
      use_value(use, loc, path, (*vda)->vda_description);
      return {path, (*vda)->vda_description};
    }
    may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Unbound_value, lid));
  }
  throw std::logic_error("Env.lookup_value: Lapply");
}

static std::pair<Path::t, const TypeData*> lookup_type_full(bool errors, bool use,
                                                            const Location& loc, Longident::t lid,
                                                            t env) {
  std::pair<Path::t, const TypeData*> r;
  if (lid->kind == Longident::Kind::Lident)
    r = lookup_ident_generic(errors, use, loc, lid->s, env->types, env, LookupError::Kind::Unbound_type);
  else if (lid->kind == Longident::Kind::Ldot)
    r = lookup_dot_generic<const TypeData*>(
        errors, use, loc, lid, env, [](StructureComponents* c) { return c->comp_types; },
        LookupError::Kind::Unbound_type);
  else
    throw std::logic_error("Env.lookup_type: Lapply");
  use_type(use, loc, r.first, r.second);
  return r;
}

static std::pair<Path::t, const lz::ModtypeDecl*> lookup_modtype_lazy(bool errors, bool use,
                                                                      const Location& loc,
                                                                      Longident::t lid, t env) {
  std::pair<Path::t, const ModtypeData*> r;
  if (lid->kind == Longident::Kind::Lident)
    r = lookup_ident_generic(errors, use, loc, lid->s, env->modtypes, env,
                             LookupError::Kind::Unbound_modtype);
  else if (lid->kind == Longident::Kind::Ldot)
    r = lookup_dot_generic<const ModtypeData*>(
        errors, use, loc, lid, env, [](StructureComponents* c) { return c->comp_modtypes; },
        LookupError::Kind::Unbound_modtype);
  else
    throw std::logic_error("Env.lookup_modtype: Lapply");
  use_modtype(use, loc, r.first, r.second->mtda_declaration);
  return {r.first, r.second->mtda_declaration};
}

static std::pair<Path::t, const ClassDeclaration*> lookup_class_(bool errors, bool use,
                                                                 const Location& loc,
                                                                 Longident::t lid, t env) {
  std::pair<Path::t, const ClassData*> r;
  if (lid->kind == Longident::Kind::Lident)
    r = lookup_ident_generic(errors, use, loc, lid->s, env->classes, env,
                             LookupError::Kind::Unbound_class);
  else if (lid->kind == Longident::Kind::Ldot)
    r = lookup_dot_generic<const ClassData*>(
        errors, use, loc, lid, env, [](StructureComponents* c) { return c->comp_classes; },
        LookupError::Kind::Unbound_class);
  else
    throw std::logic_error("Env.lookup_class: Lapply");
  use_class(use, loc, r.first, r.second->clda_declaration);
  return {r.first, r.second->clda_declaration};
}

static std::pair<Path::t, const ClassTypeDeclaration*> lookup_cltype_(bool errors, bool use,
                                                                     const Location& loc,
                                                                     Longident::t lid, t env) {
  std::pair<Path::t, const CltypeData*> r;
  if (lid->kind == Longident::Kind::Lident)
    r = lookup_ident_generic(errors, use, loc, lid->s, env->cltypes, env,
                             LookupError::Kind::Unbound_cltype);
  else if (lid->kind == Longident::Kind::Ldot)
    r = lookup_dot_generic<const CltypeData*>(
        errors, use, loc, lid, env, [](StructureComponents* c) { return c->comp_cltypes; },
        LookupError::Kind::Unbound_cltype);
  else
    throw std::logic_error("Env.lookup_cltype: Lapply");
  use_cltype(use, loc, r.first, r.second->cltda_declaration);
  return {r.first, r.second->cltda_declaration};
}

static std::vector<std::pair<const LabelDescription*, std::function<void()>>> lookup_all_labels_(
    bool errors, bool use, const Location& loc, LabelUsage usage, Longident::t lid, t env) {
  if (lid->kind == Longident::Kind::Lident)
    return lookup_all_ident_labels(errors, use, loc, usage, lid->s, env);
  if (lid->kind == Longident::Kind::Ldot) {
    auto [p, comps] = lookup_structure_components(errors, use, lid->l1, lid->l1_loc(), env);
    const Slice<LabelData>* lbls = comps->comp_labels.find_opt(lid->s);
    if (!lbls || lbls->empty())
      may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Unbound_label, lid));
    std::vector<std::pair<const LabelDescription*, std::function<void()>>> out;
    for (auto* l : *lbls) out.emplace_back(l, [use, loc, usage, env, l] { use_label(use, loc, usage, env, l); });
    return out;
  }
  throw std::logic_error("Env.lookup_all_labels: Lapply");
}

static std::vector<std::pair<const ConstructorDescription*, std::function<void()>>>
lookup_all_constructors_(bool errors, bool use, const Location& loc, ConstructorUsage usage, Longident::t lid,
                         t env) {
  if (lid->kind == Longident::Kind::Lident)
    return lookup_all_ident_constructors(errors, use, loc, usage, lid->s, env);
  if (lid->kind == Longident::Kind::Ldot) {
    if (lid->l1->kind == Longident::Kind::Lident && lid->l1->s == "*predef*")
      // Hack to support compilation of default arguments
      return lookup_all_ident_constructors(errors, use, lid->s_loc(), usage, lid->s, initial());
    auto [p, comps] = lookup_structure_components(errors, use, lid->l1, lid->l1_loc(), env);
    const Slice<const ConstructorData*>* cstrs = comps->comp_constrs.find_opt(lid->s);
    if (!cstrs || cstrs->empty())
      may_lookup_error(errors, loc, env, lerr(LookupError::Kind::Unbound_constructor, lid));
    std::vector<std::pair<const ConstructorDescription*, std::function<void()>>> out;
    for (auto* cda : *cstrs)
      out.emplace_back(cda->cda_description,
                       [use, loc, usage, env, c = cda->cda_description] {
                         use_constructor_desc(use, loc, usage, env, c);
                       });
    return out;
  }
  throw std::logic_error("Env.lookup_all_constructors: Lapply");
}

// ---- ordinary (error-reporting) lookups -------------------------------------------------
Path::t lookup_module_path(bool use, const Location& loc, bool load, Longident::t lid, t env) {
  return lookup_module_path_(true, use, loc, load, lid, env);
}
std::pair<Path::t, const ModuleDeclaration*> lookup_module(bool use, const Location& loc,
                                                           Longident::t lid, t env) {
  return lookup_module_(true, use, loc, lid, env);
}
std::pair<Path::t, const ValueDescription*> lookup_value(bool use, const Location& loc,
                                                         Longident::t lid, t env) {
  check_value_name(longident::last(lid), loc);
  return lookup_value_(true, use, loc, lid, env);
}
std::pair<Path::t, const TypeDeclaration*> lookup_type(bool use, const Location& loc,
                                                       Longident::t lid, t env) {
  auto [p, tda] = lookup_type_full(true, use, loc, lid, env);
  return {p, tda->tda_declaration};
}
std::pair<Path::t, const ModtypeDeclaration*> lookup_modtype(bool use, const Location& loc,
                                                             Longident::t lid, t env) {
  auto [p, mt] = lookup_modtype_lazy(true, use, loc, lid, env);
  return {p, lz::force_modtype_decl(mt)};
}
Path::t lookup_modtype_path(bool use, const Location& loc, Longident::t lid, t env) {
  return lookup_modtype_lazy(true, use, loc, lid, env).first;
}
std::pair<Path::t, const ClassDeclaration*> lookup_class(bool use, const Location& loc,
                                                         Longident::t lid, t env) {
  return lookup_class_(true, use, loc, lid, env);
}
std::pair<Path::t, const ClassTypeDeclaration*> lookup_cltype(bool use, const Location& loc,
                                                              Longident::t lid, t env) {
  return lookup_cltype_(true, use, loc, lid, env);
}

LookupAllCstrs lookup_all_constructors(bool use, const Location& loc, ConstructorUsage usage,
                                       Longident::t lid, t env) {
  // Typing_recovery.uncatch_errors: the caller processes the errors
  try {
    return LookupAllCstrs{true, lookup_all_constructors_(true, use, loc, usage, lid, env)};
  } catch (const Error& e) {
    if (e.kind != Error::Kind::Lookup_error) throw;
    LookupAllCstrs r{false};
    r.err_loc = e.loc;
    r.err_env = e.env;
    r.err = e.err;
    return r;
  }
}

const ConstructorDescription* lookup_constructor(bool use, const Location& loc, ConstructorUsage usage,
                                                 Longident::t lid, t env) {
  auto l = lookup_all_constructors_(true, use, loc, usage, lid, env);
  if (l.empty()) throw std::logic_error("Env.lookup_constructor");
  l.front().second();
  return l.front().first;
}

std::vector<std::pair<const ConstructorDescription*, std::function<void()>>>
lookup_all_constructors_from_type(bool use, const Location& loc, ConstructorUsage usage, Path::t ty_path, t env) {
  std::vector<std::pair<const ConstructorDescription*, std::function<void()>>> out;
  const TypeDescriptions* d;
  try {
    d = find_type_descrs(ty_path, env);
  } catch (const NotFound&) {
    return out;
  }
  if (d->kind != TypeKind::Kind::Type_variant) return out;
  for (auto* c : d->constructors)
    out.emplace_back(c, [use, loc, usage, env, c] { use_constructor_desc(use, loc, usage, env, c); });
  return out;
}

LookupAllLabels lookup_all_labels(bool use, const Location& loc, LabelUsage usage, Longident::t lid,
                                  t env) {
  try {
    return LookupAllLabels{true, lookup_all_labels_(true, use, loc, usage, lid, env)};
  } catch (const Error& e) {
    if (e.kind != Error::Kind::Lookup_error) throw;
    LookupAllLabels r{false};
    r.err_loc = e.loc;
    r.err_env = e.env;
    r.err = e.err;
    return r;
  }
}

const LabelDescription* lookup_label(bool use, const Location& loc, LabelUsage usage, Longident::t lid,
                                     t env) {
  auto l = lookup_all_labels_(true, use, loc, usage, lid, env);
  if (l.empty()) throw std::logic_error("Env.lookup_label");
  l.front().second();
  return l.front().first;
}

std::vector<std::pair<const LabelDescription*, std::function<void()>>> lookup_all_labels_from_type(
    bool use, const Location& loc, LabelUsage usage, Path::t ty_path, t env) {
  std::vector<std::pair<const LabelDescription*, std::function<void()>>> out;
  const TypeDescriptions* d;
  try {
    d = find_type_descrs(ty_path, env);
  } catch (const NotFound&) {
    return out;
  }
  if (d->kind != TypeKind::Kind::Type_record) return out;
  for (auto* l : d->labels) out.emplace_back(l, [use, loc, usage, env, l] { use_label(use, loc, usage, env, l); });
  return out;
}

InstanceVariable lookup_instance_variable(bool use, const Location& loc, std::string_view name,
                                          t env) {
  std::pair<Path::t, const ValueEntry*> r;
  if (auto f = idtbl_find_name_opt(wrap_value, use, name, env->values)) {
    r = *f;
  } else {
    LookupError e = lerr(LookupError::Kind::Unbound_instance_variable);
    e.name = zborrow(name);
    lookup_error(loc, env, e);
  }
  auto [path, entry] = r;
  if (entry->bound) {
    const ValueDescription* desc = entry->data->vda_description;
    if (desc->val_kind.kind == ValueKind::Kind::Val_ivar) {
      use_value(use, loc, path, desc);
      return {path, desc->val_kind.ivar_mut, desc->val_kind.ivar_name, desc->val_type};
    }
    LookupError e = lerr(LookupError::Kind::Not_an_instance_variable);
    e.name = zborrow(name);
    lookup_error(loc, env, e);
  }
  using RK = ValueUnboundReason::Kind;
  switch (entry->reason.kind) {
    case RK::Val_unbound_instance_variable:
      lookup_error(loc, env, lerr(LookupError::Kind::Masked_instance_variable, Longident::lident(name)));
    case RK::Val_unbound_self:
    case RK::Val_unbound_ancestor: {
      LookupError e = lerr(LookupError::Kind::Not_an_instance_variable);
      e.name = zborrow(name);
      lookup_error(loc, env, e);
    }
    case RK::Val_unbound_ghost_recursive: {
      LookupError e = lerr(LookupError::Kind::Unbound_instance_variable);
      e.name = zborrow(name);
      lookup_error(loc, env, e);
    }
  }
  throw NotFound{};
}

// ---- lookups that neither mark nor report errors (raise NotFound) -----------------------
std::pair<Path::t, const ModuleDeclaration*> find_module_by_name(Longident::t lid, t env) {
  return lookup_module_(false, false, location::none(), lid, env);
}
std::pair<Path::t, const ValueDescription*> find_value_by_name(Longident::t lid, t env) {
  return lookup_value_(false, false, location::none(), lid, env);
}
std::pair<Path::t, const TypeDeclaration*> find_type_by_name(Longident::t lid, t env) {
  auto [p, tda] = lookup_type_full(false, false, location::none(), lid, env);
  return {p, tda->tda_declaration};
}
std::pair<Path::t, const ModtypeDeclaration*> find_modtype_by_name(Longident::t lid, t env) {
  auto [p, mt] = lookup_modtype_lazy(false, false, location::none(), lid, env);
  return {p, lz::force_modtype_decl(mt)};
}
std::pair<Path::t, const ClassDeclaration*> find_class_by_name(Longident::t lid, t env) {
  return lookup_class_(false, false, location::none(), lid, env);
}
std::pair<Path::t, const ClassTypeDeclaration*> find_cltype_by_name(Longident::t lid, t env) {
  return lookup_cltype_(false, false, location::none(), lid, env);
}
const ConstructorDescription* find_constructor_by_name(Longident::t lid, t env) {
  auto l = lookup_all_constructors_(false, false, location::none(), ConstructorUsage::Positive, lid, env);
  return l.front().first;
}
const LabelDescription* find_label_by_name(Longident::t lid, t env) {
  auto l = lookup_all_labels_(false, false, location::none(), LabelUsage::Projection, lid, env);
  return l.front().first;
}

// ---- checking if a name is bound ------------------------------------------------------
bool bound_module(std::string_view name, t env) {
  if (idtbl_find_name_opt(wrap_module, false, name, env->modules)) return true;
  if (current_unit_is(name)) return false;
  try {
    find_pers_mod(false, name);
    return true;
  } catch (const NotFound&) {
    return false;
  }
}

template <class A, class B, class W>
static bool bound(W&& wrap, const IdTbl<A, B>& tbl, std::string_view name) {
  return idtbl_find_name_opt(wrap, false, name, tbl).has_value();
}
bool bound_value(std::string_view n, t env) { return bound(wrap_value, env->values, n); }
bool bound_type(std::string_view n, t env) {
  return bound(wrap_identity<const TypeData*>, env->types, n);
}
bool bound_modtype(std::string_view n, t env) {
  return bound(wrap_identity<const ModtypeData*>, env->modtypes, n);
}
bool bound_class(std::string_view n, t env) {
  return bound(wrap_identity<const ClassData*>, env->classes, n);
}
bool bound_cltype(std::string_view n, t env) {
  return bound(wrap_identity<const CltypeData*>, env->cltypes, n);
}

// ---- folding on environments ---------------------------------------------------------
void fold_values(const std::function<void(std::string_view, Path::t, const ValueDescription*)>& f,
                 Longident::t lid, t env) {
  if (!lid) {
    idtbl_fold_name<const ValueEntry*, const ValueData*>(
        wrap_value,
        [&](std::string_view name, Path::t p, const ValueEntry* const& ve) {
          if (ve->bound) f(name, p, ve->data->vda_description);
        },
        env->values);
    return;
  }
  auto [p, desc] = lookup_module_components(false, false, location::none(), lid, env);
  const ModuleComponentsRepr* r = get_components(desc);
  if (!r->is_structure) return;
  r->structure->comp_values.iter([&](std::string_view s, const ValueData* vda) {
    f(s, Path::pdot(p, s), vda->vda_description);
  });
}

void fold_types(const std::function<void(std::string_view, Path::t, const TypeDeclaration*)>& f,
                Longident::t lid, t env) {
  if (!lid) {
    idtbl_fold_name<const TypeData*, const TypeData*>(
        wrap_identity<const TypeData*>,
        [&](std::string_view name, Path::t p, const TypeData* const& tda) {
          f(name, p, tda->tda_declaration);
        },
        env->types);
    return;
  }
  auto [p, desc] = lookup_module_components(false, false, location::none(), lid, env);
  const ModuleComponentsRepr* r = get_components(desc);
  if (!r->is_structure) return;
  r->structure->comp_types.iter([&](std::string_view s, const TypeData* tda) {
    f(s, Path::pdot(p, s), tda->tda_declaration);
  });
}

void fold_modules(const std::function<void(std::string_view, Path::t, const ModuleDeclaration*)>& f,
                  Longident::t lid, t env) {
  if (!lid) {
    idtbl_fold_name<const ModuleEntry*, const ModuleData*>(
        wrap_module, [&](std::string_view name, Path::t p, const ModuleEntry* const& e) {
      switch (e->kind) {
        case ModuleEntry::Kind::Mod_unbound: return;
        case ModuleEntry::Kind::Mod_local:
          f(name, p, lz::force_module_decl(e->data->mda_declaration));
          return;
        case ModuleEntry::Kind::Mod_persistent:
          if (const ModuleData* const* mda = g_persistent_env.find_in_cache(std::string(name)))
            f(name, p, lz::force_module_decl((*mda)->mda_declaration));
          return;
      }
    }, env->modules);
    return;
  }
  auto [p, desc] = lookup_module_components(false, false, location::none(), lid, env);
  const ModuleComponentsRepr* r = get_components(desc);
  if (!r->is_structure) return;
  r->structure->comp_modules.iter([&](std::string_view s, const ModuleData* mda) {
    f(s, Path::pdot(p, s), lz::force_module_decl(mda->mda_declaration));
  });
}

void fold_constructors(const std::function<void(const ConstructorDescription*)>& f,
                       Longident::t lid, t env) {
  if (!lid) {
    tycomp_fold_name<const ConstructorData*>(
        [&](const ConstructorData* cda) { f(cda->cda_description); }, env->constrs);
    return;
  }
  auto [p, desc] = lookup_module_components(false, false, location::none(), lid, env);
  const ModuleComponentsRepr* r = get_components(desc);
  if (!r->is_structure) return;
  r->structure->comp_constrs.iter([&](std::string_view, const Slice<const ConstructorData*>& l) {
    if (!l.empty()) f(l.front()->cda_description);
  });
}

void fold_labels(const std::function<void(const LabelDescription*)>& f, Longident::t lid, t env) {
  if (!lid) {
    tycomp_fold_name<LabelData>([&](LabelData l) { f(l); }, env->labels);
    return;
  }
  auto [p, desc] = lookup_module_components(false, false, location::none(), lid, env);
  const ModuleComponentsRepr* r = get_components(desc);
  if (!r->is_structure) return;
  r->structure->comp_labels.iter([&](std::string_view, const Slice<LabelData>& l) {
    if (!l.empty()) f(l.front());
  });
}

template <class D, class R, class Proj1, class Proj2, class Get>
static void find_all_names(const std::function<void(std::string_view, Path::t, R)>& f, Proj1 proj1, Proj2 proj2,
                           Get get, Longident::t lid, t env) {
  if (!lid) {
    idtbl_fold_name<D, D>(
        wrap_identity<D>, [&](std::string_view name, Path::t p, const D& d) { f(name, p, get(d)); }, proj1(env));
    return;
  }
  auto [p, desc] = lookup_module_components(false, false, location::none(), lid, env);
  const ModuleComponentsRepr* r = get_components(desc);
  if (!r->is_structure) return;
  proj2(r->structure).iter([&](std::string_view s, const D& d) { f(s, Path::pdot(p, s), get(d)); });
}

void fold_modtypes(const std::function<void(std::string_view, Path::t, const ModtypeDeclaration*)>& f,
                   Longident::t lid, t env) {
  find_all_names<const ModtypeData*, const ModtypeDeclaration*>(
      f, [](t e) -> const auto& { return e->modtypes; }, [](auto* sc) -> const auto& { return sc->comp_modtypes; },
      [](const ModtypeData* d) { return lz::force_modtype_decl(d->mtda_declaration); }, lid, env);
}
void fold_classes(const std::function<void(std::string_view, Path::t, const ClassDeclaration*)>& f,
                  Longident::t lid, t env) {
  find_all_names<const ClassData*, const ClassDeclaration*>(
      f, [](t e) -> const auto& { return e->classes; }, [](auto* sc) -> const auto& { return sc->comp_classes; },
      [](const ClassData* d) { return d->clda_declaration; }, lid, env);
}
void fold_cltypes(const std::function<void(std::string_view, Path::t, const ClassTypeDeclaration*)>& f,
                  Longident::t lid, t env) {
  find_all_names<const CltypeData*, const ClassTypeDeclaration*>(
      f, [](t e) -> const auto& { return e->cltypes; }, [](auto* sc) -> const auto& { return sc->comp_cltypes; },
      [](const CltypeData* d) { return d->cltda_declaration; }, lid, env);
}

// ---- summaries and unscoped pairs -----------------------------------------------------
const Summary* summary(t env) {
  if (env->local_constraints.is_empty()) return env->summary;
  Summary s{Summary::Kind::Env_constraints, env->summary};
  s.constraints = env->local_constraints;
  return summ(s);
}

t with_pairs(Slice<std::pair<ident::Unscoped*, ident::Unscoped*>> id_pairs, t env) {
  EnvT* e = copy_env(env);
  e->id_pairs = id_pairs;
  return e;
}
Slice<std::pair<ident::Unscoped*, ident::Unscoped*>> get_pairs(t env) { return env->id_pairs; }
bool path_equiv(t env, Path::t p1, Path::t p2) {
  std::vector<std::pair<Ident::t, Ident::t>> pairs;
  for (auto& [a, b] : env->id_pairs) pairs.emplace_back(Ident::of_unscoped(a), Ident::of_unscoped(b));
  return path::equiv(pairs, p1, p2);
}

}  // namespace cppcaml::typing::env
