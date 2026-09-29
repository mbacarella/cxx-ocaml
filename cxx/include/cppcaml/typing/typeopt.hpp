// Port of typing/typeopt.ml: type-based optimisation info for the Lambda
// translation (value kinds, array / bigarray kinds, pointer-ness, lazy
// arguments).  The typer's Value_rec_check uses a part of it too.
#pragma once

#include "cppcaml/typing/lambda.hpp"
#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::typeopt {

using ImmediateOrPointer = lambda::ImmediateOrPointer;
using ArrayKind = lambda::ArrayKind;
enum class Classification { Int, Float, Lazy, Addr, Any };

// (stage 10: the rest of typeopt.mli)
bool is_function_type(env::t env, TypeExpr* ty, TypeExpr** arg, TypeExpr** res);  // Some (lhs, rhs)
bool is_base_type(env::t env, TypeExpr* ty, Path::t base_ty_path);
struct BigarrayKindLayout {
  lambda::BigarrayKind kind;
  lambda::BigarrayLayout layout;
};
BigarrayKindLayout bigarray_type_kind_and_layout(env::t env, TypeExpr* ty);
lambda::ValueKind value_kind(env::t env, TypeExpr* ty);
// [local_equations]: nullptr = None
lambda::ValueKind pattern_kind(const env::LocalEquations* local_equations, const typedtree::Pattern* pat);
lambda::ValueKind value_kind_union(const lambda::ValueKind& a, const lambda::ValueKind& b);

TypeExpr* scrape_ty(env::t env, TypeExpr* ty);  // nullptr = None
ImmediateOrPointer maybe_pointer_type(env::t env, TypeExpr* ty);
ImmediateOrPointer maybe_pointer(const typedtree::Expression* exp);
Classification classify(env::t env, TypeExpr* ty);
ArrayKind array_type_kind(env::t env, TypeExpr* ty);
ArrayKind array_kind(const typedtree::Expression* exp);
ArrayKind array_pattern_kind(const typedtree::Pattern* pat);

// lazy_summary = Lazy_thunk | Eager of forward_repr (Forward | Shortcut)
struct LazySummary {
  enum class Kind { Lazy_thunk, Eager };
  enum class ForwardRepr { Forward, Shortcut };
  Kind kind;
  ForwardRepr repr = ForwardRepr::Forward;  // Eager
};
LazySummary classify_lazy_argument(const typedtree::Expression* e);

}  // namespace cppcaml::typing::typeopt
