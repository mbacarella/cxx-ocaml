// Port of typing/typeclass.mli (TYPECHECKER.md, stage 5): typing of class
// declarations, class descriptions and class type declarations, and of
// immediate objects (installed as Typecore.type_object).  Errors are
// `typeclass::Error` (the OCaml Error.In_context); reporting comes with
// Printtyp.
#pragma once

#include "cppcaml/typing/format_doc.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::typeclass {

namespace tt = typedtree;
namespace pt = parsetree;
namespace et = errortrace;

template <class A>
struct ClassInfo {  // 'a class_info
  Ident::t cls_id;
  pt::StrLoc cls_id_loc;
  const ClassDeclaration* cls_decl;
  Ident::t cls_ty_id;
  const ClassTypeDeclaration* cls_ty_decl;
  Ident::t cls_obj_id;
  const TypeDeclaration* cls_obj_abbr;
  const TypeDeclaration* cls_abbr;
  long cls_arity;
  std::vector<std::string_view> cls_pub_methods;
  A cls_info;
};
struct ClassTypeInfo {  // class_type_info
  Ident::t clsty_ty_id;
  pt::StrLoc clsty_id_loc;
  const ClassTypeDeclaration* clsty_ty_decl;
  Ident::t clsty_obj_id;
  const TypeDeclaration* clsty_obj_abbr;
  const TypeDeclaration* clsty_abbr;
  const tt::TClassTypeDeclaration* clsty_info;
};

std::pair<std::vector<ClassInfo<const tt::TClassDeclaration*>>, env::t> class_declarations(
    env::t env, Slice<const pt::ClassDeclaration*> cls);
std::pair<std::vector<ClassInfo<const tt::TClassDescription*>>, env::t> class_descriptions(
    env::t env, Slice<const pt::ClassDescription*> cls);
std::pair<std::vector<ClassTypeInfo>, env::t> class_type_declarations(env::t env,
                                                                      Slice<const pt::ClassDescription*> cls);
std::pair<std::vector<ClassTypeInfo>, env::t> approx_class_declarations(env::t env,
                                                                        Slice<const pt::ClassDescription*> sdecls);

enum class Kind { Object, Class, Class_type };

struct Error : std::runtime_error {
  enum class K {
    Unconsistent_constraint, Field_type_mismatch, Unexpected_field, Structure_expected, Cannot_apply,
    Apply_wrong_label, Pattern_type_clash, Repeated_parameter, Unbound_class_2, Unbound_class_type_2,
    Abbrev_type_clash, Constructor_type_mismatch, Virtual_class, Undeclared_methods,
    Parameter_arity_mismatch, Parameter_mismatch, Bad_parameters, Bad_class_type_parameters,
    Class_match_failure, Unbound_val, Unbound_type_var, Non_generalizable_class, Cannot_coerce_self,
    Non_collapsable_conjunction, Self_clash, Mutability_mismatch, No_overriding, Duplicate,
    Closing_self_type, Polymorphic_class_parameter
  };
  Location loc;
  env::t env;
  K kind;
  // payloads (which ones are set depends on the kind)
  et::UnificationError trace;
  std::string name, name2;                    // the string arguments
  TypeExpr* ty = nullptr;
  TypeExpr* ty2 = nullptr;
  TypeExpr* ty3 = nullptr;
  const ClassType* cty = nullptr;             // Structure_expected / Cannot_apply
  ArgLabel label;                             // Apply_wrong_label
  Longident::t lid = nullptr;
  Kind class_kind = Kind::Class;              // Virtual_class / Undeclared_methods
  std::vector<std::string_view> names, names2;
  long n1 = 0, n2 = 0;
  Ident::t id = nullptr;
  std::vector<TypeExpr*> tys, tys2;           // Bad_parameters / Non_generalizable_class
  std::vector<ctype::ClassMatchFailure> failures;
  std::optional<ctype::ClosedClassFailure> closed_failure;  // Unbound_type_var
  const ClassDeclaration* clty = nullptr;
  MutableFlag mut = MutableFlag::Immutable;
  const ClassSignature* sign = nullptr;       // Closing_self_type
  format_doc::Doc decl_doc;                   // Unbound_type_var: the printed declaration
  Error(const Location& l, env::t e, K k) : std::runtime_error("Typeclass.Error"), loc(l), env(e), kind(k) {}
};
// Error_forward of Location.error (an uninterpreted extension node)
struct ErrorForward : std::runtime_error {
  const pt::Extension* ext;
  explicit ErrorForward(const pt::Extension* e) : std::runtime_error("Typeclass.Error_forward"), ext(e) {}
};

// Forward declaration, set by Typemod: type_open_descr ?used_slot env od
extern std::function<std::pair<const tt::OpenDescription*, env::t>(bool* used_slot, env::t,
                                                                  const pt::OpenDescription*)>
    type_open_descr;

// `let () = Typecore.type_object := type_object`: installed explicitly
// (Typemod's initialization calls it) to avoid static-initialization order
// issues with the Typecore global.
void install_forward_refs();

// creates typeclass.ml's module-initialization values (unbound_class)
void module_init();

}  // namespace cppcaml::typing::typeclass
