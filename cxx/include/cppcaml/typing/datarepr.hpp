// Ports of typing/data_types.ml and typing/datarepr.ml (TYPECHECKER.md):
// constructor and label descriptions, computed from type declarations.
#pragma once

#include <utility>
#include <vector>

#include "cppcaml/typing/btype.hpp"

namespace cppcaml::typing {

// ---- data_types.ml ---------------------------------------------------------
struct ConstructorTag {
  enum class Kind : std::uint8_t { Cstr_constant, Cstr_block, Cstr_unboxed, Cstr_extension };
  Kind kind;
  long n = 0;                  // Cstr_constant / Cstr_block
  Path::t ext_path = nullptr;  // Cstr_extension
  bool ext_constant = false;   // Cstr_extension: true if a constant
};

struct ConstructorDescription {
  std::string_view cstr_name;
  TypeExpr* cstr_res;
  Slice<TypeExpr*> cstr_existentials;
  Slice<TypeExpr*> cstr_args;
  long cstr_arity;
  ConstructorTag cstr_tag;
  long cstr_consts;
  long cstr_nonconsts;
  bool cstr_generalized;
  PrivateFlag cstr_private;
  Location cstr_loc;
  Attributes cstr_attributes;
  const TypeDeclaration* cstr_inlined;  // nullptr = None
  Uid cstr_uid;
};

struct LabelDescription {
  std::string_view lbl_name;
  TypeExpr* lbl_res;
  TypeExpr* lbl_arg;
  MutableFlag lbl_mut;
  AtomicFlag lbl_atomic;
  long lbl_pos;
  Slice<const LabelDescription*> lbl_all;  // shared by all the type's labels
  RecordRepresentation lbl_repres;
  PrivateFlag lbl_private;
  Location lbl_loc;
  Attributes lbl_attributes;
  Uid lbl_uid;
};

namespace data_types {
bool equal_tag(const ConstructorTag& a, const ConstructorTag& b);
bool equal_constr(const ConstructorDescription* a, const ConstructorDescription* b);
bool may_equal_constr(const ConstructorDescription* a, const ConstructorDescription* b);
Path::t cstr_res_type_path(const ConstructorDescription* c);
Slice<TypeExpr*> cstr_res_type_params(const ConstructorDescription* c);
Path::t lbl_res_type_path(const LabelDescription* l);
}  // namespace data_types

// ---- datarepr.ml ----------------------------------------------------------------
namespace datarepr {

std::vector<TypeExpr*> free_vars(TypeExpr* ty, bool param = false);  // TypeSet.elements
std::pair<std::vector<TypeExpr*>, std::vector<TypeExpr*>> constructor_existentials(
    const ConstructorArguments& cd_args, TypeExpr* cd_res);

std::vector<std::pair<Ident::t, const ConstructorDescription*>> constructors_of_type(
    const UnitInfo* current_unit, Path::t ty_path, const TypeDeclaration* decl);
std::vector<std::pair<Ident::t, const LabelDescription*>> labels_of_type(
    Path::t ty_path, const TypeDeclaration* decl);
const ConstructorDescription* extension_descr(const UnitInfo* current_unit, Path::t path_ext,
                                              const ExtensionConstructor* ext);

struct ConstrNotFound {};
// find_constr_by_tag: raises ConstrNotFound
const ConstructorDeclaration* find_constr_by_tag(
    const ConstructorTag& tag, Slice<const ConstructorDeclaration*> cstrlist);

}  // namespace datarepr

}  // namespace cppcaml::typing
