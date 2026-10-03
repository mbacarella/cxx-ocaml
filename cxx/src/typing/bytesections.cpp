// Port of bytecomp/bytesections.ml (the writer); see bytesections.hpp.
#include "cppcaml/typing/bytesections.hpp"

#include <stdexcept>

#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "cppcaml/typing/arg.hpp"

#include "cppcaml/typing/config.hpp"

namespace cppcaml::typing::bytesections {

namespace {
constexpr std::size_t kChannelBuffer = 65536;  // (an OCaml channel's IO_BUFFER_SIZE)

void write_all(int fd, const char* p, std::size_t n, long at, const std::string& path) {
  while (n > 0) {
    ssize_t k = at < 0 ? ::write(fd, p, n) : ::pwrite(fd, p, n, at);
    if (k <= 0) throw arg::SysError(path + ": " + std::strerror(errno));
    p += k;
    n -= static_cast<std::size_t>(k);
    if (at >= 0) at += k;
  }
}
}  // namespace

void OutChannel::to_file(int file, std::string path) {
  fd = file;
  path_ = std::move(path);
  flushed = 0;
  buf.clear();
}

void OutChannel::output_data(const void* p, std::size_t n) {
  const char* c = static_cast<const char*>(p);
  if (fd >= 0 && buf.size() + n > kChannelBuffer) {
    flush();
    if (n >= kChannelBuffer) {  // (a big chunk: straight to the file)
      write_all(fd, c, n, -1, path_);
      flushed += static_cast<long>(n);
      return;
    }
  }
  buf.append(c, n);
}

void OutChannel::flush() {
  if (fd < 0 || buf.empty()) return;
  write_all(fd, buf.data(), buf.size(), -1, path_);
  flushed += static_cast<long>(buf.size());
  buf.clear();
}

void OutChannel::overwrite(long at, const std::string& bytes) {
  if (fd >= 0) flush();
  if (fd < 0) {
    buf.replace(static_cast<std::size_t>(at), bytes.size(), bytes);
    return;
  }
  write_all(fd, bytes.data(), bytes.size(), at, path_);
}

const char* name_to_string(Name n) {
  switch (n) {
    case Name::CODE: return "CODE";
    case Name::DLPT: return "DLPT";
    case Name::DLLS: return "DLLS";
    case Name::DATA: return "DATA";
    case Name::OSLD: return "OSLD";
    case Name::PRIM: return "PRIM";
    case Name::SYMB: return "SYMB";
    case Name::DBUG: return "DBUG";
    case Name::HINT: return "HINT";
    case Name::CRCS: return "CRCS";
    case Name::RNTM: return "RNTM";
  }
  return "";
}

TocWriter init_record(OutChannel& outchan) { return TocWriter{{}, outchan.pos(), &outchan}; }

void record(TocWriter& t, Name name) {
  long pos = t.outchan->pos();
  if (pos < t.section_prev) throw std::invalid_argument("Bytesections.record: out_channel offset moved backward");
  t.section_table_rev.push_back(SectionEntry{name_to_string(name), t.section_prev, pos - t.section_prev});
  t.section_prev = pos;
}

void write_toc_and_trailer(TocWriter& t) {
  for (const SectionEntry& e : t.section_table_rev) {
    t.outchan->output_string(e.name);
    t.outchan->output_binary_int(e.len);
  }
  t.outchan->output_binary_int(static_cast<long>(t.section_table_rev.size()));
  t.outchan->output_string(config::exec_magic_number);
}

}  // namespace cppcaml::typing::bytesections
