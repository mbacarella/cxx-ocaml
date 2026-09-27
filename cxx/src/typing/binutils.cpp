// Port of utils/binutils.ml; see binutils.hpp.
#include "cppcaml/typing/binutils.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "cppcaml/typing/arg.hpp"

namespace cppcaml::typing::binutils {

namespace {

struct EndOfFile {};
struct Raise {  // exception Error of error
  Error e;
};

std::string char_to_hex(char c) {
  char b[8];
  std::snprintf(b, sizeof b, "0x%02x", static_cast<unsigned>(static_cast<unsigned char>(c)));
  return b;
}
std::string int_to_hex(long n) {
  char b[32];
  std::snprintf(b, sizeof b, "0x%lx", static_cast<unsigned long>(n));
  return b;
}

using Bytes = std::vector<std::uint8_t>;

// name_at ?max_len buf start
std::string name_at(const Bytes& buf, long start, long max_len = -1) {
  long len = static_cast<long>(buf.size());
  if (start < 0 || start > len) throw Raise{{Error::Kind::Out_of_range, int_to_hex(start)}};
  long max_pos = max_len < 0 ? len : std::min(len, start + max_len);
  long pos = start;
  while (pos < max_pos && buf[static_cast<std::size_t>(pos)] != 0) ++pos;
  return std::string(buf.begin() + start, buf.begin() + pos);
}

enum class Endianness { LE, BE };
enum class Bitness { B32, B64 };

struct Decoder {
  std::ifstream* ic;
  Endianness endianness;
  Bitness bitness;
};

long word_size(const Decoder& d) { return d.bitness == Bitness::B64 ? 8 : 4; }

// Bytes.get_* (raise Invalid_argument out of bounds)
std::uint64_t get_le(const Bytes& b, long i, int n) {
  if (i < 0 || i + n > static_cast<long>(b.size())) throw std::invalid_argument("index out of bounds");
  std::uint64_t r = 0;
  for (int k = n - 1; k >= 0; --k) r = (r << 8) | b[static_cast<std::size_t>(i + k)];
  return r;
}
std::uint64_t get_be(const Bytes& b, long i, int n) {
  if (i < 0 || i + n > static_cast<long>(b.size())) throw std::invalid_argument("index out of bounds");
  std::uint64_t r = 0;
  for (int k = 0; k < n; ++k) r = (r << 8) | b[static_cast<std::size_t>(i + k)];
  return r;
}
long get_uint16(const Decoder& d, const Bytes& buf, long idx) {
  return static_cast<long>(d.endianness == Endianness::LE ? get_le(buf, idx, 2) : get_be(buf, idx, 2));
}
// get_uint32: an int32
std::int32_t get_uint32(const Decoder& d, const Bytes& buf, long idx) {
  return static_cast<std::int32_t>(d.endianness == Endianness::LE ? get_le(buf, idx, 4) : get_be(buf, idx, 4));
}
// get_uint s d buf idx: Int32.unsigned_to_int (always Some with 63-bit ints)
long get_uint(const char*, const Decoder& d, const Bytes& buf, long idx) {
  return static_cast<long>(static_cast<std::uint32_t>(get_uint32(d, buf, idx)));
}
std::int64_t get_uint64(const Decoder& d, const Bytes& buf, long idx) {
  return static_cast<std::int64_t>(d.endianness == Endianness::LE ? get_le(buf, idx, 8) : get_be(buf, idx, 8));
}
std::int64_t uint64_of_uint32(std::int32_t n) { return static_cast<std::int64_t>(static_cast<std::uint32_t>(n)); }
std::int64_t get_word(const Decoder& d, const Bytes& buf, long idx) {
  return d.bitness == Bitness::B64 ? get_uint64(d, buf, idx) : uint64_of_uint32(get_uint32(d, buf, idx));
}
// uint64_to_int s n: Int64.unsigned_to_int (Some when 0 <= n <= max_int)
long uint64_to_int(const char* s, std::int64_t n) {
  if (n < 0 || n > 0x3FFFFFFFFFFFFFFFLL) throw Raise{{Error::Kind::Unsupported, s, n}};
  return static_cast<long>(n);
}

Bytes really_input_bytes(std::ifstream& ic, long len) {
  Bytes b(static_cast<std::size_t>(len));
  if (len > 0 && !ic.read(reinterpret_cast<char*>(b.data()), len)) throw EndOfFile{};
  return b;
}
Bytes load_bytes(const Decoder& d, std::int64_t off, long len) {
  d.ic->clear();
  d.ic->seekg(off);
  return really_input_bytes(*d.ic, len);
}

// ---- ELF ----
namespace elf {

long header_size(const Decoder& d) { return 40 + 3 * word_size(d); }

struct Header {
  std::int64_t e_shoff;
  long e_shentsize, e_shnum, e_shstrndx;
};

Header read_header(const Decoder& d) {
  Bytes buf = load_bytes(d, 0, header_size(d));
  long w = word_size(d);
  Header h;
  h.e_shnum = get_uint16(d, buf, 36 + 3 * w);
  h.e_shentsize = get_uint16(d, buf, 34 + 3 * w);
  h.e_shoff = get_word(d, buf, 24 + 2 * w);
  h.e_shstrndx = get_uint16(d, buf, 38 + 3 * w);
  return h;
}

enum class ShType { SHT_STRTAB, SHT_DYNSYM, SHT_OTHER };

struct Section {
  long sh_name;
  ShType sh_type;
  std::int64_t sh_addr, sh_offset;
  long sh_size, sh_entsize;
  std::string sh_name_str;
};

Bytes load_section_body(const Decoder& d, const Section& s) { return load_bytes(d, s.sh_offset, s.sh_size); }

std::vector<Section> read_sections0(const Decoder& d, const Header& h) {
  Bytes buf = load_bytes(d, h.e_shoff, h.e_shnum * h.e_shentsize);
  long w = word_size(d);
  std::vector<Section> sections;
  for (long i = 0; i < h.e_shnum; ++i) {  // Array.init
    long base = i * h.e_shentsize;
    Section s;
    s.sh_name = get_uint("sh_name", d, buf, base + 0);
    std::int32_t t = get_uint32(d, buf, base + 4);
    s.sh_type = t == 3 ? ShType::SHT_STRTAB : t == 11 ? ShType::SHT_DYNSYM : ShType::SHT_OTHER;
    s.sh_addr = get_word(d, buf, base + 8 + w);
    s.sh_offset = get_word(d, buf, base + 8 + 2 * w);
    s.sh_size = uint64_to_int("sh_size", get_word(d, buf, base + 8 + 3 * w));
    s.sh_entsize = uint64_to_int("sh_entsize", get_word(d, buf, base + 16 + 5 * w));
    sections.push_back(std::move(s));
  }
  if (h.e_shstrndx == 0) return sections;  // no string table
  Bytes shstrtbl = load_section_body(d, sections.at(static_cast<std::size_t>(h.e_shstrndx)));
  for (Section& s : sections) s.sh_name_str = name_at(shstrtbl, s.sh_name);
  return sections;
}

std::vector<Section> read_sections(const Decoder& d, Header h) {
  if (h.e_shoff == 0) return {};
  std::unique_ptr<Bytes> buf;  // lazy (load_bytes d e_shoff e_shentsize)
  auto force = [&]() -> const Bytes& {
    if (!buf) buf = std::make_unique<Bytes>(load_bytes(d, h.e_shoff, h.e_shentsize));
    return *buf;
  };
  long w = word_size(d);
  // (The real e_shnum is the sh_size of the initial section.)
  if (h.e_shnum == 0) h.e_shnum = uint64_to_int("e_shnum", get_word(d, force(), 8 + 3 * w));
  // (The real e_shstrndx is the sh_link of the initial section.)
  if (h.e_shstrndx == 0xffff) h.e_shstrndx = get_uint("e_shstrndx", d, force(), 8 + 4 * w);
  return read_sections0(d, h);
}

struct Symbol {
  std::string st_name;
  std::int64_t st_value;
  long st_shndx;
};

const Section* find_section(const std::vector<Section>& sections, ShType type_, const char* sectname) {
  for (const Section& s : sections)
    if (s.sh_type == type_ && s.sh_name_str == sectname) return &s;
  return nullptr;
}

std::vector<Symbol> read_symbols(const Decoder& d, const std::vector<Section>& sections) {
  const Section* dynsym = find_section(sections, ShType::SHT_DYNSYM, ".dynsym");
  if (!dynsym) return {};
  if (dynsym->sh_entsize == 0) throw Raise{{Error::Kind::Out_of_range, "sh_entsize=0"}};
  const Section* dynstr = find_section(sections, ShType::SHT_STRTAB, ".dynstr");
  if (!dynstr) return {};
  Bytes strtbl = load_section_body(d, *dynstr);
  Bytes buf = load_section_body(d, *dynsym);
  long w = word_size(d);
  std::vector<Symbol> syms;
  for (long i = 0; i < dynsym->sh_size / dynsym->sh_entsize; ++i) {
    long base = i * dynsym->sh_entsize;
    Symbol s;
    s.st_name = name_at(strtbl, get_uint("st_name", d, buf, base));
    s.st_value = get_word(d, buf, base + w /* ! */);
    s.st_shndx = get_uint16(d, buf, base + (d.bitness == Bitness::B64 ? 6 : 14));
    syms.push_back(std::move(s));
  }
  return syms;
}

const Symbol* find_symbol(const std::vector<Symbol>& symbols, const std::string& symname) {
  for (const Symbol& s : symbols)
    if (s.st_shndx != 0 && s.st_name == symname) return &s;
  return nullptr;
}

T read(std::ifstream& ic) {
  ic.clear();
  ic.seekg(0);
  Bytes identification = really_input_bytes(ic, 16);
  Bitness bitness;
  switch (identification[4]) {
    case 1: bitness = Bitness::B32; break;
    case 2: bitness = Bitness::B64; break;
    default: throw Raise{{Error::Kind::Unsupported, "ELFCLASS", identification[4]}};
  }
  Endianness endianness;
  switch (identification[5]) {
    case 1: endianness = Endianness::LE; break;
    case 2: endianness = Endianness::BE; break;
    default: throw Raise{{Error::Kind::Unsupported, "ELFDATA", identification[5]}};
  }
  Decoder d{&ic, endianness, bitness};
  Header header = read_header(d);
  auto sections = std::make_shared<std::vector<Section>>(read_sections(d, header));
  auto symbols = std::make_shared<std::vector<Symbol>>(read_symbols(d, *sections));
  T t;
  t.symbol_offset = [sections, symbols](const std::string& symname) -> std::optional<std::int64_t> {
    const Symbol* s = find_symbol(*symbols, symname);
    if (!s) return std::nullopt;
    // st_value in executables and shared objects holds a virtual (absolute) address
    const Section& sec = sections->at(static_cast<std::size_t>(s->st_shndx));
    return sec.sh_offset + (s->st_value - sec.sh_addr);
  };
  t.defines_symbol = [symbols](const std::string& symname) { return find_symbol(*symbols, symname) != nullptr; };
  return t;
}

}  // namespace elf

// ---- Mach-O ----
namespace mach_o {

constexpr long size_int = 4;
long header_size(const Decoder& d) { return (d.bitness == Bitness::B64 ? 6 : 5) * 4 + 2 * size_int; }

struct LcSymtab {
  std::int32_t symoff;
  long nsyms;
  std::int32_t stroff;
  long strsize;
};

struct Symbol {
  std::string n_name;
  long n_type;
  std::int64_t n_value;
};

T read(std::ifstream& ic) {
  ic.clear();
  ic.seekg(0);
  Bytes magicb = really_input_bytes(ic, 4);
  std::uint32_t magic;
  std::memcpy(&magic, magicb.data(), 4);  // Bytes.get_int32_ne
  enum { MH_MAGIC, MH_CIGAM, MH_MAGIC_64, MH_CIGAM_64 } m;
  switch (magic) {
    case 0xFEEDFACEu: m = MH_MAGIC; break;
    case 0xCEFAEDFEu: m = MH_CIGAM; break;
    case 0xFEEDFACFu: m = MH_MAGIC_64; break;
    case 0xCFFAEDFEu: m = MH_CIGAM_64; break;
    default: throw Raise{{Error::Kind::Unrecognized, std::string(magicb.begin(), magicb.end())}};
  }
  Bitness bitness = m == MH_MAGIC || m == MH_CIGAM ? Bitness::B32 : Bitness::B64;
  const bool big_endian = false;  // Sys.big_endian (the hosts c++ocamlc runs on)
  bool direct = m == MH_MAGIC || m == MH_MAGIC_64;
  Endianness endianness = direct != big_endian ? Endianness::LE : Endianness::BE;
  Decoder d{&ic, endianness, bitness};
  // read_header
  Bytes hbuf = load_bytes(d, 0, header_size(d));
  long ncmds = get_uint("ncmds", d, hbuf, 8 + 2 * size_int);
  long sizeofcmds = get_uint("sizeofcmds", d, hbuf, 12 + 2 * size_int);
  // read_load_commands
  Bytes buf = load_bytes(d, header_size(d), sizeofcmds);
  long base = 0;
  std::optional<LcSymtab> symtab;
  for (long i = 0; i < ncmds; ++i) {
    std::int32_t cmd = get_uint32(d, buf, base + 0);
    long cmdsize = get_uint("cmdsize", d, buf, base + 4);
    if (cmd == 0x2) {
      LcSymtab s;
      s.symoff = get_uint32(d, buf, base + 8);
      s.nsyms = get_uint("nsyms", d, buf, base + 12);
      s.stroff = get_uint32(d, buf, base + 16);
      s.strsize = get_uint("strsize", d, buf, base + 20);
      if (!symtab) symtab = s;  // (array_find_map: the first)
    }
    base += cmdsize;
  }
  auto symbols = std::make_shared<std::vector<Symbol>>();
  if (symtab) {
    Bytes strtbl = load_bytes(d, uint64_of_uint32(symtab->stroff), symtab->strsize);
    long size_nlist = 8 + word_size(d);
    Bytes sbuf = load_bytes(d, uint64_of_uint32(symtab->symoff), symtab->nsyms * size_nlist);
    for (long i = 0; i < symtab->nsyms; ++i) {
      long b = i * size_nlist;
      Symbol s;
      s.n_name = name_at(strtbl, get_uint("n_name", d, sbuf, b + 0));
      s.n_type = sbuf.at(static_cast<std::size_t>(b + 4));
      s.n_value = get_word(d, sbuf, b + 8);
      symbols->push_back(std::move(s));
    }
  }
  auto find_symbol = [symbols](const std::string& symname) -> const Symbol* {
    for (const Symbol& s : *symbols)
      if ((s.n_type & 0b1111) == 0b1111 /* N_EXT + N_SECT */ && s.n_name == symname) return &s;
    return nullptr;
  };
  T t;
  t.symbol_offset = [find_symbol](const std::string& symname) -> std::optional<std::int64_t> {
    const Symbol* s = find_symbol("_" + symname);
    if (!s) return std::nullopt;
    return s->n_value;
  };
  t.defines_symbol = [find_symbol](const std::string& symname) { return find_symbol("_" + symname) != nullptr; };
  return t;
}

}  // namespace mach_o

// ---- FlexDLL (PE) ----
namespace flexdll {

constexpr long header_size = 24;
constexpr long section_header_size = 40;

struct Section {
  std::string name;
  std::int64_t virtual_address;
  long size_of_raw_data;
  std::int64_t pointer_to_raw_data;
};
struct Symbol {
  std::string name;
  std::int64_t address;
};

T read(std::ifstream& ic) {
  ic.clear();
  ic.seekg(0x3c);
  Bytes lb = really_input_bytes(ic, 4);
  Decoder le{&ic, Endianness::LE, Bitness::B32};
  std::int64_t e_lfanew = uint64_of_uint32(get_uint32(le, lb, 0));
  ic.clear();
  ic.seekg(e_lfanew);
  Bytes buf = really_input_bytes(ic, header_size);
  std::string magic(buf.begin(), buf.begin() + 4);
  if (magic != std::string("PE\0\0", 4)) throw Raise{{Error::Kind::Unrecognized, magic}};
  long machine = static_cast<long>(get_le(buf, 4, 2));
  Bitness bitness;
  switch (machine) {
    case 0x8664: case 0xaa64: bitness = Bitness::B64; break;  // AMD64 / ARM64
    case 0x14c: case 0x1c0: bitness = Bitness::B32; break;    // I386 / ARM
    default: throw Raise{{Error::Kind::Unsupported, "MACHINETYPE", machine}};
  }
  Decoder d{&ic, Endianness::LE, bitness};
  // read_header
  long number_of_sections = get_uint16(d, buf, 6);
  long size_of_optional_header = get_uint16(d, buf, 20);
  (void)get_uint16(d, buf, 22);  // _characteristics
  // read_optional_header
  if (size_of_optional_header == 0) throw Raise{{Error::Kind::Unrecognized, "SizeOfOptionalHeader=0"}};
  Bytes obuf = load_bytes(d, e_lfanew + header_size, size_of_optional_header);
  std::int64_t image_base;
  switch (get_uint16(d, obuf, 0)) {
    case 0x10b: image_base = uint64_of_uint32(get_uint32(d, obuf, 28)); break;  // PE32
    case 0x20b: image_base = get_uint64(d, obuf, 24); break;                    // PE32PLUS
    default: throw Raise{{Error::Kind::Unsupported, "optional_header_magic", get_uint16(d, obuf, 0)}};
  }
  // read_sections
  Bytes sbuf = load_bytes(d, e_lfanew + header_size + size_of_optional_header,
                          number_of_sections * section_header_size);
  auto sections = std::make_shared<std::vector<Section>>();
  for (long i = 0; i < number_of_sections; ++i) {
    long base = i * section_header_size;
    Section s;
    s.name = name_at(sbuf, base + 0, 8);
    (void)get_uint("virtual_size", d, sbuf, base + 8);
    s.virtual_address = uint64_of_uint32(get_uint32(d, sbuf, base + 12));
    s.size_of_raw_data = get_uint("size_of_raw_data", d, sbuf, base + 16);
    s.pointer_to_raw_data = uint64_of_uint32(get_uint32(d, sbuf, base + 20));
    sections->push_back(std::move(s));
  }
  auto find_section = [&](const char* sectname) -> const Section* {
    for (const Section& s : *sections)
      if (s.name == sectname) return &s;
    return nullptr;
  };
  // read_symbols (the export table flexlink encodes)
  auto symbols = std::make_shared<std::vector<Symbol>>();
  if (const Section* exptbl = find_section(".exptbl")) {
    Bytes ebuf = load_bytes(d, exptbl->pointer_to_raw_data, exptbl->size_of_raw_data);
    long numexports = uint64_to_int("numexports", get_word(d, ebuf, 0));
    long w = word_size(d);
    for (long i = 0; i < numexports; ++i) {
      std::int64_t address = get_word(d, ebuf, w * (2 * i + 1));
      std::int64_t nameoff = get_word(d, ebuf, w * (2 * i + 2));
      std::int64_t off = nameoff - (exptbl->virtual_address + image_base);
      symbols->push_back(Symbol{name_at(ebuf, uint64_to_int("exptbl name offset", off)), address});
    }
  }
  T t;
  if (const Section* data = find_section(".data")) {
    std::int64_t va = data->virtual_address, ptr = data->pointer_to_raw_data;
    t.symbol_offset = [symbols, va, ptr, image_base](const std::string& symname) -> std::optional<std::int64_t> {
      for (const Symbol& s : *symbols)
        if (s.name == symname) return ptr + (s.address - (va + image_base));
      return std::nullopt;
    };
  } else {
    t.symbol_offset = [](const std::string&) -> std::optional<std::int64_t> { return std::nullopt; };
  }
  t.defines_symbol = [symbols](const std::string& symname) {
    for (const Symbol& s : *symbols)
      if (s.name == symname) return true;
    return false;
  };
  return t;
}

}  // namespace flexdll

T read_channel(std::ifstream& ic) {
  ic.clear();
  ic.seekg(0);
  Bytes m = really_input_bytes(ic, 4);
  if (m[0] == 0x7F && m[1] == 'E' && m[2] == 'L' && m[3] == 'F') return elf::read(ic);
  if ((m[0] == 0xFE && m[1] == 0xED && m[2] == 0xFA && (m[3] == 0xCE || m[3] == 0xCF)) ||
      ((m[0] == 0xCE || m[0] == 0xCF) && m[1] == 0xFA && m[2] == 0xED && m[3] == 0xFE))
    return mach_o::read(ic);
  if (m[0] == 'M' && m[1] == 'Z') return flexdll::read(ic);
  throw Raise{{Error::Kind::Unrecognized, std::string(m.begin(), m.end())}};
}

}  // namespace

std::string error_to_string(const Error& e) {
  switch (e.kind) {
    case Error::Kind::Truncated_file: return "Truncated file";
    case Error::Kind::Unrecognized: {
      std::string r = "Unrecognized magic: ";
      for (std::size_t i = 0; i < e.s.size(); ++i) r += (i ? " " : "") + char_to_hex(e.s[i]);
      return r;
    }
    case Error::Kind::Unsupported: {
      char b[64];
      std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(e.n));
      return "Unsupported: " + e.s + ": " + b;
    }
    case Error::Kind::Out_of_range: return "Out of range constant: " + e.s;
  }
  return "";
}

std::variant<T, Error> read(const std::string& filename) {
  std::ifstream ic(filename, std::ios::binary);  // open_in_bin: Sys_error escapes
  if (!ic) throw arg::SysError(filename + ": " + std::strerror(errno));
  try {
    return read_channel(ic);
  } catch (const EndOfFile&) {
    return Error{Error::Kind::Truncated_file, "", 0};
  } catch (const Raise& r) {
    return r.e;
  }
}

}  // namespace cppcaml::typing::binutils
