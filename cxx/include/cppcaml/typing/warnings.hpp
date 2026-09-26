// The part of utils/warnings.ml the typer consults.  Warnings are not
// reported yet; what the port needs is `is_active`, since some checks only
// run (and have side effects: pattern typing in Parmatch.check_unused) when
// a warning is enabled.  The state is ocamlc's default, "+a-4-7-9-27-29-30-
// 32..42-44-45-48-50-60-66..70-74" (warnings.ml's defaults_w); `-w` and
// [@warning] attributes are not interpreted yet.
#pragma once

namespace cppcaml::typing::warnings {

inline bool is_active(int number) {
  switch (number) {
    case 4: case 7: case 9: case 27: case 29: case 30: case 44: case 45: case 48: case 50: case 60:
    case 74:
      return false;
    default:
      if (number >= 32 && number <= 42) return false;
      if (number >= 66 && number <= 70) return false;
      return true;
  }
}

// the warning numbers used by the port
inline constexpr int Fragile_match = 4;
inline constexpr int Redundant_case = 11;
inline constexpr int Unreachable_case = 56;

}  // namespace cppcaml::typing::warnings
