// Port of typing/includecore.mli (cxx/PORTING.md, stage 5): inclusion checks
// for the core language (value descriptions, type declarations, extension
// constructors, class types).  The mismatch values mirror includecore.mli;
// the record / variant change lists (Diffing_with_keys, only used to print
// errors) are reduced to the first mismatching field.  Reporting comes with
// Printtyp.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/diffing.hpp"
#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::includecore {

namespace et = errortrace;
using Position = et::Position;

enum class PrimitiveMismatch { Name, Arity, No_alloc, Native_name, Result_repr, Argument_repr };
struct ValueMismatch {  // Primitive_mismatch | Not_a_primitive | Type
  enum class Kind { Primitive_mismatch, Not_a_primitive, Type };
  Kind kind;
  PrimitiveMismatch prim = PrimitiveMismatch::Name;
  Position pos = Position::First;  // No_alloc
  long index = 0;                  // Argument_repr
  et::MoregenError err;            // Type
};
struct DontMatch : std::runtime_error {  // exception Dont_match of value_mismatch
  ValueMismatch mismatch;
  explicit DontMatch(ValueMismatch m) : std::runtime_error("Includecore.Dont_match"), mismatch(std::move(m)) {}
};

enum class PrivacyMismatch {
  Private_type_abbreviation, Private_variant_type, Private_record_type, Private_extensible_variant,
  Private_row_type
};
struct TypeKindName {  // type_kind
  enum class Kind { Kind_abstract, Kind_record, Kind_variant, Kind_open, Kind_external };
  Kind kind;
  std::string_view external;
};
struct LabelMismatch {  // Type | Mutability | Atomicity
  enum class Kind { Type, Mutability, Atomicity };
  Kind kind;
  et::EqualityError err;
  Position pos = Position::First;
};
struct ConstructorMismatch;
// record_change: (label_declaration, label_declaration, label_mismatch)
// Diffing_with_keys.change
using RecordChange = diffing::KeyedChange<const LabelDeclaration*, const LabelDeclaration*, LabelMismatch>;
struct RecordMismatch {  // Label_mismatch of record_change list | Unboxed_float_representation
  enum class Kind { Label_mismatch, Unboxed_float_representation };
  Kind kind;
  std::vector<RecordChange> changes;
  Position pos = Position::First;
};
struct ConstructorMismatch {
  enum class Kind { Type, Arity, Inline_record, Kind_, Explicit_return_type };
  Kind kind;
  et::EqualityError err;
  std::vector<RecordChange> changes;  // Inline_record
  Position pos = Position::First;
};
// variant_change: (constructor_declaration, .., constructor_mismatch) change
using VariantChange =
    diffing::KeyedChange<const ConstructorDeclaration*, const ConstructorDeclaration*, ConstructorMismatch>;
struct ExtensionConstructorMismatch {  // Constructor_privacy | Constructor_mismatch of Ident.t * ..
  enum class Kind { Constructor_privacy, Constructor_mismatch, Constructor_arity };
  Kind kind;
  Ident::t id = nullptr;
  const ExtensionConstructor* ext1 = nullptr;
  const ExtensionConstructor* ext2 = nullptr;
  std::optional<ConstructorMismatch> mismatch;
};
struct PrivateVariantMismatch {
  enum class Kind { Only_outer_closed, Missing, Presence, Incompatible_types_for, Types };
  Kind kind;
  Position pos = Position::First;
  std::string_view tag;
  et::EqualityError err;
};
struct PrivateObjectMismatch {  // Missing of string | Types of equality_error
  enum class Kind { Missing, Types };
  Kind kind;
  std::string_view label;
  et::EqualityError err;
};
struct TypeMismatch {
  enum class Kind {
    Arity, Privacy, Kind_, Constraint, Manifest, Private_variant, Private_object, Variance,
    Record_mismatch, Variant_mismatch, Unboxed_representation, Immediate
  };
  Kind kind;
  PrivacyMismatch privacy = PrivacyMismatch::Private_type_abbreviation;
  TypeKindName k1{}, k2{};                // Kind
  et::EqualityError err;                  // Constraint / Manifest
  TypeExpr* ty1 = nullptr;                // Private_variant / Private_object
  TypeExpr* ty2 = nullptr;
  std::optional<PrivateVariantMismatch> private_variant;
  std::optional<PrivateObjectMismatch> private_object;
  std::optional<RecordMismatch> record;
  std::vector<VariantChange> variant_changes;  // Variant_mismatch
  Position pos = Position::First;         // Unboxed_representation
  bool immediate_violation_always = false;  // Immediate (Type_immediacy.Violation.t)
};

const typedtree::ModuleCoercion* value_descriptions(const Location& loc, env::t env, std::string_view name,
                                                    const ValueDescription* vd1, const ValueDescription* vd2);
std::optional<TypeMismatch> type_declarations(bool equality, const Location& loc, env::t env, bool mark,
                                              std::string_view name, const TypeDeclaration* decl1, Path::t path,
                                              const TypeDeclaration* decl2);
std::optional<ExtensionConstructorMismatch> extension_constructors(const Location& loc, env::t env, bool mark,
                                                                   Ident::t id, const ExtensionConstructor* ext1,
                                                                   const ExtensionConstructor* ext2);
const typedtree::ModuleCoercion* value_descriptions_consistency(env::t env, const ValueDescription* vd1,
                                                                const ValueDescription* vd2);
std::optional<TypeMismatch> type_declarations_consistency(env::t env, const TypeDeclaration* decl1,
                                                          const TypeDeclaration* decl2);
bool class_types(env::t env, const ClassType* cty1, const ClassType* cty2);

// ---- the error messages (includecore.ml's report_* functions) ----
void report_value_mismatch(std::string_view first, std::string_view second, env::t env, format_doc::Formatter& ppf,
                           const ValueMismatch& err);
void report_type_mismatch(std::string_view first, std::string_view second, std::string_view decl, env::t env,
                          format_doc::Formatter& ppf, const TypeMismatch& err);
void report_extension_constructor_mismatch(std::string_view first, std::string_view second, std::string_view decl,
                                           env::t env, format_doc::Formatter& ppf,
                                           const ExtensionConstructorMismatch& err);

}  // namespace cppcaml::typing::includecore
