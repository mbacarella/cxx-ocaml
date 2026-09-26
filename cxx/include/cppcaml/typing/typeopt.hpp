// The part of typing/typeopt.ml the typer needs (Value_rec_check): type
// classification for array kinds and for the compilation of `lazy e`.  The
// rest of Typeopt (value kinds, bigarray kinds) belongs to the Lambda
// translation.
#pragma once

#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::typeopt {

enum class ImmediateOrPointer { Immediate, Pointer };        // Lambda.immediate_or_pointer
enum class ArrayKind { Pgenarray, Paddrarray, Pintarray, Pfloatarray };  // Lambda.array_kind
enum class Classification { Int, Float, Lazy, Addr, Any };

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
