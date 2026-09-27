// See warnings.hpp: utils/warnings.ml's state, option parsing and scopes.
#include <cstdio>
#include "cppcaml/typing/warnings.hpp"

#include "cppcaml/typing/format.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

namespace cppcaml::typing::warnings {

namespace {

// name_to_number: the mnemonic names of warnings.ml's descriptions
std::optional<int> name_to_number(std::string_view s) {
  static const std::unordered_map<std::string, int> h = {
    {"comment-start", 1},
    {"comment-not-end", 2},
    {"fragile-match", 4},
    {"ignored-partial-application", 5},
    {"labels-omitted", 6},
    {"method-override", 7},
    {"partial-match", 8},
    {"missing-record-field-pattern", 9},
    {"non-unit-statement", 10},
    {"redundant-case", 11},
    {"redundant-subpat", 12},
    {"instance-variable-override", 13},
    {"illegal-backslash", 14},
    {"implicit-public-methods", 15},
    {"unerasable-optional-argument", 16},
    {"undeclared-virtual-method", 17},
    {"not-principal", 18},
    {"non-principal-labels", 19},
    {"ignored-extra-argument", 20},
    {"nonreturning-statement", 21},
    {"preprocessor", 22},
    {"useless-record-with", 23},
    {"bad-module-name", 24},
    {"unused-var", 26},
    {"unused-var-strict", 27},
    {"wildcard-arg-to-constant-constr", 28},
    {"eol-in-string", 29},
    {"duplicate-definitions", 30},
    {"module-linked-twice", 31},
    {"unused-value-declaration", 32},
    {"unused-open", 33},
    {"unused-type-declaration", 34},
    {"unused-for-index", 35},
    {"unused-ancestor", 36},
    {"unused-constructor", 37},
    {"unused-extension", 38},
    {"unused-rec-flag", 39},
    {"name-out-of-scope", 40},
    {"ambiguous-name", 41},
    {"disambiguated-name", 42},
    {"nonoptional-label", 43},
    {"open-shadow-identifier", 44},
    {"open-shadow-label-constructor", 45},
    {"bad-env-variable", 46},
    {"attribute-payload", 47},
    {"eliminated-optional-arguments", 48},
    {"no-cmi-file", 49},
    {"unexpected-docstring", 50},
    {"wrong-tailcall-expectation", 51},
    {"fragile-literal-pattern", 52},
    {"misplaced-attribute", 53},
    {"duplicated-attribute", 54},
    {"inlining-impossible", 55},
    {"unreachable-case", 56},
    {"ambiguous-var-in-pattern-guard", 57},
    {"no-cmx-file", 58},
    {"flambda-assignment-to-non-mutable-value", 59},
    {"unused-module", 60},
    {"unboxable-type-in-prim-decl", 61},
    {"constraint-on-gadt", 62},
    {"erroneous-printed-signature", 63},
    {"unsafe-array-syntax-without-parsing", 64},
    {"redefining-unit", 65},
    {"unused-open-bang", 66},
    {"unused-functor-parameter", 67},
    {"match-on-mutable-state-prevent-uncurry", 68},
    {"unused-field", 69},
    {"missing-mli", 70},
    {"unused-tmc-attribute", 71},
    {"tmc-breaks-tailcall", 72},
    {"generative-application-expects-unit", 73},
    {"degraded-to-partial-match", 74},
    {"unnecessarily-partial-tuple-pattern", 75},
  };
  auto it = h.find(std::string(s));
  if (it == h.end()) return std::nullopt;
  return it->second;
}

std::vector<int> letter(char c) {
  switch (c) {
    case 'a': {
      std::vector<int> v;
      for (int i = last_warning_number; i >= 1; --i) v.push_back(i);
      return v;
    }
    case 'c': return {1, 2};
    case 'd': return {3};
    case 'e': return {4};
    case 'f': return {5};
    case 'k': return {32, 33, 34, 35, 36, 37, 38, 39};
    case 'l': return {6};
    case 'm': return {7};
    case 'p': return {8};
    case 'r': return {9};
    case 's': return {10};
    case 'u': return {11, 12};
    case 'v': return {13};
    case 'x': return {14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 30};
    case 'y': return {26};
    case 'z': return {27};
    default: return {};  // b g h i j n o q t w
  }
}

State initial_state() {
  State s;
  s.active.fill(true);
  s.error.fill(false);
  s.alerts = std::make_shared<const AlertSet>(AlertSet{{}, false});
  s.alert_errors = std::make_shared<const AlertSet>(AlertSet{{}, true});  // all soft
  return s;
}

State& current() {
  static State s = [] {
    State st = initial_state();
    return st;
  }();
  return s;
}

void set_alert(bool error, bool enable, std::string_view s) {
  AlertSet upd;
  if (s == "all") {
    upd = {{}, !enable};
  } else {
    upd = error ? *current().alert_errors : *current().alerts;
    if (enable == upd.pos) upd.set.insert(std::string(s));
    else upd.set.erase(std::string(s));
  }
  auto p = std::make_shared<const AlertSet>(std::move(upd));
  if (error) current().alert_errors = p;
  else current().alerts = p;
}

enum class Modifier { Set, Clear, Set_all };
struct Token {
  bool is_letter;
  char c = 0;
  bool has_modifier = false;  // Letter's modifier option
  Modifier m = Modifier::Set;
  int n1 = 0, n2 = 0;  // Num
};

std::vector<Token> parse_warnings(std::string_view s) {
  auto error = [] { throw Bad("Ill-formed list of warnings"); };
  auto get_num = [&](std::size_t i, int& n) {
    n = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') n = 10 * n + (s[i++] - '0');
    return i;
  };
  auto get_range = [&](std::size_t i, int& n1, int& n2) {
    i = get_num(i, n1);
    if (i + 2 < s.size() && s[i] == '.' && s[i + 1] == '.') {
      i = get_num(i + 2, n2);
      if (n2 < n1) error();
    } else {
      n2 = n1;
    }
    return i;
  };
  auto is_alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
  std::vector<Token> tokens;
  std::size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (is_alpha(c)) {
      tokens.push_back(Token{true, c});
      ++i;
      continue;
    }
    Modifier m;
    if (c == '+') m = Modifier::Set;
    else if (c == '-') m = Modifier::Clear;
    else if (c == '@') m = Modifier::Set_all;
    else error();
    ++i;
    if (i >= s.size()) error();
    if (s[i] >= '0' && s[i] <= '9') {
      Token t{false};
      t.m = m;
      i = get_range(i, t.n1, t.n2);
      tokens.push_back(t);
    } else if (is_alpha(s[i])) {
      Token t{true, s[i]};
      t.has_modifier = true;
      t.m = m;
      tokens.push_back(t);
      ++i;
    } else {
      error();
    }
  }
  return tokens;
}

// letter_alert tokens: the deprecated-letters alert
std::optional<Alert> letter_alert(const std::vector<Token>& tokens) {
  // sequences of 2 or more consecutive unsigned letters, most recent first
  std::vector<std::vector<char>> consecutive_letters;
  std::vector<char> current;
  auto commit_chunk = [&] {
    if (current.size() >= 2) consecutive_letters.insert(consecutive_letters.begin(), current);
  };
  for (const Token& t : tokens) {
    if (t.is_letter && !t.has_modifier) {
      current.push_back(t.c);
    } else {
      commit_chunk();
      current.clear();
    }
  }
  commit_chunk();
  if (consecutive_letters.empty()) return std::nullopt;
  const std::vector<char>& example = consecutive_letters[0];
  std::size_t max_seq_len = 0;
  for (auto& x : consecutive_letters) max_seq_len = std::max(max_seq_len, x.size());
  auto print_modifier = [](format::Formatter& ppf, Modifier m) {
    ppf.print_string(m == Modifier::Set_all ? "@" : m == Modifier::Clear ? "-" : "+");
  };
  auto print_token = [&](format::Formatter& ppf, const Token& t) {
    if (!t.is_letter) {
      print_modifier(ppf, t.m);
      ppf.print_string(t.n1 == t.n2 ? std::to_string(t.n1) : std::to_string(t.n1) + ".." + std::to_string(t.n2));
    } else if (t.has_modifier) {
      print_modifier(ppf, t.m);
      ppf.print_char(t.c);
    } else {
      bool lowercase = !(t.c >= 'A' && t.c <= 'Z');
      ppf.print_char(lowercase ? '-' : '+');
      ppf.print_char(t.c);
    }
  };
  format::Formatter f;
  format::fprintf(
      f,
      "@[<v>@[Setting a warning with a sequence of lowercase or uppercase letters,@ like '%a',@ is deprecated.@]@ "
      "@[Use the equivalent signed form:@ %t.@]@ @[Hint: Enabling or disabling a warning by its mnemonic name "
      "requires a + or - prefix.@]%t@?@]",
      [&](format::Formatter& ppf) {
        for (char c : example) ppf.print_char(c);
      },
      [&](format::Formatter& ppf) {
        for (const Token& t : tokens) print_token(ppf, t);
      },
      [&](format::Formatter& ppf) {
        if (max_seq_len >= 5)
          format::fprintf(ppf, "@ @[Hint: Did you make a spelling mistake when using a mnemonic name?@]");
      });
  f.print_flush();
  Location nowhere;  // ghost_loc_in_file "_none_"
  nowhere.loc_start = Position{"_none_", 0, 0, -1};
  nowhere.loc_end = nowhere.loc_start;
  nowhere.loc_ghost = true;
  return Alert{"ocaml_deprecated_cli", f.take(), nowhere, nowhere};
}

std::optional<Alert> parse_opt(std::array<bool, last_warning_number + 1>& error,
                               std::array<bool, last_warning_number + 1>& active, bool errflag, std::string_view s) {
  auto& flags = errflag ? error : active;
  auto action = [&](Modifier m, int i) {
    switch (m) {
      case Modifier::Set:
        if (i == 3) set_alert(errflag, true, "deprecated");
        else flags[i] = true;
        break;
      case Modifier::Clear:
        if (i == 3) set_alert(errflag, false, "deprecated");
        else flags[i] = false;
        break;
      case Modifier::Set_all:
        if (i == 3) {
          set_alert(false, true, "deprecated");
          set_alert(true, true, "deprecated");
        } else {
          active[i] = true;
          error[i] = true;
        }
        break;
    }
  };
  auto eval = [&](const Token& t) {
    if (t.is_letter) {
      char lc = static_cast<char>(t.c >= 'A' && t.c <= 'Z' ? t.c - 'A' + 'a' : t.c);
      Modifier m = t.has_modifier ? t.m : (t.c == lc ? Modifier::Clear : Modifier::Set);
      for (int n : letter(lc)) action(m, n);
    } else {
      for (int n = t.n1; n <= std::min(t.n2, last_warning_number); ++n) action(t.m, n);
    }
  };
  auto parse_and_eval = [&]() -> std::optional<Alert> {
    std::vector<Token> tokens = parse_warnings(s);
    for (const Token& t : tokens) eval(t);
    return letter_alert(tokens);
  };
  if (std::optional<int> n = name_to_number(s)) {
    action(Modifier::Set, *n);
    return std::nullopt;
  }
  if (s.empty()) return parse_and_eval();
  std::optional<int> n = name_to_number(s.substr(1));
  if (s[0] == '+' && n) action(Modifier::Set, *n);
  else if (s[0] == '-' && n) action(Modifier::Clear, *n);
  else if (s[0] == '@' && n) action(Modifier::Set_all, *n);
  else return parse_and_eval();
  return std::nullopt;
}

std::optional<Alert> parse_options_(bool errflag, std::string_view s) {
  auto error = current().error;
  auto active = current().active;
  std::optional<Alert> alrt = parse_opt(error, active, errflag, s);
  current().error = error;
  current().active = active;
  return alrt;
}

// warnings.ml's module initialization: the defaults
struct Defaults {
  Defaults() {
    parse_options_(false, "+a-4-7-9-27-29-30-32..42-44-45-48-50-60-66..70-74");  // defaults_w
    parse_options_(true, "-a");                                                     // defaults_warn_error
    for (const char* a : {"unstable", "unsynchronized_access", "todo"}) set_alert(false, false, a);
  }
};

}  // namespace

bool disabled = false;

static void ensure_defaults() {
  static Defaults d;
}

State backup() {
  ensure_defaults();
  return current();
}
void restore(const State& s) {
  ensure_defaults();
  current() = s;
}
bool is_active(int number) {
  ensure_defaults();
  return !disabled && current().active[static_cast<std::size_t>(number)];
}
bool is_error(int number) {
  ensure_defaults();
  return !disabled && current().error[static_cast<std::size_t>(number)];
}
bool alert_is_active(std::string_view kind) {
  ensure_defaults();
  const AlertSet& a = *current().alerts;
  return !disabled && (a.set.count(std::string(kind)) != 0) == a.pos;
}
bool alert_is_error(std::string_view kind) {
  ensure_defaults();
  const AlertSet& a = *current().alert_errors;
  return !disabled && (a.set.count(std::string(kind)) != 0) == a.pos;
}

std::optional<Alert> parse_options(bool errflag, std::string_view s) {
  ensure_defaults();
  return parse_options_(errflag, s);
}

void parse_alert_option(std::string_view s) {
  ensure_defaults();
  auto bad = [] { throw Bad("Ill-formed list of alert settings"); };
  auto id_char = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '\'' || (c >= '0' && c <= '9');
  };
  std::size_t n = s.size(), i = 0;
  while (i != n) {
    if (i + 1 == n) bad();
    char a = s[i], b = s[i + 1];
    int kind;  // 0 ++, 1 +, 2 --, 3 -, 4 @
    std::size_t start;
    if (a == '+' && b == '+') { kind = 0; start = i + 2; }
    else if (a == '+') { kind = 1; start = i + 1; }
    else if (a == '-' && b == '-') { kind = 2; start = i + 2; }
    else if (a == '-') { kind = 3; start = i + 1; }
    else if (a == '@') { kind = 4; start = i + 1; }
    else bad();
    std::size_t j = start;
    while (j < n && id_char(s[j])) ++j;
    if (j == start) bad();
    std::string_view id = s.substr(start, j - start);
    switch (kind) {
      case 0: set_alert(true, true, id); break;
      case 1: set_alert(false, true, id); break;
      case 2: set_alert(true, false, id); break;
      case 3: set_alert(false, false, id); break;
      case 4: set_alert(true, true, id); set_alert(false, true, id); break;
    }
    i = j;
  }
}


// help_warnings (-warn-help): the descriptions (warning_descriptions.inc,
// generated from ocamlc's Warnings.descriptions), then the letters; the
// caller exits 0
namespace {
struct Description {
  int number;
  std::vector<const char*> names;
  const char* description;
  std::optional<std::pair<int, int>> since;
};
const std::vector<Description>& descriptions() {
  static const std::vector<Description> d = {
#include "warning_descriptions.inc"
  };
  return d;
}
}  // namespace

void help_warnings() {
  for (const Description& d : descriptions()) {
    std::string name = d.names.empty() ? "" : std::string(" [") + d.names[0] + "]";
    char num[16];
    std::snprintf(num, sizeof num, "%3i", d.number);
    std::string line = std::string(num) + name + " " + d.description;
    if (d.since) {
      // pp_since: " (since %d.%0*d)", minor padded to 2 digits before 5.0
      char buf[32];
      if (d.since->first >= 5) std::snprintf(buf, sizeof buf, " (since %d.%d)", d.since->first, d.since->second);
      else std::snprintf(buf, sizeof buf, " (since %d.%02d)", d.since->first, d.since->second);
      line += buf;
    }
    std::fputs((line + "\n").c_str(), stdout);
  }
  std::fputs("  A all warnings\n", stdout);
  for (char c = 'b'; c <= 'z'; ++c) {
    std::vector<int> l = letter(c);
    char up = static_cast<char>(c - 'a' + 'A');
    if (l.empty()) continue;
    if (l.size() == 1) {
      std::printf("  %c Alias for warning %i.\n", up, l[0]);
    } else {
      std::string nums;
      for (std::size_t i = 0; i < l.size(); ++i) nums += (i ? ", " : "") + std::to_string(l[i]);
      std::printf("  %c warnings %s.\n", up, nums.c_str());
    }
  }
  std::fflush(stdout);
}

}  // namespace cppcaml::typing::warnings
