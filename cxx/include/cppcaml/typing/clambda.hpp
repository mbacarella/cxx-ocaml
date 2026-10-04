// Port of middle_end/clambda.ml, clambda_primitives.ml, backend_var.ml and
// the backend half of lambda/debuginfo.ml: the Closure middle end's IR (a
// variant of Lambda with direct / indirect calls and closures explicit),
// its primitives, the approximations of values, and the backend debug
// information it carries.  Zone-allocated and immutable where OCaml's are;
// function_description keeps its mutable fields.
#pragma once

#include <cstdint>
#include <string_view>

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing {

// ---- Debuginfo (the backend part) ------------------------------------------------------------
namespace debuginfo {
struct Item {
  std::string_view dinfo_file;
  long dinfo_line;
  long dinfo_char_start;
  long dinfo_char_end;
  long dinfo_start_bol;
  long dinfo_end_bol;
  long dinfo_end_line;
  scopes dinfo_scopes;
};
using t = Slice<Item>;  // item list
inline t none() { return {}; }
inline bool is_none(const t& d) { return d.empty(); }
t from_location(const lambda::ScopedLocation& l);
Location to_location(const t& d);
t inline_(const t& dbg1, const t& dbg2);  // Debuginfo.inline: dbg1 @ dbg2
// The identities of a list's cells and items, where they are not one per
// Slice element: a list read from a .cmx (whose tails and items other lists
// share) or built by [inline_] (dbg1's items in fresh cells, then dbg2's
// cells).  Keys: [cell_key] / [item_key] for element k of a list.
std::uint64_t cell_key(const t& d, std::size_t k);
std::uint64_t item_key(const t& d, std::size_t k);
std::uint64_t fresh_key();
void set_shape(const t& d, std::vector<std::uint64_t> cells, std::vector<std::uint64_t> items);
bool has_shape(const t& d);
int compare(const t& a, const t& b);
std::string to_string(const t& d);
}  // namespace debuginfo

namespace clambda {

using lambda::ValueKind;
using Var = Ident::t;  // Backend_var.t

// Backend_var.Provenance / With_provenance
struct Provenance {
  Path::t module_path;
  debuginfo::t location;
  Ident::t original_ident;
};
struct VarWithProvenance {  // Without_provenance var | With_provenance {var; provenance}
  Var var;
  const Provenance* provenance = nullptr;
};
inline VarWithProvenance vp(Var v) { return {v, nullptr}; }

unsigned long fresh_uconstant_id();  // a fresh block identity

// ---- Clambda_primitives -----------------------------------------------------------------------
enum class Boxed : std::uint8_t { Boxed, Unboxed };
enum class MemoryAccessSize : std::uint8_t { Sixteen, Thirty_two, Sixty_four };

struct Primitive {
  // in clambda_primitives.ml's declaration order
  enum class K : std::uint8_t {
    Pread_symbol, Pmakeblock, Pmakelazyblock, Pfield, Pfield_computed, Psetfield, Psetfield_computed,
    Pfloatfield, Psetfloatfield, Pduprecord, Prunstack, Pperform, Presume, Preperform, Pccall, Praise,
    Psequand, Psequor, Pnot, Pnegint, Paddint, Psubint, Pmulint, Pdivint, Pmodint, Pandint, Porint,
    Pxorint, Plslint, Plsrint, Pasrint, Pintcomp, Pcompare_ints, Pcompare_floats, Pcompare_bints,
    Poffsetint, Poffsetref, Pintoffloat, Pfloatofint, Pnegfloat, Pabsfloat, Paddfloat, Psubfloat,
    Pmulfloat, Pdivfloat, Pfloatcomp, Pstringlength, Pstringrefu, Pstringrefs, Pbyteslength, Pbytesrefu,
    Pbytessetu, Pbytesrefs, Pbytessets, Pmakearray, Pduparray, Parraylength, Parrayrefu, Parraysetu,
    Parrayrefs, Parraysets, Pisint, Pisout, Pbintofint, Pintofbint, Pcvtbint, Pnegbint,
    Paddbint, Psubbint, Pmulbint, Pdivbint, Pmodbint, Pandbint, Porbint, Pxorbint, Plslbint, Plsrbint,
    Pasrbint, Pbintcomp, Pbigarrayref, Pbigarrayset, Pbigarraydim, Pstring_load, Pbytes_load, Pbytes_set,
    Pbigstring_load, Pbigstring_set, Pbswap16, Pbbswap, Pint_as_pointer, Patomic_load,
    Popaque, Pdls_get, Ppoll
  };
  K kind;
  std::string_view sym;  // Pread_symbol
  long n = 0;            // Pmakeblock tag, P(set)field(s) index, Pduprecord size, Poffset*, Pbigarray* dims
  MutableFlag mut = MutableFlag::Immutable;
  lambda::BlockShape shape;
  lambda::LazyBlockTag lazy_tag = lambda::LazyBlockTag::Lazy_tag;
  lambda::ImmediateOrPointer ptr = lambda::ImmediateOrPointer::Pointer;
  lambda::InitializationOrAssignment init = lambda::InitializationOrAssignment::Assignment;
  RecordRepresentation repr;
  const PrimitiveDescription* ccall = nullptr;
  lambda::RaiseKind raise = lambda::RaiseKind::Raise_regular;
  lambda::IsSafe safe = lambda::IsSafe::Safe;
  lambda::IntegerComparison icmp = lambda::IntegerComparison::Ceq;
  lambda::FloatComparison fcmp = lambda::FloatComparison::CFeq;
  lambda::ArrayKind array = lambda::ArrayKind::Pgenarray;
  BoxedInteger bi = BoxedInteger::Pnativeint;
  BoxedInteger bi2 = BoxedInteger::Pnativeint;  // Pcvtbint destination
  bool unsafe = false;                                          // Pbigarrayref/set
  lambda::BigarrayKind ba_kind = lambda::BigarrayKind::Pbigarray_unknown;
  lambda::BigarrayLayout ba_layout = lambda::BigarrayLayout::Pbigarray_unknown_layout;
  MemoryAccessSize size = MemoryAccessSize::Sixteen;  // the P*_load / P*_set of (size, is_safe)
  // the block's identity: one per construction, kept by copies (the .cmx
  // writer shares the block as ocamlopt's values do)
  unsigned long id = fresh_uconstant_id();
};
inline Primitive prim(Primitive::K k) { return Primitive{k}; }
// a literal primitive of an OCaml source file ([Pintcomp Ceq] ...): one
// block per file and value, so [p] takes the identity of the file's [key]
Primitive prim_literal(const char* unit, Primitive p, std::string_view key);
#define CLAMBDA_PRIM_LITERAL(p, key) ::cppcaml::typing::clambda::prim_literal(__FILE__, p, key)
// Clambda_primitives.equal (structural)
bool equal_primitive(const Primitive& a, const Primitive& b);

// ---- Clambda ------------------------------------------------------------------------------------
using FunctionLabel = std::string_view;
struct UStructuredConstant;
struct UConstant {  // Uconst_ref of string * ustructured_constant option | Uconst_int of int
  enum class Kind : std::uint8_t { Uconst_ref, Uconst_int };
  Kind kind;
  std::string_view sym;                     // Uconst_ref
  const UStructuredConstant* sc = nullptr;  // Uconst_ref's option
  long i = 0;                               // Uconst_int
  // the block's identity: one per construction, kept by copies (the .cmx
  // writer shares the block as ocamlopt's values do)
  unsigned long id = 0;
};
inline UConstant uconst_int(long i) { return {UConstant::Kind::Uconst_int, {}, nullptr, i, fresh_uconstant_id()}; }
inline UConstant uconst_ref(std::string_view s, const UStructuredConstant* c) {
  return {UConstant::Kind::Uconst_ref, s, c, 0, fresh_uconstant_id()};
}
struct UFunction;
struct UStructuredConstant {
  enum class Kind : std::uint8_t {
    Uconst_float, Uconst_int32, Uconst_int64, Uconst_nativeint, Uconst_block, Uconst_float_array,
    Uconst_string, Uconst_closure
  };
  Kind kind;
  double f = 0;                    // Uconst_float
  std::int64_t i = 0;              // Uconst_int32 / int64 / nativeint
  long tag = 0;                    // Uconst_block
  Slice<UConstant> fields;         // Uconst_block; Uconst_closure's free values
  Slice<double> floats;            // Uconst_float_array
  std::string_view s;              // Uconst_string; Uconst_closure's symbol
  Slice<const UFunction*> funs;    // Uconst_closure
};

struct ULambdaNode;
using ulambda = const ULambdaNode*;
struct UParam {
  VarWithProvenance var;
  ValueKind kind;
};
struct UFunction {
  FunctionLabel label;
  long arity;
  Slice<UParam> params;
  ValueKind return_;
  ulambda body;
  debuginfo::t dbg;
  Var env = nullptr;  // option
  lambda::PollAttribute poll = lambda::PollAttribute::Default_poll;
};
struct USwitch {  // ulambda_switch
  Slice<long> us_index_consts;
  Slice<ulambda> us_actions_consts;
  Slice<long> us_index_blocks;
  Slice<ulambda> us_actions_blocks;
};
struct UPhantomDefiningExpr {
  enum class Kind : std::uint8_t {
    Uphantom_const, Uphantom_var, Uphantom_offset_var, Uphantom_read_field, Uphantom_read_symbol_field,
    Uphantom_block
  };
  Kind kind;
  UConstant c;
  Var var = nullptr;
  long n = 0;  // offset_in_words / field / tag
  std::string_view sym;
  Slice<Var> fields;
};

enum class UK : std::uint8_t {
  Uvar, Uconst, Udirect_apply, Ugeneric_apply, Uclosure, Uoffset, Ulet, Uphantom_let, Uprim, Uswitch,
  Ustringswitch, Ustaticfail, Ucatch, Utrywith, Uifthenelse, Usequence, Uwhile, Ufor, Uassign, Usend,
  Uunreachable
};
struct ULambdaNode {
  UK kind;
};
template <class T>
const T* as(ulambda l) {
  return l && l->kind == T::K ? static_cast<const T*>(l) : nullptr;
}
#define ULAMBDA_CTOR(Name) \
  struct Name : ULambdaNode { \
    static constexpr UK K = UK::Name;
ULAMBDA_CTOR(Uvar) Var id; };
ULAMBDA_CTOR(Uconst) UConstant c; };
ULAMBDA_CTOR(Udirect_apply) FunctionLabel f; Slice<ulambda> args; debuginfo::t dbg; };
ULAMBDA_CTOR(Ugeneric_apply) ulambda f; Slice<ulambda> args; debuginfo::t dbg; };
ULAMBDA_CTOR(Uclosure) Slice<const UFunction*> funs; Slice<ulambda> fv; };
ULAMBDA_CTOR(Uoffset) ulambda l; long ofs; };
ULAMBDA_CTOR(Ulet) MutableFlag mut; ValueKind k; VarWithProvenance id; ulambda arg; ulambda body; };
ULAMBDA_CTOR(Uphantom_let) VarWithProvenance id; const UPhantomDefiningExpr* def; ulambda body; };
ULAMBDA_CTOR(Uprim) Primitive p; Slice<ulambda> args; debuginfo::t dbg; };
ULAMBDA_CTOR(Uswitch) ulambda arg; USwitch sw; debuginfo::t dbg; };
struct UStringCase {
  std::string_view s;
  ulambda action;
};
ULAMBDA_CTOR(Ustringswitch) ulambda arg; Slice<UStringCase> cases; ulambda def; };
ULAMBDA_CTOR(Ustaticfail) long i; Slice<ulambda> args; };
ULAMBDA_CTOR(Ucatch) long i; Slice<UParam> vars; ulambda body; ulambda handler; };
ULAMBDA_CTOR(Utrywith) ulambda body; VarWithProvenance exn; ulambda handler; };
ULAMBDA_CTOR(Uifthenelse) ulambda cond; ulambda ifso; ulambda ifnot; };
ULAMBDA_CTOR(Usequence) ulambda l1; ulambda l2; };
ULAMBDA_CTOR(Uwhile) ulambda cond; ulambda body; };
ULAMBDA_CTOR(Ufor) VarWithProvenance id; ulambda lo; ulambda hi; parsetree::DirectionFlag dir; ulambda body; };
ULAMBDA_CTOR(Uassign) Var id; ulambda e; };
ULAMBDA_CTOR(Usend) lambda::MethKind k; ulambda met; ulambda obj; Slice<ulambda> args; debuginfo::t dbg; };
ULAMBDA_CTOR(Uunreachable) };
#undef ULAMBDA_CTOR

// constructors (fresh zone nodes)
ulambda uvar(Var id);
ulambda uconst(const UConstant& c);
ulambda udirect_apply(FunctionLabel f, Slice<ulambda> args, const debuginfo::t& dbg);
ulambda ugeneric_apply(ulambda f, Slice<ulambda> args, const debuginfo::t& dbg);
ulambda uclosure(Slice<const UFunction*> funs, Slice<ulambda> fv);
ulambda uoffset(ulambda l, long ofs);
ulambda ulet(MutableFlag mut, ValueKind k, VarWithProvenance id, ulambda arg, ulambda body);
ulambda uphantom_let(VarWithProvenance id, const UPhantomDefiningExpr* def, ulambda body);
ulambda uprim(const Primitive& p, Slice<ulambda> args, const debuginfo::t& dbg);
ulambda uswitch(ulambda arg, const USwitch& sw, const debuginfo::t& dbg);
ulambda ustringswitch(ulambda arg, Slice<UStringCase> cases, ulambda def);
ulambda ustaticfail(long i, Slice<ulambda> args);
ulambda ucatch(long i, Slice<UParam> vars, ulambda body, ulambda handler);
ulambda utrywith(ulambda body, VarWithProvenance exn, ulambda handler);
ulambda uifthenelse(ulambda c, ulambda a, ulambda b);
ulambda usequence(ulambda a, ulambda b);
ulambda uwhile(ulambda c, ulambda b);
ulambda ufor(VarWithProvenance id, ulambda lo, ulambda hi, parsetree::DirectionFlag dir, ulambda body);
ulambda uassign(Var id, ulambda e);
ulambda usend(lambda::MethKind k, ulambda met, ulambda obj, Slice<ulambda> args, const debuginfo::t& dbg);
ulambda uunreachable();

// Description of known functions
struct FunctionDescription {
  FunctionLabel fun_label;  // Label of direct entry point
  long fun_arity;           // Number of arguments
  bool fun_closed;          // True if environment not used (mutable)
  // (Backend_var.With_provenance.t list * ulambda) option (mutable)
  bool has_inline = false;
  Slice<VarWithProvenance> inline_params;
  ulambda inline_body = nullptr;
  bool fun_float_const_prop;  // Can propagate FP consts (mutable)
  lambda::PollAttribute fun_poll;
};

// Approximation of values
struct ValueApproximation {
  enum class Kind : std::uint8_t { Value_closure, Value_tuple, Value_unknown, Value_const, Value_global_field };
  Kind kind;
  FunctionDescription* fundesc = nullptr;           // Value_closure
  const ValueApproximation* res = nullptr;          // Value_closure
  Slice<const ValueApproximation*> tuple;           // Value_tuple
  UConstant c;                                      // Value_const
  std::string_view sym;                             // Value_global_field
  long field = 0;                                   // Value_global_field
};
const ValueApproximation* value_unknown();  // one shared Value_unknown

// Comparison functions for constants
int compare_structured_constants(const UStructuredConstant* c1, const UStructuredConstant* c2);
int compare_constants(const UConstant& c1, const UConstant& c2);

struct USymbolProvenance {
  Slice<Ident::t> original_idents;
  Path::t module_path;
};
struct UConstantBlockField {  // Uconst_field_ref of string | Uconst_field_int of int
  bool is_ref;
  std::string_view ref;
  long i = 0;
};
struct PreallocatedBlock {
  std::string_view symbol;
  bool exported;
  long tag;
  Slice<std::optional<UConstantBlockField>> fields;
  const USymbolProvenance* provenance = nullptr;
};
struct PreallocatedConstant {
  std::string_view symbol;
  bool exported;
  const UStructuredConstant* definition;
  const USymbolProvenance* provenance = nullptr;
};

}  // namespace clambda
}  // namespace cppcaml::typing
