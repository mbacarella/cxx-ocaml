// Bytelink + Symtable: link .cmo/.cma objects into a runnable bytecode exec.
// Ports the essential half of bytecomp/{bytelink,symtable}.ml: pre-enter the
// predefined exceptions at their fixed global slots, then for each unit in link
// order resolve its relocations (global slots / C-primitive numbers / literals)
// and concatenate its code, append STOP, and write the CODE/PRIM/DATA sections
// plus the TOC trailer the runtime reads.
#include "cppcaml/link.hpp"
#include "cppcaml/cmi.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
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
  bool force_link = false;             // cu_force_link: link even if unreferenced
  std::vector<std::string> required;   // cu_required_compunits (pack submodules)
  bool selected = false;               // chosen by the reachability pass
  // Units this unit references / provides (from GetCompunit / SetCompunit relocs).
  std::vector<std::string> req_units() const {
    std::vector<std::string> r = required;
    for (const Reloc& x : relocs) if (x.k == Reloc::GetCompunit) r.push_back(x.name);
    return r;
  }
  std::vector<std::string> provides() const {
    std::vector<std::string> p;
    for (const Reloc& x : relocs) if (x.k == Reloc::SetCompunit) p.push_back(x.name);
    return p;
  }
};
// A linker input: a .cmo (one unit, always linked) or a .cma (link only the
// units transitively required).
struct InputFile { bool archive; std::vector<Unit> units; };

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
  // cu_required_compunits (field 5, a string list) and cu_force_link (field 7).
  if (r.fields.size() > 5)
    for (std::size_t e : list_elems(a, r.fields[5])) u.required.push_back(a[e].str);
  if (r.fields.size() > 7) u.force_link = a[r.fields[7]].i != 0;
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

InputFile read_objects(const std::string& path) {
  std::vector<std::uint8_t> file = read_file(path);
  if (file.size() < 16) throw std::runtime_error("not an object file: " + path);
  std::string magic((const char*)file.data(), 12);
  std::size_t off = be32(file, 12);
  m::Arena arena;
  std::size_t root = m::read_value(file.data(), file.size(), off, arena);
  InputFile in;
  if (magic == "Caml1999O038") {            // .cmo: one compilation_unit
    in.archive = false;
    in.units.push_back(parse_unit(arena, root, file));
  } else if (magic == "Caml1999A038") {     // .cma: library, field 0 = unit list
    in.archive = true;
    for (std::size_t cu : list_elems(arena, arena[root].fields[0]))
      in.units.push_back(parse_unit(arena, cu, file));
  } else {
    throw std::runtime_error("unknown object magic in " + path);
  }
  return in;
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

  // Read every input file (each is a .cmo or a .cma).
  std::vector<InputFile> files;
  for (const std::string& in : inputs) files.push_back(read_objects(in));

  // Pass 1 -- reachability (bytelink's scan_file/Linkdeps).  Fold right-to-left:
  // a .cmo is always linked; a .cma unit is linked only if force_link or its
  // name is currently required.  Each linked unit's requires become required and
  // its provides satisfied.  Because the link list (and a .cma's units) are in
  // dependency order, the reverse fold propagates needs from users to providers.
  std::set<std::string> missing;
  auto take = [&](Unit& u) {
    u.selected = true;
    for (auto& r : u.req_units()) missing.insert(r);
    for (auto& p : u.provides()) missing.erase(p);
  };
  for (auto fi = files.rbegin(); fi != files.rend(); ++fi)
    for (auto ui = fi->units.rbegin(); ui != fi->units.rend(); ++ui)
      if (!fi->archive || ui->force_link || missing.count(ui->name)) take(*ui);

  // Pass 2 -- resolve relocations of the selected units, in original link order,
  // and concatenate their code.  Alongside, record each unit's start (in code
  // WORDS) and name into a `<output>.linkmap` sidecar, so a runtime crash dump
  // (CPPCAML_FIELDTRACE) can map a bytecode offset back to a module + offset; see
  // tools/cppcaml-resolve.sh.
  std::vector<std::uint8_t> code;
  std::string linkmap;
  for (InputFile& f : files) {
    for (Unit& u : f.units) {
      if (!u.selected) continue;
      linkmap += std::to_string(code.size() / 4) + " " + u.name + "\n";
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

  // The module map sidecar (best-effort; a failure to write it is non-fatal).
  std::ofstream lm(out_path + ".linkmap");
  if (lm) lm << linkmap;
}

// ---- `ocamlc -a`: build a .cma library ------------------------------------
// A .cma is `magic(12) + be32(toc_offset) + <concatenated unit code> + marshaled
// library`; each unit's cu_pos is rewritten to its code offset within the file.
void archive(const std::vector<std::string>& cmos, const std::string& out_path) {
  std::vector<std::uint8_t> out;
  const char* magic = "Caml1999A038";
  out.insert(out.end(), magic, magic + 12);
  std::size_t depl = out.size();
  put_be32(out, 0);  // placeholder for the library-descriptor offset
  std::vector<ValPtr> units;
  for (const std::string& path : cmos) {
    std::vector<std::uint8_t> file = read_file(path);
    if (file.size() < 16 || std::string((const char*)file.data(), 12) != "Caml1999O038")
      throw std::runtime_error(path + ": not a .cmo (cannot -a a .cma input)");
    std::size_t off = be32(file, 12);
    m::Arena arena;
    std::size_t root = m::read_value(file.data(), file.size(), off, arena);
    int cu_pos = (int)arena[arena[root].fields[1]].i;
    int codesize = (int)arena[arena[root].fields[2]].i;
    ValPtr cu = conv(arena, root);
    cu->fields[1] = omarshal::vint((long long)out.size());  // rewrite cu_pos
    out.insert(out.end(), file.begin() + cu_pos, file.begin() + cu_pos + codesize);
    units.push_back(cu);
  }
  // library = { lib_units; lib_custom=false; lib_ccobjs=[]; lib_ccopts=[]; lib_dllibs=[] }
  ValPtr lib = omarshal::vblock(0, {omarshal::vlist(units), omarshal::vint(0),
                                    omarshal::vint(0), omarshal::vint(0), omarshal::vint(0)});
  std::vector<std::uint8_t> lib_bytes = omarshal::marshal(lib);
  std::uint32_t toc = (std::uint32_t)out.size();
  out.insert(out.end(), lib_bytes.begin(), lib_bytes.end());
  out[depl] = toc >> 24; out[depl + 1] = toc >> 16; out[depl + 2] = toc >> 8; out[depl + 3] = toc;
  std::ofstream f(out_path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + out_path);
  f.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
}

// ---- `ocamlc -pack`: consolidate .cmo members into one packed unit ---------
// Concatenate each member's code, renaming its global M -> Pack.M (and sibling
// references likewise), then append the structure block that builds the Pack
// record `{M0; ..; Mn-1}` from those globals and SETGLOBALs Pack.  Members must
// appear in dependency order (a sibling reference may only be backward).
void pack(const std::vector<std::string>& cmos, const std::string& pack_name,
          const std::string& out_path) {
  // opcodes (runtime/caml/opcodes.h order): see cmo.cpp's enum.
  enum { PUSH = 9, GETGLOBAL = 53, SETGLOBAL = 57, ATOM0 = 58, MAKEBLOCK = 62 };
  std::vector<std::uint8_t> code;
  std::vector<ValPtr> reloc_entries;  // (reloc_info * int) list elements
  std::vector<ValPtr> primitives;
  std::set<std::string> members, processed;
  std::vector<std::string> member_names;
  // An interface-only member (a `.cmi` in the pack list, like ocamlbuild's
  // signatures.cmi) is PM_intf: it contributes its signature to the packed
  // .cmi but no code and no slot in the structure block.
  auto is_intf = [](const std::string& p) {
    return p.size() >= 4 && p.compare(p.size() - 4, 4, ".cmi") == 0;
  };
  for (const std::string& path : cmos) {
    if (is_intf(path)) continue;
    InputFile in = read_objects(path);
    if (in.archive || in.units.size() != 1)
      throw std::runtime_error(path + ": -pack expects a single .cmo");
    members.insert(in.units[0].name);
    member_names.push_back(in.units[0].name);
  }
  auto reloc_info = [&](const Reloc& r) -> ValPtr {
    switch (r.k) {
      case Reloc::Literal:     return omarshal::vblock(0, {r.lit});
      case Reloc::GetCompunit: return omarshal::vblock(1, {omarshal::vstr(r.name)});
      case Reloc::GetPredef:   return omarshal::vblock(2, {omarshal::vstr(r.name)});
      case Reloc::SetCompunit: return omarshal::vblock(3, {omarshal::vstr(r.name)});
      case Reloc::Primitive:   return omarshal::vblock(4, {omarshal::vstr(r.name)});
    }
    return omarshal::vint(0);
  };
  auto add_reloc = [&](Reloc r, int pos) {
    reloc_entries.push_back(omarshal::vblock(0, {reloc_info(r), omarshal::vint(pos)}));
  };
  // Members: copy code, rebase + rename relocations.
  for (const std::string& path : cmos) {
    if (is_intf(path)) continue;  // PM_intf: no code
    Unit u = read_objects(path).units[0];
    int base = (int)code.size();
    code.insert(code.end(), u.code.begin(), u.code.end());
    for (Reloc r : u.relocs) {
      if (r.k == Reloc::SetCompunit && members.count(r.name)) r.name = pack_name + "." + r.name;
      else if (r.k == Reloc::GetCompunit && members.count(r.name)) {
        if (!processed.count(r.name))
          throw std::runtime_error(path + ": forward reference to pack member " + r.name);
        r.name = pack_name + "." + r.name;
      } else if (r.k == Reloc::Primitive)
        primitives.push_back(omarshal::vstr(r.name));
      add_reloc(r, base + r.pos);
    }
    processed.insert(u.name);
  }
  // Structure block: `setglobal Pack (makeblock N [getglobal Pack.M0; ..])`.
  auto word = [&](int op) {
    code.push_back((std::uint8_t)op); code.push_back(0); code.push_back(0); code.push_back(0);
  };
  auto operand0 = [&] { for (int k = 0; k < 4; ++k) code.push_back(0); };  // reloc placeholder
  int N = (int)member_names.size();
  for (int i = N - 1; i >= 0; --i) {                 // push args N-1..1, acc = arg 0
    word(GETGLOBAL);
    add_reloc(Reloc{Reloc::GetCompunit, pack_name + "." + member_names[i], nullptr, (int)code.size()}, (int)code.size());
    operand0();
    if (i != 0) word(PUSH);
  }
  if (N == 0) word(ATOM0);
  else { word(MAKEBLOCK);
         auto put_int = [&](int n) { code.push_back(n & 0xFF); code.push_back((n >> 8) & 0xFF);
                                     code.push_back((n >> 16) & 0xFF); code.push_back((n >> 24) & 0xFF); };
         put_int(N); put_int(0); }
  word(SETGLOBAL);
  add_reloc(Reloc{Reloc::SetCompunit, pack_name, nullptr, (int)code.size()}, (int)code.size());
  operand0();

  // Write the packed .cmo (mirror cmo.cpp's compilation_unit layout).
  const int pos_code = 16;  // magic(12) + 4-byte descriptor offset
  ValPtr compunit = omarshal::vblock(0, {
      omarshal::vstr(pack_name), omarshal::vint(pos_code), omarshal::vint((long long)code.size()),
      omarshal::vlist(reloc_entries), omarshal::vint(0), omarshal::vint(0),
      omarshal::vlist(primitives), omarshal::vint(0), omarshal::vint(0), omarshal::vint(0),
      omarshal::vint(0), omarshal::vint(0)});
  std::vector<std::uint8_t> cu_bytes = omarshal::marshal(compunit);
  std::vector<std::uint8_t> out;
  const char* magic = "Caml1999O038";
  out.insert(out.end(), magic, magic + 12);
  put_be32(out, (std::uint32_t)(pos_code + code.size()));
  out.insert(out.end(), code.begin(), code.end());
  out.insert(out.end(), cu_bytes.begin(), cu_bytes.end());
  std::ofstream f(out_path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + out_path);
  f.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());

  // Emit the packed .cmi: each member's own .cmi signature wrapped one level
  // deeper as `module <Member> : sig ... end`, so dependents can compile
  // against `Pack.Member.x`.  A member compiled without a .cmi is skipped.
  std::vector<std::string> member_cmis;
  for (const auto& c : cmos) {
    std::string mc = c.substr(0, c.find_last_of('.')) + ".cmi";
    std::ifstream probe(mc, std::ios::binary);
    if (probe) member_cmis.push_back(mc);
  }
  if (member_cmis.size() == cmos.size()) {
    std::string cmi_out = out_path.substr(0, out_path.find_last_of('.')) + ".cmi";
    cppcaml::cmi::cmiw::write_packed_cmi(cmi_out, pack_name, member_cmis);
  }
}

}  // namespace cppcaml::link
