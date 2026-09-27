// Port of parsing/builtin_attributes.ml: the builtin attributes' lookup,
// the tracking of unused ones (warning 53, Misplaced_attribute:
// register_attr / mark_used / warn_unused), alerts ([@deprecated],
// [@alert]) and their checks, and [@warning] / [@warnerror] / [@alert] /
// [@ppwarning] (warning_attribute, warning_scope).
//
// Types records hold attributes as `Attribute` (support.hpp): the parsed
// source's (`ast`) or a .cmi's (`attr_payload`, a generic value); both
// forms are read here.
#pragma once

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "cppcaml/typing/parsetree.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::builtin_attributes {

// ---- unused attributes (warning 53) ----
// register_attr current_phase name (the parser and Ast_invariants register
// every builtin attribute outside attribute payloads)
void register_attr(std::string_view txt, const Location& loc);
void mark_used(std::string_view txt, const Location& loc);
inline void mark_used(const parsetree::StrLoc& name) { mark_used(name.txt, name.loc); }
// warn_unused (): Misplaced_attribute for each attribute still unused, in
// the order of the source; the table is cleared
void warn_unused();
// Clflags.stop_after < Lambda || print_types (compiler_stops_before_
// attributes_consumed): the driver sets it
extern bool stops_before_lambda;
bool is_builtin_attr(std::string_view s);

// attr_equals_builtin: `s` or `ocaml.s`
inline bool attr_equals_builtin(std::string_view txt, std::string_view s) {
  return txt == s || (txt.size() == 6 + s.size() && txt.substr(0, 6) == "ocaml." && txt.substr(6) == s);
}
// has_attribute nm attrs: marks the attribute used
bool has_attribute(std::string_view nm, const parsetree::Attributes& attrs);
bool has_attribute(std::string_view nm, const Attributes& attrs);
inline bool explicit_arity(const parsetree::Attributes& attrs) { return has_attribute("explicit_arity", attrs); }

// select_attributes actions attrs: the attributes named with Return, each
// named one marked used
enum class AttrAction : std::uint8_t { Mark_used_only, Return };
std::vector<const parsetree::Attribute*> select_attributes(
    const std::vector<std::pair<std::string_view, AttrAction>>& actions, const parsetree::Attributes& attrs);

// string_of_payload / string_of_opt_payload: a lone string constant
std::optional<std::string_view> string_of_payload(const parsetree::Payload& p);
inline std::string string_of_opt_payload(const parsetree::Payload& p) {
  std::optional<std::string_view> s = string_of_payload(p);
  return s ? std::string(*s) : std::string();
}
// kind_and_message: `[@alert kind "message"]` / `[@alert kind]`
std::optional<std::pair<std::string, std::string>> kind_and_message(const parsetree::Payload& p);

// ---- marking ----
void mark_alerts_used(const parsetree::Attributes& l);
void mark_alerts_used(const Attributes& l);
void mark_warn_on_literal_pattern_used(const Attributes& l);
void mark_deprecated_mutable_used(const Attributes& l);
// mark_payload_attrs_used payload (the parser, for attributes in payloads)
void mark_payload_attrs_used(const parsetree::Payload& p);
bool warn_on_literal_pattern(const Attributes& attrs);

// ---- alerts ----
// alerts_of_attrs: kind -> message, the messages of one kind joined by "\n"
StrMap<std::string_view> alerts_of_attrs(const std::vector<const parsetree::Attribute*>& l);
StrMap<std::string_view> alerts_of_attrs(const Attributes& l);
// alerts_of_sig ~mark / alerts_of_str ~mark: the leading floating attributes
StrMap<std::string_view> alerts_of_sig(bool mark, const parsetree::Signature& sg);
StrMap<std::string_view> alerts_of_str(bool mark, const parsetree::Structure& str);
// check_alerts loc attrs s / check_alerts_inclusion ~def ~use loc attrs1 attrs2 s
void check_alerts(const Location& loc, const Attributes& attrs, std::string_view s);
void check_alerts_inclusion(const Location& def, const Location& use, const Location& loc, const Attributes& attrs1,
                            const Attributes& attrs2, std::string_view s);
void check_deprecated_mutable(const Location& loc, const Attributes& attrs, std::string_view s);
void check_deprecated_mutable_inclusion(const Location& def, const Location& use, const Location& loc,
                                        const Attributes& attrs1, const Attributes& attrs2, std::string_view s);

// ---- warning attributes ----
// warning_attribute ?ppwarning attr: [@warning "..."] / [@warnerror "..."]
// / [@alert ...] update the warning state; [@ppwarning] reports; bad
// payloads report Attribute_payload
void warning_attribute(const parsetree::Attribute* a, bool ppwarning = true);

// warning_scope ?ppwarning attrs f: f under the attributes' warning
// settings, the state restored afterwards (an exception included)
template <class F>
auto warning_scope(const parsetree::Attributes& attrs, F&& f, bool ppwarning = true) -> decltype(f()) {
  struct Restore {
    warnings::State prev;
    ~Restore() { warnings::restore(prev); }
  } r{warnings::backup()};
  for (std::size_t k = attrs.size(); k-- > 0;) warning_attribute(attrs[k], ppwarning);
  return f();
}

}  // namespace cppcaml::typing::builtin_attributes
