// Port of typing/includemod.ml.  See includemod.hpp.  Usage marking
// (Env.mark_*_used), alerts and Uid.Deps only feed warnings / the cmt file
// and are left out.
#include "cppcaml/typing/includemod.hpp"

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/includeclass.hpp"
#include "cppcaml/typing/mtype.hpp"
#include "cppcaml/typing/predef.hpp"

namespace cppcaml::typing::includemod {

using namespace types;
using MK = ModuleType::Kind;
using SK = SignatureItem::Kind;
using CK = tt::ModuleCoercion::Kind;
namespace E = error;

namespace {

template <class T, class Er>
struct Result {  // ('a, 'e) result
  bool ok;
  T value{};
  Er error{};
  static Result Ok(T v) { return Result{true, std::move(v), Er{}}; }
  static Result Err(Er e) { return Result{false, T{}, std::move(e)}; }
};

using CoercionShape = std::pair<const tt::ModuleCoercion*, shape::t>;
using MtyResult = Result<CoercionShape, E::ModuleTypeDiff>;
using MtySymResult = Result<CoercionShape, E::ModuleTypeSymptom>;

std::shared_ptr<const E::ModuleTypeDiff> share(const E::ModuleTypeDiff& d) {
  return std::make_shared<const E::ModuleTypeDiff>(d);
}
E::ModuleTypeSymptom mt_core(E::CoreModuleTypeSymptom::Kind k, Path::t p = nullptr) {
  E::ModuleTypeSymptom s{E::ModuleTypeSymptom::Kind::Mt_core};
  s.core = E::CoreModuleTypeSymptom{k, p};
  return s;
}
E::ModuleTypeSymptom functor_params(E::FunctorParamsInfo info1, E::FunctorParamsInfo info2) {
  E::ModuleTypeSymptom s{E::ModuleTypeSymptom::Kind::Functor_params};
  s.got = std::move(info1);
  s.expected = std::move(info2);
  return s;
}
E::FunctorParamsInfo cons_arg(const FunctorParameter& arg, E::FunctorParamsInfo info) {
  info.params.insert(info.params.begin(), arg);
  return info;
}

// ---- Directionality ----
enum class Mark { Mark_both, Mark_positive, Mark_neither };
enum class Pos { Strictly_positive, Positive, Negative };
struct Direction {
  bool in_eq;
  Mark mark_as_used;
  Pos pos;
};
Direction strictly_positive(bool mark, bool both) {
  Mark m = mark ? (both ? Mark::Mark_both : Mark::Mark_positive) : Mark::Mark_neither;
  return {false, m, Pos::Strictly_positive};
}
Direction unknown(bool mark) { return {false, mark ? Mark::Mark_both : Mark::Mark_neither, Pos::Positive}; }
Direction negate(Direction d) {
  d.pos = d.pos == Pos::Negative ? Pos::Positive : Pos::Negative;
  return d;
}
Direction enter_eq(Direction d) {
  return {true, d.mark_as_used, d.pos == Pos::Strictly_positive ? Pos::Positive : d.pos};
}

// ---- core_relation ----
using CoreResult = Result<const tt::ModuleCoercion*, E::SigitemSymptom>;
template <class A>
using CoreIncl = std::function<CoreResult(const Location&, env::t, Direction, subst::t, Ident::t, const A*, const A*)>;
struct CoreRelation {
  CoreIncl<ValueDescription> value_descriptions;
  CoreIncl<TypeDeclaration> type_declarations;
  CoreIncl<ExtensionConstructor> extension_constructors;
  CoreIncl<ClassDeclaration> class_declarations;
  CoreIncl<ClassTypeDeclaration> class_type_declarations;
};

E::SigitemSymptom core_symptom(E::CoreSigitemSymptom c) {
  E::SigitemSymptom s{E::SigitemSymptom::Kind::Core};
  s.core = std::move(c);
  return s;
}

// Core_inclusion: all functions "blah env x1 x2" check that x1 is included
// in x2.
CoreResult ci_value_descriptions(const Location& loc, env::t env, Direction, subst::t s, Ident::t id,
                                 const ValueDescription* vd1_, const ValueDescription* vd2_) {
  // Using [Subst] reverts expansions
  const ValueDescription* vd1 = subst::value_description(subst::identity(), vd1_);
  const ValueDescription* vd2 = subst::value_description(s, vd2_);
  try {
    return CoreResult::Ok(includecore::value_descriptions(loc, env, ident::name(id), vd1, vd2));
  } catch (const includecore::DontMatch& e) {
    E::CoreSigitemSymptom c{E::CoreSigitemSymptom::Kind::Value_descriptions};
    c.vd1 = vd1;
    c.vd2 = vd2;
    c.value = e.mismatch;
    return CoreResult::Err(core_symptom(c));
  }
}
CoreResult ci_type_declarations(const Location& loc, env::t env, Direction d, subst::t s, Ident::t id,
                                const TypeDeclaration* decl1_, const TypeDeclaration* decl2_) {
  bool mark = d.mark_as_used == Mark::Mark_both ||
              (d.mark_as_used == Mark::Mark_positive && d.pos != Pos::Negative);
  const TypeDeclaration* decl1 = subst::type_declaration(subst::identity(), decl1_);
  const TypeDeclaration* decl2 = subst::type_declaration(s, decl2_);
  if (auto err = includecore::type_declarations(false, loc, env, mark, ident::name(id), decl1, Path::pident(id), decl2)) {
    E::CoreSigitemSymptom c{E::CoreSigitemSymptom::Kind::Type_declarations};
    c.td1 = decl1;
    c.td2 = decl2;
    c.type = err;
    return CoreResult::Err(core_symptom(c));
  }
  return CoreResult::Ok(tt::tcoerce_none());
}
CoreResult ci_extension_constructors(const Location& loc, env::t env, Direction d, subst::t s, Ident::t id,
                                     const ExtensionConstructor* ext1_, const ExtensionConstructor* ext2_) {
  bool mark = d.mark_as_used == Mark::Mark_both ||
              (d.mark_as_used == Mark::Mark_positive && d.pos != Pos::Negative);
  const ExtensionConstructor* ext1 = subst::extension_constructor(subst::identity(), ext1_);
  const ExtensionConstructor* ext2 = subst::extension_constructor(s, ext2_);
  if (auto err = includecore::extension_constructors(loc, env, mark, id, ext1, ext2)) {
    E::CoreSigitemSymptom c{E::CoreSigitemSymptom::Kind::Extension_constructors};
    c.ext1 = ext1;
    c.ext2 = ext2;
    c.ext = err;
    return CoreResult::Err(core_symptom(c));
  }
  return CoreResult::Ok(tt::tcoerce_none());
}
CoreResult ci_class_type_declarations(const Location& loc, env::t env, Direction, subst::t s, Ident::t,
                                      const ClassTypeDeclaration* d1_, const ClassTypeDeclaration* d2_) {
  const ClassTypeDeclaration* d1 = subst::cltype_declaration(subst::identity(), d1_);
  const ClassTypeDeclaration* d2 = subst::cltype_declaration(s, d2_);
  auto reason = includeclass::class_type_declarations(loc, env, d1, d2);
  if (reason.empty()) return CoreResult::Ok(tt::tcoerce_none());
  E::CoreSigitemSymptom c{E::CoreSigitemSymptom::Kind::Class_type_declarations};
  c.clty1 = d1;
  c.clty2 = d2;
  c.classes = reason;
  return CoreResult::Err(core_symptom(c));
}
CoreResult ci_class_declarations(const Location&, env::t env, Direction, subst::t s, Ident::t,
                                 const ClassDeclaration* d1_, const ClassDeclaration* d2_) {
  const ClassDeclaration* d1 = subst::class_declaration(subst::identity(), d1_);
  const ClassDeclaration* d2 = subst::class_declaration(s, d2_);
  auto reason = includeclass::class_declarations(env, d1, d2);
  if (reason.empty()) return CoreResult::Ok(tt::tcoerce_none());
  E::CoreSigitemSymptom c{E::CoreSigitemSymptom::Kind::Class_declarations};
  c.cl1 = d1;
  c.cl2 = d2;
  c.classes = reason;
  return CoreResult::Err(core_symptom(c));
}

const CoreRelation& core_inclusion() {
  static const CoreRelation r{ci_value_descriptions, ci_type_declarations, ci_extension_constructors,
                              ci_class_declarations, ci_class_type_declarations};
  return r;
}
const CoreRelation& core_consistency() {
  static const CoreRelation r{
      [](const Location&, env::t env, Direction, subst::t, Ident::t, const ValueDescription* vd1,
         const ValueDescription* vd2) {
        try {
          return CoreResult::Ok(includecore::value_descriptions_consistency(env, vd1, vd2));
        } catch (const includecore::DontMatch& e) {
          E::CoreSigitemSymptom c{E::CoreSigitemSymptom::Kind::Value_descriptions};
          c.vd1 = vd1;
          c.vd2 = vd2;
          c.value = e.mismatch;
          return CoreResult::Err(core_symptom(c));
        }
      },
      [](const Location&, env::t env, Direction, subst::t, Ident::t, const TypeDeclaration* d1,
         const TypeDeclaration* d2) {
        if (auto err = includecore::type_declarations_consistency(env, d1, d2)) {
          E::CoreSigitemSymptom c{E::CoreSigitemSymptom::Kind::Type_declarations};
          c.td1 = d1;
          c.td2 = d2;
          c.type = err;
          return CoreResult::Err(core_symptom(c));
        }
        return CoreResult::Ok(tt::tcoerce_none());
      },
      [](const Location&, env::t, Direction, subst::t, Ident::t, const ExtensionConstructor*,
         const ExtensionConstructor*) { return CoreResult::Ok(tt::tcoerce_none()); },
      [](const Location&, env::t, Direction, subst::t, Ident::t, const ClassDeclaration*, const ClassDeclaration*) {
        return CoreResult::Ok(tt::tcoerce_none());
      },
      [](const Location&, env::t, Direction, subst::t, Ident::t, const ClassTypeDeclaration*,
         const ClassTypeDeclaration*) { return CoreResult::Ok(tt::tcoerce_none()); }};
  return r;
}

// Expand a module type identifier when possible
const ModuleType* expand_modtype_path(env::t env, Path::t path) {
  try {
    return env::find_modtype_expansion(path, env);
  } catch (const env::NotFound&) {
    return nullptr;
  }
}

Result<const ModuleType*, E::CoreModuleTypeSymptom> expand_module_alias_(bool strengthen, env::t env, Path::t path) {
  using R = Result<const ModuleType*, E::CoreModuleTypeSymptom>;
  try {
    const ModuleType* x = strengthen ? env::find_strengthened_module(true, path, env)
                                     : env::find_module(path, env)->md_type;
    return R::Ok(x);
  } catch (const env::NotFound&) {
    return R::Err(E::CoreModuleTypeSymptom{E::CoreModuleTypeSymptom::Kind::Unbound_module_path, path});
  }
}

}  // namespace

std::string_view kind_of_field_desc(const FieldDesc& fd) {
  switch (fd.kind) {
    case FieldKind::Field_value: return "value";
    case FieldKind::Field_type: return "type";
    case FieldKind::Field_exception: return "exception";
    case FieldKind::Field_typext: return "extension constructor";
    case FieldKind::Field_module: return "module";
    case FieldKind::Field_modtype: return "module type";
    case FieldKind::Field_class: return "class";
    case FieldKind::Field_classtype: return "class type";
  }
  return "";
}
FieldDesc field_desc(FieldKind kind, Ident::t id) { return {ident::name(id), kind}; }

ItemIdentName item_ident_name(const SignatureItem* it) {
  Ident::t id = it->id;
  switch (it->kind) {
    case SK::Sig_value: return {id, it->value->val_loc, field_desc(FieldKind::Field_value, id)};
    case SK::Sig_type: return {id, it->type->type_loc, field_desc(FieldKind::Field_type, id)};
    case SK::Sig_typext: {
      FieldKind k = path::same(it->ext->ext_type_path, predef::paths().exn) ? FieldKind::Field_exception
                                                                              : FieldKind::Field_typext;
      return {id, it->ext->ext_loc, field_desc(k, id)};
    }
    case SK::Sig_module: return {id, it->md->md_loc, field_desc(FieldKind::Field_module, id)};
    case SK::Sig_modtype: return {id, it->mtd->mtd_loc, field_desc(FieldKind::Field_modtype, id)};
    case SK::Sig_class: return {id, it->cls->cty_loc, field_desc(FieldKind::Field_class, id)};
    case SK::Sig_class_type: return {id, it->clty->clty_loc, field_desc(FieldKind::Field_classtype, id)};
  }
  throw std::logic_error("item_ident_name");
}

bool is_runtime_component(const SignatureItem* it) {
  switch (it->kind) {
    case SK::Sig_value: return it->value->val_kind.kind != ValueKind::Kind::Val_prim;
    case SK::Sig_type:
    case SK::Sig_modtype:
    case SK::Sig_class_type: return false;
    case SK::Sig_module: return it->presence == ModulePresence::Mp_present;
    case SK::Sig_typext:
    case SK::Sig_class: return true;
  }
  return false;
}

subst::t item_subst(Ident::t id1, const SignatureItem* item2, subst::t s) {
  switch (item2->kind) {
    case SK::Sig_type: return subst::add_type(item2->id, Path::pident(id1), s);
    case SK::Sig_module: return subst::add_module(item2->id, Path::pident(id1), s);
    case SK::Sig_modtype: return subst::add_modtype(item2->id, Path::pident(id1), s);
    default: return s;
  }
}

namespace {

// Simplify a structure coercion
bool equal_module_paths(env::t env, Path::t p1, subst::t s, Path::t p2) {
  if (path::same(p1, p2)) return true;
  // Path.same (normalize p1) (normalize (Subst.module_path subst p2)): right to left
  Path::t n2 = env::normalize_module_path(nullptr, env, subst::module_path(s, p2));
  Path::t n1 = env::normalize_module_path(nullptr, env, p1);
  return path::same(n1, n2);
}
bool equal_modtype_paths(env::t env, Path::t p1, subst::t s, Path::t p2) {
  if (path::same(p1, p2)) return true;
  Path::t n2 = env::normalize_modtype_path(env, subst::modtype_path(s, p2));
  Path::t n1 = env::normalize_modtype_path(env, p1);
  return path::same(n1, n2);
}

const tt::ModuleCoercion* simplify_structure_coercion(const std::vector<tt::PosCoercion>& cc,
                                                      const std::vector<tt::IdPosCoercion>& id_pos_list) {
  bool identity = true;
  for (std::size_t k = 0; k < cc.size(); ++k)
    if (!(cc[k].pos == static_cast<long>(k) && cc[k].cc->kind == CK::Tcoerce_none)) {
      identity = false;
      break;
    }
  if (identity) return tt::tcoerce_none();
  auto* c = make<tt::ModuleCoercion>(tt::ModuleCoercion{CK::Tcoerce_structure});
  c->pos_cc = slice(cc);
  c->id_pos_cc = slice(id_pos_list);
  return c;
}

E::FunctorParamsInfo retrieve_functor_params(env::t env, const ModuleType* mty) {
  std::vector<FunctorParameter> before;
  for (;;) {
    switch (mty->kind) {
      case MK::Mty_ident:
        if (const ModuleType* m = expand_modtype_path(env, mty->path)) {
          mty = m;
          continue;
        }
        return {before, mty};
      case MK::Mty_alias: {
        auto r = expand_module_alias_(false, env, mty->path);
        if (r.ok) {
          mty = r.value;
          continue;
        }
        return {before, mty};
      }
      case MK::Mty_functor:
        before.push_back(mty->param);
        mty = mty->res;
        continue;
      case MK::Mty_signature: return {before, mty};
    }
  }
}

// When computing a signature difference, we need to distinguish between
// recoverable errors at the value level and unrecoverable errors at the type
// level that require us to stop the computation of the difference.
struct SignDiff {
  std::vector<tt::PosCoercion> runtime_coercions;
  shape::ItemMap shape_map;
  bool deep_modifications = false;
  std::vector<std::pair<const SignatureItem*, E::SigitemSymptom>> errors;
  std::vector<E::Untypable> untypables;
};
SignDiff merge(SignDiff x, const SignDiff& y) {
  x.runtime_coercions.insert(x.runtime_coercions.end(), y.runtime_coercions.begin(), y.runtime_coercions.end());
  // the shape map is threaded during the difference computation: the last
  // one contains all previous elements.
  x.shape_map = y.shape_map;
  x.deep_modifications = x.deep_modifications || y.deep_modifications;
  x.errors.insert(x.errors.end(), y.errors.begin(), y.errors.end());
  x.untypables.insert(x.untypables.end(), y.untypables.begin(), y.untypables.end());
  return x;
}

MtyResult modtypes_(const CoreRelation& core, Direction direction, const Location& loc, env::t env, subst::t s,
                    const ModuleType* mty1, const ModuleType* mty2, shape::t shape);
MtySymResult try_modtypes(const CoreRelation& core, Direction direction, const Location& loc, env::t env, subst::t s,
                          const ModuleType* mty1, const ModuleType* mty2, shape::t orig_shape);
struct FunctorParamResult {
  Result<const tt::ModuleCoercion*, E::FunctorParamSymptom> cc;
  env::t env;
  subst::t subst;
};
FunctorParamResult functor_param(const CoreRelation& core, Direction direction, const Location& loc, env::t env,
                                 subst::t s, const FunctorParameter& param1, const FunctorParameter& param2);
std::pair<env::t, subst::t> equate_one_functor_param(subst::t s, env::t env, const ModuleType* arg2_, Ident::t name1,
                                                     Ident::t name2);
MtyResult strengthened_modtypes(const CoreRelation& core, Direction direction, const Location& loc, bool aliasable,
                                env::t env, subst::t s, const ModuleType* mty1, Path::t path1,
                                const ModuleType* mty2, shape::t shape);
MtyResult strengthened_module_decl_(const CoreRelation& core, const Location& loc, bool aliasable,
                                    Direction direction, env::t env, subst::t s, const ModuleDeclaration* md1,
                                    Path::t path1, const ModuleDeclaration* md2, shape::t shape);
Result<CoercionShape, E::SignatureSymptom> signatures_(const CoreRelation& core, Direction direction,
                                                       const Location& loc, env::t env, subst::t s, Signature sig1,
                                                       Signature sig2, shape::t mod_shape);
struct Paired {
  const SignatureItem* item1;
  const SignatureItem* item2;
  long pos;
};
SignDiff signature_components(const CoreRelation& core, Direction direction, const Location& loc, env::t old_env,
                              env::t env, subst::t s, shape::t orig_shape, shape::ItemMap shape_map,
                              const std::vector<Paired>& paired, std::size_t k);
MtyResult module_declarations(const CoreRelation& core, const Location& loc, env::t env, Direction direction,
                              subst::t s, Ident::t id1, const ModuleDeclaration* md1, const ModuleDeclaration* md2,
                              shape::t orig_shape);
Result<const tt::ModuleCoercion*, E::SigitemSymptom> modtype_infos(const CoreRelation& core, const Location& loc,
                                                                  env::t env, Direction direction, subst::t s,
                                                                  Ident::t id, const ModtypeDeclaration* info1,
                                                                  const ModtypeDeclaration* info2);
Result<const tt::ModuleCoercion*, E::ModuleTypeDeclarationSymptom> check_modtype_equiv_(
    const CoreRelation& core, Direction direction, const Location& loc, env::t env, const ModuleType* mty1,
    const ModuleType* mty2);

MtyResult modtypes_(const CoreRelation& core, Direction direction, const Location& loc, env::t env, subst::t s,
                    const ModuleType* mty1, const ModuleType* mty2, shape::t shape) {
  MtySymResult r = try_modtypes(core, direction, loc, env, s, mty1, mty2, shape);
  if (r.ok) return MtyResult::Ok(r.value);
  const ModuleType* mty2_ = subst::modtype(subst::Scoping::make_local(), s, mty2);
  return MtyResult::Err(E::ModuleTypeDiff{mty1, mty2_, std::make_shared<const E::ModuleTypeSymptom>(r.error)});
}

MtySymResult try_modtypes(const CoreRelation& core, Direction direction, const Location& loc, env::t env, subst::t s,
                          const ModuleType* mty1, const ModuleType* mty2, shape::t orig_shape) {
  using CMK = E::CoreModuleTypeSymptom::Kind;
  auto ok_none = [&] { return MtySymResult::Ok({tt::tcoerce_none(), orig_shape}); };
  if (mty1->kind == MK::Mty_alias && mty2->kind == MK::Mty_alias) {
    if (equal_module_paths(env, mty1->path, s, mty2->path)) return ok_none();
    return MtySymResult::Err(mt_core(CMK::Incompatible_aliases));
  }
  if (mty1->kind == MK::Mty_alias) {
    Path::t p1;
    try {
      Location none = location::none();
      p1 = env::normalize_module_path(&none, env, mty1->path);
    } catch (const env::Error& e) {
      if (e.kind != env::Error::Kind::Missing_module) throw;
      return MtySymResult::Err(mt_core(CMK::Unbound_module_path, e.path2));
    }
    auto em = expand_module_alias_(false, env, p1);
    if (!em.ok) {
      E::ModuleTypeSymptom sy{E::ModuleTypeSymptom::Kind::Mt_core};
      sy.core = em.error;
      return MtySymResult::Err(sy);
    }
    MtyResult r = strengthened_modtypes(core, direction, loc, true, env, s, em.value, p1, mty2, orig_shape);
    if (r.ok) return MtySymResult::Ok(r.value);
    E::ModuleTypeSymptom sy{E::ModuleTypeSymptom::Kind::After_alias_expansion};
    sy.diff = share(r.error);
    return MtySymResult::Err(sy);
  }
  if (mty1->kind == MK::Mty_ident && mty2->kind == MK::Mty_ident) {
    Path::t p1 = env::normalize_modtype_path(env, mty1->path);
    Path::t p2 = env::normalize_modtype_path(env, subst::modtype_path(s, mty2->path));
    if (path::same(p1, p2)) return ok_none();
    // (expand_modtype_path env p1, expand_modtype_path env p2): right to left
    const ModuleType* e2 = expand_modtype_path(env, p2);
    const ModuleType* e1 = expand_modtype_path(env, p1);
    if (e1 && e2) return try_modtypes(core, direction, loc, env, s, e1, e2, orig_shape);
    return MtySymResult::Err(mt_core(CMK::Abstract_module_type));
  }
  if (mty1->kind == MK::Mty_ident) {
    Path::t p1 = env::normalize_modtype_path(env, mty1->path);
    if (const ModuleType* e1 = expand_modtype_path(env, p1))
      return try_modtypes(core, direction, loc, env, s, e1, mty2, orig_shape);
    return MtySymResult::Err(mt_core(CMK::Abstract_module_type));
  }
  if (mty2->kind == MK::Mty_ident) {
    Path::t p2 = env::normalize_modtype_path(env, subst::modtype_path(s, mty2->path));
    if (const ModuleType* e2 = expand_modtype_path(env, p2))
      return try_modtypes(core, direction, loc, env, s, mty1, e2, orig_shape);
    if (mty1->kind == MK::Mty_functor)
      return MtySymResult::Err(functor_params(retrieve_functor_params(env, mty1), E::FunctorParamsInfo{{}, mty2}));
    return MtySymResult::Err(mt_core(CMK::Not_an_identifier));
  }
  if (mty1->kind == MK::Mty_signature && mty2->kind == MK::Mty_signature) {
    auto r = signatures_(core, direction, loc, env, s, mty1->sign, mty2->sign, orig_shape);
    if (r.ok) return MtySymResult::Ok(r.value);
    E::ModuleTypeSymptom sy{E::ModuleTypeSymptom::Kind::Signature};
    sy.sig = std::make_shared<const E::SignatureSymptom>(r.error);
    return MtySymResult::Err(sy);
  }
  if (mty1->kind == MK::Mty_functor && mty2->kind == MK::Mty_functor) {
    const FunctorParameter& param1 = mty1->param;
    const FunctorParameter& param2 = mty2->param;
    FunctorParamResult fp = functor_param(core, negate(direction), loc, env, s, param1, param2);
    env::t env2 = fp.env;
    subst::t s2 = fp.subst;
    Ident::t var;
    shape::t res_shape;
    if (auto d = shape::decompose_abs(orig_shape)) {
      var = d->first;
      res_shape = d->second;
    } else {
      // Using a fresh variable with a placeholder uid here is fine
      auto [v, shape_var] = shape::fresh_var(uid::internal_not_actually_unique());
      var = v;
      res_shape = shape::app(nullptr, orig_shape, shape_var);
    }
    MtyResult cc_res = modtypes_(core, direction, loc, env2, s2, mty1->res, mty2->res, res_shape);
    if (fp.cc.ok && cc_res.ok) {
      shape::t final_res_shape = cc_res.value.second;
      shape::t final_shape = final_res_shape == res_shape ? orig_shape : shape::abs(nullptr, var, final_res_shape);
      if (fp.cc.value->kind == CK::Tcoerce_none && cc_res.value.first->kind == CK::Tcoerce_none)
        return MtySymResult::Ok({tt::tcoerce_none(), final_shape});
      auto* c = make<tt::ModuleCoercion>(tt::ModuleCoercion{CK::Tcoerce_functor});
      c->arg = fp.cc.value;
      c->res = cc_res.value.first;
      return MtySymResult::Ok({c, final_shape});
    }
    if (!cc_res.ok && cc_res.error.symptom->kind == E::ModuleTypeSymptom::Kind::Functor_params) {
      const E::ModuleTypeSymptom& res = *cc_res.error.symptom;
      // (Error.cons_arg param1 res.got, Error.cons_arg param2 res.expected)
      E::FunctorParamsInfo got = cons_arg(param1, res.got);
      E::FunctorParamsInfo expected = cons_arg(param2, res.expected);
      return MtySymResult::Err(functor_params(got, expected));
    }
    if (!fp.cc.ok) {
      auto params = [&](const FunctorParameter& param, const ModuleType* res) {
        return cons_arg(param, retrieve_functor_params(env2, res));
      };
      // Error.functor_params (params env param1 res1) (params env param2 res2): right to left
      E::FunctorParamsInfo i2 = params(param2, mty2->res);
      E::FunctorParamsInfo i1 = params(param1, mty1->res);
      return MtySymResult::Err(functor_params(i1, i2));
    }
    E::ModuleTypeSymptom sy{E::ModuleTypeSymptom::Kind::Functor_result};
    sy.diff = share(cc_res.error);
    return MtySymResult::Err(sy);
  }
  if (mty1->kind == MK::Mty_functor || mty2->kind == MK::Mty_functor) {
    E::FunctorParamsInfo i2 = retrieve_functor_params(env, mty2);
    E::FunctorParamsInfo i1 = retrieve_functor_params(env, mty1);
    return MtySymResult::Err(functor_params(i1, i2));
  }
  // _, Mty_alias _
  return MtySymResult::Err(mt_core(CMK::Not_an_alias));
}

// Functor parameters
FunctorParamResult functor_param(const CoreRelation& core, Direction direction, const Location& loc, env::t env,
                                 subst::t s, const FunctorParameter& param1, const FunctorParameter& param2) {
  using R = Result<const tt::ModuleCoercion*, E::FunctorParamSymptom>;
  if (param1.is_unit && param2.is_unit) return {R::Ok(tt::tcoerce_none()), env, s};
  if (!param1.is_unit && !param2.is_unit) {
    const ModuleType* arg2_ = subst::modtype(subst::Scoping::keep(), s, param2.mty);
    R cc_arg;
    MtyResult r = modtypes_(core, direction, loc, env, subst::identity(), arg2_, param1.mty, shape::dummy_mod());
    if (r.ok) {
      cc_arg = R::Ok(r.value.first);
    } else {
      E::FunctorParamSymptom sy{E::FunctorParamSymptom::Kind::Mismatch};
      sy.mismatch = share(r.error);
      cc_arg = R::Err(sy);
    }
    auto [env2, s2] = equate_one_functor_param(s, env, arg2_, param1.id, param2.id);
    return {cc_arg, env2, s2};
  }
  E::FunctorParamSymptom sy{E::FunctorParamSymptom::Kind::Incompatible_params};
  sy.arg = param1;
  sy.param = param2;
  return {R::Err(sy), env, s};
}

std::pair<env::t, subst::t> equate_one_functor_param(subst::t s, env::t env, const ModuleType* arg2_, Ident::t name1,
                                                     Ident::t name2) {
  if (name1 && name2) {
    // two matching abstract parameters: we add one identifier to the
    // environment and record the equality between the two identifiers in
    // the substitution  (tuple: right to left)
    subst::t s2 = subst::add_module(name2, Path::pident(name1), s);
    env::t e2 = env::add_module(name1, ModulePresence::Mp_present, arg2_, env);
    return {e2, s2};
  }
  if (!name1 && name2) {
    Ident::t id1 = ident::rename(name2);
    subst::t s2 = subst::add_module(name2, Path::pident(id1), s);
    env::t e2 = env::add_module(id1, ModulePresence::Mp_present, arg2_, env);
    return {e2, s2};
  }
  if (name1) return {env::add_module(name1, ModulePresence::Mp_present, arg2_, env), s};
  return {env, s};
}

MtyResult strengthened_modtypes(const CoreRelation& core, Direction direction, const Location& loc, bool aliasable,
                                env::t env, subst::t s, const ModuleType* mty1, Path::t path1,
                                const ModuleType* mty2, shape::t shape) {
  if (mty1->kind == MK::Mty_ident && mty2->kind == MK::Mty_ident &&
      equal_modtype_paths(env, mty1->path, s, mty2->path))
    return MtyResult::Ok({tt::tcoerce_none(), shape});
  const ModuleType* m1 = mtype::strengthen(aliasable, env, mty1, path1);
  return modtypes_(core, direction, loc, env, s, m1, mty2, shape);
}

MtyResult strengthened_module_decl_(const CoreRelation& core, const Location& loc, bool aliasable,
                                    Direction direction, env::t env, subst::t s, const ModuleDeclaration* md1,
                                    Path::t path1, const ModuleDeclaration* md2, shape::t shape) {
  if (md1->md_type->kind == MK::Mty_ident && md2->md_type->kind == MK::Mty_ident &&
      equal_modtype_paths(env, md1->md_type->path, s, md2->md_type->path))
    return MtyResult::Ok({tt::tcoerce_none(), shape});
  const ModuleDeclaration* m1 = mtype::strengthen_decl(aliasable, env, md1, path1);
  return modtypes_(core, direction, loc, env, s, m1->md_type, md2->md_type, shape);
}

// Inclusion between signatures
Result<CoercionShape, E::SignatureSymptom> signatures_(const CoreRelation& core, Direction direction,
                                                       const Location& loc, env::t env, subst::t s, Signature sig1,
                                                       Signature sig2, shape::t mod_shape) {
  using R = Result<CoercionShape, E::SignatureSymptom>;
  // Environment used to check inclusion of components
  env::t new_env = env::add_signature(sig1, env::in_signature(true, env));
  // Keep ids for module aliases
  std::vector<tt::IdPosCoercion> id_pos_list;  // head first (fold_left conses)
  {
    long pos = 0;
    for (auto* item : sig1) {
      if (item->kind == SK::Sig_module && item->presence == ModulePresence::Mp_present) {
        id_pos_list.insert(id_pos_list.begin(), tt::IdPosCoercion{item->id, pos, tt::tcoerce_none()});
        ++pos;
      } else if (is_runtime_component(item)) {
        ++pos;
      }
    }
  }
  // Build a table of the components of sig1, along with their positions.
  // The table is indexed by kind and name of component
  struct Comp {
    Ident::t id;
    const SignatureItem* item;
    long pos;
  };
  std::map<FieldDesc, Comp, FieldDescLess> comps1;
  long exported_len1 = 0, runtime_len1 = 0;
  {
    long pos = 0;
    for (auto* item : sig1) {
      long p, nextpos;
      if (is_runtime_component(item)) {
        p = pos;
        nextpos = pos + 1;
      } else {
        p = -1;
        nextpos = pos;
      }
      if (item_visibility(item) == Visibility::Exported) {
        // (do not pair private items)
        ItemIdentName n = item_ident_name(item);
        ++exported_len1;
        comps1.insert_or_assign(n.desc, Comp{n.id, item, p});
      }
      pos = nextpos;
    }
    runtime_len1 = pos;
  }
  long exported_len2 = 0, runtime_len2 = 0;
  for (auto* i : sig2) {
    if (item_visibility(i) == Visibility::Exported) ++exported_len2;
    if (is_runtime_component(i)) ++runtime_len2;
  }
  // Pair each component of sig2 with a component of sig1, identifying the
  // names along the way.
  std::map<FieldDesc, const SignatureItem*, FieldDescLess> additions;
  for (auto& [k, c] : comps1) additions.emplace(k, c.item);
  std::vector<Paired> paired;                    // source order
  std::vector<const SignatureItem*> unpaired;    // head first (consed)
  subst::t sub = s;
  for (auto* item2 : sig2) {
    ItemIdentName n2 = item_ident_name(item2);
    FieldDesc name2 = n2.desc;
    bool report = true;
    if (item2->kind == SK::Sig_type && !item2->type->type_manifest && name2.kind == FieldKind::Field_type &&
        btype::is_row_name(name2.name)) {
      // Do not report in case of failure, as the main type will generate an
      // error
      name2 = FieldDesc{zstr(name2.name.substr(0, name2.name.size() - 4)), FieldKind::Field_type};
      report = false;
    }
    auto f = comps1.find(name2);
    if (f != comps1.end()) {
      additions.erase(name2);
      sub = item_subst(f->second.id, item2, sub);
      paired.push_back({f->second.item, item2, f->second.pos});
    } else if (report) {
      unpaired.insert(unpaired.begin(), item2);
    }
  }
  SignDiff d = signature_components(core, direction, loc, env, new_env, sub, mod_shape, shape::map::empty(), paired, 0);
  if (unpaired.empty() && d.errors.empty() && d.untypables.empty()) {
    shape::t sh = (!d.deep_modifications && exported_len1 == exported_len2)
                      ? mod_shape
                      : shape::str(mod_shape->has_uid ? &mod_shape->uid : nullptr, d.shape_map);
    if (runtime_len1 == runtime_len2)  // see PR#5098
      return R::Ok({simplify_structure_coercion(d.runtime_coercions, id_pos_list), sh});
    auto* c = make<tt::ModuleCoercion>(tt::ModuleCoercion{CK::Tcoerce_structure});
    c->pos_cc = slice(d.runtime_coercions);
    c->id_pos_cc = slice(id_pos_list);
    return R::Ok({c, sh});
  }
  E::SignatureSymptom sy;
  sy.env = new_env;
  sy.subst = sub;
  sy.sig1 = sig1;
  sy.sig2 = sig2;
  sy.missings = unpaired;
  sy.incompatibles = d.errors;
  sy.oks = d.runtime_coercions;
  for (auto& [k, it] : additions) sy.additions.push_back(it);
  sy.untypables = d.untypables;
  return R::Err(sy);
}

// Inclusion between signature components
SignDiff signature_components(const CoreRelation& core, Direction direction, const Location& loc, env::t old_env,
                              env::t env, subst::t s, shape::t orig_shape, shape::ItemMap shape_map,
                              const std::vector<Paired>& paired, std::size_t k) {
  if (k == paired.size()) {
    SignDiff e;
    e.shape_map = shape_map;
    return e;
  }
  const SignatureItem* sigi1 = paired[k].item1;
  const SignatureItem* sigi2 = paired[k].item2;
  long pos = paired[k].pos;
  bool shape_modified = false;
  CoreResult item;
  bool recoverable = false;
  bool present_at_runtime = false;
  if (sigi1->kind == SK::Sig_value && sigi2->kind == SK::Sig_value) {
    item = core.value_descriptions(loc, env, direction, s, sigi1->id, sigi1->value, sigi2->value);
    recoverable = true;
    present_at_runtime = sigi2->value->val_kind.kind != ValueKind::Kind::Val_prim;
    shape_map = shape::map::add_value_proj(shape_map, sigi1->id, orig_shape);
  } else if (sigi1->kind == SK::Sig_type && sigi2->kind == SK::Sig_type) {
    item = core.type_declarations(loc, env, direction, s, sigi1->id, sigi1->type, sigi2->type);
    // Right now we don't filter hidden constructors / labels from the shape.
    shape_map = shape::map::add_type_proj(shape_map, sigi1->id, orig_shape);
  } else if (sigi1->kind == SK::Sig_typext && sigi2->kind == SK::Sig_typext) {
    item = core.extension_constructors(loc, env, direction, s, sigi1->id, sigi1->ext, sigi2->ext);
    shape_map = shape::map::add_extcons_proj(shape_map, sigi1->id, orig_shape);
    present_at_runtime = true;
  } else if (sigi1->kind == SK::Sig_module && sigi2->kind == SK::Sig_module) {
    shape::t mod_orig_shape = shape::proj(nullptr, orig_shape, shape::item::module_(sigi1->id));
    MtyResult r = module_declarations(core, loc, env, direction, s, sigi1->id, sigi1->md, sigi2->md, mod_orig_shape);
    if (r.ok) {
      if (r.value.second != mod_orig_shape) shape_modified = true;
      shape::t mod_shape = shape::set_uid_if_none(r.value.second, sigi1->md->md_uid);
      item = CoreResult::Ok(r.value.first);
      shape_map = shape::map::add_module(shape_map, sigi1->id, mod_shape);
    } else {
      E::SigitemSymptom sy{E::SigitemSymptom::Kind::Module_type};
      sy.diff = share(r.error);
      item = CoreResult::Err(sy);
      // We add the original shape to the map, even though there is a type
      // error.  It could still be useful for merlin.
      shape_map = shape::map::add_module(shape_map, sigi1->id, mod_orig_shape);
    }
    ModulePresence pres1 = sigi1->presence, pres2 = sigi2->presence;
    if (pres1 == ModulePresence::Mp_present && pres2 == ModulePresence::Mp_present) {
      present_at_runtime = true;
    } else if (pres2 == ModulePresence::Mp_absent) {
      present_at_runtime = false;
    } else if (sigi1->md->md_type->kind == MK::Mty_alias) {
      present_at_runtime = true;
      if (item.ok) {
        auto* c = make<tt::ModuleCoercion>(tt::ModuleCoercion{CK::Tcoerce_alias});
        c->alias_env = env;
        c->alias_path = sigi1->md->md_type->path;
        c->alias_coercion = item.value;
        item.value = c;
      }
    } else {
      throw std::logic_error("Includemod.signature_components: absent module");
    }
  } else if (sigi1->kind == SK::Sig_modtype && sigi2->kind == SK::Sig_modtype) {
    item = modtype_infos(core, loc, env, direction, s, sigi1->id, sigi1->mtd, sigi2->mtd);
    shape_map = shape::map::add_module_type_proj(shape_map, sigi1->id, orig_shape);
  } else if (sigi1->kind == SK::Sig_class && sigi2->kind == SK::Sig_class) {
    item = core.class_declarations(loc, env, direction, s, sigi1->id, sigi1->cls, sigi2->cls);
    shape_map = shape::map::add_class_proj(shape_map, sigi1->id, orig_shape);
    present_at_runtime = true;
  } else if (sigi1->kind == SK::Sig_class_type && sigi2->kind == SK::Sig_class_type) {
    item = core.class_type_declarations(loc, env, direction, s, sigi1->id, sigi1->clty, sigi2->clty);
    shape_map = shape::map::add_class_type_proj(shape_map, sigi1->id, orig_shape);
  } else {
    throw std::logic_error("Includemod.signature_components");
  }
  bool deep_modifications = shape_modified;
  SignDiff first;
  first.deep_modifications = deep_modifications;
  if (item.ok) {
    // (Uid.Deps.record_declaration_dependency: cmt only)
    if (present_at_runtime) first.runtime_coercions.push_back({pos, item.value});
  } else {
    first.errors.push_back({sigi1, item.error});
  }
  bool cont = item.ok || recoverable;
  SignDiff rest;
  if (cont) {
    rest = signature_components(core, direction, loc, old_env, env, s, orig_shape, shape_map, paired, k + 1);
  } else {
    for (std::size_t j = k + 1; j < paired.size(); ++j)
      rest.untypables.push_back({paired[j].item1, paired[j].item2, paired[j].pos});
  }
  return merge(first, rest);
}

MtyResult module_declarations(const CoreRelation& core, const Location& loc, env::t env, Direction direction,
                              subst::t s, Ident::t id1, const ModuleDeclaration* md1, const ModuleDeclaration* md2,
                              shape::t orig_shape) {
  // (alerts; Env.mark_module_used)
  Path::t p1 = Path::pident(id1);
  return strengthened_modtypes(core, direction, loc, true, env, s, md1->md_type, p1, md2->md_type, orig_shape);
}

// Inclusion between module type specifications
Result<const tt::ModuleCoercion*, E::SigitemSymptom> modtype_infos(const CoreRelation& core, const Location& loc,
                                                                  env::t env, Direction direction, subst::t s,
                                                                  Ident::t id, const ModtypeDeclaration* info1,
                                                                  const ModtypeDeclaration* info2_) {
  using R = Result<const tt::ModuleCoercion*, E::SigitemSymptom>;
  const ModtypeDeclaration* info2 = subst::modtype_declaration(subst::Scoping::keep(), s, info2_);
  Result<const tt::ModuleCoercion*, E::ModuleTypeDeclarationSymptom> r;
  if (!info2->mtd_type) {
    r.ok = true;
    r.value = tt::tcoerce_none();
  } else if (info1->mtd_type) {
    r = check_modtype_equiv_(core, direction, loc, env, info1->mtd_type, info2->mtd_type);
  } else {
    auto* mty1 = make<ModuleType>(ModuleType{MK::Mty_ident});
    mty1->path = Path::pident(id);
    r = check_modtype_equiv_(core, direction, loc, env, mty1, info2->mtd_type);
  }
  if (r.ok) return R::Ok(r.value);
  E::SigitemSymptom sy{E::SigitemSymptom::Kind::Module_type_declaration};
  sy.mtd1 = info1;
  sy.mtd2 = info2;
  sy.mtd_symptom = r.error;
  return R::Err(sy);
}

Result<const tt::ModuleCoercion*, E::ModuleTypeDeclarationSymptom> check_modtype_equiv_(
    const CoreRelation& core, Direction direction, const Location& loc, env::t env, const ModuleType* mty1,
    const ModuleType* mty2) {
  using R = Result<const tt::ModuleCoercion*, E::ModuleTypeDeclarationSymptom>;
  using DK = E::ModuleTypeDeclarationSymptom::Kind;
  bool nested_eq = direction.in_eq;
  Direction d = enter_eq(direction);
  MtyResult c1 = modtypes_(core, d, loc, env, subst::identity(), mty1, mty2, shape::dummy_mod());
  // For nested module type paths, we check only one side of the
  // equivalence: the outer module type checks the other side.
  std::optional<MtyResult> c2;
  if (!nested_eq) c2 = modtypes_(core, negate(d), loc, env, subst::identity(), mty2, mty1, shape::dummy_mod());
  bool c2_ok = !c2 || c2->ok;
  if (c1.ok && c2_ok) {
    if (c1.value.first->kind == CK::Tcoerce_none && (!c2 || c2->value.first->kind == CK::Tcoerce_none))
      return R::Ok(tt::tcoerce_none());
    E::ModuleTypeDeclarationSymptom sy{DK::Illegal_permutation};
    sy.coercion = c1.value.first;
    return R::Err(sy);
  }
  if (c1.ok) {
    E::ModuleTypeDeclarationSymptom sy{DK::Not_greater_than};
    sy.diff = share(c2->error);
    return R::Err(sy);
  }
  if (c2_ok) {
    E::ModuleTypeDeclarationSymptom sy{DK::Not_less_than};
    sy.diff = share(c1.error);
    return R::Err(sy);
  }
  E::ModuleTypeDeclarationSymptom sy{DK::Incomparable};
  sy.less_than = share(c1.error);
  sy.greater_than = share(c2->error);
  return R::Err(sy);
}

Result<const tt::ModuleCoercion*, E::ModuleTypeDiff> check_modtype_inclusion_raw(const Location& loc, env::t env,
                                                                                 const ModuleType* mty1, Path::t path1,
                                                                                 const ModuleType* mty2) {
  using R = Result<const tt::ModuleCoercion*, E::ModuleTypeDiff>;
  bool aliasable = env::is_aliasable(path1, env);
  Direction direction = unknown(true);
  MtyResult r = strengthened_modtypes(core_inclusion(), direction, loc, aliasable, env, subst::identity(), mty1, path1,
                                      mty2, shape::dummy_mod());
  if (r.ok) return R::Ok(r.value.first);
  return R::Err(r.error);
}

void check_functor_application_in_path(bool errors, const Location& loc, Longident::t lid_whole_app,
                                       Path::t f0_path,
                                       const std::vector<std::pair<Path::t, const ModuleType*>>& args,
                                       Path::t arg_path, const ModuleType* arg_mty, const ModuleType* param_mty,
                                       env::t env) {
  auto r = check_modtype_inclusion_raw(loc, env, arg_mty, arg_path, param_mty);
  if (r.ok) return;
  if (!errors) throw env::NotFound{};
  ApplyError ae;
  const ModuleType* mty_f = env::find_module(f0_path, env)->md_type;
  for (auto& [p, m] : args) {
    bool aliasable = env::is_aliasable(p, env);
    const ModuleType* smd = mtype::strengthen(aliasable, env, m, p);
    ae.args.push_back({E::FunctorArgDescr{E::FunctorArgDescr::Kind::Named, p}, smd});
  }
  ae.loc = loc;
  ae.env = env;
  ae.app_name = ApplicationName{ApplicationNameKind::Full_application_path, lid_whole_app};
  ae.mty_f = mty_f;
  throw ae;
}

E::All in_module_type(const E::ModuleTypeDiff& d) {
  E::All a{E::All::Kind::In_Module_type};
  a.diff = share(d);
  return a;
}

}  // namespace

void install_forward_refs() { env::check_functor_application = check_functor_application_in_path; }

std::optional<Explanation> check_modtype_inclusion(const Location& loc, env::t env, const ModuleType* mty1,
                                                   Path::t path1, const ModuleType* mty2) {
  auto r = check_modtype_inclusion_raw(loc, env, mty1, path1, mty2);
  if (r.ok) return std::nullopt;
  return Explanation{env, in_module_type(r.error)};
}

// Check that an implementation of a compilation unit meets its interface.
std::pair<const tt::ModuleCoercion*, shape::t> compunit(env::t env, bool mark, std::string_view impl_name,
                                                       Signature impl_sig, std::string_view intf_name,
                                                       Signature intf_sig, shape::t unit_shape) {
  // Location.in_file impl_name
  Location loc = location::none();
  loc.loc_start.pos_fname = loc.loc_end.pos_fname = zstr(impl_name);
  loc.loc_start.pos_lnum = loc.loc_end.pos_lnum = 1;
  loc.loc_start.pos_bol = loc.loc_end.pos_bol = 0;
  loc.loc_start.pos_cnum = loc.loc_end.pos_cnum = -1;
  loc.loc_ghost = true;
  Direction direction = strictly_positive(mark, false);
  auto r = signatures_(core_inclusion(), direction, loc, env, subst::identity(), impl_sig, intf_sig, unit_shape);
  if (!r.ok) {
    E::All a{E::All::Kind::In_Compilation_unit};
    a.got_name = std::string(impl_name);
    a.expected_name = std::string(intf_name);
    a.sig = std::make_shared<const E::SignatureSymptom>(r.error);
    throw Error(Explanation{env, a});
  }
  return r.value;
}

// Hide the context and substitution parameters to the outside world
std::pair<const tt::ModuleCoercion*, shape::t> modtypes_constraint(shape::t shape, const Location& loc, env::t env,
                                                                  bool mark, const ModuleType* mty1,
                                                                  const ModuleType* mty2) {
  // modtypes with shape is used when typing module expressions in Typemod
  Direction direction = strictly_positive(mark, true);
  MtyResult r = modtypes_(core_inclusion(), direction, loc, env, subst::identity(), mty1, mty2, shape);
  if (r.ok) return r.value;
  throw Error(Explanation{env, in_module_type(r.error)});
}

void modtypes_consistency(const Location& loc, env::t env, const ModuleType* mty1, const ModuleType* mty2) {
  Direction direction = unknown(false);
  MtyResult r = modtypes_(core_consistency(), direction, loc, env, subst::identity(), mty1, mty2, shape::dummy_mod());
  if (!r.ok) throw Error(Explanation{env, in_module_type(r.error)});
}

const tt::ModuleCoercion* modtypes(const Location& loc, env::t env, bool mark, const ModuleType* mty1,
                                   const ModuleType* mty2) {
  Direction direction = unknown(mark);
  MtyResult r = modtypes_(core_inclusion(), direction, loc, env, subst::identity(), mty1, mty2, shape::dummy_mod());
  if (r.ok) return r.value.first;
  throw Error(Explanation{env, in_module_type(r.error)});
}

static const tt::ModuleCoercion* gen_signatures(env::t env, subst::t s, Direction direction, Signature sig1,
                                                Signature sig2) {
  auto r = signatures_(core_inclusion(), direction, location::none(), env, s, sig1, sig2, shape::dummy_mod());
  if (r.ok) return r.value.first;
  E::All a{E::All::Kind::In_Signature};
  a.sig = std::make_shared<const E::SignatureSymptom>(r.error);
  throw Error(Explanation{env, a});
}

const tt::ModuleCoercion* signatures(env::t env, subst::t s, bool mark, Signature sig1, Signature sig2) {
  return gen_signatures(env, s, unknown(mark), sig1, sig2);
}

void check_implementation(env::t env, Signature impl, Signature intf) {
  gen_signatures(env, subst::identity(), strictly_positive(true, false), impl, intf);
}

void type_declarations(const Location& loc, env::t env, bool mark, Ident::t id, const TypeDeclaration* decl1,
                       const TypeDeclaration* decl2) {
  Direction direction = unknown(mark);
  CoreResult r = ci_type_declarations(loc, env, direction, subst::identity(), id, decl1, decl2);
  if (r.ok) return;
  if (r.error.kind != E::SigitemSymptom::Kind::Core) throw std::logic_error("Includemod.type_declarations");
  E::All a{E::All::Kind::In_Type_declaration};
  a.id = id;
  a.core = r.error.core;
  throw Error(Explanation{env, a});
}

const tt::ModuleCoercion* strengthened_module_decl(const Location& loc, bool aliasable, env::t env, bool mark,
                                                   const ModuleDeclaration* md1, Path::t path1,
                                                   const ModuleDeclaration* md2) {
  Direction direction = unknown(mark);
  MtyResult r = strengthened_module_decl_(core_inclusion(), loc, aliasable, direction, env, subst::identity(), md1,
                                          path1, md2, shape::dummy_mod());
  if (r.ok) return r.value.first;
  throw Error(Explanation{env, in_module_type(r.error)});
}

const ModuleType* expand_module_alias(bool strengthen, env::t env, Path::t path) {
  auto r = expand_module_alias_(strengthen, env, path);
  if (r.ok) return r.value;
  E::All a{E::All::Kind::In_Expansion};
  a.expansion = E::CoreModuleTypeSymptom{E::CoreModuleTypeSymptom::Kind::Unbound_module_path, path};
  throw Error(Explanation{env, a});
}

void check_modtype_equiv(const Location& loc, env::t env, Ident::t id, const ModuleType* mty1,
                         const ModuleType* mty2) {
  Direction direction = unknown(true);
  auto r = check_modtype_equiv_(core_inclusion(), direction, loc, env, mty1, mty2);
  if (r.ok) return;
  E::All a{E::All::Kind::In_Module_type_substitution};
  a.id = id;
  a.mty1 = mty1;
  a.mty2 = mty2;
  a.mtd_symptom = r.error;
  throw Error(Explanation{env, a});
}

}  // namespace cppcaml::typing::includemod
