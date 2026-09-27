// Port of the parts of parsing/pprintast.ml the message printers use
// (TYPECHECKER.md stage 9): Pprintast.Doc's longident / identifier printers.
#pragma once

#include <string_view>

#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/longident.hpp"

namespace cppcaml::typing::pprintast {

enum class LongidentKind : std::uint8_t { Constr, Type, Value, Other };

void ident_of_name(LongidentKind kind, format_doc::Formatter& ppf, std::string_view txt);
void any_longident(LongidentKind kind, format_doc::Formatter& ppf, Longident::t l);
void longident(format_doc::Formatter& ppf, Longident::t l);       // ~kind:Other
void constr(format_doc::Formatter& ppf, Longident::t l);          // ~kind:Constr
void type_longident(format_doc::Formatter& ppf, Longident::t l);  // ~kind:Type
void value_longident(format_doc::Formatter& ppf, Longident::t l); // ~kind:Value
void tyvar(format_doc::Formatter& ppf, std::string_view s);

}  // namespace cppcaml::typing::pprintast
