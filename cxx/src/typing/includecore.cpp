// Port of typing/includecore.ml: inclusion checks for the core language.
// The record / variant difference lists (Diffing_with_keys, computed only to
// print errors) are reduced to the first mismatching field; the decisions
// are includecore.ml's.  Alerts / deprecation checks and usage marking only
// feed warnings and are left out.
#include "cppcaml/typing/includecore.hpp"
#include "cppcaml/typing/builtin_attributes.hpp"

#include "cppcaml/typing/btype.hpp"

namespace cppcaml::typing::includecore {

using namespace types;
using namespace btype;
using TK = TypeKind::Kind;

namespace {

bool equal_native_repr(const NativeRepr& a, const NativeRepr& b) {
  if (a.kind != b.kind) return false;
  if (a.kind == NativeRepr::Kind::Unboxed_integer) return a.bi == b.bi;
  return true;
}

std::optional<ValueMismatch> primitive_descriptions(const PrimitiveDescription* pd1, const PrimitiveDescription* pd2) {
  auto pm = [](PrimitiveMismatch m) {
    ValueMismatch v{ValueMismatch::Kind::Primitive_mismatch};
    v.prim = m;
    return v;
  };
  if (pd1->prim_name != pd2->prim_name) return pm(PrimitiveMismatch::Name);
  if (pd1->prim_arity != pd2->prim_arity) return pm(PrimitiveMismatch::Arity);
  if (!pd1->prim_alloc && pd2->prim_alloc) {
    ValueMismatch v = pm(PrimitiveMismatch::No_alloc);
    v.pos = Position::First;
    return v;
  }
  if (pd1->prim_alloc && !pd2->prim_alloc) {
    ValueMismatch v = pm(PrimitiveMismatch::No_alloc);
    v.pos = Position::Second;
    return v;
  }
  if (pd1->prim_native_name != pd2->prim_native_name) return pm(PrimitiveMismatch::Native_name);
  if (!equal_native_repr(pd1->prim_native_repr_res, pd2->prim_native_repr_res))
    return pm(PrimitiveMismatch::Result_repr);
  // native_repr_args
  auto& a1 = pd1->prim_native_repr_args;
  auto& a2 = pd2->prim_native_repr_args;
  if (a1.size() != a2.size()) throw std::logic_error("Includecore.native_repr_args");
  for (std::size_t i = 0; i < a1.size(); ++i)
    if (!equal_native_repr(a1[i], a2[i])) {
      ValueMismatch v = pm(PrimitiveMismatch::Argument_repr);
      v.index = static_cast<long>(i) + 1;
      return v;
    }
  return std::nullopt;
}

}  // namespace

// A value description [vd1] is consistent with the value description [vd2]
// if there is a context E such that [E |- vd1 <: vd2] for the ordinary
// subtyping.
const typedtree::ModuleCoercion* value_descriptions_consistency(env::t env, const ValueDescription* vd1,
                                                                const ValueDescription* vd2) {
  using VK = ValueKind::Kind;
  bool p1 = vd1->val_kind.kind == VK::Val_prim, p2 = vd2->val_kind.kind == VK::Val_prim;
  if (p1 && p2) {
    if (auto err = primitive_descriptions(vd1->val_kind.prim, vd2->val_kind.prim)) throw DontMatch(*err);
    return typedtree::tcoerce_none();
  }
  if (p1) {
    auto* pc = make<typedtree::PrimitiveCoercion>(
        typedtree::PrimitiveCoercion{vd1->val_kind.prim, vd2->val_type, env, vd1->val_loc});
    auto* c = make<typedtree::ModuleCoercion>(typedtree::ModuleCoercion{typedtree::ModuleCoercion::Kind::Tcoerce_primitive});
    c->prim = pc;
    return c;
  }
  if (p2) throw DontMatch(ValueMismatch{ValueMismatch::Kind::Not_a_primitive});
  return typedtree::tcoerce_none();
}

const typedtree::ModuleCoercion* value_descriptions(const Location& loc, env::t env, std::string_view name,
                                                    const ValueDescription* vd1, const ValueDescription* vd2) {
  builtin_attributes::check_alerts_inclusion(vd1->val_loc, vd2->val_loc, loc, vd1->val_attributes, vd2->val_attributes,
                                             name);
  try {
    ctype::moregeneral(env, vd1->val_type, vd2->val_type);
  } catch (const ctype::Moregen& m) {
    ValueMismatch v{ValueMismatch::Kind::Type};
    v.err = m.err;
    throw DontMatch(v);
  }
  return value_descriptions_consistency(env, vd1, vd2);
}

namespace {

// Inclusion between manifest types (particularly for private row types)
bool is_absrow(env::t env, TypeExpr* ty) {
  auto* tc = as<Tconstr>(get_desc(ty));
  if (!(tc && tc->path->kind == Path::Kind::Pident)) return false;
  // This function is checking for an abstract row on the side that is being
  // included into.  In this case, the abstract row variable has been
  // substituted for an object or variant type.
  DescKind k = get_desc(ctype::expand_head_nolink(env, ty))->kind;
  return k == DescKind::Tobject || k == DescKind::Tvariant;
}

TypeKindName of_kind(const TypeKind* k) {
  switch (k->kind) {
    case TK::Type_abstract: return {TypeKindName::Kind::Kind_abstract};
    case TK::Type_record: return {TypeKindName::Kind::Kind_record};
    case TK::Type_variant: return {TypeKindName::Kind::Kind_variant};
    case TK::Type_open: return {TypeKindName::Kind::Kind_open};
    case TK::Type_external: return {TypeKindName::Kind::Kind_external, k->external};
  }
  return {};
}

std::vector<TypeExpr*> vec(Slice<TypeExpr*> s) { return std::vector<TypeExpr*>(s.begin(), s.end()); }
std::vector<TypeExpr*> cat(const std::vector<TypeExpr*>& a, const std::vector<TypeExpr*>& b) {
  std::vector<TypeExpr*> r = a;
  r.insert(r.end(), b.begin(), b.end());
  return r;
}
// Ctype.equal env rename tl1 tl2, as an option of the error
std::optional<et::EqualityError> equal_opt(env::t env, bool rename, const std::vector<TypeExpr*>& tl1,
                                           const std::vector<TypeExpr*>& tl2) {
  try {
    ctype::equal(env, rename, slice(tl1), slice(tl2));
    return std::nullopt;
  } catch (const ctype::Equality& e) {
    return e.err;
  }
}

// ---- Record_diffing ----
std::optional<LabelMismatch> compare_labels(env::t env, const std::vector<TypeExpr*>& params1,
                                            const std::vector<TypeExpr*>& params2, const LabelDeclaration* ld1,
                                            const LabelDeclaration* ld2) {
  if (ld1->ld_mutable != ld2->ld_mutable) {
    LabelMismatch m{LabelMismatch::Kind::Mutability};
    m.pos = ld1->ld_mutable == MutableFlag::Mutable ? Position::First : Position::Second;
    return m;
  }
  if (ld1->ld_atomic != ld2->ld_atomic) {
    LabelMismatch m{LabelMismatch::Kind::Atomicity};
    m.pos = ld1->ld_atomic == AtomicFlag::Atomic ? Position::First : Position::Second;
    return m;
  }
  auto tl1 = cat(params1, {ld1->ld_type});
  auto tl2 = cat(params2, {ld2->ld_type});
  if (auto e = equal_opt(env, true, tl1, tl2)) {
    LabelMismatch m{LabelMismatch::Kind::Type};
    m.err = *e;
    return m;
  }
  return std::nullopt;
}

// Record_diffing.equal
bool record_equal(const Location& loc, env::t env, std::vector<TypeExpr*> params1, std::vector<TypeExpr*> params2,
                  Slice<const LabelDeclaration*> labels1, Slice<const LabelDeclaration*> labels2) {
  for (std::size_t k = 0;; ++k) {
    if (k == labels1.size() && k == labels2.size()) return true;
    if (k == labels1.size() || k == labels2.size()) return false;
    const LabelDeclaration* ld1 = labels1[k];
    const LabelDeclaration* ld2 = labels2[k];
    if (ident::name(ld1->ld_id) != ident::name(ld2->ld_id)) return false;
    builtin_attributes::check_deprecated_mutable_inclusion(ld1->ld_loc, ld2->ld_loc, loc, ld1->ld_attributes,
                                                           ld2->ld_attributes, ident::name(ld1->ld_id));
    if (compare_labels(env, params1, params2, ld1, ld2)) return false;
    // add arguments to the parameters, cf. PR#7378
    params1.insert(params1.begin(), ld1->ld_type);
    params2.insert(params2.begin(), ld2->ld_type);
  }
}

using ParamsState = std::pair<std::vector<TypeExpr*>, std::vector<TypeExpr*>>;

// Record_diffing.diffing
std::vector<RecordChange> record_diffing(const Location& loc, env::t env, const std::vector<TypeExpr*>& params1,
                                         const std::vector<TypeExpr*>& params2, Slice<const LabelDeclaration*> l,
                                         Slice<const LabelDeclaration*> r) {
  using LD = const LabelDeclaration*;
  using KD = diffing::KeyedDiff<LD, LD, LabelMismatch, ParamsState>;
  using M = diffing::Mismatch<LD, LD, LabelMismatch>;
  KD d;
  d.key_left = [](LD x) { return std::string(ident::name(x->ld_id)); };
  d.key_right = d.key_left;
  d.update = [](const KD::change& c, const ParamsState& st) {
    if (c.k != KD::change::K::Keep) return st;
    // We need to add equality between existential type parameters (in inline records)
    ParamsState r = st;
    r.first.insert(r.first.begin(), c.left.data->ld_type);
    r.second.insert(r.second.begin(), c.right.data->ld_type);
    return r;
  };
  d.test = [env](const ParamsState& st, const diffing::WithPos<LD>& x, const diffing::WithPos<LD>& y) {
    KD::TR res{};
    std::string name1(ident::name(x.data->ld_id)), name2(ident::name(y.data->ld_id));
    if (name1 != name2) {
      bool types_match = !compare_labels(env, st.first, st.second, x.data, y.data);
      M m{M::K::Name};
      m.types_match = types_match;
      m.pos = x.pos;
      m.got_name = name1;
      m.expected_name = name2;
      res.ok = false;
      res.err = m;
      return res;
    }
    if (std::optional<LabelMismatch> reason = compare_labels(env, st.first, st.second, x.data, y.data)) {
      M m{M::K::Type};
      m.pos = x.pos;
      m.got = x.data;
      m.expected = y.data;
      m.reason = *reason;
      res.ok = false;
      res.err = m;
      return res;
    }
    res.ok = true;
    return res;
  };
  d.weight = [](const KD::change& c) -> long {
    switch (c.k) {
      case KD::change::K::Insert:
      case KD::change::K::Delete: return 100;
      case KD::change::K::Keep: return 0;
      case KD::change::K::Change: return c.diff.k == M::K::Name ? (c.diff.types_match ? 98 : 99) : 50;
    }
    return 0;
  };
  return d.diff({params1, params2}, std::vector<LD>(l.begin(), l.end()), std::vector<LD>(r.begin(), r.end()));
}

std::optional<std::vector<RecordChange>> record_compare(const Location& loc, env::t env, const std::vector<TypeExpr*>& params1,
                                                        const std::vector<TypeExpr*>& params2,
                                                        Slice<const LabelDeclaration*> l,
                                                        Slice<const LabelDeclaration*> r) {
  if (record_equal(loc, env, params1, params2, l, r)) return std::nullopt;
  return record_diffing(loc, env, params1, params2, l, r);
}

std::optional<TypeMismatch> record_compare_with_representation(const Location& loc, env::t env, const std::vector<TypeExpr*>& params1,
                                                               const std::vector<TypeExpr*>& params2,
                                                               Slice<const LabelDeclaration*> l,
                                                               Slice<const LabelDeclaration*> r,
                                                               const RecordRepresentation& rep1,
                                                               const RecordRepresentation& rep2) {
  using RK = RecordRepresentation::Kind;
  if (auto ch = record_compare(loc, env, params1, params2, l, r)) {
    TypeMismatch m{TypeMismatch::Kind::Record_mismatch};
    m.record = RecordMismatch{RecordMismatch::Kind::Label_mismatch, *ch};
    return m;
  }
  auto unboxed = [](Position p) {
    TypeMismatch m{TypeMismatch::Kind::Unboxed_representation};
    m.pos = p;
    return m;
  };
  auto flt = [](Position p) {
    TypeMismatch m{TypeMismatch::Kind::Record_mismatch};
    m.record = RecordMismatch{RecordMismatch::Kind::Unboxed_float_representation, {}, p};
    return m;
  };
  if (rep1.kind == RK::Record_unboxed && rep2.kind == RK::Record_unboxed) return std::nullopt;
  if (rep1.kind == RK::Record_unboxed) return unboxed(Position::First);
  if (rep2.kind == RK::Record_unboxed) return unboxed(Position::Second);
  if (rep1.kind == RK::Record_float && rep2.kind == RK::Record_float) return std::nullopt;
  if (rep1.kind == RK::Record_float) return flt(Position::First);
  if (rep2.kind == RK::Record_float) return flt(Position::Second);
  if (rep1.kind == rep2.kind) return std::nullopt;
  throw std::logic_error("Includecore.Record_diffing.compare_with_representation");
}

// ---- Variant_diffing ----
std::optional<ConstructorMismatch> compare_constructor_arguments(const Location& loc, env::t env, const std::vector<TypeExpr*>& params1,
                                                                 const std::vector<TypeExpr*>& params2,
                                                                 const ConstructorArguments& arg1,
                                                                 const ConstructorArguments& arg2) {
  using CK = ConstructorArguments::Kind;
  if (arg1.kind == CK::Cstr_tuple && arg2.kind == CK::Cstr_tuple) {
    if (arg1.tuple.size() != arg2.tuple.size()) return ConstructorMismatch{ConstructorMismatch::Kind::Arity};
    // Ctype.equal must be called on all arguments at once, cf. PR#7378
    if (auto e = equal_opt(env, true, cat(params1, vec(arg1.tuple)), cat(params2, vec(arg2.tuple)))) {
      ConstructorMismatch m{ConstructorMismatch::Kind::Type};
      m.err = *e;
      return m;
    }
    return std::nullopt;
  }
  if (arg1.kind == CK::Cstr_record && arg2.kind == CK::Cstr_record) {
    if (auto ch = record_compare(loc, env, params1, params2, arg1.record, arg2.record)) {
      ConstructorMismatch m{ConstructorMismatch::Kind::Inline_record};
      m.changes = *ch;
      return m;
    }
    return std::nullopt;
  }
  ConstructorMismatch m{ConstructorMismatch::Kind::Kind_};
  m.pos = arg1.kind == CK::Cstr_record ? Position::First : Position::Second;
  return m;
}

std::optional<ConstructorMismatch> compare_constructors(const Location& loc, env::t env, const std::vector<TypeExpr*>& params1,
                                                        const std::vector<TypeExpr*>& params2, TypeExpr* res1,
                                                        TypeExpr* res2, const ConstructorArguments& args1,
                                                        const ConstructorArguments& args2) {
  if (res1 && res2) {
    if (auto e = equal_opt(env, true, {res1}, {res2})) {
      ConstructorMismatch m{ConstructorMismatch::Kind::Type};
      m.err = *e;
      return m;
    }
    return compare_constructor_arguments(loc, env, {res1}, {res2}, args1, args2);
  }
  if (res1 || res2) {
    ConstructorMismatch m{ConstructorMismatch::Kind::Explicit_return_type};
    m.pos = res1 ? Position::First : Position::Second;
    return m;
  }
  return compare_constructor_arguments(loc, env, params1, params2, args1, args2);
}

// Variant_diffing.equal
bool variant_equal(const Location& loc, env::t env, const std::vector<TypeExpr*>& params1, const std::vector<TypeExpr*>& params2,
                   Slice<const ConstructorDeclaration*> cstrs1, Slice<const ConstructorDeclaration*> cstrs2) {
  if (cstrs1.size() != cstrs2.size()) return false;
  for (std::size_t k = 0; k < cstrs1.size(); ++k) {
    const ConstructorDeclaration* cd1 = cstrs1[k];
    const ConstructorDeclaration* cd2 = cstrs2[k];
    if (ident::name(cd1->cd_id) != ident::name(cd2->cd_id)) return false;
    builtin_attributes::check_alerts_inclusion(cd1->cd_loc, cd2->cd_loc, loc, cd1->cd_attributes, cd2->cd_attributes,
                                               ident::name(cd1->cd_id));
    if (compare_constructors(loc, env, params1, params2, cd1->cd_res, cd2->cd_res, cd1->cd_args, cd2->cd_args))
      return false;
  }
  return true;
}

// Variant_diffing.diffing
std::vector<VariantChange> variant_diffing(const Location& loc, env::t env, const std::vector<TypeExpr*>& params1,
                                           const std::vector<TypeExpr*>& params2,
                                           Slice<const ConstructorDeclaration*> l,
                                           Slice<const ConstructorDeclaration*> r) {
  using CD = const ConstructorDeclaration*;
  using KD = diffing::KeyedDiff<CD, CD, ConstructorMismatch, ParamsState>;
  using M = diffing::Mismatch<CD, CD, ConstructorMismatch>;
  KD d;
  d.key_left = [](CD x) { return std::string(ident::name(x->cd_id)); };
  d.key_right = d.key_left;
  d.update = [](const KD::change&, const ParamsState& st) { return st; };
  d.test = [env, loc](const ParamsState& st, const diffing::WithPos<CD>& x, const diffing::WithPos<CD>& y) {
    KD::TR res{};
    const ConstructorDeclaration *cd1 = x.data, *cd2 = y.data;
    std::string name1(ident::name(cd1->cd_id)), name2(ident::name(cd2->cd_id));
    if (name1 != name2) {
      bool types_match =
          !compare_constructors(loc, env, st.first, st.second, cd1->cd_res, cd2->cd_res, cd1->cd_args, cd2->cd_args);
      M m{M::K::Name};
      m.types_match = types_match;
      m.pos = x.pos;
      m.got_name = name1;
      m.expected_name = name2;
      res.ok = false;
      res.err = m;
      return res;
    }
    if (std::optional<ConstructorMismatch> reason =
            compare_constructors(loc, env, st.first, st.second, cd1->cd_res, cd2->cd_res, cd1->cd_args, cd2->cd_args)) {
      M m{M::K::Type};
      m.pos = x.pos;
      m.got = cd1;
      m.expected = cd2;
      m.reason = *reason;
      res.ok = false;
      res.err = m;
      return res;
    }
    res.ok = true;
    return res;
  };
  d.weight = [](const KD::change& c) -> long {
    switch (c.k) {
      case KD::change::K::Insert:
      case KD::change::K::Delete: return 100;
      case KD::change::K::Keep: return 0;
      case KD::change::K::Change: return c.diff.k == M::K::Name ? (c.diff.types_match ? 98 : 99) : 50;
    }
    return 0;
  };
  return d.diff({params1, params2}, std::vector<CD>(l.begin(), l.end()), std::vector<CD>(r.begin(), r.end()));
}

std::optional<TypeMismatch> variant_compare_with_representation(const Location& loc, env::t env, const std::vector<TypeExpr*>& params1,
                                                                const std::vector<TypeExpr*>& params2,
                                                                Slice<const ConstructorDeclaration*> cstrs1,
                                                                Slice<const ConstructorDeclaration*> cstrs2,
                                                                VariantRepresentation rep1,
                                                                VariantRepresentation rep2) {
  if (!variant_equal(loc, env, params1, params2, cstrs1, cstrs2)) {
    TypeMismatch m{TypeMismatch::Kind::Variant_mismatch};
    m.variant_changes = variant_diffing(loc, env, params1, params2, cstrs1, cstrs2);
    return m;
  }
  using VR = VariantRepresentation;
  if (rep1 == rep2) return std::nullopt;
  TypeMismatch m{TypeMismatch::Kind::Unboxed_representation};
  m.pos = rep1 == VR::Variant_unboxed ? Position::First : Position::Second;
  return m;
}

// Inclusion between "private" annotations
std::optional<PrivacyMismatch> privacy_mismatch(env::t env, const TypeDeclaration* decl1,
                                                const TypeDeclaration* decl2) {
  if (!(decl1->type_private == PrivateFlag::Private && decl2->type_private == PrivateFlag::Public))
    return std::nullopt;
  TK k1 = decl1->type_kind->kind, k2 = decl2->type_kind->kind;
  if (k1 == TK::Type_record && k2 == TK::Type_record) return PrivacyMismatch::Private_record_type;
  if (k1 == TK::Type_variant && k2 == TK::Type_variant) return PrivacyMismatch::Private_variant_type;
  if (k1 == TK::Type_open && k2 == TK::Type_open) return PrivacyMismatch::Private_extensible_variant;
  if (k1 == TK::Type_abstract && k2 == TK::Type_abstract && decl2->type_manifest) {
    if (!decl1->type_manifest) return std::nullopt;
    TypeExpr* ty1 = ctype::expand_head_nolink(env, decl1->type_manifest);
    const TypeDesc* d = get_desc(ty1);
    if (auto* v = as<Tvariant>(d); v && is_constr_row(true, row_more(v->row)))
      return PrivacyMismatch::Private_row_type;
    if (auto* o = as<Tobject>(d); o && is_constr_row(true, ctype::flatten_fields(o->fields).second))
      return PrivacyMismatch::Private_row_type;
    return PrivacyMismatch::Private_type_abbreviation;
  }
  return std::nullopt;
}

std::optional<PrivateVariantMismatch> private_variant(env::t env, const RowDesc* row1, std::vector<TypeExpr*> params1,
                                                      const RowDesc* row2, std::vector<TypeExpr*> params2) {
  using PV = PrivateVariantMismatch;
  using RF = RowFieldView::Kind;
  ctype::MergedRowFields m = ctype::merge_row_fields(row_fields(row1), row_fields(row2));
  bool row1_closed = row_closed(row1);
  bool row2_closed = row_closed(row2);
  if (row2_closed && !row1_closed) return PV{PV::Kind::Only_outer_closed};
  if (row2_closed) {
    auto f = ctype::filter_row_fields(false, m.r1);
    if (!f.empty()) {
      PV e{PV::Kind::Missing};
      e.pos = Position::Second;
      e.tag = f[0].label;
      return e;
    }
  }
  for (auto& [s, f] : m.r2)
    if (row_field_repr(f).kind == RF::Rpresent) {
      PV e{PV::Kind::Missing};
      e.pos = Position::First;
      e.tag = s;
      return e;
    }
  auto tl1 = params1, tl2 = params2;
  for (auto& pr : m.pairs) {
    std::string_view s = pr.label;
    RowFieldView f1 = row_field_repr(pr.f1), f2 = row_field_repr(pr.f2);
    auto incompatible = [&] {
      PV e{PV::Kind::Incompatible_types_for};
      e.tag = s;
      return e;
    };
    auto missing = [&](Position p) {
      PV e{PV::Kind::Missing};
      e.pos = p;
      e.tag = s;
      return e;
    };
    if (f1.kind == RF::Rpresent && f2.kind == RF::Rpresent) {
      if (f1.present && f2.present) {
        tl1.insert(tl1.begin(), f1.present);
        tl2.insert(tl2.begin(), f2.present);
      } else if (f1.present || f2.present) {
        return incompatible();
      }
    } else if (f1.kind == RF::Rpresent && f2.kind == RF::Reither) {
      if (f1.present && !f2.constant && f2.arg_types.size() == 1) {
        tl1.insert(tl1.begin(), f1.present);
        tl2.insert(tl2.begin(), f2.arg_types[0]);
      } else if (!f1.present && f2.constant && f2.arg_types.empty()) {
      } else {
        return incompatible();
      }
    } else if (f1.kind == RF::Rpresent && f2.kind == RF::Rabsent) {
      return missing(Position::Second);
    } else if (f1.kind == RF::Reither && f2.kind == RF::Reither) {
      if (f1.constant == f2.constant && f1.arg_types.size() == f2.arg_types.size()) {
        tl1.insert(tl1.begin(), f1.arg_types.begin(), f1.arg_types.end());
        tl2.insert(tl2.begin(), f2.arg_types.begin(), f2.arg_types.end());
      } else {
        return incompatible();
      }
    } else if (f1.kind == RF::Reither && f2.kind == RF::Rpresent) {
      PV e{PV::Kind::Presence};
      e.tag = s;
      return e;
    } else if (f1.kind == RF::Reither && f2.kind == RF::Rabsent) {
      return missing(Position::Second);
    } else if (f1.kind == RF::Rabsent && f2.kind != RF::Rpresent) {
    } else {  // Rabsent, Rpresent
      return missing(Position::First);
    }
  }
  if (auto e = equal_opt(env, true, tl1, tl2)) {
    PV r{PV::Kind::Types};
    r.err = *e;
    return r;
  }
  return std::nullopt;
}

std::optional<PrivateObjectMismatch> private_object(env::t env, const std::vector<ctype::FieldEntry>& fields1,
                                                    const std::vector<TypeExpr*>& params1,
                                                    const std::vector<ctype::FieldEntry>& fields2,
                                                    const std::vector<TypeExpr*>& params2) {
  ctype::AssociatedFields a = ctype::associate_fields(fields1, fields2);
  if (!a.miss2.empty()) {
    PrivateObjectMismatch m{PrivateObjectMismatch::Kind::Missing};
    m.label = a.miss2[0].name;
    return m;
  }
  std::vector<TypeExpr*> tl1, tl2;
  for (auto& p : a.pairs) {
    tl1.push_back(p.t1);
    tl2.push_back(p.t2);
  }
  if (auto e = equal_opt(env, true, cat(params1, tl1), cat(params2, tl2))) {
    PrivateObjectMismatch m{PrivateObjectMismatch::Kind::Types};
    m.err = *e;
    return m;
  }
  return std::nullopt;
}

std::optional<TypeMismatch> type_manifest(env::t env, TypeExpr* ty1, Slice<TypeExpr*> params1, TypeExpr* ty2,
                                          Slice<TypeExpr*> params2, PrivateFlag priv2, const TypeKind* kind2) {
  // let ty1' = .. and ty2' = ..: left to right
  TypeExpr* ty1_ = ctype::expand_head_nolink(env, ty1);
  TypeExpr* ty2_ = ctype::expand_head_nolink(env, ty2);
  const TypeDesc* d1 = get_desc(ty1_);
  const TypeDesc* d2 = get_desc(ty2_);
  auto* v1 = as<Tvariant>(d1);
  auto* v2 = as<Tvariant>(d2);
  if (v1 && v2 && is_absrow(env, row_more(v2->row))) {
    std::vector<TypeExpr*> a = cat({ty1}, vec(params1)), b = cat({row_more(v2->row)}, vec(params2));
    if (!ctype::is_equal(env, true, slice(a), slice(b))) throw std::logic_error("Includecore.type_manifest");
    if (auto e = private_variant(env, v1->row, vec(params1), v2->row, vec(params2))) {
      TypeMismatch m{TypeMismatch::Kind::Private_variant};
      m.ty1 = ty1;
      m.ty2 = ty2;
      m.private_variant = e;
      return m;
    }
    return std::nullopt;
  }
  auto* o1 = as<Tobject>(d1);
  auto* o2 = as<Tobject>(d2);
  if (o1 && o2 && is_absrow(env, ctype::flatten_fields(o2->fields).second)) {
    auto [fields2, rest2] = ctype::flatten_fields(o2->fields);
    auto fields1 = ctype::flatten_fields(o1->fields).first;
    std::vector<TypeExpr*> a = cat({ty1}, vec(params1)), b = cat({rest2}, vec(params2));
    if (!ctype::is_equal(env, true, slice(a), slice(b))) throw std::logic_error("Includecore.type_manifest");
    if (auto e = private_object(env, fields1, vec(params1), fields2, vec(params2))) {
      TypeMismatch m{TypeMismatch::Kind::Private_object};
      m.ty1 = ty1;
      m.ty2 = ty2;
      m.private_object = e;
      return m;
    }
    return std::nullopt;
  }
  bool is_private_abbrev_2 = false;
  if (priv2 == PrivateFlag::Private && kind2->kind == TK::Type_abstract) {
    // Same checks as the [when] guards from above, inverted
    if (auto* v = as<Tvariant>(d2)) is_private_abbrev_2 = !is_absrow(env, row_more(v->row));
    else if (auto* o = as<Tobject>(d2)) is_private_abbrev_2 = !is_absrow(env, ctype::flatten_fields(o->fields).second);
    else is_private_abbrev_2 = true;
  }
  try {
    if (is_private_abbrev_2) {
      ctype::equal_private(env, params1, ty1, params2, ty2);
    } else {
      std::vector<TypeExpr*> a = cat(vec(params1), {ty1}), b = cat(vec(params2), {ty2});
      ctype::equal(env, true, slice(a), slice(b));
    }
  } catch (const ctype::Equality& e) {
    TypeMismatch m{TypeMismatch::Kind::Manifest};
    m.err = e.err;
    return m;
  }
  return std::nullopt;
}

}  // namespace

// A type declaration [td1] is consistent with the type declaration [td2]
// if there is a context E such E |- td1 <: td2 for the ordinary subtyping.
std::optional<TypeMismatch> type_declarations_consistency(env::t env, const TypeDeclaration* decl1,
                                                          const TypeDeclaration* decl2) {
  if (decl1->type_arity != decl2->type_arity) return TypeMismatch{TypeMismatch::Kind::Arity};
  if (auto p = privacy_mismatch(env, decl1, decl2)) {
    TypeMismatch m{TypeMismatch::Kind::Privacy};
    m.privacy = *p;
    return m;
  }
  return std::nullopt;
}

std::optional<TypeMismatch> type_declarations(bool equality, const Location& loc, env::t env, bool mark,
                                              std::string_view name, const TypeDeclaration* decl1, Path::t path,
                                              const TypeDeclaration* decl2) {
  builtin_attributes::check_alerts_inclusion(decl1->type_loc, decl2->type_loc, loc, decl1->type_attributes,
                                             decl2->type_attributes, name);
  if (auto err = type_declarations_consistency(env, decl1, decl2)) return err;
  std::optional<TypeMismatch> err;
  auto constraint_err = [](const et::EqualityError& e) {
    TypeMismatch m{TypeMismatch::Kind::Constraint};
    m.err = e;
    return m;
  };
  if (!decl2->type_manifest) {
    if (auto e = equal_opt(env, true, vec(decl1->type_params), vec(decl2->type_params))) err = constraint_err(*e);
  } else if (decl1->type_manifest) {
    err = type_manifest(env, decl1->type_manifest, decl1->type_params, decl2->type_manifest, decl2->type_params,
                        decl2->type_private, decl2->type_kind);
  } else {
    TypeExpr* ty1 = newgenty(tconstr(path, decl2->type_params, make<MemoRef>(mnil())));
    if (auto e = equal_opt(env, true, vec(decl1->type_params), vec(decl2->type_params))) {
      err = constraint_err(*e);
    } else if (auto e2 = equal_opt(env, false, {ty1}, {decl2->type_manifest})) {
      TypeMismatch m{TypeMismatch::Kind::Manifest};
      m.err = *e2;
      err = m;
    }
  }
  if (err) return err;
  TK k1 = decl1->type_kind->kind, k2 = decl2->type_kind->kind;
  if (k2 == TK::Type_abstract) {
  } else if (k1 == TK::Type_variant && k2 == TK::Type_variant) {
    if (mark) {
      auto mark_cstrs = [](env::ConstructorUsage usage, Slice<const ConstructorDeclaration*> cstrs) {
        for (const ConstructorDeclaration* c : cstrs) env::mark_constructor_used(usage, c->cd_uid);
      };
      env::ConstructorUsage usage = decl2->type_private == PrivateFlag::Public ? env::ConstructorUsage::Exported
                                                                              : env::ConstructorUsage::Exported_private;
      mark_cstrs(usage, decl1->type_kind->constructors);
      if (equality) mark_cstrs(env::ConstructorUsage::Exported, decl2->type_kind->constructors);
    }
    err = variant_compare_with_representation(loc, env, vec(decl1->type_params), vec(decl2->type_params),
                                              decl1->type_kind->constructors, decl2->type_kind->constructors,
                                              decl1->type_kind->variant_repr, decl2->type_kind->variant_repr);
  } else if (k1 == TK::Type_record && k2 == TK::Type_record) {
    if (mark) {
      auto mark_lbls = [](env::LabelUsage usage, Slice<const LabelDeclaration*> lbls) {
        for (const LabelDeclaration* l : lbls) env::mark_label_used(usage, l->ld_uid);
      };
      env::LabelUsage usage =
          decl2->type_private == PrivateFlag::Public ? env::LabelUsage::Exported : env::LabelUsage::Exported_private;
      mark_lbls(usage, decl1->type_kind->labels);
      if (equality) mark_lbls(env::LabelUsage::Exported, decl2->type_kind->labels);
    }
    err = record_compare_with_representation(loc, env, vec(decl1->type_params), vec(decl2->type_params),
                                             decl1->type_kind->labels, decl2->type_kind->labels,
                                             decl1->type_kind->record_repr, decl2->type_kind->record_repr);
  } else if (k1 == TK::Type_open && k2 == TK::Type_open) {
  } else if (k1 == TK::Type_external && k2 == TK::Type_external &&
             decl1->type_kind->external == decl2->type_kind->external) {
  } else {
    TypeMismatch m{TypeMismatch::Kind::Kind_};
    m.k1 = of_kind(decl1->type_kind);
    m.k2 = of_kind(decl2->type_kind);
    err = m;
  }
  if (err) return err;
  bool abstr = type_kind_is_abstract(decl2) && !decl2->type_manifest;
  // If attempt to assign a non-immediate type (e.g. string) to a type that
  // must be immediate, then we error
  if (abstr) {
    TypeImmediacy t = decl1->type_immediate, as_ = decl2->type_immediate;
    using TI = TypeImmediacy;
    bool ok = as_ == TI::Unknown || (t == TI::Always && as_ == TI::Always) ||
              ((t == TI::Always || t == TI::Always_on_64bits) && as_ == TI::Always_on_64bits);
    if (!ok) {
      TypeMismatch m{TypeMismatch::Kind::Immediate};
      m.immediate_violation_always = as_ == TI::Always;
      return m;
    }
  }
  // We need to check coherence of internal and exported variance
  bool abstr_ = abstr || decl2->type_private == PrivateFlag::Private;
  bool need_variance = abstr_ || decl1->type_private == PrivateFlag::Private || k1 == TK::Type_open;
  if (!need_variance) return std::nullopt;
  bool opn = k2 == TK::Type_open && !decl2->type_manifest;
  auto& v1s = decl1->type_variance;
  auto& v2s = decl2->type_variance;
  if (v1s.size() != v2s.size()) throw std::invalid_argument("List.combine");
  if (decl2->type_params.size() != v1s.size()) throw std::invalid_argument("List.for_all2");
  using variance::F;
  for (std::size_t i = 0; i < v1s.size(); ++i) {
    TypeExpr* ty = decl2->type_params[i];
    variance::t v1 = v1s[i], v2 = v2s[i];
    auto imp = [](bool a, bool b) { return !a || b; };
    bool co1 = variance::mem(F::May_pos, v1), cn1 = variance::mem(F::May_neg, v1);
    bool co2 = variance::mem(F::May_pos, v2), cn2 = variance::mem(F::May_neg, v2);
    bool upper;
    if (abstr_) upper = imp(co1, co2) && imp(cn1, cn2);
    else if (opn || !is_Tvar(ty)) upper = co1 == co2 && cn1 == cn2;
    else upper = true;
    bool p1 = variance::mem(F::Pos, v1), n1 = variance::mem(F::Neg, v1), j1 = variance::mem(F::Inj, v1);
    bool p2 = variance::mem(F::Pos, v2), n2 = variance::mem(F::Neg, v2), j2 = variance::mem(F::Inj, v2);
    // Only check the lower bound for abstract types.
    bool lower = imp(abstr, imp(p2, p1) && imp(n2, n1) && imp(j2, j1));
    if (!(upper && lower)) return TypeMismatch{TypeMismatch::Kind::Variance};
  }
  return std::nullopt;
}

// Inclusion between extension constructors
std::optional<ExtensionConstructorMismatch> extension_constructors(const Location& loc, env::t env, bool mark, Ident::t id,
                                                                   const ExtensionConstructor* ext1,
                                                                   const ExtensionConstructor* ext2) {
  if (mark) {
    env::ConstructorUsage usage =
        ext2->ext_private == PrivateFlag::Public ? env::ConstructorUsage::Exported : env::ConstructorUsage::Exported_private;
    env::mark_extension_used(usage, ext1->ext_uid);
  }
  TypeExpr* ty1 = newgenty(tconstr(ext1->ext_type_path, ext1->ext_type_params, make<MemoRef>(mnil())));
  TypeExpr* ty2 = newgenty(tconstr(ext2->ext_type_path, ext2->ext_type_params, make<MemoRef>(mnil())));
  auto tl1 = cat({ty1}, vec(ext1->ext_type_params));
  auto tl2 = cat({ty2}, vec(ext2->ext_type_params));
  auto mismatch = [&](const ConstructorMismatch& c) {
    ExtensionConstructorMismatch m{ExtensionConstructorMismatch::Kind::Constructor_mismatch};
    m.id = id;
    m.ext1 = ext1;
    m.ext2 = ext2;
    m.mismatch = c;
    return m;
  };
  if (auto e = equal_opt(env, true, tl1, tl2)) {
    ConstructorMismatch c{ConstructorMismatch::Kind::Type};
    c.err = *e;
    return mismatch(c);
  }
  if (auto r = compare_constructors(loc, env, vec(ext1->ext_type_params), vec(ext2->ext_type_params), ext1->ext_ret_type,
                                    ext2->ext_ret_type, ext1->ext_args, ext2->ext_args))
    return mismatch(*r);
  if (ext1->ext_private == PrivateFlag::Private && ext2->ext_private == PrivateFlag::Public)
    return ExtensionConstructorMismatch{ExtensionConstructorMismatch::Kind::Constructor_privacy};
  return std::nullopt;
}

bool class_types(env::t env, const ClassType* cty1, const ClassType* cty2) {
  return ctype::match_class_types(env, cty1, cty2).empty();
}

}  // namespace cppcaml::typing::includecore
