// Ports of asmcomp/amd64/emit.mlp, asmcomp/emitaux.ml and the x86 layer
// (x86_dsl.ml, x86_proc.ml, x86_gas.ml) for Linux: the X86_ast lines are
// printed as they are emitted (X86_gas prints each independently of the
// others) and collected until end_assembly.  See emit.hpp.
#include "cppcaml/typing/emit.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/hashtbl.hpp"

namespace cppcaml::typing::emit {

namespace {

[[noreturn]] void fatal(const std::string& s) { throw std::runtime_error(s); }

std::string out;  // the asm lines so far (X86_proc.asm_code, printed)

// ---- X86_ast / X86_gas ----------------------------------------------------------------------
enum class DataType { NONE, REAL4, REAL8, BYTE, WORD, DWORD, QWORD, OWORD, NEAR, PROC };
enum class R64 { RAX, RBX, RCX, RDX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

const char* string_of_reg64(R64 r) {
  static const char* n[] = {"rax", "rbx", "rcx", "rdx", "rsp", "rbp", "rsi", "rdi",
                            "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
  return n[static_cast<int>(r)];
}
const char* string_of_reg8l(R64 r) {
  static const char* n[] = {"al",  "bl",  "cl",   "dl",   "spl",  "bpl",  "sil",  "dil",
                            "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
  return n[static_cast<int>(r)];
}
const char* string_of_reg16(R64 r) {
  static const char* n[] = {"ax",  "bx",  "cx",   "dx",   "sp",   "bp",   "si",   "di",
                            "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
  return n[static_cast<int>(r)];
}
const char* string_of_reg32(R64 r) {
  static const char* n[] = {"eax", "ebx", "ecx",  "edx",  "esp",  "ebp",  "esi",  "edi",
                            "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
  return n[static_cast<int>(r)];
}

struct Arg {
  enum class K { Imm, Sym, Reg8L, Reg8H, Reg16, Reg32, Reg64, Regf, Mem, Mem64_RIP } k;
  std::int64_t imm = 0;
  std::string sym;  // Sym; Mem's / Mem64_RIP's symbol
  R64 reg = R64::RAX;  // registers; Mem's idx
  int reg8h = 0;       // AH BH CH DH
  int xmm = 0;
  DataType typ = DataType::NONE;
  long scale = 0;
  std::optional<R64> base;
  bool has_sym = false;
  long displ = 0;
  bool operator==(const Arg& o) const {
    return k == o.k && imm == o.imm && sym == o.sym && reg == o.reg && reg8h == o.reg8h && xmm == o.xmm &&
           typ == o.typ && scale == o.scale && base == o.base && has_sym == o.has_sym && displ == o.displ;
  }
  bool operator!=(const Arg& o) const { return !(*this == o); }
};

Arg imm(std::int64_t n) { return Arg{Arg::K::Imm, n}; }
Arg sym(std::string s) {
  Arg a{Arg::K::Sym};
  a.sym = std::move(s);
  return a;
}
Arg regk(Arg::K k, R64 r) {
  Arg a{k};
  a.reg = r;
  return a;
}
Arg reg64a(R64 r) { return regk(Arg::K::Reg64, r); }
Arg xmm(int n) {
  Arg a{Arg::K::Regf};
  a.xmm = n;
  return a;
}
Arg mem64(DataType typ, long displ, R64 idx, long scale = 1, std::optional<R64> base = std::nullopt) {
  Arg a{Arg::K::Mem};
  a.typ = typ;
  a.displ = displ;
  a.reg = idx;
  a.scale = scale;
  a.base = base;
  return a;
}
Arg mem64_rip(DataType typ, std::string s, long ofs = 0) {
  Arg a{Arg::K::Mem64_RIP};
  a.typ = typ;
  a.sym = std::move(s);
  a.displ = ofs;
  return a;
}

const Arg al = regk(Arg::K::Reg8L, R64::RAX);
const Arg cl = regk(Arg::K::Reg8L, R64::RCX);
const Arg ah = [] {
  Arg a{Arg::K::Reg8H};
  a.reg8h = 0;
  return a;
}();
const Arg rax = reg64a(R64::RAX), rbx = reg64a(R64::RBX), r10 = reg64a(R64::R10), r11 = reg64a(R64::R11),
          r12 = reg64a(R64::R12), r13 = reg64a(R64::R13), r15 = reg64a(R64::R15), rsp = reg64a(R64::RSP);
const Arg xmm15 = xmm(15);

std::string i64s(std::int64_t n) { return std::to_string(n); }

std::string opt_displ(long displ) {
  if (displ == 0) return "";
  if (displ > 0) return "+" + std::to_string(displ);
  return std::to_string(displ);
}

std::string arg_s(const Arg& a) {
  switch (a.k) {
    case Arg::K::Sym: return "$" + a.sym;
    case Arg::K::Imm: return "$" + i64s(a.imm);
    case Arg::K::Reg8L: return std::string("%") + string_of_reg8l(a.reg);
    case Arg::K::Reg8H: {
      static const char* n[] = {"ah", "bh", "ch", "dh"};
      return std::string("%") + n[a.reg8h];
    }
    case Arg::K::Reg16: return std::string("%") + string_of_reg16(a.reg);
    case Arg::K::Reg32: return std::string("%") + string_of_reg32(a.reg);
    case Arg::K::Reg64: return std::string("%") + string_of_reg64(a.reg);
    case Arg::K::Regf: return "%xmm" + std::to_string(a.xmm);
    case Arg::K::Mem: {
      std::string b;
      if (!a.has_sym) {
        if (a.displ != 0 || a.scale == 0) b += std::to_string(a.displ);
      } else {
        b += a.sym + opt_displ(a.displ);
      }
      if (a.scale != 0) {
        b += '(';
        if (a.base) b += std::string("%") + string_of_reg64(*a.base);
        if (a.base || a.scale != 1) b += ',';
        b += std::string("%") + string_of_reg64(a.reg);
        if (a.scale != 1) b += "," + std::to_string(a.scale);
        b += ')';
      }
      return b;
    }
    case Arg::K::Mem64_RIP: return a.sym + opt_displ(a.displ) + "(%rip)";
  }
  return "";
}

DataType typeof_(const Arg& a) {
  switch (a.k) {
    case Arg::K::Mem:
    case Arg::K::Mem64_RIP: return a.typ;
    case Arg::K::Reg8L:
    case Arg::K::Reg8H: return DataType::BYTE;
    case Arg::K::Reg16: return DataType::WORD;
    case Arg::K::Reg32: return DataType::DWORD;
    case Arg::K::Reg64: return DataType::QWORD;
    case Arg::K::Imm:
    case Arg::K::Sym: return DataType::NONE;
    case Arg::K::Regf: fatal("X86_gas.typeof");
  }
  return DataType::NONE;
}
const char* suf(const Arg& a) {
  switch (typeof_(a)) {
    case DataType::BYTE: return "b";
    case DataType::WORD: return "w";
    case DataType::DWORD:
    case DataType::REAL8: return "l";
    case DataType::QWORD: return "q";
    case DataType::REAL4: return "s";
    case DataType::NONE: return "";
    default: fatal("X86_gas.suf");
  }
}

void line(const std::string& s) {
  out += s;
  out += '\n';
}
void i0(const std::string& s) { line("\t" + s); }
void i1(const std::string& s, const Arg& x) { line("\t" + s + "\t" + arg_s(x)); }
void i1_s(const std::string& s, const Arg& x) { line("\t" + s + suf(x) + "\t" + arg_s(x)); }
void i2(const std::string& s, const Arg& x, const Arg& y) { line("\t" + s + "\t" + arg_s(x) + ", " + arg_s(y)); }
void i2_s(const std::string& s, const Arg& x, const Arg& y) {
  line("\t" + s + suf(y) + "\t" + arg_s(x) + ", " + arg_s(y));
}
void i2_ss(const std::string& s, const Arg& x, const Arg& y) {
  line("\t" + s + suf(x) + suf(y) + "\t" + arg_s(x) + ", " + arg_s(y));
}
void i1_call_jmp(const std::string& s, const Arg& x) {
  switch (x.k) {
    case Arg::K::Reg32:
    case Arg::K::Reg64:
    case Arg::K::Mem:
    case Arg::K::Mem64_RIP: line("\t" + s + "\t*" + arg_s(x)); return;
    case Arg::K::Sym: line("\t" + s + "\t" + x.sym); return;
    default: fatal("X86_gas.i1_call_jmp");
  }
}

enum class Cond { L, GE, LE, G, B, AE, BE, A, E, NE, O, NO, S, NS, P, NP };
const char* string_of_condition(Cond c) {
  switch (c) {
    case Cond::E: return "e";
    case Cond::AE: return "ae";
    case Cond::A: return "a";
    case Cond::GE: return "ge";
    case Cond::G: return "g";
    case Cond::NE: return "ne";
    case Cond::B: return "b";
    case Cond::BE: return "be";
    case Cond::L: return "l";
    case Cond::LE: return "le";
    case Cond::NP: return "np";
    case Cond::P: return "p";
    case Cond::NS: return "ns";
    case Cond::S: return "s";
    case Cond::NO: return "no";
    case Cond::O: return "o";
  }
  return "";
}

// X86_dsl.I
namespace I {
void add(const Arg& x, const Arg& y) { i2_s("add", x, y); }
void addsd(const Arg& x, const Arg& y) { i2("addsd", x, y); }
void and_(const Arg& x, const Arg& y) { i2_s("and", x, y); }
void andpd(const Arg& x, const Arg& y) { i2("andpd", x, y); }
void bswap(const Arg& x) { i1("bswap", x); }
void call(const Arg& x) { i1_call_jmp("call", x); }
void cmp(const Arg& x, const Arg& y) { i2_s("cmp", x, y); }
void cmpsd(const char* c, const Arg& x, const Arg& y) { i2(std::string("cmp") + c + "sd", x, y); }
void comisd(const Arg& x, const Arg& y) { i2("comisd", x, y); }
void cqo() { i0("cqto"); }
void cvtsd2ss(const Arg& x, const Arg& y) { i2("cvtsd2ss", x, y); }
void cvtsi2sd(const Arg& x, const Arg& y) { i2(std::string("cvtsi2sd") + suf(x), x, y); }
void cvtss2sd(const Arg& x, const Arg& y) { i2("cvtss2sd", x, y); }
void cvttsd2si(const Arg& x, const Arg& y) { i2_s("cvttsd2si", x, y); }
void dec(const Arg& x) { i1_s("dec", x); }
void divsd(const Arg& x, const Arg& y) { i2("divsd", x, y); }
void idiv(const Arg& x) { i1_s("idiv", x); }
void imul(const Arg& x, const std::optional<Arg>& y) {
  if (y) i2_s("imul", x, *y);
  else i1_s("imul", x);
}
void inc(const Arg& x) { i1_s("inc", x); }
void j(Cond c, const Arg& x) { i1_call_jmp(std::string("j") + string_of_condition(c), x); }
void jmp(const Arg& x) { i1_call_jmp("jmp", x); }
void lea(const Arg& x, const Arg& y) { i2_s("lea", x, y); }
void mov(const Arg& x, const Arg& y) {
  if (x.k == Arg::K::Imm && y.k == Arg::K::Reg64 && !(x.imm <= 0x7FFFFFFFLL && x.imm >= -0x80000000LL))
    i2("movabsq", x, y);
  else
    i2_s("mov", x, y);
}
void movapd(const Arg& x, const Arg& y) { i2("movapd", x, y); }
void movd(const Arg& x, const Arg& y) { i2("movd", x, y); }
void movsd(const Arg& x, const Arg& y) { i2("movsd", x, y); }
void movss(const Arg& x, const Arg& y) { i2("movss", x, y); }
void movsx(const Arg& x, const Arg& y) { i2_ss("movs", x, y); }
void movsxd(const Arg& x, const Arg& y) { i2("movslq", x, y); }
void movzx(const Arg& x, const Arg& y) { i2_ss("movz", x, y); }
void mulsd(const Arg& x, const Arg& y) { i2("mulsd", x, y); }
void neg(const Arg& x) { i1("neg", x); }
void nop() { i0("nop"); }
void or_(const Arg& x, const Arg& y) { i2_s("or", x, y); }
void pop(const Arg& x) { i1_s("pop", x); }
void push(const Arg& x) { i1_s("push", x); }
void ret() { i0("ret"); }
void sal(const Arg& x, const Arg& y) { i2_s("sal", x, y); }
void sar(const Arg& x, const Arg& y) { i2_s("sar", x, y); }
void set(Cond c, const Arg& x) { i1(std::string("set") + string_of_condition(c), x); }
void shr(const Arg& x, const Arg& y) { i2_s("shr", x, y); }
void sqrtsd(const Arg& x, const Arg& y) { i2("sqrtsd", x, y); }
void sub(const Arg& x, const Arg& y) { i2_s("sub", x, y); }
void subsd(const Arg& x, const Arg& y) { i2("subsd", x, y); }
void test(const Arg& x, const Arg& y) { i2_s("test", x, y); }
void ucomisd(const Arg& x, const Arg& y) { i2("ucomisd", x, y); }
void xchg(const Arg& x, const Arg& y) { i2("xchg", x, y); }
void xor_(const Arg& x, const Arg& y) { i2_s("xor", x, y); }
void xorpd(const Arg& x, const Arg& y) { i2("xorpd", x, y); }
}  // namespace I

// constants
struct Constant {
  enum class K { Const, ConstThis, ConstLabel, ConstAdd, ConstSub } k;
  std::int64_t n = 0;
  std::string label;
  std::shared_ptr<Constant> c1, c2;
};
Constant cconst(std::int64_t n) { return Constant{Constant::K::Const, n}; }
Constant clabel(std::string l) {
  Constant c{Constant::K::ConstLabel};
  c.label = std::move(l);
  return c;
}
Constant cthis() { return Constant{Constant::K::ConstThis}; }
Constant cbin(Constant::K k, Constant a, Constant b) {
  Constant c{k};
  c.c1 = std::make_shared<Constant>(std::move(a));
  c.c2 = std::make_shared<Constant>(std::move(b));
  return c;
}
std::string scst(const Constant& c) {
  switch (c.k) {
    case Constant::K::ConstThis: return ".";
    case Constant::K::ConstLabel: return c.label;
    case Constant::K::Const: {
      if (c.n <= 0x7FFFFFFFLL && c.n >= -0x80000000LL) return i64s(c.n);
      char buf[32];
      std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(c.n));
      return buf;
    }
    case Constant::K::ConstAdd: return "(" + scst(*c.c1) + " + " + scst(*c.c2) + ")";
    case Constant::K::ConstSub: return "(" + scst(*c.c1) + " - " + scst(*c.c2) + ")";
  }
  return "";
}
std::string cst(const Constant& c) {
  switch (c.k) {
    case Constant::K::ConstAdd: return scst(*c.c1) + " + " + scst(*c.c2);
    case Constant::K::ConstSub: return scst(*c.c1) + " - " + scst(*c.c2);
    default: return scst(c);
  }
}

// X86_proc.string_of_string_literal
std::string string_of_string_literal(std::string_view s) {
  std::string b;
  bool last_was_escape = false;
  char buf[8];
  for (char ch : s) {
    unsigned char c = static_cast<unsigned char>(ch);
    if (c >= '0' && c <= '9') {
      if (last_was_escape) {
        std::snprintf(buf, sizeof buf, "\\%o", c);
        b += buf;
      } else
        b += static_cast<char>(c);
    } else if (c >= ' ' && c <= '~' && c != '"' && c != '\\') {
      b += static_cast<char>(c);
      last_was_escape = false;
    } else {
      std::snprintf(buf, sizeof buf, "\\%o", c);
      b += buf;
      last_was_escape = true;
    }
  }
  return b;
}

// OCaml's %S (String.escaped, quoted)
std::string ocaml_quoted(std::string_view s) {
  std::string b = "\"";
  char buf[8];
  for (char ch : s) {
    unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': b += "\\\""; break;
      case '\\': b += "\\\\"; break;
      case '\n': b += "\\n"; break;
      case '\t': b += "\\t"; break;
      case '\r': b += "\\r"; break;
      case '\b': b += "\\b"; break;
      default:
        if (c >= ' ' && c <= '~') b += static_cast<char>(c);
        else {
          std::snprintf(buf, sizeof buf, "\\%03u", c);
          b += buf;
        }
    }
  }
  return b + "\"";
}

// X86_dsl.D
namespace D {
void section(const std::vector<std::string>& name, const std::optional<std::string>& flags,
             const std::vector<std::string>& args) {
  if (name.size() == 1 && name[0] == ".data") {
    line("\t.data");
    return;
  }
  if (name.size() == 1 && name[0] == ".text") {
    line("\t.text");
    return;
  }
  std::string s = "\t.section ";
  for (std::size_t k = 0; k < name.size(); ++k) s += (k ? "," : "") + name[k];
  if (flags) s += "," + ocaml_quoted(*flags);
  if (!args.empty()) {
    s += ",";
    for (std::size_t k = 0; k < args.size(); ++k) s += (k ? "," : "") + args[k];
  }
  line(s);
}
void align(long n) { line("\t.align\t" + std::to_string(n)); }
void byte(const Constant& c) { line("\t.byte\t" + cst(c)); }
void bytes(std::string_view s) { line("\t.ascii\t\"" + string_of_string_literal(s) + "\""); }
void cfi_adjust_cfa_offset(long n) { line("\t.cfi_adjust_cfa_offset " + std::to_string(n)); }
void cfi_endproc() { line("\t.cfi_endproc"); }
void cfi_startproc() { line("\t.cfi_startproc"); }
void cfi_remember_state() { line("\t.cfi_remember_state"); }
void cfi_restore_state() { line("\t.cfi_restore_state"); }
void cfi_def_cfa_register(const std::string& reg) { line("\t.cfi_def_cfa_register %" + reg); }
void comment(const std::string& s) { line("\t\t\t\t/* " + s + " */"); }
void data() { line("\t.data"); }
void file(long file_num, std::string_view file_name) {
  line("\t.file\t" + std::to_string(file_num) + "\t\"" + string_of_string_literal(file_name) + "\"");
}
void global(const std::string& s) { line("\t.globl\t" + s); }
void label(const std::string& s) { line(s + ":"); }
void loc(long file_num, long ln, long col) {
  // PR#7726: Location.none uses column -1, breaks LLVM assembler
  if (col >= 0) line("\t.loc\t" + std::to_string(file_num) + "\t" + std::to_string(ln) + "\t" + std::to_string(col));
  else line("\t.loc\t" + std::to_string(file_num) + "\t" + std::to_string(ln));
}
void long_(const Constant& c) { line("\t.long\t" + cst(c)); }
void qword(const Constant& c) { line("\t.quad\t" + cst(c)); }
void size(const std::string& name, const Constant& c) { line("\t.size " + name + "," + cst(c)); }
void space(long n) { line("\t.space\t" + std::to_string(n)); }
void text() { line("\t.text"); }
void type_(const std::string& name, const std::string& typ) { line("\t.type " + name + "," + typ); }
void word(const Constant& c) { line("\t.word\t" + cst(c)); }
}  // namespace D

// ---- Emitaux ----------------------------------------------------------------------------------
struct FrameDebuginfo {  // Dbg_alloc | Dbg_raise | Dbg_other
  enum class K { Dbg_alloc, Dbg_raise, Dbg_other } k;
  std::vector<mach::AllocDbginfo> alloc;
  debuginfo::t dbg;
};
struct FrameDescr {
  long fd_lbl;
  long fd_frame_size;
  std::vector<long> fd_live_offset;
  FrameDebuginfo fd_debuginfo;
};
reg::NewestFirst<FrameDescr> frame_descriptors;  // list order: the newest first

// a debuginfo list copied out of the function's scratch zone (Asmgen): the
// frame table is emitted at end_assembly
debuginfo::t keep_dbg(const debuginfo::t& d) {
  if (d.empty()) return d;
  ZoneScope perm(permanent_zone());
  return slice(std::vector<debuginfo::Item>(d.begin(), d.end()));
}

void record_frame_descr(long label, long frame_size, std::vector<long> live_offset, FrameDebuginfo debuginfo) {
  debuginfo.dbg = keep_dbg(debuginfo.dbg);
  for (mach::AllocDbginfo& a : debuginfo.alloc) a.alloc_dbg = keep_dbg(a.alloc_dbg);
  std::sort(live_offset.begin(), live_offset.end());
  live_offset.erase(std::unique(live_offset.begin(), live_offset.end()), live_offset.end());
  frame_descriptors.push_front(FrameDescr{label, frame_size, live_offset, debuginfo});
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
  bool operator==(const DefKey& o) const {
    return filename == o.filename && defname == o.defname && loc == o.loc;
  }
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
  bool operator==(const DbgKey& o) const {
    return rs == o.rs && debuginfo::compare(slice(rdbg), slice(o.rdbg)) == 0;
  }
};
struct DbgHash {
  long operator()(const DbgKey& k) const {
    using hashtbl::HValue;
    return hashtbl::hash_value(HValue::block(0, {HValue::integer(k.rs ? 1 : 0), HValue::integer(debuginfo_hash(k.rdbg))}));
  }
};

std::string emit_label(long lbl) { return ".L" + std::to_string(lbl); }

void emit_frames() {
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
  auto efa_16 = [](long n) { D::word(cconst(n)); };
  auto efa_16_checked = [&](long n) {
    if (n >= 0x10000) throw std::runtime_error("stack frame too large (" + std::to_string(n) + " bytes)");
    efa_16(n);
  };
  auto efa_label_rel = [](long lbl, std::int32_t ofs) {
    D::long_(cbin(Constant::K::ConstAdd, cbin(Constant::K::ConstSub, clabel(emit_label(lbl)), cthis()), cconst(ofs)));
  };
  auto efa_32 = [](std::int32_t n) { D::long_(cconst(n)); };
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
    D::qword(clabel(emit_label(fd.fd_lbl)));
    efa_16_checked(fd.fd_frame_size + flags);
    efa_16_checked(static_cast<long>(fd.fd_live_offset.size()));
    for (long o : fd.fd_live_offset) efa_16_checked(o);
    if (flags != 0) {
      switch (fd.fd_debuginfo.k) {
        case FrameDebuginfo::K::Dbg_other:
          D::align(4);
          efa_label_rel(label_debuginfos(false, fd.fd_debuginfo.dbg), 0);
          break;
        case FrameDebuginfo::K::Dbg_raise:
          D::align(4);
          efa_label_rel(label_debuginfos(true, fd.fd_debuginfo.dbg), 0);
          break;
        case FrameDebuginfo::K::Dbg_alloc: {
          D::byte(cconst(static_cast<long>(fd.fd_debuginfo.alloc.size())));
          for (auto& d : fd.fd_debuginfo.alloc) D::byte(cconst(d.alloc_words - 2));
          if (flags == 3) {
            D::align(4);
            for (auto& d : fd.fd_debuginfo.alloc) {
              if (debuginfo::is_none(d.alloc_dbg)) efa_32(0);
              else efa_label_rel(label_debuginfos(false, d.alloc_dbg), 0);
            }
          }
          break;
        }
      }
    }
    D::align(8);
  };
  auto emit_filename = [](const std::string& name, long lbl) {
    D::label(emit_label(lbl));
    D::bytes(name + std::string(1, '\0'));
  };
  auto emit_defname = [&](const DefKey& key, std::pair<long, long> v) {
    // These must be 32-bit aligned, both because they contain a 32-bit
    // value, and because emit_debuginfo assumes the low 2 bits of their
    // addresses are 0.
    D::align(4);
    D::label(emit_label(v.second));
    efa_label_rel(v.first, 0);
    // Include the additional 64-bits of location information which didn't
    // pack in the main 64-bit word
    if (key.loc) {
      efa_16(std::get<0>(*key.loc));
      efa_16(std::get<1>(*key.loc));
      efa_32(static_cast<std::int32_t>(std::get<2>(*key.loc)));
    }
    D::bytes(key.defname + std::string(1, '\0'));
  };
  auto fully_pack_info = [](bool fd_raise, const debuginfo::Item& d, bool has_next) {
    std::int64_t kind = fd_raise ? 1 : 0, next = has_next ? 1 : 0;
    std::int64_t char_end = d.dinfo_char_end + d.dinfo_start_bol - d.dinfo_end_bol;
    std::int64_t char_end_offset = d.dinfo_end_bol - d.dinfo_start_bol;
    auto sh = [](std::int64_t x, int n) { return static_cast<std::int64_t>(static_cast<std::uint64_t>(x) << n); };
    return sh(d.dinfo_line, 51) + sh(d.dinfo_end_line - d.dinfo_line, 48) + sh(d.dinfo_char_start, 42) +
           sh(char_end, 35) + sh(char_end_offset, 26) + sh(kind, 1) + next;
  };
  auto partially_pack_info = [](bool fd_raise, const debuginfo::Item& d, bool has_next) {
    std::int64_t start_line = std::min<std::int64_t>(0x7FFFF, d.dinfo_line);
    std::int64_t end_line = std::min<std::int64_t>(0x3FFFF, d.dinfo_end_line - d.dinfo_line);
    std::int64_t kind = fd_raise ? 1 : 0, next = has_next ? 1 : 0;
    auto sh = [](std::int64_t x, int n) { return static_cast<std::int64_t>(static_cast<std::uint64_t>(x) << n); };
    return sh(1, 63) + sh(start_line, 44) + sh(end_line, 26) + sh(kind, 1) + next;
  };
  auto emit_debuginfo = [&](const DbgKey& key, long lbl) {
    // Due to inlined functions, a single debuginfo may have multiple
    // locations.  These are represented sequentially in memory (innermost
    // frame first), with the low bit of the packed debuginfo being 0 on the
    // last entry.
    D::align(4);
    D::label(emit_label(lbl));
    if (key.rdbg.empty()) fatal("Emitaux.emit_debuginfo");
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
      efa_label_rel(label_defname(std::string(d.dinfo_file), defname, loc), static_cast<std::int32_t>(info));
      efa_32(static_cast<std::int32_t>(info >> 32));
      rs = false;
    }
  };
  D::qword(cconst(static_cast<long>(frame_descriptors.size())));
  for (const FrameDescr& fd : frame_descriptors) emit_frame(fd);
  for (auto& [k, lbl] : debuginfos.to_seq()) emit_debuginfo(k, lbl);
  for (auto& [n, lbl] : filenames.to_seq()) emit_filename(n, lbl);
  for (auto& [k, v] : defnames.to_seq()) emit_defname(k, v);
  D::align(8);
  frame_descriptors.clear();
}

// Emit debug information (only with -g: Config.asm_cfi_supported)
std::vector<std::pair<std::string, long>> file_pos_nums;  // list order: the newest first
long file_pos_num_cnt = 1;
void reset_debug_info() {
  file_pos_nums.clear();
  file_pos_num_cnt = 1;
}
void emit_debug_info(const debuginfo::t& dbg) {
  if (!clflags::debug) return;  // Config.with_frame_pointers = false
  if (dbg.empty()) return;
  const debuginfo::Item& d = dbg[dbg.size() - 1];  // (List.rev dbg)'s head
  if (d.dinfo_line <= 0) return;  // PR#6243
  std::string file_name(d.dinfo_file);
  long file_num = -1;
  for (auto& [n, k] : file_pos_nums)
    if (n == file_name) {
      file_num = k;
      break;
    }
  if (file_num < 0) {
    file_num = file_pos_num_cnt++;
    D::file(file_num, file_name);
    file_pos_nums.insert(file_pos_nums.begin(), {file_name, file_num});
  }
  D::loc(file_num, d.dinfo_line, d.dinfo_char_start);
}

// ---- Emit (amd64) -----------------------------------------------------------------------------
const R64 int_reg_name[] = {R64::RAX, R64::RBX, R64::RDI, R64::RSI, R64::RDX, R64::RCX, R64::R8,
                            R64::R9,  R64::R12, R64::R13, R64::R10, R64::R11, R64::RBP};

Arg register_name(long r) { return r < 100 ? reg64a(int_reg_name[r]) : xmm(static_cast<int>(r - 100)); }

constexpr long stack_threshold_size = 32 * 8;  // Config.stack_threshold * 8 (bytes)
constexpr long stack_ctx_words = 7;           // Domainstate.stack_ctx_words
enum class DomainField { young_limit = 0, current_stack = 5, exn_handler = 6, c_stack = 8, dls_root = 42, extra_params = 65 };

struct GcCall {
  long gc_lbl, gc_return_lbl, gc_frame_lbl;
};
struct BoundErrorCall {
  long bd_lbl, bd_frame;
};
struct Env {
  const linear::Fundecl* f;
  long stack_offset = 0;
  reg::NewestFirst<GcCall> call_gc_sites;  // list order: the newest first
  reg::NewestFirst<BoundErrorCall> bound_error_sites;  // list order: the newest first
  std::optional<long> bound_error_call;
};

long frame_size(const Env& env) {  // includes return address
  if (env.f->fun_frame_required)
    return env.stack_offset + 8 * (env.f->fun_num_stack_slots[0] + env.f->fun_num_stack_slots[1]) + 8;
  return env.stack_offset + 8;
}

long slot_offset(const Env& env, const reg::Location& loc, long cl) {
  switch (loc.k) {
    case reg::Location::K::Incoming: return frame_size(env) + loc.n;
    case reg::Location::K::Local:
      if (cl == 0) return env.stack_offset + loc.n * 8;
      return env.stack_offset + (env.f->fun_num_stack_slots[0] + loc.n) * 8;
    case reg::Location::K::Outgoing: return loc.n;
    default: fatal("Emit.slot_offset");
  }
}

// Symbols
std::string emit_symbol(std::string_view s) {
  auto is_special = [](char c) {
    return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') && c != '.';
  };
  if (std::none_of(s.begin(), s.end(), is_special)) return std::string(s);
  std::string b;
  char buf[8];
  for (char c : s) {
    if (is_special(c)) {
      std::snprintf(buf, sizeof buf, "$%02x", static_cast<unsigned char>(c));
      b += buf;
    } else
      b += c;
  }
  return b;
}

std::set<std::string> symbols_defined, symbols_used;
void add_def_symbol(std::string_view s) { symbols_defined.insert(std::string(s)); }
void add_used_symbol(std::string_view s) { symbols_used.insert(std::string(s)); }

// X86_proc.use_plt (evaluated at initialization: Clflags.dlcode's default)
Arg rel_plt(std::string_view s) { return sym(emit_symbol(s) + "@PLT"); }
void emit_call(std::string_view s) { I::call(rel_plt(s)); }
void emit_jump(std::string_view s) { I::jmp(rel_plt(s)); }

void load_symbol_addr(std::string_view s, const Arg& arg) {
  if (clflags::dlcode) I::mov(mem64_rip(DataType::QWORD, emit_symbol(s) + "@GOTPCREL"), arg);
  else if (clflags::pic_code) I::lea(mem64_rip(DataType::NONE, emit_symbol(s)), arg);
  else I::mov(sym(emit_symbol(s)), arg);
}

Arg domain_field(DomainField f) { return mem64(DataType::QWORD, static_cast<long>(f) * 8, R64::R14); }

Arg label(long l) { return sym(emit_label(l)); }
void def_label(long l) { D::label(emit_label(l)); }

void emit_Llabel(const Env& env, bool fallthrough, long lbl) {
  if (!fallthrough && env.f->fun_fast) D::align(4);
  def_label(lbl);
}

DataType x86_data_type_for_stack_slot(cmm::MachtypeComponent ty) {
  return ty == cmm::MachtypeComponent::Float ? DataType::REAL8 : DataType::QWORD;
}

Arg reg(const Env& env, const reg::Reg* r) {
  using LK = reg::Location::K;
  switch (r->loc.k) {
    case LK::Reg: return register_name(r->loc.n);
    case LK::Domainstate: {
      long ofs = r->loc.n + static_cast<long>(DomainField::extra_params) * 8;
      return mem64(x86_data_type_for_stack_slot(r->typ), ofs, R64::R14);
    }
    case LK::Local:
    case LK::Incoming:
    case LK::Outgoing:
      return mem64(x86_data_type_for_stack_slot(r->typ), slot_offset(env, r->loc, proc::register_class(r)), R64::RSP);
    case LK::Unknown: fatal("Emit.reg");
  }
  return rax;
}

R64 reg64(const reg::Reg* r) {
  if (r->loc.k != reg::Location::K::Reg) fatal("Emit.reg64");
  return int_reg_name[r->loc.n];
}

// Output a reference to the lower 8, 16 or 32 bits of a register
Arg emit_subreg(const Env& env, Arg::K k, DataType typ, const reg::Reg* r) {
  if (r->loc.k == reg::Location::K::Reg && r->loc.n < 13) return regk(k, int_reg_name[r->loc.n]);
  if (r->loc.k == reg::Location::K::Local || r->loc.k == reg::Location::K::Incoming ||
      r->loc.k == reg::Location::K::Outgoing || r->loc.k == reg::Location::K::Domainstate)
    return mem64(typ, slot_offset(env, r->loc, proc::register_class(r)), R64::RSP);
  fatal("Emit.emit_subreg");
}

// Output an addressing mode
Arg addressing(const arch::AddressingMode& addr, DataType typ, linear::Instr i, std::size_t n) {
  using AK = arch::AddressingMode::K;
  switch (addr.k) {
    case AK::Ibased:
      add_used_symbol(addr.sym);
      return mem64_rip(typ, emit_symbol(addr.sym), addr.displ);
    case AK::Iindexed: return mem64(typ, addr.displ, reg64(i->arg[n]));
    case AK::Iindexed2: return mem64(typ, addr.displ, reg64(i->arg[n + 1]), 1, reg64(i->arg[n]));
    case AK::Iscaled:
      if (addr.scale == 2) return mem64(typ, addr.displ, reg64(i->arg[n]), 1, reg64(i->arg[n]));
      return mem64(typ, addr.displ, reg64(i->arg[n]), addr.scale);
    case AK::Iindexed2scaled: return mem64(typ, addr.displ, reg64(i->arg[n + 1]), addr.scale, reg64(i->arg[n]));
  }
  return rax;
}

// Record live pointers at call points -- see Emitaux
long record_frame_label(const Env& env, const reg::Set& live, FrameDebuginfo dbg) {
  long lbl = cmm::new_label();
  std::vector<long> live_offset;  // consed: built in reverse, then reversed
  for (reg::Reg* r : live) {
    if (r->typ == cmm::MachtypeComponent::Val) {
      if (r->loc.k == reg::Location::K::Reg) live_offset.push_back((r->loc.n << 1) + 1);
      else if (r->loc.k != reg::Location::K::Unknown)
        live_offset.push_back(slot_offset(env, r->loc, proc::register_class(r)));
    } else if (r->typ == cmm::MachtypeComponent::Addr) {
      fatal("bad GC root " + reg::name(r));
    }
  }
  std::reverse(live_offset.begin(), live_offset.end());
  record_frame_descr(lbl, frame_size(env), live_offset, std::move(dbg));
  return lbl;
}

void record_frame(const Env& env, const reg::Set& live, FrameDebuginfo dbg) {
  def_label(record_frame_label(env, live, std::move(dbg)));
}

FrameDebuginfo dbg_other(const debuginfo::t& d) { return {FrameDebuginfo::K::Dbg_other, {}, d}; }
FrameDebuginfo dbg_raise(const debuginfo::t& d) { return {FrameDebuginfo::K::Dbg_raise, {}, d}; }
FrameDebuginfo dbg_alloc(const std::vector<mach::AllocDbginfo>& a) { return {FrameDebuginfo::K::Dbg_alloc, a, {}}; }

void emit_call_gc(const GcCall& gc) {
  def_label(gc.gc_lbl);
  emit_call("caml_call_gc");
  def_label(gc.gc_frame_lbl);
  I::jmp(label(gc.gc_return_lbl));
}

long bound_error_label(Env& env, const debuginfo::t& dbg) {
  if (clflags::debug) {
    long lbl_bound_error = cmm::new_label();
    long lbl_frame = record_frame_label(env, {}, dbg_other(dbg));
    env.bound_error_sites.push_front({lbl_bound_error, lbl_frame});
    return lbl_bound_error;
  }
  if (!env.bound_error_call) env.bound_error_call = cmm::new_label();
  return *env.bound_error_call;
}

void emit_call_bound_errors(const Env& env) {
  for (const BoundErrorCall& bd : env.bound_error_sites) {
    def_label(bd.bd_lbl);
    emit_call("caml_ml_array_bound_error");
    def_label(bd.bd_frame);
  }
  if (env.bound_error_call) {
    def_label(*env.bound_error_call);
    emit_call("caml_ml_array_bound_error");
  }
}

// Names for instructions
using Op2 = void (*)(const Arg&, const Arg&);
Op2 instr_for_intop(mach::IntegerOperation op) {
  using IO = mach::IntegerOperation;
  switch (op) {
    case IO::Iadd: return I::add;
    case IO::Isub: return I::sub;
    case IO::Imul: return [](const Arg& a, const Arg& b) { I::imul(a, b); };
    case IO::Iand: return I::and_;
    case IO::Ior: return I::or_;
    case IO::Ixor: return I::xor_;
    case IO::Ilsl: return I::sal;
    case IO::Ilsr: return I::shr;
    case IO::Iasr: return I::sar;
    default: fatal("Emit.instr_for_intop");
  }
}
Op2 instr_for_floatop(mach::Operation::K k) {
  using MK = mach::Operation::K;
  switch (k) {
    case MK::Iaddf: return I::addsd;
    case MK::Isubf: return I::subsd;
    case MK::Imulf: return I::mulsd;
    case MK::Idivf: return I::divsd;
    default: fatal("Emit.instr_for_floatop");
  }
}
Op2 instr_for_floatarithmem(arch::FloatOperation op) {
  switch (op) {
    case arch::FloatOperation::Ifloatadd: return I::addsd;
    case arch::FloatOperation::Ifloatsub: return I::subsd;
    case arch::FloatOperation::Ifloatmul: return I::mulsd;
    case arch::FloatOperation::Ifloatdiv: return I::divsd;
  }
  return I::addsd;
}

Cond cond(const mach::IntegerComparison& c) {
  using IC = lambda::IntegerComparison;
  if (c.is_signed) switch (c.c) {
      case IC::Ceq: return Cond::E;
      case IC::Cne: return Cond::NE;
      case IC::Cle: return Cond::LE;
      case IC::Cgt: return Cond::G;
      case IC::Clt: return Cond::L;
      case IC::Cge: return Cond::GE;
    }
  switch (c.c) {
    case IC::Ceq: return Cond::E;
    case IC::Cne: return Cond::NE;
    case IC::Cle: return Cond::BE;
    case IC::Cgt: return Cond::A;
    case IC::Clt: return Cond::B;
    case IC::Cge: return Cond::AE;
  }
  return Cond::E;
}

// Output an = 0 or <> 0 test.
void output_test_zero(const Env& env, const reg::Reg* a) {
  if (a->loc.k == reg::Location::K::Reg) I::test(reg(env, a), reg(env, a));
  else I::cmp(imm(0), reg(env, a));
}

// Output a floating-point compare and branch
void emit_float_test(const Env& env, lambda::FloatComparison cmp, linear::Instr i, const Arg& lbl) {
  using FC = lambda::FloatComparison;
  auto arg = [&](std::size_t n) { return reg(env, i->arg[n]); };
  switch (cmp) {
    case FC::CFeq: {
      long next = cmm::new_label();
      I::ucomisd(arg(1), arg(0));
      I::j(Cond::P, label(next));  // skip if unordered
      I::j(Cond::E, lbl);          // branch taken if x=y
      def_label(next);
      return;
    }
    case FC::CFneq:
      I::ucomisd(arg(1), arg(0));
      I::j(Cond::P, lbl);  // branch taken if unordered
      I::j(Cond::NE, lbl);  // branch taken if x<y or x>y
      return;
    case FC::CFlt:
      I::comisd(arg(0), arg(1));
      I::j(Cond::A, lbl);
      return;
    case FC::CFnlt:
      I::comisd(arg(0), arg(1));
      I::j(Cond::BE, lbl);
      return;
    case FC::CFle:
      I::comisd(arg(0), arg(1));
      I::j(Cond::AE, lbl);
      return;
    case FC::CFnle:
      I::comisd(arg(0), arg(1));
      I::j(Cond::B, lbl);
      return;
    case FC::CFgt:
      I::comisd(arg(1), arg(0));
      I::j(Cond::A, lbl);
      return;
    case FC::CFngt:
      I::comisd(arg(1), arg(0));
      I::j(Cond::BE, lbl);
      return;
    case FC::CFge:
      I::comisd(arg(1), arg(0));
      I::j(Cond::AE, lbl);
      return;
    case FC::CFnge:
      I::comisd(arg(1), arg(0));
      I::j(Cond::B, lbl);
      return;
  }
}

// Deallocate the stack frame before a return or tail call
void output_epilogue(const Env& env, const std::function<void()>& f) {
  if (env.f->fun_frame_required) {
    long n = frame_size(env) - 8;
    if (n != 0) {
      I::add(imm(n), rsp);
      D::cfi_adjust_cfa_offset(-n);
    }
    f();
    // reset CFA back cause function body may continue
    if (n != 0) D::cfi_adjust_cfa_offset(n);
  } else
    f();
}

// Floating-point constants
reg::NewestFirst<std::pair<std::int64_t, long>> float_constants;  // list order: the newest first

long add_float_constant(std::int64_t c) {
  for (auto& [k, l] : float_constants)
    if (k == c) return l;
  long lbl = cmm::new_label();
  float_constants.push_front({c, lbl});
  return lbl;
}

void emit_global_label(const std::string& s) {
  std::string lbl(compilenv::make_symbol(std::string_view(s)));
  add_def_symbol(lbl);
  std::string e = emit_symbol(lbl);
  D::global(e);
  D::label(e);
}

// Output .text section directive, or named .text.caml.<name> if enabled
void emit_named_text_section(std::string_view func_name) {
  if (clflags::function_sections)
    D::section({".text.caml." + emit_symbol(func_name)}, std::string("ax"), {"@progbits"});
  else
    D::text();
}

bool same_loc(const reg::Location& a, const reg::Location& b) { return a.k == b.k && a.n == b.n; }

// Emit an instruction
void emit_instr(Env& env, bool fallthrough, linear::Instr i) {
  using LK = linear::Instruction::K;
  using MK = mach::Operation::K;
  using IO = mach::IntegerOperation;
  using SK = arch::SpecificOperation::K;
  using MC = cmm::MemoryChunk;
  auto arg = [&](std::size_t n) { return reg(env, i->arg[n]); };
  auto res = [&](std::size_t n) { return reg(env, i->res[n]); };
  auto arg8 = [&](std::size_t n) { return emit_subreg(env, Arg::K::Reg8L, DataType::BYTE, i->arg[n]); };
  auto arg16 = [&](std::size_t n) { return emit_subreg(env, Arg::K::Reg16, DataType::WORD, i->arg[n]); };
  auto arg32 = [&](std::size_t n) { return emit_subreg(env, Arg::K::Reg32, DataType::DWORD, i->arg[n]); };
  auto res16 = [&](std::size_t n) { return emit_subreg(env, Arg::K::Reg16, DataType::WORD, i->res[n]); };
  auto res32 = [&](std::size_t n) { return emit_subreg(env, Arg::K::Reg32, DataType::DWORD, i->res[n]); };
  auto arg64 = [&](std::size_t n) { return reg64(i->arg[n]); };
  emit_debug_info(i->dbg);
  switch (i->desc) {
    case LK::Lend: return;
    case LK::Lprologue:
      if (env.f->fun_frame_required) {
        long n = frame_size(env) - 8;
        if (n != 0) {
          I::sub(imm(n), rsp);
          D::cfi_adjust_cfa_offset(n);
        }
      }
      return;
    case LK::Lop: break;
    case LK::Lreloadretaddr: return;
    case LK::Lreturn: output_epilogue(env, [] { I::ret(); }); return;
    case LK::Llabel: emit_Llabel(env, fallthrough, i->lbl); return;
    case LK::Lbranch: I::jmp(label(i->lbl)); return;
    case LK::Lcondbranch: {
      Arg lbl = label(i->lbl);
      const mach::Test& tst = i->test;
      switch (tst.k) {
        case mach::Test::K::Itruetest:
          output_test_zero(env, i->arg[0]);
          I::j(Cond::NE, lbl);
          return;
        case mach::Test::K::Ifalsetest:
          output_test_zero(env, i->arg[0]);
          I::j(Cond::E, lbl);
          return;
        case mach::Test::K::Iinttest:
          I::cmp(arg(1), arg(0));
          I::j(cond(tst.icmp), lbl);
          return;
        case mach::Test::K::Iinttest_imm:
          if ((tst.icmp.c == lambda::IntegerComparison::Ceq || tst.icmp.c == lambda::IntegerComparison::Cne) &&
              tst.n == 0) {
            output_test_zero(env, i->arg[0]);
            I::j(cond(tst.icmp), lbl);
            return;
          }
          I::cmp(imm(tst.n), arg(0));
          I::j(cond(tst.icmp), lbl);
          return;
        case mach::Test::K::Ifloattest: emit_float_test(env, tst.fcmp, i, lbl); return;
        case mach::Test::K::Ioddtest:
          I::test(imm(1), arg8(0));
          I::j(Cond::NE, lbl);
          return;
        case mach::Test::K::Ieventest:
          I::test(imm(1), arg8(0));
          I::j(Cond::E, lbl);
          return;
      }
      return;
    }
    case LK::Lcondbranch3:
      I::cmp(imm(1), arg(0));
      if (i->lbl0) I::j(Cond::B, label(*i->lbl0));
      if (i->lbl1) I::j(Cond::E, label(*i->lbl1));
      if (i->lbl2) I::j(Cond::A, label(*i->lbl2));
      return;
    case LK::Lswitch: {
      std::string lbl = emit_label(cmm::new_label());
      // rax and rdx are clobbered by the Lswitch, meaning that no variable
      // that is live across the Lswitch is assigned to rax or rdx.  However,
      // the argument to Lswitch can still be assigned to one of these two
      // registers, so we must be careful not to clobber it before use.
      bool in_rax = i->arg[0]->loc.k == reg::Location::K::Reg && i->arg[0]->loc.n == 0;
      reg::Reg* tmp1 = proc::phys_reg(in_rax ? 4 : 0);
      reg::Reg* tmp2 = proc::phys_reg(in_rax ? 0 : 4);
      I::lea(mem64_rip(DataType::NONE, lbl), reg(env, tmp1));
      I::movsxd(mem64(DataType::DWORD, 0, arg64(0), 4, reg64(tmp1)), reg(env, tmp2));
      I::add(reg(env, tmp2), reg(env, tmp1));
      I::jmp(reg(env, tmp1));
      D::section({".rodata"}, std::nullopt, {});
      D::align(4);
      D::label(lbl);
      for (long l : i->lbls) D::long_(cbin(Constant::K::ConstSub, clabel(emit_label(l)), clabel(lbl)));
      emit_named_text_section(env.f->fun_name);
      return;
    }
    case LK::Lentertrap: return;  // no frame pointers
    case LK::Ladjust_trap_depth: {
      // each trap occupies 16 bytes on the stack
      long delta = 16 * i->delta_traps;
      D::cfi_adjust_cfa_offset(delta);
      env.stack_offset += delta;
      return;
    }
    case LK::Lpushtrap:
      if (clflags::pic_code) I::lea(mem64_rip(DataType::NONE, emit_label(i->lbl)), r11);
      else I::mov(sym(emit_label(i->lbl)), r11);
      I::push(r11);
      D::cfi_adjust_cfa_offset(8);
      I::push(domain_field(DomainField::exn_handler));
      D::cfi_adjust_cfa_offset(8);
      I::mov(rsp, domain_field(DomainField::exn_handler));
      env.stack_offset += 16;
      return;
    case LK::Lpoptrap:
      I::pop(domain_field(DomainField::exn_handler));
      D::cfi_adjust_cfa_offset(-8);
      I::add(imm(8), rsp);
      D::cfi_adjust_cfa_offset(-8);
      env.stack_offset -= 16;
      return;
    case LK::Lraise:
      switch (i->raise) {
        case lambda::RaiseKind::Raise_regular:
          emit_call("caml_raise_exn");
          record_frame(env, {}, dbg_raise(i->dbg));
          return;
        case lambda::RaiseKind::Raise_reraise:
          emit_call("caml_reraise_exn");
          record_frame(env, {}, dbg_raise(i->dbg));
          return;
        case lambda::RaiseKind::Raise_notrace:
          I::mov(domain_field(DomainField::exn_handler), rsp);
          I::pop(domain_field(DomainField::exn_handler));
          I::pop(r11);
          I::jmp(r11);
          return;
      }
      return;
  }
  // Lop
  const mach::Operation& op = i->op;
  switch (op.k) {
    case MK::Imove:
    case MK::Ispill:
    case MK::Ireload: {
      const reg::Reg* src = i->arg[0];
      const reg::Reg* dst = i->res[0];
      if (!same_loc(src->loc, dst->loc)) {
        if (src->typ == cmm::MachtypeComponent::Float) {
          if (src->loc.k == reg::Location::K::Reg && dst->loc.k == reg::Location::K::Reg)
            I::movapd(reg(env, src), reg(env, dst));
          else
            I::movsd(reg(env, src), reg(env, dst));
        } else
          I::mov(reg(env, src), reg(env, dst));
      }
      return;
    }
    case MK::Iconst_int: {
      std::int64_t n = op.n;
      bool in_reg = i->res[0]->loc.k == reg::Location::K::Reg;
      if (n == 0) {
        // Clearing the bottom half also clears the top half
        if (in_reg) I::xor_(res32(0), res32(0));
        else I::mov(imm(0), res(0));
      } else if (n > 0 && n <= 0xFFFFFFFFLL) {
        // Similarly, setting only the bottom half clears the top half.
        if (in_reg) I::mov(imm(n), res32(0));
        else I::mov(imm(n), res(0));
      } else
        I::mov(imm(n), res(0));
      return;
    }
    case MK::Iconst_float:
      if (op.n == 0) I::xorpd(res(0), res(0));  // +0.0
      else {
        long lbl = add_float_constant(op.n);
        I::movsd(mem64_rip(DataType::NONE, emit_label(lbl)), res(0));
      }
      return;
    case MK::Iconst_symbol:
      add_used_symbol(op.func);
      load_symbol_addr(op.func, res(0));
      return;
    case MK::Icall_ind:
      I::call(arg(0));
      record_frame(env, i->live, dbg_other(i->dbg));
      return;
    case MK::Icall_imm:
      add_used_symbol(op.func);
      emit_call(op.func);
      record_frame(env, i->live, dbg_other(i->dbg));
      return;
    case MK::Itailcall_ind: output_epilogue(env, [&] { I::jmp(arg(0)); }); return;
    case MK::Itailcall_imm:
      if (op.func == env.f->fun_name) I::jmp(label(env.f->fun_tailrec_entry_point_label));
      else
        output_epilogue(env, [&] {
          add_used_symbol(op.func);
          emit_jump(op.func);
        });
      return;
    case MK::Iextcall:
      add_used_symbol(op.func);
      if (op.stack_ofs > 0) {
        I::mov(rsp, r13);
        I::lea(mem64(DataType::QWORD, op.stack_ofs, R64::RSP), r12);
        load_symbol_addr(op.func, rax);
        emit_call("caml_c_call_stack_args");
        record_frame(env, i->live, dbg_other(i->dbg));
      } else if (op.alloc) {
        load_symbol_addr(op.func, rax);
        emit_call("caml_c_call");
        record_frame(env, i->live, dbg_other(i->dbg));
      } else {
        I::mov(rsp, rbx);
        D::cfi_remember_state();
        D::cfi_def_cfa_register("rbx");
        I::mov(domain_field(DomainField::c_stack), rsp);
        emit_call(op.func);
        I::mov(rbx, rsp);
        D::cfi_restore_state();
      }
      return;
    case MK::Istackoffset: {
      long n = op.n;
      if (n < 0) I::add(imm(-n), rsp);
      else if (n > 0) I::sub(imm(n), rsp);
      if (n != 0) D::cfi_adjust_cfa_offset(n);
      env.stack_offset += n;
      return;
    }
    case MK::Iload: {
      Arg dest = res(0);
      switch (op.chunk) {
        case MC::Word_int:
        case MC::Word_val:
        case MC::Sixtyfour: I::mov(addressing(op.addr, DataType::QWORD, i, 0), dest); return;
        case MC::Byte_unsigned: I::movzx(addressing(op.addr, DataType::BYTE, i, 0), dest); return;
        case MC::Byte_signed: I::movsx(addressing(op.addr, DataType::BYTE, i, 0), dest); return;
        case MC::Sixteen_unsigned: I::movzx(addressing(op.addr, DataType::WORD, i, 0), dest); return;
        case MC::Sixteen_signed: I::movsx(addressing(op.addr, DataType::WORD, i, 0), dest); return;
        case MC::Thirtytwo_unsigned: I::mov(addressing(op.addr, DataType::DWORD, i, 0), res32(0)); return;
        case MC::Thirtytwo_signed: I::movsxd(addressing(op.addr, DataType::DWORD, i, 0), dest); return;
        case MC::Single:
          I::xorpd(dest, dest);  // avoid partial register stall
          I::cvtss2sd(addressing(op.addr, DataType::REAL4, i, 0), dest);
          return;
        case MC::Double: I::movsd(addressing(op.addr, DataType::REAL8, i, 0), dest); return;
      }
      return;
    }
    case MK::Istore:
      switch (op.chunk) {
        case MC::Word_int:
        case MC::Word_val:
        case MC::Sixtyfour: I::mov(arg(0), addressing(op.addr, DataType::QWORD, i, 1)); return;
        case MC::Byte_unsigned:
        case MC::Byte_signed: I::mov(arg8(0), addressing(op.addr, DataType::BYTE, i, 1)); return;
        case MC::Sixteen_unsigned:
        case MC::Sixteen_signed: I::mov(arg16(0), addressing(op.addr, DataType::WORD, i, 1)); return;
        case MC::Thirtytwo_signed:
        case MC::Thirtytwo_unsigned: I::mov(arg32(0), addressing(op.addr, DataType::DWORD, i, 1)); return;
        case MC::Single:
          I::cvtsd2ss(arg(0), xmm15);
          I::movss(xmm15, addressing(op.addr, DataType::REAL4, i, 1));
          return;
        case MC::Double: I::movsd(arg(0), addressing(op.addr, DataType::REAL8, i, 1)); return;
      }
      return;
    case MK::Ialloc: {
      long n = op.n;
      if (env.f->fun_fast) {
        I::sub(imm(n), r15);
        I::cmp(domain_field(DomainField::young_limit), r15);
        long lbl_call_gc = cmm::new_label();
        long lbl_frame = record_frame_label(env, i->live, dbg_alloc(op.dbginfo));
        I::j(Cond::B, label(lbl_call_gc));
        long lbl_after_alloc = cmm::new_label();
        def_label(lbl_after_alloc);
        I::lea(mem64(DataType::NONE, 8, R64::R15), res(0));
        env.call_gc_sites.push_front({lbl_call_gc, lbl_after_alloc, lbl_frame});
      } else {
        switch (n) {
          case 16: emit_call("caml_alloc1"); break;
          case 24: emit_call("caml_alloc2"); break;
          case 32: emit_call("caml_alloc3"); break;
          default:
            I::sub(imm(n), r15);
            emit_call("caml_allocN");
        }
        long l = record_frame_label(env, i->live, dbg_alloc(op.dbginfo));
        def_label(l);
        I::lea(mem64(DataType::NONE, 8, R64::R15), res(0));
      }
      return;
    }
    case MK::Ipoll: {
      I::cmp(domain_field(DomainField::young_limit), r15);
      long gc_call_label = cmm::new_label();
      long lbl_after_poll = op.return_label ? *op.return_label : cmm::new_label();
      long lbl_frame = record_frame_label(env, i->live, dbg_alloc({}));
      if (!op.return_label) I::j(Cond::BE, label(gc_call_label));
      else I::j(Cond::A, label(*op.return_label));
      env.call_gc_sites.push_front({gc_call_label, lbl_after_poll, lbl_frame});
      if (!op.return_label) def_label(lbl_after_poll);
      else I::jmp(label(gc_call_label));
      return;
    }
    case MK::Iintop:
      switch (op.intop.op) {
        case IO::Icomp:
          I::cmp(arg(1), arg(0));
          I::set(cond(op.intop.cmp), al);
          I::movzx(al, res(0));
          return;
        case IO::Icheckbound: {
          long lbl = bound_error_label(env, i->dbg);
          I::cmp(arg(1), arg(0));
          I::j(Cond::BE, label(lbl));
          return;
        }
        case IO::Idiv:
        case IO::Imod:
          I::cqo();
          I::idiv(arg(1));
          return;
        case IO::Ilsl:
        case IO::Ilsr:
        case IO::Iasr:
          // We have i.arg.(0) = i.res.(0) and i.arg.(1) = %rcx
          instr_for_intop(op.intop.op)(cl, res(0));
          return;
        case IO::Imulh: I::imul(arg(1), std::nullopt); return;
        default:
          // We have i.arg.(0) = i.res.(0)
          instr_for_intop(op.intop.op)(arg(1), res(0));
          return;
      }
    case MK::Iintop_imm: {
      long n = op.n;
      switch (op.intop.op) {
        case IO::Icomp:
          I::cmp(imm(n), arg(0));
          I::set(cond(op.intop.cmp), al);
          I::movzx(al, res(0));
          return;
        case IO::Icheckbound: {
          long lbl = bound_error_label(env, i->dbg);
          I::cmp(imm(n), arg(0));
          I::j(Cond::BE, label(lbl));
          return;
        }
        default: break;
      }
      if (op.intop.op == IO::Iadd && !same_loc(i->arg[0]->loc, i->res[0]->loc)) {
        I::lea(mem64(DataType::NONE, n, arg64(0)), res(0));
        return;
      }
      if ((op.intop.op == IO::Iadd && n == 1) || (op.intop.op == IO::Isub && n == -1)) {
        I::inc(res(0));
        return;
      }
      if ((op.intop.op == IO::Iadd && n == -1) || (op.intop.op == IO::Isub && n == 1)) {
        I::dec(res(0));
        return;
      }
      // We have i.arg.(0) = i.res.(0)
      instr_for_intop(op.intop.op)(imm(n), res(0));
      return;
    }
    case MK::Icompf: {
      using FC = lambda::FloatComparison;
      const char* c;
      bool need_swap;
      switch (op.fcmp) {
        case FC::CFeq: c = "eq"; need_swap = false; break;
        case FC::CFneq: c = "neq"; need_swap = false; break;
        case FC::CFlt: c = "lt"; need_swap = false; break;
        case FC::CFnlt: c = "nlt"; need_swap = false; break;
        case FC::CFgt: c = "lt"; need_swap = true; break;
        case FC::CFngt: c = "nlt"; need_swap = true; break;
        case FC::CFle: c = "le"; need_swap = false; break;
        case FC::CFnle: c = "nle"; need_swap = false; break;
        case FC::CFge: c = "le"; need_swap = true; break;
        default: c = "nle"; need_swap = true; break;  // CFnge
      }
      Arg a0 = need_swap ? arg(1) : arg(0);
      Arg a1 = need_swap ? arg(0) : arg(1);
      I::cmpsd(c, a1, a0);
      I::movd(a0, res(0));
      I::neg(res(0));
      return;
    }
    case MK::Inegf: I::xorpd(mem64_rip(DataType::OWORD, emit_symbol("caml_negf_mask")), res(0)); return;
    case MK::Iabsf: I::andpd(mem64_rip(DataType::OWORD, emit_symbol("caml_absf_mask")), res(0)); return;
    case MK::Iaddf:
    case MK::Isubf:
    case MK::Imulf:
    case MK::Idivf: instr_for_floatop(op.k)(arg(1), res(0)); return;
    case MK::Ifloatofint:
      I::xorpd(res(0), res(0));  // avoid partial register stall
      I::cvtsi2sd(arg(0), res(0));
      return;
    case MK::Iintoffloat: I::cvttsd2si(arg(0), res(0)); return;
    case MK::Iopaque: return;
    case MK::Ispecific:
      switch (op.spec.k) {
        case SK::Ilea: I::lea(addressing(op.spec.addr, DataType::NONE, i, 0), res(0)); return;
        case SK::Istore_int: I::mov(imm(op.spec.n), addressing(op.spec.addr, DataType::QWORD, i, 0)); return;
        case SK::Ioffset_loc: I::add(imm(op.spec.n), addressing(op.spec.addr, DataType::QWORD, i, 0)); return;
        case SK::Ifloatarithmem:
          instr_for_floatarithmem(op.spec.fop)(addressing(op.spec.addr, DataType::REAL8, i, 1), res(0));
          return;
        case SK::Ibswap:
          if (op.spec.n == 16) {
            I::xchg(ah, al);
            I::movzx(res16(0), res(0));
          } else if (op.spec.n == 32) {
            I::bswap(res32(0));
            I::movsxd(res32(0), res(0));
          } else if (op.spec.n == 64)
            I::bswap(res(0));
          else
            fatal("Emit: Ibswap");
          return;
        case SK::Isqrtf:
          if (arg(0) != res(0)) I::xorpd(res(0), res(0));  // avoid partial register stall
          I::sqrtsd(arg(0), res(0));
          return;
        case SK::Ifloatsqrtf:
          I::xorpd(res(0), res(0));  // avoid partial register stall
          I::sqrtsd(addressing(op.spec.addr, DataType::REAL8, i, 0), res(0));
          return;
        case SK::Isextend32: I::movsxd(arg32(0), res(0)); return;
        case SK::Izextend32: I::mov(arg32(0), res32(0)); return;
      }
      return;
    case MK::Idls_get: I::mov(domain_field(DomainField::dls_root), res(0)); return;
    case MK::Ireturn_addr: {
      long offset = frame_size(env) - 8;
      I::mov(mem64(DataType::QWORD, offset, R64::RSP), res(0));
      return;
    }
    default: fatal("Emit.emit_instr");
  }
}

}  // namespace

// Emission of a function declaration
void fundecl(const linear::Fundecl& f) {
  Env env{&f};
  emit_named_text_section(f.fun_name);
  D::align(16);
  add_def_symbol(f.fun_name);
  D::global(emit_symbol(f.fun_name));
  D::label(emit_symbol(f.fun_name));
  emit_debug_info(f.fun_dbg);
  D::cfi_startproc();
  if (clflags::runtime_variant == "d") emit_call("caml_assert_stack_invariants");
  long max_frame_size = frame_size(env) + f.fun_extra_stack_used;
  std::optional<std::pair<long, long>> handle_overflow;
  if (f.fun_contains_nontail_calls || max_frame_size >= stack_threshold_size) {
    long overflow = cmm::new_label();
    long ret = cmm::new_label();
    long threshold_offset = stack_ctx_words * 8 + stack_threshold_size;
    I::lea(mem64(DataType::NONE, -(max_frame_size + threshold_offset), R64::RSP), r10);
    I::cmp(domain_field(DomainField::current_stack), r10);
    I::j(Cond::B, label(overflow));
    def_label(ret);
    handle_overflow = std::pair{overflow, ret};
  }
  // emit_all env true fundecl.fun_body
  bool fallthrough = true;
  for (linear::Instr i = f.fun_body; i->desc != linear::Instruction::K::Lend; i = i->next) {
    emit_instr(env, fallthrough, i);
    fallthrough = linear::has_fallthrough(*i);
  }
  for (const GcCall& gc : env.call_gc_sites) emit_call_gc(gc);
  emit_call_bound_errors(env);
  if (handle_overflow) {
    auto [overflow, ret] = *handle_overflow;
    def_label(overflow);
    // Pass the desired frame size on the stack, since all of the
    // argument-passing registers may be in use.  Also serves to align the
    // stack properly before the call
    I::push(imm(32 + max_frame_size / 8));
    D::cfi_adjust_cfa_offset(8);
    // measured in words
    emit_call("caml_call_realloc_stack");
    I::pop(r10);  // ignored
    D::cfi_adjust_cfa_offset(-8);
    I::jmp(label(ret));
  }
  if (f.fun_frame_required) {
    long n = frame_size(env) - 8;
    if (n != 0) D::cfi_adjust_cfa_offset(-n);
  }
  D::cfi_endproc();
  D::type_(emit_symbol(f.fun_name), "@function");
  D::size(emit_symbol(f.fun_name), cbin(Constant::K::ConstSub, cthis(), clabel(emit_symbol(f.fun_name))));
}

// Emission of data
void data(const std::vector<cmm::DataItem>& l) {
  using DK = cmm::DataItem::K;
  D::data();
  D::align(8);
  for (const cmm::DataItem& d : l) {
    switch (d.kind) {
      case DK::Cglobal_symbol: D::global(emit_symbol(d.s)); break;
      case DK::Cdefine_symbol:
        add_def_symbol(d.s);
        D::label(emit_symbol(d.s));
        break;
      case DK::Cint8: D::byte(cconst(d.n)); break;
      case DK::Cint16: D::word(cconst(d.n)); break;
      case DK::Cint32:
      case DK::Cint: (d.kind == DK::Cint32 ? D::long_ : D::qword)(cconst(d.n)); break;
      case DK::Csingle: {
        float f = static_cast<float>(d.f);
        std::int32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        D::long_(cconst(bits));
        break;
      }
      case DK::Cdouble: {
        std::int64_t bits;
        std::memcpy(&bits, &d.f, sizeof bits);
        D::qword(cconst(bits));
        break;
      }
      case DK::Csymbol_address:
        add_used_symbol(d.s);
        D::qword(clabel(emit_symbol(d.s)));
        break;
      case DK::Cstring: D::bytes(d.s); break;
      case DK::Cskip:
        if (d.n > 0) D::space(d.n);
        break;
      case DK::Calign: D::align(d.n); break;
    }
  }
}

// Beginning / end of an assembly file
void begin_assembly() {
  out.clear();
  reset_debug_info();  // PR#5603
  float_constants.clear();
  if (clflags::dlcode) {
    // from amd64.S; could emit these constants on demand
    D::section({".rodata.cst16"}, std::string("aM"), {"@progbits", "16"});
    D::align(16);
    D::label(emit_symbol("caml_negf_mask"));
    D::qword(cconst(static_cast<std::int64_t>(0x8000000000000000ULL)));
    D::qword(cconst(0));
    D::align(16);
    D::label(emit_symbol("caml_absf_mask"));
    D::qword(cconst(0x7FFFFFFFFFFFFFFFLL));
    D::qword(cconst(-1));
  }
  D::data();
  emit_global_label("data_begin");
  emit_named_text_section(compilenv::make_symbol(std::string_view("code_begin")));
  emit_global_label("code_begin");
}

std::string end_assembly() {
  if (!float_constants.empty()) {
    D::section({".rodata.cst8"}, std::string("aM"), {"@progbits", "8"});
    D::align(8);
    for (auto& [c, lbl] : float_constants) {
      D::label(emit_label(lbl));
      D::qword(cconst(c));
    }
  }
  emit_named_text_section(compilenv::make_symbol(std::string_view("code_end")));
  emit_global_label("code_end");
  // emit_imp_table (empty on Linux)
  D::data();
  D::comment("relocation table start");
  D::align(8);
  D::comment("relocation table end");
  D::data();
  D::qword(cconst(0));  // PR#6329
  emit_global_label("data_end");
  D::qword(cconst(0));
  D::align(8);  // PR#7591
  emit_global_label("frametable");
  emit_frames();
  std::string frametable = emit_symbol(compilenv::make_symbol(std::string_view("frametable")));
  D::type_(frametable, "@object");
  D::size(frametable, cbin(Constant::K::ConstSub, cthis(), clabel(frametable)));
  // Mark stack as non-executable, PR#4564
  D::section({".note.GNU-stack"}, std::string(""), {"%progbits"});
  std::string r = "\t.file \"\"\n" + out;  // PR#7037
  out.clear();
  symbols_used.clear();
  symbols_defined.clear();
  return r;
}

}  // namespace cppcaml::typing::emit
