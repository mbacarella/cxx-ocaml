// Port of utils/warnings.ml's state (TYPECHECKER.md): the active / error
// flags of every warning and the alert settings, with `-w` / `-warn-error`
// / `-alert` parsing and the save/restore the [@warning] scopes use.  What
// a warning *says* (Warnings.message, report) is not ported yet: the typer
// needs `is_active`, since some checks only run (and have side effects:
// pattern typing in Parmatch.check_unused creates idents and uids) when a
// warning is enabled.
#pragma once

#include <array>
#include <set>
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

// parse_options errflag s (the deprecated-letters alert it may return is
// not reported yet); raises Bad
void parse_options(bool errflag, std::string_view s);
void parse_alert_option(std::string_view s);

}  // namespace cppcaml::typing::warnings
