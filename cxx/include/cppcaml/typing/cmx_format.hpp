// Port of file_formats/cmx_format.mli (the non-flambda export info) and
// Compilenv.read_unit_info: a .cmx is cmx_magic_number, the marshaled
// unit_infos, then the BLAKE128 digest of the file up to it.  The reader
// lives with the .cmi Reader (cmi_format.cpp), whose idents, paths and
// strings it shares.
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cppcaml/typing/clambda.hpp"

namespace cppcaml::typing::cmx_format {

inline constexpr const char* cmx_magic_number = "Caml1999Y038";

using Crcs = std::vector<std::pair<std::string_view, std::optional<std::string>>>;

struct UnitInfos {
  std::string_view ui_name;         // Name of unit implemented
  std::string_view ui_symbol;       // Prefix for symbols
  std::vector<std::string_view> ui_defines;  // Unit and sub-units implemented
  Crcs ui_imports_cmi;              // Interfaces imported
  Crcs ui_imports_cmx;              // Infos imported
  std::vector<long> ui_curry_fun;   // Currying functions needed
  std::vector<long> ui_apply_fun;   // Apply functions needed
  std::vector<long> ui_send_fun;    // Send functions needed
  const clambda::ValueApproximation* ui_export_info;  // Clambda approx
  bool ui_force_link;               // Always linked
  std::optional<std::string_view> ui_for_pack;  // Part of a pack
  bool ui_need_stdlib;              // caml_standard_library_nat needed
};

struct Error : std::runtime_error {
  enum class Kind { Not_a_unit_info, Corrupted_unit_info };
  Kind kind;
  std::string filename;
  Error(Kind k, std::string f) : std::runtime_error("Compilenv.Error"), kind(k), filename(std::move(f)) {}
};

// Compilenv.read_unit_info filename: the unit infos and the digest
std::pair<UnitInfos*, std::string> read_unit_info(const std::string& filename);

// Compilenv.write_unit_info: the .cmx file's bytes (magic, the marshaled
// unit infos, their BLAKE128 digest)
std::string write_unit_info(const UnitInfos& ui);

}  // namespace cppcaml::typing::cmx_format
