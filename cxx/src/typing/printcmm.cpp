// Port of asmcomp/printcmm.ml.  See printcmm.hpp.
#include "cppcaml/typing/printcmm.hpp"

#include <string>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/printclambda.hpp"

namespace cppcaml::typing::printcmm {

using namespace cmm;
using format::Formatter;
using format::fprintf;
using format::pr;
using OK = Operation::K;
using Expr = cmm::expression;

namespace {

void rec_flag(Formatter& ppf, cmm::RecFlag f) {
  if (f == cmm::RecFlag::Recursive) fprintf(ppf, " rec");
}

void machtype_component(Formatter& ppf, MachtypeComponent c) {
  switch (c) {
    case MachtypeComponent::Val: fprintf(ppf, "val"); return;
    case MachtypeComponent::Addr: fprintf(ppf, "addr"); return;
    case MachtypeComponent::Int: fprintf(ppf, "int"); return;
    case MachtypeComponent::Float: fprintf(ppf, "float"); return;
  }
}

void exttype(Formatter& ppf, Exttype t) {
  switch (t) {
    case Exttype::XInt: fprintf(ppf, "int"); return;
    case Exttype::XInt32: fprintf(ppf, "int32"); return;
    case Exttype::XInt64: fprintf(ppf, "int64"); return;
    case Exttype::XFloat: fprintf(ppf, "float"); return;
  }
}

void extcall_signature(Formatter& ppf, Machtype ty_res, Slice<Exttype> ty_args) {
  for (std::size_t k = 0; k < ty_args.size(); ++k) {
    if (k) fprintf(ppf, ",");
    exttype(ppf, ty_args[k]);
  }
  fprintf(ppf, "->%a", pr(machtype, ty_res));
}

const char* integer_comparison(IntegerComparison c) {
  switch (c) {
    case IntegerComparison::Ceq: return "==";
    case IntegerComparison::Cne: return "!=";
    case IntegerComparison::Clt: return "<";
    case IntegerComparison::Cle: return "<=";
    case IntegerComparison::Cgt: return ">";
    case IntegerComparison::Cge: return ">=";
  }
  return "";
}

const char* float_comparison(FloatComparison c) {
  switch (c) {
    case FloatComparison::CFeq: return "==";
    case FloatComparison::CFneq: return "!=";
    case FloatComparison::CFlt: return "<";
    case FloatComparison::CFnlt: return "!<";
    case FloatComparison::CFle: return "<=";
    case FloatComparison::CFnle: return "!<=";
    case FloatComparison::CFgt: return ">";
    case FloatComparison::CFngt: return "!>";
    case FloatComparison::CFge: return ">=";
    case FloatComparison::CFnge: return "!>=";
  }
  return "";
}

const char* chunk(MemoryChunk c) {
  switch (c) {
    case MemoryChunk::Byte_unsigned: return "unsigned int8";
    case MemoryChunk::Byte_signed: return "signed int8";
    case MemoryChunk::Sixteen_unsigned: return "unsigned int16";
    case MemoryChunk::Sixteen_signed: return "signed int16";
    case MemoryChunk::Thirtytwo_unsigned: return "unsigned int32";
    case MemoryChunk::Thirtytwo_signed: return "signed int32";
    case MemoryChunk::Sixtyfour: return "int64";
    case MemoryChunk::Word_int: return "int";
    case MemoryChunk::Word_val: return "val";
    case MemoryChunk::Single: return "float32";
    case MemoryChunk::Double: return "float64";
  }
  return "";
}

std::string location(const debuginfo::t& d) {
  if (!clflags::locations) return "";
  return debuginfo::to_string(d);
}

std::string operation(const debuginfo::t& d, const Operation& o) {
  switch (o.kind) {
    case OK::Capply: return "app" + location(d);
    case OK::Cextcall: return "extcall \"" + std::string(o.name) + "\"" + location(d);
    case OK::Cload: {
      std::string s = "load";
      if (o.mut == MutableFlag::Mutable) s += "_mut";
      if (o.is_atomic) s += "_atomic";
      return s + " " + chunk(o.chunk);
    }
    case OK::Calloc: return "alloc" + location(d);
    case OK::Cstore: {
      const char* init = "";
      switch (o.init) {
        case lambda::InitializationOrAssignment::Heap_initialization: init = "(heap-init)"; break;
        case lambda::InitializationOrAssignment::Root_initialization: init = "(root-init)"; break;
        case lambda::InitializationOrAssignment::Assignment: break;
      }
      return std::string("store ") + chunk(o.chunk) + init;
    }
    case OK::Caddi: return "+";
    case OK::Csubi: return "-";
    case OK::Cmuli: return "*";
    case OK::Cmulhi: return "*h";
    case OK::Cdivi: return "/";
    case OK::Cmodi: return "mod";
    case OK::Cand: return "and";
    case OK::Cor: return "or";
    case OK::Cxor: return "xor";
    case OK::Clsl: return "<<";
    case OK::Clsr: return ">>u";
    case OK::Casr: return ">>s";
    case OK::Ccmpi: return integer_comparison(o.icmp);
    case OK::Caddv: return "+v";
    case OK::Cadda: return "+a";
    case OK::Ccmpa: return std::string(integer_comparison(o.icmp)) + "a";
    case OK::Cnegf: return "~f";
    case OK::Cabsf: return "absf";
    case OK::Caddf: return "+f";
    case OK::Csubf: return "-f";
    case OK::Cmulf: return "*f";
    case OK::Cdivf: return "/f";
    case OK::Cfloatofint: return "floatofint";
    case OK::Cintoffloat: return "intoffloat";
    case OK::Ccmpf: return std::string(float_comparison(o.fcmp)) + "f";
    case OK::Craise: return std::string(lambda::raise_kind(o.raise)) + location(d);
    case OK::Ccheckbound: return "checkbound" + location(d);
    case OK::Copaque: return "opaque";
    case OK::Catomic_fetch_add: return "atomic_fetch_add";
    case OK::Cdls_get: return "dls_get";
    case OK::Cpoll: return "poll";
  }
  return "";
}

void expr(Formatter& ppf, Expr e);
void sequence(Formatter& ppf, Expr e);

void vp(Formatter& ppf, const VarWithProvenance& v) { printclambda::print_vp(ppf, v); }
void var(Formatter& ppf, Var v) { printclambda::print_var(ppf, v); }

void expr(Formatter& ppf, Expr e) {
  switch (e->kind) {
    case EK::Cconst_int: fprintf(ppf, "%i", static_cast<const Cconst_int*>(e)->n); return;
    case EK::Cconst_natint:
      fprintf(ppf, "%s", std::to_string(static_cast<const Cconst_natint*>(e)->n));
      return;
    case EK::Cconst_float: fprintf(ppf, "%s", printclambda::float_F(static_cast<const Cconst_float*>(e)->f)); return;
    case EK::Cconst_symbol: fprintf(ppf, "\"%s\"", static_cast<const Cconst_symbol*>(e)->s); return;
    case EK::Cvar: var(ppf, static_cast<const Cvar*>(e)->id); return;
    case EK::Cvar_mut: fprintf(ppf, "!%a", pr(var, static_cast<const Cvar_mut*>(e)->id)); return;
    case EK::Creturn_addr: fprintf(ppf, "return_addr"); return;
    case EK::Clet: {
      auto* x = static_cast<const Clet*>(e);
      if (x->body->kind == EK::Clet) {
        auto print_binding = [](Formatter& f, const Clet* l) {
          fprintf(f, "@[<2>%a@ %a@]", pr(vp, l->id), pr(expr, l->def));
        };
        fprintf(ppf, "@[<2>(let@ @[<1>(");
        print_binding(ppf, x);
        Expr b = x->body;
        while (auto* l = as<Clet>(b)) {
          fprintf(ppf, "@ ");
          print_binding(ppf, l);
          b = l->body;
        }
        fprintf(ppf, ")@]@ %a)@]", pr(sequence, b));
        return;
      }
      fprintf(ppf, "@[<2>(let@ @[<2>%a@ %a@]@ %a)@]", pr(vp, x->id), pr(expr, x->def), pr(sequence, x->body));
      return;
    }
    case EK::Clet_mut: {
      auto* x = static_cast<const Clet_mut*>(e);
      fprintf(ppf, "@[<2>(let_mut@ @[<2>%a: %a@ %a@]@ %a)@]", pr(vp, x->id), pr(machtype, x->ty), pr(expr, x->def),
              pr(sequence, x->body));
      return;
    }
    case EK::Cphantom_let: {
      auto* x = static_cast<const Cphantom_let*>(e);
      fprintf(ppf, "@[<2>(let?@ @[<2>%a@ ()@]@ %a)@]", pr(vp, x->id), pr(sequence, x->body));
      return;
    }
    case EK::Cassign: {
      auto* x = static_cast<const Cassign*>(e);
      fprintf(ppf, "@[<2>(assign @[<2>%a@ %a@])@]", pr(var, x->id), pr(expr, x->e));
      return;
    }
    case EK::Ctuple: {
      auto* x = static_cast<const Ctuple*>(e);
      auto tuple = [x](Formatter& f) {
        bool first = true;
        for (Expr y : x->el) {
          if (first) first = false;
          else fprintf(f, "@ ");
          expr(f, y);
        }
      };
      fprintf(ppf, "@[<1>[%t]@]", tuple);
      return;
    }
    case EK::Cop: {
      auto* x = static_cast<const Cop*>(e);
      fprintf(ppf, "@[<2>(%s", operation(x->dbg, x->op));
      for (Expr y : x->args) fprintf(ppf, "@ %a", pr(expr, y));
      if (x->op.kind == OK::Capply) fprintf(ppf, "@ %a", pr(machtype, x->op.ty));
      else if (x->op.kind == OK::Cextcall)
        fprintf(ppf, "@ %t", [x](Formatter& f) { extcall_signature(f, x->op.ty, x->op.ty_args); });
      fprintf(ppf, ")@]");
      return;
    }
    case EK::Csequence: {
      auto* x = static_cast<const Csequence*>(e);
      fprintf(ppf, "@[<2>(seq@ %a@ %a)@]", pr(sequence, x->e1), pr(sequence, x->e2));
      return;
    }
    case EK::Cifthenelse: {
      auto* x = static_cast<const Cifthenelse*>(e);
      fprintf(ppf, "@[<2>(if@ %a@ %a@ %a)@]", pr(expr, x->cond), pr(expr, x->ifso), pr(expr, x->ifnot));
      return;
    }
    case EK::Cswitch: {
      auto* x = static_cast<const Cswitch*>(e);
      auto print_cases = [x](Formatter& f) {
        for (std::size_t i = 0; i < x->cases.size(); ++i) {
          auto print_case = [x, i](Formatter& g) {
            for (std::size_t j = 0; j < x->index.size(); ++j)
              if (x->index[j] == static_cast<long>(i)) fprintf(g, "case %i:", static_cast<long>(j));
          };
          fprintf(f, "@ @[<2>%t@ %a@]", print_case, pr(sequence, x->cases[i].e));
        }
      };
      fprintf(ppf, "@[<v 0>@[<2>(switch@ %a@ @]%t)@]", pr(expr, x->e), print_cases);
      return;
    }
    case EK::Ccatch: {
      auto* x = static_cast<const Ccatch*>(e);
      auto print_handlers = [x](Formatter& f) {
        for (const Handler& h : x->handlers) {
          auto ids = [&h](Formatter& g) {
            for (const CatchParam& p : h.ids) fprintf(g, "@ %a: %a", pr(vp, p.id), pr(machtype, p.ty));
          };
          fprintf(f, "(%i%t)@ %a", h.n, ids, pr(sequence, h.body));
        }
      };
      fprintf(ppf, "@[<2>(catch%t@ %a@;<1 -2>with%t)@]", [x](Formatter& f) { rec_flag(f, x->rec); },
              pr(sequence, x->body), print_handlers);
      return;
    }
    case EK::Cexit: {
      auto* x = static_cast<const Cexit*>(e);
      fprintf(ppf, "@[<2>(exit %i", x->n);
      for (Expr y : x->args) fprintf(ppf, "@ %a", pr(expr, y));
      fprintf(ppf, ")@]");
      return;
    }
    case EK::Ctrywith: {
      auto* x = static_cast<const Ctrywith*>(e);
      fprintf(ppf, "@[<2>(try@ %a@;<1 -2>with@ %a@ %a)@]", pr(sequence, x->body), pr(vp, x->exn),
              pr(sequence, x->handler));
      return;
    }
  }
}

void sequence(Formatter& ppf, Expr e) {
  if (auto* s = as<Csequence>(e)) {
    fprintf(ppf, "%a@ %a", pr(sequence, s->e1), pr(sequence, s->e2));
    return;
  }
  fprintf(ppf, "%a", pr(expr, e));
}

}  // namespace

void machtype(Formatter& ppf, Machtype mty) {
  if (mty.empty()) {
    fprintf(ppf, "unit");
    return;
  }
  machtype_component(ppf, mty[0]);
  for (std::size_t i = 1; i < mty.size(); ++i) fprintf(ppf, "*%a", pr(machtype_component, mty[i]));
}

void print_expression(Formatter& ppf, Expr e) { fprintf(ppf, "%a", pr(expr, e)); }

void fundecl(Formatter& ppf, const Fundecl& f) {
  auto print_cases = [&f](Formatter& g) {
    bool first = true;
    for (const CatchParam& p : f.fun_args) {
      if (first) first = false;
      else fprintf(g, "@ ");
      fprintf(g, "%a: %a", pr(vp, p.id), pr(machtype, p.ty));
    }
  };
  fprintf(ppf, "@[<1>(function%s %s@;<1 4>@[<1>(%t)@]@ @[%a@])@]@.", location(f.fun_dbg), f.fun_name, print_cases,
          pr(sequence, f.fun_body));
}

namespace {
void data_item(Formatter& ppf, const DataItem& d) {
  using DK = DataItem::K;
  switch (d.kind) {
    case DK::Cdefine_symbol: fprintf(ppf, "\"%s\":", d.s); return;
    case DK::Cglobal_symbol: fprintf(ppf, "global \"%s\"", d.s); return;
    case DK::Cint8: fprintf(ppf, "byte %i", static_cast<long>(d.n)); return;
    case DK::Cint16: fprintf(ppf, "int16 %i", static_cast<long>(d.n)); return;
    case DK::Cint32: fprintf(ppf, "int32 %s", std::to_string(d.n)); return;
    case DK::Cint: fprintf(ppf, "int %s", std::to_string(d.n)); return;
    case DK::Csingle: fprintf(ppf, "single %s", printclambda::float_F(d.f)); return;
    case DK::Cdouble: fprintf(ppf, "double %s", printclambda::float_F(d.f)); return;
    case DK::Csymbol_address: fprintf(ppf, "addr \"%s\"", d.s); return;
    case DK::Cstring: fprintf(ppf, "string \"%s\"", d.s); return;
    case DK::Cskip: fprintf(ppf, "skip %i", static_cast<long>(d.n)); return;
    case DK::Calign: fprintf(ppf, "align %i", static_cast<long>(d.n)); return;
  }
}
}  // namespace

void data(Formatter& ppf, const std::vector<DataItem>& dl) {
  auto items = [&dl](Formatter& f) {
    for (const DataItem& d : dl) fprintf(f, "@ %a", pr(data_item, d));
  };
  fprintf(ppf, "@[<hv 1>(data%t)@]", items);
}

void phrase(Formatter& ppf, const Phrase& p) {
  if (p.fn) fundecl(ppf, *p.fn);
  else data(ppf, p.data);
}

}  // namespace cppcaml::typing::printcmm
