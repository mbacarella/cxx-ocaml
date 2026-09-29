// Port of middle_end/compilenv.ml (the Closure half): the compilation
// environment of the unit being compiled -- its unit_infos (written as the
// .cmx), the imported units' infos (read from their .cmx), symbols and the
// structured constants.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppcaml/typing/clambda.hpp"
#include "cppcaml/typing/cmx_format.hpp"

namespace cppcaml::typing::compilenv {

struct Error : std::runtime_error {
  enum class Kind { Not_a_unit_info, Corrupted_unit_info, Illegal_renaming, Mismatching_for_pack };
  Kind kind;
  std::string filename, name, modname, pack_1, current_unit;
  std::optional<std::string> pack_2;
  explicit Error(Kind k) : std::runtime_error("Compilenv.Error"), kind(k) {}
};
// report_error (the plain text of report_error_doc)
std::string error_message(const Error& e);

cmx_format::UnitInfos& current_unit();
void reset(const std::optional<std::string>& packname, std::string_view name);
std::string_view current_unit_name();

std::string concat_symbol(std::string_view unitname, std::string_view id);
// make_symbol ?unitname idopt (unitname defaults to current_unit.ui_symbol)
std::string_view make_symbol(std::optional<std::string_view> idopt);
std::string_view make_symbol_in(std::string_view unitname, std::optional<std::string_view> idopt);

const cmx_format::UnitInfos* get_global_info(Ident::t global_ident);
void require_global(Ident::t global_ident);
const clambda::ValueApproximation* global_approx(Ident::t id);
Ident::t stdlib_symbol_name();
std::string_view symbol_for_global(Ident::t id);
void set_global_approx(const clambda::ValueApproximation* approx);

void need_curry_fun(long n);
void need_apply_fun(long n);
void need_send_fun(long n);
void need_stdlib_location();

std::string_view new_const_symbol();

// the structured constants: snapshot / backtrack restore the table
struct Snapshot {
  std::size_t log_size;
};
Snapshot snapshot();
void backtrack(Snapshot s);
std::string_view new_structured_constant(const clambda::UStructuredConstant* cst, bool shared);
void add_exported_constant(std::string_view s);
void clear_structured_constants();
const clambda::UStructuredConstant* structured_constant_of_symbol(std::string_view s);
std::vector<clambda::PreallocatedConstant> structured_constants();

}  // namespace cppcaml::typing::compilenv
