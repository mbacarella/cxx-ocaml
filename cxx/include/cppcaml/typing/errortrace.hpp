// Port of typing/errortrace.ml (TYPECHECKER.md): the traces carried by
// unification / equality / moregen / subtyping failures.  The OCaml GADT
// indices (unification vs comparison) become a runtime `variety`; the
// Format_doc documents in first-class-module errors are rendered strings.
#pragma once

#include <string>
#include <vector>

#include "cppcaml/typing/types.hpp"

namespace cppcaml::typing::errortrace {

enum class Position : std::uint8_t { First, Second };
enum class Order : std::uint8_t { Less, Equal, More };
inline Position swap_position(Position p) {
  return p == Position::First ? Position::Second : Position::First;
}
inline Order swap_order(Order o) {
  return o == Order::Less ? Order::More : o == Order::More ? Order::Less : Order::Equal;
}

struct ExpandedType {
  TypeExpr* ty;
  TypeExpr* expanded;
};
inline ExpandedType trivial_expansion(TypeExpr* ty) { return {ty, ty}; }

template <class A>
struct Diff {
  A got;
  A expected;
};
template <class A>
Diff<A> swap_diff(const Diff<A>& d) {
  return {d.expected, d.got};
}

// 'a escape_kind / 'a escape
template <class A>
struct Escape {
  enum class Kind : std::uint8_t {
    Constructor, Univ, Self, Module_type, Module, Equation, Constraint
  };
  Kind kind;
  Path::t path = nullptr;    // Constructor / Module_type
  TypeExpr* univ = nullptr;  // Univ
  Ident::t module = nullptr; // Module
  A equation{};              // Equation
  TypeExpr* context = nullptr;  // option
};

enum class FixedRowCaseKind : std::uint8_t { Cannot_be_closed, Cannot_add_tags };
struct FixedRowCase {
  FixedRowCaseKind kind;
  std::vector<std::string_view> tags;
};

// 'variety variant
struct Variant {
  enum class Kind : std::uint8_t {
    Incompatible_types_for, No_tags, No_intersection, Fixed_row,
    Presence_not_guaranteed_for, Openness
  };
  Kind kind;
  std::string_view name;                     // Incompatible_types_for / Presence_not_guaranteed_for
  Position pos = Position::First;            // No_tags / Fixed_row / Presence / Openness
  std::vector<RowFieldEntry> tags;           // No_tags
  FixedRowCase fixed_case{};                 // Fixed_row
  const FixedExplanation* fixed = nullptr;   // Fixed_row
};

// 'variety obj
struct Obj {
  enum class Kind : std::uint8_t { Missing_field, Abstract_row, Self_cannot_be_closed, Kind_differ };
  Kind kind;
  Position pos = Position::First;
  std::string_view name;
  types::FieldKindView k1 = types::FieldKindView::Fpublic, k2 = types::FieldKindView::Fpublic;
};

struct FirstClassModule {
  enum class Kind : std::uint8_t {
    Package_cannot_scrape, Package_inclusion, Package_coercion, Constraint_on_missing_type,
    Constraint_with_deps, Constraint_on_mismatched_type
  };
  Kind kind;
  Path::t path = nullptr;
  std::string doc;  // Package_inclusion / Package_coercion
  Position pos = Position::First;
  std::vector<std::string_view> lhs;
  const TypeDeclaration* decl = nullptr;
};

struct Univar {
  bool is_var_mismatch;
  Order order = Order::Equal;
  Diff<TypeExpr*> diff{};
  std::vector<TypeExpr*> quantification;  // Quantification_mismatch
};

// ('a, 'variety) elt
template <class A>
struct Elt {
  enum class Kind : std::uint8_t {
    Diff, Variant, Obj, Escape, Function_label_mismatch, Tuple_label_mismatch,
    Incompatible_fields, First_class_module, Univar, Rec_occur
  };
  Kind kind;
  errortrace::Diff<A> diff{};
  errortrace::Variant variant{};
  errortrace::Obj obj{};
  errortrace::Escape<A> escape{};
  errortrace::Diff<ArgLabel> label_diff{};    // Function_label_mismatch
  errortrace::Diff<OptStr> tuple_label_diff{};  // Tuple_label_mismatch
  std::string_view field_name;                // Incompatible_fields
  errortrace::Diff<TypeExpr*> field_diff{};   // Incompatible_fields
  errortrace::FirstClassModule fcm{};
  errortrace::Univar univar{};
  TypeExpr* rec1 = nullptr;                   // Rec_occur
  TypeExpr* rec2 = nullptr;

  static Elt mk(Kind k) {
    Elt e;
    e.kind = k;
    return e;
  }
};

template <class A>
using Trace = std::vector<Elt<A>>;  // ('a, 'variety) t -- an OCaml list, head first

using TypeTrace = Trace<TypeExpr*>;       // 'variety trace
using ErrorTrace = Trace<ExpandedType>;   // 'variety error

template <class A>
Elt<A> swap_elt(const Elt<A>& x) {
  using K = typename Elt<A>::Kind;
  Elt<A> r = x;
  switch (x.kind) {
    case K::Diff: r.diff = swap_diff(x.diff); break;
    case K::Incompatible_fields: r.field_diff = swap_diff(x.field_diff); break;
    case K::Obj:
      if (x.obj.kind == Obj::Kind::Missing_field || x.obj.kind == Obj::Kind::Abstract_row)
        r.obj.pos = swap_position(x.obj.pos);
      else if (x.obj.kind == Obj::Kind::Kind_differ)
        std::swap(r.obj.k1, r.obj.k2);
      break;
    case K::Variant:
      if (x.variant.kind == Variant::Kind::Fixed_row || x.variant.kind == Variant::Kind::No_tags)
        r.variant.pos = swap_position(x.variant.pos);
      break;
    case K::Univar:
      if (x.univar.is_var_mismatch) {
        r.univar.order = swap_order(x.univar.order);
        r.univar.diff = swap_diff(x.univar.diff);
      }
      break;
    case K::First_class_module:
      if (x.fcm.kind == FirstClassModule::Kind::Constraint_on_missing_type ||
          x.fcm.kind == FirstClassModule::Kind::Constraint_with_deps ||
          x.fcm.kind == FirstClassModule::Kind::Constraint_on_mismatched_type)
        r.fcm.pos = swap_position(x.fcm.pos);
      break;
    default:
      break;
  }
  return r;
}

template <class A>
Trace<A> swap_trace(const Trace<A>& t) {
  Trace<A> r;
  for (auto& e : t) r.push_back(swap_elt(e));
  return r;
}

// map_elt f: Diff and Escape Equation carry 'a
template <class A, class B, class F>
Elt<B> map_elt(F&& f, const Elt<A>& x) {
  Elt<B> r;
  r.kind = static_cast<typename Elt<B>::Kind>(x.kind);
  r.variant = x.variant;
  r.obj = x.obj;
  r.label_diff = x.label_diff;
  r.tuple_label_diff = x.tuple_label_diff;
  r.field_name = x.field_name;
  r.field_diff = x.field_diff;
  r.fcm = x.fcm;
  r.univar = x.univar;
  r.rec1 = x.rec1;
  r.rec2 = x.rec2;
  r.escape.kind = static_cast<typename Escape<B>::Kind>(x.escape.kind);
  r.escape.path = x.escape.path;
  r.escape.univ = x.escape.univ;
  r.escape.module = x.escape.module;
  r.escape.context = x.escape.context;
  if (x.kind == Elt<A>::Kind::Diff) {
    // map_diff: ordering is often meaningful when dealing with type_expr
    B got = f(x.diff.got);
    B expected = f(x.diff.expected);
    r.diff = {got, expected};
  }
  if (x.kind == Elt<A>::Kind::Escape && x.escape.kind == Escape<A>::Kind::Equation)
    r.escape.equation = f(x.escape.equation);
  return r;
}

struct UnificationError {  // { trace : unification error }
  ErrorTrace trace;
};
struct EqualityError {
  ErrorTrace trace;
  std::vector<std::pair<TypeExpr*, TypeExpr*>> subst;
};
struct MoregenError {
  ErrorTrace trace;
};
struct ComparisonError {
  bool is_equality;
  EqualityError equality;
  MoregenError moregen;
};

inline UnificationError swap_unification_error(const UnificationError& e) {
  return {swap_trace(e.trace)};
}

namespace subtype {
template <class A>
using Trace = std::vector<Diff<A>>;
struct Error {
  Trace<ExpandedType> trace;
  ErrorTrace unification_trace;
};
}  // namespace subtype

}  // namespace cppcaml::typing::errortrace
