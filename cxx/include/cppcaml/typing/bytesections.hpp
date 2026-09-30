// Port of bytecomp/bytesections.ml: the sections of a bytecode executable
// (recording them as they are written, then the table of contents and the
// trailer), over an in-memory output channel.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cppcaml::typing::bytesections {

// An out_channel: the bytes written so far (pos_out = their count)
struct OutChannel {
  std::string buf;
  long pos() const { return static_cast<long>(buf.size()); }
  void output_string(const std::string& s) { buf += s; }
  void output_bytes(const std::vector<std::uint8_t>& b) { buf.append(b.begin(), b.end()); }
  void output_char(char c) { buf += c; }
  void output_byte(int n) { buf += static_cast<char>(n & 0xFF); }
  void output_binary_int(long n) {
    output_byte(static_cast<int>(n >> 24));
    output_byte(static_cast<int>(n >> 16));
    output_byte(static_cast<int>(n >> 8));
    output_byte(static_cast<int>(n));
  }
};

// Name.t: CODE CRCS DATA DBUG DLLS DLPT OSLD PRIM RNTM SYMB | Other
enum class Name { CODE, CRCS, DATA, DBUG, DLLS, DLPT, OSLD, PRIM, RNTM, SYMB };
const char* name_to_string(Name n);

struct SectionEntry {
  std::string name;
  long pos;
  long len;
};

// toc_writer
struct TocWriter {
  std::vector<SectionEntry> section_table_rev;  // (the latest last)
  long section_prev;
  OutChannel* outchan;
};

TocWriter init_record(OutChannel& outchan);
// raises std::invalid_argument when the channel moved backward
void record(TocWriter& t, Name name);
void write_toc_and_trailer(TocWriter& t);

}  // namespace cppcaml::typing::bytesections
