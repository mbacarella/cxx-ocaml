// Port of bytecomp/emitcode.ml (cxx/PORTING.md stage 10): the instruction
// list to relocatable bytecode (with the `emit` peephole), and the .cmo:
// magic, code, the debugging events (-g) and the
// marshaled Cmo_format.compilation_unit.
//
// Marshal preserves physical sharing, so the values are built with the
// identities OCaml's have (see the Values class): a string is one value per
// storage (an ident's name, a primitive's name, a constant's string), a
// structured constant's boxed integer one per constant, blocks otherwise
// fresh.
//
// Deviations: Clflags.bytecode_compatible_32 (-compat-32) and to_memory
// (the toplevel's) are not ported.
#include "cppcaml/flat_map.hpp"
#include "cppcaml/typing/emitcode.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/location.hpp"

#include <optional>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "cppcaml/omarshal.hpp"
#include "cppcaml/typing/bytegen.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/cmi_format.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/translmod.hpp"

namespace cppcaml::typing::emitcode {
namespace {

namespace o = cppcaml::omarshal;
using V = o::ValPtr;
using namespace instruct;
using L = lambda::StructuredConstant;
using lambda::IntegerComparison;


[[noreturn]] void fatal_error(const char* msg) { throw std::logic_error(msg); }

// ---- opcodes (runtime/caml/opcodes.h, sequential from 0) -----------------------
enum Opcode : int {
  opACC0, opACC1, opACC2, opACC3, opACC4, opACC5, opACC6, opACC7, opACC, opPUSH, opPUSHACC0,
  opPUSHACC1, opPUSHACC2, opPUSHACC3, opPUSHACC4, opPUSHACC5, opPUSHACC6, opPUSHACC7, opPUSHACC,
  opPOP, opASSIGN, opENVACC1, opENVACC2, opENVACC3, opENVACC4, opENVACC, opPUSHENVACC1,
  opPUSHENVACC2, opPUSHENVACC3, opPUSHENVACC4, opPUSHENVACC, opPUSH_RETADDR, opAPPLY, opAPPLY1,
  opAPPLY2, opAPPLY3, opAPPTERM, opAPPTERM1, opAPPTERM2, opAPPTERM3, opRETURN, opRESTART, opGRAB,
  opCLOSURE, opCLOSUREREC, opOFFSETCLOSUREM3, opOFFSETCLOSURE0, opOFFSETCLOSURE3,
  opOFFSETCLOSURE, opPUSHOFFSETCLOSUREM3, opPUSHOFFSETCLOSURE0, opPUSHOFFSETCLOSURE3,
  opPUSHOFFSETCLOSURE, opGETGLOBAL, opPUSHGETGLOBAL, opGETGLOBALFIELD, opPUSHGETGLOBALFIELD,
  opSETGLOBAL, opATOM0, opATOM, opPUSHATOM0, opPUSHATOM, opMAKEBLOCK, opMAKEBLOCK1, opMAKEBLOCK2,
  opMAKEBLOCK3, opMAKEFLOATBLOCK, opGETFIELD0, opGETFIELD1, opGETFIELD2, opGETFIELD3, opGETFIELD,
  opGETFLOATFIELD, opSETFIELD0, opSETFIELD1, opSETFIELD2, opSETFIELD3, opSETFIELD,
  opSETFLOATFIELD, opVECTLENGTH, opGETVECTITEM, opSETVECTITEM, opGETBYTESCHAR, opSETBYTESCHAR,
  opBRANCH, opBRANCHIF, opBRANCHIFNOT, opSWITCH, opBOOLNOT, opPUSHTRAP, opPOPTRAP, opRAISE,
  opCHECK_SIGNALS, opC_CALL1, opC_CALL2, opC_CALL3, opC_CALL4, opC_CALL5, opC_CALLN, opCONST0,
  opCONST1, opCONST2, opCONST3, opCONSTINT, opPUSHCONST0, opPUSHCONST1, opPUSHCONST2,
  opPUSHCONST3, opPUSHCONSTINT, opNEGINT, opADDINT, opSUBINT, opMULINT, opDIVINT, opMODINT,
  opANDINT, opORINT, opXORINT, opLSLINT, opLSRINT, opASRINT, opEQ, opNEQ, opLTINT, opLEINT,
  opGTINT, opGEINT, opOFFSETINT, opOFFSETREF, opISINT, opGETMETHOD, opBEQ, opBNEQ, opBLTINT,
  opBLEINT, opBGTINT, opBGEINT, opULTINT, opUGEINT, opBULTINT, opBUGEINT, opGETPUBMET,
  opGETDYNMET, opSTOP, opEVENT, opBREAK, opRERAISE, opRAISE_NOTRACE, opGETSTRINGCHAR, opPERFORM,
  opRESUME, opRESUMETERM, opREPERFORMTERM,
};

// ---- Filename (Unix) / Location.absolute_path ------------------------------------

bool is_dir_sep(const std::string& s, long i) { return s[i] == '/'; }
bool is_relative(const std::string& n) { return n.empty() || n[0] != '/'; }

std::string generic_dirname(const std::string& name) {
  if (name.empty()) return ".";
  long n = static_cast<long>(name.size()) - 1;
  // trailing_sep
  while (n >= 0 && is_dir_sep(name, n)) --n;
  if (n < 0) return name.substr(0, 1);
  // base
  while (n >= 0 && !is_dir_sep(name, n)) --n;
  if (n < 0) return ".";
  // intermediate_sep
  while (n >= 0 && is_dir_sep(name, n)) --n;
  if (n < 0) return name.substr(0, 1);
  return name.substr(0, n + 1);
}

std::string generic_basename(const std::string& name) {
  long n = static_cast<long>(name.size()) - 1;
  // find_end
  while (true) {
    if (n < 0) return name.substr(0, 1);  // String.sub name 0 1 (name = "" handled below)
    if (is_dir_sep(name, n)) { --n; continue; }
    break;
  }
  long p = n + 1;
  // find_beg
  while (n >= 0 && !is_dir_sep(name, n)) --n;
  return name.substr(n + 1, p - n - 1);
}
std::string basename(const std::string& name) {
  if (name.empty()) return ".";  // generic_basename: name = "" -> current_dir_name
  return generic_basename(name);
}

std::string concat(const std::string& dirname, const std::string& filename) {
  long l = static_cast<long>(dirname.size());
  if (l == 0 || is_dir_sep(dirname, l - 1)) return dirname + filename;
  return dirname + "/" + filename;
}

// Sys.getcwd: read once (c++ocamlc never changes its directory)
const std::string& getcwd_() {
  static const std::string cwd = [] {
    std::vector<char> buf(4096);
    while (!::getcwd(buf.data(), buf.size())) buf.resize(buf.size() * 2);
    return std::string(buf.data());
  }();
  return cwd;
}

std::string absolute_path(const std::string& s0) {
  std::string s = is_relative(s0) ? concat(getcwd_(), s0) : s0;
  s = location::rewrite_absolute_path(s);
  std::function<std::string(const std::string&)> aux = [&](const std::string& s) -> std::string {
    std::string base = basename(s);
    std::string dir = generic_dirname(s);
    if (dir == s) return dir;
    if (base == ".") return aux(dir);
    if (base == "..") return generic_dirname(aux(dir));
    return concat(aux(dir), base);
  };
  return aux(s);
}

// ---- values with OCaml's identities ------------------------------------------------

class Values {
 public:
  V i(long n) { return o::vint(n); }
  V b(bool x) { return o::vint(x ? 1 : 0); }
  // one string value per storage (as cmi_format.cpp's Writer)
  V str(std::string_view sv) {
    if (!sv.data()) return o::vstr(std::string(sv));
    auto [it, fresh] = strs_.try_emplace(std::make_pair(sv.data(), sv.size()), nullptr);
    if (fresh) it->second = o::vstr(std::string(sv));
    return it->second;
  }
  V fresh_str(std::string s) { return o::vstr(std::move(s)); }
  V some(V x) { return o::vblock(0, {std::move(x)}); }
  V none() { return o::vint(0); }

  // Symtable.transl_const
  V transl_const(const L* sc) {
    switch (sc->kind) {
      case L::Kind::Const_int: return i(sc->i);
      case L::Kind::Const_char: return i(sc->i);
      case L::Kind::Const_float: return o::vdbl(float_of_string(sc->s));
      case L::Kind::Const_int32:
      case L::Kind::Const_int64:
      case L::Kind::Const_nativeint: {
        // the boxed integer the constant holds: the typed constant's box, else one per constant
        const void* key = sc->box ? sc->box : sc;
        auto it = boxed_.find(key);
        if (it != boxed_.end()) return it->second;
        V v = boxed(sc->kind, sc->boxed);
        boxed_[key] = v;
        return v;
      }
      case L::Kind::Const_immstring: return str(sc->s);
      case L::Kind::Const_block: {  // Obj.new_block: fresh
        std::vector<V> fs;
        for (const L* f : sc->fields) fs.push_back(transl_const(f));
        return o::vblock(static_cast<int>(sc->i), std::move(fs));
      }
      case L::Kind::Const_float_array: {
        std::vector<double> ds;
        for (std::string_view f : sc->floats) ds.push_back(float_of_string(f));
        if (ds.empty()) return o::vblock(0, {});  // Floatarray.create 0: the atom [||]
        return o::vdblarr(std::move(ds));
      }
    }
    fatal_error("Emitcode.transl_const");
  }

 private:
  // float_of_string on an OCaml float literal ('_' separators, hex floats)
  static double float_of_string(std::string_view s0) {
    std::string s;
    for (char ch : s0)
      if (ch != '_') s += ch;
    const char* p = s.c_str();
    char* end = nullptr;
    double v = std::strtod(p, &end);
    return end == p ? 0.0 : v;
  }
  // a boxed integer as extern.c writes it: CODE_CUSTOM_FIXED, the
  // identifier, the serialized data (runtime/ints.c)
  static V boxed(L::Kind k, std::int64_t n) {
    std::string raw;
    auto be32 = [&](std::uint32_t x) {
      for (int s = 3; s >= 0; --s) raw.push_back(static_cast<char>((x >> (8 * s)) & 0xff));
    };
    auto be64 = [&](std::uint64_t x) {
      for (int s = 7; s >= 0; --s) raw.push_back(static_cast<char>((x >> (8 * s)) & 0xff));
    };
    raw.push_back(static_cast<char>(0x19));  // CODE_CUSTOM_FIXED
    if (k == L::Kind::Const_int32) {
      raw += "_i";
      raw.push_back('\0');
      be32(static_cast<std::uint32_t>(n));
      return o::vcustom2(raw, 4, 4);
    }
    if (k == L::Kind::Const_int64) {
      raw += "_j";
      raw.push_back('\0');
      be64(static_cast<std::uint64_t>(n));
      return o::vcustom2(raw, 8, 8);
    }
    raw += "_n";
    raw.push_back('\0');
    if (n >= INT32_MIN && n <= INT32_MAX) {
      raw.push_back(1);
      be32(static_cast<std::uint32_t>(n));
    } else {
      raw.push_back(2);
      be64(static_cast<std::uint64_t>(n));
    }
    return o::vcustom2(raw, 4, 8);
  }

  FlatMap<std::pair<const char*, std::size_t>, V> strs_;
  FlatMap<const void*, V> boxed_;
  FlatMap<const void*, V> objs_;
  V boxedint_kinds_[3];
};

// ---- the emitter ---------------------------------------------------------------------

struct LabelDef {  // Label_defined of int | Label_undefined of (int * int) list
  bool defined = false;
  long def = 0;
  std::vector<std::pair<long, long>> patchlist;  // newest last (OCaml's is newest first)
};

enum class RelocK { Reloc_literal, Reloc_getcompunit, Reloc_getpredef, Reloc_setcompunit, Reloc_primitive };
struct Reloc {
  RelocK k;
  const L* lit = nullptr;     // Reloc_literal
  std::string_view name;      // the others
  long pos;
};

class Emitter {
 public:
  std::vector<unsigned char> out_buffer;
  long out_position = 0;
  std::vector<LabelDef> label_table;
  std::vector<Reloc> reloc_info;             // in emission order (List.rev !reloc_info)
  std::vector<DebugEvent*> events;           // in emission order (!events reversed)
  std::set<std::string> debug_dirs;

  Emitter() {
    // init ()
    label_table.resize(16);
    out_buffer.reserve(1024);
  }

  void out_word(long b1, long b2, long b3, long b4) {
    out_buffer.push_back(static_cast<unsigned char>(b1));
    out_buffer.push_back(static_cast<unsigned char>(b2));
    out_buffer.push_back(static_cast<unsigned char>(b3));
    out_buffer.push_back(static_cast<unsigned char>(b4));
    out_position += 4;
  }
  void out(int opcode) { out_word(opcode, 0, 0, 0); }
  void out_int(long n) { out_word(n, n >> 8, n >> 16, n >> 24); }

  static bool const_as_int(const L* c, long& n) {
    if (c->kind == L::Kind::Const_int || c->kind == L::Kind::Const_char) {
      n = c->i;
      return true;
    }
    return false;
  }
  static bool is_immed(long i) { return immed_min <= i && i <= immed_max; }
  static bool is_immed_const(const L* k) {
    long n;
    return const_as_int(k, n) && is_immed(n);
  }
  void out_const(const L* c) {
    long n;
    if (!const_as_int(c, n)) fatal_error("Emitcode.const_as_int");
    out_int(n);
  }

  // ---- labels and backpatching ----
  void extend_label_table(long needed) {
    std::size_t new_size = std::max<std::size_t>(label_table.size(), 16);
    while (static_cast<std::size_t>(needed) >= new_size) new_size *= 2;
    label_table.resize(new_size);
  }
  void backpatch(long pos, long orig) {
    long displ = (out_position - orig) >> 2;
    out_buffer[pos] = static_cast<unsigned char>(displ);
    out_buffer[pos + 1] = static_cast<unsigned char>(displ >> 8);
    out_buffer[pos + 2] = static_cast<unsigned char>(displ >> 16);
    out_buffer[pos + 3] = static_cast<unsigned char>(displ >> 24);
  }
  void define_label(long lbl) {
    if (static_cast<std::size_t>(lbl) >= label_table.size()) extend_label_table(lbl);
    LabelDef& d = label_table[lbl];
    if (d.defined) fatal_error("Emitcode.define_label");
    for (auto it = d.patchlist.rbegin(); it != d.patchlist.rend(); ++it) backpatch(it->first, it->second);
    d.patchlist.clear();
    d.defined = true;
    d.def = out_position;
  }
  void out_label_with_orig(long orig, long lbl) {
    if (static_cast<std::size_t>(lbl) >= label_table.size()) extend_label_table(lbl);
    LabelDef& d = label_table[lbl];
    if (d.defined) {
      out_int((d.def - orig) >> 2);
    } else {
      d.patchlist.emplace_back(out_position, orig);
      out_int(0);
    }
  }
  void out_label(long l) { out_label_with_orig(out_position, l); }

  // ---- relocation information ----
  void enter(Reloc r) {
    r.pos = out_position;
    reloc_info.push_back(r);
  }
  void slot_for_literal(const L* sc) {
    enter({RelocK::Reloc_literal, sc, {}, 0});
    out_int(0);
  }
  void slot_for_getglobal(Ident::t id) {
    std::string_view name = ident::name(id);
    if (ident::is_predef(id))
      enter({RelocK::Reloc_getpredef, nullptr, name, 0});
    else if (ident::global(id))
      enter({RelocK::Reloc_getcompunit, nullptr, name, 0});
    else
      fatal_error("Emitcode.slot_for_getglobal");
    out_int(0);
  }
  void slot_for_setglobal(Ident::t id) {
    std::string_view name = ident::name(id);
    if (!ident::persistent(id)) fatal_error("Emitcode.slot_for_setglobal");
    enter({RelocK::Reloc_setcompunit, nullptr, name, 0});
    out_int(0);
  }
  void slot_for_c_prim(std::string_view name) {
    enter({RelocK::Reloc_primitive, nullptr, name, 0});
    out_int(0);
  }

  // ---- debugging events ----
  void record_event(DebugEvent* ev) {
    // the directories are pure functions of the event's file name (and the
    // fixed cwd): computed once per name -- a unit's events share one
    std::string_view fname = ev->ev_loc.loc_start.pos_fname;
    if (!last_dirs_ || fname != last_fname_) {
      std::string path(fname);
      last_fname_ = path;
      last_dir_ = generic_dirname(absolute_path(path));
      last_cwd_ = is_relative(path) ? std::optional<std::string>(location::rewrite_absolute_path(getcwd_()))
                                    : std::nullopt;
      last_dirs_ = true;
    }
    debug_dirs.insert(last_dir_);
    if (last_cwd_) debug_dirs.insert(*last_cwd_);
    ev->ev_pos = out_position;
    events.push_back(ev);
  }
  bool last_dirs_ = false;
  std::string last_fname_, last_dir_;
  std::optional<std::string> last_cwd_;

  // ---- one instruction ----
  void emit_comp(IntegerComparison c) {
    switch (c) {
      case IntegerComparison::Ceq: out(opEQ); break;
      case IntegerComparison::Cne: out(opNEQ); break;
      case IntegerComparison::Clt: out(opLTINT); break;
      case IntegerComparison::Cle: out(opLEINT); break;
      case IntegerComparison::Cgt: out(opGTINT); break;
      case IntegerComparison::Cge: out(opGEINT); break;
    }
  }
  void emit_branch_comp(IntegerComparison c) {
    switch (c) {
      case IntegerComparison::Ceq: out(opBEQ); break;
      case IntegerComparison::Cne: out(opBNEQ); break;
      case IntegerComparison::Clt: out(opBLTINT); break;
      case IntegerComparison::Cle: out(opBLEINT); break;
      case IntegerComparison::Cgt: out(opBGTINT); break;
      case IntegerComparison::Cge: out(opBGEINT); break;
    }
  }

  void emit_instr(const Instruction& in) {
    long n = in.n;
    switch (in.k) {
      case IK::Klabel: define_label(n); break;
      case IK::Kacc:
        if (n < 8) out(opACC0 + static_cast<int>(n));
        else { out(opACC); out_int(n); }
        break;
      case IK::Kenvacc:
        if (n >= 1 && n <= 4) out(opENVACC1 + static_cast<int>(n) - 1);
        else { out(opENVACC); out_int(n); }
        break;
      case IK::Kpush: out(opPUSH); break;
      case IK::Kpop: out(opPOP); out_int(n); break;
      case IK::Kassign: out(opASSIGN); out_int(n); break;
      case IK::Kpush_retaddr: out(opPUSH_RETADDR); out_label(n); break;
      case IK::Kapply:
        if (n < 4) out(opAPPLY1 + static_cast<int>(n) - 1);
        else { out(opAPPLY); out_int(n); }
        break;
      case IK::Kappterm:
        if (n < 4) { out(opAPPTERM1 + static_cast<int>(n) - 1); out_int(in.m); }
        else { out(opAPPTERM); out_int(n); out_int(in.m); }
        break;
      case IK::Kreturn: out(opRETURN); out_int(n); break;
      case IK::Krestart: out(opRESTART); break;
      case IK::Kgrab: out(opGRAB); out_int(n); break;
      case IK::Kclosure: out(opCLOSURE); out_int(in.m); out_label(n); break;
      case IK::Kclosurerec: {
        out(opCLOSUREREC); out_int(static_cast<long>(in.lbls.size())); out_int(n);
        long org = out_position;
        for (label l : in.lbls) out_label_with_orig(org, l);
        break;
      }
      case IK::Koffsetclosure:
        if (n == -3 || n == 0 || n == 3) out(opOFFSETCLOSURE0 + static_cast<int>(n) / 3);
        else { out(opOFFSETCLOSURE); out_int(n); }
        break;
      case IK::Kgetglobal: out(opGETGLOBAL); slot_for_getglobal(in.id); break;
      case IK::Ksetglobal: out(opSETGLOBAL); slot_for_setglobal(in.id); break;
      case IK::Kconst: {
        const L* sc = in.cst;
        if (sc->kind == L::Kind::Const_int && is_immed(sc->i)) {
          if (sc->i >= 0 && sc->i <= 3) out(opCONST0 + static_cast<int>(sc->i));
          else { out(opCONSTINT); out_int(sc->i); }
        } else if (sc->kind == L::Kind::Const_char) {
          out(opCONSTINT); out_int(sc->i);
        } else if (sc->kind == L::Kind::Const_block && sc->fields.empty()) {
          if (sc->i == 0) out(opATOM0);
          else { out(opATOM); out_int(sc->i); }
        } else {
          out(opGETGLOBAL); slot_for_literal(sc);
        }
        break;
      }
      case IK::Kmakeblock:
        if (n == 0) {
          if (in.m == 0) out(opATOM0);
          else { out(opATOM); out_int(in.m); }
        } else if (n < 4) {
          out(opMAKEBLOCK1 + static_cast<int>(n) - 1); out_int(in.m);
        } else {
          out(opMAKEBLOCK); out_int(n); out_int(in.m);
        }
        break;
      case IK::Kgetfield:
        if (n < 4) out(opGETFIELD0 + static_cast<int>(n));
        else { out(opGETFIELD); out_int(n); }
        break;
      case IK::Ksetfield:
        if (n < 4) out(opSETFIELD0 + static_cast<int>(n));
        else { out(opSETFIELD); out_int(n); }
        break;
      case IK::Kmakefloatblock:
        if (n == 0) out(opATOM0);
        else { out(opMAKEFLOATBLOCK); out_int(n); }
        break;
      case IK::Kgetfloatfield: out(opGETFLOATFIELD); out_int(n); break;
      case IK::Ksetfloatfield: out(opSETFLOATFIELD); out_int(n); break;
      case IK::Kvectlength: out(opVECTLENGTH); break;
      case IK::Kgetvectitem: out(opGETVECTITEM); break;
      case IK::Ksetvectitem: out(opSETVECTITEM); break;
      case IK::Kgetstringchar: out(opGETSTRINGCHAR); break;
      case IK::Kgetbyteschar: out(opGETBYTESCHAR); break;
      case IK::Ksetbyteschar: out(opSETBYTESCHAR); break;
      case IK::Kbranch: out(opBRANCH); out_label(n); break;
      case IK::Kbranchif: out(opBRANCHIF); out_label(n); break;
      case IK::Kbranchifnot: out(opBRANCHIFNOT); out_label(n); break;
      case IK::Kstrictbranchif: out(opBRANCHIF); out_label(n); break;
      case IK::Kstrictbranchifnot: out(opBRANCHIFNOT); out_label(n); break;
      case IK::Kswitch: {
        out(opSWITCH);
        out_int(static_cast<long>(in.sw_consts.size()) + (static_cast<long>(in.sw_blocks.size()) << 16));
        long org = out_position;
        for (label l : in.sw_consts) out_label_with_orig(org, l);
        for (label l : in.sw_blocks) out_label_with_orig(org, l);
        break;
      }
      case IK::Kboolnot: out(opBOOLNOT); break;
      case IK::Kpushtrap: out(opPUSHTRAP); out_label(n); break;
      case IK::Kpoptrap: out(opPOPTRAP); break;
      case IK::Kraise:
        switch (in.raise) {
          case lambda::RaiseKind::Raise_regular: out(opRAISE); break;
          case lambda::RaiseKind::Raise_reraise: out(opRERAISE); break;
          case lambda::RaiseKind::Raise_notrace: out(opRAISE_NOTRACE); break;
        }
        break;
      case IK::Kcheck_signals: out(opCHECK_SIGNALS); break;
      case IK::Kccall:
        if (n <= 5) { out(opC_CALL1 + static_cast<int>(n) - 1); slot_for_c_prim(in.prim); }
        else { out(opC_CALLN); out_int(n); slot_for_c_prim(in.prim); }
        break;
      case IK::Knegint: out(opNEGINT); break;
      case IK::Kaddint: out(opADDINT); break;
      case IK::Ksubint: out(opSUBINT); break;
      case IK::Kmulint: out(opMULINT); break;
      case IK::Kdivint: out(opDIVINT); break;
      case IK::Kmodint: out(opMODINT); break;
      case IK::Kandint: out(opANDINT); break;
      case IK::Korint: out(opORINT); break;
      case IK::Kxorint: out(opXORINT); break;
      case IK::Klslint: out(opLSLINT); break;
      case IK::Klsrint: out(opLSRINT); break;
      case IK::Kasrint: out(opASRINT); break;
      case IK::Kintcomp: emit_comp(in.icmp); break;
      case IK::Koffsetint: out(opOFFSETINT); out_int(n); break;
      case IK::Koffsetref: out(opOFFSETREF); out_int(n); break;
      case IK::Kisint: out(opISINT); break;
      case IK::Kisout: out(opULTINT); break;
      case IK::Kgetmethod: out(opGETMETHOD); break;
      case IK::Kgetpubmet: out(opGETPUBMET); out_int(n); out_int(0); break;
      case IK::Kgetdynmet: out(opGETDYNMET); break;
      case IK::Kevent: record_event(in.event); break;
      case IK::Kperform: out(opPERFORM); break;
      case IK::Kresume: out(opRESUME); break;
      case IK::Kresumeterm: out(opRESUMETERM); out_int(n); break;
      case IK::Kreperformterm: out(opREPERFORMTERM); out_int(n); break;
      case IK::Kstop: out(opSTOP); break;
    }
  }

  // ---- a list of instructions, with the peephole optimizations ----
  static code remerge_events(DebugEvent* ev1, code c) {
    if (c && c->hd.k == IK::Kevent) {
      Instruction e = instr(IK::Kevent);
      e.event = bytegen::merge_events(ev1, c->hd.event);
      return cons(e, c->tl);
    }
    Instruction e = instr(IK::Kevent);
    e.event = ev1;
    return cons(e, c);
  }

  void emit(code c) {
    auto nth = [](code x, int k) -> const Instruction* {
      while (k-- > 0 && x) x = x->tl;
      return x ? &x->hd : nullptr;
    };
    auto drop = [](code x, int k) {
      while (k-- > 0) x = x->tl;
      return x;
    };
    while (c) {
      const Instruction& i0 = c->hd;
      const Instruction* i1 = nth(c, 1);
      const Instruction* i2 = nth(c, 2);
      const Instruction* i3 = nth(c, 3);
      if (i0.k == IK::Kpush && i1) {
        // optimization of integer tests
        if (i1->k == IK::Kconst && is_immed_const(i1->cst) && i2 && i3 &&
            (i3->k == IK::Kbranchif || i3->k == IK::Kbranchifnot)) {
          bool ifnot = i3->k == IK::Kbranchifnot;
          if (i2->k == IK::Kintcomp) {
            IntegerComparison cmp = i2->icmp;
            emit_branch_comp(ifnot ? lambda::negate_integer_comparison(cmp) : cmp);
            out_const(i1->cst);
            out_label(i3->n);
            c = drop(c, 4);
            continue;
          }
          // same for range tests
          if (i2->k == IK::Kisout) {
            out(ifnot ? opBUGEINT : opBULTINT);
            out_const(i1->cst);
            out_label(i3->n);
            c = drop(c, 4);
            continue;
          }
        }
        // Some special case of push ; i ; ret generated by the match compiler
        if (i1->k == IK::Kacc && i1->n == 0 && i2 && i2->k == IK::Kreturn) {
          c = cons(instr(IK::Kreturn, i2->n - 1), drop(c, 3));
          continue;
        }
        // General push then access scheme
        if (i1->k == IK::Kacc) {
          if (i1->n < 8) out(opPUSHACC0 + static_cast<int>(i1->n));
          else { out(opPUSHACC); out_int(i1->n); }
          c = drop(c, 2);
          continue;
        }
        if (i1->k == IK::Kenvacc) {
          if (i1->n >= 1 && i1->n < 4) out(opPUSHENVACC1 + static_cast<int>(i1->n) - 1);
          else { out(opPUSHENVACC); out_int(i1->n); }
          c = drop(c, 2);
          continue;
        }
        if (i1->k == IK::Koffsetclosure) {
          long ofs = i1->n;
          if (ofs == -3 || ofs == 0 || ofs == 3) out(opPUSHOFFSETCLOSURE0 + static_cast<int>(ofs) / 3);
          else { out(opPUSHOFFSETCLOSURE); out_int(ofs); }
          c = drop(c, 2);
          continue;
        }
        if (i1->k == IK::Kgetglobal && i2 && i2->k == IK::Kgetfield) {
          out(opPUSHGETGLOBALFIELD); slot_for_getglobal(i1->id); out_int(i2->n);
          c = drop(c, 3);
          continue;
        }
        if (i1->k == IK::Kgetglobal) {
          out(opPUSHGETGLOBAL); slot_for_getglobal(i1->id);
          c = drop(c, 2);
          continue;
        }
        if (i1->k == IK::Kconst) {
          const L* sc = i1->cst;
          if (sc->kind == L::Kind::Const_int && is_immed(sc->i)) {
            if (sc->i >= 0 && sc->i <= 3) out(opPUSHCONST0 + static_cast<int>(sc->i));
            else { out(opPUSHCONSTINT); out_int(sc->i); }
          } else if (sc->kind == L::Kind::Const_char) {
            out(opPUSHCONSTINT); out_int(sc->i);
          } else if (sc->kind == L::Kind::Const_block && sc->fields.empty()) {
            if (sc->i == 0) out(opPUSHATOM0);
            else { out(opPUSHATOM); out_int(sc->i); }
          } else {
            out(opPUSHGETGLOBAL); slot_for_literal(sc);
          }
          c = drop(c, 2);
          continue;
        }
        if (i1->k == IK::Kevent && i1->event->ev_kind.k == DebugEventKindK::Event_before && i2) {
          DebugEvent* ev = i1->event;
          if (i2->k == IK::Kgetglobal && i3 && i3->k == IK::Kgetfield) {
            c = cons(i0, cons(*i2, cons(*i3, remerge_events(ev, drop(c, 4)))));
            continue;
          }
          if (i2->k == IK::Kacc || i2->k == IK::Kenvacc || i2->k == IK::Koffsetclosure ||
              i2->k == IK::Kgetglobal || i2->k == IK::Kconst) {
            c = cons(i0, cons(*i2, remerge_events(ev, drop(c, 3))));
            continue;
          }
        }
      }
      if (i0.k == IK::Kgetglobal && i1 && i1->k == IK::Kgetfield) {
        out(opGETGLOBALFIELD); slot_for_getglobal(i0.id); out_int(i1->n);
        c = drop(c, 2);
        continue;
      }
      // Default case
      emit_instr(i0);
      c = c->tl;
    }
  }
};

void output_binary_int(std::string& out, long n) {
  out.push_back(static_cast<char>((n >> 24) & 0xff));
  out.push_back(static_cast<char>((n >> 16) & 0xff));
  out.push_back(static_cast<char>((n >> 8) & 0xff));
  out.push_back(static_cast<char>(n & 0xff));
}
void output_value(std::string& out, const V& v, bool compressed = false) {
  std::vector<std::uint8_t> bytes = o::marshal(v, compressed);
  out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
// Compression.output_value
void compressed_output_value(std::string& out, const V& v) {
  output_value(out, v, config::compression_supported);
}

V reloc_info_value(Values& w, const Reloc& r) {
  switch (r.k) {
    case RelocK::Reloc_literal: return o::vblock(0, {w.transl_const(r.lit)});
    case RelocK::Reloc_getcompunit: return o::vblock(1, {w.str(r.name)});
    case RelocK::Reloc_getpredef: return o::vblock(2, {w.str(r.name)});
    case RelocK::Reloc_setcompunit: return o::vblock(3, {w.str(r.name)});
    case RelocK::Reloc_primitive: return o::vblock(4, {w.str(r.name)});
  }
  fatal_error("Emitcode.reloc_info");
}

}  // namespace

struct ValueContext::Impl {
  Values v;
};
ValueContext::ValueContext() : impl(std::make_unique<Impl>()) {}
ValueContext::~ValueContext() = default;
omarshal::ValPtr ValueContext::str(std::string_view s) { return impl->v.str(s); }

PackedFile to_packed_file(std::string& out, code c, ValueContext& w) {
  Emitter em;
  em.emit(c);
  out.append(reinterpret_cast<const char*>(em.out_buffer.data()), em.out_buffer.size());
  PackedFile r;
  r.size = em.out_position;
  for (const Reloc& x : em.reloc_info) r.relocs.emplace_back(reloc_info_value(w.impl->v, x), x.pos);
  r.events.assign(em.events.rbegin(), em.events.rend());
  r.debug_dirs = em.debug_dirs;
  return r;
}

void to_file(std::FILE* outchan, std::string_view filename, std::string_view modname,
             const lambda::IdentSet& required_globals, code c) {
  o::ArenaScope arena_scope;  // the values made here die with the output
  Emitter em;
  std::string buf(config::cmo_magic_number);
  long pos_depl = static_cast<long>(buf.size());
  output_binary_int(buf, 0);
  long pos_code = static_cast<long>(buf.size());
  em.emit(c);
  buf.append(reinterpret_cast<const char*>(em.out_buffer.data()), em.out_buffer.size());
  long pos_debug = 0, size_debug = 0;
  if (clflags::debug) {
    em.debug_dirs.insert(generic_dirname(absolute_path(std::string(filename))));
    long p = static_cast<long>(buf.size());
    std::vector<const DebugEvent*> evs(em.events.rbegin(), em.events.rend());  // !events
    std::vector<std::string> dirs(em.debug_dirs.begin(), em.debug_dirs.end());
    std::vector<std::uint8_t> ev_bytes = cmi_format::marshal_debug_events(evs);
    buf.append(reinterpret_cast<const char*>(ev_bytes.data()), ev_bytes.size());
    std::vector<V> dv;
    for (std::string& d : dirs) dv.push_back(o::vstr(std::move(d)));
    compressed_output_value(buf, o::vlist(dv));
    pos_debug = p;
    size_debug = static_cast<long>(buf.size()) - p;
  }
  Values w;
  V cu_name = w.str(uid::unit_name_string(modname));  // Unit_info.modname: the one string
  std::vector<V> relocs;
  for (const Reloc& r : em.reloc_info) {
    V info;
    switch (r.k) {
      case RelocK::Reloc_literal: info = o::vblock(0, {w.transl_const(r.lit)}); break;
      case RelocK::Reloc_getcompunit: info = o::vblock(1, {w.str(r.name)}); break;
      case RelocK::Reloc_getpredef: info = o::vblock(2, {w.str(r.name)}); break;
      case RelocK::Reloc_setcompunit:
        // the unit's ident is Ident.create_persistent of Unit_info's modname:
        // its name is cu_name's string
        info = o::vblock(3, {r.name == modname ? cu_name : w.str(r.name)});
        break;
      case RelocK::Reloc_primitive: info = o::vblock(4, {w.str(r.name)}); break;
    }
    relocs.push_back(o::vblock(0, {info, w.i(r.pos)}));
  }
  std::vector<V> imports;
  for (auto& [name, crc] : env::imports()) {
    // Env.imports's strings: the first one Persistent_env.add_import saw for
    // each name (often a persistent ident's name, shared with a required
    // global); the unit's own name is the one Unit_info carries into cu_name
    // -- unless -cmi-file named the interface (Unit_info.Artifact.
    // from_filename makes a fresh modname, which Env.read_signature added)
    V n = name == modname && !clflags::cmi_file ? cu_name : w.str(env::import_name(name));
    imports.push_back(o::vblock(0, {n, crc ? w.some(w.fresh_str(*crc)) : w.none()}));
  }
  std::vector<V> prims;
  for (const PrimitiveDescription* p : translmod::primitive_declarations) prims.push_back(w.str(p->prim_name));
  std::vector<V> required;
  for (Ident::t id : required_globals) required.push_back(w.str(ident::name(id)));
  V compunit = o::vblock(0, {
      cu_name,                                                   // cu_name
      w.i(pos_code),                                             // cu_pos
      w.i(em.out_position),                                      // cu_codesize
      o::vlist(relocs),                                          // cu_reloc
      o::vlist(imports),                                         // cu_imports
      o::vlist(required),                                        // cu_required_compunits
      o::vlist(prims),                                           // cu_primitives
      w.b(clflags::link_everything),                             // cu_force_link
      w.i(pos_debug),                                            // cu_debug
      w.i(size_debug),                                           // cu_debugsize
  });
  long pos_compunit = static_cast<long>(buf.size());
  output_value(buf, compunit);
  std::string depl;
  output_binary_int(depl, pos_compunit);
  std::memcpy(buf.data() + pos_depl, depl.data(), 4);
  if (std::fwrite(buf.data(), 1, buf.size(), outchan) != buf.size())
    throw std::runtime_error("Emitcode.to_file: write error");
}

}  // namespace cppcaml::typing::emitcode
