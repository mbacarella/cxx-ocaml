// Port of the typer-facing part of typing/typeopt.ml (and of
// typing/typedecl_unboxed.ml, which it uses).
#include "cppcaml/typing/typeopt.hpp"
#include "cppcaml/typing/typedecl_unboxed.hpp"

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/predef.hpp"

#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace cppcaml::typing::typeopt {

using namespace types;
using namespace btype;
namespace tt = typedtree;
using XK = tt::ExpressionDesc::Kind;

// Config.flat_float_array (ocamlc's default configuration)
static constexpr bool flat_float_array = true;

using typedecl_unboxed::get_unboxed_type_representation;

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

// scrape env ty = Option.map get_desc (scrape_ty env ty)
static const TypeDesc* scrape(env::t env, TypeExpr* ty) {
  TypeExpr* t = scrape_ty(env, ty);
  return t ? get_desc(t) : nullptr;
}

bool is_function_type(env::t env, TypeExpr* ty, TypeExpr** arg, TypeExpr** res) {
  const TypeDesc* d = scrape(env, ty);
  auto* a = d ? as<Tarrow>(d) : nullptr;
  if (!a) return false;
  if (arg) *arg = a->t1;
  if (res) *res = a->t2;
  return true;
}

bool is_base_type(env::t env, TypeExpr* ty, Path::t base_ty_path) {
  const TypeDesc* d = scrape(env, ty);
  auto* c = d ? as<Tconstr>(d) : nullptr;
  return c && path::same(c->path, base_ty_path);
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
          case TC::Atomic_loc: return Classification::Addr;
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

// Whether a forward block is needed for a lazy thunk on a value, i.e. if the
// value can be represented as a float/forward/lazy
static bool lazy_val_requires_forward(env::t env, TypeExpr* ty) {
  switch (classify(env, ty)) {
    case Classification::Any:
    case Classification::Lazy: return true;
    case Classification::Float: return flat_float_array;
    case Classification::Addr:
    case Classification::Int: return false;
  }
  throw std::logic_error("Typeopt.lazy_val_requires_forward");
}

// The compilation of the expression [lazy e] depends on the form of e:
// constants, floats and identifiers are optimized.  The optimization must be
// taken into account when determining whether a recursive binding is safe.
LazyArgument classify_lazy_argument(const tt::Expression* e) {
  const tt::ExpressionDesc* d = e->exp_desc;
  switch (d->kind) {
    case XK::Texp_constant:
      if (as<tt::Texp_constant>(d)->c.kind == tt::Constant::Kind::Const_float)
        return flat_float_array ? LazyArgument::Float_that_cannot_be_shortcut : LazyArgument::Constant_or_function;
      return LazyArgument::Constant_or_function;
    case XK::Texp_function: return LazyArgument::Constant_or_function;
    case XK::Texp_construct:
      if (as<tt::Texp_construct>(d)->cstr->cstr_arity == 0) return LazyArgument::Constant_or_function;
      return LazyArgument::Other;
    case XK::Texp_ident:
      if (lazy_val_requires_forward(e->exp_env, e->exp_type)) return LazyArgument::Identifier_forward_value;
      return LazyArgument::Identifier_other;
    default: return LazyArgument::Other;
  }
}

// ---- the rest of typeopt.ml (stage 10) ----------------------------------------
using lambda::BigarrayKind;
using lambda::BigarrayLayout;

template <class T>
static T bigarray_decode_type(env::t env, TypeExpr* ty, const std::vector<std::pair<std::string_view, T>>& tbl, T dfl) {
  const TypeDesc* d = scrape(env, ty);
  auto* c = d ? as<Tconstr>(d) : nullptr;
  if (c && c->args.empty() && c->path->kind == Path::Kind::Pdot && c->path->p1->kind == Path::Kind::Pident &&
      ident::name(c->path->p1->id) == "Stdlib__Bigarray") {
    for (auto& [name, v] : tbl)
      if (name == c->path->s) return v;
    return dfl;
  }
  return dfl;
}

BigarrayKindLayout bigarray_type_kind_and_layout(env::t env, TypeExpr* typ) {
  static const std::vector<std::pair<std::string_view, BigarrayKind>> kind_table = {
      {"float16_elt", BigarrayKind::Pbigarray_float16},
      {"float32_elt", BigarrayKind::Pbigarray_float32},
      {"float64_elt", BigarrayKind::Pbigarray_float64},
      {"int8_signed_elt", BigarrayKind::Pbigarray_sint8},
      {"int8_unsigned_elt", BigarrayKind::Pbigarray_uint8},
      {"int16_signed_elt", BigarrayKind::Pbigarray_sint16},
      {"int16_unsigned_elt", BigarrayKind::Pbigarray_uint16},
      {"int32_elt", BigarrayKind::Pbigarray_int32},
      {"int64_elt", BigarrayKind::Pbigarray_int64},
      {"int_elt", BigarrayKind::Pbigarray_caml_int},
      {"nativeint_elt", BigarrayKind::Pbigarray_native_int},
      {"complex32_elt", BigarrayKind::Pbigarray_complex32},
      {"complex64_elt", BigarrayKind::Pbigarray_complex64}};
  static const std::vector<std::pair<std::string_view, BigarrayLayout>> layout_table = {
      {"c_layout", BigarrayLayout::Pbigarray_c_layout}, {"fortran_layout", BigarrayLayout::Pbigarray_fortran_layout}};
  const TypeDesc* d = scrape(env, typ);
  auto* c = d ? as<Tconstr>(d) : nullptr;
  if (c && c->args.size() == 3) {
    // (kind, layout): a tuple, evaluated right to left
    BigarrayLayout l = bigarray_decode_type(env, c->args[2], layout_table, BigarrayLayout::Pbigarray_unknown_layout);
    BigarrayKind k = bigarray_decode_type(env, c->args[1], kind_table, BigarrayKind::Pbigarray_unknown);
    return {k, l};
  }
  return {BigarrayKind::Pbigarray_unknown, BigarrayLayout::Pbigarray_unknown_layout};
}

lambda::ValueKind value_kind(env::t env, TypeExpr* ty0) {
  using VK = lambda::ValueKind;
  TypeExpr* ty = scrape_ty(env, ty0);
  if (!ty) return VK::gen();
  if (is_immediate(ctype::immediacy(env, ty))) return VK::intval();
  auto* c = as<Tconstr>(get_desc(ty));
  if (!c) return VK::gen();
  const predef::Paths& p = predef::paths();
  if (path::same(c->path, p.float_)) return VK::floatval();
  if (path::same(c->path, p.int32)) return VK::boxedint(BoxedInteger::Pint32);
  if (path::same(c->path, p.int64)) return VK::boxedint(BoxedInteger::Pint64);
  if (path::same(c->path, p.nativeint)) return VK::boxedint(BoxedInteger::Pnativeint);
  return VK::gen();
}

lambda::ValueKind value_kind_union(const lambda::ValueKind& a, const lambda::ValueKind& b) {
  return lambda::equal_value_kind(a, b) ? a : lambda::ValueKind::gen();
}

}  // namespace cppcaml::typing::typeopt
