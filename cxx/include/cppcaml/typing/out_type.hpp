// Port of typing/out_type.mli (TYPECHECKER.md stage 9): the outcome trees
// of types, declarations and signatures -- naming of type variables, alias
// marking of cyclic / shared types, identifier disambiguation (`t/2`) in
// the printing environment.  The short-paths machinery (-short-paths,
// Clflags.real_paths = false) is not ported: paths print as they are.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/outcometree.hpp"
#include "cppcaml/typing/shape.hpp"
#include "cppcaml/typing/types.hpp"

namespace cppcaml::typing::out_type {

namespace ot = outcometree;

// Shape.Sig_component_kind.t
enum class Namespace : std::uint8_t {
  Value, Type, Constructor, Label, Module, Module_type, Extension_constructor, Class, Class_type
};
std::string_view namespace_to_string(Namespace n);

// the printing environment
env::t printing_env();
void set_printing_env(env::t env);
// wrap_printing_env ~error env f
void wrap_printing_env(bool error, env::t env, const std::function<void()>& f);

// Out_name / ident naming
ot::OutName* ident_name(std::optional<Namespace> ns, Ident::t id);

// Ident_conflicts
namespace ident_conflicts {
void reset();
bool exists();
// err_msg: the explanation document of the collisions seen, None if none
std::optional<format_doc::Doc> err_msg();
// err_print ppf: "@,%a" of err_msg, if any
void err_print(format_doc::Formatter& ppf);
}  // namespace ident_conflicts

// Ident_names
namespace ident_names {
void enable(bool b);
void with_fuzzy(Ident::t id, const std::function<void()>& f);
}  // namespace ident_names

// tree_of_path ?disambiguation p (None namespace) / namespaced_tree_of_path
const ot::OutIdent* tree_of_path(Path::t p, bool disambiguation = true);
const ot::OutIdent* namespaced_tree_of_path(Namespace n, Path::t p);
const ot::OutIdent* tree_of_type_path(Path::t p);

ot::OutRecStatus tree_of_rec(RecStatus rs);

enum class Mode : std::uint8_t { Type, Type_scheme };

// preparation of type variables and aliases
void reset();
void reset_except_conflicts();
void prepare_for_printing(const std::vector<TypeExpr*>& tyl);
void add_type_to_preparation(TypeExpr* ty);
void prepare_type(TypeExpr* ty);
void reserve_names(TypeExpr* ty);  // Variable_names.reserve
void mark_loops(TypeExpr* ty);     // Aliases.mark_loops
void reset_aliases();              // Aliases.reset
void add_delayed(TypeExpr* ty);    // Aliases.add_delayed (proxy t)
void with_local_names(const std::function<void()>& f);
void add_subst(const std::vector<std::pair<TypeExpr*, TypeExpr*>>& subst);  // Variable_names.add_subst
std::string new_name();            // Variable_names.new_name
std::string name_of_type_named(TypeExpr* repr_t);  // name_of_type new_name t

// print_labels / with_labels
extern bool print_labels;
void with_labels(bool b, const std::function<void()>& f);

const ot::OutType* tree_of_typexp(Mode mode, TypeExpr* ty);
std::vector<const ot::OutType*> tree_of_typlist(Mode mode, const std::vector<TypeExpr*>& tyl);
void prepared_type_expr(format_doc::Formatter& ppf, TypeExpr* ty);
void prepared_type_scheme(format_doc::Formatter& ppf, TypeExpr* ty);
void type_expr_with_reserved_names(format_doc::Formatter& ppf, TypeExpr* ty);
void typexp(Mode mode, format_doc::Formatter& ppf, TypeExpr* ty);

// declarations
const ot::OutSigItem* tree_of_type_declaration(Ident::t id, const TypeDeclaration* decl, RecStatus rs);
const ot::OutSigItem* tree_of_prepared_type_declaration(Ident::t id, const TypeDeclaration* decl, RecStatus rs);
void add_type_declaration_to_preparation(Ident::t id, const TypeDeclaration* decl);
void prepared_type_declaration(Ident::t id, format_doc::Formatter& ppf, const TypeDeclaration* decl);
ot::OutLabel tree_of_label(const LabelDeclaration* l);
std::vector<const ot::OutType*> tree_of_constructor_arguments(const ConstructorArguments& a);
void prepare_type_constructor_arguments(const ConstructorArguments& a);
void add_constructor_to_preparation(const ConstructorDeclaration* c);
void prepared_constructor(format_doc::Formatter& ppf, const ConstructorDeclaration* c);
ot::OutConstructor tree_of_single_constructor(const ConstructorDeclaration* cd);
const ot::OutSigItem* tree_of_extension_constructor(Ident::t id, const ExtensionConstructor* ext, ExtStatus es);
void add_extension_constructor_to_preparation(const ExtensionConstructor* ext);
void prepared_extension_constructor(Ident::t id, format_doc::Formatter& ppf, const ExtensionConstructor* ext);
std::pair<std::vector<const ot::OutType*>, const ot::OutType*> extension_constructor_args_and_ret_type_subtree(
    const ConstructorArguments& args, TypeExpr* ret);
const ot::OutSigItem* tree_of_value_description(Ident::t id, const ValueDescription* decl);
const ot::OutClassType* tree_of_class_type(Mode mode, const ClassType* cty);
void prepare_class_type(const ClassType* cty);
const ot::OutSigItem* tree_of_class_declaration(Ident::t id, const ClassDeclaration* cl, RecStatus rs);
const ot::OutSigItem* tree_of_cltype_declaration(Ident::t id, const ClassTypeDeclaration* cl, RecStatus rs);
const ot::OutModuleType* tree_of_modtype(const ModuleType* mty);
const ot::OutSigItem* tree_of_modtype_declaration(Ident::t id, const ModtypeDeclaration* decl);
const ot::OutSigItem* tree_of_module(Ident::t id, const ModuleType* mty, RecStatus rs);
std::vector<const ot::OutSigItem*> tree_of_signature(Signature sg);
const ot::OutSigItem* tree_of_sigitem(const SignatureItem* it);
ot::OutFunctorParam tree_of_functor_parameter_tree(const FunctorParameter& p);

// type expansions (error messages)
struct ExpansionDiff {
  const ot::OutType* ty;
  const ot::OutType* expanded;  // option
  const ot::OutType* manifest;  // option
};
struct ExpansionPair {  // Errortrace.{ty; expanded}
  TypeExpr* ty;
  TypeExpr* expanded;
};
ExpansionDiff trees_of_type_expansion(Mode mode, const ExpansionPair& e);
void pp_type(format_doc::Formatter& ppf, const ot::OutType* t);
void pp_type_expansion(format_doc::Formatter& ppf, const ExpansionDiff& d);
ExpansionPair prepare_expansion(const ExpansionPair& e);
TypeExpr* hide_variant_name(TypeExpr* t);
bool same_path(TypeExpr* t, TypeExpr* t2);

// Internal_names
namespace internal_names {
void reset();
void add(Path::t p);
struct Explanation {
  enum class K : std::uint8_t { Existential, Equation } k;
  std::string constructor;
  TypeExpr* lhs = nullptr;
  TypeExpr* rhs = nullptr;
};
std::vector<std::pair<std::vector<Path::t>, Explanation>> explain(env::t env);
}  // namespace internal_names

}  // namespace cppcaml::typing::out_type
