// Port of lambda/printlambda.ml (cxx/PORTING.md stage 10).  See
// printlambda.hpp.
#include "cppcaml/typing/printlambda.hpp"

#include <functional>
#include <string>

#include "cppcaml/lexer.hpp"
#include "cppcaml/typing/path.hpp"

namespace cppcaml::typing::printlambda {

using format::Formatter;
using format::fprintf;
using lambda::ValueKind;
using lambda::StructuredConstant;
using lambda::ArrayKind;
using lambda::BigarrayKind;
using lambda::BigarrayLayout;
using lambda::InitializationOrAssignment;
using lambda::ImmediateOrPointer;
using lambda::BlockShape;
using lambda::IntegerComparison;
using lambda::FloatComparison;
using lambda::Primitive;
using lambda::LazyBlockTag;
using lambda::IsSafe;
using lambda::CompileTimeConstant;
using lambda::FunctionAttribute;
using lambda::InlineAttribute;
using lambda::SpecialiseAttribute;
using lambda::LocalAttribute;
using lambda::PollAttribute;
using lambda::TailcallAttribute;
using lambda::LFunction;
using lambda::LK;
using lambda::Llet;
using lambda::Lmutlet;
using lambda::Lvar;
using lambda::Lmutvar;
using lambda::Lconst;
using lambda::Lapply;
using lambda::Lfunction;
using lambda::Lletrec;
using lambda::Lprim;
using lambda::Lswitch;
using lambda::Lstringswitch;
using lambda::Lstaticraise;
using lambda::Lstaticcatch;
using lambda::Ltrywith;
using lambda::Lifthenelse;
using lambda::Lsequence;
using lambda::Lwhile;
using lambda::Lfor;
using lambda::Lassign;
using lambda::Lsend;
using lambda::Levent;
using lambda::Lifused;
using lambda::LambdaApply;
using lambda::RecBinding;
using lambda::LambdaSwitch;
using lambda::SwitchCase;
using lambda::StringCase;
using lambda::Param;
using lambda::LetKind;
using lambda::MethKind;
using lambda::FunctionKind;
using lambda::EventKind;
using lambda::LambdaEvent;
using lambda::Program;
using lambda::as;
using lambda::equal_value_kind;

namespace {

// ---- Ident.print (Format_doc.compat doc_print: ~with_scope:false) ----------
// pp_stamped, with Clflags.canonical_ids off
void pp_stamped(Formatter& ppf, std::string_view name, int stamp) {
  if (!flags::unique_ids)
    fprintf(ppf, "%s", name);
  else
    fprintf(ppf, "%s/%i", name, stamp);
}

}  // namespace

void ident(Formatter& ppf, Ident::t id) {
  switch (id->kind) {
    case Ident::Kind::Global:
      fprintf(ppf, "%s!", id->name_);
      return;
    case Ident::Kind::Predef:
      pp_stamped(ppf, id->name_, id->stamp_);
      fprintf(ppf, "!");
      return;
    case Ident::Kind::Local:
      pp_stamped(ppf, id->name_, id->stamp_);
      return;
    case Ident::Kind::Unscoped: {
      auto d = ident::Unscoped::get_desc(id->us);
      fprintf(ppf, "U");
      pp_stamped(ppf, d.name, d.stamp);
      return;
    }
    case Ident::Kind::Scoped:
      pp_stamped(ppf, id->name_, id->stamp_);
      fprintf(ppf, "%s", "");
      return;
  }
}

namespace {

void struct_const(Formatter& ppf, const StructuredConstant* c) {
  using SK = StructuredConstant::Kind;
  switch (c->kind) {
    case SK::Const_int: fprintf(ppf, "%i", c->i); return;
    case SK::Const_char: fprintf(ppf, "%C", static_cast<char>(c->i)); return;
    case SK::Const_immstring: fprintf(ppf, "%S", c->s); return;
    case SK::Const_float: fprintf(ppf, "%s", c->s); return;
    case SK::Const_int32: fprintf(ppf, "%lil", static_cast<std::int32_t>(c->boxed)); return;
    case SK::Const_int64: fprintf(ppf, "%LiL", c->boxed); return;
    case SK::Const_nativeint: fprintf(ppf, "%nin", c->boxed); return;
    case SK::Const_block: {
      if (c->fields.empty()) {
        fprintf(ppf, "[%i]", c->i);
        return;
      }
      auto sconsts = [c](Formatter& ppf) {
        bool first = true;
        for (const StructuredConstant* sc : c->fields) {
          if (first) {
            first = false;
            continue;
          }
          fprintf(ppf, "@ %a", format::pr(struct_const, sc));
        }
      };
      fprintf(ppf, "@[<1>[%i:@ @[%a%a@]]@]", c->i, format::pr(struct_const, c->fields[0]), sconsts);
      return;
    }
    case SK::Const_float_array: {
      if (c->floats.empty()) {
        fprintf(ppf, "[| |]");
        return;
      }
      auto floats = [c](Formatter& ppf) {
        bool first = true;
        for (std::string_view f : c->floats) {
          if (first) {
            first = false;
            continue;
          }
          fprintf(ppf, "@ %s", f);
        }
      };
      fprintf(ppf, "@[<1>[|@[%s%a@]|]@]", c->floats[0], floats);
      return;
    }
  }
}

std::string_view array_kind(ArrayKind k) {
  switch (k) {
    case ArrayKind::Pgenarray: return "gen";
    case ArrayKind::Paddrarray: return "addr";
    case ArrayKind::Pintarray: return "int";
    case ArrayKind::Pfloatarray: return "float";
  }
  return "";
}

std::string_view boxed_integer_name(BoxedInteger bi) {
  switch (bi) {
    case BoxedInteger::Pnativeint: return "nativeint";
    case BoxedInteger::Pint32: return "int32";
    case BoxedInteger::Pint64: return "int64";
  }
  return "";
}

void return_kind(Formatter& ppf, const ValueKind& k) {
  switch (k.kind) {
    case ValueKind::Kind::Pgenval: return;
    case ValueKind::Kind::Pintval: fprintf(ppf, ": int@ "); return;
    case ValueKind::Kind::Pfloatval: fprintf(ppf, ": float@ "); return;
    case ValueKind::Kind::Pboxedintval: fprintf(ppf, ": %s@ ", boxed_integer_name(k.bi)); return;
  }
}

std::string_view field_kind(const ValueKind& k) {
  switch (k.kind) {
    case ValueKind::Kind::Pgenval: return "*";
    case ValueKind::Kind::Pintval: return "int";
    case ValueKind::Kind::Pfloatval: return "float";
    case ValueKind::Kind::Pboxedintval: return boxed_integer_name(k.bi);
  }
  return "";
}

void print_boxed_integer_conversion(Formatter& ppf, BoxedInteger bi1, BoxedInteger bi2) {
  fprintf(ppf, "%s_of_%s", boxed_integer_name(bi2), boxed_integer_name(bi1));
}

std::string boxed_integer_mark(std::string_view name, BoxedInteger bi) {
  switch (bi) {
    case BoxedInteger::Pnativeint: return "Nativeint." + std::string(name);
    case BoxedInteger::Pint32: return "Int32." + std::string(name);
    case BoxedInteger::Pint64: return "Int64." + std::string(name);
  }
  return "";
}

void print_boxed_integer(std::string_view name, Formatter& ppf, BoxedInteger bi) {
  fprintf(ppf, "%s", boxed_integer_mark(name, bi));
}

}  // namespace

void print_bigarray(std::string_view name, bool unsafe, BigarrayKind kind, Formatter& ppf, BigarrayLayout layout) {
  std::string_view k;
  switch (kind) {
    case BigarrayKind::Pbigarray_unknown: k = "generic"; break;
    case BigarrayKind::Pbigarray_float16: k = "float16"; break;
    case BigarrayKind::Pbigarray_float32: k = "float32"; break;
    case BigarrayKind::Pbigarray_float64: k = "float64"; break;
    case BigarrayKind::Pbigarray_sint8: k = "sint8"; break;
    case BigarrayKind::Pbigarray_uint8: k = "uint8"; break;
    case BigarrayKind::Pbigarray_sint16: k = "sint16"; break;
    case BigarrayKind::Pbigarray_uint16: k = "uint16"; break;
    case BigarrayKind::Pbigarray_int32: k = "int32"; break;
    case BigarrayKind::Pbigarray_int64: k = "int64"; break;
    case BigarrayKind::Pbigarray_caml_int: k = "camlint"; break;
    case BigarrayKind::Pbigarray_native_int: k = "nativeint"; break;
    case BigarrayKind::Pbigarray_complex32: k = "complex32"; break;
    case BigarrayKind::Pbigarray_complex64: k = "complex64"; break;
  }
  std::string_view l;
  switch (layout) {
    case BigarrayLayout::Pbigarray_unknown_layout: l = "unknown"; break;
    case BigarrayLayout::Pbigarray_c_layout: l = "C"; break;
    case BigarrayLayout::Pbigarray_fortran_layout: l = "Fortran"; break;
  }
  std::string n = unsafe ? "unsafe_" + std::string(name) : std::string(name);
  fprintf(ppf, "Bigarray.%s[%s,%s]", n, k, l);
}

namespace {

std::string_view init_name(InitializationOrAssignment init) {
  switch (init) {
    case InitializationOrAssignment::Heap_initialization: return "(heap-init)";
    case InitializationOrAssignment::Root_initialization: return "(root-init)";
    case InitializationOrAssignment::Assignment: return "";
  }
  return "";
}

std::string_view ptr_name(ImmediateOrPointer p) {
  switch (p) {
    case ImmediateOrPointer::Pointer: return "ptr";
    case ImmediateOrPointer::Immediate: return "imm";
  }
  return "";
}

}  // namespace

void value_kind(Formatter& ppf, const ValueKind& k) {
  switch (k.kind) {
    case ValueKind::Kind::Pgenval: return;
    case ValueKind::Kind::Pintval: fprintf(ppf, "[int]"); return;
    case ValueKind::Kind::Pfloatval: fprintf(ppf, "[float]"); return;
    case ValueKind::Kind::Pboxedintval: fprintf(ppf, "[%s]", boxed_integer_name(k.bi)); return;
  }
}

void record_rep(Formatter& ppf, const RecordRepresentation& r) {
  using RK = RecordRepresentation::Kind;
  switch (r.kind) {
    case RK::Record_regular: fprintf(ppf, "regular"); return;
    case RK::Record_inlined: fprintf(ppf, "inlined(%i)", r.inlined_tag); return;
    case RK::Record_unboxed:
      if (!r.unboxed_inlined)
        fprintf(ppf, "unboxed");
      else
        fprintf(ppf, "inlined(unboxed)");
      return;
    case RK::Record_float: fprintf(ppf, "float"); return;
    case RK::Record_extension: {
      Path::t p = r.extension;
      fprintf(ppf, "ext(%a)", [p](Formatter& ppf) { print_path(ppf, p); });
      return;
    }
  }
}

void block_shape(Formatter& ppf, const BlockShape& shape) {
  if (!shape.some || shape.kinds.empty()) return;
  bool all_gen = true;
  for (const ValueKind& k : shape.kinds)
    if (!equal_value_kind(k, ValueKind::gen())) all_gen = false;
  if (all_gen) return;
  if (shape.kinds.size() == 1) {
    fprintf(ppf, " (%s)", field_kind(shape.kinds[0]));
    return;
  }
  fprintf(ppf, " (%s", field_kind(shape.kinds[0]));
  bool first = true;
  for (const ValueKind& elt : shape.kinds) {
    if (first) {
      first = false;
      continue;
    }
    fprintf(ppf, ",%s", field_kind(elt));
  }
  fprintf(ppf, ")");
}

void integer_comparison(Formatter& ppf, IntegerComparison c) {
  switch (c) {
    case IntegerComparison::Ceq: fprintf(ppf, "=="); return;
    case IntegerComparison::Cne: fprintf(ppf, "!="); return;
    case IntegerComparison::Clt: fprintf(ppf, "<"); return;
    case IntegerComparison::Cle: fprintf(ppf, "<="); return;
    case IntegerComparison::Cgt: fprintf(ppf, ">"); return;
    case IntegerComparison::Cge: fprintf(ppf, ">="); return;
  }
}

void float_comparison(Formatter& ppf, FloatComparison c) {
  switch (c) {
    case FloatComparison::CFeq: fprintf(ppf, "==."); return;
    case FloatComparison::CFneq: fprintf(ppf, "!=."); return;
    case FloatComparison::CFlt: fprintf(ppf, "<."); return;
    case FloatComparison::CFnlt: fprintf(ppf, "!<."); return;
    case FloatComparison::CFle: fprintf(ppf, "<=."); return;
    case FloatComparison::CFnle: fprintf(ppf, "!<=."); return;
    case FloatComparison::CFgt: fprintf(ppf, ">."); return;
    case FloatComparison::CFngt: fprintf(ppf, "!>."); return;
    case FloatComparison::CFge: fprintf(ppf, ">=."); return;
    case FloatComparison::CFnge: fprintf(ppf, "!>=."); return;
  }
}

void primitive(Formatter& ppf, const Primitive& p) {
  using K = Primitive::K;
  auto pbi = [&](std::string_view name) { print_boxed_integer(name, ppf, p.bi); };
  auto un = [&](std::string_view unsafe_name, std::string_view safe_name) {
    if (p.unsafe)
      fprintf(ppf, "%s", unsafe_name);
    else
      fprintf(ppf, "%s", safe_name);
  };
  switch (p.kind) {
    case K::Pbytes_to_string: fprintf(ppf, "bytes_to_string"); return;
    case K::Pbytes_of_string: fprintf(ppf, "bytes_of_string"); return;
    case K::Pignore: fprintf(ppf, "ignore"); return;
    case K::Pgetglobal: fprintf(ppf, "global %a", format::pr(ident, p.id)); return;
    case K::Psetglobal: fprintf(ppf, "setglobal %a", format::pr(ident, p.id)); return;
    case K::Pmakeblock:
      if (p.mut == MutableFlag::Immutable)
        fprintf(ppf, "makeblock %i%a", p.n, format::pr(block_shape, p.shape));
      else
        fprintf(ppf, "makemutable %i%a", p.n, format::pr(block_shape, p.shape));
      return;
    case K::Pmakelazyblock:
      if (p.lazy_tag == LazyBlockTag::Lazy_tag)
        fprintf(ppf, "makelazyblock");
      else
        fprintf(ppf, "makeforwardblock");
      return;
    case K::Pfield: {
      std::string_view instr = p.ptr == ImmediateOrPointer::Immediate ? "field_int "
                               : p.mut == MutableFlag::Mutable        ? "field_mut "
                                                                      : "field_imm ";
      fprintf(ppf, "%s%i", instr, p.n);
      return;
    }
    case K::Pfield_computed: fprintf(ppf, "field_computed"); return;
    case K::Psetfield: fprintf(ppf, "setfield_%s%s %i", ptr_name(p.ptr), init_name(p.init), p.n); return;
    case K::Psetfield_computed:
      fprintf(ppf, "setfield_%s%s_computed", ptr_name(p.ptr), init_name(p.init));
      return;
    case K::Pfloatfield: fprintf(ppf, "floatfield %i", p.n); return;
    case K::Psetfloatfield: fprintf(ppf, "setfloatfield%s %i", init_name(p.init), p.n); return;
    case K::Pduprecord: {
      const RecordRepresentation& rep = p.repr;
      fprintf(ppf, "duprecord %a %i", [&rep](Formatter& ppf) { record_rep(ppf, rep); }, p.n);
      return;
    }
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
    case K::Pdivint:
      if (p.safe == IsSafe::Safe)
        fprintf(ppf, "/");
      else
        fprintf(ppf, "/u");
      return;
    case K::Pmodint:
      if (p.safe == IsSafe::Safe)
        fprintf(ppf, "mod");
      else
        fprintf(ppf, "mod_unsafe");
      return;
    case K::Pandint: fprintf(ppf, "and"); return;
    case K::Porint: fprintf(ppf, "or"); return;
    case K::Pxorint: fprintf(ppf, "xor"); return;
    case K::Plslint: fprintf(ppf, "lsl"); return;
    case K::Plsrint: fprintf(ppf, "lsr"); return;
    case K::Pasrint: fprintf(ppf, "asr"); return;
    case K::Pintcomp: integer_comparison(ppf, p.icmp); return;
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
    case K::Pfloatcomp: float_comparison(ppf, p.fcmp); return;
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
      if (p.mut == MutableFlag::Mutable)
        fprintf(ppf, "makearray[%s]", array_kind(p.array));
      else
        fprintf(ppf, "makearray_imm[%s]", array_kind(p.array));
      return;
    case K::Pduparray:
      if (p.mut == MutableFlag::Mutable)
        fprintf(ppf, "duparray[%s]", array_kind(p.array));
      else
        fprintf(ppf, "duparray_imm[%s]", array_kind(p.array));
      return;
    case K::Parrayrefu: fprintf(ppf, "array.unsafe_get[%s]", array_kind(p.array)); return;
    case K::Parraysetu: fprintf(ppf, "array.unsafe_set[%s]", array_kind(p.array)); return;
    case K::Parrayrefs: fprintf(ppf, "array.get[%s]", array_kind(p.array)); return;
    case K::Parraysets: fprintf(ppf, "array.set[%s]", array_kind(p.array)); return;
    case K::Pctconst: {
      std::string_view const_name;
      switch (p.ctconst) {
        case CompileTimeConstant::Big_endian: const_name = "big_endian"; break;
        case CompileTimeConstant::Word_size: const_name = "word_size"; break;
        case CompileTimeConstant::Int_size: const_name = "int_size"; break;
        case CompileTimeConstant::Max_wosize: const_name = "max_wosize"; break;
        case CompileTimeConstant::Ostype_unix: const_name = "ostype_unix"; break;
        case CompileTimeConstant::Ostype_win32: const_name = "ostype_win32"; break;
        case CompileTimeConstant::Ostype_cygwin: const_name = "ostype_cygwin"; break;
        case CompileTimeConstant::Backend_type: const_name = "backend_type"; break;
        case CompileTimeConstant::Standard_library_default: const_name = "standard_library_default"; break;
      }
      fprintf(ppf, "sys.constant_%s", const_name);
      return;
    }
    case K::Pisint: fprintf(ppf, "isint"); return;
    case K::Pisout: fprintf(ppf, "isout"); return;
    case K::Pbintofint: pbi("of_int"); return;
    case K::Pintofbint: pbi("to_int"); return;
    case K::Pcvtbint: print_boxed_integer_conversion(ppf, p.bi, p.bi2); return;
    case K::Pnegbint: pbi("neg"); return;
    case K::Paddbint: pbi("add"); return;
    case K::Psubbint: pbi("sub"); return;
    case K::Pmulbint: pbi("mul"); return;
    case K::Pdivbint: pbi(p.safe == IsSafe::Safe ? "div" : "div_unsafe"); return;
    case K::Pmodbint: pbi(p.safe == IsSafe::Safe ? "mod" : "mod_unsafe"); return;
    case K::Pandbint: pbi("and"); return;
    case K::Porbint: pbi("or"); return;
    case K::Pxorbint: pbi("xor"); return;
    case K::Plslbint: pbi("lsl"); return;
    case K::Plsrbint: pbi("lsr"); return;
    case K::Pasrbint: pbi("asr"); return;
    case K::Pbintcomp:
      switch (p.icmp) {
        case IntegerComparison::Ceq: pbi("=="); return;
        case IntegerComparison::Cne: pbi("!="); return;
        case IntegerComparison::Clt: pbi("<"); return;
        case IntegerComparison::Cgt: pbi(">"); return;
        case IntegerComparison::Cle: pbi("<="); return;
        case IntegerComparison::Cge: pbi(">="); return;
      }
      return;
    case K::Pbigarrayref: print_bigarray("get", p.unsafe, p.ba_kind, ppf, p.ba_layout); return;
    case K::Pbigarrayset: print_bigarray("set", p.unsafe, p.ba_kind, ppf, p.ba_layout); return;
    case K::Pbigarraydim: fprintf(ppf, "Bigarray.dim_%i", p.n); return;
    case K::Pstring_load_16: un("string.unsafe_get16", "string.get16"); return;
    case K::Pstring_load_32: un("string.unsafe_get32", "string.get32"); return;
    case K::Pstring_load_64: un("string.unsafe_get64", "string.get64"); return;
    case K::Pbytes_load_16: un("bytes.unsafe_get16", "bytes.get16"); return;
    case K::Pbytes_load_32: un("bytes.unsafe_get32", "bytes.get32"); return;
    case K::Pbytes_load_64: un("bytes.unsafe_get64", "bytes.get64"); return;
    case K::Pbytes_set_16: un("bytes.unsafe_set16", "bytes.set16"); return;
    case K::Pbytes_set_32: un("bytes.unsafe_set32", "bytes.set32"); return;
    case K::Pbytes_set_64: un("bytes.unsafe_set64", "bytes.set64"); return;
    case K::Pbigstring_load_16: un("bigarray.array1.unsafe_get16", "bigarray.array1.get16"); return;
    case K::Pbigstring_load_32: un("bigarray.array1.unsafe_get32", "bigarray.array1.get32"); return;
    case K::Pbigstring_load_64: un("bigarray.array1.unsafe_get64", "bigarray.array1.get64"); return;
    case K::Pbigstring_set_16: un("bigarray.array1.unsafe_set16", "bigarray.array1.set16"); return;
    case K::Pbigstring_set_32: un("bigarray.array1.unsafe_set32", "bigarray.array1.set32"); return;
    case K::Pbigstring_set_64: un("bigarray.array1.unsafe_set64", "bigarray.array1.set64"); return;
    case K::Pbswap16: fprintf(ppf, "bswap16"); return;
    case K::Pbbswap: pbi("bswap"); return;
    case K::Pint_as_pointer: fprintf(ppf, "int_as_pointer"); return;
    case K::Patomic_load: fprintf(ppf, "atomic_load"); return;
    case K::Popaque: fprintf(ppf, "opaque"); return;
    case K::Pdls_get: fprintf(ppf, "dls_get"); return;
    case K::Ppoll: fprintf(ppf, "poll"); return;
  }
}

std::string name_of_primitive(const Primitive& p) {
  using K = Primitive::K;
  switch (p.kind) {
    case K::Pbytes_of_string: return "Pbytes_of_string";
    case K::Pbytes_to_string: return "Pbytes_to_string";
    case K::Pignore: return "Pignore";
    case K::Pgetglobal: return "Pgetglobal";
    case K::Psetglobal: return "Psetglobal";
    case K::Pmakeblock: return "Pmakeblock";
    case K::Pmakelazyblock: return "Pmakelazyblock";
    case K::Pfield: return "Pfield";
    case K::Pfield_computed: return "Pfield_computed";
    case K::Psetfield: return "Psetfield";
    case K::Psetfield_computed: return "Psetfield_computed";
    case K::Pfloatfield: return "Pfloatfield";
    case K::Psetfloatfield: return "Psetfloatfield";
    case K::Pduprecord: return "Pduprecord";
    case K::Pccall: return "Pccall";
    case K::Praise: return "Praise";
    case K::Psequand: return "Psequand";
    case K::Psequor: return "Psequor";
    case K::Pnot: return "Pnot";
    case K::Pnegint: return "Pnegint";
    case K::Paddint: return "Paddint";
    case K::Psubint: return "Psubint";
    case K::Pmulint: return "Pmulint";
    case K::Pdivint: return "Pdivint";
    case K::Pmodint: return "Pmodint";
    case K::Pandint: return "Pandint";
    case K::Porint: return "Porint";
    case K::Pxorint: return "Pxorint";
    case K::Plslint: return "Plslint";
    case K::Plsrint: return "Plsrint";
    case K::Pasrint: return "Pasrint";
    case K::Pintcomp: return "Pintcomp";
    case K::Pcompare_ints: return "Pcompare_ints";
    case K::Pcompare_floats: return "Pcompare_floats";
    case K::Pcompare_bints: return "Pcompare";
    case K::Poffsetint: return "Poffsetint";
    case K::Poffsetref: return "Poffsetref";
    case K::Pintoffloat: return "Pintoffloat";
    case K::Pfloatofint: return "Pfloatofint";
    case K::Pnegfloat: return "Pnegfloat";
    case K::Pabsfloat: return "Pabsfloat";
    case K::Paddfloat: return "Paddfloat";
    case K::Psubfloat: return "Psubfloat";
    case K::Pmulfloat: return "Pmulfloat";
    case K::Pdivfloat: return "Pdivfloat";
    case K::Pfloatcomp: return "Pfloatcomp";
    case K::Pstringlength: return "Pstringlength";
    case K::Pstringrefu: return "Pstringrefu";
    case K::Pstringrefs: return "Pstringrefs";
    case K::Pbyteslength: return "Pbyteslength";
    case K::Pbytesrefu: return "Pbytesrefu";
    case K::Pbytessetu: return "Pbytessetu";
    case K::Pbytesrefs: return "Pbytesrefs";
    case K::Pbytessets: return "Pbytessets";
    case K::Parraylength: return "Parraylength";
    case K::Pmakearray: return "Pmakearray";
    case K::Pduparray: return "Pduparray";
    case K::Parrayrefu: return "Parrayrefu";
    case K::Parraysetu: return "Parraysetu";
    case K::Parrayrefs: return "Parrayrefs";
    case K::Parraysets: return "Parraysets";
    case K::Pctconst: return "Pctconst";
    case K::Pisint: return "Pisint";
    case K::Pisout: return "Pisout";
    case K::Pbintofint: return "Pbintofint";
    case K::Pintofbint: return "Pintofbint";
    case K::Pcvtbint: return "Pcvtbint";
    case K::Pnegbint: return "Pnegbint";
    case K::Paddbint: return "Paddbint";
    case K::Psubbint: return "Psubbint";
    case K::Pmulbint: return "Pmulbint";
    case K::Pdivbint: return "Pdivbint";
    case K::Pmodbint: return "Pmodbint";
    case K::Pandbint: return "Pandbint";
    case K::Porbint: return "Porbint";
    case K::Pxorbint: return "Pxorbint";
    case K::Plslbint: return "Plslbint";
    case K::Plsrbint: return "Plsrbint";
    case K::Pasrbint: return "Pasrbint";
    case K::Pbintcomp: return "Pbintcomp";
    case K::Pbigarrayref: return "Pbigarrayref";
    case K::Pbigarrayset: return "Pbigarrayset";
    case K::Pbigarraydim: return "Pbigarraydim";
    case K::Pstring_load_16: return "Pstring_load_16";
    case K::Pstring_load_32: return "Pstring_load_32";
    case K::Pstring_load_64: return "Pstring_load_64";
    case K::Pbytes_load_16: return "Pbytes_load_16";
    case K::Pbytes_load_32: return "Pbytes_load_32";
    case K::Pbytes_load_64: return "Pbytes_load_64";
    case K::Pbytes_set_16: return "Pbytes_set_16";
    case K::Pbytes_set_32: return "Pbytes_set_32";
    case K::Pbytes_set_64: return "Pbytes_set_64";
    case K::Pbigstring_load_16: return "Pbigstring_load_16";
    case K::Pbigstring_load_32: return "Pbigstring_load_32";
    case K::Pbigstring_load_64: return "Pbigstring_load_64";
    case K::Pbigstring_set_16: return "Pbigstring_set_16";
    case K::Pbigstring_set_32: return "Pbigstring_set_32";
    case K::Pbigstring_set_64: return "Pbigstring_set_64";
    case K::Pbswap16: return "Pbswap16";
    case K::Pbbswap: return "Pbbswap";
    case K::Pint_as_pointer: return "Pint_as_pointer";
    case K::Patomic_load: return "Patomic_load";
    case K::Popaque: return "Popaque";
    case K::Prunstack: return "Prunstack";
    case K::Presume: return "Presume";
    case K::Pperform: return "Pperform";
    case K::Preperform: return "Preperform";
    case K::Pdls_get: return "Pdls_get";
    case K::Ppoll: return "Ppoll";
  }
  return "";
}

namespace {

void function_attribute(Formatter& ppf, const FunctionAttribute& t) {
  if (t.is_a_functor) fprintf(ppf, "is_a_functor@ ");
  if (t.stub) fprintf(ppf, "stub@ ");
  switch (t.inline_.kind) {
    case InlineAttribute::Kind::Default_inline: break;
    case InlineAttribute::Kind::Always_inline: fprintf(ppf, "always_inline@ "); break;
    case InlineAttribute::Kind::Hint_inline: fprintf(ppf, "hint_inline@ "); break;
    case InlineAttribute::Kind::Never_inline: fprintf(ppf, "never_inline@ "); break;
    case InlineAttribute::Kind::Unroll: fprintf(ppf, "unroll(%i)@ ", t.inline_.unroll); break;
  }
  switch (t.specialise) {
    case SpecialiseAttribute::Default_specialise: break;
    case SpecialiseAttribute::Always_specialise: fprintf(ppf, "always_specialise@ "); break;
    case SpecialiseAttribute::Never_specialise: fprintf(ppf, "never_specialise@ "); break;
  }
  switch (t.local) {
    case LocalAttribute::Default_local: break;
    case LocalAttribute::Always_local: fprintf(ppf, "always_local@ "); break;
    case LocalAttribute::Never_local: fprintf(ppf, "never_local@ "); break;
  }
  if (t.tmc_candidate) fprintf(ppf, "tail_mod_cons@ ");
  switch (t.poll) {
    case PollAttribute::Default_poll: break;
    case PollAttribute::Error_poll: fprintf(ppf, "error_poll@ "); break;
  }
}

void apply_tailcall_attribute(Formatter& ppf, TailcallAttribute a) {
  switch (a) {
    case TailcallAttribute::Default_tailcall: return;
    case TailcallAttribute::Tailcall_expectation_true: fprintf(ppf, " tailcall"); return;
    case TailcallAttribute::Tailcall_expectation_false: fprintf(ppf, " tailcall(false)"); return;
  }
}

void apply_inlined_attribute(Formatter& ppf, const InlineAttribute& a) {
  switch (a.kind) {
    case InlineAttribute::Kind::Default_inline: return;
    case InlineAttribute::Kind::Always_inline: fprintf(ppf, " always_inline"); return;
    case InlineAttribute::Kind::Never_inline: fprintf(ppf, " never_inline"); return;
    case InlineAttribute::Kind::Hint_inline: fprintf(ppf, " hint_inline"); return;
    case InlineAttribute::Kind::Unroll: fprintf(ppf, " never_inline(%i)", a.unroll); return;
  }
}

void apply_specialised_attribute(Formatter& ppf, SpecialiseAttribute a) {
  switch (a) {
    case SpecialiseAttribute::Default_specialise: return;
    case SpecialiseAttribute::Always_specialise: fprintf(ppf, " always_specialise"); return;
    case SpecialiseAttribute::Never_specialise: fprintf(ppf, " never_specialise"); return;
  }
}

void lam(Formatter& ppf, lambda::lambda l);
void sequence(Formatter& ppf, lambda::lambda l);
void lfunction(Formatter& ppf, const LFunction* f);

// `List.iter (fun l -> fprintf ppf "@ %a" lam l) largs`
auto lams(Slice<lambda::lambda> largs) {
  return [largs](Formatter& ppf) {
    for (lambda::lambda l : largs) fprintf(ppf, "@ %a", format::pr(lam, l));
  };
}

std::string_view let_kind(lambda::lambda l) {
  if (auto* ll = as<Llet>(l)) {
    switch (ll->str) {
      case LetKind::Alias: return "a";
      case LetKind::Strict: return "";
      case LetKind::StrictOpt: return "o";
    }
  }
  if (as<Lmutlet>(l)) return "mut";
  return "";
}

void lam(Formatter& ppf, lambda::lambda l) {
  switch (l->kind) {
    case LK::Lvar:
      ident(ppf, as<Lvar>(l)->id);
      return;
    case LK::Lmutvar:
      fprintf(ppf, "*%a", format::pr(ident, as<Lmutvar>(l)->id));
      return;
    case LK::Lconst:
      struct_const(ppf, as<Lconst>(l)->c);
      return;
    case LK::Lapply: {
      const LambdaApply& ap = as<Lapply>(l)->ap;
      fprintf(ppf, "@[<2>(apply@ %a%a%a%a%a)@]", format::pr(lam, ap.ap_func), lams(ap.ap_args),
              format::pr(apply_tailcall_attribute, ap.ap_tailcall),
              format::pr(apply_inlined_attribute, ap.ap_inlined),
              format::pr(apply_specialised_attribute, ap.ap_specialised));
      return;
    }
    case LK::Lfunction:
      lfunction(ppf, as<Lfunction>(l)->f);
      return;
    case LK::Llet:
    case LK::Lmutlet: {
      auto parts = [](lambda::lambda l, ValueKind& k, Ident::t& id, lambda::lambda& arg, lambda::lambda& body) {
        if (auto* ll = as<Llet>(l)) {
          k = ll->k, id = ll->id, arg = ll->arg, body = ll->body;
          return true;
        }
        if (auto* ml = as<Lmutlet>(l)) {
          k = ml->k, id = ml->id, arg = ml->arg, body = ml->body;
          return true;
        }
        return false;
      };
      ValueKind k;
      Ident::t id;
      lambda::lambda arg, body;
      parts(l, k, id, arg, body);
      fprintf(ppf, "@[<2>(let@ @[<hv 1>(@[<2>%a =%s%a@ %a@]", format::pr(ident, id), let_kind(l),
              format::pr(value_kind, k), format::pr(lam, arg));
      // letbody
      lambda::lambda expr = body;
      for (;;) {
        ValueKind k2;
        Ident::t id2;
        lambda::lambda arg2, body2;
        if (!parts(expr, k2, id2, arg2, body2)) break;
        fprintf(ppf, "@ @[<2>%a =%s%a@ %a@]", format::pr(ident, id2), let_kind(expr),
                format::pr(value_kind, k2), format::pr(lam, arg2));
        expr = body2;
      }
      fprintf(ppf, ")@]@ %a)@]", format::pr(lam, expr));
      return;
    }
    case LK::Lletrec: {
      auto* lr = as<Lletrec>(l);
      auto bindings = [lr](Formatter& ppf) {
        bool spc = false;
        for (const RecBinding& b : lr->decl) {
          if (spc)
            fprintf(ppf, "@ ");
          else
            spc = true;
          fprintf(ppf, "@[<2>%a@ %a@]", format::pr(ident, b.id), format::pr(lfunction, b.def));
        }
      };
      fprintf(ppf, "@[<2>(letrec@ (@[<hv 1>%a@])@ %a)@]", bindings, format::pr(lam, lr->body));
      return;
    }
    case LK::Lprim: {
      auto* lp = as<Lprim>(l);
      const Primitive& prim = lp->p;
      fprintf(ppf, "@[<2>(%a%a)@]", [&prim](Formatter& ppf) { primitive(ppf, prim); }, lams(lp->args));
      return;
    }
    case LK::Lswitch: {
      auto* ls = as<Lswitch>(l);
      const LambdaSwitch& sw = ls->sw;
      auto switch_ = [&sw](Formatter& ppf) {
        bool spc = false;
        for (const SwitchCase& c : sw.sw_consts) {
          if (spc)
            fprintf(ppf, "@ ");
          else
            spc = true;
          fprintf(ppf, "@[<hv 1>case int %i:@ %a@]", c.key, format::pr(lam, c.action));
        }
        for (const SwitchCase& c : sw.sw_blocks) {
          if (spc)
            fprintf(ppf, "@ ");
          else
            spc = true;
          fprintf(ppf, "@[<hv 1>case tag %i:@ %a@]", c.key, format::pr(lam, c.action));
        }
        if (sw.sw_failaction) {
          if (spc)
            fprintf(ppf, "@ ");
          else
            spc = true;
          fprintf(ppf, "@[<hv 1>default:@ %a@]", format::pr(lam, sw.sw_failaction));
        }
      };
      fprintf(ppf, "@[<1>(%s %a@ @[<v 0>%a@])@]", sw.sw_failaction ? "switch" : "switch*",
              format::pr(lam, ls->arg), switch_);
      return;
    }
    case LK::Lstringswitch: {
      auto* ls = as<Lstringswitch>(l);
      auto switch_ = [ls](Formatter& ppf) {
        bool spc = false;
        for (const StringCase& c : ls->cases) {
          if (spc)
            fprintf(ppf, "@ ");
          else
            spc = true;
          fprintf(ppf, "@[<hv 1>case \"%s\":@ %a@]", format::string_escaped(c.s), format::pr(lam, c.action));
        }
        if (ls->def) {
          if (spc)
            fprintf(ppf, "@ ");
          else
            spc = true;
          fprintf(ppf, "@[<hv 1>default:@ %a@]", format::pr(lam, ls->def));
        }
      };
      fprintf(ppf, "@[<1>(stringswitch %a@ @[<v 0>%a@])@]", format::pr(lam, ls->arg), switch_);
      return;
    }
    case LK::Lstaticraise: {
      auto* lr = as<Lstaticraise>(l);
      fprintf(ppf, "@[<2>(exit@ %d%a)@]", lr->i, lams(lr->args));
      return;
    }
    case LK::Lstaticcatch: {
      auto* lc = as<Lstaticcatch>(l);
      Slice<Param> vars = lc->params;
      fprintf(ppf, "@[<2>(catch@ %a@;<1 -1>with (%d%a)@ %a)@]", format::pr(lam, lc->body), lc->i,
              [vars](Formatter& ppf) {
                for (const Param& x : vars)
                  fprintf(ppf, " %a%a", format::pr(ident, x.id), format::pr(value_kind, x.kind));
              },
              format::pr(lam, lc->handler));
      return;
    }
    case LK::Ltrywith: {
      auto* lt = as<Ltrywith>(l);
      fprintf(ppf, "@[<2>(try@ %a@;<1 -1>with %a@ %a)@]", format::pr(lam, lt->body), format::pr(ident, lt->exn),
              format::pr(lam, lt->handler));
      return;
    }
    case LK::Lifthenelse: {
      auto* li = as<Lifthenelse>(l);
      fprintf(ppf, "@[<2>(if@ %a@ %a@ %a)@]", format::pr(lam, li->cond), format::pr(lam, li->ifso),
              format::pr(lam, li->ifnot));
      return;
    }
    case LK::Lsequence: {
      auto* ls = as<Lsequence>(l);
      fprintf(ppf, "@[<2>(seq@ %a@ %a)@]", format::pr(lam, ls->l1), format::pr(sequence, ls->l2));
      return;
    }
    case LK::Lwhile: {
      auto* lw = as<Lwhile>(l);
      fprintf(ppf, "@[<2>(while@ %a@ %a)@]", format::pr(lam, lw->cond), format::pr(lam, lw->body));
      return;
    }
    case LK::Lfor: {
      auto* lf = as<Lfor>(l);
      fprintf(ppf, "@[<2>(for %a@ %a@ %s@ %a@ %a)@]", format::pr(ident, lf->id), format::pr(lam, lf->lo),
              lf->dir == parsetree::DirectionFlag::Upto ? "to" : "downto", format::pr(lam, lf->hi),
              format::pr(lam, lf->body));
      return;
    }
    case LK::Lassign: {
      auto* la = as<Lassign>(l);
      fprintf(ppf, "@[<2>(assign@ %a@ %a)@]", format::pr(ident, la->id), format::pr(lam, la->e));
      return;
    }
    case LK::Lsend: {
      auto* ls = as<Lsend>(l);
      std::string_view kind = ls->k == MethKind::Self ? "self" : ls->k == MethKind::Cached ? "cache" : "";
      fprintf(ppf, "@[<2>(send%s@ %a@ %a%a)@]", kind, format::pr(lam, ls->obj), format::pr(lam, ls->met),
              lams(ls->args));
      return;
    }
    case LK::Levent: {
      auto* le = as<Levent>(l);
      const LambdaEvent* ev = le->ev;
      std::string_view kind;
      switch (ev->lev_kind) {
        case EventKind::Lev_before: kind = "before"; break;
        case EventKind::Lev_after: kind = "after"; break;
        case EventKind::Lev_function: kind = "funct-body"; break;
        case EventKind::Lev_pseudo: kind = "pseudo"; break;
      }
      // -dno-locations also hides the placement of debug events
      if (!flags::locations) {
        lam(ppf, le->l);
      } else if (!ev->lev_loc.known()) {
        fprintf(ppf, "@[<2>(%s <unknown location>@ %a)@]", kind, format::pr(lam, le->l));
      } else {
        const Location& loc = ev->lev_loc.loc();
        fprintf(ppf, "@[<2>(%s %s %s(%i)%s:%i-%i@ %a)@]", kind, debuginfo::string_of_scopes(ev->lev_loc.sc),
                loc.loc_start.pos_fname, loc.loc_start.pos_lnum, loc.loc_ghost ? "<ghost>" : "",
                loc.loc_start.pos_cnum, loc.loc_end.pos_cnum, format::pr(lam, le->l));
      }
      return;
    }
    case LK::Lifused: {
      auto* lu = as<Lifused>(l);
      fprintf(ppf, "@[<2>(ifused@ %a@ %a)@]", format::pr(ident, lu->id), format::pr(lam, lu->l));
      return;
    }
  }
}

void sequence(Formatter& ppf, lambda::lambda l) {
  if (auto* ls = as<Lsequence>(l))
    fprintf(ppf, "%a@ %a", format::pr(sequence, ls->l1), format::pr(sequence, ls->l2));
  else
    lam(ppf, l);
}

void lfunction(Formatter& ppf, const LFunction* f) {
  auto pr_params = [f](Formatter& ppf) {
    switch (f->kind) {
      case FunctionKind::Curried:
        for (const Param& p : f->params)
          fprintf(ppf, "@ %a%a", format::pr(ident, p.id), format::pr(value_kind, p.kind));
        return;
      case FunctionKind::Tupled: {
        fprintf(ppf, " (");
        bool first = true;
        for (const Param& p : f->params) {
          if (first)
            first = false;
          else
            fprintf(ppf, ",@ ");
          ident(ppf, p.id);
          value_kind(ppf, p.kind);
        }
        fprintf(ppf, ")");
        return;
      }
    }
  };
  fprintf(ppf, "@[<2>(function%a@ %a%a%a)@]", pr_params, format::pr(function_attribute, f->attr),
          format::pr(return_kind, f->return_), format::pr(lam, f->body));
}

// Printtyp.path = `!Oprint.out_ident ppf (tree_of_path ~disambiguation:false
// None p)`, as it prints while !printing_env is Env.empty (so no
// rewrite_double_underscore_paths).  Printtyp's port (stage 9) installs the
// general one through print_path.
void print_lident(Formatter& ppf, std::string_view s) {
  if (s == "::")
    ppf.print_string("(::)");
  else if (is_ocaml_keyword(s))
    fprintf(ppf, "\\#%s", s);
  else
    ppf.print_string(s);
}

void default_print_path(Formatter& ppf, Path::t p) {
  switch (p->kind) {
    case Path::Kind::Pident:
      print_lident(ppf, ident::name(p->id));
      return;
    case Path::Kind::Pdot:
      // non_shadowed_stdlib None: Namespace.lookup None raises Not_found
      if (p->p1->kind == Path::Kind::Pident && p->p1->id->kind == Ident::Kind::Global &&
          p->p1->id->name_ == "Stdlib") {
        print_lident(ppf, p->s);
        return;
      }
      default_print_path(ppf, p->p1);
      ppf.print_char('.');
      print_lident(ppf, p->s);
      return;
    case Path::Kind::Papply:
      fprintf(ppf, "%a(%a)", format::pr(default_print_path, p->p1), format::pr(default_print_path, p->p2));
      return;
    case Path::Kind::Pextra_ty:
      if (p->extra == Path::Extra::Pcstr_ty) {
        default_print_path(ppf, p->p1);
        ppf.print_char('.');
        print_lident(ppf, p->s);
      } else {
        default_print_path(ppf, p->p1);
      }
      return;
  }
}

}  // namespace

void (*print_path)(Formatter& ppf, Path::t p) = default_print_path;

void structured_constant(Formatter& ppf, const StructuredConstant* c) { struct_const(ppf, c); }

void lambda(Formatter& ppf, lambda::lambda l) { lam(ppf, l); }

void program(Formatter& ppf, const Program& p) { lambda(ppf, p.code); }

std::string dump(lambda::lambda l) {
  Formatter ppf;
  fprintf(ppf, "%a@.", format::pr(lam, l));
  return ppf.take();
}

}  // namespace cppcaml::typing::printlambda
