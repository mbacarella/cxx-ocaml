// Port of typing/typedecl_unboxed.ml, typing/typedecl_immediacy.ml and
// typing/type_immediacy.ml.
#include "cppcaml/typing/typedecl_unboxed.hpp"

#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "typedecl_properties.hpp"

namespace cppcaml::typing {

using namespace types;

namespace typedecl_unboxed {
// We use the Ctype.expand_head_opt version of expand_head to get access to
// the manifest type of private abbreviations.
static TypeExpr* get_unboxed_type_representation(env::t env, TypeExpr* ty, int fuel) {
  if (fuel < 0) return nullptr;
  ty = ctype::expand_head_opt(env, ty);
  auto* tc = as<Tconstr>(get_desc(ty));
  if (!tc) return ty;
  const TypeDeclaration* decl;
  try {
    decl = env::find_type(tc->path, env);
  } catch (const env::NotFound&) {
    return ty;
  }
  const TypeKind* k = decl->type_kind;
  TypeExpr* ty2 = nullptr;
  if (k->kind == TypeKind::Kind::Type_record && k->labels.size() == 1 &&
      k->record_repr.kind == RecordRepresentation::Kind::Record_unboxed) {
    ty2 = k->labels[0]->ld_type;
  } else if (k->kind == TypeKind::Kind::Type_variant && k->constructors.size() == 1 &&
             k->variant_repr == VariantRepresentation::Variant_unboxed) {
    const ConstructorArguments& a = k->constructors[0]->cd_args;
    if (a.kind == ConstructorArguments::Kind::Cstr_tuple && a.tuple.size() == 1) ty2 = a.tuple[0];
    else if (a.kind == ConstructorArguments::Kind::Cstr_record && a.record.size() == 1) ty2 = a.record[0]->ld_type;
  }
  if (!ty2) return ty;
  ty2 = ctype::maybe_instance_poly(ty2);
  return get_unboxed_type_representation(env, ctype::apply(env, decl->type_params, ty2, tc->args), fuel - 1);
}
TypeExpr* get_unboxed_type_representation(env::t env, TypeExpr* ty) {
  // Do not give too much fuel: PR#7424
  return get_unboxed_type_representation(env, ty, 100);
}
}  // namespace typedecl_unboxed

namespace type_immediacy {
std::optional<Violation> coerce(TypeImmediacy t, TypeImmediacy as_) {
  using TI = TypeImmediacy;
  if (as_ == TI::Unknown) return std::nullopt;
  if (as_ == TI::Always) {
    if (t == TI::Always) return std::nullopt;
    return Violation::Not_always_immediate;
  }
  // as_ = Always_on_64bits
  if (t == TI::Unknown) return Violation::Not_always_immediate_on_64bits;
  return std::nullopt;
}
TypeImmediacy of_attributes(const Attributes& attrs) {
  if (builtin_attributes::has_attribute("immediate", attrs)) return TypeImmediacy::Always;
  if (builtin_attributes::has_attribute("immediate64", attrs)) return TypeImmediacy::Always_on_64bits;
  return TypeImmediacy::Unknown;
}
}  // namespace type_immediacy

namespace typedecl_immediacy {
TypeImmediacy compute_decl(env::t env, const TypeDeclaration* tdecl) {
  const TypeKind* k = tdecl->type_kind;
  TypeExpr* arg = nullptr;
  if (k->kind == TypeKind::Kind::Type_variant && k->constructors.size() == 1 &&
      k->variant_repr == VariantRepresentation::Variant_unboxed) {
    const ConstructorArguments& a = k->constructors[0]->cd_args;
    if (a.kind == ConstructorArguments::Kind::Cstr_tuple && a.tuple.size() == 1) arg = a.tuple[0];
    else if (a.kind == ConstructorArguments::Kind::Cstr_record && a.record.size() == 1) arg = a.record[0]->ld_type;
  } else if (k->kind == TypeKind::Kind::Type_record && k->labels.size() == 1 &&
             k->record_repr.kind == RecordRepresentation::Kind::Record_unboxed) {
    arg = k->labels[0]->ld_type;
  }
  if (arg) {
    TypeExpr* argrepr = typedecl_unboxed::get_unboxed_type_representation(env, arg);
    if (!argrepr) return TypeImmediacy::Unknown;
    return ctype::immediacy(env, argrepr);
  }
  if (k->kind == TypeKind::Kind::Type_variant) {
    for (auto* c : k->constructors)
      if (!(c->cd_args.kind == ConstructorArguments::Kind::Cstr_tuple && c->cd_args.tuple.empty()))
        return TypeImmediacy::Unknown;
    return TypeImmediacy::Always;
  }
  if (k->kind == TypeKind::Kind::Type_abstract) {
    if (tdecl->type_manifest) return ctype::immediacy(env, tdecl->type_manifest);
    return type_immediacy::of_attributes(tdecl->type_attributes);
  }
  return TypeImmediacy::Unknown;
}

std::vector<std::pair<Ident::t, const TypeDeclaration*>> update_decls(
    env::t env, const std::vector<std::pair<Ident::t, const TypeDeclaration*>>& decls) {
  using namespace typedecl_properties;
  Property<TypeImmediacy, Unit> property;
  property.eq = [](const TypeImmediacy& a, const TypeImmediacy& b) { return a == b; };
  property.merge = [](const TypeImmediacy&, const TypeImmediacy& new_prop) { return new_prop; };
  property.default_ = [](Decl) { return TypeImmediacy::Unknown; };
  property.compute = [](env::t e, Decl decl, const Unit&) { return compute_decl(e, decl); };
  property.update_decl = [](Decl decl, const TypeImmediacy& immediacy) {
    TypeDeclaration* d = make<TypeDeclaration>(*decl);
    d->type_immediate = immediacy;
    return static_cast<Decl>(d);
  };
  property.check = [](env::t, Ident::t, Decl decl, const Unit&) {
    TypeImmediacy written_by_user = type_immediacy::of_attributes(decl->type_attributes);
    if (auto v = type_immediacy::coerce(decl->type_immediate, written_by_user)) throw Error(decl->type_loc, *v);
  };
  return compute_property_noreq(property, env, decls);
}
}  // namespace typedecl_immediacy

}  // namespace cppcaml::typing
