// Port of typing/printtyp.mli (TYPECHECKER.md stage 9): the printers of
// types, declarations and signatures (Printtyp.Doc: Format_doc printers),
// over Out_type's trees and Oprint.
#pragma once

#include <string>

#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/out_type.hpp"

namespace cppcaml::typing::printtyp {

using format_doc::Formatter;

// Printtyp.Doc
void wrap_printing_env(bool error, env::t env, const std::function<void()>& f);
void ident(Formatter& ppf, Ident::t id);
void longident(Formatter& ppf, Longident::t lid);
void typexp(out_type::Mode mode, Formatter& ppf, TypeExpr* ty);
void type_expansion(out_type::Mode k, Formatter& ppf, const out_type::ExpansionPair& e);
void type_declaration(Ident::t id, Formatter& ppf, const TypeDeclaration* decl);
void type_expr(Formatter& ppf, TypeExpr* ty);
void shared_type_scheme(Formatter& ppf, TypeExpr* ty);
void type_scheme(Formatter& ppf, TypeExpr* ty);
void path(Formatter& ppf, Path::t p);
void type_path(Formatter& ppf, Path::t p);
void value_description(Ident::t id, Formatter& ppf, const ValueDescription* decl);
void class_type(Formatter& ppf, const ClassType* cty);
void class_declaration(Ident::t id, Formatter& ppf, const ClassDeclaration* cl);
void cltype_declaration(Ident::t id, Formatter& ppf, const ClassTypeDeclaration* cl);
void modtype(Formatter& ppf, const ModuleType* mty);
void modtype_declaration(Ident::t id, Formatter& ppf, const ModtypeDeclaration* decl);
void constructor(Formatter& ppf, const ConstructorDeclaration* c);
void constructor_arguments(Formatter& ppf, const ConstructorArguments& a);
void label(Formatter& ppf, const LabelDeclaration* l);
void extension_constructor(Ident::t id, Formatter& ppf, const ExtensionConstructor* ext);
void extension_only_constructor(Ident::t id, Formatter& ppf, const ExtensionConstructor* ext);
void signature(Formatter& ppf, Signature sg);
void namespaced_ident(out_type::Namespace ns, Formatter& ppf, Ident::t id);

std::string string_of_path(Path::t p);
std::vector<std::string> strings_of_paths(out_type::Namespace ns, const std::vector<Path::t>& p);

// printed_signature sourcefile ppf sg: the signature -i prints (collisions
// between printed names are Warning 63's, reported by stage 9b)
void printed_signature(const std::string& sourcefile, Formatter& ppf, Signature sg);

// install Printtyp.path where the printers ported before it needed one
// (Printlambda's record_rep)
void install_hooks();

}  // namespace cppcaml::typing::printtyp
