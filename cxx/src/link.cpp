// Bytelink + Symtable: link .cmo/.cma objects into a runnable bytecode exec.
// Ports the essential half of bytecomp/{bytelink,symtable}.ml: pre-enter the
// predefined exceptions at their fixed global slots, then for each unit in link
// order resolve its relocations (global slots / C-primitive numbers / literals)
// and concatenate its code, append STOP, and write the CODE/PRIM/DATA sections
// plus the TOC trailer the runtime reads.
#include "cppcaml/link.hpp"
#include "cppcaml/builtin_prims.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

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
  // cu_imports: (interface name, crc) this unit was compiled against.  An empty
  // crc is `None` (no digest recorded); used for the link-time consistency check.
  std::vector<std::pair<std::string, std::string>> imports;
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
struct InputFile {
  bool archive;
  std::string path;   // source .cmo/.cma path (for consistency-check diagnostics)
  std::vector<Unit> units;
  // lib_dllibs from a .cma: (suffixed, name) pairs naming the C-stub shared
  // libraries the library needs (e.g. (true, "-lunixbyt")).  Drives the DLLS
  // section so the runtime dlopen()s them and resolves their C primitives.
  std::vector<std::pair<bool, std::string>> dllibs;
};

// Convert a decoded Marshal value (the literal Obj.t) to an omarshal value.
ValPtr conv(const m::Arena& a, std::size_t id, std::unordered_map<std::size_t, ValPtr>* memo = nullptr) {
  const m::Value& v = a[id];
  // input_value then output_value keeps what the marshaled value shared:
  // with a memo, one ValPtr per arena block / string
  if (memo && v.kind != m::Value::Kind::Int)
    if (auto it = memo->find(id); it != memo->end()) return it->second;
  ValPtr r;
  switch (v.kind) {
    case m::Value::Kind::Int:
      if (!v.custom_raw().empty())  // a boxed int32/int64/nativeint literal
        return omarshal::vcustom(v.custom_raw(), v.custom_bsize());
      return omarshal::vint(v.i);
    case m::Value::Kind::String: r = omarshal::vstr(v.str()); break;
    case m::Value::Kind::Double: r = omarshal::vdbl(v.d()); break;
    case m::Value::Kind::Block: {
      std::vector<ValPtr> fs;
      for (auto f : v.fields) fs.push_back(conv(a, f, memo));
      r = omarshal::vblock((int)v.tag, std::move(fs));
      break;
    }
    case m::Value::Kind::DoubleArray: r = omarshal::vdblarr(v.darr()); break;
  }
  if (!r) return omarshal::vint(0);
  if (memo) (*memo)[id] = r;
  return r;
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
  u.name = a[r.fields[0]].str();
  int cu_pos = (int)a[r.fields[1]].i;
  int cu_codesize = (int)a[r.fields[2]].i;
  u.code.assign(file.begin() + cu_pos, file.begin() + cu_pos + cu_codesize);
  // cu_required_compunits (field 5, a string list) and cu_force_link (field 7).
  if (r.fields.size() > 5)
    for (std::size_t e : list_elems(a, r.fields[5])) u.required.push_back(a[e].str());
  if (r.fields.size() > 7) u.force_link = a[r.fields[7]].i != 0;
  // cu_imports (field 4): (modname * crc option) list.  crc is `None` (immediate)
  // or `Some digest` (a block whose field 0 is the raw digest string).
  if (r.fields.size() > 4)
    for (std::size_t e : list_elems(a, r.fields[4])) {
      const m::Value& pair = a[e];
      if (pair.fields.size() < 2) continue;
      std::string name = a[pair.fields[0]].str(), crc;
      const m::Value& opt = a[pair.fields[1]];
      if (opt.kind == m::Value::Kind::Block && !opt.fields.empty())
        crc = a[opt.fields[0]].str();   // Some digest
      u.imports.emplace_back(std::move(name), std::move(crc));
    }
  for (std::size_t e : list_elems(a, r.fields[3])) {  // cu_reloc: (reloc_info * int) list
    const m::Value& pair = a[e];
    const m::Value& info = a[pair.fields[0]];
    int pos = (int)a[pair.fields[1]].i;
    Reloc rel; rel.pos = pos;
    switch (info.tag) {
      case 0: rel.k = Reloc::Literal; rel.lit = conv(a, info.fields[0]); break;
      case 1: rel.k = Reloc::GetCompunit; rel.name = a[info.fields[0]].str(); break;
      case 2: rel.k = Reloc::GetPredef; rel.name = a[info.fields[0]].str(); break;
      case 3: rel.k = Reloc::SetCompunit; rel.name = a[info.fields[0]].str(); break;
      case 4: rel.k = Reloc::Primitive; rel.name = a[info.fields[0]].str(); break;
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
  arena.finalize();
  InputFile in;
  in.path = path;
  if (magic == "Caml1999O038") {            // .cmo: one compilation_unit
    in.archive = false;
    in.units.push_back(parse_unit(arena, root, file));
  } else if (magic == "Caml1999A038") {     // .cma: library, field 0 = unit list
    in.archive = true;
    for (std::size_t cu : list_elems(arena, arena[root].fields[0]))
      in.units.push_back(parse_unit(arena, cu, file));
    // lib_dllibs = field 4: list of (suffixed:bool * string).
    if (arena[root].fields.size() > 4) {
      for (std::size_t e : list_elems(arena, arena[root].fields[4])) {
        const m::Value& d = arena[e];
        if (d.kind == m::Value::Kind::Block && d.fields.size() >= 2)
          in.dllibs.emplace_back(arena[d.fields[0]].i != 0, arena[d.fields[1]].str());
      }
    }
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

  //   Symtable.init enters the WHOLE runtime primitive table before any
  // relocation is patched (symtable.ml:279, `Array.iter set_prim_table
  // Runtimedef.builtin_primitives`), so a builtin's number is its index in
  // that table and never depends on what the program uses; a primitive the
  // runtime does not export -- a C stub -- is appended after them by of_prim,
  // which is num_of_prim's fallthrough.  Numbering only the used ones instead
  // gave every C_CALL a different operand AND a short PRIM section (55 names
  // where upstream writes all 482), which is why no linked program ever
  // matched byte-for-byte.  NOBUILTINPRIMS reverts to the old numbering.
  void init_prims() {
    if (std::getenv("NOBUILTINPRIMS")) return;
    for (const char* name : kBuiltinPrimitives) of_prim(name);
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
                     const std::string& out_path, const std::string& runtime_path,
                     bool link_everything, bool no_auto_link, bool write_linkmap) {
  Symtable st;
  st.init_prims();
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
      if (!fi->archive || ui->force_link || link_everything || missing.count(ui->name)) take(*ui);

  // Interface-consistency check (bytelink's Consistbl / check_consistency): every
  // unit records the CRC of each .cmi it was compiled against (cu_imports).  Two
  // linked units that assume different CRCs for the same interface were built
  // against incompatible versions of it -- the runtime layout they expect differs,
  // so linking them yields code that reads wrong fields and crashes.  Reject that
  // here with a clear error instead of letting the interpreter segfault later.
  {
    std::map<std::string, std::pair<std::string, std::string>> seen;  // intf -> (crc, file)
    for (const InputFile& f : files)
      for (const Unit& u : f.units) {
        if (!u.selected) continue;
        for (const auto& [name, crc] : u.imports) {
          if (crc.empty()) continue;                 // None: no assumption recorded
          auto it = seen.find(name);
          if (it == seen.end()) { seen[name] = {crc, f.path}; continue; }
          if (it->second.first != crc)
            throw std::runtime_error(
                "Files " + f.path + " and " + it->second.second +
                " make inconsistent assumptions over interface " + name);
        }
      }
  }

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

  //   SYMB: the global map, `output_value oc !global_table` (symtable.ml:321)
  // -- a `{cnt : int; tbl : int Global.Map.t}` record.  `Global.Map` is
  // `Map.Make`, so the value marshalled is the AVL TREE ITSELF (`Empty` = 0,
  // `Node {l; v; d; r; h}` = a 5-field block), whose shape depends on the
  // ORDER the keys were added in.  Slots are handed out by `enter` at insert
  // time, so slot order IS insertion order and replaying it rebuilds
  // upstream's exact tree -- which is also why this can only be written once
  // the slot numbering already matches (it does: DATA, the array indexed by
  // slot, is byte-identical).  A literal's slot comes from `incr`, which bumps
  // the counter WITHOUT adding to the table, and is skipped here.
  //   The key is `Glob_compunit of compunit | Glob_predef of predef` with both
  // payloads `[@@unboxed]` (cmo_format.mli:22,26), so a key is a 1-field block
  // holding the name string, tag 0 for a unit and tag 1 for a predef -- and
  // the map's ordering is the POLYMORPHIC compare, which orders by tag first
  // and then by the string.  NOSYMBSECT drops the section again.
  std::vector<std::pair<std::string, int>> gslots;   // (key, slot), slot order
  for (auto& [key, slot] : st.globals) gslots.push_back({key, slot});
  std::sort(gslots.begin(), gslots.end(),
            [](auto& a, auto& b) { return a.second < b.second; });
  struct Node {                                 // stdlib/map.ml's 'a t
    std::shared_ptr<Node> l, r;
    bool predef; std::string name;              // the key
    int d; int h;
  };
  using NPtr = std::shared_ptr<Node>;
  auto height = [](const NPtr& n) { return n ? n->h : 0; };
  // `compare` on Global.t: tag first (Glob_compunit = 0 before Glob_predef),
  // then the payload string.
  auto keycmp = [](bool ap, const std::string& an, bool bp,
                   const std::string& bn) {
    if (ap != bp) return ap ? 1 : -1;
    return an.compare(bn) < 0 ? -1 : (an == bn ? 0 : 1);
  };
  std::function<NPtr(NPtr, bool, const std::string&, int, NPtr)> create =
      [&](NPtr l, bool p, const std::string& v, int d, NPtr r) {
        auto n = std::make_shared<Node>();
        n->l = std::move(l); n->r = std::move(r);
        n->predef = p; n->name = v; n->d = d;
        n->h = (height(n->l) >= height(n->r) ? height(n->l) : height(n->r)) + 1;
        return n;
      };
  // map.ml's `bal`, verbatim -- the rotations decide the tree's shape and so
  // its marshalled bytes.
  std::function<NPtr(NPtr, bool, const std::string&, int, NPtr)> bal =
      [&](NPtr l, bool p, const std::string& v, int d, NPtr r) -> NPtr {
        int hl = height(l), hr = height(r);
        if (hl > hr + 2) {
          if (height(l->l) >= height(l->r))
            return create(l->l, l->predef, l->name, l->d,
                          create(l->r, p, v, d, std::move(r)));
          NPtr lr = l->r;
          return create(create(l->l, l->predef, l->name, l->d, lr->l),
                        lr->predef, lr->name, lr->d,
                        create(lr->r, p, v, d, std::move(r)));
        }
        if (hr > hl + 2) {
          if (height(r->r) >= height(r->l))
            return create(create(std::move(l), p, v, d, r->l),
                          r->predef, r->name, r->d, r->r);
          NPtr rl = r->l;
          return create(create(std::move(l), p, v, d, rl->l),
                        rl->predef, rl->name, rl->d,
                        create(rl->r, r->predef, r->name, r->d, r->r));
        }
        return create(std::move(l), p, v, d, std::move(r));
      };
  std::function<NPtr(NPtr, bool, const std::string&, int)> madd =
      [&](NPtr t, bool p, const std::string& v, int d) -> NPtr {
        if (!t) return create(nullptr, p, v, d, nullptr);
        int c = keycmp(p, v, t->predef, t->name);
        if (c == 0) return create(t->l, p, v, d, t->r);
        if (c < 0) return bal(madd(t->l, p, v, d), t->predef, t->name, t->d, t->r);
        return bal(t->l, t->predef, t->name, t->d, madd(t->r, p, v, d));
      };
  NPtr groot;
  for (auto& [key, slot] : gslots)
    groot = madd(groot, key.rfind("P:", 0) == 0, key.substr(2), slot);
  std::function<ValPtr(const NPtr&)> tval = [&](const NPtr& n) -> ValPtr {
    if (!n) return omarshal::vint(0);                       // Empty
    return omarshal::vblock(0, {tval(n->l),
                                omarshal::vblock(n->predef ? 1 : 0,
                                                 {omarshal::vstr(n->name)}),
                                omarshal::vint(n->d), tval(n->r),
                                omarshal::vint(n->h)});
  };
  std::vector<std::uint8_t> symb;
  if (!std::getenv("NOSYMBSECT"))
    symb = omarshal::marshal(
        omarshal::vblock(0, {omarshal::vint(st.cnt), tval(groot)}));

  //   CRCS: `output_value outchan (extract_crc_interfaces())` (bytelink.ml:647)
  // -- a `(modname * Digest.t option) list` over every interface any linked
  // unit imported.  Consistbl.extract sort_uniq's the names and then folds
  // with `::`, so the list comes out in DESCENDING name order (consistbl.ml:61).
  // A name recorded with no digest stays `None`.  NOCRCSSECT drops it again.
  std::map<std::string, std::string> crcs;   // name -> digest ("" = None)
  for (const InputFile& fi : files)
    for (const Unit& u : fi.units) {
      if (!u.selected) continue;
      for (auto& [nm, crc] : u.imports) {
        auto it = crcs.find(nm);
        if (it == crcs.end()) crcs.emplace(nm, crc);
        else if (it->second.empty()) it->second = crc;
      }
    }
  std::vector<ValPtr> crcl;                  // built in DESCENDING order
  for (auto it = crcs.rbegin(); it != crcs.rend(); ++it)
    crcl.push_back(omarshal::vblock(
        0, {omarshal::vstr(it->first),
            it->second.empty() ? omarshal::vint(0)
                               : omarshal::vblock(0, {omarshal::vstr(it->second)})}));
  std::vector<std::uint8_t> crcsec;
  if (!std::getenv("NOCRCSSECT"))
    crcsec = omarshal::marshal(omarshal::vlist(crcl));

  // PRIM: required C-primitive names, NUL-terminated, in numbering order.
  std::vector<std::uint8_t> prim;
  for (auto& name : st.prim_order) { prim.insert(prim.end(), name.begin(), name.end()); prim.push_back(0); }

  // DLLS: the C-stub shared libraries to dlopen at startup (from .cma dllibs).
  // Each entry is `('-' if suffixed else ':') name '\0'`; a suffixed `-l<x>`
  // dllib becomes `-dll<x>` (bytelink's process_dllib).  The unmodified runtime
  // reads DLLS, loads each library, then resolves the PRIM names against them.
  std::vector<std::uint8_t> dlls;
  {
    std::vector<std::pair<bool, std::string>> seen;
    for (const InputFile& fi : files)
      for (const auto& [suffixed, name] : no_auto_link ? decltype(fi.dllibs){} : fi.dllibs) {
        bool out_suffixed = suffixed;
        std::string out_name = name;
        if (suffixed && name.rfind("-l", 0) == 0) out_name = "dll" + name.substr(2);
        auto key = std::make_pair(out_suffixed, out_name);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        dlls.push_back(out_suffixed ? '-' : ':');
        dlls.insert(dlls.end(), out_name.begin(), out_name.end());
        dlls.push_back(0);
      }
  }

  // ---- assemble: [shebang] CODE DLLS PRIM DATA SYMB CRCS + TOC + trailer ----
  std::vector<std::uint8_t> out;
  if (!runtime_path.empty()) {
    std::string sh = "#!" + runtime_path + "\n";
    out.insert(out.end(), sh.begin(), sh.end());
  }
  out.insert(out.end(), code.begin(), code.end());
  if (!dlls.empty()) out.insert(out.end(), dlls.begin(), dlls.end());
  out.insert(out.end(), prim.begin(), prim.end());
  out.insert(out.end(), data.begin(), data.end());
  out.insert(out.end(), symb.begin(), symb.end());
  out.insert(out.end(), crcsec.begin(), crcsec.end());
  auto section = [&](const char* nm, std::size_t len) {
    out.insert(out.end(), nm, nm + 4); put_be32(out, (std::uint32_t)len);
  };
  // TOC order must match the body layout above.
  int nsec = 3;
  section("CODE", code.size());
  if (!dlls.empty()) { section("DLLS", dlls.size()); ++nsec; }
  section("PRIM", prim.size());
  section("DATA", data.size());
  if (!symb.empty()) { section("SYMB", symb.size()); ++nsec; }
  if (!crcsec.empty()) { section("CRCS", crcsec.size()); ++nsec; }
  put_be32(out, nsec);  // number of sections
  const char* magic = "Caml1999X038";
  out.insert(out.end(), magic, magic + 12);

  std::ofstream f(out_path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + out_path);
  f.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());

  // The module map sidecar (best-effort; a failure to write it is non-fatal).
  if (write_linkmap) {
    std::ofstream lm(out_path + ".linkmap");
    if (lm) lm << linkmap;
  }
}

// ---- `ocamlc -a`: build a .cma library ------------------------------------
// A .cma is `magic(12) + be32(toc_offset) + <concatenated unit code> + marshaled
// library`; each unit's cu_pos is rewritten to its code offset within the file.
void archive(const std::vector<std::string>& cmos, const std::string& out_path, bool link_everything) {
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
    arena.finalize();
    auto field = [&](int i) { return (long long)arena[arena[root].fields[i]].i; };
    long long cu_pos = field(1), codesize = field(2);
    std::unordered_map<std::size_t, ValPtr> memo;
    ValPtr cu = conv(arena, root, &memo);
    // Bytelibrarian.copy_compunit: the code, then the debug and hint
    // sections, each at its new position
    cu->fields[1] = omarshal::vint((long long)out.size());  // cu_pos
    if (link_everything) cu->fields[7] = omarshal::vint(1);  // cu_force_link
    out.insert(out.end(), file.begin() + cu_pos, file.begin() + cu_pos + codesize);
    if (arena[root].fields.size() > 11) {
      long long debug = field(8), debugsize = field(9), hint = field(10), hintsize = field(11);
      if (debug > 0) {
        cu->fields[8] = omarshal::vint((long long)out.size());
        out.insert(out.end(), file.begin() + debug, file.begin() + debug + debugsize);
      }
      if (hint > 0) {
        cu->fields[10] = omarshal::vint((long long)out.size());
        out.insert(out.end(), file.begin() + hint, file.begin() + hint + hintsize);
      }
    }
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

}  // namespace cppcaml::link
