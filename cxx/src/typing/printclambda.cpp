// Port of middle_end/printclambda.ml and printclambda_primitives.ml (the
// -dclambda / -drawclambda dumps).
#include "cppcaml/typing/printclambda.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/printlambda.hpp"

namespace cppcaml::typing::printclambda {

using format::Formatter;
using format::fprintf;
using format::pr;
using namespace clambda;

namespace {

// Printf's %F: "%.12g", a '.' added when it reads as an integer, and
// OCaml's names for the special values
std::string string_F(double x) {
  if (std::isnan(x)) return "nan";
  if (std::isinf(x)) return x < 0 ? "neg_infinity" : "infinity";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.12g", x);
  std::string s = buf;
  for (char c : s)
    if (c == '.' || c == 'e' || c == 'E') return s;
  return s + ".";
}

// ---- Printclambda_primitives ------------------------------------------------------------------
const char* boxed_integer_name(BoxedInteger bi) {
  switch (bi) {
    case BoxedInteger::Pnativeint: return "nativeint";
    case BoxedInteger::Pint32: return "int32";
    case BoxedInteger::Pint64: return "int64";
  }
  return "";
}
std::string boxed_integer_mark(const char* name, BoxedInteger bi) {
  switch (bi) {
    case BoxedInteger::Pnativeint: return std::string("Nativeint.") + name;
    case BoxedInteger::Pint32: return std::string("Int32.") + name;
    case BoxedInteger::Pint64: return std::string("Int64.") + name;
  }
  return "";
}
void print_boxed_integer(const char* name, Formatter& ppf, BoxedInteger bi) {
  fprintf(ppf, "%s", boxed_integer_mark(name, bi));
}
const char* array_kind(lambda::ArrayKind k) {
  switch (k) {
    case lambda::ArrayKind::Pgenarray: return "gen";
    case lambda::ArrayKind::Paddrarray: return "addr";
    case lambda::ArrayKind::Pintarray: return "int";
    case lambda::ArrayKind::Pfloatarray: return "float";
  }
  return "";
}
const char* access_size(MemoryAccessSize s) {
  switch (s) {
    case MemoryAccessSize::Sixteen: return "16";
    case MemoryAccessSize::Thirty_two: return "32";
    case MemoryAccessSize::Sixty_four: return "64";
  }
  return "";
}
const char* access_safety(lambda::IsSafe s) { return s == lambda::IsSafe::Safe ? "" : "unsafe_"; }
const char* init_string(lambda::InitializationOrAssignment i) {
  switch (i) {
    case lambda::InitializationOrAssignment::Heap_initialization: return "(heap-init)";
    case lambda::InitializationOrAssignment::Root_initialization: return "(root-init)";
    case lambda::InitializationOrAssignment::Assignment: return "";
  }
  return "";
}

}  // namespace

void primitive(Formatter& ppf, const clambda::Primitive& p) {
  using K = clambda::Primitive::K;
  using lambda::IsSafe;
  using lambda::ImmediateOrPointer;
  switch (p.kind) {
    case K::Pread_symbol: fprintf(ppf, "read_symbol %s", p.sym); return;
    case K::Pmakeblock:
      if (p.mut == MutableFlag::Immutable)
        fprintf(ppf, "makeblock %i%a", p.n, pr(printlambda::block_shape, p.shape));
      else
        fprintf(ppf, "makemutable %i%a", p.n, pr(printlambda::block_shape, p.shape));
      return;
    case K::Pmakelazyblock:
      fprintf(ppf, p.lazy_tag == lambda::LazyBlockTag::Lazy_tag ? "makelazyblock" : "makeforwardblock");
      return;
    case K::Pfield: {
      const char* instr = p.ptr == ImmediateOrPointer::Immediate ? "field_int "
                          : p.mut == MutableFlag::Mutable        ? "field_mut "
                                                                 : "field_imm ";
      fprintf(ppf, "%s%i", instr, p.n);
      return;
    }
    case K::Pfield_computed: fprintf(ppf, "field_computed"); return;
    case K::Psetfield:
      fprintf(ppf, "setfield_%s%s %i", p.ptr == ImmediateOrPointer::Pointer ? "ptr" : "imm", init_string(p.init), p.n);
      return;
    case K::Psetfield_computed:
      fprintf(ppf, "setfield_%s%s_computed", p.ptr == ImmediateOrPointer::Pointer ? "ptr" : "imm",
              init_string(p.init));
      return;
    case K::Pfloatfield: fprintf(ppf, "floatfield %i", p.n); return;
    case K::Psetfloatfield: fprintf(ppf, "setfloatfield%s %i", init_string(p.init), p.n); return;
    case K::Pduprecord: fprintf(ppf, "duprecord %a %i", pr(printlambda::record_rep, p.repr), p.n); return;
    case K::Prunstack: fprintf(ppf, "runstack"); return;
    case K::Pperform: fprintf(ppf, "perform"); return;
    case K::Presume: fprintf(ppf, "resume"); return;
    case K::Preperform: fprintf(ppf, "reperform"); return;
    case K::Pccall: fprintf(ppf, "%s", p.ccall->prim_name); return;
    case K::Praise: fprintf(ppf, "%s", lambda::raise_kind(p.raise)); return;
    case K::Psequand: fprintf(ppf, "&&"); return;
    case K::Psequor: fprintf(ppf, "||"); return;
    case K::Pnot: fprintf(ppf, "not"); return;
    case K::Pnegint: fprintf(ppf, "~"); return;
    case K::Paddint: fprintf(ppf, "+"); return;
    case K::Psubint: fprintf(ppf, "-"); return;
    case K::Pmulint: fprintf(ppf, "*"); return;
    case K::Pdivint: fprintf(ppf, p.safe == IsSafe::Safe ? "/" : "/u"); return;
    case K::Pmodint: fprintf(ppf, p.safe == IsSafe::Safe ? "mod" : "mod_unsafe"); return;
    case K::Pandint: fprintf(ppf, "and"); return;
    case K::Porint: fprintf(ppf, "or"); return;
    case K::Pxorint: fprintf(ppf, "xor"); return;
    case K::Plslint: fprintf(ppf, "lsl"); return;
    case K::Plsrint: fprintf(ppf, "lsr"); return;
    case K::Pasrint: fprintf(ppf, "asr"); return;
    case K::Pintcomp: printlambda::integer_comparison(ppf, p.icmp); return;
    case K::Pcompare_ints: fprintf(ppf, "compare_ints"); return;
    case K::Pcompare_floats: fprintf(ppf, "compare_floats"); return;
    case K::Pcompare_bints: fprintf(ppf, "compare_bints %s", boxed_integer_name(p.bi)); return;
    case K::Poffsetint: fprintf(ppf, "%i+", p.n); return;
    case K::Poffsetref: fprintf(ppf, "+:=%i", p.n); return;
    case K::Pintoffloat: fprintf(ppf, "int_of_float"); return;
    case K::Pfloatofint: fprintf(ppf, "float_of_int"); return;
    case K::Pnegfloat: fprintf(ppf, "~."); return;
    case K::Pabsfloat: fprintf(ppf, "abs."); return;
    case K::Paddfloat: fprintf(ppf, "+."); return;
    case K::Psubfloat: fprintf(ppf, "-."); return;
    case K::Pmulfloat: fprintf(ppf, "*."); return;
    case K::Pdivfloat: fprintf(ppf, "/."); return;
    case K::Pfloatcomp: printlambda::float_comparison(ppf, p.fcmp); return;
    case K::Pstringlength: fprintf(ppf, "string.length"); return;
    case K::Pstringrefu: fprintf(ppf, "string.unsafe_get"); return;
    case K::Pstringrefs: fprintf(ppf, "string.get"); return;
    case K::Pbyteslength: fprintf(ppf, "bytes.length"); return;
    case K::Pbytesrefu: fprintf(ppf, "bytes.unsafe_get"); return;
    case K::Pbytessetu: fprintf(ppf, "bytes.unsafe_set"); return;
    case K::Pbytesrefs: fprintf(ppf, "bytes.get"); return;
    case K::Pbytessets: fprintf(ppf, "bytes.set"); return;
    case K::Parraylength: fprintf(ppf, "array.length[%s]", array_kind(p.array)); return;
    case K::Pmakearray:
      fprintf(ppf, p.mut == MutableFlag::Mutable ? "makearray[%s]" : "makearray_imm[%s]", array_kind(p.array));
      return;
    case K::Pduparray:
      fprintf(ppf, p.mut == MutableFlag::Mutable ? "duparray[%s]" : "duparray_imm[%s]", array_kind(p.array));
      return;
    case K::Parrayrefu: fprintf(ppf, "array.unsafe_get[%s]", array_kind(p.array)); return;
    case K::Parraysetu: fprintf(ppf, "array.unsafe_set[%s]", array_kind(p.array)); return;
    case K::Parrayrefs: fprintf(ppf, "array.get[%s]", array_kind(p.array)); return;
    case K::Parraysets: fprintf(ppf, "array.set[%s]", array_kind(p.array)); return;
    case K::Pisint: fprintf(ppf, "isint"); return;
    case K::Pisout: fprintf(ppf, "isout"); return;
    case K::Pcheckbound: fprintf(ppf, "checkbound"); return;
    case K::Pbintofint: print_boxed_integer("of_int", ppf, p.bi); return;
    case K::Pintofbint: print_boxed_integer("to_int", ppf, p.bi); return;
    case K::Pcvtbint: fprintf(ppf, "%s_of_%s", boxed_integer_name(p.bi2), boxed_integer_name(p.bi)); return;
    case K::Pnegbint: print_boxed_integer("neg", ppf, p.bi); return;
    case K::Paddbint: print_boxed_integer("add", ppf, p.bi); return;
    case K::Psubbint: print_boxed_integer("sub", ppf, p.bi); return;
    case K::Pmulbint: print_boxed_integer("mul", ppf, p.bi); return;
    case K::Pdivbint: print_boxed_integer(p.safe == IsSafe::Safe ? "div" : "div_unsafe", ppf, p.bi); return;
    case K::Pmodbint: print_boxed_integer(p.safe == IsSafe::Safe ? "mod" : "mod_unsafe", ppf, p.bi); return;
    case K::Pandbint: print_boxed_integer("and", ppf, p.bi); return;
    case K::Porbint: print_boxed_integer("or", ppf, p.bi); return;
    case K::Pxorbint: print_boxed_integer("xor", ppf, p.bi); return;
    case K::Plslbint: print_boxed_integer("lsl", ppf, p.bi); return;
    case K::Plsrbint: print_boxed_integer("lsr", ppf, p.bi); return;
    case K::Pasrbint: print_boxed_integer("asr", ppf, p.bi); return;
    case K::Pbintcomp: {
      using C = lambda::IntegerComparison;
      const char* op = p.icmp == C::Ceq ? "==" : p.icmp == C::Cne ? "!=" : p.icmp == C::Clt ? "<"
                       : p.icmp == C::Cgt ? ">" : p.icmp == C::Cle ? "<=" : ">=";
      print_boxed_integer(op, ppf, p.bi);
      return;
    }
    case K::Pbigarrayref: printlambda::print_bigarray("get", p.unsafe, p.ba_kind, ppf, p.ba_layout); return;
    case K::Pbigarrayset: printlambda::print_bigarray("set", p.unsafe, p.ba_kind, ppf, p.ba_layout); return;
    case K::Pbigarraydim: fprintf(ppf, "Bigarray.dim_%i", p.n); return;
    case K::Pstring_load:
      fprintf(ppf, "string.%sget%s", access_safety(p.safe), access_size(p.size));
      return;
    case K::Pbytes_load: fprintf(ppf, "bytes.%sget%s", access_safety(p.safe), access_size(p.size)); return;
    case K::Pbytes_set: fprintf(ppf, "bytes.%sset%s", access_safety(p.safe), access_size(p.size)); return;
    case K::Pbigstring_load:
      fprintf(ppf, "bigarray.array1.%sget%s", access_safety(p.safe), access_size(p.size));
      return;
    case K::Pbigstring_set:
      fprintf(ppf, "bigarray.array1.%sset%s", access_safety(p.safe), access_size(p.size));
      return;
    case K::Pbswap16: fprintf(ppf, "bswap16"); return;
    case K::Pbbswap: print_boxed_integer("bswap", ppf, p.bi); return;
    case K::Pint_as_pointer: fprintf(ppf, "int_as_pointer"); return;
    case K::Patomic_load: fprintf(ppf, "atomic_load"); return;
    case K::Patomic_fetch_add: fprintf(ppf, "atomic_fetch_add"); return;
    case K::Popaque: fprintf(ppf, "opaque"); return;
    case K::Pdls_get: fprintf(ppf, "dls_get"); return;
    case K::Ppoll: fprintf(ppf, "poll"); return;
  }
}

// ---- Printclambda -------------------------------------------------------------------------------
namespace {

const char* mutable_flag(MutableFlag m) { return m == MutableFlag::Mutable ? "[mut]" : ""; }

const char* value_kind(const lambda::ValueKind& k) {
  switch (k.kind) {
    case lambda::ValueKind::Kind::Pgenval: return "";
    case lambda::ValueKind::Kind::Pintval: return ":int";
    case lambda::ValueKind::Kind::Pfloatval: return ":float";
    case lambda::ValueKind::Kind::Pboxedintval:
      switch (k.bi) {
        case BoxedInteger::Pnativeint: return ":nativeint";
        case BoxedInteger::Pint32: return ":int32";
        case BoxedInteger::Pint64: return ":int64";
      }
  }
  return "";
}

void var(Formatter& ppf, Var v) { printlambda::ident(ppf, v); }

// Debuginfo.print_compact
void debuginfo_print_compact(Formatter& ppf, const debuginfo::t& t) {
  for (std::size_t k = 0; k < t.size(); ++k) {
    const debuginfo::Item& item = t[k];
    if (k) fprintf(ppf, ";");
    fprintf(ppf, "%s:%i", location::show_filename(std::string(item.dinfo_file)), item.dinfo_line);
    if (item.dinfo_char_start >= 0) fprintf(ppf, ",%i--%i", item.dinfo_char_start, item.dinfo_char_end);
  }
}

// Backend_var.With_provenance.print
void vp(Formatter& ppf, const VarWithProvenance& x) {
  if (!x.provenance) {
    var(ppf, x.var);
    return;
  }
  const Provenance* p = x.provenance;
  fprintf(ppf, "%a[", pr(var, x.var));
  fprintf(ppf, "@[<hov 1>(");
  fprintf(ppf, "@[<hov 1>(module_path@ %a)@]@ ", pr(printlambda::print_path, p->module_path));
  if (clflags::locations) fprintf(ppf, "@[<hov 1>(location@ %a)@]@ ", pr(debuginfo_print_compact, p->location));
  fprintf(ppf, "@[<hov 1>(original_ident@ %a)@]", pr(var, p->original_ident));
  fprintf(ppf, ")@]");
  fprintf(ppf, "]");
}

void lam(Formatter& ppf, ulambda l);
void sequence(Formatter& ppf, ulambda l);
void uconstant(Formatter& ppf, const UConstant& c);
void one_fun(Formatter& ppf, const UFunction* f);

void structured_constant(Formatter& ppf, const UStructuredConstant* c) {
  using K = UStructuredConstant::Kind;
  switch (c->kind) {
    case K::Uconst_float: fprintf(ppf, "%s", string_F(c->f)); return;
    case K::Uconst_int32: fprintf(ppf, "%ldl", c->i); return;
    case K::Uconst_int64: fprintf(ppf, "%LdL", c->i); return;
    case K::Uconst_nativeint: fprintf(ppf, "%ndn", c->i); return;
    case K::Uconst_block:
      fprintf(ppf, "block(%i", c->tag);
      for (const UConstant& u : c->fields) fprintf(ppf, ",%a", pr(uconstant, u));
      fprintf(ppf, ")");
      return;
    case K::Uconst_float_array:
      if (c->floats.empty()) {
        fprintf(ppf, "floatarray()");
        return;
      }
      fprintf(ppf, "floatarray(%s", string_F(c->floats[0]));
      for (std::size_t k = 1; k < c->floats.size(); ++k) fprintf(ppf, ",%s", string_F(c->floats[k]));
      fprintf(ppf, ")");
      return;
    case K::Uconst_string: fprintf(ppf, "%S", c->s); return;
    case K::Uconst_closure: {
      auto funs = [c](Formatter& f) {
        for (const UFunction* fn : c->funs) fprintf(f, "@ %a", pr(one_fun, fn));
      };
      auto sconsts = [c](Formatter& f) {
        for (const UConstant& sc : c->fields) fprintf(f, "@ %a", pr(uconstant, sc));
      };
      fprintf(ppf, "@[<2>(const_closure%t %s@ %t)@]", funs, c->s, sconsts);
      return;
    }
  }
}

void one_fun(Formatter& ppf, const UFunction* f) {
  auto idents = [f](Formatter& g) {
    for (const UParam& x : f->params) fprintf(g, "@ %a%a", pr(vp, x.var), pr(printlambda::value_kind, x.kind));
  };
  fprintf(ppf, "(fun@ %s%s@ %i@ @[<2>%t@]@ @[<2>%a@])", f->label, value_kind(f->return_), f->arity, idents,
          pr(lam, f->body));
}

void phantom_defining_expr(Formatter& ppf, const UPhantomDefiningExpr* e) {
  using K = UPhantomDefiningExpr::Kind;
  switch (e->kind) {
    case K::Uphantom_const: uconstant(ppf, e->c); return;
    case K::Uphantom_var: var(ppf, e->var); return;
    case K::Uphantom_offset_var: fprintf(ppf, "%a+(%i)", pr(var, e->var), e->n); return;
    case K::Uphantom_read_field: fprintf(ppf, "%a[%i]", pr(var, e->var), e->n); return;
    case K::Uphantom_read_symbol_field: fprintf(ppf, "%s[%i]", e->sym, e->n); return;
    case K::Uphantom_block:
      fprintf(ppf, "[%i: ", e->n);
      for (Var f : e->fields) fprintf(ppf, "%a; ", pr(var, f));
      fprintf(ppf, "]");
      return;
  }
}
void phantom_defining_expr_opt(Formatter& ppf, const UPhantomDefiningExpr* e) {
  if (!e) fprintf(ppf, "DEAD");
  else phantom_defining_expr(ppf, e);
}

void uconstant(Formatter& ppf, const UConstant& c) {
  if (c.kind == UConstant::Kind::Uconst_int) {
    fprintf(ppf, "%i", c.i);
  } else if (c.sc) {
    fprintf(ppf, "%S=%a", c.sym, pr(structured_constant, c.sc));
  } else {
    fprintf(ppf, "%S", c.sym);
  }
}

void lams(Formatter& ppf, Slice<ulambda> largs) {
  for (ulambda l : largs) fprintf(ppf, "@ %a", pr(lam, l));
}

void lam(Formatter& ppf, ulambda l) {
  switch (l->kind) {
    case UK::Uvar: var(ppf, as<Uvar>(l)->id); return;
    case UK::Uconst: uconstant(ppf, as<Uconst>(l)->c); return;
    case UK::Udirect_apply: {
      auto* x = as<Udirect_apply>(l);
      fprintf(ppf, "@[<2>(apply*@ %s %a)@]", x->f, pr(lams, x->args));
      return;
    }
    case UK::Ugeneric_apply: {
      auto* x = as<Ugeneric_apply>(l);
      fprintf(ppf, "@[<2>(apply@ %a%a)@]", pr(lam, x->f), pr(lams, x->args));
      return;
    }
    case UK::Uclosure: {
      auto* x = as<Uclosure>(l);
      auto funs = [x](Formatter& f) {
        for (const UFunction* fn : x->funs) fprintf(f, "@ @[<2>%a@]", pr(one_fun, fn));
      };
      auto fvs = [x](Formatter& f) {
        for (ulambda v : x->fv) fprintf(f, "@ %a", pr(lam, v));
      };
      fprintf(ppf, "@[<2>(closure@ %t %t)@]", funs, fvs);
      return;
    }
    case UK::Uoffset: {
      auto* x = as<Uoffset>(l);
      fprintf(ppf, "@[<2>(offset %a %i)@]", pr(lam, x->l), x->ofs);
      return;
    }
    case UK::Ulet: {
      auto* x = as<Ulet>(l);
      fprintf(ppf, "@[<2>(let@ @[<hv 1>(@[<2>%a%s%s@ %a@]", pr(vp, x->id), mutable_flag(x->mut), value_kind(x->k),
              pr(lam, x->arg));
      ulambda expr = x->body;
      while (auto* y = as<Ulet>(expr)) {
        fprintf(ppf, "@ @[<2>%a%s%s@ %a@]", pr(vp, y->id), mutable_flag(y->mut), value_kind(y->k), pr(lam, y->arg));
        expr = y->body;
      }
      fprintf(ppf, ")@]@ %a)@]", pr(lam, expr));
      return;
    }
    case UK::Uphantom_let: {
      auto* x = as<Uphantom_let>(l);
      fprintf(ppf, "@[<2>(phantom_let@ @[<hv 1>(@[<2>%a@ %a@]", pr(vp, x->id),
              pr(phantom_defining_expr_opt, x->def));
      ulambda expr = x->body;
      while (auto* y = as<Uphantom_let>(expr)) {
        fprintf(ppf, "@ @[<2>%a@ %a@]", pr(vp, y->id), pr(phantom_defining_expr_opt, y->def));
        expr = y->body;
      }
      fprintf(ppf, ")@]@ %a)@]", pr(lam, expr));
      return;
    }
    case UK::Uprim: {
      auto* x = as<Uprim>(l);
      fprintf(ppf, "@[<2>(%a%a)@]", pr(primitive, x->p), pr(lams, x->args));
      return;
    }
    case UK::Uswitch: {
      auto* x = as<Uswitch>(l);
      auto print_cases = [](Formatter& f, const char* tag, const Slice<long>& index, const Slice<ulambda>& cases) {
        for (std::size_t i = 0; i < cases.size(); ++i) {
          auto print_case = [&](Formatter& g) {
            for (std::size_t j = 0; j < index.size(); ++j)
              if (index[j] == static_cast<long>(i)) fprintf(g, "case %s %i:", tag, static_cast<long>(j));
          };
          fprintf(f, "@ @[<2>%t@ %a@]", print_case, pr(sequence, cases[i]));
        }
      };
      auto sw = [&](Formatter& f) {
        print_cases(f, "int", x->sw.us_index_consts, x->sw.us_actions_consts);
        print_cases(f, "tag", x->sw.us_index_blocks, x->sw.us_actions_blocks);
      };
      fprintf(ppf, "@[<v 0>@[<2>(switch@ %a@ @]%t)@]", pr(lam, x->arg), sw);
      return;
    }
    case UK::Ustringswitch: {
      auto* x = as<Ustringswitch>(l);
      auto sw = [x](Formatter& f) {
        bool spc = false;
        for (const UStringCase& c : x->cases) {
          if (spc) fprintf(f, "@ ");
          else spc = true;
          fprintf(f, "@[<hv 1>case \"%s\":@ %a@]", format::string_escaped(c.s), pr(lam, c.action));
        }
        if (x->def) {
          if (spc) fprintf(f, "@ ");
          else spc = true;
          fprintf(f, "@[<hv 1>default:@ %a@]", pr(lam, x->def));
        }
      };
      fprintf(ppf, "@[<1>(switch %a@ @[<v 0>%t@])@]", pr(lam, x->arg), sw);
      return;
    }
    case UK::Ustaticfail: {
      auto* x = as<Ustaticfail>(l);
      fprintf(ppf, "@[<2>(exit@ %i%a)@]", x->i, pr(lams, x->args));
      return;
    }
    case UK::Ucatch: {
      auto* x = as<Ucatch>(l);
      auto vars = [x](Formatter& f) {
        for (const UParam& v : x->vars) fprintf(f, " %a%a", pr(vp, v.var), pr(printlambda::value_kind, v.kind));
      };
      fprintf(ppf, "@[<2>(catch@ %a@;<1 -1>with (%i%t)@ %a)@]", pr(lam, x->body), x->i, vars, pr(lam, x->handler));
      return;
    }
    case UK::Utrywith: {
      auto* x = as<Utrywith>(l);
      fprintf(ppf, "@[<2>(try@ %a@;<1 -1>with %a@ %a)@]", pr(lam, x->body), pr(vp, x->exn), pr(lam, x->handler));
      return;
    }
    case UK::Uifthenelse: {
      auto* x = as<Uifthenelse>(l);
      fprintf(ppf, "@[<2>(if@ %a@ %a@ %a)@]", pr(lam, x->cond), pr(lam, x->ifso), pr(lam, x->ifnot));
      return;
    }
    case UK::Usequence: {
      auto* x = as<Usequence>(l);
      fprintf(ppf, "@[<2>(seq@ %a@ %a)@]", pr(lam, x->l1), pr(sequence, x->l2));
      return;
    }
    case UK::Uwhile: {
      auto* x = as<Uwhile>(l);
      fprintf(ppf, "@[<2>(while@ %a@ %a)@]", pr(lam, x->cond), pr(lam, x->body));
      return;
    }
    case UK::Ufor: {
      auto* x = as<Ufor>(l);
      fprintf(ppf, "@[<2>(for %a@ %a@ %s@ %a@ %a)@]", pr(vp, x->id), pr(lam, x->lo),
              x->dir == parsetree::DirectionFlag::Upto ? "to" : "downto", pr(lam, x->hi), pr(lam, x->body));
      return;
    }
    case UK::Uassign: {
      auto* x = as<Uassign>(l);
      fprintf(ppf, "@[<2>(assign@ %a@ %a)@]", pr(var, x->id), pr(lam, x->e));
      return;
    }
    case UK::Usend: {
      auto* x = as<Usend>(l);
      const char* kind = x->k == lambda::MethKind::Self ? "self" : x->k == lambda::MethKind::Cached ? "cache" : "";
      fprintf(ppf, "@[<2>(send%s@ %a@ %a%a)@]", kind, pr(lam, x->obj), pr(lam, x->met), pr(lams, x->args));
      return;
    }
    case UK::Uunreachable: fprintf(ppf, "unreachable"); return;
  }
}

void sequence(Formatter& ppf, ulambda l) {
  if (auto* x = as<Usequence>(l)) {
    fprintf(ppf, "%a@ %a", pr(sequence, x->l1), pr(sequence, x->l2));
    return;
  }
  lam(ppf, l);
}

}  // namespace

void clambda(Formatter& ppf, ulambda l) { fprintf(ppf, "%a@.", pr(lam, l)); }

void approx(Formatter& ppf, const ValueApproximation* a) {
  using K = ValueApproximation::Kind;
  switch (a->kind) {
    case K::Value_closure:
      fprintf(ppf, "@[<2>function %s@ arity %i", a->fundesc->fun_label, a->fundesc->fun_arity);
      if (a->fundesc->fun_closed) fprintf(ppf, "@ (closed)");
      if (a->fundesc->has_inline) fprintf(ppf, "@ (inline)");
      fprintf(ppf, "@ -> @ %a@]", pr(approx, a->res));
      return;
    case K::Value_tuple: {
      auto tuple = [a](Formatter& f) {
        for (std::size_t i = 0; i < a->tuple.size(); ++i) {
          if (i > 0) fprintf(f, ";@ ");
          fprintf(f, "%i: %a", static_cast<long>(i), pr(approx, a->tuple[i]));
        }
      };
      fprintf(ppf, "@[<hov 1>(%t)@]", tuple);
      return;
    }
    case K::Value_unknown: fprintf(ppf, "_"); return;
    case K::Value_const: fprintf(ppf, "@[const(%a)@]", pr(uconstant, a->c)); return;
    case K::Value_global_field: fprintf(ppf, "@[global(%s,%i)@]", a->sym, a->field); return;
  }
}

std::string dump(ulambda l) {
  Formatter ppf;
  clambda(ppf, l);
  return ppf.take();
}

}  // namespace cppcaml::typing::printclambda
