// Port of bytecomp/bytepackager.ml (cxx/PORTING.md stage 10): -pack.
//
// The member units' descriptors (and debug events) are read back
// with the Marshal reader and re-marshaled as omarshal values that keep the
// reader's sharing (one value per decoded object), combined with the values
// the package's own code produces (Emitcode.to_packed_file) in one
// marshaling context: the packed descriptor shares strings exactly where
// OCaml's does.
#include "cppcaml/typing/bytepackager.hpp"
#include "cppcaml/typing/config.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include "cppcaml/marshal.hpp"
#include "cppcaml/omarshal.hpp"
#include "cppcaml/typing/bytegen.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/cmi_format.hpp"
#include "cppcaml/typing/emitcode.hpp"
#include "cppcaml/typing/persistent_env.hpp"
#include "cppcaml/typing/printlambda.hpp"
#include "cppcaml/typing/simplif.hpp"
#include "cppcaml/typing/translmod.hpp"
#include "cppcaml/typing/typemod.hpp"

namespace cppcaml::typing::bytepackager {
namespace {

namespace fs = std::filesystem;
namespace m = cppcaml::marshal;
namespace o = cppcaml::omarshal;
using V = o::ValPtr;


// compilation_unit fields (Cmo_format)
enum CuField {
  cu_name, cu_pos, cu_codesize, cu_reloc, cu_imports, cu_required_compunits, cu_primitives, cu_force_link,
  cu_debug, cu_debugsize
};

std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t o) {
  return (static_cast<std::uint32_t>(b[o]) << 24) | (static_cast<std::uint32_t>(b[o + 1]) << 16) |
         (static_cast<std::uint32_t>(b[o + 2]) << 8) | static_cast<std::uint32_t>(b[o + 3]);
}

// A value read back from a member file, re-marshaled with its sharing: one
// omarshal value per decoded object.
class Reader {
 public:
  explicit Reader(const m::Arena& a) : a_(a) {}
  V operator()(std::size_t id) {
    const m::Value& v = a_[id];
    if (v.kind == m::Value::Kind::Int && v.custom_raw().empty()) return o::vint(v.i);
    auto it = memo_.find(id);
    if (it != memo_.end()) return it->second;
    V r;
    switch (v.kind) {
      case m::Value::Kind::Int: {  // a boxed int32 / int64 / nativeint
        const std::string& raw = v.custom_raw();
        if (raw.size() > 2 && raw[1] == '_' && raw[2] == 'n') r = o::vcustom2(raw, 4, 8);
        else r = o::vcustom2(raw, v.custom_bsize(), v.custom_bsize());
        break;
      }
      case m::Value::Kind::String: r = o::vstr(v.str()); break;
      case m::Value::Kind::Double: r = o::vdbl(v.d()); break;
      case m::Value::Kind::DoubleArray: r = o::vdblarr(v.darr()); break;
      case m::Value::Kind::Block: {
        // registered before its fields: a cycle closes on it
        r = o::vblock(static_cast<int>(v.tag), {});
        memo_[id] = r;
        for (std::size_t f : v.fields) r->fields.push_back((*this)(f));
        return r;
      }
    }
    memo_[id] = r;
    return r;
  }

 private:
  const m::Arena& a_;
  std::unordered_map<std::size_t, V> memo_;
};

std::vector<std::size_t> list_elems(const m::Arena& a, std::size_t id) {
  std::vector<std::size_t> out;
  while (a[id].kind == m::Value::Kind::Block && a[id].fields.size() == 2) {
    out.push_back(a[id].fields[0]);
    id = a[id].fields[1];
  }
  return out;
}

// type pack_member_kind = PM_intf | PM_impl of compilation_unit
struct Member {
  std::string pm_file;
  std::string_view pm_name;         // Unit_info.Artifact.modname member
  std::string_view pm_packed_ident; // targetname ^ "." ^ member_name
  bool intf = false;
  // PM_impl: the file, its decoded descriptor
  std::vector<std::uint8_t> bytes;
  std::unique_ptr<m::Arena> arena;
  std::size_t cu = 0;
  std::unique_ptr<Reader> reader;
  std::size_t field(CuField f) const { return (*arena)[cu].fields[f]; }
  long int_field(CuField f) const { return static_cast<long>((*arena)[field(f)].i); }
  const std::string& str(std::size_t id) const { return (*arena)[id].str(); }
};

// Unit_info.lax_modname_from_source
std::string modname_from_source(const std::string& f) {
  std::string base = fs::path(f).filename().string();
  std::size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  if (!base.empty() && base[0] >= 'a' && base[0] <= 'z') base[0] = static_cast<char>(base[0] - 'a' + 'A');
  return base;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

Member read_member_info(std::string_view targetname, const std::string& file) {
  Member mb;
  mb.pm_file = file;
  std::string member_name = modname_from_source(file);
  mb.pm_name = zborrow(member_name);
  // PR#7479: make sure it is either a .cmi or a .cmo
  if (file.size() >= 4 && file.compare(file.size() - 4, 4, ".cmi") == 0) {
    mb.intf = true;
  } else {
    mb.bytes = read_file(file);
    std::size_t n = config::cmo_magic_number.size();
    if (mb.bytes.size() < n + 4 || std::memcmp(mb.bytes.data(), config::cmo_magic_number.data(), n) != 0) {
      Error e(Error::Kind::Not_an_object_file);
      e.file = file;
      throw e;
    }
    std::size_t off = be32(mb.bytes, n);
    mb.arena = std::make_unique<m::Arena>();
    mb.cu = m::read_value(mb.bytes.data(), mb.bytes.size(), off, *mb.arena);
    mb.arena->finalize();
    mb.reader = std::make_unique<Reader>(*mb.arena);
    if (mb.str(mb.field(cu_name)) != member_name) {
      Error e(Error::Kind::Illegal_renaming);
      e.name = member_name;
      e.file = file;
      e.id = mb.str(mb.field(cu_name));
      throw e;
    }
  }
  mb.pm_packed_ident = zborrow(std::string(targetname) + "." + member_name);
  return mb;
}

// ---- Bytelink's interface consistency (crc_interfaces, interfaces) ---------

struct Interfaces {
  struct Crc {
    std::string crc;
    V value;  // the member's string
    std::string source;
  };
  std::unordered_map<std::string, Crc> crc_interfaces;  // Consistbl
  struct Name {
    std::string s;
    V value;
  };
  std::vector<Name> interfaces;  // !interfaces, newest first

  // Bytelink.check_consistency file_name cu
  void check_consistency(const std::string& file_name, Member& mb) {
    const m::Arena& a = *mb.arena;
    for (std::size_t e : list_elems(a, mb.field(cu_imports))) {
      const m::Value& pair = a[e];
      std::size_t nid = pair.fields[0], crco = pair.fields[1];
      const std::string& name = a[nid].str();
      interfaces.insert(interfaces.begin(), Name{name, (*mb.reader)(nid)});
      if (a[crco].kind != m::Value::Kind::Block) continue;  // None
      std::size_t cid = a[crco].fields[0];
      const std::string& crc = a[cid].str();
      auto it = crc_interfaces.find(name);
      if (it == crc_interfaces.end()) {
        crc_interfaces.emplace(name, Crc{crc, (*mb.reader)(cid), file_name});
      } else if (it->second.crc != crc) {
        Error err(Error::Kind::Inconsistent_import);
        err.name = name;
        err.file = file_name;
        err.auth = it->second.source;
        throw err;
      }
    }
  }

  // List.sort_uniq on !interfaces (String.compare): which of equal names
  // stays is the one this merge sort keeps
  static std::vector<Name> sort_uniq(std::vector<Name> l) {
    using List = std::vector<Name>;
    auto cmp = [](const Name& x, const Name& y) { return x.s.compare(y.s) < 0 ? -1 : x.s == y.s ? 0 : 1; };
    auto rev_append = [](List l1, List accu) {  // accu, then l1 reversed: accu is kept head-first
      // lists are represented head-first (front = head)
      List r;
      for (auto it = l1.rbegin(); it != l1.rend(); ++it) r.push_back(*it);
      r.insert(r.end(), accu.begin(), accu.end());
      return r;
    };
    // accumulators are consed at the head: build them reversed then flip
    auto rev_merge = [&](const List& l1, const List& l2, bool rev) {
      List accu_rev;  // accu, newest last
      std::size_t i = 0, j = 0;
      while (i < l1.size() && j < l2.size()) {
        int c = cmp(l1[i], l2[j]);
        if (c == 0) {
          accu_rev.push_back(l1[i]);
          ++i;
          ++j;
        } else if (rev ? c > 0 : c < 0) {
          accu_rev.push_back(l1[i]);
          ++i;
        } else {
          accu_rev.push_back(l2[j]);
          ++j;
        }
      }
      List accu(accu_rev.rbegin(), accu_rev.rend());
      if (i < l1.size()) return rev_append(List(l1.begin() + i, l1.end()), accu);
      return rev_append(List(l2.begin() + j, l2.end()), accu);
    };
    std::function<std::pair<List, List>(long, List)> sort, rev_sort;
    auto sort_small = [&](long n, const List& l, bool rev) -> std::optional<std::pair<List, List>> {
      auto lt = [&](int c) { return rev ? c > 0 : c < 0; };
      if (n == 2 && l.size() >= 2) {
        const Name &x1 = l[0], &x2 = l[1];
        int c = cmp(x1, x2);
        List s = c == 0 ? List{x1} : lt(c) ? List{x1, x2} : List{x2, x1};
        return std::make_pair(s, List(l.begin() + 2, l.end()));
      }
      if (n == 3 && l.size() >= 3) {
        const Name &x1 = l[0], &x2 = l[1], &x3 = l[2];
        List s;
        int c = cmp(x1, x2);
        if (c == 0) {
          int c2 = cmp(x1, x3);
          s = c2 == 0 ? List{x1} : lt(c2) ? List{x1, x3} : List{x3, x1};
        } else if (lt(c)) {
          int c2 = cmp(x2, x3);
          if (c2 == 0) s = {x1, x2};
          else if (lt(c2)) s = {x1, x2, x3};
          else {
            int c3 = cmp(x1, x3);
            if (c3 == 0) s = {x1, x2};
            else if (lt(c3)) s = {x1, x3, x2};
            else s = {x3, x1, x2};
          }
        } else {
          int c2 = cmp(x1, x3);
          if (c2 == 0) s = {x2, x1};
          else if (lt(c2)) s = {x2, x1, x3};
          else {
            int c3 = cmp(x2, x3);
            if (c3 == 0) s = {x2, x1};
            else if (lt(c3)) s = {x2, x3, x1};
            else s = {x3, x2, x1};
          }
        }
        return std::make_pair(s, List(l.begin() + 3, l.end()));
      }
      return std::nullopt;
    };
    sort = [&](long n, List l) -> std::pair<List, List> {
      if (auto r = sort_small(n, l, false)) return *r;
      long n1 = n >> 1, n2 = n - n1;
      auto [s1, l2] = rev_sort(n1, l);
      auto [s2, tl] = rev_sort(n2, l2);
      return {rev_merge(s1, s2, true), tl};
    };
    rev_sort = [&](long n, List l) -> std::pair<List, List> {
      if (auto r = sort_small(n, l, true)) return *r;
      long n1 = n >> 1, n2 = n - n1;
      auto [s1, l2] = sort(n1, l);
      auto [s2, tl] = sort(n2, l2);
      return {rev_merge(s1, s2, false), tl};
    };
    if (l.size() < 2) return l;
    return sort(static_cast<long>(l.size()), l).first;
  }

  // Bytelink.extract_crc_interfaces (): Consistbl.extract, (name, crc option)
  // in reverse sorted order
  std::vector<std::pair<Name, std::optional<Crc>>> extract() const {
    std::vector<Name> l = sort_uniq(interfaces);
    std::vector<std::pair<Name, std::optional<Crc>>> assc;
    for (const Name& n : l) {
      auto it = crc_interfaces.find(n.s);
      assc.insert(assc.begin(),
                  {n, it == crc_interfaces.end() ? std::nullopt : std::optional<Crc>(it->second)});
    }
    return assc;
  }
};

// ---- the state -----------------------------------------------------------------

struct Mapped {
  std::string_view packed_modname;
  bool processed;
};

// Path.Map as the member events' composed substitution holds it: keys
// Pident (Global name), ordered by name; the AVL of map.ml
struct PathMapNode {
  V value;  // Empty (int 0) | Node {l; v; d; r; h}
  std::string key;
  V l_, r_;
  long h = 0;
};

struct State {
  std::vector<V> relocs;  // (reloc_info * int), in output order
  std::vector<V> events;  // output order
  std::set<std::string> debug_dirs;
  std::vector<V> primitives;
  long offset = 0;
  // Subst: identity, or the modules map (Path.Map) of the added modules
  bool subst_identity = true;
  V subst_modules = o::vint(0);  // Path.Map.t
  std::map<std::string, Mapped> mapping;
};

// map.ml's Map.add over values: keys are Pident (Global name), compared by
// name (Ident.compare on Globals)
struct PathMap {
  static long height(const V& m) { return m.is_int() ? 0 : m->fields[4].int_value(); }
  static const std::string& key_name(const V& m) {
    return m->fields[1]->fields[0]->fields[0]->str();  // Pident (Global name)
  }
  static V create(const V& l, const V& x, const V& d, const V& r) {
    long hl = height(l), hr = height(r);
    return o::vblock(0, {l, x, d, r, o::vint(hl >= hr ? hl + 1 : hr + 1)});
  }
  static V bal(const V& l, const V& x, const V& d, const V& r) {
    long hl = height(l), hr = height(r);
    if (hl > hr + 2) {
      const V &ll = l->fields[0], &lv = l->fields[1], &ld = l->fields[2], &lr = l->fields[3];
      if (height(ll) >= height(lr)) return create(ll, lv, ld, create(lr, x, d, r));
      return create(create(ll, lv, ld, lr->fields[0]), lr->fields[1], lr->fields[2], create(lr->fields[3], x, d, r));
    }
    if (hr > hl + 2) {
      const V &rl = r->fields[0], &rv = r->fields[1], &rd = r->fields[2], &rr = r->fields[3];
      if (height(rr) >= height(rl)) return create(create(l, x, d, rl), rv, rd, rr);
      return create(create(l, x, d, rl->fields[0]), rl->fields[1], rl->fields[2], create(rl->fields[3], rv, rd, rr));
    }
    return o::vblock(0, {l, x, d, r, o::vint(hl >= hr ? hl + 1 : hr + 1)});
  }
  static V add(const V& x, const std::string& xname, const V& data, const V& m) {
    if (m.is_int()) return o::vblock(0, {o::vint(0), x, data, o::vint(0), o::vint(1)});
    const V &l = m->fields[0], &v = m->fields[1], &d = m->fields[2], &r = m->fields[3];
    int c = xname.compare(key_name(m));
    if (c == 0) return d == data ? m : o::vblock(0, {l, x, data, r, m->fields[4]});
    if (c < 0) {
      V ll = add(x, xname, data, l);
      return l == ll ? m : bal(ll, v, d, r);
    }
    V rr = add(x, xname, data, r);
    return r == rr ? m : bal(l, v, d, rr);
  }
};

// Update a relocation.  adjust its offset, and rename GETGLOBAL and
// SETGLOBAL relocations that correspond to one of the units being
// consolidated.
V rename_relocation(std::string_view packagename, const std::string& objfile, const State& st,
                    emitcode::ValueContext& w, Member& mb, long base, std::size_t pair_id) {
  const m::Arena& a = *mb.arena;
  const m::Value& pair = a[pair_id];
  std::size_t rel = pair.fields[0];
  long ofs = static_cast<long>(a[pair.fields[1]].i);
  // PR#5276: unique-ize dotted global names, which appear if one of the
  // units being consolidated is itself a packed module.
  auto make_compunit_name_unique = [&](std::size_t cu) -> V {
    const std::string& s = a[cu].str();
    if (s.find('.') != std::string::npos) return o::vstr(std::string(packagename) + "." + s);
    return (*mb.reader)(cu);
  };
  V rel2;
  unsigned tag = a[rel].tag;
  if (tag == 1 || tag == 3) {  // Reloc_getcompunit | Reloc_setcompunit
    std::size_t cu = a[rel].fields[0];
    auto it = st.mapping.find(a[cu].str());
    if (it == st.mapping.end()) {
      rel2 = o::vblock(static_cast<int>(tag), {make_compunit_name_unique(cu)});
    } else if (tag == 1) {
      if (!it->second.processed) {
        Error e(Error::Kind::Forward_reference);
        e.file = objfile;
        e.name = a[cu].str();
        throw e;
      }
      rel2 = o::vblock(1, {w.str(it->second.packed_modname)});
    } else {
      if (it->second.processed) {
        Error e(Error::Kind::Multiple_definition);
        e.file = objfile;
        e.name = a[cu].str();
        throw e;
      }
      rel2 = o::vblock(3, {w.str(it->second.packed_modname)});
    }
  } else {  // Reloc_literal | Reloc_getpredef | Reloc_primitive
    rel2 = (*mb.reader)(rel);
  }
  return o::vblock(0, {rel2, o::vint(base + ofs)});
}

// relocate a debugging event: { ev with ev_pos = base + ev.ev_pos;
// ev_module = prefix ^ "." ^ ev.ev_module; ev_typsubst = Subst.compose
// ev.ev_typsubst subst }
V relocate_debug(long base, std::string_view prefix, const State& st, Member& mb, std::size_t ev,
                 std::unordered_map<std::size_t, V>& composed) {
  const m::Arena& a = *mb.arena;
  const m::Value& e = a[ev];
  std::vector<V> fs;
  for (std::size_t f : e.fields) fs.push_back((*mb.reader)(f));
  fs[0] = o::vint(base + static_cast<long>(a[e.fields[0]].i));
  fs[1] = o::vstr(std::string(prefix) + "." + a[e.fields[1]].str());
  if (!st.subst_identity) {
    // compose s1 s2 = { types = merge s1.types s2.types; modules = ...;
    // modtypes = ...; for_saving = s1 || s2; loc = keep_latest_loc } -- s2
    // (the packager's) has only modules, no loc, not for saving
    std::size_t s1 = e.fields[7];
    auto it = composed.find(s1);
    V r;
    if (it != composed.end()) {
      r = o::vblock(0, {});
      r->fields = it->second->fields;  // a fresh record per compose
    } else {
      const m::Value& sv = a[s1];
      for (int k = 0; k < 3; ++k)
        if (a[sv.fields[k]].kind != m::Value::Kind::Int)
          throw std::runtime_error("-pack -g: a member's debug event carries a substitution (a nested pack)");
      r = o::vblock(0, {o::vint(0), st.subst_modules, o::vint(0), (*mb.reader)(sv.fields[3]),
                        (*mb.reader)(sv.fields[4])});
      composed[s1] = r;
    }
    fs[7] = r;
  }
  return o::vblock(static_cast<int>(e.tag), std::move(fs));
}

// Read the bytecode from a .cmo file, append it, rename compunits as
// indicated by [mapping] in reloc info, accumulate relocs, debug info, etc.
void rename_append_bytecode(std::string_view packagename, std::string& oc, State& st, Interfaces& itf,
                            emitcode::ValueContext& w, Member& mb) {
  const m::Arena& a = *mb.arena;
  itf.check_consistency(mb.pm_file, mb);
  for (std::size_t r : list_elems(a, mb.field(cu_reloc)))
    st.relocs.push_back(rename_relocation(packagename, mb.pm_file, st, w, mb, st.offset, r));
  for (std::size_t p : list_elems(a, mb.field(cu_primitives))) st.primitives.push_back((*mb.reader)(p));
  long pos = mb.int_field(cu_pos), size = mb.int_field(cu_codesize);
  oc.append(reinterpret_cast<const char*>(mb.bytes.data() + pos), static_cast<std::size_t>(size));
  if (clflags::debug && mb.int_field(cu_debug) > 0) {
    std::size_t off = static_cast<std::size_t>(mb.int_field(cu_debug));
    std::size_t evs = m::read_value(mb.bytes.data(), mb.bytes.size(), off, *mb.arena);
    std::size_t dirs = m::read_value(mb.bytes.data(), mb.bytes.size(), off, *mb.arena);
    mb.arena->finalize();
    std::unordered_map<std::size_t, V> composed;
    for (std::size_t ev : list_elems(*mb.arena, evs))
      st.events.push_back(relocate_debug(st.offset, packagename, st, mb, ev, composed));
    for (std::size_t d : list_elems(*mb.arena, dirs)) st.debug_dirs.insert((*mb.arena)[d].str());
  }
  st.offset += size;
}

// Same, for a .cmo or .cmi member
void rename_append_pack_member(std::string_view packagename, std::string& oc, State& st, Interfaces& itf,
                               emitcode::ValueContext& w, Member& mb) {
  if (mb.intf) return;
  rename_append_bytecode(packagename, oc, st, itf, w, mb);
  // record_as_processed
  st.mapping[std::string(mb.pm_name)].processed = true;
  // Subst.add_module id' (Pdot (root, Ident.name id')) state.subst, root =
  // Pident (Ident.create_persistent packagename): the member's compunit
  // string is the ident's name
  V name = w.str(mb.pm_name);
  V id = o::vblock(2, {name});  // Global name
  V root = o::vblock(0, {o::vblock(2, {w.str(packagename)})});
  V target = o::vblock(1, {root, name});
  st.subst_modules = PathMap::add(o::vblock(0, {id}), std::string(mb.pm_name), target, st.subst_modules);
  st.subst_identity = false;
}

void output_binary_int(std::string& out, long n) {
  out.push_back(static_cast<char>((n >> 24) & 0xff));
  out.push_back(static_cast<char>((n >> 16) & 0xff));
  out.push_back(static_cast<char>((n >> 8) & 0xff));
  out.push_back(static_cast<char>(n & 0xff));
}
void output_value(std::string& out, const V& v, bool compressed = false) {
  std::vector<std::uint8_t> bytes = o::marshal(v, compressed);
  out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
// Compression.output_value
void compressed_output_value(std::string& out, const V& v) {
  output_value(out, v, config::compression_supported);
}

// Build the .cmo file obtained by packaging the given .cmo files.
void package_object_files(std::vector<std::string>& files, const std::string& targetfile,
                          std::string_view targetname, const typedtree::ModuleCoercion* coercion) {
  std::vector<Member> members;
  for (const std::string& f : files) members.push_back(read_member_info(targetname, f));  // map_left_right
  // required_compunits: fold_right over the members; Set.add keeps the
  // string already there
  std::map<std::string, V> required;
  for (auto mit = members.rbegin(); mit != members.rend(); ++mit) {
    if (mit->intf) continue;
    const m::Arena& a = *mit->arena;
    std::vector<std::size_t> relocs = list_elems(a, mit->field(cu_reloc));
    for (auto it = relocs.rbegin(); it != relocs.rend(); ++it) {
      std::size_t rel = a[*it].fields[0];
      if (a[rel].tag == 3) required.erase(a[a[rel].fields[0]].str());
    }
    std::vector<std::size_t> req = list_elems(a, mit->field(cu_required_compunits));
    for (auto it = req.rbegin(); it != req.rend(); ++it)
      required.try_emplace(a[*it].str(), (*mit->reader)(*it));
  }
  emitcode::ValueContext w;
  std::string oc(config::cmo_magic_number);
  long pos_depl = static_cast<long>(oc.size());
  output_binary_int(oc, 0);
  long pos_code = static_cast<long>(oc.size());
  State st;
  for (const Member& mb : members) st.mapping[std::string(mb.pm_name)] = Mapped{mb.pm_packed_ident, false};
  Interfaces itf;
  for (Member& mb : members) rename_append_pack_member(targetname, oc, st, itf, w, mb);
  // build_global_target: the code that builds the tuple representing the
  // package module
  {
    std::vector<Ident::t> components;
    for (const Member& mb : members)
      components.push_back(mb.intf ? nullptr : Ident::create_persistent(mb.pm_packed_ident));
    lambda::lambda lam = translmod::transl_package(slice(components), Ident::create_persistent(targetname), coercion);
    lam = simplif::simplify_lambda(lam);
    if (clflags::dump_lambda) std::cerr << printlambda::dump(lam);
    instruct::code instrs = bytegen::compile_implementation(targetname, lam);
    emitcode::PackedFile pf = emitcode::to_packed_file(oc, instrs, w);
    // events = List.rev_append pack_events state.events: the package's
    // events (their positions not relocated) after the members'
    std::vector<const instruct::DebugEvent*> pevs(pf.events.begin(), pf.events.end());
    // (their ev_module is target_name: the package name string, as in the
    // members' composed substitutions)
    for (V& ev : cmi_format::debug_event_values(pevs, w.str(targetname))) st.events.push_back(ev);
    st.debug_dirs.insert(pf.debug_dirs.begin(), pf.debug_dirs.end());
    for (auto& [r, ofs] : pf.relocs) st.relocs.push_back(o::vblock(0, {r, o::vint(st.offset + ofs)}));
    st.offset += pf.size;
  }
  long pos_debug = static_cast<long>(oc.size());
  if (clflags::debug && !st.events.empty()) {
    compressed_output_value(oc, o::vlist(st.events));
    std::vector<V> dirs;
    for (const std::string& d : st.debug_dirs) dirs.push_back(o::vstr(d));
    compressed_output_value(oc, o::vlist(dirs));
  }
  bool force_link = false;
  for (const Member& mb : members)
    if (!mb.intf && mb.int_field(cu_force_link)) force_link = true;
  long pos_final = static_cast<long>(oc.size());
  V cu_name_v = w.str(targetname);
  std::vector<V> imports{
      o::vblock(0, {cu_name_v, o::vblock(0, {o::vstr(env::crc_of_unit(std::string(targetname)))})})};
  for (auto& [name, crc] : itf.extract()) {
    bool member = false;
    for (const Member& mb : members)
      if (mb.pm_name == name.s) member = true;
    if (member) continue;
    imports.push_back(o::vblock(0, {name.value, crc ? o::vblock(0, {crc->value}) : o::vint(0)}));
  }
  std::vector<V> req;
  for (auto& [s, v] : required) req.push_back(v);
  V compunit = o::vblock(0, {
      cu_name_v,
      o::vint(pos_code),
      o::vint(pos_debug - pos_code),
      o::vlist(st.relocs),
      o::vlist(imports),
      o::vlist(req),
      o::vlist(st.primitives),
      o::vint(force_link ? 1 : 0),
      o::vint(pos_final > pos_debug ? pos_debug : 0),
      o::vint(pos_final - pos_debug),
  });
  output_value(oc, compunit);
  std::string depl;
  output_binary_int(depl, pos_final);
  std::memcpy(oc.data() + pos_depl, depl.data(), 4);
  std::ofstream out(targetfile, std::ios::binary);
  if (!out) throw std::runtime_error("cannot open " + targetfile);
  out.write(oc.data(), static_cast<std::streamsize>(oc.size()));
  if (!out) throw std::runtime_error("write error on " + targetfile);
}

}  // namespace

void package_files(env::t initial_env, const std::vector<std::string>& files0, const std::string& targetfile) {
  std::vector<std::string> files;
  for (const std::string& f : files0) {
    try {
      files.push_back(load_path::find(f));
    } catch (const load_path::NotFound&) {
      Error e(Error::Kind::File_not_found);
      e.file = f;
      throw e;
    }
  }
  std::string targetname = modname_from_source(targetfile);
  std::size_t slash = targetfile.rfind('/');
  std::size_t dot = targetfile.find('.', slash == std::string::npos ? 0 : slash + 1);
  std::string target_cmi = (dot == std::string::npos ? targetfile : targetfile.substr(0, dot)) + ".cmi";
  try {
    const typedtree::ModuleCoercion* coercion = typemod::package_units(initial_env, files, targetname, target_cmi);
    package_object_files(files, targetfile, zborrow(targetname), coercion);
  } catch (...) {
    std::remove(targetfile.c_str());
    throw;
  }
}

std::string report_error(const Error& e) {
  switch (e.kind) {
    case Error::Kind::Forward_reference:
      return "Forward reference to " + e.name + " in file \"" + e.file + "\"";
    case Error::Kind::Multiple_definition:
      return "File \"" + e.file + "\" redefines " + e.name;
    case Error::Kind::Not_an_object_file:
      return "\"" + e.file + "\" is not a bytecode object file";
    case Error::Kind::Illegal_renaming:
      return "Wrong file naming: \"" + e.file + "\" contains the code for " + e.name + " when " + e.id +
             " was expected";
    case Error::Kind::File_not_found:
      return "File " + e.file + " not found";
    case Error::Kind::Inconsistent_import:
      return "Files \"" + e.file + "\" and \"" + e.auth + "\" make inconsistent assumptions over interface " +
             e.name;
  }
  return "Bytepackager.Error";
}

}  // namespace cppcaml::typing::bytepackager
