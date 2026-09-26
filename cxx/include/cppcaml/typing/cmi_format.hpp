// Port of file_formats/cmi_format.ml (the read side) for the typing/ port
// (TYPECHECKER.md).  `read_cmi` decodes a .cmi's marshaled values straight
// into typing::Types -- the analogue of `input_value`: every marshaled block
// becomes exactly one C++ object, so sharing and cycles are preserved.
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cppcaml/typing/types.hpp"

namespace cppcaml::typing::cmi_format {

inline constexpr const char* cmi_magic_number = "Caml1999I038";

struct PersFlag {
  enum class Kind : std::uint8_t { Rectypes, Alerts, Opaque };
  Kind kind;
  StrMap<std::string_view> alerts;  // Alerts
};

struct CmiInfos {
  std::string_view cmi_name;
  Signature cmi_sign;
  // crcs = (modname * Digest.BLAKE128.t option) list; the digest as raw bytes
  std::vector<std::pair<std::string, std::optional<std::string>>> cmi_crcs;
  std::vector<PersFlag> cmi_flags;
};

struct Error : std::runtime_error {
  enum class Kind { Not_an_interface, Wrong_version_interface, Corrupted_interface };
  Kind kind;
  std::string filename;
  std::string older_newer;  // Wrong_version_interface
  Error(Kind k, std::string f, std::string on = {});
};

// Allocates in the current zone (typing::zone()).
CmiInfos read_cmi(const std::string& filename);

}  // namespace cppcaml::typing::cmi_format
