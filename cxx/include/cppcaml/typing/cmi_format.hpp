// Port of file_formats/cmi_format.ml (the read side) for the typing/ port
// (cxx/PORTING.md).  `read_cmi` decodes a .cmi's marshaled values straight
// into typing::Types -- the analogue of `input_value`: every marshaled block
// becomes exactly one C++ object, so sharing and cycles are preserved.
#pragma once

#include "cppcaml/omarshal.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cppcaml/typing/types.hpp"

namespace cppcaml::typing::instruct {
struct DebugEvent;
}

namespace cppcaml::typing::cmi_format {

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

// output_cmi filename cmi: writes the .cmi (via a temporary file) and
// returns its CRC, the raw BLAKE128 digest.  The signature must have been
// substituted for saving (Env.save_signature does it).
std::string output_cmi(const std::string& filename, const CmiInfos& cmi);
// the bytes output_cmi writes (magic, header, crcs, flags) and the crc
std::pair<std::string, std::string> output_cmi_bytes(const CmiInfos& cmi);

// the size of Marshal.to_buffer's output for (got, expected) (the Includemod
// error printer's is_big: the -error-size threshold)
std::size_t marshaled_size(const ModuleType* a, const ModuleType* b);
std::size_t marshaled_size(const ModtypeDeclaration* a, const ModtypeDeclaration* b);

// input_value of one generic value at [off], which it advances past it
// (Pparse's binary ASTs): an OValue graph with input_value's sharing -- one
// OValue per marshaled block, one string per marshaled string, position
// records with their identity (OValue::pos).  Throws marshal::Error.
// With [keep], the strings and position records (what a decoded parsetree
// keeps) are allocated there and the rest in the current zone, which the
// caller can then drop.
const OValue* input_ovalue(const std::uint8_t* data, std::size_t len, std::size_t& off, Zone* keep = nullptr);
// output_value of a generic value, with the .cmi Writer's sharing (strings
// by storage, positions and locations as it writes a typed tree's)
std::vector<std::uint8_t> output_ovalue(const OValue* v);

// The debugging events of a .cmo (Emitcode.to_file with -g): the marshaled
// `debug_event list`, whose typing values (types, Env summaries, Subst.t)
// go through the .cmi Writer.  (Not in cmi_format.mli.)
std::vector<std::uint8_t> marshal_debug_events(const std::vector<const instruct::DebugEvent*>& events);
// The same events as values, to marshal within a larger list (-pack's,
// whose other events are the members' read back).
// [unit_name]: the value of the unit's name string, when the caller shares
// it with values of its own (nullptr: a fresh one).
std::vector<omarshal::ValPtr> debug_event_values(const std::vector<const instruct::DebugEvent*>& events,
                                                 omarshal::ValPtr unit_name = nullptr);

}  // namespace cppcaml::typing::cmi_format
