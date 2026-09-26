// Port of the typer-facing part of typing/typeopt.ml (and of
// typing/typedecl_unboxed.ml, which it uses).
#include "cppcaml/typing/typeopt.hpp"

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/predef.hpp"

namespace cppcaml::typing::typeopt {

using namespace types;
using namespace btype;
namespace tt = typedtree;
using XK = tt::ExpressionDesc::Kind;

// Config.flat_float_array (ocamlc's default configuration)
static constexpr bool flat_float_array = true;

// Typedecl_unboxed.get_unboxed_type_representation.  We use the
// Ctype.expand_head_opt version of expand_head to get access to the
// manifest type of private abbreviations.
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
static TypeExpr* get_unboxed_type_representation(env::t env, TypeExpr* ty) {
  // Do not give too much fuel: PR#7424
  return get_unboxed_type_representation(env, ty, 100);
}

TypeExpr* scrape_ty(env::t env, TypeExpr* ty) {
  ty = ctype::maybe_instance_poly(ty);
  if (get_desc(ty)->kind != DescKind::Tconstr) return ty;
  ty = ctype::expand_head_opt(env, ty);
  auto* tc = as<Tconstr>(get_desc(ty));
  if (!tc) return ty;
  const TypeDeclaration* decl;
  try {
    decl = env::find_type(tc->path, env);
  } catch (const env::NotFound&) {
    return nullptr;
  }
  const TypeKind* k = decl->type_kind;
  if ((k->kind == TypeKind::Kind::Type_variant && k->variant_repr == VariantRepresentation::Variant_unboxed) ||
      (k->kind == TypeKind::Kind::Type_record && k->record_repr.kind == RecordRepresentation::Kind::Record_unboxed)) {
    TypeExpr* r = get_unboxed_type_representation(env, ty);
    return r ? ctype::maybe_instance_poly(r) : nullptr;
  }
  return ty;
}

static bool is_immediate(TypeImmediacy i) {
  switch (i) {
    case TypeImmediacy::Unknown: return false;
    case TypeImmediacy::Always: return true;
    case TypeImmediacy::Always_on_64bits:
      // In bytecode, we don't know at compile time whether we are
      // targeting 32 or 64 bits.
      return clflags::native_code;  // && Sys.word_size = 64
  }
  return false;
}

ImmediateOrPointer maybe_pointer_type(env::t env, TypeExpr* ty) {
  TypeExpr* t = scrape_ty(env, ty);
  if (!t) return ImmediateOrPointer::Pointer;
  return is_immediate(ctype::immediacy(env, t)) ? ImmediateOrPointer::Immediate : ImmediateOrPointer::Pointer;
}

ImmediateOrPointer maybe_pointer(const tt::Expression* exp) { return maybe_pointer_type(exp->exp_env, exp->exp_type); }

Classification classify(env::t env, TypeExpr* ty0) {
  TypeExpr* ty = scrape_ty(env, ty0);
  if (!ty) return Classification::Any;
  if (maybe_pointer_type(env, ty) == ImmediateOrPointer::Immediate) return Classification::Int;
  const TypeDesc* d = get_desc(ty);
  switch (d->kind) {
    case DescKind::Tvar:
    case DescKind::Tunivar: return Classification::Any;
    case DescKind::Tconstr: {
      Path::t p = as<Tconstr>(d)->path;
      using TC = predef::TypeConstr;
      std::optional<TC> c = predef::find_type_constr(p);
      if (c) {
        switch (*c) {
          case TC::Float: return Classification::Float;
          case TC::Lazy_t: return Classification::Lazy;
          case TC::Int:
          case TC::Char: return Classification::Int;
          case TC::String:
          case TC::Bytes:
          case TC::Int32:
          case TC::Int64:
          case TC::Nativeint:
          case TC::Extension_constructor:
          case TC::Continuation:
          case TC::Array:
          case TC::Floatarray:
          case TC::Iarray:
          case TC::Atomic_loc:
          case TC::Todo_info: return Classification::Addr;
          default: break;  // data_type_constr
        }
      }
      try {
        switch (env::find_type(p, env)->type_kind->kind) {
          case TypeKind::Kind::Type_abstract:
          case TypeKind::Kind::Type_external: return Classification::Any;
          default: return Classification::Addr;
        }
      } catch (const env::NotFound&) {
        // This can happen due to e.g. missing -I options, causing some
        // .cmi files to be unavailable.
        return Classification::Any;
      }
    }
    case DescKind::Tarrow:
    case DescKind::Ttuple:
    case DescKind::Tpackage:
    case DescKind::Tobject:
    case DescKind::Tnil:
    case DescKind::Tvariant:
    case DescKind::Tfunctor: return Classification::Addr;
    default: throw std::logic_error("Typeopt.classify");
  }
}

ArrayKind array_type_kind(env::t env, TypeExpr* ty) {
  TypeExpr* t = scrape_ty(env, ty);
  auto* tc = t ? as<Tconstr>(get_desc(t)) : nullptr;
  if (tc && tc->args.size() == 1 &&
      (path::same(tc->path, predef::paths().array) || path::same(tc->path, predef::paths().iarray))) {
    switch (classify(env, tc->args[0])) {
      case Classification::Any: return flat_float_array ? ArrayKind::Pgenarray : ArrayKind::Paddrarray;
      case Classification::Float: return flat_float_array ? ArrayKind::Pfloatarray : ArrayKind::Paddrarray;
      case Classification::Addr:
      case Classification::Lazy: return ArrayKind::Paddrarray;
      case Classification::Int: return ArrayKind::Pintarray;
    }
  }
  if (tc && tc->args.empty() && path::same(tc->path, predef::paths().floatarray)) return ArrayKind::Pfloatarray;
  // This can happen with e.g. Obj.field
  return ArrayKind::Pgenarray;
}

ArrayKind array_kind(const tt::Expression* exp) { return array_type_kind(exp->exp_env, exp->exp_type); }
ArrayKind array_pattern_kind(const tt::Pattern* pat) { return array_type_kind(pat->pat_env, pat->pat_type); }

// The compilation of the expression [lazy e] depends on the form of e: in
// some cases we optimize it into [let x = e in lazy x], evaluating [e] right
// now (if it is equivalent) and avoiding creating a thunk.  This
// optimization must be taken into account when determining whether a
// recursive binding is safe.
LazySummary classify_lazy_argument(const tt::Expression* e) {
  // We can compile [lazy e] into [let x = e in lazy x] whenever [e] is
  // "commutative" (no coeffects, only generative effects), with a cutoff on
  // expression size.
  const int size_cutoff = 42;
  int size = 0;
  std::function<bool(const tt::Expression*)> small_and_commutative = [&](const tt::Expression* e2) -> bool {
    ++size;
    if (size > size_cutoff) return false;
    const tt::ExpressionDesc* d = e2->exp_desc;
    switch (d->kind) {
      case XK::Texp_ident:
      case XK::Texp_constant:
      case XK::Texp_function:
      case XK::Texp_lazy: return true;
      case XK::Texp_variant: {
        auto* v = as<tt::Texp_variant>(d);
        return !v->arg || small_and_commutative(v->arg);
      }
      case XK::Texp_construct:
        for (auto* a : as<tt::Texp_construct>(d)->args)
          if (!small_and_commutative(a)) return false;
        return true;
      case XK::Texp_array:
        for (auto* a : as<tt::Texp_array>(d)->el)
          if (!small_and_commutative(a)) return false;
        return true;
      case XK::Texp_tuple:
        for (auto& a : as<tt::Texp_tuple>(d)->el)
          if (!small_and_commutative(a.exp)) return false;
        return true;
      case XK::Texp_record: {
        auto* r = as<tt::Texp_record>(d);
        for (auto& f : r->fields) {
          if (!f.def.kept) {
            if (!small_and_commutative(f.def.exp)) return false;
          } else {
            ++size;
          }
        }
        return !r->extended_expression || small_and_commutative(r->extended_expression);
      }
      case XK::Texp_extension_constructor: return true;
      // under-approximations (Texp_let, Texp_pack, Texp_field,
      // Texp_atomic_loc, Texp_object, Texp_struct_item) and the
      // (typically) not commutative expressions
      default: return false;
    }
  };
  // In [let x = e in lazy x], [lazy x] sometimes need to be a [Forward]
  // block, but this block can typically be shortcut into just [x]. The
  // forward block is required for expressions that may have a lazy type
  // themselves, or float when flat-float-array is enabled.
  if (!small_and_commutative(e)) return {LazySummary::Kind::Lazy_thunk};
  using FR = LazySummary::ForwardRepr;
  switch (classify(e->exp_env, e->exp_type)) {
    case Classification::Addr:
    case Classification::Int: return {LazySummary::Kind::Eager, FR::Shortcut};
    case Classification::Any:
    case Classification::Lazy: return {LazySummary::Kind::Eager, FR::Forward};
    case Classification::Float: return {LazySummary::Kind::Eager, flat_float_array ? FR::Forward : FR::Shortcut};
  }
  throw std::logic_error("Typeopt.classify_lazy_argument");
}

}  // namespace cppcaml::typing::typeopt
