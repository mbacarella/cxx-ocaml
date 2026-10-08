// An in-process assembler for c++ocamlopt's amd64 output: GNU as 2.42's
// object for the same text, byte for byte.  The parts of gas it follows are
// named where they are used: the frags and their relaxation (write.c
// relax_segment / relax_frag, tc-i386.c md_estimate_size_before_relax), the
// NOP padding (i386_generate_nops), the fixups and their reduction to
// section symbols (adjust_reloc_syms, fixup_segment, tc_i386_fix_adjustable),
// the .eh_frame (dw2gencfi.c), the symbol table (write.c's symbol pruning,
// BFD's elf_map_symbols / swap_out_syms) and the file layout (BFD's
// assign_file_positions_except_relocs, elf-strtab.c's suffix merging).
#include "cppcaml/typing/amd64/x86_assembler.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_map>

namespace cppcaml::typing::x86_assembler {
namespace {

[[noreturn]] void unsupported(const std::string& what) { throw Unsupported(what); }

// ---- registers --------------------------------------------------------------------------------
struct Reg {
  int num = 0;       // 0-15 (the encoding number)
  int width = 0;     // 8 16 32 64; 128 for %xmm
  bool high8 = false;  // %ah %ch %dh %bh (encoded 4-7, no REX)
  bool rex8 = false;   // %spl %bpl %sil %dil (need a REX)
};

const std::unordered_map<std::string_view, Reg>& reg_table() {
  static const std::unordered_map<std::string_view, Reg> t = [] {
    std::unordered_map<std::string_view, Reg> m;
    static const char* r64[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    static const char* r32[] = {"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                                "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
    static const char* r16[] = {"ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
                                "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
    static const char* r8[] = {"al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
                               "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
    static const char* xmm[] = {"xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
                                "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};
    for (int i = 0; i < 16; ++i) {
      m[r64[i]] = Reg{i, 64};
      m[r32[i]] = Reg{i, 32};
      m[r16[i]] = Reg{i, 16};
      m[r8[i]] = Reg{i, 8, false, i >= 4 && i < 8};
      m[xmm[i]] = Reg{i, 128};
    }
    static const char* h8[] = {"ah", "ch", "dh", "bh"};
    for (int i = 0; i < 4; ++i) m[h8[i]] = Reg{4 + i, 8, true};
    return m;
  }();
  return t;
}

// the DWARF register numbers of the 64-bit registers (x86-64 psABI)
int dwarf_reg(int num) {
  static const int d[] = {0, 2, 1, 3, 7, 6, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
  return d[num];
}

// ---- symbols, sections, frags, fixups ---------------------------------------------------------
enum class SymKind : std::uint8_t { Normal, Section, File, Temp };
enum : int { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4 };

struct Sym {
  std::string name;
  SymKind kind = SymKind::Normal;
  int sec = -1;  // -1: undefined
  int frag = 0;
  std::uint64_t off = 0;  // in the frag
  bool global = false;
  int type = STT_NOTYPE;
  // .size: the value of [size_end] - [size_start] + size_const, or a constant
  bool has_size = false;
  bool size_parse_const = false;  // s_size folded it to a constant
  int size_end = -1, size_start = -1;
  std::int64_t size_const = 0;
  bool used_in_reloc = false;
  std::uint32_t out_index = 0;
};

enum class Var : std::uint8_t { None, Align, Branch };

struct Frag {
  std::vector<std::uint8_t> data;  // the fixed part
  Var var = Var::None;
  // Var::Align
  int align_pow = 0;
  bool code_fill = false;
  bool last_insn_normal = true;  // (tc_frag_data.last_insn_normal)
  // Var::Branch: data's last byte is the short opcode (eb / 7x)
  int target = -1;
  bool cond = false;
  bool plt = false;  // @PLT: never resolved here (elf_symbol_resolved_in_segment_p)
  bool long_form = false;
  // relaxation
  std::uint64_t addr = 0;
  int region = 0;
  bool marker = false;

  std::uint64_t var_size() const {
    if (var == Var::Branch) return long_form ? (cond ? 5 : 4) : 1;
    return 0;
  }
};

enum class RType : std::uint8_t { Generic, PLT32, REX_GOTPCRELX, GOTPCRELX };
enum : std::uint32_t {
  R_X86_64_64 = 1,
  R_X86_64_PC32 = 2,
  R_X86_64_PLT32 = 4,
  R_X86_64_32 = 10,
  R_X86_64_16 = 12,
  R_X86_64_8 = 14,
  R_X86_64_PC64 = 24,
  R_X86_64_GOTPCRELX = 41,
  R_X86_64_REX_GOTPCRELX = 42,
};

// The field holds [add] + [off] - [sub], minus its own address when [pcrel]
// (an instruction's pc-relative displacement has its -4 - immediate size in
// [off] already).
struct Fix {
  int frag = 0;
  std::uint32_t where = 0;
  std::uint8_t size = 0;
  bool pcrel = false;
  int add = -1, sub = -1;
  std::int64_t off = 0;
  RType rtype = RType::Generic;
};

struct Rela {
  std::uint64_t offset;
  std::uint32_t type;
  int sym;  // index into syms
  std::int64_t addend;
};

enum : std::uint32_t { SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOTE = 7, SHT_NOBITS = 8 };
enum : std::uint64_t {
  SHF_WRITE = 1,
  SHF_ALLOC = 2,
  SHF_EXECINSTR = 4,
  SHF_MERGE = 0x10,
  SHF_STRINGS = 0x20,
  SHF_INFO_LINK = 0x40
};

struct Section {
  std::string name;
  std::uint32_t type = SHT_PROGBITS;
  std::uint64_t flags = 0;
  std::uint64_t entsize = 0;
  int align_pow = 0;
  std::vector<Frag> frags{1};
  std::vector<Fix> fixes;
  int sym = -1;  // the section symbol
  bool last_insn_directive = false;  // (tc_segment_info_data.last_insn.kind)
  // output
  std::vector<std::uint8_t> bytes;
  std::vector<Rela> relas;
  std::uint64_t size = 0;
  bool code() const { return flags & SHF_EXECINSTR; }
};

// a .cfi_* directive (struct cfi_insn_data)
enum : int {
  DW_CFA_advance_loc = 0x40,
  DW_CFA_offset = 0x80,
  DW_CFA_advance_loc1 = 0x02,
  DW_CFA_advance_loc2 = 0x03,
  DW_CFA_advance_loc4 = 0x04,
  DW_CFA_remember_state = 0x0a,
  DW_CFA_restore_state = 0x0b,
  DW_CFA_def_cfa = 0x0c,
  DW_CFA_def_cfa_register = 0x0d,
  DW_CFA_def_cfa_offset = 0x0e,
  DW_CFA_def_cfa_offset_sf = 0x13,
};
struct CfiInsn {
  int insn;
  int reg = 0;
  std::int64_t offset = 0;
  int lab1 = -1, lab2 = -1;  // DW_CFA_advance_loc: temp symbols
};
struct Fde {
  int start = -1, end = -1;  // temp symbols
  std::vector<CfiInsn> data;
  int last_address = -1;
  std::int64_t cur_cfa_offset = 0;
  std::vector<std::int64_t> cfa_save_stack;
};

// ---- the assembler state ----------------------------------------------------------------------
struct Assembler {
  std::vector<Sym> syms;
  std::vector<int> symbol_list;  // gas's symbol chain (symbol_rootP ...)
  std::unordered_map<std::string, int> by_name;
  std::vector<Section> sections;
  int cur = 0;  // the current section
  int got_symbol = -1;
  std::vector<Fde> fdes;
  int open_fde = -1;

  Assembler() {
    // elf_begin: the known sections' symbols lead the chain
    new_section(".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0);
    new_section(".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 0);
    new_section(".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 0);
  }

  int new_section(std::string name, std::uint32_t type, std::uint64_t flags, std::uint64_t entsize,
                  bool with_symbol = true) {
    Section s;
    s.name = std::move(name);
    s.type = type;
    s.flags = flags;
    s.entsize = entsize;
    sections.push_back(std::move(s));
    int idx = static_cast<int>(sections.size()) - 1;
    if (with_symbol) section_symbol(idx);
    return idx;
  }

  int section_symbol(int sec) {
    Section& s = sections[sec];
    if (s.sym >= 0) return s.sym;
    Sym y;
    y.name = s.name;
    y.kind = SymKind::Section;
    y.sec = sec;
    syms.push_back(std::move(y));
    s.sym = static_cast<int>(syms.size()) - 1;
    symbol_list.push_back(s.sym);
    return s.sym;
  }

  // symbol_find_or_make
  int symbol(std::string_view name) {
    auto it = by_name.find(std::string(name));
    if (it != by_name.end()) return it->second;
    Sym y;
    y.name = std::string(name);
    syms.push_back(std::move(y));
    int idx = static_cast<int>(syms.size()) - 1;
    by_name.emplace(std::string(name), idx);
    // a .L label is a local_symbol, off the chain until local_symbol_convert
    if (!is_local_label(syms[idx])) symbol_list.push_back(idx);
    return idx;
  }

  Frag& frag_now() { return sections[cur].frags.back(); }
  int frag_now_index() const { return static_cast<int>(sections[cur].frags.size()) - 1; }

  // symbol_temp_new_now
  int temp_now() {
    Sym y;
    y.kind = SymKind::Temp;
    y.sec = cur;
    y.frag = frag_now_index();
    y.off = frag_now().data.size();
    syms.push_back(std::move(y));
    return static_cast<int>(syms.size()) - 1;
  }

  void define_label(std::string_view name) {
    int s = symbol(name);
    Sym& y = syms[s];
    if (y.sec >= 0) unsupported("symbol redefined: " + std::string(name));
    y.sec = cur;
    y.frag = frag_now_index();
    y.off = frag_now().data.size();
  }

  // frag_var: close the current frag as a variable one and start a new frag
  Frag& frag_var(Var v) {
    Frag& f = frag_now();
    f.var = v;
    sections[cur].frags.emplace_back();
    return sections[cur].frags[sections[cur].frags.size() - 2];
  }

  void out(std::uint8_t b) { frag_now().data.push_back(b); }
  void out_le(std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) out(static_cast<std::uint8_t>(v >> (8 * i)));
  }

  void add_fix(int size, bool pcrel, int add, int sub, std::int64_t off, RType rtype = RType::Generic) {
    Fix f;
    f.frag = frag_now_index();
    f.where = static_cast<std::uint32_t>(frag_now().data.size());
    f.size = static_cast<std::uint8_t>(size);
    f.pcrel = pcrel;
    f.add = add;
    f.sub = sub;
    f.off = off;
    f.rtype = rtype;
    sections[cur].fixes.push_back(f);
    out_le(0, size);
  }

  void record_alignment(int pow) {
    if (pow > sections[cur].align_pow) sections[cur].align_pow = pow;
  }

  // ---- expressions ----
  struct Expr {
    std::int64_t num = 0;
    int add = -1, sub = -1;
    enum class Mod { None, PLT, GOTPCREL } mod = Mod::None;
  };

  struct Lexer {
    std::string_view s;
    std::size_t i = 0;
    void ws() {
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    }
    bool eof() {
      ws();
      return i >= s.size();
    }
    char peek() {
      ws();
      return i < s.size() ? s[i] : '\0';
    }
    bool eat(char c) {
      if (peek() == c) {
        ++i;
        return true;
      }
      return false;
    }
  };

  static bool name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '$';
  }
  static bool name_char(char c) { return name_start(c) || (c >= '0' && c <= '9'); }

  // a linear combination of symbols, the "." temps included
  struct Lin {
    std::int64_t num = 0;
    std::vector<std::pair<int, int>> terms;  // (symbol, +1/-1)
    void add(int s, int coef) {
      for (auto& t : terms)
        if (t.first == s) {
          t.second += coef;
          return;
        }
      terms.emplace_back(s, coef);
    }
    void scale(int k) {
      num *= k;
      for (auto& t : terms) t.second *= k;
    }
    void merge(const Lin& o) {
      num += o.num;
      for (auto& t : o.terms) add(t.first, t.second);
    }
  };

  Lin parse_primary(Lexer& lx, int& dot) {
    char c = lx.peek();
    Lin r;
    if (c == '(') {
      ++lx.i;
      r = parse_sum(lx, dot);
      if (!lx.eat(')')) unsupported("expression: missing )");
      return r;
    }
    if (c == '-') {
      ++lx.i;
      r = parse_primary(lx, dot);
      r.scale(-1);
      return r;
    }
    if (c == '+') {
      ++lx.i;
      return parse_primary(lx, dot);
    }
    if (c >= '0' && c <= '9') {
      std::size_t j = lx.i;
      std::uint64_t v = 0;
      if (c == '0' && j + 1 < lx.s.size() && (lx.s[j + 1] == 'x' || lx.s[j + 1] == 'X')) {
        j += 2;
        std::size_t st = j;
        while (j < lx.s.size() && std::isxdigit(static_cast<unsigned char>(lx.s[j]))) {
          char d = lx.s[j];
          v = v * 16 + static_cast<std::uint64_t>(d <= '9' ? d - '0' : (d | 0x20) - 'a' + 10);
          ++j;
        }
        if (j == st) unsupported("expression: bad hex");
      } else {
        if (c == '0' && j + 1 < lx.s.size() && lx.s[j + 1] >= '0' && lx.s[j + 1] <= '9')
          unsupported("expression: octal");
        while (j < lx.s.size() && lx.s[j] >= '0' && lx.s[j] <= '9') {
          v = v * 10 + static_cast<std::uint64_t>(lx.s[j] - '0');
          ++j;
        }
      }
      if (j < lx.s.size() && name_char(lx.s[j])) unsupported("expression: number suffix");
      lx.i = j;
      r.num = static_cast<std::int64_t>(v);
      return r;
    }
    if (name_start(c)) {
      std::size_t j = lx.i;
      while (j < lx.s.size() && name_char(lx.s[j])) ++j;
      std::string_view name = lx.s.substr(lx.i, j - lx.i);
      lx.i = j;
      if (name == ".") {
        if (dot < 0) dot = temp_now();
        r.add(dot, 1);
        return r;
      }
      if (lx.i < lx.s.size() && lx.s[lx.i] == '@') {
        // handled by the caller (parse_expr) through lex_got
        unsupported("expression: misplaced @");
      }
      r.add(symbol(name), 1);
      return r;
    }
    unsupported(std::string("expression: ") + std::string(lx.s.substr(lx.i)));
  }

  Lin parse_sum(Lexer& lx, int& dot) {
    Lin r = parse_primary(lx, dot);
    for (;;) {
      char c = lx.peek();
      if (c == '+') {
        ++lx.i;
        r.merge(parse_primary(lx, dot));
      } else if (c == '-') {
        ++lx.i;
        Lin t = parse_primary(lx, dot);
        t.scale(-1);
        r.merge(t);
      } else
        return r;
    }
  }

  Expr to_expr(const Lin& l) {
    Expr e;
    e.num = l.num;
    for (auto& [s, k] : l.terms) {
      if (k == 0) continue;
      if (k == 1 && e.add < 0) e.add = s;
      else if (k == -1 && e.sub < 0) e.sub = s;
      else unsupported("expression: not symbol - symbol + constant");
    }
    if (e.sub >= 0 && e.add < 0) unsupported("expression: - symbol");
    return e;
  }

  // an operand expression, with lex_got's @PLT / @GOTPCREL suffix
  Expr parse_expr(std::string_view s) {
    Expr::Mod mod = Expr::Mod::None;
    std::string buf;
    std::size_t at = s.find('@');
    if (at != std::string_view::npos) {
      std::size_t j = at + 1;
      while (j < s.size() && name_char(s[j])) ++j;
      std::string_view suffix = s.substr(at + 1, j - at - 1);
      if (suffix == "PLT") mod = Expr::Mod::PLT;
      else if (suffix == "GOTPCREL") {
        mod = Expr::Mod::GOTPCREL;
        if (got_symbol < 0) got_symbol = symbol("_GLOBAL_OFFSET_TABLE_");
      } else
        unsupported("relocation @" + std::string(suffix));
      buf = std::string(s.substr(0, at)) + " " + std::string(s.substr(j));
      s = buf;
    }
    Lexer lx{s};
    int dot = -1;
    Lin l = parse_sum(lx, dot);
    if (!lx.eof()) unsupported("expression: trailing " + std::string(s.substr(lx.i)));
    Expr e = to_expr(l);
    e.mod = mod;
    if (mod != Expr::Mod::None && (e.add < 0 || e.sub >= 0)) unsupported("@ on a non-symbol");
    return e;
  }

  // ---- operands ----
  struct Mem {
    int base = -1;  // 16: %rip
    int index = -1;
    int scale = 1;
    Expr disp;
  };
  struct Operand {
    enum class K { Reg, Imm, Mem, Label } k = K::Imm;
    bool star = false;
    Reg reg;
    Expr imm;  // Imm; Label
    Mem mem;
  };

  Reg parse_reg_name(std::string_view s) {
    auto it = reg_table().find(s);
    if (it == reg_table().end()) unsupported("register %" + std::string(s));
    return it->second;
  }

  Operand parse_operand(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    Operand o;
    if (!s.empty() && s.front() == '*') {
      o.star = true;
      s.remove_prefix(1);
    }
    if (s.empty()) unsupported("empty operand");
    if (s.front() == '%') {
      o.k = Operand::K::Reg;
      o.reg = parse_reg_name(s.substr(1));
      return o;
    }
    if (s.front() == '$') {
      o.k = Operand::K::Imm;
      o.imm = parse_expr(s.substr(1));
      if (o.imm.add >= 0) unsupported("symbolic immediate");
      return o;
    }
    if (s.back() == ')') {
      std::size_t lp = s.rfind('(');
      if (lp == std::string_view::npos) unsupported("operand " + std::string(s));
      o.k = Operand::K::Mem;
      std::string_view in = s.substr(lp + 1, s.size() - lp - 2);
      std::string_view disp = s.substr(0, lp);
      // base, index, scale
      std::vector<std::string_view> parts;
      std::size_t st = 0;
      for (std::size_t j = 0; j <= in.size(); ++j)
        if (j == in.size() || in[j] == ',') {
          std::string_view p = in.substr(st, j - st);
          while (!p.empty() && p.front() == ' ') p.remove_prefix(1);
          while (!p.empty() && p.back() == ' ') p.remove_suffix(1);
          parts.push_back(p);
          st = j + 1;
        }
      auto gpr64 = [&](std::string_view r) {
        if (r.size() < 2 || r[0] != '%') unsupported("address register " + std::string(r));
        if (r == "%rip") return 16;
        Reg g = parse_reg_name(r.substr(1));
        if (g.width != 64) unsupported("non-64-bit address register");
        return g.num;
      };
      if (!parts.empty() && !parts[0].empty()) o.mem.base = gpr64(parts[0]);
      if (parts.size() > 1 && !parts[1].empty()) {
        o.mem.index = gpr64(parts[1]);
        if (o.mem.index == 16 || o.mem.index == 4) unsupported("index register");
      }
      if (parts.size() > 2) {
        if (parts[2] == "1") o.mem.scale = 1;
        else if (parts[2] == "2") o.mem.scale = 2;
        else if (parts[2] == "4") o.mem.scale = 4;
        else if (parts[2] == "8") o.mem.scale = 8;
        else unsupported("scale");
      }
      if (parts.size() > 3) unsupported("address");
      if (o.mem.base == 16 && o.mem.index >= 0) unsupported("rip with index");
      bool blank = true;
      for (char c : disp)
        if (c != ' ' && c != '\t') blank = false;
      if (!blank) o.mem.disp = parse_expr(disp);
      if (o.mem.disp.sub >= 0) unsupported("symbol difference displacement");
      if (o.mem.disp.add >= 0 && o.mem.base != 16) unsupported("absolute symbolic address");
      if (o.mem.disp.mod == Expr::Mod::PLT) unsupported("@PLT address");
      return o;
    }
    o.k = Operand::K::Label;
    o.imm = parse_expr(s);
    if (o.imm.sub >= 0 || o.imm.mod == Expr::Mod::GOTPCREL) unsupported("branch target " + std::string(s));
    return o;
  }

  // ---- instruction encoding ----
  struct Enc {
    std::vector<std::uint8_t> prefixes;
    bool w = false;
    bool rex_byte = false;  // a %spl-%dil operand
    bool high_byte = false;  // a %ah-%bh operand
    std::vector<std::uint8_t> opcode;
    int opreg = -1;  // +r encodings: the register in the opcode's low bits
    bool modrm = false;
    int reg = 0;  // ModRM.reg (a register or an extension)
    bool rm_is_reg = true;
    int rm = 0;
    Mem mem;
    int imm_size = 0;
    std::int64_t imm = 0;
    RType rtype = RType::Generic;  // the displacement's (GOTPCREL)
    bool gotpcrel = false;
  };

  static bool fits8(std::int64_t v) { return v >= -128 && v <= 127; }
  static bool fits32(std::int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

  void emit(Enc& e) {
    int rex = 0;
    if (e.w) rex |= 8;
    if (e.modrm && e.reg >= 8) rex |= 4;
    if (e.modrm && !e.rm_is_reg) {
      if (e.mem.index >= 8) rex |= 2;
      if (e.mem.base >= 8 && e.mem.base != 16) rex |= 1;
    } else if (e.modrm && e.rm >= 8)
      rex |= 1;
    if (e.opreg >= 8) rex |= 1;
    if (e.rex_byte) rex |= 0x40;
    if (rex && e.high_byte) unsupported("%ah-%bh with a REX prefix");
    for (std::uint8_t p : e.prefixes) out(p);
    if (rex) out(static_cast<std::uint8_t>(0x40 | (rex & 0xf)));
    for (std::size_t k = 0; k < e.opcode.size(); ++k) {
      std::uint8_t b = e.opcode[k];
      if (k + 1 == e.opcode.size() && e.opreg >= 0) b = static_cast<std::uint8_t>(b + (e.opreg & 7));
      out(b);
    }
    if (e.modrm) {
      int r = (e.reg & 7) << 3;
      if (e.rm_is_reg) {
        out(static_cast<std::uint8_t>(0xc0 | r | (e.rm & 7)));
      } else {
        const Mem& m = e.mem;
        if (m.base == 16) {
          out(static_cast<std::uint8_t>(0x05 | r));
          if (m.disp.add >= 0) {
            RType rt = RType::Generic;
            if (m.disp.mod == Expr::Mod::GOTPCREL) {
              if (!e.gotpcrel) unsupported("@GOTPCREL here");
              rt = rex ? RType::REX_GOTPCRELX : RType::GOTPCRELX;
            }
            add_fix(4, true, m.disp.add, -1, m.disp.num - 4 - e.imm_size, rt);
          } else
            out_le(static_cast<std::uint64_t>(m.disp.num), 4);
        } else if (m.base < 0) {
          // (,%index,scale): SIB with no base, disp32
          if (m.index < 0) unsupported("absolute address");
          out(static_cast<std::uint8_t>(0x04 | r));
          out(static_cast<std::uint8_t>(sib_scale(m.scale) | ((m.index & 7) << 3) | 5));
          out_le(static_cast<std::uint64_t>(m.disp.num), 4);
        } else {
          std::int64_t d = m.disp.num;
          int mod = (d == 0 && (m.base & 7) != 5) ? 0 : fits8(d) ? 1 : 2;
          bool sib = m.index >= 0 || (m.base & 7) == 4;
          if (sib) {
            out(static_cast<std::uint8_t>((mod << 6) | r | 4));
            int idx = m.index >= 0 ? (m.index & 7) : 4;
            out(static_cast<std::uint8_t>(sib_scale(m.index >= 0 ? m.scale : 1) | (idx << 3) | (m.base & 7)));
          } else
            out(static_cast<std::uint8_t>((mod << 6) | r | (m.base & 7)));
          if (mod == 1) out(static_cast<std::uint8_t>(d));
          else if (mod == 2) out_le(static_cast<std::uint64_t>(d), 4);
        }
      }
    }
    if (e.imm_size) out_le(static_cast<std::uint64_t>(e.imm), e.imm_size);
    sections[cur].last_insn_directive = false;
  }

  static std::uint8_t sib_scale(int s) {
    return static_cast<std::uint8_t>((s == 1 ? 0 : s == 2 ? 1 : s == 4 ? 2 : 3) << 6);
  }

  // ModRM.rm from an operand (a register or memory)
  void set_rm(Enc& e, const Operand& o) {
    e.modrm = true;
    if (o.k == Operand::K::Reg) {
      e.rm_is_reg = true;
      e.rm = o.reg.num;
      note_byte_reg(e, o.reg);
    } else if (o.k == Operand::K::Mem) {
      e.rm_is_reg = false;
      e.mem = o.mem;
    } else
      unsupported("operand kind");
  }
  void set_reg(Enc& e, const Reg& r) {
    e.modrm = true;
    e.reg = r.num;
    note_byte_reg(e, r);
  }
  static void note_byte_reg(Enc& e, const Reg& r) {
    if (r.width == 8) {
      if (r.rex8) e.rex_byte = true;
      if (r.high8) e.high_byte = true;
    }
  }
  static void size_prefix(Enc& e, int size) {
    if (size == 16) e.prefixes.push_back(0x66);
    if (size == 64) e.w = true;
  }

  static int cond_code(std::string_view c) {
    static const std::pair<std::string_view, int> t[] = {
        {"o", 0},   {"no", 1},  {"b", 2},   {"c", 2},   {"nae", 2}, {"ae", 3},  {"nb", 3},  {"nc", 3},
        {"e", 4},   {"z", 4},   {"ne", 5},  {"nz", 5},  {"be", 6},  {"na", 6},  {"a", 7},   {"nbe", 7},
        {"s", 8},   {"ns", 9},  {"p", 10},  {"pe", 10}, {"np", 11}, {"po", 11}, {"l", 12},  {"nge", 12},
        {"ge", 13}, {"nl", 13}, {"le", 14}, {"ng", 14}, {"g", 15},  {"nle", 15}};
    for (auto& [n, v] : t)
      if (n == c) return v;
    return -1;
  }

  // jmp / jcc to a label: output_branch's frag_var
  void branch(int cc, const Operand& o) {
    if (o.imm.add < 0 || o.imm.num != 0) unsupported("branch target");
    out(cc < 0 ? 0xeb : static_cast<std::uint8_t>(0x70 + cc));
    Frag& f = frag_var(Var::Branch);
    f.target = o.imm.add;
    f.cond = cc >= 0;
    f.plt = o.imm.mod == Expr::Mod::PLT;
    sections[cur].last_insn_directive = false;
  }

  struct Mnemonic {
    std::string_view base;
    int size;  // 0: from the operands
  };

  // the operand size of an instruction without a suffix: its registers'
  static int size_from_regs(const std::vector<Operand>& ops) {
    int sz = 0;
    for (const Operand& o : ops)
      if (o.k == Operand::K::Reg && o.reg.width <= 64) {
        if (sz && sz != o.reg.width) unsupported("mixed operand sizes");
        sz = o.reg.width;
      }
    return sz;
  }

  void check_ops(const std::vector<Operand>& ops, std::size_t n, std::string_view mn) {
    if (ops.size() != n) unsupported("operand count: " + std::string(mn));
  }

  void instruction(std::string_view mn, std::vector<Operand>& ops) {
    using K = Operand::K;
    // output_insn: the line entry ties to the instruction's start
    dwarf2_emit_insn();
    for (const Operand& o : ops)
      if (o.star && !(mn == "jmp" || mn == "call")) unsupported("* operand");
    Enc e;
    // ---- no operands
    if (ops.empty()) {
      if (mn == "ret") e.opcode = {0xc3};
      else if (mn == "nop") e.opcode = {0x90};
      else if (mn == "cqto" || mn == "cqo") e.w = true, e.opcode = {0x99};
      else if (mn == "cltd" || mn == "cdq") e.opcode = {0x99};
      else if (mn == "cltq" || mn == "cdqe") e.w = true, e.opcode = {0x98};
      else if (mn == "hlt") e.opcode = {0xf4};
      else if (mn == "pause") e.opcode = {0xf3, 0x90};
      else if (mn == "mfence") e.opcode = {0x0f, 0xae, 0xf0};
      else if (mn == "lfence") e.opcode = {0x0f, 0xae, 0xe8};
      else if (mn == "sfence") e.opcode = {0x0f, 0xae, 0xf8};
      else if (mn == "rdtsc") e.opcode = {0x0f, 0x31};
      else if (mn == "rdpmc") e.opcode = {0x0f, 0x33};
      else if (mn == "ud2") e.opcode = {0x0f, 0x0b};
      else unsupported("instruction " + std::string(mn));
      emit(e);
      return;
    }
    // ---- branches
    if (mn == "jmp" || mn == "call") {
      check_ops(ops, 1, mn);
      Operand& o = ops[0];
      if (o.star) {
        if (o.k == K::Reg && o.reg.width != 64) unsupported("indirect via a non-64-bit register");
        if (o.k == K::Mem && o.mem.disp.mod != Expr::Mod::None) unsupported("indirect via @GOTPCREL");
        e.opcode = {0xff};
        e.reg = mn == "jmp" ? 4 : 2;
        set_rm(e, o);
        emit(e);
        return;
      }
      if (o.k != K::Label) unsupported("branch operand");
      if (mn == "jmp") {
        branch(-1, o);
        return;
      }
      if (o.imm.add < 0) unsupported("call to a constant");
      out(0xe8);
      add_fix(4, true, o.imm.add, -1, o.imm.num - 4,
              o.imm.mod == Expr::Mod::PLT ? RType::PLT32 : RType::Generic);
      sections[cur].last_insn_directive = false;
      return;
    }
    if (mn.size() >= 2 && mn[0] == 'j') {
      int cc = cond_code(mn.substr(1));
      if (cc < 0) unsupported("instruction " + std::string(mn));
      check_ops(ops, 1, mn);
      if (ops[0].k != K::Label) unsupported("conditional branch operand");
      branch(cc, ops[0]);
      return;
    }
    if (mn.size() >= 4 && mn.substr(0, 3) == "set") {
      int cc = cond_code(mn.substr(3));
      if (cc >= 0) {
        check_ops(ops, 1, mn);
        if (ops[0].k == K::Reg && ops[0].reg.width != 8) unsupported("setcc operand");
        e.opcode = {0x0f, static_cast<std::uint8_t>(0x90 + cc)};
        e.reg = 0;
        set_rm(e, ops[0]);
        emit(e);
        return;
      }
    }
    // ---- SSE2
    if (sse(mn, ops)) return;
    // ---- the rest: a base and an operand size
    if (special(mn, ops)) return;
    Mnemonic m = split_suffix(mn);
    integer(m, ops, mn);
  }

  // (prefix, opcode) of the scalar-double / packed-double register-memory forms
  bool sse(std::string_view mn, std::vector<Operand>& ops) {
    using K = Operand::K;
    struct Op2 {
      std::string_view name;
      std::uint8_t prefix;
      std::uint8_t op;
    };
    static const Op2 arith[] = {
        {"addsd", 0xf2, 0x58},    {"mulsd", 0xf2, 0x59},    {"subsd", 0xf2, 0x5c},    {"divsd", 0xf2, 0x5e},
        {"sqrtsd", 0xf2, 0x51},   {"minsd", 0xf2, 0x5d},    {"maxsd", 0xf2, 0x5f},    {"cvtsd2ss", 0xf2, 0x5a},
        {"cvtss2sd", 0xf3, 0x5a}, {"ucomisd", 0x66, 0x2e},  {"comisd", 0x66, 0x2f},   {"andpd", 0x66, 0x54},
        {"andnpd", 0x66, 0x55},   {"orpd", 0x66, 0x56},     {"xorpd", 0x66, 0x57},    {"addss", 0xf3, 0x58},
        {"mulss", 0xf3, 0x59},    {"subss", 0xf3, 0x5c},    {"divss", 0xf3, 0x5e},    {"sqrtss", 0xf3, 0x51},
    };
    auto xmm = [](const Operand& o) { return o.k == K::Reg && o.reg.width == 128; };
    auto xmm_or_mem = [&](const Operand& o) { return xmm(o) || o.k == K::Mem; };
    Enc e;
    for (const Op2& a : arith)
      if (a.name == mn) {
        check_ops(ops, 2, mn);
        if (!xmm_or_mem(ops[0]) || !xmm(ops[1])) unsupported("operands of " + std::string(mn));
        e.prefixes = {a.prefix};
        e.opcode = {0x0f, a.op};
        set_reg(e, ops[1].reg);
        set_rm(e, ops[0]);
        emit(e);
        return true;
      }
    static const std::pair<std::string_view, int> cmps[] = {{"cmpeqsd", 0},  {"cmpltsd", 1},  {"cmplesd", 2},
                                                            {"cmpunordsd", 3}, {"cmpneqsd", 4}, {"cmpnltsd", 5},
                                                            {"cmpnlesd", 6}, {"cmpordsd", 7}};
    for (auto& [name, pred] : cmps)
      if (name == mn) {
        check_ops(ops, 2, mn);
        if (!xmm_or_mem(ops[0]) || !xmm(ops[1])) unsupported("operands of " + std::string(mn));
        e.prefixes = {0xf2};
        e.opcode = {0x0f, 0xc2};
        set_reg(e, ops[1].reg);
        set_rm(e, ops[0]);
        e.imm_size = 1;
        e.imm = pred;
        emit(e);
        return true;
      }
    // moves: the load form for register-register (gas's first template)
    struct Mov {
      std::string_view name;
      std::uint8_t prefix, load, store;
    };
    static const Mov movs[] = {{"movsd", 0xf2, 0x10, 0x11},
                               {"movss", 0xf3, 0x10, 0x11},
                               {"movapd", 0x66, 0x28, 0x29},
                               {"movupd", 0x66, 0x10, 0x11}};
    for (const Mov& mv : movs)
      if (mv.name == mn) {
        check_ops(ops, 2, mn);
        e.prefixes = {mv.prefix};
        if (xmm(ops[1]) && xmm_or_mem(ops[0])) {
          e.opcode = {0x0f, mv.load};
          set_reg(e, ops[1].reg);
          set_rm(e, ops[0]);
        } else if (xmm(ops[0]) && ops[1].k == K::Mem) {
          e.opcode = {0x0f, mv.store};
          set_reg(e, ops[0].reg);
          set_rm(e, ops[1]);
        } else
          unsupported("operands of " + std::string(mn));
        emit(e);
        return true;
      }
    if (mn == "movd" || mn == "movq") {
      if (ops.size() != 2) return false;
      if (xmm(ops[0]) && ops[1].k == K::Reg && (ops[1].reg.width == 64 || ops[1].reg.width == 32)) {
        e.prefixes = {0x66};
        e.w = ops[1].reg.width == 64;
        e.opcode = {0x0f, 0x7e};
        set_reg(e, ops[0].reg);
        set_rm(e, ops[1]);
        emit(e);
        return true;
      }
      if (xmm(ops[1]) && ops[0].k == K::Reg && (ops[0].reg.width == 64 || ops[0].reg.width == 32)) {
        e.prefixes = {0x66};
        e.w = ops[0].reg.width == 64;
        e.opcode = {0x0f, 0x6e};
        set_reg(e, ops[1].reg);
        set_rm(e, ops[0]);
        emit(e);
        return true;
      }
      if (mn == "movd" || xmm(ops[0]) || xmm(ops[1])) unsupported("operands of " + std::string(mn));
      return false;
    }
    // conversions with a general register: cvtsi2sd[lq] gpr/mem, xmm; cvttsd2si[lq] xmm/mem, gpr
    auto cvt = [&](std::uint8_t prefix, std::uint8_t op, bool to_xmm, int size) {
      check_ops(ops, 2, mn);
      e.prefixes = {prefix};
      e.opcode = {0x0f, op};
      if (to_xmm) {
        if (!xmm(ops[1]) || xmm(ops[0]) || ops[0].k == K::Imm) unsupported("operands of " + std::string(mn));
        if (ops[0].k == K::Reg && ops[0].reg.width != size) unsupported("operand size of " + std::string(mn));
        set_reg(e, ops[1].reg);
        set_rm(e, ops[0]);
      } else {
        if (!xmm_or_mem(ops[0]) || ops[1].k != K::Reg || ops[1].reg.width != size)
          unsupported("operands of " + std::string(mn));
        set_reg(e, ops[1].reg);
        set_rm(e, ops[0]);
      }
      e.w = size == 64;
      emit(e);
      return true;
    };
    if (mn == "cvtsi2sdq") return cvt(0xf2, 0x2a, true, 64);
    if (mn == "cvtsi2sdl") return cvt(0xf2, 0x2a, true, 32);
    if (mn == "cvttsd2siq") return cvt(0xf2, 0x2c, false, 64);
    if (mn == "cvttsd2sil") return cvt(0xf2, 0x2c, false, 32);
    if (mn == "cvtsd2siq") return cvt(0xf2, 0x2d, false, 64);
    if (mn == "cvtsd2sil") return cvt(0xf2, 0x2d, false, 32);
    return false;
  }

  // movz / movs, movslq, movabsq, bswap, xchg, lock
  bool special(std::string_view mn, std::vector<Operand>& ops) {
    using K = Operand::K;
    Enc e;
    // movz?? / movs?? (movzbq movzwl movsbl movswq ...)
    if (mn.size() == 6 && (mn.substr(0, 4) == "movz" || mn.substr(0, 4) == "movs") &&
        (mn[4] == 'b' || mn[4] == 'w') && (mn[5] == 'w' || mn[5] == 'l' || mn[5] == 'q')) {
      check_ops(ops, 2, mn);
      int from = mn[4] == 'b' ? 8 : 16;
      int to = mn[5] == 'w' ? 16 : mn[5] == 'l' ? 32 : 64;
      if (from >= to) unsupported("instruction " + std::string(mn));
      if (ops[1].k != K::Reg || ops[1].reg.width != to) unsupported("operands of " + std::string(mn));
      if (ops[0].k == K::Reg && ops[0].reg.width != from) unsupported("operands of " + std::string(mn));
      size_prefix(e, to);
      bool z = mn[3] == 'z';
      e.opcode = {0x0f, static_cast<std::uint8_t>((z ? 0xb6 : 0xbe) + (from == 16 ? 1 : 0))};
      set_reg(e, ops[1].reg);
      set_rm(e, ops[0]);
      emit(e);
      return true;
    }
    if (mn == "movslq") {
      check_ops(ops, 2, mn);
      if (ops[1].k != K::Reg || ops[1].reg.width != 64 || (ops[0].k == K::Reg && ops[0].reg.width != 32))
        unsupported("operands of movslq");
      e.w = true;
      e.opcode = {0x63};
      set_reg(e, ops[1].reg);
      set_rm(e, ops[0]);
      emit(e);
      return true;
    }
    if (mn == "movabsq" || mn == "movabs") {
      check_ops(ops, 2, mn);
      if (ops[0].k != K::Imm || ops[1].k != K::Reg || ops[1].reg.width != 64) unsupported("operands of movabsq");
      e.w = true;
      e.opcode = {0xb8};
      e.opreg = ops[1].reg.num;
      e.imm_size = 8;
      e.imm = ops[0].imm.num;
      emit(e);
      return true;
    }
    if (mn == "bswap" || mn == "bswapq" || mn == "bswapl") {
      check_ops(ops, 1, mn);
      if (ops[0].k != K::Reg || ops[0].reg.width < 32 || ops[0].reg.width > 64) unsupported("operand of bswap");
      e.w = ops[0].reg.width == 64;
      e.opcode = {0x0f, 0xc8};
      e.opreg = ops[0].reg.num;
      emit(e);
      return true;
    }
    if (mn == "xchg" || mn == "xchgq" || mn == "xchgl" || mn == "xchgw" || mn == "xchgb") {
      check_ops(ops, 2, mn);
      int size = mn.size() == 5 ? suffix_size(mn[4]) : size_from_regs(ops);
      if (size == 0) unsupported("xchg operand size");
      size_prefix(e, size);
      if (ops[0].k == K::Reg && ops[1].k == K::Reg && size != 8 && (ops[0].reg.num == 0 || ops[1].reg.num == 0)) {
        // the short form with the accumulator
        e.opcode = {0x90};
        e.opreg = ops[0].reg.num == 0 ? ops[1].reg.num : ops[0].reg.num;
        emit(e);
        return true;
      }
      const Operand& r = ops[0].k == K::Reg ? ops[0] : ops[1];
      const Operand& rm = ops[0].k == K::Reg ? ops[1] : ops[0];
      if (r.k != K::Reg) unsupported("operands of xchg");
      e.opcode = {static_cast<std::uint8_t>(size == 8 ? 0x86 : 0x87)};
      set_reg(e, r.reg);
      set_rm(e, rm);
      emit(e);
      return true;
    }
    return false;
  }

  static int suffix_size(char c) {
    switch (c) {
      case 'b': return 8;
      case 'w': return 16;
      case 'l': return 32;
      case 'q': return 64;
    }
    return 0;
  }

  Mnemonic split_suffix(std::string_view mn) {
    static const std::string_view bases[] = {"add", "or",  "adc", "sbb",  "and", "sub", "xor", "cmp", "test",
                                             "mov", "lea", "inc", "dec",  "neg", "not", "mul", "imul", "div",
                                             "idiv", "push", "pop", "sal", "shl", "sar", "shr", "rol", "ror",
                                             "bsf", "bsr", "bts", "xadd", "lock xadd", "popcnt", "lzcnt", "tzcnt"};
    for (std::string_view b : bases)
      if (mn == b) return {b, 0};
    if (!mn.empty()) {
      int sz = suffix_size(mn.back());
      if (sz)
        for (std::string_view b : bases)
          if (mn.substr(0, mn.size() - 1) == b) return {b, sz};
    }
    unsupported("instruction " + std::string(mn));
  }

  void integer(Mnemonic m, std::vector<Operand>& ops, std::string_view mn) {
    using K = Operand::K;
    int size = m.size ? m.size : size_from_regs(ops);
    if (m.base == "push" || m.base == "pop") size = 64;
    if (size == 0) unsupported("operand size of " + std::string(mn));
    for (const Operand& o : ops)
      if (o.k == K::Reg && (o.reg.width == 128 || (o.reg.width != size && m.base != "sal" && m.base != "shl" &&
                                                   m.base != "sar" && m.base != "shr" && m.base != "rol" &&
                                                   m.base != "ror")))
        unsupported("operand size of " + std::string(mn));
    Enc e;
    size_prefix(e, size);
    std::string_view b = m.base;
    if (b == "lock xadd") {
      e.prefixes.insert(e.prefixes.begin(), 0xf0);
      b = "xadd";
    }
    bool byte = size == 8;
    // the ALU group: add or adc sbb and sub xor cmp
    static const std::string_view alu[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
    for (int k = 0; k < 8; ++k)
      if (b == alu[k]) {
        check_ops(ops, 2, mn);
        const Operand &src = ops[0], &dst = ops[1];
        if (src.k == K::Imm) {
          if (dst.k != K::Reg && dst.k != K::Mem) unsupported("operands of " + std::string(mn));
          std::int64_t v = src.imm.num;
          bool acc = dst.k == K::Reg && dst.reg.num == 0 && !dst.reg.high8;
          if (byte) {
            if (acc) {
              e.opcode = {static_cast<std::uint8_t>(k * 8 + 4)};
            } else {
              e.opcode = {0x80};
              e.reg = k;
              set_rm(e, dst);
            }
            e.imm_size = 1;
          } else if (fits8(v)) {
            e.opcode = {0x83};
            e.reg = k;
            set_rm(e, dst);
            e.imm_size = 1;
          } else {
            if (acc) e.opcode = {static_cast<std::uint8_t>(k * 8 + 5)};
            else {
              e.opcode = {0x81};
              e.reg = k;
              set_rm(e, dst);
            }
            e.imm_size = size == 16 ? 2 : 4;
            if (size == 64 && !fits32(v)) unsupported("immediate out of range");
          }
          e.imm = v;
        } else if (src.k == K::Reg) {
          e.opcode = {static_cast<std::uint8_t>(k * 8 + (byte ? 0 : 1))};
          set_reg(e, src.reg);
          set_rm(e, dst);
        } else if (src.k == K::Mem && dst.k == K::Reg) {
          e.opcode = {static_cast<std::uint8_t>(k * 8 + (byte ? 2 : 3))};
          set_reg(e, dst.reg);
          set_rm(e, src);
        } else
          unsupported("operands of " + std::string(mn));
        emit(e);
        return;
      }
    if (b == "test") {
      check_ops(ops, 2, mn);
      const Operand &src = ops[0], &dst = ops[1];
      if (src.k == K::Imm) {
        bool acc = dst.k == K::Reg && dst.reg.num == 0 && !dst.reg.high8;
        if (acc) e.opcode = {static_cast<std::uint8_t>(byte ? 0xa8 : 0xa9)};
        else {
          e.opcode = {static_cast<std::uint8_t>(byte ? 0xf6 : 0xf7)};
          e.reg = 0;
          set_rm(e, dst);
        }
        e.imm_size = byte ? 1 : size == 16 ? 2 : 4;
        e.imm = src.imm.num;
      } else if (src.k == K::Reg || dst.k == K::Reg) {
        const Operand& r = src.k == K::Reg ? src : dst;
        const Operand& rm = src.k == K::Reg ? dst : src;
        e.opcode = {static_cast<std::uint8_t>(byte ? 0x84 : 0x85)};
        set_reg(e, r.reg);
        set_rm(e, rm);
      } else
        unsupported("operands of test");
      emit(e);
      return;
    }
    if (b == "mov") {
      check_ops(ops, 2, mn);
      const Operand &src = ops[0], &dst = ops[1];
      if (src.k == K::Imm) {
        std::int64_t v = src.imm.num;
        if (dst.k == K::Reg && size != 64) {
          e.opcode = {static_cast<std::uint8_t>(byte ? 0xb0 : 0xb8)};
          e.opreg = dst.reg.num;
          note_byte_reg(e, dst.reg);
        } else {
          if (size == 64 && !fits32(v)) unsupported("movq immediate out of range");
          e.opcode = {static_cast<std::uint8_t>(byte ? 0xc6 : 0xc7)};
          e.reg = 0;
          set_rm(e, dst);
        }
        e.imm_size = byte ? 1 : size == 16 ? 2 : 4;
        e.imm = v;
      } else if (src.k == K::Reg) {
        e.opcode = {static_cast<std::uint8_t>(byte ? 0x88 : 0x89)};
        set_reg(e, src.reg);
        set_rm(e, dst);
      } else if (src.k == K::Mem && dst.k == K::Reg) {
        e.opcode = {static_cast<std::uint8_t>(byte ? 0x8a : 0x8b)};
        set_reg(e, dst.reg);
        set_rm(e, src);
        e.gotpcrel = true;
      } else
        unsupported("operands of mov");
      emit(e);
      return;
    }
    if (b == "lea") {
      check_ops(ops, 2, mn);
      if (ops[0].k != K::Mem || ops[1].k != K::Reg || byte) unsupported("operands of lea");
      e.opcode = {0x8d};
      set_reg(e, ops[1].reg);
      set_rm(e, ops[0]);
      emit(e);
      return;
    }
    // the unary groups
    static const std::pair<std::string_view, std::pair<int, int>> unary[] = {
        {"inc", {0xfe, 0}}, {"dec", {0xfe, 1}}, {"not", {0xf6, 2}}, {"neg", {0xf6, 3}},
        {"mul", {0xf6, 4}}, {"div", {0xf6, 6}}, {"idiv", {0xf6, 7}}};
    for (auto& [name, enc] : unary)
      if (b == name) {
        check_ops(ops, 1, mn);
        e.opcode = {static_cast<std::uint8_t>(enc.first + (byte ? 0 : 1))};
        e.reg = enc.second;
        set_rm(e, ops[0]);
        emit(e);
        return;
      }
    if (b == "imul") {
      if (ops.size() == 1) {
        e.opcode = {static_cast<std::uint8_t>(byte ? 0xf6 : 0xf7)};
        e.reg = 5;
        set_rm(e, ops[0]);
      } else if (ops.size() == 2 && ops[0].k == K::Imm && ops[1].k == K::Reg && !byte) {
        std::int64_t v = ops[0].imm.num;
        e.opcode = {static_cast<std::uint8_t>(fits8(v) ? 0x6b : 0x69)};
        set_reg(e, ops[1].reg);
        set_rm(e, ops[1]);
        e.imm_size = fits8(v) ? 1 : size == 16 ? 2 : 4;
        e.imm = v;
      } else if (ops.size() == 2 && ops[1].k == K::Reg && ops[0].k != K::Imm && !byte) {
        e.opcode = {0x0f, 0xaf};
        set_reg(e, ops[1].reg);
        set_rm(e, ops[0]);
      } else
        unsupported("operands of imul");
      emit(e);
      return;
    }
    if (b == "push") {
      check_ops(ops, 1, mn);
      e.w = false;
      const Operand& o = ops[0];
      if (o.k == K::Imm) {
        bool s8 = fits8(o.imm.num);
        e.opcode = {static_cast<std::uint8_t>(s8 ? 0x6a : 0x68)};
        e.imm_size = s8 ? 1 : 4;
        e.imm = o.imm.num;
      } else if (o.k == K::Reg) {
        if (o.reg.width != 64) unsupported("push operand");
        e.opcode = {0x50};
        e.opreg = o.reg.num;
      } else {
        e.opcode = {0xff};
        e.reg = 6;
        set_rm(e, o);
      }
      emit(e);
      return;
    }
    if (b == "pop") {
      check_ops(ops, 1, mn);
      e.w = false;
      const Operand& o = ops[0];
      if (o.k == K::Reg) {
        if (o.reg.width != 64) unsupported("pop operand");
        e.opcode = {0x58};
        e.opreg = o.reg.num;
      } else if (o.k == K::Mem) {
        e.opcode = {0x8f};
        e.reg = 0;
        set_rm(e, o);
      } else
        unsupported("pop operand");
      emit(e);
      return;
    }
    static const std::pair<std::string_view, int> shifts[] = {{"rol", 0}, {"ror", 1}, {"sal", 4},
                                                              {"shl", 4}, {"shr", 5}, {"sar", 7}};
    for (auto& [name, ext] : shifts)
      if (b == name) {
        std::size_t n = ops.size();
        if (n == 0 || n > 2) unsupported("operands of " + std::string(mn));
        const Operand& dst = ops[n - 1];
        if (dst.k == K::Reg && dst.reg.width != size) unsupported("operand size of " + std::string(mn));
        e.reg = ext;
        if (n == 1) e.opcode = {static_cast<std::uint8_t>(byte ? 0xd0 : 0xd1)};
        else if (ops[0].k == K::Reg) {
          if (ops[0].reg.width != 8 || ops[0].reg.num != 1 || ops[0].reg.high8) unsupported("shift count register");
          e.opcode = {static_cast<std::uint8_t>(byte ? 0xd2 : 0xd3)};
        } else if (ops[0].k == K::Imm) {
          if (ops[0].imm.num == 1) e.opcode = {static_cast<std::uint8_t>(byte ? 0xd0 : 0xd1)};
          else {
            e.opcode = {static_cast<std::uint8_t>(byte ? 0xc0 : 0xc1)};
            e.imm_size = 1;
            e.imm = ops[0].imm.num;
          }
        } else
          unsupported("shift count");
        set_rm(e, dst);
        emit(e);
        return;
      }
    if (b == "bsf" || b == "bsr" || b == "popcnt" || b == "lzcnt" || b == "tzcnt") {
      check_ops(ops, 2, mn);
      if (ops[1].k != K::Reg || byte || ops[0].k == K::Imm) unsupported("operands of " + std::string(mn));
      if (b == "popcnt") e.prefixes.insert(e.prefixes.begin(), 0xf3), e.opcode = {0x0f, 0xb8};
      else if (b == "lzcnt") e.prefixes.insert(e.prefixes.begin(), 0xf3), e.opcode = {0x0f, 0xbd};
      else if (b == "tzcnt") e.prefixes.insert(e.prefixes.begin(), 0xf3), e.opcode = {0x0f, 0xbc};
      else e.opcode = {0x0f, static_cast<std::uint8_t>(b == "bsf" ? 0xbc : 0xbd)};
      set_reg(e, ops[1].reg);
      set_rm(e, ops[0]);
      emit(e);
      return;
    }
    if (b == "bts") {
      check_ops(ops, 2, mn);
      if (byte) unsupported("btsb");
      if (ops[0].k == K::Imm) {
        e.opcode = {0x0f, 0xba};
        e.reg = 5;
        set_rm(e, ops[1]);
        e.imm_size = 1;
        e.imm = ops[0].imm.num;
      } else if (ops[0].k == K::Reg) {
        e.opcode = {0x0f, 0xab};
        set_reg(e, ops[0].reg);
        set_rm(e, ops[1]);
      } else
        unsupported("operands of bts");
      emit(e);
      return;
    }
    if (b == "xadd") {
      check_ops(ops, 2, mn);
      if (ops[0].k != K::Reg) unsupported("operands of xadd");
      e.opcode = {0x0f, static_cast<std::uint8_t>(byte ? 0xc0 : 0xc1)};
      set_reg(e, ops[0].reg);
      set_rm(e, ops[1]);
      emit(e);
      return;
    }
    unsupported("instruction " + std::string(mn));
  }

  // ---- directives ----
  static std::vector<std::string_view> split_args(std::string_view s) {
    std::vector<std::string_view> r;
    int depth = 0;
    bool str = false;
    std::size_t st = 0;
    for (std::size_t j = 0; j < s.size(); ++j) {
      char c = s[j];
      if (str) {
        if (c == '\\') ++j;
        else if (c == '"') str = false;
        continue;
      }
      if (c == '"') str = true;
      else if (c == '(') ++depth;
      else if (c == ')') --depth;
      else if (c == ',' && depth == 0) {
        r.push_back(trim(s.substr(st, j - st)));
        st = j + 1;
      }
    }
    r.push_back(trim(s.substr(st)));
    return r;
  }
  static std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
  }

  // i386_cons_align
  void cons_align() {
    if (sections[cur].code()) sections[cur].last_insn_directive = true;
  }

  void cons(int nbytes, std::string_view args) {
    cons_align();
    for (std::string_view a : split_args(args)) {
      Lexer lx{a};
      int dot = -1;
      Lin l = parse_sum(lx, dot);
      if (!lx.eof()) unsupported("expression: " + std::string(a));
      Expr e = to_expr(l);
      if (e.add < 0) out_le(static_cast<std::uint64_t>(e.num), nbytes);
      else add_fix(nbytes, false, e.add, e.sub, e.num);
    }
  }

  void ascii(std::string_view args) {
    cons_align();
    for (std::string_view a : split_args(args)) {
      if (a.size() < 2 || a.front() != '"' || a.back() != '"') unsupported(".ascii operand");
      for (std::size_t j = 1; j + 1 < a.size(); ++j) {
        char c = a[j];
        if (c != '\\') {
          out(static_cast<std::uint8_t>(c));
          continue;
        }
        ++j;
        if (j + 1 >= a.size()) unsupported(".ascii escape");
        c = a[j];
        if (c >= '0' && c <= '7') {
          int v = 0, n = 0;
          while (n < 3 && j + 1 < a.size() && a[j] >= '0' && a[j] <= '7') {
            v = v * 8 + (a[j] - '0');
            ++j;
            ++n;
          }
          --j;
          out(static_cast<std::uint8_t>(v));
        } else if (c == 'n') out('\n');
        else if (c == 't') out('\t');
        else if (c == 'r') out('\r');
        else if (c == 'b') out('\b');
        else if (c == 'f') out('\f');
        else if (c == '\\' || c == '"') out(static_cast<std::uint8_t>(c));
        else unsupported(".ascii escape");
      }
    }
  }

  // s_align_bytes
  void align(std::string_view args) {
    std::vector<std::string_view> a = split_args(args);
    if (a.size() != 1) unsupported(".align with a fill");
    Expr e = parse_expr(a[0]);
    if (e.add >= 0 || e.num <= 0 || (e.num & (e.num - 1))) unsupported(".align operand");
    int pow = 0;
    while ((std::int64_t{1} << pow) < e.num) ++pow;
    bool code = sections[cur].code();
    if (pow) {
      bool normal = !sections[cur].last_insn_directive;
      Frag& f = frag_var(Var::Align);
      f.align_pow = pow;
      f.code_fill = code;
      f.last_insn_normal = normal;
    }
    record_alignment(pow);
  }

  void section_directive(std::string_view args) {
    std::vector<std::string_view> a = split_args(args);
    std::string name(a[0]);
    int existing = -1;
    for (std::size_t k = 0; k < sections.size(); ++k)
      if (sections[k].name == name) existing = static_cast<int>(k);
    std::uint32_t type = SHT_PROGBITS;
    std::uint64_t flags = 0, entsize = 0;
    if (a.size() >= 2) {
      std::string_view f = a[1];
      if (f.size() < 2 || f.front() != '"' || f.back() != '"') unsupported(".section flags");
      for (char c : f.substr(1, f.size() - 2)) switch (c) {
          case 'a': flags |= SHF_ALLOC; break;
          case 'w': flags |= SHF_WRITE; break;
          case 'x': flags |= SHF_EXECINSTR; break;
          case 'M': flags |= SHF_MERGE; break;
          case 'S': flags |= SHF_STRINGS; break;
          default: unsupported(".section flag");
        }
      if (a.size() >= 3) {
        std::string_view t = a[2];
        if (t == "@progbits" || t == "%progbits") type = SHT_PROGBITS;
        else if (t == "@nobits" || t == "%nobits") type = SHT_NOBITS;
        else if (t == "@note" || t == "%note") type = SHT_NOTE;
        else unsupported(".section type");
      }
      if (flags & SHF_MERGE) {
        if (a.size() < 4) unsupported(".section entsize");
        Expr e = parse_expr(a[3]);
        if (e.add >= 0) unsupported(".section entsize");
        entsize = static_cast<std::uint64_t>(e.num);
      } else if (a.size() > 3)
        unsupported(".section arguments");
    } else {
      // _bfd_elf_get_sec_type_attr's special sections
      auto prefix = [&](std::string_view p) {
        return name == p || (name.size() > p.size() && name.compare(0, p.size(), p) == 0 && name[p.size()] == '.');
      };
      if (prefix(".rodata")) flags = SHF_ALLOC;
      else if (prefix(".text")) flags = SHF_ALLOC | SHF_EXECINSTR;
      else if (prefix(".data")) flags = SHF_ALLOC | SHF_WRITE;
      else if (prefix(".bss")) flags = SHF_ALLOC | SHF_WRITE, type = SHT_NOBITS;
      else if (existing < 0) unsupported(".section " + name);
    }
    if (existing >= 0) {
      Section& s = sections[existing];
      if (a.size() >= 2 && (s.flags != flags || s.type != type)) unsupported("section attributes changed");
      cur = existing;
      return;
    }
    cur = new_section(name, type, flags, entsize);
  }

  void size_directive(std::string_view args) {
    std::vector<std::string_view> a = split_args(args);
    if (a.size() != 2) unsupported(".size");
    int s = symbol(a[0]);
    Lexer lx{a[1]};
    int dot = -1;
    Lin l = parse_sum(lx, dot);
    if (!lx.eof()) unsupported(".size expression");
    Expr e = to_expr(l);
    Sym& y = syms[s];
    y.has_size = true;
    y.size_parse_const = e.add < 0 || (e.sub >= 0 && syms[e.add].sec >= 0 && syms[e.add].sec == syms[e.sub].sec &&
                                       syms[e.add].frag == syms[e.sub].frag);
    y.size_end = e.add;
    y.size_start = e.sub;
    y.size_const = e.num;
  }

  void type_directive(std::string_view args) {
    std::vector<std::string_view> a = split_args(args);
    if (a.size() != 2) unsupported(".type");
    int s = symbol(a[0]);
    if (a[1] == "@function" || a[1] == "%function" || a[1] == "STT_FUNC") syms[s].type = STT_FUNC;
    else if (a[1] == "@object" || a[1] == "%object" || a[1] == "STT_OBJECT") syms[s].type = STT_OBJECT;
    else if (a[1] == "@notype" || a[1] == "%notype") syms[s].type = STT_NOTYPE;
    else unsupported(".type " + std::string(a[1]));
  }

  // dot_cfi
  Fde& cfi_fde() {
    if (open_fde < 0) unsupported("CFI outside .cfi_startproc");
    return fdes[open_fde];
  }
  bool at_last_address(const Fde& f) {
    const Sym& l = syms[f.last_address];
    return l.sec == cur && l.frag == frag_now_index() && l.off == frag_now().data.size();
  }
  void cfi_advance(Fde& f) {
    if (!at_last_address(f)) {
      int lab = temp_now();
      CfiInsn i{DW_CFA_advance_loc};
      i.lab1 = f.last_address;
      i.lab2 = lab;
      f.data.push_back(i);
      f.last_address = lab;
    }
  }
  int cfi_reg(std::string_view s) {
    s = trim(s);
    if (!s.empty() && s[0] == '%') {
      Reg r = parse_reg_name(s.substr(1));
      if (r.width != 64) unsupported("CFI register");
      return dwarf_reg(r.num);
    }
    Expr e = parse_expr(s);
    if (e.add >= 0) unsupported("CFI register");
    return static_cast<int>(e.num);
  }
  std::int64_t cfi_const(std::string_view s) {
    Expr e = parse_expr(s);
    if (e.add >= 0) unsupported("CFI constant");
    return e.num;
  }

  void cfi(std::string_view d, std::string_view args) {
    if (d == ".cfi_startproc") {
      if (open_fde >= 0) unsupported("nested .cfi_startproc");
      if (!trim(args).empty()) unsupported(".cfi_startproc simple");
      Fde f;
      f.start = temp_now();
      f.last_address = f.start;
      // tc_cfi_frame_initial_instructions: def_cfa %rsp, 8; offset %rip at cfa-8
      f.data.push_back(CfiInsn{DW_CFA_def_cfa, 7, 8});
      f.cur_cfa_offset = 8;
      f.data.push_back(CfiInsn{DW_CFA_offset, 16, -8});
      fdes.push_back(std::move(f));
      open_fde = static_cast<int>(fdes.size()) - 1;
      return;
    }
    if (d == ".cfi_endproc") {
      Fde& f = cfi_fde();
      f.end = temp_now();
      open_fde = -1;
      return;
    }
    Fde& f = cfi_fde();
    std::vector<std::string_view> a = split_args(args);
    if (d == ".cfi_adjust_cfa_offset") {
      cfi_advance(f);
      f.cur_cfa_offset += cfi_const(a[0]);
      f.data.push_back(CfiInsn{DW_CFA_def_cfa_offset, 0, f.cur_cfa_offset});
    } else if (d == ".cfi_def_cfa_offset") {
      cfi_advance(f);
      f.cur_cfa_offset = cfi_const(a[0]);
      f.data.push_back(CfiInsn{DW_CFA_def_cfa_offset, 0, f.cur_cfa_offset});
    } else if (d == ".cfi_def_cfa_register") {
      cfi_advance(f);
      f.data.push_back(CfiInsn{DW_CFA_def_cfa_register, cfi_reg(a[0])});
    } else if (d == ".cfi_def_cfa") {
      if (a.size() != 2) unsupported(".cfi_def_cfa");
      cfi_advance(f);
      f.cur_cfa_offset = cfi_const(a[1]);
      f.data.push_back(CfiInsn{DW_CFA_def_cfa, cfi_reg(a[0]), f.cur_cfa_offset});
    } else if (d == ".cfi_offset") {
      if (a.size() != 2) unsupported(".cfi_offset");
      cfi_advance(f);
      f.data.push_back(CfiInsn{DW_CFA_offset, cfi_reg(a[0]), cfi_const(a[1])});
    } else if (d == ".cfi_remember_state") {
      cfi_advance(f);
      f.data.push_back(CfiInsn{DW_CFA_remember_state});
      f.cfa_save_stack.push_back(f.cur_cfa_offset);
    } else if (d == ".cfi_restore_state") {
      cfi_advance(f);
      f.data.push_back(CfiInsn{DW_CFA_restore_state});
      if (f.cfa_save_stack.empty()) unsupported("CFI state restore without remember");
      f.cur_cfa_offset = f.cfa_save_stack.back();
      f.cfa_save_stack.pop_back();
    } else
      unsupported(std::string(d));
  }

  void directive(std::string_view d, std::string_view args) {
    if (d == ".text") {
      cur = 0;
      return;
    }
    if (d == ".data") {
      cur = 1;
      return;
    }
    if (d == ".section") return section_directive(args);
    if (d == ".align" || d == ".balign") return align(args);
    if (d == ".byte") return cons(1, args);
    if (d == ".word" || d == ".short" || d == ".value") return cons(2, args);
    if (d == ".long" || d == ".int") return cons(4, args);
    if (d == ".quad") return cons(8, args);
    if (d == ".ascii") return ascii(args);
    if (d == ".space" || d == ".skip") {
      cons_align();
      Expr e = parse_expr(args);
      if (e.add >= 0 || e.num < 0) unsupported(".space");
      for (std::int64_t k = 0; k < e.num; ++k) out(0);
      return;
    }
    if (d == ".globl" || d == ".global") {
      for (std::string_view a : split_args(args)) syms[symbol(a)].global = true;
      return;
    }
    if (d == ".type") return type_directive(args);
    if (d == ".size") return size_directive(args);
    if (d == ".loc") return loc_directive(args);
    if (d == ".file") {
      std::string_view a = trim(args);
      if (!a.empty() && a[0] >= '0' && a[0] <= '9') return file_directive(a);
      if (a != "\"\"") unsupported(".file " + std::string(a));
      if (file_sym < 0) {
        Sym y;
        y.kind = SymKind::File;
        syms.push_back(std::move(y));
        file_sym = static_cast<int>(syms.size()) - 1;
        // elf_file_symbol: moved to the front of the chain
        symbol_list.insert(symbol_list.begin(), file_sym);
      }
      return;
    }
    if (d.substr(0, 5) == ".cfi_") return cfi(d, args);
    unsupported("directive " + std::string(d));
  }
  int file_sym = -1;

  // ---- line numbers (dwarf2dbg.c) ----
  struct LineEntry {
    int label;
    unsigned filenum, line, column;
  };
  struct LineSeg {
    int sec;
    std::vector<LineEntry> entries;
  };
  std::vector<LineSeg> line_segs;  // all_segs: in the order of their first entries
  std::vector<int> line_seg_of;    // section -> line_segs index, -1
  struct FileEntry {
    std::optional<std::string> filename;
    unsigned dir = 0;
  };
  std::vector<FileEntry> files;                  // files_in_use = size
  std::vector<std::optional<std::string>> dirs;  // dirs_in_use = size
  unsigned cur_filenum = 1, cur_line = 1, cur_column = 0;
  bool loc_directive_seen = false;

  // dwarf2_emit_insn + dwarf2_gen_line_info
  void dwarf2_emit_insn() {
    if (!loc_directive_seen) return;
    loc_directive_seen = false;  // dwarf2_consume_line_info
    if (cur_line == 0) return;
    const Section& s = sections[cur];
    if ((s.flags & (SHF_ALLOC | SHF_EXECINSTR)) != (SHF_ALLOC | SHF_EXECINSTR) || s.type == SHT_NOBITS) return;
    if (line_seg_of.size() <= static_cast<std::size_t>(cur)) line_seg_of.resize(cur + 1, -1);
    if (line_seg_of[cur] < 0) {
      line_seg_of[cur] = static_cast<int>(line_segs.size());
      line_segs.push_back(LineSeg{cur, {}});
    }
    line_segs[line_seg_of[cur]].entries.push_back(LineEntry{temp_now(), cur_filenum, cur_line, cur_column});
  }

  std::string c_string(std::string_view& a) {
    a = trim(a);
    if (a.empty() || a[0] != '"') unsupported("string expected");
    std::string r;
    std::size_t j = 1;
    for (; j < a.size() && a[j] != '"'; ++j) {
      char c = a[j];
      if (c != '\\') {
        r += c;
        continue;
      }
      if (++j >= a.size()) unsupported("string escape");
      c = a[j];
      if (c >= '0' && c <= '7') {
        int v = 0, n = 0;
        while (n < 3 && j < a.size() && a[j] >= '0' && a[j] <= '7') v = v * 8 + (a[j++] - '0'), ++n;
        --j;
        r += static_cast<char>(v);
      } else if (c == 'n') r += '\n';
      else if (c == 't') r += '\t';
      else if (c == '\\' || c == '"') r += c;
      else unsupported("string escape");
    }
    if (j >= a.size()) unsupported("unterminated string");
    a.remove_prefix(j + 1);
    return r;
  }

  // get_directory_table_entry (can_use_zero false, no file0 dirname)
  unsigned directory_entry(std::string_view dirname) {
    std::size_t dirlen = dirname.size();
    if (dirlen == 0) return 0;
    if (dirname[dirlen - 1] == '/') {
      --dirlen;
      if (dirlen == 0) return 0;
    }
    dirname = dirname.substr(0, dirlen);
    unsigned d = 0;
    for (; d < dirs.size(); ++d)
      if (dirs[d] && *dirs[d] == dirname) return d;
    if (d == 0) d = 1;
    if (dirs.size() <= d) dirs.resize(d + 1);
    dirs[d] = std::string(dirname);
    return d;
  }

  // dwarf2_directive_filename + allocate_filename_to_slot (no dirname, no md5)
  void file_directive(std::string_view a) {
    std::size_t j = 0;
    while (j < a.size() && a[j] >= '0' && a[j] <= '9') ++j;
    unsigned long num = std::stoul(std::string(a.substr(0, j)));
    if (num < 1) unsupported(".file 0");
    a.remove_prefix(j);
    std::string filename = c_string(a);
    if (!trim(a).empty()) unsupported(".file with a directory or md5");
    // get_basename
    std::size_t slash = filename.rfind('/');
    std::size_t base = slash == std::string::npos ? 0 : slash + 1;
    if (base == 1) base = 0;
    std::string file = filename.substr(base);
    if (num < files.size() && files[num].filename) {
      const std::optional<std::string>& dir = files[num].dir < dirs.size() ? dirs[files[num].dir] : std::nullopt;
      if (dir) {
        if (filename.size() > dir->size() && filename.compare(0, dir->size(), *dir) == 0 &&
            filename[dir->size()] == '/' && filename.substr(dir->size() + 1) == *files[num].filename)
          return;
      } else if (file == *files[num].filename && base == 0)
        return;
      unsupported("file table slot reused");
    }
    unsigned d = directory_entry(std::string_view(filename).substr(0, base));
    if (files.size() <= num) files.resize(num + 1);
    files[num].filename = file;
    files[num].dir = d;
  }

  // dwarf2_directive_loc
  void loc_directive(std::string_view a) {
    if (loc_directive_seen) dwarf2_emit_insn();
    std::vector<std::string_view> w;
    for (std::size_t j = 0; j < a.size();) {
      while (j < a.size() && (a[j] == ' ' || a[j] == '\t')) ++j;
      std::size_t st = j;
      while (j < a.size() && a[j] != ' ' && a[j] != '\t') ++j;
      if (j > st) w.push_back(a.substr(st, j - st));
    }
    auto num = [&](std::string_view t) {
      if (t.empty()) unsupported(".loc");
      for (char c : t)
        if (c < '0' || c > '9') unsupported(".loc " + std::string(t));
      return static_cast<unsigned>(std::stoul(std::string(t)));
    };
    if (w.size() < 2 || w.size() > 3) unsupported(".loc");
    unsigned filenum = num(w[0]), line = num(w[1]);
    if (filenum < 1 || filenum >= files.size() || !files[filenum].filename) unsupported("unassigned file number");
    cur_filenum = filenum;
    cur_line = line;
    if (w.size() == 3) cur_column = num(w[2]);
    loc_directive_seen = true;
  }

  // ---- the input ----
  void line(std::string_view s) {
    for (;;) {
      s = trim(s);
      if (s.empty()) return;
      // a label
      std::size_t j = 0;
      while (j < s.size() && name_char(s[j])) ++j;
      if (j > 0 && j < s.size() && s[j] == ':' && !(s[0] >= '0' && s[0] <= '9')) {
        define_label(s.substr(0, j));
        s.remove_prefix(j + 1);
        continue;
      }
      break;
    }
    if (s[0] == '.') {
      std::size_t j = 0;
      while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
      directive(s.substr(0, j), s.substr(j));
      return;
    }
    std::size_t j = 0;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
    std::string_view mn = s.substr(0, j);
    std::string_view rest = trim(s.substr(j));
    std::string lock_mn;
    if (mn == "lock") {
      std::size_t k = 0;
      while (k < rest.size() && rest[k] != ' ' && rest[k] != '\t') ++k;
      lock_mn = "lock " + std::string(rest.substr(0, k));
      mn = lock_mn;
      rest = trim(rest.substr(k));
    }
    std::vector<Operand> ops;
    if (!rest.empty())
      for (std::string_view a : split_args(rest)) ops.push_back(parse_operand(a));
    instruction(mn, ops);
  }

  void parse(std::string_view text) {
    std::string buf;
    std::size_t p = 0;
    bool in_comment = false;
    while (p < text.size()) {
      std::size_t e = text.find('\n', p);
      if (e == std::string_view::npos) e = text.size();
      std::string_view l = text.substr(p, e - p);
      p = e + 1;
      // strip /* */ and # comments outside strings
      buf.clear();
      bool str = false;
      for (std::size_t k = 0; k < l.size(); ++k) {
        char c = l[k];
        if (in_comment) {
          if (c == '*' && k + 1 < l.size() && l[k + 1] == '/') in_comment = false, ++k;
          continue;
        }
        if (str) {
          buf += c;
          if (c == '\\' && k + 1 < l.size()) buf += l[++k];
          else if (c == '"') str = false;
          continue;
        }
        if (c == '"') str = true, buf += c;
        else if (c == '/' && k + 1 < l.size() && l[k + 1] == '*') in_comment = true, ++k;
        else if (c == '#') break;
        else if (c == ';') unsupported("; separator");
        else buf += c;
      }
      line(buf);
    }
    if (open_fde >= 0) unsupported("missing .cfi_endproc");
  }

  // ---- relaxation (write.c relax_segment) ----
  std::uint64_t sym_value(int s) const {
    const Sym& y = syms[s];
    return sections[y.sec].frags[y.frag].addr + y.off;
  }
  static std::uint64_t relax_align(std::uint64_t address, int pow) {
    std::uint64_t mask = ~(~std::uint64_t{0} << pow);
    return ((address + mask) & ~mask) - address;
  }

  // md_estimate_size_before_relax: a branch that cannot be resolved here is
  // made long, with its fixup appended now
  bool branch_resolved_here(const Frag& f, int sec) const {
    // local, or global without @PLT (elf_symbol_resolved_in_segment_p)
    return !f.plt && syms[f.target].sec == sec;
  }

  void relax_section(int sec) {
    Section& s = sections[sec];
    std::uint64_t address = 0;
    int region = 0;
    for (std::size_t k = 0; k < s.frags.size(); ++k) {
      Frag& f = s.frags[k];
      if (f.var == Var::Branch && !branch_resolved_here(f, sec)) {
        // the forced long form, its fixup at the end of the section's list
        const Sym& t = syms[f.target];
        bool plt = f.plt;
        std::uint8_t opc = f.data.back();
        if (f.cond) {
          f.data.back() = 0x0f;
          f.data.push_back(static_cast<std::uint8_t>(opc + 0x10));
        } else
          f.data.back() = 0xe9;
        Fix x;
        x.frag = static_cast<int>(k);
        x.where = static_cast<std::uint32_t>(f.data.size());
        x.size = 4;
        x.pcrel = true;
        x.add = f.target;
        x.off = -4;
        // need_plt32_p: undefined or global
        x.rtype = plt || t.sec < 0 || t.global ? RType::PLT32 : RType::Generic;
        s.fixes.push_back(x);
        for (int b = 0; b < 4; ++b) f.data.push_back(0);
        f.var = Var::None;
        f.long_form = false;
      }
      f.region = region;
      f.marker = false;
      f.addr = address;
      address += f.data.size();
      if (f.var == Var::Align) {
        address += relax_align(address, f.align_pow);
        region += 1;
      } else
        address += f.var_size();
    }
    for (;;) {
      std::int64_t stretch = 0;
      bool stretched = false;
      for (Frag& f : s.frags) {
        std::int64_t growth = 0;
        f.marker = !f.marker;
        std::uint64_t was = f.addr;
        f.addr += static_cast<std::uint64_t>(stretch);
        std::uint64_t addr = f.addr;
        if (f.var == Var::Align) {
          std::uint64_t oldoff = relax_align(was + f.data.size(), f.align_pow);
          std::uint64_t newoff = relax_align(addr + f.data.size(), f.align_pow);
          growth = static_cast<std::int64_t>(newoff) - static_cast<std::int64_t>(oldoff);
        } else if (f.var == Var::Branch)
          growth = relax_frag(f, stretch);
        if (growth) {
          stretch += growth;
          stretched = true;
        }
      }
      if (!stretched) break;
    }
    s.size = 0;
    if (!s.frags.empty()) {
      const Frag& l = s.frags.back();
      s.size = l.addr + l.data.size() + (l.var == Var::Align ? relax_align(l.addr + l.data.size(), l.align_pow)
                                                             : l.var_size());
    }
  }

  // relax_frag with md_relax_table's short (reach 127+1 / -128+1) and long states
  std::int64_t relax_frag(Frag& f, std::int64_t stretch) {
    if (f.long_form) return 0;
    const Sym& t = syms[f.target];
    const Frag& tf = sections[t.sec].frags[t.frag];
    std::int64_t target = static_cast<std::int64_t>(tf.addr + t.off);
    std::int64_t address = static_cast<std::int64_t>(f.addr + f.data.size());
    if (stretch != 0 && tf.marker != f.marker) {
      if (stretch < 0 || tf.region == f.region) target += stretch;
      else if (target < address) return 0;
    }
    std::int64_t aim = target - address;
    bool fits = aim < 0 ? aim >= -128 + 1 : aim <= 127 + 1;
    if (fits) return 0;
    f.long_form = true;
    return f.cond ? 4 : 3;
  }

  // ---- the section contents (md_convert_frag, i386_generate_nops) ----
  static void nops(std::vector<std::uint8_t>& out, std::uint64_t count, bool last_insn_normal) {
    static const std::uint8_t alt[11][11] = {
        {0x90},
        {0x66, 0x90},
        {0x0f, 0x1f, 0x00},
        {0x0f, 0x1f, 0x40, 0x00},
        {0x0f, 0x1f, 0x44, 0x00, 0x00},
        {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},
        {0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00},
        {0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}};
    if (count == 0) return;
    if (!last_insn_normal) {
      out.push_back(0x90);
      --count;
    }
    if (count / 11 > 7) unsupported("long NOP padding");
    std::uint64_t last = count % 11;
    for (std::uint64_t k = 0; k < count / 11; ++k) out.insert(out.end(), alt[10], alt[10] + 11);
    if (last) out.insert(out.end(), alt[last - 1], alt[last - 1] + last);
  }

  void contents(Section& s) {
    s.bytes.clear();
    if (s.type == SHT_NOBITS) {
      for (const Frag& f : s.frags)
        if (!f.data.empty()) unsupported("data in a nobits section");
      return;
    }
    s.bytes.reserve(s.size);
    for (const Frag& f : s.frags) {
      s.bytes.insert(s.bytes.end(), f.data.begin(), f.data.end());
      if (f.var == Var::Align) {
        std::uint64_t n = relax_align(f.addr + f.data.size(), f.align_pow);
        if (f.code_fill) nops(s.bytes, n, f.last_insn_normal);
        else s.bytes.insert(s.bytes.end(), n, 0);
      } else if (f.var == Var::Branch) {
        std::int64_t target = static_cast<std::int64_t>(sym_value(f.target));
        if (!f.long_form) {
          std::int64_t d = target - static_cast<std::int64_t>(f.addr + f.data.size() + 1);
          s.bytes.push_back(static_cast<std::uint8_t>(d));
        } else {
          std::uint8_t opc = s.bytes.back();
          std::int64_t d = target - static_cast<std::int64_t>(f.addr + f.data.size() + f.var_size());
          if (f.cond) {
            s.bytes.back() = 0x0f;
            s.bytes.push_back(static_cast<std::uint8_t>(opc + 0x10));
          } else
            s.bytes.back() = 0xe9;
          for (int b = 0; b < 4; ++b) s.bytes.push_back(static_cast<std::uint8_t>(d >> (8 * b)));
        }
      }
    }
  }

  // ---- fixups (fixup_segment, adjust_reloc_syms) ----
  bool is_local_label(const Sym& y) const {
    return y.kind == SymKind::Normal && y.name.size() >= 2 && y.name[0] == '.' && y.name[1] == 'L';
  }
  bool external(const Sym& y) const { return y.kind == SymKind::Normal && (y.global || y.sec < 0); }

  void fixups(int sec) {
    Section& s = sections[sec];
    for (const Fix& x : s.fixes) {
      std::uint64_t P = s.frags[x.frag].addr + x.where;
      int add = x.add, sub = x.sub;
      std::int64_t off = x.off;
      bool pcrel = x.pcrel;
      if (sub >= 0) {
        const Sym& b = syms[sub];
        if (b.sec < 0) unsupported("subtraction of an undefined symbol");
        if (add >= 0 && syms[add].sec == b.sec) {
          off += static_cast<std::int64_t>(sym_value(add)) - static_cast<std::int64_t>(sym_value(sub));
          add = -1;
        } else if (b.sec == sec && !pcrel) {
          off += static_cast<std::int64_t>(P) - static_cast<std::int64_t>(sym_value(sub));
          pcrel = true;
        } else
          unsupported("symbol difference across sections");
        sub = -1;
      }
      auto store = [&](std::int64_t v) {
        for (int b = 0; b < x.size; ++b) s.bytes[P + b] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(v) >> (8 * b));
      };
      if (add < 0) {
        if (pcrel) unsupported("pc-relative constant");
        store(off);
        continue;
      }
      const Sym& a = syms[add];
      // resolved here: a pc-relative reference to a local symbol of this section
      if (pcrel && x.rtype == RType::Generic && a.sec == sec && !external(a)) {
        store(static_cast<std::int64_t>(sym_value(add)) + off - static_cast<std::int64_t>(P));
        continue;
      }
      std::uint32_t type;
      switch (x.rtype) {
        case RType::PLT32: type = R_X86_64_PLT32; break;
        case RType::REX_GOTPCRELX: type = R_X86_64_REX_GOTPCRELX; break;
        case RType::GOTPCRELX: type = R_X86_64_GOTPCRELX; break;
        default:
          if (pcrel) {
            if (x.size == 4) type = R_X86_64_PC32;
            else if (x.size == 8) type = R_X86_64_PC64;
            else unsupported("pc-relative fixup size");
          } else {
            if (x.size == 8) type = R_X86_64_64;
            else if (x.size == 4) type = R_X86_64_32;
            else if (x.size == 2) type = R_X86_64_16;
            else type = R_X86_64_8;
          }
      }
      // adjust_reloc_syms: a local symbol becomes its section's symbol
      // (not a global or undefined symbol (S_FORCE_RELOC), not a section
      // symbol already, not a GOT relocation (tc_i386_fix_adjustable))
      int rsym = add;
      bool adjustable = (a.kind == SymKind::Temp || (a.kind == SymKind::Normal && !external(a))) &&
                        (x.rtype == RType::Generic || x.rtype == RType::PLT32);
      if (adjustable) {
        const Section& as = sections[a.sec];
        if ((as.flags & SHF_MERGE) && (pcrel || off != 0)) adjustable = false;
      }
      if (adjustable) {
        off += static_cast<std::int64_t>(sym_value(add));
        rsym = section_symbol(a.sec);
      }
      s.relas.push_back(Rela{P, type, rsym, off});
    }
  }

  // ---- .eh_frame (cfi_finish) ----
  struct CieEntry {
    std::vector<CfiInsn> insns;  // its initial instructions
    std::uint64_t start;
  };
  static bool initial_cie_insn(const CfiInsn& i, const CfiInsn* j) {
    if (j && i.insn != j->insn) return false;
    switch (i.insn) {
      case DW_CFA_offset:
      case DW_CFA_def_cfa:
        if (j && (i.reg != j->reg || i.offset != j->offset)) return false;
        return true;
      case DW_CFA_def_cfa_register:
        if (j && i.reg != j->reg) return false;
        return true;
      case DW_CFA_def_cfa_offset:
        if (j && i.offset != j->offset) return false;
        return true;
      default: return false;
    }
  }

  static void uleb(std::vector<std::uint8_t>& o, std::uint64_t v) {
    do {
      std::uint8_t b = v & 0x7f;
      v >>= 7;
      if (v) b |= 0x80;
      o.push_back(b);
    } while (v);
  }
  static void sleb(std::vector<std::uint8_t>& o, std::int64_t v) {
    for (;;) {
      std::uint8_t b = v & 0x7f;
      v >>= 7;
      bool done = (v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40));
      if (!done) b |= 0x80;
      o.push_back(b);
      if (done) return;
    }
  }
  static void le32(std::vector<std::uint8_t>& o, std::uint64_t v) {
    for (int b = 0; b < 4; ++b) o.push_back(static_cast<std::uint8_t>(v >> (8 * b)));
  }

  void output_cfi_insn(std::vector<std::uint8_t>& o, const CfiInsn& i) {
    switch (i.insn) {
      case DW_CFA_advance_loc: {
        std::uint64_t delta = sym_value(i.lab2) - sym_value(i.lab1);
        if (delta == 0) {
        } else if (delta <= 0x3f) o.push_back(static_cast<std::uint8_t>(DW_CFA_advance_loc + delta));
        else if (delta <= 0xff) {
          o.push_back(DW_CFA_advance_loc1);
          o.push_back(static_cast<std::uint8_t>(delta));
        } else if (delta <= 0xffff) {
          o.push_back(DW_CFA_advance_loc2);
          o.push_back(static_cast<std::uint8_t>(delta));
          o.push_back(static_cast<std::uint8_t>(delta >> 8));
        } else {
          o.push_back(DW_CFA_advance_loc4);
          le32(o, delta);
        }
        break;
      }
      case DW_CFA_def_cfa:
        if (i.offset < 0) unsupported("negative CFA offset");
        o.push_back(DW_CFA_def_cfa);
        uleb(o, static_cast<std::uint64_t>(i.reg));
        uleb(o, static_cast<std::uint64_t>(i.offset));
        break;
      case DW_CFA_def_cfa_register:
        o.push_back(DW_CFA_def_cfa_register);
        uleb(o, static_cast<std::uint64_t>(i.reg));
        break;
      case DW_CFA_def_cfa_offset:
        if (i.offset < 0) {
          o.push_back(DW_CFA_def_cfa_offset_sf);
          sleb(o, i.offset / -8);
        } else {
          o.push_back(DW_CFA_def_cfa_offset);
          uleb(o, static_cast<std::uint64_t>(i.offset));
        }
        break;
      case DW_CFA_offset:
        if (i.reg > 0x3f) unsupported("CFA offset register");
        o.push_back(static_cast<std::uint8_t>(DW_CFA_offset + i.reg));
        uleb(o, static_cast<std::uint64_t>(i.offset / -8));
        break;
      case DW_CFA_remember_state:
      case DW_CFA_restore_state: o.push_back(static_cast<std::uint8_t>(i.insn)); break;
      default: unsupported("CFI instruction");
    }
  }

  void eh_frame() {
    if (fdes.empty()) return;
    Gen g{*this, eh_sec};
    std::vector<std::uint8_t>& o = g.d;
    std::vector<CieEntry> cies;  // searched newest first (cie_root)
    auto align = [&](int pow) {
      while (o.size() % (std::size_t{1} << pow)) o.push_back(0);  // DW_CFA_nop
    };
    for (std::size_t n = 0; n < fdes.size(); ++n) {
      const Fde& f = fdes[n];
      if (f.end < 0) unsupported("open CFI");
      if (syms[f.start].sec != syms[f.end].sec) unsupported("CFI across sections");
      // select_cie_for_fde
      std::size_t first = 0;
      const CieEntry* cie = nullptr;
      for (auto it = cies.rbegin(); it != cies.rend() && !cie; ++it) {
        std::size_t k = 0;
        for (; k < it->insns.size() && k < f.data.size(); ++k)
          if (!initial_cie_insn(it->insns[k], &f.data[k])) break;
        if (k == it->insns.size()) {
          cie = &*it;
          first = k;
        }
      }
      if (!cie) {
        CieEntry c;
        std::size_t k = 0;
        while (k < f.data.size() && initial_cie_insn(f.data[k], nullptr)) ++k;
        c.insns.assign(f.data.begin(), f.data.begin() + static_cast<std::ptrdiff_t>(k));
        first = k;
        // output_cie
        c.start = o.size();
        std::size_t len_at = o.size();
        le32(o, 0);
        le32(o, 0);  // CIE id
        o.push_back(1);  // version
        o.push_back('z');
        o.push_back('R');
        o.push_back(0);
        uleb(o, 1);   // code alignment
        sleb(o, -8);  // data alignment
        o.push_back(16);  // return column
        uleb(o, 1);       // augmentation size
        o.push_back(0x1b);  // DW_EH_PE_pcrel | DW_EH_PE_sdata4
        for (const CfiInsn& i : c.insns) output_cfi_insn(o, i);
        align(2);
        g.put(len_at, o.size() - len_at - 4, 4);
        cies.push_back(std::move(c));
        cie = &cies.back();
      }
      // output_fde
      std::size_t len_at = o.size();
      le32(o, 0);
      le32(o, o.size() - cie->start);  // CIE pointer
      g.fix(4, f.start, 0, true);
      le32(o, sym_value(f.end) - sym_value(f.start));
      uleb(o, 0);  // augmentation size
      for (std::size_t k = first; k < f.data.size(); ++k) output_cfi_insn(o, f.data[k]);
      align(n + 1 == fdes.size() ? 3 : 2);
      g.put(len_at, o.size() - len_at - 4, 4);
    }
    g.finish();
  }

  // ---- sections made at the end: .debug_* (dwarf2_finish), .eh_frame (cfi_finish) ----
  struct Gen {
    Assembler& a;
    int sec;
    std::vector<std::uint8_t> d;
    std::vector<Fix> fixes;
    void b(std::uint64_t v) { d.push_back(static_cast<std::uint8_t>(v)); }
    void le(std::uint64_t v, int n) {
      for (int k = 0; k < n; ++k) b(v >> (8 * k));
    }
    void put(std::size_t at, std::uint64_t v, int n) {
      for (int k = 0; k < n; ++k) d[at + k] = static_cast<std::uint8_t>(v >> (8 * k));
    }
    void ul(std::uint64_t v) { uleb(d, v); }
    void sl(std::int64_t v) { sleb(d, v); }
    void str(const std::string& s) {
      d.insert(d.end(), s.begin(), s.end());
      d.push_back(0);
    }
    // emit_expr of [add] + [off]
    void fix(int size, int add, std::int64_t off, bool pcrel = false) {
      Fix f;
      f.frag = 0;
      f.where = static_cast<std::uint32_t>(d.size());
      f.size = static_cast<std::uint8_t>(size);
      f.pcrel = pcrel;
      f.add = add;
      f.off = off;
      fixes.push_back(f);
      le(0, size);
    }
    int here() { return a.temp_at(sec, 0, d.size()); }
    void finish() {
      Section& s = a.sections[sec];
      s.frags.assign(1, Frag{});
      s.frags[0].data = d;
      s.bytes = std::move(d);
      s.size = s.bytes.size();
      s.fixes = std::move(fixes);
    }
  };

  int temp_at(int sec, int frag, std::uint64_t off) {
    Sym y;
    y.kind = SymKind::Temp;
    y.sec = sec;
    y.frag = frag;
    y.off = off;
    syms.push_back(std::move(y));
    return static_cast<int>(syms.size()) - 1;
  }
  // a section's start and end (first_frag_for_seg / last_frag_for_seg)
  int section_start(int sec) { return temp_at(sec, 0, 0); }
  int section_end(int sec) {
    const Section& s = sections[sec];
    return temp_at(sec, static_cast<int>(s.frags.size()) - 1, s.frags.back().data.size());
  }

  int eh_sec = -1;
  int debug_line = -1, debug_info = -1, debug_abbrev = -1, debug_aranges = -1, debug_str = -1, debug_ranges = -1;
  std::vector<std::pair<std::string, std::string>> prefix_map;  // --debug-prefix-map, in order

  // remap_debug_filename: the last map given is tried first
  std::string remap(const std::string& f) const {
    for (auto it = prefix_map.rbegin(); it != prefix_map.rend(); ++it)
      if (f.compare(0, it->first.size(), it->first) == 0) return it->second + f.substr(it->first.size());
    return f;
  }

  // libiberty's getpwd: $PWD when it names the current directory
  static std::string getpwd() {
    const char* p = std::getenv("PWD");
    struct stat ps, ds;
    if (p && *p == '/' && ::stat(p, &ps) == 0 && ::stat(".", &ds) == 0 && ps.st_ino == ds.st_ino &&
        ps.st_dev == ds.st_dev)
      return p;
    std::vector<char> buf(4096);
    while (!::getcwd(buf.data(), buf.size())) {
      if (errno != ERANGE) unsupported("getcwd");
      buf.resize(buf.size() * 2);
    }
    return buf.data();
  }

  // dwarf2_finish's sections and section symbols, then cfi_finish's
  void make_final_sections() {
    if (!line_segs.empty()) {
      debug_line = new_section(".debug_line", SHT_PROGBITS, 0, 0, false);
      debug_info = new_section(".debug_info", SHT_PROGBITS, 0, 0, false);
      debug_abbrev = new_section(".debug_abbrev", SHT_PROGBITS, 0, 0, false);
      debug_aranges = new_section(".debug_aranges", SHT_PROGBITS, 0, 0, false);
      debug_str = new_section(".debug_str", SHT_PROGBITS, SHF_MERGE | SHF_STRINGS, 1, false);
      sections[debug_aranges].align_pow = 4;
      if (line_segs.size() > 1) {
        debug_ranges = new_section(".debug_ranges", SHT_PROGBITS, 0, 0, false);
        sections[debug_ranges].align_pow = 4;
      }
      // out_debug_aranges', then out_debug_info's section_symbol calls
      section_symbol(debug_info);
      section_symbol(debug_abbrev);
      section_symbol(debug_line);
    }
    if (!fdes.empty()) {
      eh_sec = new_section(".eh_frame", SHT_PROGBITS, SHF_ALLOC, 0, false);
      sections[eh_sec].align_pow = 3;
    }
  }

  // emit_inc_line_addr
  static void inc_line_addr(Gen& g, std::int64_t line_delta, std::uint64_t addr_delta, bool end_sequence) {
    const std::uint64_t max_special_addr_delta = (255 - 13) / 14;
    if (end_sequence) {
      if (addr_delta == max_special_addr_delta) g.b(8);  // DW_LNS_const_add_pc
      else if (addr_delta) {
        g.b(2);  // DW_LNS_advance_pc
        g.ul(addr_delta);
      }
      g.b(0);
      g.b(1);
      g.b(1);  // DW_LNE_end_sequence
      return;
    }
    std::uint64_t tmp = static_cast<std::uint64_t>(static_cast<std::uint32_t>(line_delta + 5));
    bool need_copy = false;
    if (tmp >= 14) {
      g.b(3);  // DW_LNS_advance_line
      g.sl(line_delta);
      line_delta = 0;
      tmp = 5;
      need_copy = true;
    }
    if (line_delta == 0 && addr_delta == 0) {
      g.b(1);  // DW_LNS_copy
      return;
    }
    tmp += 13;
    if (addr_delta < 256 + max_special_addr_delta) {
      std::uint64_t opcode = tmp + addr_delta * 14;
      if (opcode <= 255) {
        g.b(opcode);
        return;
      }
      opcode = tmp + (addr_delta - max_special_addr_delta) * 14;
      if (opcode <= 255) {
        g.b(8);
        g.b(opcode);
        return;
      }
    }
    g.b(2);
    g.ul(addr_delta);
    g.b(need_copy ? 1 : tmp);
  }

  void process_entries(Gen& g, const LineSeg& ls) {
    unsigned filenum = 1, line = 1, column = 0;
    std::uint64_t last = 0;
    bool first = true;
    for (const LineEntry& e : ls.entries) {
      if (filenum != e.filenum) {
        filenum = e.filenum;
        g.b(4);  // DW_LNS_set_file
        g.ul(filenum);
      }
      if (column != e.column) {
        column = e.column;
        g.b(5);  // DW_LNS_set_column
        g.ul(column);
      }
      std::int64_t line_delta = static_cast<int>(e.line - line);
      std::uint64_t addr = sym_value(e.label);
      if (first) {
        // out_set_addr
        g.b(0);
        g.ul(9);
        g.b(2);  // DW_LNE_set_address
        g.fix(8, e.label, 0);
        inc_line_addr(g, line_delta, 0, false);
        first = false;
      } else
        inc_line_addr(g, line_delta, addr - last, false);
      line = e.line;
      last = addr;
    }
    inc_line_addr(g, 0, sections[ls.sec].size - last, true);
  }

  // a function symbol with a size (out_debug_abbrev / out_debug_info)
  bool dwarf_function(const Sym& y) const {
    if (y.kind != SymKind::Normal || y.sec < 0 || y.type != STT_FUNC || !y.has_size) return false;
    return !(y.size_parse_const && symbol_size(y) == 0);
  }
  std::uint64_t symbol_size(const Sym& y) const {
    std::int64_t v = y.size_const;
    if (y.size_end >= 0) {
      if (y.size_start < 0 || syms[y.size_end].sec != syms[y.size_start].sec) unsupported(".size expression");
      v += static_cast<std::int64_t>(sym_value(y.size_end)) - static_cast<std::int64_t>(sym_value(y.size_start));
    }
    return static_cast<std::uint64_t>(v);
  }

  void dwarf2_contents() {
    if (line_segs.empty()) return;
    for (std::size_t i = 1; i < files.size(); ++i)
      if (!files[i].filename) unsupported("unassigned file number");
    if (files.size() < 2) unsupported("no file 1");
    // out_debug_line
    {
      Gen g{*this, debug_line};
      std::size_t len_at = g.d.size();
      g.le(0, 4);
      g.le(3, 2);  // version
      std::size_t hl_at = g.d.size();
      g.le(0, 4);
      std::size_t prologue_start = g.d.size();
      g.b(1);     // minimum instruction length
      g.b(1);     // default is_stmt
      g.b(0xfb);  // line base -5
      g.b(14);    // line range
      g.b(13);    // opcode base
      for (int x : {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1}) g.b(static_cast<std::uint64_t>(x));
      for (std::size_t i = 1; i < dirs.size(); ++i) {
        if (!dirs[i]) unsupported("directory table hole");
        g.str(remap(*dirs[i]));
      }
      g.b(0);
      for (std::size_t i = 1; i < files.size(); ++i) {
        g.str(*files[i].filename);
        g.ul(files[i].dir);
        g.ul(0);
        g.ul(0);
      }
      g.b(0);
      g.put(hl_at, g.d.size() - prologue_start, 4);
      for (const LineSeg& ls : line_segs) process_entries(g, ls);
      g.put(len_at, g.d.size() - len_at - 4, 4);
      g.finish();
    }
    // out_debug_ranges
    int ranges_sym = -1;
    if (debug_ranges >= 0) {
      Gen g{*this, debug_ranges};
      ranges_sym = g.here();
      g.le(~std::uint64_t{0}, 8);
      g.le(0, 8);
      for (const LineSeg& ls : line_segs) {
        g.fix(8, section_start(ls.sec), 0);
        g.fix(8, section_end(ls.sec), 0);
      }
      g.le(0, 16);
      g.finish();
    }
    // out_debug_aranges
    {
      Gen g{*this, debug_aranges};
      g.le(0, 4);
      g.le(2, 2);  // version
      g.fix(4, sections[debug_info].sym, 0);
      g.b(8);  // address size
      g.b(0);  // segment size
      g.le(0, 4);  // the header aligned to twice the address size
      for (const LineSeg& ls : line_segs) {
        g.fix(8, section_start(ls.sec), 0);
        g.le(sections[ls.sec].size, 8);
      }
      g.le(0, 16);
      g.put(0, g.d.size() - 4, 4);
      g.finish();
    }
    // out_debug_abbrev
    bool have_efunc = false, have_lfunc = false;
    for (int s : symbol_list) {
      const Sym& y = syms[s];
      if (!dwarf_function(y)) continue;
      if (external(y)) have_efunc = true;
      else have_lfunc = true;
    }
    bool funcs = have_efunc || have_lfunc;
    {
      Gen g{*this, debug_abbrev};
      g.ul(1);
      g.ul(0x11);  // DW_TAG_compile_unit
      g.b(funcs ? 1 : 0);
      g.ul(0x10), g.ul(0x06);  // DW_AT_stmt_list, DW_FORM_data4
      if (line_segs.size() == 1) {
        g.ul(0x11), g.ul(0x01);  // DW_AT_low_pc, DW_FORM_addr
        g.ul(0x12), g.ul(0x01);  // DW_AT_high_pc, DW_FORM_addr
      } else
        g.ul(0x55), g.ul(0x06);  // DW_AT_ranges, DW_FORM_data4
      g.ul(0x03), g.ul(0x0e);    // DW_AT_name, DW_FORM_strp
      g.ul(0x1b), g.ul(0x0e);    // DW_AT_comp_dir
      g.ul(0x25), g.ul(0x0e);    // DW_AT_producer
      g.ul(0x13), g.ul(0x05);    // DW_AT_language, DW_FORM_data2
      g.ul(0), g.ul(0);
      if (funcs) {
        g.ul(2);
        g.ul(0x2e);  // DW_TAG_subprogram
        g.b(0);
        g.ul(0x03), g.ul(0x0e);
        if (have_efunc) g.ul(0x3f), g.ul(0x0c);  // DW_AT_external, DW_FORM_flag
        g.ul(0x49), g.ul(0x15);  // DW_AT_type, DW_FORM_ref_udata
        g.ul(0x11), g.ul(0x01);
        g.ul(0x12), g.ul(0x01);
        g.ul(0), g.ul(0);
        g.ul(3);
        g.ul(0x3b);  // DW_TAG_unspecified_type
        g.b(0);
        g.ul(0), g.ul(0);
      }
      g.b(0);
      g.finish();
    }
    // out_debug_str and out_debug_info
    Gen str{*this, debug_str};
    int name_sym = str.here();
    {
      std::string name;
      if (files[1].dir) {
        if (files[1].dir >= dirs.size() || !dirs[files[1].dir]) unsupported("directory table hole");
        name = remap(*dirs[files[1].dir]) + "/";
      }
      str.str(name + *files[1].filename);
    }
    int comp_dir_sym = str.here();
    str.str(remap(getpwd()));
    int producer_sym = str.here();
    str.str("GNU AS 2.42");
    std::vector<int> functions;
    if (funcs)
      for (int s : symbol_list)
        if (dwarf_function(syms[s])) functions.push_back(s);
    std::vector<int> name_syms;
    for (int s : functions) {
      name_syms.push_back(str.here());
      str.str(syms[s].name);
    }
    // the DW_AT_type references are uleb128s of the unspecified type's
    // offset: gas's rs_leb128 frags, grown from one byte until they hold it
    for (int width = 1;; ++width) {
      Gen g{*this, debug_info};
      g.le(0, 4);
      g.le(3, 2);  // version
      g.fix(4, sections[debug_abbrev].sym, 0);
      g.b(8);
      g.ul(1);
      g.fix(4, sections[debug_line].sym, 0);
      if (line_segs.size() == 1) {
        g.fix(8, section_start(line_segs[0].sec), 0);
        g.fix(8, section_end(line_segs[0].sec), 0);
      } else
        g.fix(4, ranges_sym, 0);
      g.fix(4, name_sym, 0);
      g.fix(4, comp_dir_sym, 0);
      g.fix(4, producer_sym, 0);
      g.le(0x8001, 2);  // DW_LANG_Mips_Assembler
      std::uint64_t no_type_tag = 0;
      if (funcs) {
        std::vector<std::size_t> type_at;
        for (std::size_t k = 0; k < functions.size(); ++k) {
          const Sym& y = syms[functions[k]];
          g.ul(2);
          g.fix(4, name_syms[k], 0);
          if (have_efunc) g.b(external(y) ? 1 : 0);
          type_at.push_back(g.d.size());
          g.d.insert(g.d.end(), static_cast<std::size_t>(width), 0);
          g.fix(8, functions[k], 0);
          g.fix(8, functions[k], static_cast<std::int64_t>(symbol_size(y)));
        }
        no_type_tag = g.d.size();
        g.ul(3);
        g.b(0);
        std::vector<std::uint8_t> leb;
        uleb(leb, no_type_tag);
        if (leb.size() > static_cast<std::size_t>(width)) continue;
        for (std::size_t at : type_at) {
          for (int k = 0; k < width; ++k)
            g.d[at + k] = static_cast<std::uint8_t>(((no_type_tag >> (7 * k)) & 0x7f) | (k + 1 < width ? 0x80 : 0));
        }
      }
      g.put(0, g.d.size() - 4, 4);
      g.finish();
      break;
    }
    str.finish();
  }

  // ---- fixups, second pass (fixup_segment): symbol_mark_used_in_reloc;
  // a kept .L label joins the chain now (local_symbol_convert)
  void mark_used_in_relocs() {
    for (Section& s : sections)
      for (const Rela& r : s.relas) {
        Sym& y = syms[r.sym];
        if (!y.used_in_reloc && is_local_label(y)) symbol_list.push_back(r.sym);
        y.used_in_reloc = true;
      }
  }

  // ---- the ELF object ----
  // elf-strtab.c: strings in insertion order, suffixes merged into longer ones
  struct StrTab {
    std::vector<std::string> strs;
    std::unordered_map<std::string, std::size_t> idx;
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint8_t> data;
    std::size_t add(const std::string& s) {
      auto it = idx.find(s);
      if (it != idx.end()) return it->second;
      strs.push_back(s);
      idx.emplace(s, strs.size() - 1);
      return strs.size() - 1;
    }
    void finalize() {
      std::size_t n = strs.size();
      std::vector<std::size_t> order(n);
      for (std::size_t k = 0; k < n; ++k) order[k] = k;
      std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const std::string &A = strs[a], &B = strs[b];
        std::size_t la = A.size(), lb = B.size(), l = std::min(la, lb);
        for (std::size_t k = 1; k <= l; ++k) {
          unsigned char x = static_cast<unsigned char>(A[la - k]), y = static_cast<unsigned char>(B[lb - k]);
          if (x != y) return x < y;
        }
        return la < lb;
      });
      std::vector<long> suffix_of(n, -1);
      if (n) {
        std::size_t e = order[n - 1];
        for (std::size_t k = n - 1; k-- > 0;) {
          std::size_t c = order[k];
          const std::string &E = strs[e], &C = strs[c];
          if (E.size() > C.size() && E.compare(E.size() - C.size(), C.size(), C) == 0)
            suffix_of[c] = static_cast<long>(e);
          else
            e = c;
        }
      }
      offsets.assign(n, 0);
      data.assign(1, 0);
      for (std::size_t k = 0; k < n; ++k)
        if (suffix_of[k] < 0) {
          offsets[k] = static_cast<std::uint32_t>(data.size());
          data.insert(data.end(), strs[k].begin(), strs[k].end());
          data.push_back(0);
        }
      for (std::size_t k = 0; k < n; ++k)
        if (suffix_of[k] >= 0) {
          const std::string& E = strs[static_cast<std::size_t>(suffix_of[k])];
          offsets[k] = static_cast<std::uint32_t>(offsets[static_cast<std::size_t>(suffix_of[k])] + E.size() -
                                                  strs[k].size());
        }
    }
    std::uint32_t offset(const std::string& s) const {
      if (s.empty()) return 0;
      return offsets[idx.at(s)];
    }
  };

  struct Shdr {
    std::uint32_t name = 0, type = 0;
    std::uint64_t flags = 0, offset = 0, size = 0;
    std::uint32_t link = 0, info = 0;
    std::uint64_t addralign = 0, entsize = 0;
    const std::vector<std::uint8_t>* bytes = nullptr;
    std::string sname;
  };

  std::vector<std::uint8_t> write() {
    // the symbol table: the chain's kept symbols, locals first
    std::vector<int> locals, globals;
    for (int s : symbol_list) {
      const Sym& y = syms[s];
      switch (y.kind) {
        case SymKind::Section:
          if (y.used_in_reloc) locals.push_back(s);
          break;
        case SymKind::File: locals.push_back(s); break;
        case SymKind::Temp: break;
        case SymKind::Normal:
          if (external(y)) globals.push_back(s);
          else if (!is_local_label(y) || y.used_in_reloc) locals.push_back(s);
          break;
      }
    }
    std::vector<int> order = locals;
    order.insert(order.end(), globals.begin(), globals.end());
    for (std::size_t k = 0; k < order.size(); ++k) syms[order[k]].out_index = static_cast<std::uint32_t>(k + 1);
    StrTab strtab;
    for (int s : order) {
      const Sym& y = syms[s];
      if (y.kind == SymKind::Normal && !y.name.empty()) strtab.add(y.name);
    }
    strtab.finalize();

    // the section headers: each section, its relocations right after it
    std::vector<Shdr> sh(1);
    std::vector<int> shndx(sections.size());
    std::vector<std::vector<std::uint8_t>> rela_bytes(sections.size());
    StrTab shstrtab;
    shstrtab.add(".symtab");
    shstrtab.add(".strtab");
    shstrtab.add(".shstrtab");
    std::vector<std::size_t> rela_hdr;
    for (std::size_t k = 0; k < sections.size(); ++k) {
      Section& s = sections[k];
      Shdr h;
      h.sname = s.name;
      h.type = s.type;
      h.flags = s.flags;
      h.size = s.size;
      h.addralign = std::uint64_t{1} << s.align_pow;
      h.entsize = s.entsize;
      h.bytes = &s.bytes;
      shstrtab.add(s.name);
      shndx[k] = static_cast<int>(sh.size());
      sh.push_back(h);
      if (!s.relas.empty()) {
        std::vector<std::uint8_t>& rb = rela_bytes[k];
        for (const Rela& r : s.relas) {
          for (int b = 0; b < 8; ++b) rb.push_back(static_cast<std::uint8_t>(r.offset >> (8 * b)));
          std::uint64_t info = (std::uint64_t{syms[r.sym].out_index} << 32) | r.type;
          for (int b = 0; b < 8; ++b) rb.push_back(static_cast<std::uint8_t>(info >> (8 * b)));
          for (int b = 0; b < 8; ++b)
            rb.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(r.addend) >> (8 * b)));
        }
        Shdr r;
        r.sname = ".rela" + s.name;
        r.type = SHT_RELA;
        r.flags = SHF_INFO_LINK;
        r.size = rb.size();
        r.info = static_cast<std::uint32_t>(shndx[k]);
        r.addralign = 8;
        r.entsize = 24;
        r.bytes = &rb;
        shstrtab.add(r.sname);
        rela_hdr.push_back(sh.size());
        sh.push_back(r);
      }
    }
    std::size_t symtab_i = sh.size(), strtab_i = symtab_i + 1, shstrtab_i = symtab_i + 2;
    // .symtab
    std::vector<std::uint8_t> symtab(24, 0);
    for (int s : order) {
      const Sym& y = syms[s];
      std::uint32_t name = 0;
      std::uint8_t info = 0;
      std::uint16_t ndx = 0;
      std::uint64_t value = 0, size = 0;
      bool global = external(y);
      switch (y.kind) {
        case SymKind::File:
          info = STT_FILE;
          ndx = 0xfff1;
          break;
        case SymKind::Section:
          info = STT_SECTION;
          ndx = static_cast<std::uint16_t>(shndx[y.sec]);
          break;
        default:
          name = strtab.offset(y.name);
          info = static_cast<std::uint8_t>((global ? 0x10 : 0) | y.type);
          if (y.sec >= 0) {
            ndx = static_cast<std::uint16_t>(shndx[y.sec]);
            value = sym_value(s);
          }
          if (y.has_size) {
            std::int64_t v = y.size_const;
            if (y.size_end >= 0) {
              if (y.size_start < 0 || syms[y.size_end].sec != syms[y.size_start].sec)
                unsupported(".size expression");
              v += static_cast<std::int64_t>(sym_value(y.size_end)) - static_cast<std::int64_t>(sym_value(y.size_start));
            }
            size = static_cast<std::uint64_t>(v);
          }
      }
      for (int b = 0; b < 4; ++b) symtab.push_back(static_cast<std::uint8_t>(name >> (8 * b)));
      symtab.push_back(info);
      symtab.push_back(0);
      symtab.push_back(static_cast<std::uint8_t>(ndx));
      symtab.push_back(static_cast<std::uint8_t>(ndx >> 8));
      for (int b = 0; b < 8; ++b) symtab.push_back(static_cast<std::uint8_t>(value >> (8 * b)));
      for (int b = 0; b < 8; ++b) symtab.push_back(static_cast<std::uint8_t>(size >> (8 * b)));
    }
    Shdr hs;
    hs.sname = ".symtab";
    hs.type = SHT_SYMTAB;
    hs.size = symtab.size();
    hs.link = static_cast<std::uint32_t>(strtab_i);
    hs.info = static_cast<std::uint32_t>(locals.size() + 1);
    hs.addralign = 8;
    hs.entsize = 24;
    hs.bytes = &symtab;
    sh.push_back(hs);
    Shdr ht;
    ht.sname = ".strtab";
    ht.type = SHT_STRTAB;
    ht.size = strtab.data.size();
    ht.addralign = 1;
    ht.bytes = &strtab.data;
    sh.push_back(ht);
    shstrtab.finalize();
    Shdr hh;
    hh.sname = ".shstrtab";
    hh.type = SHT_STRTAB;
    hh.size = shstrtab.data.size();
    hh.addralign = 1;
    hh.bytes = &shstrtab.data;
    sh.push_back(hh);
    for (std::size_t r : rela_hdr) sh[r].link = static_cast<std::uint32_t>(symtab_i);
    for (Shdr& h : sh)
      if (!h.sname.empty()) h.name = shstrtab.offset(h.sname);

    // file positions: the contents, .symtab, .strtab, the relocations,
    // .shstrtab, the section headers
    std::uint64_t off = 64;
    auto place = [&](Shdr& h) {
      std::uint64_t a = h.addralign ? h.addralign : 1;
      off = (off + a - 1) / a * a;
      h.offset = off;
      if (h.type != SHT_NOBITS) off += h.size;
    };
    for (std::size_t k = 1; k < sh.size(); ++k)
      if (sh[k].type != SHT_RELA && k != symtab_i && k != strtab_i && k != shstrtab_i) place(sh[k]);
    place(sh[symtab_i]);
    place(sh[strtab_i]);
    for (std::size_t r : rela_hdr) place(sh[r]);
    place(sh[shstrtab_i]);
    std::uint64_t shoff = (off + 7) / 8 * 8;

    std::vector<std::uint8_t> o(shoff + sh.size() * 64, 0);
    auto put = [&](std::size_t at, std::uint64_t v, int n) {
      for (int b = 0; b < n; ++b) o[at + b] = static_cast<std::uint8_t>(v >> (8 * b));
    };
    static const std::uint8_t ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
    std::memcpy(o.data(), ident, 16);
    put(16, 1, 2);     // ET_REL
    put(18, 62, 2);    // EM_X86_64
    put(20, 1, 4);     // EV_CURRENT
    put(40, shoff, 8);
    put(52, 64, 2);    // e_ehsize
    put(58, 64, 2);    // e_shentsize
    put(60, sh.size(), 2);
    put(62, shstrtab_i, 2);
    for (std::size_t k = 0; k < sh.size(); ++k) {
      const Shdr& h = sh[k];
      if (h.bytes && h.type != SHT_NOBITS && !h.bytes->empty())
        std::memcpy(o.data() + h.offset, h.bytes->data(), h.bytes->size());
      std::size_t at = shoff + k * 64;
      put(at, h.name, 4);
      put(at + 4, h.type, 4);
      put(at + 8, h.flags, 8);
      put(at + 24, h.offset, 8);
      put(at + 32, h.size, 8);
      put(at + 40, h.link, 4);
      put(at + 44, h.info, 4);
      put(at + 48, h.addralign, 8);
      put(at + 56, h.entsize, 8);
    }
    return o;
  }

  std::vector<std::uint8_t> run(std::string_view text) {
    parse(text);
    std::size_t assembled = sections.size();
    make_final_sections();
    for (std::size_t k = 0; k < assembled; ++k) {
      relax_section(static_cast<int>(k));
      contents(sections[k]);
    }
    // undefined labels
    for (const Sym& y : syms)
      if (y.kind == SymKind::Normal && y.sec < 0 && !y.global && is_local_label(y))
        unsupported("undefined local label " + y.name);
    dwarf2_contents();
    eh_frame();
    for (std::size_t k = 0; k < sections.size(); ++k) fixups(static_cast<int>(k));
    mark_used_in_relocs();
    return write();
  }
};

}  // namespace

std::vector<std::uint8_t> assemble(std::string_view text,
                                   const std::vector<std::pair<std::string, std::string>>& debug_prefix_map) {
  Assembler a;
  a.prefix_map = debug_prefix_map;
  return a.run(text);
}

}  // namespace cppcaml::typing::x86_assembler
