// See misc.hpp.
#include "cppcaml/typing/misc.hpp"

#include <algorithm>
#include <cstdlib>

namespace cppcaml::typing::misc {

namespace style {
void inline_code(fd::Formatter& ppf, std::string_view s) {
  fd::pp_open_stag(ppf, "inline_code");
  fd::pp_print_string(ppf, s);
  fd::pp_close_stag(ppf);
}
void hint(fd::Formatter& ppf) { fd::fprintf(ppf, "@{<hint>Hint@}"); }
}  // namespace style

std::optional<long> edit_distance(std::string_view a, std::string_view b, long cutoff) {
  long la = static_cast<long>(a.size()), lb = static_cast<long>(b.size());
  cutoff = std::min(std::max(la, lb), cutoff);
  if (std::labs(la - lb) > cutoff) return std::nullopt;
  std::vector<std::vector<long>> m(la + 1, std::vector<long>(lb + 1, cutoff + 1));
  m[0][0] = 0;
  for (long i = 1; i <= la; ++i) m[i][0] = i;
  for (long j = 1; j <= lb; ++j) m[0][j] = j;
  for (long i = 1; i <= la; ++i)
    for (long j = std::max(1L, i - cutoff - 1); j <= std::min(lb, i + cutoff + 1); ++j) {
      long cost = a[i - 1] == b[j - 1] ? 0 : 1;
      long best = std::min(1 + std::min(m[i - 1][j], m[i][j - 1]), m[i - 1][j - 1] + cost);
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) best = std::min(best, m[i - 2][j - 2] + cost);
      m[i][j] = best;
    }
  long result = m[la][lb];
  if (result > cutoff) return std::nullopt;
  return result;
}

std::vector<std::string> spellcheck(const std::vector<std::string>& env0, std::string_view name) {
  long cutoff;
  switch (name.size()) {
    case 1: case 2: cutoff = 0; break;
    case 3: case 4: cutoff = 1; break;
    case 5: case 6: cutoff = 2; break;
    default: cutoff = 3; break;
  }
  // List.sort_uniq (fun s1 s2 -> String.compare s2 s1) env: descending
  std::vector<std::string> env = env0;
  std::sort(env.begin(), env.end(), [](const std::string& a, const std::string& b) { return b < a; });
  env.erase(std::unique(env.begin(), env.end()), env.end());
  // fold_left: best_choice is consed, so later heads come first
  std::vector<std::string> best;  // in reverse (consing) order
  long best_dist = -1;            // max_int
  for (const std::string& head : env) {
    std::optional<long> d = edit_distance(name, head, cutoff);
    if (!d) continue;
    if (best_dist < 0 || *d < best_dist) {
      best = {head};
      best_dist = *d;
    } else if (*d == best_dist) {
      best.push_back(head);
    }
  }
  std::reverse(best.begin(), best.end());
  return best;
}

std::optional<fd::Doc> did_you_mean(const std::vector<std::string>& choices,
                                    const std::function<void(fd::Formatter&, const std::string&)>& pp0) {
  if (choices.empty()) return std::nullopt;
  std::function<void(fd::Formatter&, const std::string&)> pp =
      pp0 ? pp0 : [](fd::Formatter& ppf, const std::string& s) { style::inline_code(ppf, s); };
  std::vector<std::string> rest(choices.begin(), choices.end() - 1);
  const std::string& last = choices.back();
  return fd::doc_printf(
      "@[@{<hint>Hint@}: @{<ralign>Did you mean @}%a%s%a?@]",
      [&](fd::Formatter& ppf) { fd::pp_print_list(ppf, pp, rest, fd::comma); }, rest.empty() ? "" : " or ",
      [&](fd::Formatter& ppf) { pp(ppf, last); });
}

std::pair<fd::Doc, fd::Doc> align_hint(std::string_view prefix, const fd::Doc& main, const fd::Doc& hint) {
  return fd::align_prefix2({main, static_cast<long>(prefix.size())}, {hint, 0});
}
std::pair<fd::Doc, fd::Doc> align_error_hint(const fd::Doc& main, const fd::Doc& hint) {
  return align_hint("Error: ", main, hint);
}

void print_manual_section(fd::Formatter& ppf, const std::vector<long>& section) {
  fd::fprintf(ppf, "manual section %a", [&](fd::Formatter& f) {
    bool first = true;
    for (long n : section) {
      if (!first) fd::pp_print_char(f, '.');
      first = false;
      fd::pp_print_int(f, n);
    }
  });
}
void print_see_manual(fd::Formatter& ppf, const std::vector<long>& section) {
  fd::fprintf(ppf, "(see %a)", [&](fd::Formatter& f) { print_manual_section(f, section); });
}
void print_manual_hint(fd::Formatter& ppf, const std::vector<long>& section) {
  fd::fprintf(ppf, "%t (%a):", [](fd::Formatter& f) { style::hint(f); },
              [&](fd::Formatter& f) { print_manual_section(f, section); });
}

}  // namespace cppcaml::typing::misc
