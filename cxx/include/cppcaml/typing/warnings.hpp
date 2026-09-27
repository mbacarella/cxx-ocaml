// Port of utils/warnings.ml (TYPECHECKER.md): the active / error flags of
// every warning and the alert settings, with `-w` / `-warn-error` /
// `-alert` parsing and the save/restore the [@warning] scopes use; the
// warnings themselves (Warnings.t), what they say (message) and their
// reporting (report, report_alert, check_fatal -- warnings_message.cpp).
#pragma once

#include <array>
#include <optional>
#include <set>
#include <vector>

#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/support.hpp"
#include <stdexcept>
#include <string>
#include <string_view>

namespace cppcaml::typing::warnings {

inline constexpr int last_warning_number = 75;

// the warning numbers used by the port
inline constexpr int Fragile_match = 4;
inline constexpr int Redundant_case = 11;
inline constexpr int Misplaced_attribute = 53;
inline constexpr int Unreachable_case = 56;

// (Misc.Stdlib.String.Set.t * bool): false = the set's complement
struct AlertSet {
  std::set<std::string> set;
  bool pos = false;
};

struct State {
  std::array<bool, last_warning_number + 1> active{};
  std::array<bool, last_warning_number + 1> error{};
  AlertSet alerts;
  AlertSet alert_errors;
};

// Arg.Bad of the option parsers
struct Bad : std::runtime_error {
  explicit Bad(const std::string& m) : std::runtime_error(m) {}
};

State backup();
void restore(const State& s);
bool is_active(int number);
bool is_error(int number);
bool alert_is_active(std::string_view kind);
bool alert_is_error(std::string_view kind);

// without_warnings f
template <class F>
auto without_warnings(F&& f) -> decltype(f());
extern bool disabled;

template <class F>
auto with_state(const State& st, F&& f) -> decltype(f()) {
  State prev = backup();
  restore(st);
  struct Restore {
    State prev;
    ~Restore() { restore(prev); }
  } r{prev};
  return f();
}

template <class F>
auto without_warnings(F&& f) -> decltype(f()) {
  bool saved = disabled;
  disabled = true;
  struct Restore {
    bool saved;
    ~Restore() { disabled = saved; }
  } r{saved};
  return f();
}

// ---- Warnings.t ------------------------------------------------------------------

enum class FieldUsage : std::uint8_t { Unused, Not_read, Not_mutated };
enum class ConstructorUsage : std::uint8_t { Unused, Not_constructed, Only_exported_private };
enum class TypeDeclarationUsage : std::uint8_t { Declaration, Alias };

// One warning; the payload fields a constructor uses are noted.
struct Warning {
  enum class K : std::uint8_t {
    Comment_start, Comment_not_end, Fragile_match /*s*/, Ignored_partial_application,
    Labels_omitted /*l*/, Method_override /*l*/, Partial_match /*doc*/,
    Missing_record_field_pattern /*s*/, Non_unit_statement, Redundant_case, Redundant_subpat,
    Instance_variable_override /*l*/, Illegal_backslash, Implicit_public_methods /*l*/,
    Unerasable_optional_argument, Undeclared_virtual_method /*s*/, Not_principal /*doc*/,
    Non_principal_labels /*s*/, Ignored_extra_argument, Nonreturning_statement, Preprocessor /*s*/,
    Useless_record_with, Bad_module_name /*s*/, All_clauses_guarded, Unused_var /*s*/,
    Unused_var_strict /*s*/, Wildcard_arg_to_constant_constr, Eol_in_string,
    Duplicate_definitions /*s s2 s3 s4*/, Unused_value_declaration /*s*/, Unused_open /*s*/,
    Unused_type_declaration /*s tdusage*/, Unused_for_index /*s*/, Unused_ancestor /*s*/,
    Unused_constructor /*s cusage*/, Unused_extension /*s b cusage*/, Unused_rec_flag,
    Name_out_of_scope /*s l b*/, Ambiguous_name /*l l2 b s*/, Disambiguated_name /*s*/,
    Nonoptional_label /*s*/, Open_shadow_identifier /*s s2*/, Open_shadow_label_constructor /*s s2*/,
    Bad_env_variable /*s s2*/, Attribute_payload /*s s2*/, Eliminated_optional_arguments /*l*/,
    No_cmi_file /*s opt*/, Unexpected_docstring /*b*/, Wrong_tailcall_expectation /*b*/,
    Fragile_literal_pattern, Misplaced_attribute /*s*/, Duplicated_attribute /*s*/,
    Inlining_impossible /*s*/, Unreachable_case, Ambiguous_var_in_pattern_guard /*l*/,
    No_cmx_file /*s*/, Flambda_assignment_to_non_mutable_value, Unused_module /*s*/,
    Unboxable_type_in_prim_decl /*s*/, Constraint_on_gadt, Erroneous_printed_signature /*s*/,
    Unsafe_array_syntax_without_parsing, Redefining_unit /*s*/, Unused_open_bang /*s*/,
    Unused_functor_parameter /*s*/, Match_on_mutable_state_prevent_uncurry,
    Unused_field /*s fusage*/, Missing_mli, Unused_tmc_attribute, Tmc_breaks_tailcall,
    Generative_application_expects_unit, Degraded_to_partial_match,
    Unnecessarily_partial_tuple_pattern
  };
  K k;
  std::string s, s2, s3, s4;
  std::vector<std::string> l, l2;
  bool b = false;
  std::optional<std::string> opt;
  format_doc::Doc doc;
  FieldUsage fusage = FieldUsage::Unused;
  ConstructorUsage cusage = ConstructorUsage::Unused;
  TypeDeclarationUsage tdusage = TypeDeclarationUsage::Declaration;

  static Warning make(K k) {
    Warning w;
    w.k = k;
    return w;
  }
  static Warning with_s(K k, std::string s) {
    Warning w = make(k);
    w.s = std::move(s);
    return w;
  }
  static Warning with_l(K k, std::vector<std::string> l) {
    Warning w = make(k);
    w.l = std::move(l);
    return w;
  }
};

int number(const Warning& w);
bool is_active(const Warning& w);
bool is_error(const Warning& w);

// type alert = {kind; message; def; use}
struct Alert {
  std::string kind;
  std::string message;
  Location def;
  Location use;
};
bool alert_is_active(const Alert& a);
bool alert_is_error(const Alert& a);

// message w
format_doc::Doc message(const Warning& w);

struct ReportingInformation {
  std::string id;
  format_doc::Doc message;
  bool is_error;
  std::vector<std::pair<Location, format_doc::Doc>> sub_locs;
};
// report w / report_alert a: None = `Inactive
std::optional<ReportingInformation> report(const Warning& w);
std::optional<ReportingInformation> report_alert(const Alert& a);

extern int nerrors;
void reset_fatal();
// check_fatal: raises Errors (Location.Already_displayed_error) when a
// warning-as-error was reported
void check_fatal();

// help_warnings (-warn-help), on stdout
void help_warnings();

// parse_options errflag s: the deprecated-letters alert it may return;
// raises Bad
std::optional<Alert> parse_options(bool errflag, std::string_view s);
void parse_alert_option(std::string_view s);

}  // namespace cppcaml::typing::warnings
