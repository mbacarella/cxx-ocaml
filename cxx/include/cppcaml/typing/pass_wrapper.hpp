// Port of middle_end/flambda/pass_wrapper.ml: a registered pass (for
// -dump-pass) and its dump around a run.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/format.hpp"

namespace cppcaml::typing::pass_wrapper {

// register ~pass_name (at startup, as OCaml's module initializers do)
inline bool register_pass(const char* pass_name) {
  clflags::all_passes.insert(clflags::all_passes.begin(), pass_name);
  return true;
}

inline bool dumped_pass(const std::string& s) {
  for (const std::string& p : clflags::dumped_passes_list)
    if (p == s) return true;
  return false;
}

// with_dump ~ppf_dump ~pass_name ~f ~input ~print_input ~print_output
template <class R>
std::optional<R> with_dump(format::Formatter& ppf_dump, const std::string& pass_name,
                           const std::function<std::optional<R>()>& f,
                           const std::function<void(format::Formatter&)>& print_input,
                           const std::function<void(format::Formatter&, const R&)>& print_output) {
  bool dump = dumped_pass(pass_name);
  std::optional<R> result = f();
  if (!result) {
    if (dump) {
      format::fprintf(ppf_dump, "%s: no-op.\n\n", pass_name);
      ppf_dump.print_flush();
    }
    return std::nullopt;
  }
  if (dump) {
    format::fprintf(ppf_dump, "Before %s:@ %a@.@.", pass_name, print_input);
    format::fprintf(ppf_dump, "After %s:@ %a@.@.", pass_name, [&](format::Formatter& g) { print_output(g, *result); });
  }
  return result;
}

}  // namespace cppcaml::typing::pass_wrapper
