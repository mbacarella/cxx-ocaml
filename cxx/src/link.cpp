// Bytelink + Symtable: link .cmo/.cma objects into a runnable bytecode exec.
// Ports the essential half of bytecomp/{bytelink,symtable}.ml: pre-enter the
// predefined exceptions at their fixed global slots, then for each unit in link
// order resolve its relocations (global slots / C-primitive numbers / literals)
// and concatenate its code, append STOP, and write the CODE/PRIM/DATA sections
// plus the TOC trailer the runtime reads.
#include "cppcaml/link.hpp"

#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "cppcaml/marshal.hpp"
#include "cppcaml/omarshal.hpp"

namespace cppcaml::link {
namespace {
namespace m = cppcaml::marshal;
using omarshal::ValPtr;

constexpr int OP_STOP = 143;  // opcodes.h: STOP

// The runtime's predefined exceptions, in the fixed slot order it expects
// (runtimedef.ml builtin_exceptions).
const char* kBuiltinExceptions[] = {
    "Out_of_memory", "Sys_error", "Failure", "Invalid_argument", "End_of_file",
    "Division_by_zero", "Not_found", "Match_failure", "Stack_overflow",
    "Sys_blocked_io", "Assert_failure", "Undefined_recursive_module", "Todo"};

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t o) {
  return ((std::uint32_t)b[o] << 24) | ((std::uint32_t)b[o + 1] << 16) |
         ((std::uint32_t)b[o + 2] << 8) | (std::uint32_t)b[o + 3];
}

// Relocation as read from a unit descriptor.
struct Reloc {
  enum K { Literal, GetCompunit, GetPredef, SetCompunit, Primitive } k;
  std::string name;   // non-literal
  ValPtr lit;         // literal value
  int pos;
};
struct Unit {
  std::string name;
  std::vector<std::uint8_t> code;
  std::vector<Reloc> relocs;
};

// Convert a decoded Marshal value (the literal Obj.t) to an omarshal value.
ValPtr conv(const m::Arena& a, std::size_t id) {
  const m::Value& v = a[id];
  switch (v.kind) {
    case m::Value::Kind::Int:
      if (!v.custom_raw.empty())  // a boxed int32/int64/nativeint literal
        return omarshal::vcustom(v.custom_raw, 1 + (v.custom_bsize + 7) / 8);
      return omarshal::vint(v.i);
    case m::Value::Kind::String: return omarshal::vstr(v.str);
    case m::Value::Kind::Double: return omarshal::vdbl(v.d);
    case m::Value::Kind::Block: {
      std::vector<ValPtr> fs;
      for (auto f : v.fields) fs.push_back(conv(a, f));
      return omarshal::vblock((int)v.tag, std::move(fs));
    }
    case m::Value::Kind::DoubleArray:
      return omarshal::vdblarr(v.darr);
  }
  return omarshal::vint(0);
}

// Walk an OCaml list value (cons blocks / 0) collecting element arena ids.
std::vector<std::size_t> list_elems(const m::Arena& a, std::size_t id) {
  std::vector<std::size_t> out;
  while (a[id].kind == m::Value::Kind::Block && a[id].fields.size() == 2) {
    out.push_back(a[id].fields[0]);
    id = a[id].fields[1];
  }
  return out;
}

// Parse a compilation_unit block into a Unit, reading its code from `file`.
Unit parse_unit(const m::Arena& a, std::size_t cu, const std::vector<std::uint8_t>& file) {
  const m::Value& r = a[cu];
  Unit u;
  u.name = a[r.fields[0]].str;
  int cu_pos = (int)a[r.fields[1]].i;
  int cu_codesize = (int)a[r.fields[2]].i;
  u.code.assign(file.begin() + cu_pos, file.begin() + cu_pos + cu_codesize);
  for (std::size_t e : list_elems(a, r.fields[3])) {  // cu_reloc: (reloc_info * int) list
    const m::Value& pair = a[e];
    const m::Value& info = a[pair.fields[0]];
    int pos = (int)a[pair.fields[1]].i;
    Reloc rel; rel.pos = pos;
    switch (info.tag) {
      case 0: rel.k = Reloc::Literal; rel.lit = conv(a, info.fields[0]); break;
      case 1: rel.k = Reloc::GetCompunit; rel.name = a[info.fields[0]].str; break;
      case 2: rel.k = Reloc::GetPredef; rel.name = a[info.fields[0]].str; break;
      case 3: rel.k = Reloc::SetCompunit; rel.name = a[info.fields[0]].str; break;
      case 4: rel.k = Reloc::Primitive; rel.name = a[info.fields[0]].str; break;
      default: continue;
    }
    u.relocs.push_back(std::move(rel));
  }
  return u;
}

std::vector<Unit> read_objects(const std::string& path) {
  std::vector<std::uint8_t> file = read_file(path);
  if (file.size() < 16) throw std::runtime_error("not an object file: " + path);
  std::string magic((const char*)file.data(), 12);
  std::size_t off = be32(file, 12);
  m::Arena arena;
  std::size_t root = m::read_value(file.data(), file.size(), off, arena);
  std::vector<Unit> units;
  if (magic == "Caml1999O038") {            // .cmo: one compilation_unit
    units.push_back(parse_unit(arena, root, file));
  } else if (magic == "Caml1999A038") {     // .cma: library, field 0 = unit list
    for (std::size_t cu : list_elems(arena, arena[root].fields[0]))
      units.push_back(parse_unit(arena, cu, file));
  } else {
    throw std::runtime_error("unknown object magic in " + path);
  }
  return units;
}

// ---- Symtable ----
struct Symtable {
  std::map<std::string, int> globals;   // key "C:name"/"P:name" -> slot
  int cnt = 0;
  std::map<std::string, int> prims;     // C primitive -> number
  std::vector<std::string> prim_order;
  std::vector<std::pair<int, ValPtr>> literals;  // (slot, value)

  static std::string gkey(bool predef, const std::string& n) { return (predef ? "P:" : "C:") + n; }
  int enter_global(const std::string& key) {
    auto it = globals.find(key);
    if (it != globals.end()) return it->second;
    int n = cnt++; globals[key] = n; return n;
  }
  int find_global(const std::string& key) {
    auto it = globals.find(key);
    return it == globals.end() ? -1 : it->second;
  }
  int literal_slot(ValPtr v) { int n = cnt++; literals.push_back({n, std::move(v)}); return n; }
  int of_prim(const std::string& name) {
    auto it = prims.find(name);
    if (it != prims.end()) return it->second;
    int n = (int)prim_order.size(); prims[name] = n; prim_order.push_back(name); return n;
  }

  void init_predef() {
    int i = 0;
    for (const char* name : kBuiltinExceptions) {
      int slot = enter_global(gkey(true, name));
      // the exception value: (object_tag) [ name ; -i-1 ]
      literals.push_back({slot, omarshal::vblock(248, {omarshal::vstr(name), omarshal::vint(-i - 1)})});
      ++i;
    }
  }
};

void patch(std::vector<std::uint8_t>& code, int pos, int n) {
  code[pos] = n & 0xFF; code[pos + 1] = (n >> 8) & 0xFF;
  code[pos + 2] = (n >> 16) & 0xFF; code[pos + 3] = (n >> 24) & 0xFF;
}

void put_be32(std::vector<std::uint8_t>& v, std::uint32_t n) {
  v.push_back(n >> 24); v.push_back(n >> 16); v.push_back(n >> 8); v.push_back(n);
}

}  // namespace

void link_executable(const std::vector<std::string>& inputs,
                     const std::string& out_path, const std::string& runtime_path) {
  Symtable st;
  st.init_predef();

  // Gather units in link order, then resolve relocations and concatenate code.
  std::vector<std::uint8_t> code;
  for (const std::string& in : inputs) {
    for (Unit u : read_objects(in)) {
      for (const Reloc& r : u.relocs) {
        int n = 0;
        switch (r.k) {
          case Reloc::Literal: n = st.literal_slot(r.lit); break;
          case Reloc::GetCompunit: n = st.find_global(Symtable::gkey(false, r.name)); break;
          case Reloc::GetPredef: n = st.find_global(Symtable::gkey(true, r.name)); break;
          case Reloc::SetCompunit: n = st.enter_global(Symtable::gkey(false, r.name)); break;
          case Reloc::Primitive: n = st.of_prim(r.name); break;
        }
        if (n < 0)
          throw std::runtime_error("undefined global referenced by " + u.name + ": " + r.name);
        patch(u.code, r.pos, n);
      }
      code.insert(code.end(), u.code.begin(), u.code.end());
    }
  }
  // Append STOP (4-byte little-endian opcode word).
  code.push_back(OP_STOP); code.push_back(0); code.push_back(0); code.push_back(0);

  // DATA: the initial global table (array tag 0, size cnt), literals filled in.
  std::vector<ValPtr> globals(st.cnt, omarshal::vint(0));
  for (auto& [slot, v] : st.literals) globals[slot] = v;
  std::vector<std::uint8_t> data = omarshal::marshal(omarshal::vblock(0, std::move(globals)));

  // PRIM: required C-primitive names, NUL-terminated, in numbering order.
  std::vector<std::uint8_t> prim;
  for (auto& name : st.prim_order) { prim.insert(prim.end(), name.begin(), name.end()); prim.push_back(0); }

  // ---- assemble the executable: [shebang] CODE PRIM DATA + TOC + trailer ----
  std::vector<std::uint8_t> out;
  if (!runtime_path.empty()) {
    std::string sh = "#!" + runtime_path + "\n";
    out.insert(out.end(), sh.begin(), sh.end());
  }
  out.insert(out.end(), code.begin(), code.end());
  out.insert(out.end(), prim.begin(), prim.end());
  out.insert(out.end(), data.begin(), data.end());
  auto section = [&](const char* nm, std::size_t len) {
    out.insert(out.end(), nm, nm + 4); put_be32(out, (std::uint32_t)len);
  };
  section("CODE", code.size());
  section("PRIM", prim.size());
  section("DATA", data.size());
  put_be32(out, 3);  // number of sections
  const char* magic = "Caml1999X038";
  out.insert(out.end(), magic, magic + 12);

  std::ofstream f(out_path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + out_path);
  f.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
}

}  // namespace cppcaml::link
