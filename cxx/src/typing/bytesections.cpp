// Port of bytecomp/bytesections.ml (the writer); see bytesections.hpp.
#include "cppcaml/typing/bytesections.hpp"

#include <stdexcept>

#include "cppcaml/typing/config.hpp"

namespace cppcaml::typing::bytesections {

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
