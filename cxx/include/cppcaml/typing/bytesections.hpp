// Port of bytecomp/bytesections.ml: the sections of a bytecode executable
// (recording them as they are written, then the table of contents and the
// trailer), over an in-memory output channel.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cppcaml::typing::bytesections {

// An out_channel: the bytes written so far (pos_out = their count).  In
// memory, or (to_file) a file's: its buffer then goes to the file as it
// fills, as an OCaml channel's, and `buf` holds only the unflushed tail.
struct OutChannel {
  std::string buf;
  int fd = -1;       // to_file's (else in memory)
  long flushed = 0;  // the bytes already in the file
  long pos() const { return flushed + static_cast<long>(buf.size()); }
  // raises arg::SysError (path's) when the file cannot be written
  void to_file(int file, std::string path);
  void output_string(const std::string& s) { output_data(s.data(), s.size()); }
  void output_bytes(const std::vector<std::uint8_t>& b) { output_data(b.data(), b.size()); }
  void output_data(const void* p, std::size_t n);
  void output_char(char c) { output_data(&c, 1); }
  void output_byte(int n) { output_char(static_cast<char>(n & 0xFF)); }
  void output_binary_int(long n) {
    output_byte(static_cast<int>(n >> 24));
    output_byte(static_cast<int>(n >> 16));
    output_byte(static_cast<int>(n >> 8));
    output_byte(static_cast<int>(n));
  }
  // seek_out at; output the bytes; seek_out back to the end
  void overwrite(long at, const std::string& bytes);
  // the file's: write what is buffered
  void flush();

 private:
  std::string path_;
};

// Name.t: CODE CRCS DATA DBUG HINT DLLS DLPT OSLD PRIM RNTM SYMB | Other
enum class Name { CODE, CRCS, DATA, DBUG, HINT, DLLS, DLPT, OSLD, PRIM, RNTM, SYMB };
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
