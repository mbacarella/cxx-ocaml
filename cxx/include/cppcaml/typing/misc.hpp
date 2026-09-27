// Port of the message helpers of utils/misc.ml (TYPECHECKER.md stage 9):
// Misc.Style's inline code and hint printers, spellchecking
// (edit_distance, spellcheck, did_you_mean), the error-hint alignment and
// the manual references.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppcaml/typing/format_doc.hpp"

namespace cppcaml::typing::misc {

namespace fd = format_doc;

// ---- Misc.Style ----
namespace style {
// as_inline_code printer ppf x
template <class P, class V>
void as_inline_code(P printer, fd::Formatter& ppf, const V& x) {
  fd::pp_open_stag(ppf, "inline_code");
  printer(ppf, x);
  fd::pp_close_stag(ppf);
}
void inline_code(fd::Formatter& ppf, std::string_view s);
// Style.hint: "@{<hint>Hint@}"
void hint(fd::Formatter& ppf);
// Style.setup !Clflags.color (Location.setup_tags): once, the first time a
// location or a report is printed -- from then on the formatters mark
// their semantic tags, in colour when enabled (-color, OCAML_COLOR,
// NO_COLOR; auto: $TERM is set, not "dumb", and stderr is a tty)
void setup();
bool marks_enabled();
// mark_open_tag / mark_close_tag: a style tag's ANSI code (or nothing
// without colours); Format's default "<t>" / "</t>" for another tag.  A
// Misc.Style.Style tag is "@style:<codes>" (Diffing's styles).
std::string mark_open_tag(const std::string& tag);
std::string mark_close_tag(const std::string& tag);
// the `%a` argument `as_inline_code printer` applied to x
template <class P, class V>
auto code(P printer, V x) {
  return [printer, x](fd::Formatter& ppf) { as_inline_code(printer, ppf, x); };
}
inline auto code_str(std::string s) {
  return [s = std::move(s)](fd::Formatter& ppf) { inline_code(ppf, s); };
}
}  // namespace style

// edit_distance a b cutoff: None when above the cutoff
std::optional<long> edit_distance(std::string_view a, std::string_view b, long cutoff);
// spellcheck env name: the closest names
std::vector<std::string> spellcheck(const std::vector<std::string>& env, std::string_view name);
// did_you_mean ?(pp = Style.inline_code) choices
std::optional<fd::Doc> did_you_mean(const std::vector<std::string>& choices,
                                    const std::function<void(fd::Formatter&, const std::string&)>& pp = nullptr);

// align_hint ~prefix ~main ~hint / align_error_hint ~main ~hint
std::pair<fd::Doc, fd::Doc> align_hint(std::string_view prefix, const fd::Doc& main, const fd::Doc& hint);
std::pair<fd::Doc, fd::Doc> align_error_hint(const fd::Doc& main, const fd::Doc& hint);

// ordinal_suffix n: "st" / "nd" / "rd" / "th"
const char* ordinal_suffix(long n);

// the manual references
void print_manual_section(fd::Formatter& ppf, const std::vector<long>& section);
void print_see_manual(fd::Formatter& ppf, const std::vector<long>& section);
void print_manual_hint(fd::Formatter& ppf, const std::vector<long>& section);

}  // namespace cppcaml::typing::misc
