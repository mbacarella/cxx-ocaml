// Emitcode + .cmo writer.  Port of the byte-encoding half of bytecomp/emitcode.ml
// (compact opcode selection, label backpatching, relocations, the `emit`
// peephole pass) plus a minimal OCaml Marshal *writer* for the
// Cmo_format.compilation_unit descriptor.
#include "cppcaml/cmo.hpp"
#include "cppcaml/omarshal.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <variant>
#include <vector>

namespace cppcaml::cmo {
namespace {
using bytecode::Code;
using bytecode::ICell;
using bytecode::Instr;
using bytecode::Op;
using lambda::Lam;
using lambda::LamPtr;

// ---- opcodes (runtime/caml/opcodes.h order: sequential from 0) ----
enum OP {
  ACC0, ACC1, ACC2, ACC3, ACC4, ACC5, ACC6, ACC7, ACC, PUSH,
  PUSHACC0, PUSHACC1, PUSHACC2, PUSHACC3, PUSHACC4, PUSHACC5, PUSHACC6, PUSHACC7,
  PUSHACC, POP, ASSIGN, ENVACC1, ENVACC2, ENVACC3, ENVACC4, ENVACC,
  PUSHENVACC1, PUSHENVACC2, PUSHENVACC3, PUSHENVACC4, PUSHENVACC, PUSH_RETADDR,
  APPLY, APPLY1, APPLY2, APPLY3, APPTERM, APPTERM1, APPTERM2, APPTERM3,
  RETURN, RESTART, GRAB, CLOSURE, CLOSUREREC, OFFSETCLOSUREM3, OFFSETCLOSURE0,
  OFFSETCLOSURE3, OFFSETCLOSURE, PUSHOFFSETCLOSUREM3, PUSHOFFSETCLOSURE0,
  PUSHOFFSETCLOSURE3, PUSHOFFSETCLOSURE, GETGLOBAL, PUSHGETGLOBAL,
  GETGLOBALFIELD, PUSHGETGLOBALFIELD, SETGLOBAL, ATOM0, ATOM, PUSHATOM0,
  PUSHATOM, MAKEBLOCK, MAKEBLOCK1, MAKEBLOCK2, MAKEBLOCK3, MAKEFLOATBLOCK,
  GETFIELD0, GETFIELD1, GETFIELD2, GETFIELD3, GETFIELD, GETFLOATFIELD,
  SETFIELD0, SETFIELD1, SETFIELD2, SETFIELD3, SETFIELD, SETFLOATFIELD,
  VECTLENGTH, GETVECTITEM, SETVECTITEM, GETBYTESCHAR, SETBYTESCHAR, BRANCH,
  BRANCHIF, BRANCHIFNOT, SWITCH, BOOLNOT, PUSHTRAP, POPTRAP, RAISE,
  CHECK_SIGNALS, C_CALL1, C_CALL2, C_CALL3, C_CALL4, C_CALL5, C_CALLN, CONST0,
  CONST1, CONST2, CONST3, CONSTINT, PUSHCONST0, PUSHCONST1, PUSHCONST2,
  PUSHCONST3, PUSHCONSTINT, NEGINT, ADDINT, SUBINT, MULINT, DIVINT, MODINT,
  ANDINT, ORINT, XORINT, LSLINT, LSRINT, ASRINT, EQ, NEQ, LTINT, LEINT, GTINT,
  GEINT, OFFSETINT, OFFSETREF, ISINT, GETMETHOD, BEQ, BNEQ, BLTINT, BLEINT,
  BGTINT, BGEINT, ULTINT, UGEINT, BULTINT, BUGEINT, GETPUBMET, GETDYNMET, STOP,
  EVENT, BREAK, RERAISE, RAISE_NOTRACE, GETSTRINGCHAR, PERFORM, RESUME,
  RESUMETERM, REPERFORMTERM
};

using omarshal::ValPtr;
using omarshal::vint;
using omarshal::vstr;
using omarshal::vblock;
using omarshal::vlist;

// Predefined exceptions (runtimedef.ml builtin_exceptions): a GETGLOBAL of one of
// these resolves to a fixed predef slot (Reloc_getpredef), not a compilation unit.
bool is_predef_exn(const std::string& n0) {
  std::string n = n0.substr(0, n0.find('/'));  // strip a "/stamp" suffix
  static const std::set<std::string> s = {
      "Out_of_memory", "Sys_error", "Failure", "Invalid_argument", "End_of_file",
      "Division_by_zero", "Not_found", "Match_failure", "Stack_overflow",
      "Sys_blocked_io", "Assert_failure", "Undefined_recursive_module"};
  return s.count(n) != 0;
}

// The structured constant carried by a Reloc_literal.
// OCaml float-literal semantics: '_' separators are allowed, degenerate hex
// forms ("0x.", "0xp0") mean zero, overflow is infinity -- strtod handles
// hexfloat and never throws (std::stod threw out_of_range on 0x-extremes).
double float_of_lit(const std::string& s0) {
  std::string s;
  for (char ch : s0) if (ch != '_') s += ch;
  const char* p = s.c_str();
  char* end = nullptr;
  double v = std::strtod(p, &end);
  return end == p ? 0.0 : v;
}

ValPtr const_value(const LamPtr& c) {
  switch (c->k) {
    case Lam::K::ConstInt: return vint(c->int_val);
    case Lam::K::ConstChar: return vint(c->int_val);
    case Lam::K::ConstString: return vstr(c->str_val);
    case Lam::K::ConstFloat: return omarshal::vdbl(float_of_lit(c->str_val));
    case Lam::K::ConstBlock: {
      std::vector<ValPtr> fs;
      for (auto& a : c->args) fs.push_back(const_value(a));
      return vblock(c->prim_arg, std::move(fs));
    }
    default: return vint(0);
  }
}

// ---- relocations ----
struct Reloc {
  enum K { Literal, GetCompunit, GetPredef, SetCompunit, Primitive } k;
  std::string name;
  ValPtr lit;
  int pos;
};

// ---- byte emitter (emitcode.ml: emit_instr + the emit peephole pass) ----
struct Emitter {
  std::vector<std::uint8_t> code;
  std::vector<Reloc> relocs;
  // label backpatching: label -> defined position, plus pending (operand-pos, orig-pos)
  std::map<int, int> defined;
  std::map<int, std::vector<std::pair<int, int>>> pending;

  int pos() const { return (int)code.size(); }
  void out_byte(int b) { code.push_back((std::uint8_t)b); }
  void out_word(int b1, int b2, int b3, int b4) { out_byte(b1); out_byte(b2); out_byte(b3); out_byte(b4); }
  void out(int op) { out_word(op, 0, 0, 0); }
  void out_int(long long n) { out_word((int)(n & 0xFF), (int)((n >> 8) & 0xFF), (int)((n >> 16) & 0xFF), (int)((n >> 24) & 0xFF)); }
  void patch_at(int at, int displ) {
    code[at] = displ & 0xFF; code[at + 1] = (displ >> 8) & 0xFF;
    code[at + 2] = (displ >> 16) & 0xFF; code[at + 3] = (displ >> 24) & 0xFF;
  }
  void define_label(int lbl) {
    defined[lbl] = pos();
    for (auto& [at, orig] : pending[lbl]) patch_at(at, (pos() - orig) >> 2);
    pending.erase(lbl);
  }
  void out_label_orig(int orig, int lbl) {
    auto it = defined.find(lbl);
    if (it != defined.end()) out_int((it->second - orig) >> 2);
    else { pending[lbl].push_back({pos(), orig}); out_int(0); }
  }
  void out_label(int lbl) { out_label_orig(pos(), lbl); }

  void slot_literal(const LamPtr& c) { relocs.push_back({Reloc::Literal, "", const_value(c), pos()}); out_int(0); }
  void slot_getglobal(const std::string& name0, bool predef) {
    // The -dinstr name carries a predef's "/stamp" suffix; the reloc wants the
    // bare identifier (compunit names never contain '/').
    std::string name = name0.substr(0, name0.find('/'));
    relocs.push_back({predef ? Reloc::GetPredef : Reloc::GetCompunit, name, nullptr, pos()});
    out_int(0);
  }
  void slot_setglobal(const std::string& name) { relocs.push_back({Reloc::SetCompunit, name, nullptr, pos()}); out_int(0); }
  void slot_cprim(const std::string& name) { relocs.push_back({Reloc::Primitive, name, nullptr, pos()}); out_int(0); }

  static bool is_immed(long long n) { return n >= -(1LL << 30) && n < (1LL << 30); }
  static bool is_immed_const(const LamPtr& c) {
    return (c->k == Lam::K::ConstInt && is_immed(c->int_val)) || c->k == Lam::K::ConstChar;
  }
  static long long const_as_int(const LamPtr& c) { return c->int_val; }

  // A single instruction (no peephole; emit() handles fusions first).
  void emit_instr(const Instr& in) {
    switch (in.op) {
      case Op::Label: define_label(in.a); break;
      case Op::Acc: if (in.a < 8) out(ACC0 + in.a); else { out(ACC); out_int(in.a); } break;
      case Op::Envacc:
        if (in.a >= 1 && in.a <= 4) out(ENVACC1 + in.a - 1); else { out(ENVACC); out_int(in.a); } break;
      case Op::Push: out(PUSH); break;
      case Op::Pop: out(POP); out_int(in.a); break;
      case Op::Assign: out(ASSIGN); out_int(in.a); break;
      case Op::PushRetaddr: out(PUSH_RETADDR); out_label(in.a); break;
      case Op::Apply: if (in.a < 4) out(APPLY1 + in.a - 1); else { out(APPLY); out_int(in.a); } break;
      case Op::Appterm:
        if (in.a < 4) { out(APPTERM1 + in.a - 1); out_int(in.b); }
        else { out(APPTERM); out_int(in.a); out_int(in.b); } break;
      case Op::Return: out(RETURN); out_int(in.a); break;
      case Op::Restart: out(RESTART); break;
      case Op::Grab: out(GRAB); out_int(in.a); break;
      case Op::Closure: out(CLOSURE); out_int(in.b); out_label(in.a); break;
      case Op::Closurerec: {
        out(CLOSUREREC); out_int(in.a); out_int(in.b);  // nfuncs, nfv
        int org = pos();
        for (int lbl : in.labels) out_label_orig(org, lbl);
        break;
      }
      case Op::Offsetclosure:
        if (in.a == -3 || in.a == 0 || in.a == 3) out(OFFSETCLOSURE0 + in.a / 3);
        else { out(OFFSETCLOSURE); out_int(in.a); } break;
      case Op::Getglobal: out(GETGLOBAL); slot_getglobal(in.str, is_predef_exn(in.str)); break;
      case Op::Setglobal: out(SETGLOBAL); slot_setglobal(in.str); break;
      case Op::Const: emit_const(in.cst); break;
      case Op::Makeblock:
        if (in.a == 0) { if (in.b == 0) out(ATOM0); else { out(ATOM); out_int(in.b); } }
        else if (in.a < 4) { out(MAKEBLOCK1 + in.a - 1); out_int(in.b); }
        else { out(MAKEBLOCK); out_int(in.a); out_int(in.b); } break;
      case Op::Getfield: if (in.a < 4) out(GETFIELD0 + in.a); else { out(GETFIELD); out_int(in.a); } break;
      case Op::Setfield: if (in.a < 4) out(SETFIELD0 + in.a); else { out(SETFIELD); out_int(in.a); } break;
      case Op::Getvectitem: out(GETVECTITEM); break;
      case Op::Setvectitem: out(SETVECTITEM); break;
      case Op::Getstringchar: out(GETSTRINGCHAR); break;
      case Op::Vectlength: out(VECTLENGTH); break;
      case Op::Makefloatblock: out(MAKEFLOATBLOCK); out_int(in.a); break;
      case Op::Getfloatfield: out(GETFLOATFIELD); out_int(in.a); break;
      case Op::Setfloatfield: out(SETFLOATFIELD); out_int(in.a); break;
      case Op::Getbyteschar: out(GETBYTESCHAR); break;
      case Op::Setbyteschar: out(SETBYTESCHAR); break;
      case Op::Physeq: out(EQ); break;     // physical == uses the EQ opcode
      case Op::Physneq: out(NEQ); break;   // physical != uses the NEQ opcode
      case Op::Branch: out(BRANCH); out_label(in.a); break;
      case Op::Branchif: out(BRANCHIF); out_label(in.a); break;
      case Op::Branchifnot: out(BRANCHIFNOT); out_label(in.a); break;
      case Op::Strictbranchif: out(BRANCHIF); out_label(in.a); break;
      case Op::Strictbranchifnot: out(BRANCHIFNOT); out_label(in.a); break;
      case Op::Switch: {
        out(SWITCH);
        int nc = in.nconsts, nb = (int)in.labels.size() - nc;
        out_int(nc | (nb << 16));
        int org = pos();
        for (int k = 0; k < (int)in.labels.size(); ++k) out_label_orig(org, in.labels[k]);
        break;
      }
      case Op::Boolnot: out(BOOLNOT); break;
      case Op::Pushtrap: out(PUSHTRAP); out_label(in.a); break;
      case Op::Poptrap: out(POPTRAP); break;
      case Op::Raise: out(RAISE); break;
      case Op::Reraise: out(RERAISE); break;
      case Op::RaiseNotrace: out(RAISE_NOTRACE); break;
      case Op::CheckSignals: out(CHECK_SIGNALS); break;
      case Op::Ccall:
        if (in.a <= 5) { out(C_CALL1 + in.a - 1); slot_cprim(in.str); }
        else { out(C_CALLN); out_int(in.a); slot_cprim(in.str); } break;
      case Op::Negint: out(NEGINT); break;
      case Op::Addint: out(ADDINT); break;
      case Op::Subint: out(SUBINT); break;
      case Op::Mulint: out(MULINT); break;
      case Op::Divint: out(DIVINT); break;
      case Op::Modint: out(MODINT); break;
      case Op::Andint: out(ANDINT); break;
      case Op::Orint: out(ORINT); break;
      case Op::Xorint: out(XORINT); break;
      case Op::Lslint: out(LSLINT); break;
      case Op::Lsrint: out(LSRINT); break;
      case Op::Asrint: out(ASRINT); break;
      case Op::Eqint: out(EQ); break;
      case Op::Neqint: out(NEQ); break;
      case Op::Ltint: out(LTINT); break;
      case Op::Leint: out(LEINT); break;
      case Op::Gtint: out(GTINT); break;
      case Op::Geint: out(GEINT); break;
      case Op::Offsetint: out(OFFSETINT); out_int(in.a); break;
      case Op::Offsetref: out(OFFSETREF); out_int(in.a); break;
      case Op::Isint: out(ISINT); break;
      case Op::Perform: out(PERFORM); break;
      case Op::Isout: out(ULTINT); break;
      case Op::Stop: out(STOP); break;
      default: break;
    }
  }
  void emit_const(const LamPtr& c) {
    if (c->k == Lam::K::ConstInt && is_immed(c->int_val)) {
      long long i = c->int_val;
      if (i >= 0 && i <= 3) out(CONST0 + (int)i); else { out(CONSTINT); out_int(i); }
    } else if (c->k == Lam::K::ConstChar) {
      out(CONSTINT); out_int(c->int_val);
    } else if (c->k == Lam::K::ConstBlock && c->args.empty()) {
      if (c->prim_arg == 0) out(ATOM0); else { out(ATOM); out_int(c->prim_arg); }
    } else {
      out(GETGLOBAL); slot_literal(c);  // string / non-empty block / float literal
    }
  }
  void emit_pushconst(const LamPtr& c) {
    if (c->k == Lam::K::ConstInt && is_immed(c->int_val)) {
      long long i = c->int_val;
      if (i >= 0 && i <= 3) out(PUSHCONST0 + (int)i); else { out(PUSHCONSTINT); out_int(i); }
    } else if (c->k == Lam::K::ConstChar) {
      out(PUSHCONSTINT); out_int(c->int_val);
    } else if (c->k == Lam::K::ConstBlock && c->args.empty()) {
      if (c->prim_arg == 0) out(PUSHATOM0); else { out(PUSHATOM); out_int(c->prim_arg); }
    } else {
      out(PUSHGETGLOBAL); slot_literal(c);
    }
  }

  // Walk the cons-list applying the emit peephole, then default to emit_instr.
  void emit(Code c) {
    auto at = [](const Code& x, int n) -> const Instr* {
      const ICell* p = x.get();
      while (n-- > 0 && p) p = p->tail.get();
      return p ? &p->head : nullptr;
    };
    auto drop = [](Code x, int n) { while (n-- > 0 && x) x = x->tail; return x; };
    while (c) {
      const Instr& h = c->head;
      const Instr* h1 = at(c, 1);
      const Instr* h2 = at(c, 2);
      const Instr* h3 = at(c, 3);
      // push; const k; intcmp; branch(if/ifnot)  ->  branch-compare
      if (h.op == Op::Push && h1 && h1->op == Op::Const && is_immed_const(h1->cst) &&
          h2 && h3 && (h3->op == Op::Branchif || h3->op == Op::Branchifnot)) {
        int cmp = -1;
        switch (h2->op) {
          case Op::Eqint: cmp = BEQ; break; case Op::Neqint: cmp = BNEQ; break;
          case Op::Ltint: cmp = BLTINT; break; case Op::Leint: cmp = BLEINT; break;
          case Op::Gtint: cmp = BGTINT; break; case Op::Geint: cmp = BGEINT; break;
          case Op::Isout: cmp = (h3->op == Op::Branchif) ? BULTINT : BUGEINT; break;
          default: break;
        }
        if (cmp >= 0) {
          // negate for branchifnot on the comparison forms
          if (h2->op != Op::Isout && h3->op == Op::Branchifnot) {
            switch (h2->op) {
              case Op::Eqint: cmp = BNEQ; break; case Op::Neqint: cmp = BEQ; break;
              case Op::Ltint: cmp = BGEINT; break; case Op::Leint: cmp = BGTINT; break;
              case Op::Gtint: cmp = BLEINT; break; case Op::Geint: cmp = BLTINT; break;
              default: break;
            }
          }
          out(cmp); out_int(const_as_int(h1->cst)); out_label(h3->a);
          c = drop(c, 4); continue;
        }
      }
      // push; acc 0; return m  ->  return (m-1)
      if (h.op == Op::Push && h1 && h1->op == Op::Acc && h1->a == 0 && h2 && h2->op == Op::Return) {
        Instr r; r.op = Op::Return; r.a = h2->a - 1;
        c = bytecode::Code(std::make_shared<const ICell>(ICell{r, drop(c, 3)}));
        continue;
      }
      // push; acc n  ->  PUSHACC
      if (h.op == Op::Push && h1 && h1->op == Op::Acc) {
        if (h1->a < 8) out(PUSHACC0 + h1->a); else { out(PUSHACC); out_int(h1->a); }
        c = drop(c, 2); continue;
      }
      if (h.op == Op::Push && h1 && h1->op == Op::Envacc) {
        if (h1->a >= 1 && h1->a <= 4) out(PUSHENVACC1 + h1->a - 1); else { out(PUSHENVACC); out_int(h1->a); }
        c = drop(c, 2); continue;
      }
      if (h.op == Op::Push && h1 && h1->op == Op::Offsetclosure) {
        if (h1->a == -3 || h1->a == 0 || h1->a == 3) out(PUSHOFFSETCLOSURE0 + h1->a / 3);
        else { out(PUSHOFFSETCLOSURE); out_int(h1->a); }
        c = drop(c, 2); continue;
      }
      // push; getglobal[; getfield]
      if (h.op == Op::Push && h1 && h1->op == Op::Getglobal && h2 && h2->op == Op::Getfield) {
        out(PUSHGETGLOBALFIELD); slot_getglobal(h1->str, is_predef_exn(h1->str)); out_int(h2->a);
        c = drop(c, 3); continue;
      }
      if (h.op == Op::Push && h1 && h1->op == Op::Getglobal) {
        out(PUSHGETGLOBAL); slot_getglobal(h1->str, is_predef_exn(h1->str));
        c = drop(c, 2); continue;
      }
      if (h.op == Op::Push && h1 && h1->op == Op::Const) {
        emit_pushconst(h1->cst);
        c = drop(c, 2); continue;
      }
      if (h.op == Op::Getglobal && h1 && h1->op == Op::Getfield) {
        out(GETGLOBALFIELD); slot_getglobal(h.str, is_predef_exn(h.str)); out_int(h1->a);
        c = drop(c, 2); continue;
      }
      emit_instr(h);
      c = c->tail;
    }
  }
};

void append_be32(std::vector<std::uint8_t>& v, std::uint32_t n) {
  v.push_back(n >> 24); v.push_back(n >> 16); v.push_back(n >> 8); v.push_back(n);
}

}  // namespace

void write_cmo(const bytecode::Code& code, const std::string& module_name,
               const std::string& path) {
  Emitter em;
  em.emit(code);

  // --- build the compilation_unit descriptor as a marshalable value ---
  std::vector<ValPtr> relocs;
  for (auto& r : em.relocs) {
    ValPtr info;
    switch (r.k) {
      case Reloc::Literal: info = vblock(0, {r.lit}); break;
      case Reloc::GetCompunit: info = vblock(1, {vstr(r.name)}); break;  // compunit unboxed
      case Reloc::GetPredef: info = vblock(2, {vstr(r.name)}); break;
      case Reloc::SetCompunit: info = vblock(3, {vstr(r.name)}); break;
      case Reloc::Primitive: info = vblock(4, {vstr(r.name)}); break;
    }
    relocs.push_back(vblock(0, {info, vint(r.pos)}));  // (reloc_info * int)
  }
  std::vector<ValPtr> prims;  // C primitives declared in this unit
  for (auto& r : em.relocs)
    if (r.k == Reloc::Primitive) prims.push_back(vstr(r.name));

  const int pos_code = 16;  // magic(12) + 4-byte compunit-offset placeholder
  int codesize = (int)em.code.size();
  int pos_compunit = pos_code + codesize;

  ValPtr compunit = vblock(0, {
      vstr(module_name),          // cu_name (compunit unboxed)
      vint(pos_code),             // cu_pos
      vint(codesize),             // cu_codesize
      vlist(relocs),              // cu_reloc
      vint(0),                    // cu_imports = []
      vint(0),                    // cu_required_compunits = []
      vlist(prims),               // cu_primitives
      vint(0),                    // cu_force_link
      vint(0),                    // cu_debug
      vint(0),                    // cu_debugsize
      vint(0),                    // cu_hint
      vint(0),                    // cu_hintsize
  });
  std::vector<std::uint8_t> cu_bytes = omarshal::marshal(compunit);

  // --- assemble the .cmo ---
  std::vector<std::uint8_t> out;
  const char* magic = "Caml1999O038";
  out.insert(out.end(), magic, magic + 12);
  append_be32(out, (std::uint32_t)pos_compunit);  // absolute offset of descriptor
  out.insert(out.end(), em.code.begin(), em.code.end());
  out.insert(out.end(), cu_bytes.begin(), cu_bytes.end());

  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + path);
  f.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
}

}  // namespace cppcaml::cmo
