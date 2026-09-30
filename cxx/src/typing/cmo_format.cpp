// See cmo_format.hpp.
#include "cppcaml/typing/cmo_format.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <unordered_map>

#include "cppcaml/marshal.hpp"
#include "cppcaml/typing/arg.hpp"

namespace cppcaml::typing::cmo_format {

namespace {
namespace m = cppcaml::marshal;
namespace o = cppcaml::omarshal;

// A value read back from an object file, re-marshaled with its sharing:
// one omarshal value per decoded object (Bytepackager's Reader).
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
}  // namespace

std::vector<V> list_elems(const V& l0) {
  std::vector<V> out;
  for (V l = l0; l.kind() == omarshal::Value::Block && l->fields.size() == 2; l = l->fields[1])
    out.push_back(l->fields[0]);
  return out;
}

std::vector<Reloc> CompUnit::relocs() const {
  std::vector<Reloc> r;
  for (const V& e : list_elems(v->fields[cu_reloc])) {  // (reloc_info * int)
    const V& info = e->fields[0];
    Reloc rel{};
    rel.pos = static_cast<long>(e->fields[1].int_value());
    rel.k = static_cast<Reloc::K>(info->tag);
    if (rel.k == Reloc::K::Reloc_literal) {
      rel.literal = info->fields[0];
    } else {
      rel.name = info->fields[0]->str();
      rel.name_obj = info->fields[0];
    }
    r.push_back(std::move(rel));
  }
  return r;
}

std::vector<std::pair<std::string, std::optional<std::string>>> CompUnit::imports() const {
  std::vector<std::pair<std::string, std::optional<std::string>>> r;
  for (const V& e : list_elems(v->fields[cu_imports])) {
    const V& crc = e->fields[1];
    if (crc.kind() == omarshal::Value::Block) r.emplace_back(e->fields[0]->str(), crc->fields[0]->str());
    else r.emplace_back(e->fields[0]->str(), std::nullopt);
  }
  return r;
}

std::vector<std::pair<V, V>> CompUnit::import_objs() const {
  std::vector<std::pair<V, V>> r;
  for (const V& e : list_elems(v->fields[cu_imports])) {
    const V& crc = e->fields[1];
    r.emplace_back(e->fields[0], crc.kind() == omarshal::Value::Block ? crc->fields[0] : nullptr);
  }
  return r;
}

std::vector<std::string> CompUnit::required_compunits() const {
  std::vector<std::string> r;
  for (const V& e : list_elems(v->fields[cu_required_compunits])) r.push_back(e->str());
  return r;
}

std::vector<std::string> CompUnit::primitives() const {
  std::vector<std::string> r;
  for (const V& e : list_elems(v->fields[cu_primitives])) r.push_back(e->str());
  return r;
}

ObjFile::ObjFile(const std::string& path) : path_(path) {
  std::ifstream f(path, std::ios::binary);  // open_in_bin
  if (!f) throw arg::SysError(path + ": " + std::strerror(errno));
  // (one read of the file's size, not a character at a time: a library is
  // hundreds of megabytes)
  f.seekg(0, std::ios::end);
  std::streamoff n = f.tellg();
  f.seekg(0);
  if (n > 0) {
    bytes_.resize(static_cast<std::size_t>(n));
    f.read(reinterpret_cast<char*>(bytes_.data()), n);
    bytes_.resize(static_cast<std::size_t>(f.gcount()));
  }
}

std::optional<std::string> ObjFile::read_string(long pos, long len) const {
  if (pos < 0 || pos + len > size()) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(bytes_.data()) + pos, static_cast<std::size_t>(len));
}

std::optional<long> ObjFile::read_binary_int(long pos) const {
  if (pos < 0 || pos + 4 > size()) return std::nullopt;
  const std::uint8_t* b = bytes_.data() + pos;
  std::uint32_t n = (static_cast<std::uint32_t>(b[0]) << 24) | (static_cast<std::uint32_t>(b[1]) << 16) |
                    (static_cast<std::uint32_t>(b[2]) << 8) | static_cast<std::uint32_t>(b[3]);
  return static_cast<long>(static_cast<std::int32_t>(n));  // input_binary_int: signed
}

V ObjFile::input_value(long& pos) const {
  if (pos < 0 || pos >= size()) throw EndOfFile{};
  m::Arena arena;
  std::size_t root;
  std::size_t off = static_cast<std::size_t>(pos);
  try {
    root = m::read_value(bytes_.data(), bytes_.size(), off, arena);
  } catch (const std::exception&) {
    throw EndOfFile{};
  }
  pos = static_cast<long>(off);
  arena.finalize();
  Reader reader(arena);
  return reader(root);
}

std::vector<std::uint8_t> ObjFile::raw_value(long& pos) const {
  if (pos < 0 || pos >= size()) throw EndOfFile{};
  std::size_t off = static_cast<std::size_t>(pos);
  std::vector<std::uint8_t> r;
  try {
    r = m::raw_value(bytes_.data(), bytes_.size(), off);
  } catch (const std::exception&) {
    throw EndOfFile{};
  }
  pos = static_cast<long>(off);
  return r;
}

}  // namespace cppcaml::typing::cmo_format
