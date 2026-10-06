// .cmi images: see cmi_image.hpp.
#include "cppcaml/typing/cmi_image.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <elf.h>
#include <link.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cppcaml/os.hpp"

namespace cppcaml::typing::cmi_image {

namespace fs = std::filesystem;

namespace {

// ---- the address space --------------------------------------------------------
// Above a position-independent executable and its heap (0x55..-0x56..), below
// the kernel's mmap area (0x7f.., growing down): the pinned singletons, then
// the image slots, one image per slot.
constexpr std::uintptr_t kPinnedBase = 0x600000000000ULL;
constexpr std::size_t kPinnedSize = 1 << 20;
constexpr std::uintptr_t kSlotsBase = 0x610000000000ULL;
constexpr std::uintptr_t kSlotsEnd = 0x700000000000ULL;
constexpr std::size_t kSlotSize = std::size_t{64} << 20;  // an image's maximum size
constexpr std::uint64_t kNumSlots = (kSlotsEnd - kSlotsBase) / kSlotSize;
#if defined(__APPLE__)
constexpr std::size_t kPage = 16384;  // arm64's (a multiple of x86_64's)
#else
constexpr std::size_t kPage = 4096;
#endif

std::size_t round_up(std::size_t n, std::size_t a) { return (n + a - 1) & ~(a - 1); }

// mmap exactly at [addr] or fail (MAP_FIXED_NOREPLACE; an older kernel ignoring
// the flag places the mapping elsewhere, which is undone here -- as is
// macOS's, which has no such flag but takes a free address as given)
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif
void* map_at(std::uintptr_t addr, std::size_t len, int prot, int flags, int fd, off_t off) {
  void* want = reinterpret_cast<void*>(addr);
  void* p = ::mmap(want, len, prot, flags | MAP_FIXED_NOREPLACE, fd, off);
  if (p == MAP_FAILED) return nullptr;
  if (p != want) {
    ::munmap(p, len);
    return nullptr;
  }
  return p;
}

struct Pinned {
  char* base = nullptr;
  std::size_t off = 0;
};
Pinned& pinned() {
  static Pinned p = [] {
    Pinned q;
    q.base = static_cast<char*>(
        map_at(kPinnedBase, kPinnedSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    return q;
  }();
  return p;
}
bool in_pinned(std::uint64_t w) {
  const Pinned& p = pinned();
  return p.base && w >= reinterpret_cast<std::uint64_t>(p.base) &&
         w < reinterpret_cast<std::uint64_t>(p.base) + kPinnedSize;
}

// ---- a 64-bit hash (integrity and names, not security) ----------------------
std::uint64_t mix(std::uint64_t h, std::uint64_t w) {
  h ^= w;
  h *= 0x9E3779B97F4A7C15ULL;
  return h ^ (h >> 29);
}
std::uint64_t hash_bytes(const void* data, std::size_t n, std::uint64_t h = 0x243F6A8885A308D3ULL) {
  const auto* p = static_cast<const unsigned char*>(data);
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    std::uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = mix(h, w);
  }
  std::uint64_t w = 0;
  std::memcpy(&w, p + i, n - i);
  return mix(mix(h, w), n);
}

// ---- this build's identity: its GNU build-id (Mach-O: its LC_UUID), else the
// executable's stat ----
struct BuildId {
  unsigned char bytes[40] = {};
  std::uint32_t len = 0;
};
#if defined(__APPLE__)
void find_uuid(BuildId& id) {
  const auto* mh = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
  if (!mh || mh->magic != MH_MAGIC_64) return;
  const char* p = reinterpret_cast<const char*>(mh + 1);
  for (std::uint32_t i = 0; i < mh->ncmds; ++i) {
    const auto* lc = reinterpret_cast<const load_command*>(p);
    if (lc->cmd == LC_UUID) {
      const auto* u = reinterpret_cast<const uuid_command*>(lc);
      std::memcpy(id.bytes, u->uuid, sizeof u->uuid);
      id.len = sizeof u->uuid;
      return;
    }
    p += lc->cmdsize;
  }
}
#else
int find_build_id(struct dl_phdr_info* info, std::size_t, void* data) {
  auto* id = static_cast<BuildId*>(data);
  if (info->dlpi_name && info->dlpi_name[0]) return 0;  // the main program only
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_NOTE) continue;
    const char* p = reinterpret_cast<const char*>(info->dlpi_addr + ph.p_vaddr);
    const char* end = p + ph.p_memsz;
    while (p + sizeof(ElfW(Nhdr)) <= end) {
      const auto* nh = reinterpret_cast<const ElfW(Nhdr)*>(p);
      const char* name = p + sizeof(ElfW(Nhdr));
      const char* desc = name + round_up(nh->n_namesz, 4);
      if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && std::memcmp(name, "GNU", 4) == 0 &&
          nh->n_descsz <= sizeof id->bytes) {
        std::memcpy(id->bytes, desc, nh->n_descsz);
        id->len = nh->n_descsz;
        return 1;
      }
      p = desc + round_up(nh->n_descsz, 4);
    }
  }
  return 1;
}
#endif
const BuildId& build_id() {
  static BuildId id = [] {
    BuildId b;
#if defined(__APPLE__)
    find_uuid(b);
#else
    dl_iterate_phdr(find_build_id, &b);
#endif
    if (b.len == 0) {  // no build-id note: the executable file's identity
      struct stat st{};
      if (::stat(os::self_exe().c_str(), &st) == 0) {
        std::uint64_t w[5] = {static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino),
                              static_cast<std::uint64_t>(st.st_size), static_cast<std::uint64_t>(os::mtime(st).tv_sec),
                              static_cast<std::uint64_t>(os::mtime(st).tv_nsec)};
        std::memcpy(b.bytes, w, sizeof w);
        b.len = sizeof w;
      }
    }
    return b;
  }();
  return id;
}

// CPPCAML_CMI_CACHE_DEBUG=1: why an image is not used or not recorded
void debug(const char* what, const std::string& file) {
  static const bool on = [] {
    const char* e = std::getenv("CPPCAML_CMI_CACHE_DEBUG");
    return e && (std::strcmp(e, "1") == 0 || std::strcmp(e, "2") == 0);
  }();
  if (on) std::fprintf(stderr, "c++ocamlc cmi cache: %s: %s\n", file.c_str(), what);
}

// ---- configuration ----------------------------------------------------------------
struct Config {
  bool on = false;
  std::string dir;
  bool verify = false;
  std::uint64_t max_bytes = std::uint64_t{2048} << 20;
};
const Config& config() {
  static Config c = [] {
    Config k;
    const char* e = std::getenv("CPPCAML_CMI_CACHE");
    if (e && std::strcmp(e, "0") == 0) return k;
    if (!pinned().base || build_id().len == 0) {
      debug(!pinned().base ? "no pinned region: cache off" : "no build identity: cache off", "-");
      return k;
    }
    if (e && *e) {
      k.dir = e;
    } else if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) {
      k.dir = std::string(x) + "/c++ocamlc/cmi";
    } else if (const char* h = std::getenv("HOME"); h && *h) {
      k.dir = std::string(h) + "/.cache/c++ocamlc/cmi";
    } else {
      return k;
    }
    k.on = true;
    if (const char* v = std::getenv("CPPCAML_CMI_CACHE_VERIFY"); v && std::strcmp(v, "1") == 0) k.verify = true;
    if (const char* m = std::getenv("CPPCAML_CMI_CACHE_MAX"); m && *m) k.max_bytes = std::strtoull(m, nullptr, 10) << 20;
    return k;
  }();
  return c;
}

// ---- keys and the image header -------------------------------------------------------
struct Key {
  std::uint64_t dev, ino, size;
  std::int64_t mtime_s, mtime_ns, ctime_s, ctime_ns;
};
bool key_of(const std::string& filename, Key& k) {
  struct stat st{};
  if (::stat(filename.c_str(), &st) != 0) return false;
  k = Key{static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino),
          static_cast<std::uint64_t>(st.st_size), os::mtime(st).tv_sec, os::mtime(st).tv_nsec,
          os::ctime(st).tv_sec, os::ctime(st).tv_nsec};
  return true;
}
std::uint64_t key_hash(const Key& k) {
  const BuildId& b = build_id();
  return hash_bytes(&k, sizeof k, hash_bytes(b.bytes, b.len));
}
std::string image_path(std::uint64_t h) {
  char name[32];
  std::snprintf(name, sizeof name, "%016llx.img", static_cast<unsigned long long>(h));
  return config().dir + "/" + name;
}

constexpr char kMagic[8] = {'C', 'X', 'X', 'C', 'M', 'I', 'M', 'G'};
constexpr std::uint32_t kVersion = 1;

struct FlagRec {
  std::uint64_t kind;
  std::uint64_t alerts_root;
};
struct Header {
  char magic[8];
  std::uint32_t version;
  std::uint32_t build_len;
  unsigned char build[40];
  Key key;
  std::uint64_t header_bytes;  // page-rounded: where the data starts in the file
  std::uint64_t base, data_size;
  std::uint64_t name_ptr, name_len, sign_ptr, sign_len;
  std::uint64_t nflags, ncrcs, blob_len;  // then nflags FlagRec, then the crcs blob
  std::uint64_t data_hash;
  std::uint64_t header_hash;  // of the header (this field 0) and what follows it
};

// the crcs: per entry, name length, name, has-digest, digest length, digest
void put_u64(std::string& s, std::uint64_t v) { s.append(reinterpret_cast<const char*>(&v), 8); }
bool get_u64(const char*& p, const char* end, std::uint64_t& v) {
  if (end - p < 8) return false;
  std::memcpy(&v, p, 8);
  p += 8;
  return true;
}
bool get_str(const char*& p, const char* end, std::string& s) {
  std::uint64_t n;
  if (!get_u64(p, end, n) || static_cast<std::uint64_t>(end - p) < n) return false;
  s.assign(p, n);
  p += n;
  return true;
}

bool in_range(std::uint64_t p, std::uint64_t n, std::uint64_t base, std::uint64_t size) {
  return p >= base && n <= size && p - base <= size - n;
}

// ---- eviction: keep the directory under config().max_bytes ---------------------------
void maybe_evict(std::uint64_t salt) {
  if ((salt & 63) != 0) return;  // now and then
  std::error_code ec;
  std::vector<std::pair<fs::file_time_type, std::pair<std::uint64_t, fs::path>>> files;
  std::uint64_t total = 0;
  for (const auto& e : fs::directory_iterator(config().dir, ec)) {
    if (!e.is_regular_file(ec) || e.path().extension() != ".img") continue;
    std::uint64_t sz = e.file_size(ec);
    total += sz;
    files.push_back({e.last_write_time(ec), {sz, e.path()}});
  }
  if (total <= config().max_bytes) return;
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    if (total <= config().max_bytes / 10 * 8) break;
    if (fs::remove(f.second.second, ec)) total -= f.second.first;
  }
}

}  // namespace

// ---- the pinned region -------------------------------------------------------------------
void* pinned_alloc(std::size_t n, std::size_t align) {
  Pinned& p = pinned();
  if (p.base) {
    std::size_t off = round_up(p.off, align);
    if (off + n <= kPinnedSize) {
      p.off = off + n;
      return p.base + off;
    }
  }
  return ::operator new(n, std::align_val_t{align});
}

// ---- loading -----------------------------------------------------------------------------
std::optional<cmi_format::CmiInfos> load(const std::string& filename) {
  const Config& cfg = config();
  if (!cfg.on) return std::nullopt;
  Key k;
  if (!key_of(filename, k)) return std::nullopt;
  std::string path = image_path(key_hash(k));
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  struct CloseFd {
    int fd;
    ~CloseFd() { ::close(fd); }
  } close_fd{fd};
  struct stat st{};
  if (::fstat(fd, &st) != 0) return std::nullopt;
  Header h{};
  if (::pread(fd, &h, sizeof h, 0) != static_cast<ssize_t>(sizeof h)) return std::nullopt;
  const BuildId& b = build_id();
  if (std::memcmp(h.magic, kMagic, sizeof kMagic) != 0 || h.version != kVersion || h.build_len != b.len ||
      std::memcmp(h.build, b.bytes, b.len) != 0 || std::memcmp(&h.key, &k, sizeof k) != 0)
    return std::nullopt;
  if (h.header_bytes % kPage != 0 || h.header_bytes < sizeof h || h.header_bytes > (std::uint64_t{1} << 24) ||
      static_cast<std::uint64_t>(st.st_size) != h.header_bytes + h.data_size || h.data_size == 0 ||
      h.data_size > kSlotSize || h.base < kSlotsBase || h.base >= kSlotsEnd || (h.base - kSlotsBase) % kSlotSize != 0)
    return std::nullopt;
  // the rest of the header, and its checksum
  std::string hdr(h.header_bytes, '\0');
  if (::pread(fd, hdr.data(), hdr.size(), 0) != static_cast<ssize_t>(hdr.size())) return std::nullopt;
  std::uint64_t want = h.header_hash;
  reinterpret_cast<Header*>(hdr.data())->header_hash = 0;
  if (h.nflags > 16 || h.blob_len > hdr.size()) return std::nullopt;
  std::size_t tail = sizeof h + h.nflags * sizeof(FlagRec) + h.blob_len;
  if (tail > hdr.size() || hash_bytes(hdr.data(), tail) != want) return std::nullopt;
  // map the data where it was recorded
  void* m = map_at(h.base, h.data_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, static_cast<off_t>(h.header_bytes));
  if (!m) return std::nullopt;
  auto fail = [&]() -> std::optional<cmi_format::CmiInfos> {
    ::munmap(m, h.data_size);
    return std::nullopt;
  };
  if (cfg.verify && hash_bytes(m, h.data_size) != h.data_hash) return fail();
  if (!in_range(h.name_ptr, h.name_len, h.base, h.data_size) ||
      (h.sign_len && !in_range(h.sign_ptr, h.sign_len * sizeof(const SignatureItem*), h.base, h.data_size)))
    return fail();
  cmi_format::CmiInfos ci;
  ci.cmi_name = std::string_view(reinterpret_cast<const char*>(h.name_ptr), h.name_len);
  ci.cmi_sign = Signature{h.sign_len ? reinterpret_cast<const SignatureItem* const*>(h.sign_ptr) : nullptr,
                          h.sign_len};
  const char* p = hdr.data() + sizeof h;
  for (std::uint64_t i = 0; i < h.nflags; ++i, p += sizeof(FlagRec)) {
    FlagRec fr;
    std::memcpy(&fr, p, sizeof fr);
    if (fr.kind > 2 || (fr.alerts_root && !in_range(fr.alerts_root, 1, h.base, h.data_size))) return fail();
    cmi_format::PersFlag pf{static_cast<cmi_format::PersFlag::Kind>(fr.kind)};
    using Node = StrMap<std::string_view>::Node;
    pf.alerts = StrMap<std::string_view>(reinterpret_cast<const Node*>(fr.alerts_root));
    ci.cmi_flags.push_back(pf);
  }
  const char* end = p + h.blob_len;
  for (std::uint64_t i = 0; i < h.ncrcs; ++i) {
    std::pair<std::string, std::optional<std::string>> e;
    std::uint64_t has;
    if (!get_str(p, end, e.first) || !get_u64(p, end, has)) return fail();
    if (has) {
      std::string d;
      if (!get_str(p, end, d)) return fail();
      e.second = std::move(d);
    }
    ci.cmi_crcs.push_back(std::move(e));
  }
  permanent_zone().adopt(static_cast<const char*>(m), h.data_size);
  return ci;
}

// ---- recording -------------------------------------------------------------------------
struct Recorder {
  std::string filename;
  Key key;
  std::uint64_t hash;
  char* base;
  std::size_t reserved;
  Zone zone;
  Zone* saved = nullptr;
};

Recorder* begin_record(const std::string& filename, std::size_t file_size) {
  if (!config().on) return nullptr;
  Key k;
  if (!key_of(filename, k)) return nullptr;
  std::uint64_t h = key_hash(k);
  // a decoded .cmi takes some tens of bytes per byte of file
  std::size_t reserve = std::min(kSlotSize, round_up(std::max<std::size_t>(file_size * 64, 1 << 20), kPage));
  for (std::uint64_t probe = 0; probe < 8; ++probe) {
    std::uintptr_t base = kSlotsBase + ((h + probe * 0x9E3779B1ULL) % kNumSlots) * kSlotSize;
    void* m = map_at(base, reserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (!m) continue;
    debug("recording", filename);
    auto* r = new Recorder{filename, k, h, static_cast<char*>(m), reserve, {}, nullptr};
    r->zone.use_region(r->base, reserve);
    return r;
  }
  debug("no free slot to record in", filename);
  return nullptr;
}

void enter(Recorder* r) {
  r->saved = &zone();
  set_zone(&r->zone);
}
void leave(Recorder* r) { set_zone(r->saved); }

// A Recorder is never destroyed: its Zone's destructor would run the
// destructors of objects that are in use (or unmapped).
void abandon(Recorder* r) {
  debug("recording abandoned (region full or decode failed)", r->filename);
  ::munmap(r->base, r->reserved);
}

namespace {
// CPPCAML_CMI_CACHE_DEBUG=2: the 8-byte words of a recorded image that look
// like pointers into this process's other mappings.  Only a report: the
// decoded objects reference only one another and the pinned singletons
// (the Reader's constructors copy strings into the zone and borrow nothing
// else), but structs the Reader builds on the stack carry uninitialized
// padding and unused fields -- stack bytes that can look like anything.
std::size_t outside_looking_words(const char* base, std::size_t used) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> maps;
  {
#if defined(__APPLE__)
    mach_vm_address_t a = 0;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    while (::mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
                            reinterpret_cast<vm_region_info_t>(&info), &count, &object) == KERN_SUCCESS) {
      maps.push_back({a, a + sz});
      a += sz;
      count = VM_REGION_BASIC_INFO_COUNT_64;
    }
#else
    std::ifstream in("/proc/self/maps");
    std::string line;
    while (std::getline(in, line)) {
      unsigned long long a = 0, b = 0;
      if (std::sscanf(line.c_str(), "%llx-%llx", &a, &b) == 2) maps.push_back({a, b});
    }
#endif
    std::sort(maps.begin(), maps.end());
  }
  std::uint64_t lo = reinterpret_cast<std::uint64_t>(base), hi = lo + used;
  std::size_t n = 0;
  for (std::size_t off = 0; off + 8 <= used; off += 8) {
    std::uint64_t w;
    std::memcpy(&w, base + off, 8);
    if (w < 4096 || (w >= lo && w < hi) || in_pinned(w)) continue;
    auto it = std::upper_bound(maps.begin(), maps.end(), std::make_pair(w, ~std::uint64_t{0}));
    if (it != maps.begin() && w < std::prev(it)->second) ++n;
  }
  return n;
}
}  // namespace

void commit(Recorder* r, const cmi_format::CmiInfos& ci) {
  std::size_t used = r->zone.region_used();
  std::size_t mapped = round_up(std::max<std::size_t>(used, 1), kPage);
  if (mapped < r->reserved) ::munmap(r->base + mapped, r->reserved - mapped);
  // the objects are in use from here on, whatever becomes of the image
  permanent_zone().adopt(r->base, used);
  std::uint64_t lo = reinterpret_cast<std::uint64_t>(r->base);
  auto inside = [&](const void* p, std::size_t n) {
    return in_range(reinterpret_cast<std::uint64_t>(p), n, lo, used);
  };
  bool ok = used > 0 && inside(ci.cmi_name.data(), ci.cmi_name.size()) &&
            (ci.cmi_sign.empty() || inside(ci.cmi_sign.p, ci.cmi_sign.n * sizeof(const SignatureItem*))) &&
            ci.cmi_flags.size() <= 16;
  for (const auto& f : ci.cmi_flags)
    if (f.alerts.root() && !inside(f.alerts.root(), 1)) ok = false;
  if (!ok) {
    debug("not recorded (roots outside the image)", r->filename);
    return;
  }
  if (const char* d = std::getenv("CPPCAML_CMI_CACHE_DEBUG"); d && std::strcmp(d, "2") == 0)
    std::fprintf(stderr, "c++ocamlc cmi cache: %s: %zu bytes, %zu words looking outside\n", r->filename.c_str(),
                 used, outside_looking_words(r->base, used));
  // the header
  std::string blob;
  for (const auto& [name, crc] : ci.cmi_crcs) {
    put_u64(blob, name.size());
    blob += name;
    put_u64(blob, crc ? 1 : 0);
    if (crc) {
      put_u64(blob, crc->size());
      blob += *crc;
    }
  }
  Header h{};
  std::memcpy(h.magic, kMagic, sizeof kMagic);
  h.version = kVersion;
  const BuildId& b = build_id();
  h.build_len = b.len;
  std::memcpy(h.build, b.bytes, b.len);
  h.key = r->key;
  std::size_t tail = sizeof h + ci.cmi_flags.size() * sizeof(FlagRec) + blob.size();
  h.header_bytes = round_up(tail, kPage);
  h.base = lo;
  h.data_size = used;
  h.name_ptr = reinterpret_cast<std::uint64_t>(ci.cmi_name.data());
  h.name_len = ci.cmi_name.size();
  h.sign_ptr = reinterpret_cast<std::uint64_t>(ci.cmi_sign.p);
  h.sign_len = ci.cmi_sign.n;
  h.nflags = ci.cmi_flags.size();
  h.ncrcs = ci.cmi_crcs.size();
  h.blob_len = blob.size();
  h.data_hash = hash_bytes(r->base, used);
  std::string hdr(h.header_bytes, '\0');
  std::memcpy(hdr.data(), &h, sizeof h);
  char* p = hdr.data() + sizeof h;
  for (const auto& f : ci.cmi_flags) {
    FlagRec fr{static_cast<std::uint64_t>(f.kind), reinterpret_cast<std::uint64_t>(f.alerts.root())};
    std::memcpy(p, &fr, sizeof fr);
    p += sizeof fr;
  }
  std::memcpy(p, blob.data(), blob.size());
  reinterpret_cast<Header*>(hdr.data())->header_hash = hash_bytes(hdr.data(), tail);
  // written to a temporary file and renamed: a reader sees a whole image or none
  std::error_code ec;
  fs::create_directories(config().dir, ec);
  std::string final_path = image_path(r->hash);
  std::string tmp = final_path + ".XXXXXX";
  int fd = ::mkstemp(tmp.data());
  if (fd < 0) debug("cannot create an image file", r->filename);
  if (fd >= 0) {
    bool good = ::write(fd, hdr.data(), hdr.size()) == static_cast<ssize_t>(hdr.size());
    for (std::size_t off = 0; good && off < used;) {
      ssize_t n = ::write(fd, r->base + off, used - off);
      if (n <= 0) good = false;
      else off += static_cast<std::size_t>(n);
    }
    good = (::close(fd) == 0) && good;
    if (!good || ::rename(tmp.c_str(), final_path.c_str()) != 0) {
      debug("image not written", r->filename);
      ::unlink(tmp.c_str());
    }
    else maybe_evict(r->hash >> 7);
  }
}

}  // namespace cppcaml::typing::cmi_image
