// Port of file_formats/cmo_format.mli, as the linker and the librarian read
// object files: a compilation_unit / library descriptor decoded with
// input_value into omarshal values that keep what the marshaled data shared
// (one value per decoded object), so writing a descriptor, a literal or a
// debug-event list back (output_value) gives OCaml's bytes.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cppcaml/omarshal.hpp"

namespace cppcaml::typing::cmo_format {

using V = omarshal::ValPtr;

// compilation_unit's fields (a mutable record: the librarian and the linker
// rewrite cu_pos, cu_force_link, cu_debug in place)
enum CuField {
  cu_name, cu_pos, cu_codesize, cu_reloc, cu_imports, cu_required_compunits, cu_primitives, cu_force_link,
  cu_debug, cu_debugsize
};
// library's fields
enum LibField { lib_units, lib_custom, lib_ccobjs, lib_ccopts, lib_dllibs };

// reloc_info
struct Reloc {
  enum class K { Reloc_literal, Reloc_getcompunit, Reloc_getpredef, Reloc_setcompunit, Reloc_primitive } k;
  V literal;         // Reloc_literal: the Obj.t
  std::string name;  // the others (Compunit / Predef_exn / primitive name)
  V name_obj;        // ... the string object itself
  long pos;
};

// a compilation_unit value
struct CompUnit {
  V v;
  long int_field(CuField f) const { return static_cast<long>(v->fields[f].int_value()); }
  void set_int_field(CuField f, long n) { v->fields[f] = omarshal::vint(n); }
  std::string name() const { return v->fields[cu_name]->str(); }
  std::vector<Reloc> relocs() const;
  std::vector<std::pair<std::string, std::optional<std::string>>> imports() const;
  // cu_imports as the string objects: (name, crc) with crc null for None
  std::vector<std::pair<V, V>> import_objs() const;
  std::vector<std::string> required_compunits() const;
  std::vector<std::string> primitives() const;
  bool force_link() const { return v->fields[cu_force_link].int_value() != 0; }
};

// the elements of an OCaml list value
std::vector<V> list_elems(const V& l);

// An object file read in memory; input_value at an offset decodes one
// marshaled value (a fresh sharing context per call, as input_value).
class ObjFile {
 public:
  // raises arg::SysError when the file cannot be read
  explicit ObjFile(const std::string& path);
  const std::string& path() const { return path_; }
  long size() const { return static_cast<long>(bytes_.size()); }
  // really_input_string at pos: std::nullopt at end of file
  std::optional<std::string> read_string(long pos, long len) const;
  // input_binary_int at pos: std::nullopt at end of file
  std::optional<long> read_binary_int(long pos) const;
  // input_value at pos, advancing pos past the value (raises EndOfFile when
  // truncated)
  V input_value(long& pos) const;
  // the bytes output_value would write for that value (marshal::raw_value),
  // without decoding it
  std::vector<std::uint8_t> raw_value(long& pos) const;
  const std::uint8_t* data() const { return bytes_.data(); }

 private:
  std::string path_;
  std::vector<std::uint8_t> bytes_;
};
struct EndOfFile {};

}  // namespace cppcaml::typing::cmo_format
