// Port of asmcomp/emitaux.ml (text output).  See emitaux.hpp.
#include "emitaux.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <stdexcept>
#include <tuple>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/hashtbl.hpp"

namespace cppcaml::typing::emitaux {

std::string output;

namespace {
bool config_true(const char* name) {
  std::optional<std::string> v = config::config_var(name);
  return v && *v == "true";
}
}  // namespace

void emit_string(std::string_view s) { output.append(s); }
void emit_int(long n) { output += std::to_string(n); }
void emit_char(char c) { output += c; }
void emit_nativeint(std::int64_t n) { output += std::to_string(n); }
void emit_int32(std::int32_t n) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%" PRIx32, static_cast<std::uint32_t>(n));
  output += b;
}

bool macosx() { return config::system == "macosx"; }

std::string symbol(std::string_view s) {
  std::string r;
  if (macosx()) r += '_';
  for (char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') r += c;
    else if (c == compilenv::symbol_separator()) r += c;
    else {
      char b[8];
      std::snprintf(b, sizeof b, "%02x", static_cast<unsigned char>(c));
      r += compilenv::escape_prefix();
      r += b;
    }
  }
  return r;
}
void emit_symbol(std::string_view s) { output += symbol(s); }

void emit_string_literal(std::string_view s) {
  bool last_was_escape = false;
  emit_string("\"");
  auto octal = [](char c) {
    char b[8];
    std::snprintf(b, sizeof b, "\\%o", static_cast<unsigned char>(c));
    output += b;
  };
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      if (last_was_escape) octal(c);
      else output += c;
    } else if (c >= ' ' && c <= '~' && c != '"' && c != '\\') {
      output += c;
      last_was_escape = false;
    } else {
      octal(c);
      last_was_escape = true;
    }
  }
  emit_string("\"");
}

void emit_string_directive(std::string_view directive, std::string_view s) {
  std::size_t l = s.size();
  if (l == 0) return;
  if (l < 80) {
    emit_string(directive);
    emit_string_literal(s);
    emit_char('\n');
    return;
  }
  for (std::size_t i = 0; i < l;) {
    std::size_t n = std::min<std::size_t>(l - i, 80);
    emit_string(directive);
    emit_string_literal(s.substr(i, n));
    emit_char('\n');
    i += n;
  }
}

void emit_bytes_directive(std::string_view directive, std::string_view s) {
  int pos = 0;
  for (char c : s) {
    if (pos == 0) emit_string(directive);
    else emit_char(',');
    emit_int(static_cast<unsigned char>(c));
    ++pos;
    if (pos >= 16) {
      emit_char('\n');
      pos = 0;
    }
  }
  if (pos > 0) emit_char('\n');
}

void emit_float64_directive(std::string_view directive, std::int64_t x) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%" PRIx64, static_cast<std::uint64_t>(x));
  output += '\t';
  output.append(directive);
  output += '\t';
  output += b;
  output += '\n';
}

void emit_float32_directive(std::string_view directive, std::int32_t x) {
  output += '\t';
  output.append(directive);
  output += '\t';
  emit_int32(x);
  output += '\n';
}

void emit_size_directive(std::string_view symbol) {
  if (config_true("asm_size_type_directives")) {
    emit_string("\t.size\t");
    emit_symbol(symbol);
    emit_string(", . - ");
    emit_symbol(symbol);
    emit_char('\n');
  }
}

void emit_type_directive(std::string_view symbol, std::string_view ty) {
  if (config_true("asm_size_type_directives")) {
    emit_string("\t.type\t");
    emit_symbol(symbol);
    emit_string(", ");
    emit_string(ty);
    emit_char('\n');
  }
}

void emit_nonexecstack_note() {
  if (config_true("with_nonexecstack_note")) emit_string("\t.section .note.GNU-stack,\"\",%progbits\n");
}

// ---- Record live pointers at call points -------------------------------------------------------
namespace {
struct FrameDescr {
  long fd_lbl;                       // Return address
  long fd_frame_size;                // Size of stack frame
  std::vector<long> fd_live_offset;  // Offsets/regs of live addresses
  FrameDebuginfo fd_debuginfo;       // Location, if any
};
reg::NewestFirst<FrameDescr> frame_descriptors;  // list order: the newest first

// a debuginfo list copied out of the function's scratch zone (Asmgen): the
// frame table is emitted at end_assembly
debuginfo::t keep_dbg(const debuginfo::t& d) {
  if (d.empty()) return d;
  ZoneScope perm(permanent_zone());
  return slice(std::vector<debuginfo::Item>(d.begin(), d.end()));
}

// Hashtbl.hash on a Debuginfo item
hashtbl::HValue hv_item(const debuginfo::Item& d) {
  using hashtbl::HValue;
  HValue scopes = d.dinfo_scopes
                      ? HValue::block(0, {HValue::integer(static_cast<long>(d.dinfo_scopes->item)),
                                          HValue::string(std::string(d.dinfo_scopes->str)),
                                          HValue::string(std::string(d.dinfo_scopes->str_fun))})
                      : HValue::integer(0);
  return HValue::block(0, {HValue::string(std::string(d.dinfo_file)), HValue::integer(d.dinfo_line),
                           HValue::integer(d.dinfo_char_start), HValue::integer(d.dinfo_char_end),
                           HValue::integer(d.dinfo_start_bol), HValue::integer(d.dinfo_end_bol),
                           HValue::integer(d.dinfo_end_line), scopes});
}
// Debuginfo.hash t = List.fold_left (fun hash item -> Hashtbl.hash (hash, item)) 0 t
long debuginfo_hash(const std::vector<debuginfo::Item>& t) {
  long h = 0;
  for (const debuginfo::Item& item : t)
    h = hashtbl::hash_value(hashtbl::HValue::block(0, {hashtbl::HValue::integer(h), hv_item(item)}));
  return h;
}

struct StrHash {
  long operator()(const std::string& s) const { return hashtbl::hash_string(s); }
};
using Loc3 = std::tuple<long, long, long>;
struct DefKey {
  std::string filename, defname;
  std::optional<Loc3> loc;
  bool operator==(const DefKey& o) const { return filename == o.filename && defname == o.defname && loc == o.loc; }
};
struct DefHash {
  long operator()(const DefKey& k) const {
    using hashtbl::HValue;
    HValue loc = k.loc ? HValue::block(0, {HValue::block(0, {HValue::integer(std::get<0>(*k.loc)),
                                                              HValue::integer(std::get<1>(*k.loc)),
                                                              HValue::integer(std::get<2>(*k.loc))})})
                       : HValue::integer(0);
    return hashtbl::hash_value(HValue::block(0, {HValue::string(k.filename), HValue::string(k.defname), loc}));
  }
};
struct DbgKey {
  bool rs;
  std::vector<debuginfo::Item> rdbg;
  bool operator==(const DbgKey& o) const { return rs == o.rs && debuginfo::compare(slice(rdbg), slice(o.rdbg)) == 0; }
};
struct DbgHash {
  long operator()(const DbgKey& k) const {
    using hashtbl::HValue;
    return hashtbl::hash_value(HValue::block(0, {HValue::integer(k.rs ? 1 : 0), HValue::integer(debuginfo_hash(k.rdbg))}));
  }
};
}  // namespace

void record_frame_descr(long label, long frame_size, std::vector<long> live_offset, FrameDebuginfo debuginfo) {
  debuginfo.dbg = keep_dbg(debuginfo.dbg);
  for (mach::AllocDbginfo& a : debuginfo.alloc) a.alloc_dbg = keep_dbg(a.alloc_dbg);
  std::sort(live_offset.begin(), live_offset.end());
  live_offset.erase(std::unique(live_offset.begin(), live_offset.end()), live_offset.end());
  frame_descriptors.push_front(FrameDescr{label, frame_size, live_offset, debuginfo});
}

void emit_frames(const EmitFrameActions& a) {
  hashtbl::Hashtbl<std::string, long, StrHash> filenames(7);
  auto label_filename = [&](const std::string& name) {
    if (const long* l = filenames.find_opt(name)) return *l;
    long lbl = cmm::new_label();
    filenames.add(name, lbl);
    return lbl;
  };
  hashtbl::Hashtbl<DefKey, std::pair<long, long>, DefHash> defnames(7);
  auto label_defname = [&](const std::string& filename, const std::string& defname, const std::optional<Loc3>& loc) {
    DefKey key{filename, defname, loc};
    if (const auto* v = defnames.find_opt(key)) return v->second;
    long file_lbl = label_filename(filename);
    long def_lbl = cmm::new_label();
    defnames.add(key, {file_lbl, def_lbl});
    return def_lbl;
  };
  hashtbl::Hashtbl<DbgKey, long, DbgHash> debuginfos(7);
  auto label_debuginfos = [&](bool rs, const debuginfo::t& dbg) {
    DbgKey key{rs, std::vector<debuginfo::Item>(dbg.begin(), dbg.end())};
    std::reverse(key.rdbg.begin(), key.rdbg.end());
    if (const long* l = debuginfos.find_opt(key)) return *l;
    long lbl = cmm::new_label();
    debuginfos.add(key, lbl);
    return lbl;
  };
  auto efa_16_checked = [&](long n) {
    if (n >= 0x10000) throw std::runtime_error("stack frame too large (" + std::to_string(n) + " bytes)");
    a.efa_16(n);
  };
  auto emit_frame = [&](const FrameDescr& fd) {
    long flags;
    switch (fd.fd_debuginfo.k) {
      case FrameDebuginfo::K::Dbg_other:
      case FrameDebuginfo::K::Dbg_raise: flags = debuginfo::is_none(fd.fd_debuginfo.dbg) ? 0 : 1; break;
      default: {
        bool any = false;
        for (auto& d : fd.fd_debuginfo.alloc)
          if (!debuginfo::is_none(d.alloc_dbg)) any = true;
        flags = clflags::debug && any ? 3 : 2;
      }
    }
    a.efa_code_label(fd.fd_lbl);
    efa_16_checked(fd.fd_frame_size + flags);
    efa_16_checked(static_cast<long>(fd.fd_live_offset.size()));
    for (long o : fd.fd_live_offset) efa_16_checked(o);
    if (flags != 0) {
      switch (fd.fd_debuginfo.k) {
        case FrameDebuginfo::K::Dbg_other:
          a.efa_align(4);
          a.efa_label_rel(label_debuginfos(false, fd.fd_debuginfo.dbg), 0);
          break;
        case FrameDebuginfo::K::Dbg_raise:
          a.efa_align(4);
          a.efa_label_rel(label_debuginfos(true, fd.fd_debuginfo.dbg), 0);
          break;
        case FrameDebuginfo::K::Dbg_alloc: {
          a.efa_8(static_cast<long>(fd.fd_debuginfo.alloc.size()));
          for (auto& d : fd.fd_debuginfo.alloc) a.efa_8(d.alloc_words - 2);
          if (flags == 3) {
            a.efa_align(4);
            for (auto& d : fd.fd_debuginfo.alloc) {
              if (debuginfo::is_none(d.alloc_dbg)) a.efa_32(0);
              else a.efa_label_rel(label_debuginfos(false, d.alloc_dbg), 0);
            }
          }
          break;
        }
      }
    }
    a.efa_align(8);  // Arch.size_addr
  };
  auto emit_filename = [&](const std::string& name, long lbl) {
    a.efa_def_label(lbl);
    a.efa_string(name);
  };
  auto emit_defname = [&](const DefKey& key, std::pair<long, long> v) {
    // These must be 32-bit aligned, both because they contain a 32-bit
    // value, and because emit_debuginfo assumes the low 2 bits of their
    // addresses are 0.
    a.efa_align(4);
    a.efa_def_label(v.second);
    a.efa_label_rel(v.first, 0);
    // Include the additional 64-bits of location information which didn't
    // pack in the main 64-bit word
    if (key.loc) {
      a.efa_16(std::get<0>(*key.loc));
      a.efa_16(std::get<1>(*key.loc));
      a.efa_32(static_cast<std::int32_t>(std::get<2>(*key.loc)));
    }
    a.efa_string(key.defname);
  };
  auto sh = [](std::int64_t x, int n) { return static_cast<std::int64_t>(static_cast<std::uint64_t>(x) << n); };
  // See format in caml_debuginfo_location in runtime/backtrace-nat.c
  auto fully_pack_info = [&](bool fd_raise, const debuginfo::Item& d, bool has_next) {
    std::int64_t kind = fd_raise ? 1 : 0, next = has_next ? 1 : 0;
    std::int64_t char_end = d.dinfo_char_end + d.dinfo_start_bol - d.dinfo_end_bol;
    std::int64_t char_end_offset = d.dinfo_end_bol - d.dinfo_start_bol;
    return sh(d.dinfo_line, 51) + sh(d.dinfo_end_line - d.dinfo_line, 48) + sh(d.dinfo_char_start, 42) +
           sh(char_end, 35) + sh(char_end_offset, 26) + sh(kind, 1) + next;
  };
  auto partially_pack_info = [&](bool fd_raise, const debuginfo::Item& d, bool has_next) {
    std::int64_t start_line = std::min<std::int64_t>(0x7FFFF, d.dinfo_line);
    std::int64_t end_line = std::min<std::int64_t>(0x3FFFF, d.dinfo_end_line - d.dinfo_line);
    std::int64_t kind = fd_raise ? 1 : 0, next = has_next ? 1 : 0;
    return sh(1, 63) + sh(start_line, 44) + sh(end_line, 26) + sh(kind, 1) + next;
  };
  auto emit_debuginfo = [&](const DbgKey& key, long lbl) {
    // Due to inlined functions, a single debuginfo may have multiple
    // locations.  These are represented sequentially in memory (innermost
    // frame first), with the low bit of the packed debuginfo being 0 on the
    // last entry.
    a.efa_align(4);
    a.efa_def_label(lbl);
    if (key.rdbg.empty()) throw std::runtime_error("Emitaux.emit_debuginfo");
    bool rs = key.rs;
    for (std::size_t k = 0; k < key.rdbg.size(); ++k) {
      const debuginfo::Item& d = key.rdbg[k];
      bool has_next = k + 1 < key.rdbg.size();
      std::string defname(debuginfo::string_of_scopes(d.dinfo_scopes));
      long char_end = d.dinfo_char_end + d.dinfo_start_bol - d.dinfo_end_bol;
      bool is_fully_packable = d.dinfo_line <= 0xFFF && d.dinfo_end_line - d.dinfo_line <= 0x7 &&
                               d.dinfo_char_start <= 0x3F && char_end <= 0x7F &&
                               d.dinfo_end_bol - d.dinfo_start_bol <= 0x1FF;
      std::int64_t info = is_fully_packable ? fully_pack_info(rs, d, has_next) : partially_pack_info(rs, d, has_next);
      std::optional<Loc3> loc;
      if (!is_fully_packable)
        loc = Loc3{std::min<long>(0xFFFF, d.dinfo_char_start), std::min<long>(0xFFFF, char_end),
                   std::min<long>(0x3FFFFFFF, d.dinfo_char_end)};
      a.efa_label_rel(label_defname(std::string(d.dinfo_file), defname, loc), static_cast<std::int32_t>(info));
      a.efa_32(static_cast<std::int32_t>(info >> 32));
      rs = false;
    }
  };
  a.efa_word(static_cast<long>(frame_descriptors.size()));
  for (const FrameDescr& fd : frame_descriptors) emit_frame(fd);
  for (auto& [k, lbl] : debuginfos.to_seq()) emit_debuginfo(k, lbl);
  for (auto& [n, lbl] : filenames.to_seq()) emit_filename(n, lbl);
  for (auto& [k, v] : defnames.to_seq()) emit_defname(k, v);
  a.efa_align(8);  // Arch.size_addr
  frame_descriptors.clear();
}

bool is_generic_function(std::string_view name) {
  for (std::string_view p : {"caml_apply", "caml_curry", "caml_send", "caml_tuplify"})
    if (name.substr(0, p.size()) == p) return true;
  return false;
}

// ---- CFI directives ----------------------------------------------------------------------------
bool is_cfi_enabled() { return config::asm_cfi_supported; }
void cfi_startproc() {
  if (is_cfi_enabled()) emit_string("\t.cfi_startproc\n");
}
void cfi_endproc() {
  if (is_cfi_enabled()) emit_string("\t.cfi_endproc\n");
}
void cfi_remember_state() {
  if (is_cfi_enabled()) emit_string("\t.cfi_remember_state\n");
}
void cfi_restore_state() {
  if (is_cfi_enabled()) emit_string("\t.cfi_restore_state\n");
}
void cfi_adjust_cfa_offset(long n) {
  if (!is_cfi_enabled()) return;
  emit_string("\t.cfi_adjust_cfa_offset\t");
  emit_int(n);
  emit_string("\n");
}
void cfi_def_cfa_offset(long n) {
  if (!is_cfi_enabled()) return;
  emit_string("\t.cfi_def_cfa_offset\t");
  emit_int(n);
  emit_string("\n");
}
void cfi_offset(long reg, long offset) {
  if (!is_cfi_enabled()) return;
  emit_string("\t.cfi_offset ");
  emit_int(reg);
  emit_string(", ");
  emit_int(offset);
  emit_string("\n");
}
void cfi_def_cfa_register(long reg) {
  if (!is_cfi_enabled()) return;
  emit_string("\t.cfi_def_cfa_register ");
  emit_int(reg);
  emit_string("\n");
}

// ---- Emit debug information ----------------------------------------------------------------------
namespace {
std::vector<std::pair<std::string, long>> file_pos_nums;  // list order: the newest first
long file_pos_num_cnt = 1;                                 // Number of files
}  // namespace

// Reset debug state at beginning of asm file
void reset_debug_info() {
  file_pos_nums.clear();
  file_pos_num_cnt = 1;
}

// We only display .file if the file has not been seen before.  We display
// .loc for every instruction.
void emit_debug_info_gen(const debuginfo::t& dbg,
                         const std::function<void(long file_num, const std::string& file_name)>& file_emitter,
                         const std::function<void(long file_num, long line, long col)>& loc_emitter) {
  if (!(is_cfi_enabled() && (clflags::debug || config::with_frame_pointers))) return;
  if (dbg.empty()) return;
  const debuginfo::Item& d = dbg[dbg.size() - 1];  // (List.rev dbg)'s head
  if (d.dinfo_line <= 0) return;                   // PR#6243
  std::string file_name(d.dinfo_file);
  long file_num = -1;
  for (auto& [n, k] : file_pos_nums)
    if (n == file_name) {
      file_num = k;
      break;
    }
  if (file_num < 0) {
    file_num = file_pos_num_cnt++;
    file_emitter(file_num, file_name);
    file_pos_nums.insert(file_pos_nums.begin(), {file_name, file_num});
  }
  loc_emitter(file_num, d.dinfo_line, d.dinfo_char_start);
}

void emit_debug_info(const debuginfo::t& dbg) {
  emit_debug_info_gen(
      dbg,
      [](long file_num, const std::string& file_name) {
        emit_string("\t.file\t");
        emit_int(file_num);
        emit_char('\t');
        emit_string_literal(file_name);
        emit_char('\n');
      },
      [](long file_num, long line, long) {
        emit_string("\t.loc\t");
        emit_int(file_num);
        emit_char('\t');
        emit_int(line);
        emit_char('\n');
      });
}

void reset() {
  reset_debug_info();
  frame_descriptors.clear();
}

void emit_named_text_section(std::string_view func_name, char prefix_char) {
  if (clflags::function_sections) {
    emit_string("\t.section .text.caml.");
    emit_symbol(func_name);
    emit_char(',');
    emit_string_literal("ax");
    emit_char(',');
    emit_char(prefix_char);
    emit_string("progbits\n");
  } else {
    emit_string("\t.text\n");
  }
}

}  // namespace cppcaml::typing::emitaux
