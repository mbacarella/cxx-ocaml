// Port of the message helpers of utils/misc.ml (cxx/PORTING.md stage 9):
// Misc.Style's inline code and hint printers, spellchecking
// (edit_distance, spellcheck, did_you_mean), the error-hint alignment and
// the manual references.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppcaml/typing/build_path_prefix_map.hpp"
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

// exception Fatal_error: fatal_errorf prints ">> Fatal error: <msg>" on
// stderr and raises it; nothing reports it, so it reaches the uncaught
// exception handler ("Fatal error: exception Misc.Fatal_error", exit 2)
struct FatalError {};
[[noreturn]] void fatal_error(const std::string& msg);

// get_build_path_prefix_map (): BUILD_PATH_PREFIX_MAP, decoded once (an
// invalid value is a fatal error)
const build_path_prefix_map::Map* get_build_path_prefix_map();
// invert_build_path_prefix_map path
std::vector<std::string> invert_build_path_prefix_map(const std::string& path);

// ---- files and paths (the linker's) ----
// find_in_path path name: raises NotFound
struct NotFound {};
std::string find_in_path(const std::vector<std::string>& path, const std::string& name);
// remove_file: when it is a regular file, ignoring errors
void remove_file(const std::string& filename);
// split_path_contents ?(sep = ':') s
std::vector<std::string> split_path_contents(const std::string& s, char sep = ':');
// concat_null_terminated l
std::string concat_null_terminated(const std::vector<std::string>& l);
// replace_substring ~before ~after str
std::string replace_substring(const std::string& before, const std::string& after, const std::string& str);

// ---- Misc.RuntimeID ----
struct RuntimeID {
  bool dev = false;
  int release = 0;
  int reserved = 0;
  bool no_flat_float_array = false;
  bool fp = false;
  bool tsan = false;
  bool int31 = false;
  bool is_static = false;  // static
  bool no_compression = false;
  bool ansi = false;

  // make_zinc () / make_bytecode (), with the Config / Sys defaults
  static RuntimeID make_zinc();
  static RuntimeID make_bytecode();
  bool is_zinc() const;
  bool is_bytecode() const;
  std::string to_string() const;
  static std::optional<RuntimeID> of_string(const std::string& s);
  // ocamlrun variant t (raises std::invalid_argument unless is_zinc)
  std::string ocamlrun(const std::string& variant) const;
};
// RuntimeID.shared_runtime Sys.Bytecode (~prefix:"-l", ~host:Config.target)
std::string shared_runtime_bytecode();
// RuntimeID.stubslib name (~runtime_id:(make_bytecode ()), ~host:Config.target)
std::string stubslib(const std::string& name);

}  // namespace cppcaml::typing::misc
