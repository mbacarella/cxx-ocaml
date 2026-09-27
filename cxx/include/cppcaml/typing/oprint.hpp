// Port of typing/oprint.mli (TYPECHECKER.md stage 9): the printers of the
// outcome trees (outcometree.hpp) -- types, class types, module types and
// signature items.  (The toplevel's value / phrase printers are not ported.)
// OCaml's hooks (`out_type := ...`) are plain functions here.
#pragma once

#include <string_view>

#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/outcometree.hpp"

namespace cppcaml::typing::oprint {

using format_doc::Formatter;
namespace ot = outcometree;

void out_ident(Formatter& ppf, const ot::OutIdent* id);          // print_ident
void print_lident(Formatter& ppf, std::string_view s);
void value_ident(Formatter& ppf, std::string_view name);
bool parenthesized_ident(std::string_view name);
void out_type(Formatter& ppf, const ot::OutType* ty);           // print_out_type
void out_type_args(Formatter& ppf, const std::vector<const ot::OutType*>& tyl);  // print_typargs
void out_label(Formatter& ppf, const ot::OutLabel& l);
void out_constr(Formatter& ppf, const ot::OutConstructor& c);
void out_class_type(Formatter& ppf, const ot::OutClassType* cty);
void out_module_type(Formatter& ppf, const ot::OutModuleType* mty);
void out_sig_item(Formatter& ppf, const ot::OutSigItem* item);
void out_signature(Formatter& ppf, const std::vector<const ot::OutSigItem*>& sg);
void out_type_extension(Formatter& ppf, const ot::OutTypeExtension& te);
void out_functor_parameters(Formatter& ppf, const std::vector<ot::OutFunctorParam>& params);
void type_parameter(Formatter& ppf, const ot::OutTypeParam& p);
// Pprintast.Doc.tyvar
void tyvar(Formatter& ppf, std::string_view s);
std::string tyvar_of_name(std::string_view s);

// Misc.Utf8_lexeme.is_valid_identifier
bool is_valid_identifier(std::string_view s);

}  // namespace cppcaml::typing::oprint
