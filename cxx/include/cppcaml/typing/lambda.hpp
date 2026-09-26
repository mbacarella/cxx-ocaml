// Port of lambda/lambda.mli (TYPECHECKER.md stage 10): the Lambda
// intermediate code, field for field, and the functions of lambda.ml.  Nodes
// are immutable and zone-allocated; one struct per constructor, matched with
// `as<L>(lam)` like the Types descs.  (Debuginfo.Scoped_location is here
// too: it is Lambda's location type.)
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/typedtree.hpp"
#include "cppcaml/typing/types.hpp"

namespace cppcaml::typing {

// ---- Debuginfo.Scoped_location --------------------------------------------
namespace debuginfo {
struct Scopes {  // Cons {item; str; str_fun} (nullptr = Empty)
  enum class Item : std::uint8_t {
    Sc_anonymous_function, Sc_value_definition, Sc_module_definition, Sc_class_definition,
    Sc_method_definition
  };
  Item item;
  std::string_view str;
  std::string_view str_fun;
};
using scopes = const Scopes*;
inline constexpr scopes empty_scopes = nullptr;
std::string string_of_scopes(scopes s);
scopes enter_anonymous_function(scopes s);
scopes enter_value_definition(scopes s, Ident::t id);
scopes enter_module_definition(scopes s, Ident::t id);
scopes enter_class_definition(scopes s, Ident::t id);
scopes enter_method_definition(scopes s, std::string_view label);

struct ScopedLocation {  // Loc_unknown | Loc_known {loc; scopes}
  bool known = false;
  Location loc{};
  scopes sc = nullptr;
};
inline ScopedLocation loc_unknown() { return {}; }
ScopedLocation of_location(scopes s, const Location& loc);
Location to_location(const ScopedLocation& l);
std::string string_of_scoped_location(const ScopedLocation& l);
}  // namespace debuginfo

namespace lambda {

using debuginfo::ScopedLocation;

// Ident.Set / Ident.Map: ordered by Ident.compare
struct IdentLess {
  bool operator()(Ident::t a, Ident::t b) const { return ident::compare(a, b) < 0; }
};
using IdentSet = std::set<Ident::t, IdentLess>;
template <class V>
using IdentMap = std::map<Ident::t, V, IdentLess>;

enum class CompileTimeConstant : std::uint8_t {
  Big_endian, Word_size, Int_size, Max_wosize, Ostype_unix, Ostype_win32, Ostype_cygwin, Backend_type,
  Standard_library_default
};
enum class ImmediateOrPointer : std::uint8_t { Immediate, Pointer };
enum class InitializationOrAssignment : std::uint8_t { Assignment, Heap_initialization, Root_initialization };
enum class IsSafe : std::uint8_t { Safe, Unsafe };
enum class LazyBlockTag : std::uint8_t { Lazy_tag, Forward_tag };
enum class IntegerComparison : std::uint8_t { Ceq, Cne, Clt, Cgt, Cle, Cge };
enum class PhysicalComparison : std::uint8_t { CPeq, CPneq };
enum class FloatComparison : std::uint8_t { CFeq, CFneq, CFlt, CFnlt, CFgt, CFngt, CFle, CFnle, CFge, CFnge };
enum class ArrayKind : std::uint8_t { Pgenarray, Paddrarray, Pintarray, Pfloatarray };
enum class BigarrayKind : std::uint8_t {
  Pbigarray_unknown, Pbigarray_float16, Pbigarray_float32, Pbigarray_float64, Pbigarray_sint8, Pbigarray_uint8,
  Pbigarray_sint16, Pbigarray_uint16, Pbigarray_int32, Pbigarray_int64, Pbigarray_caml_int,
  Pbigarray_native_int, Pbigarray_complex32, Pbigarray_complex64
};
enum class BigarrayLayout : std::uint8_t { Pbigarray_unknown_layout, Pbigarray_c_layout, Pbigarray_fortran_layout };
enum class RaiseKind : std::uint8_t { Raise_regular, Raise_reraise, Raise_notrace };

// value_kind = Pgenval | Pfloatval | Pboxedintval of boxed_integer | Pintval
struct ValueKind {
  enum class Kind : std::uint8_t { Pgenval, Pfloatval, Pboxedintval, Pintval };
  Kind kind = Kind::Pgenval;
  BoxedInteger bi = BoxedInteger::Pnativeint;  // Pboxedintval
  static ValueKind gen() { return {}; }
  static ValueKind intval() { return {Kind::Pintval}; }
  static ValueKind floatval() { return {Kind::Pfloatval}; }
  static ValueKind boxedint(BoxedInteger b) { return {Kind::Pboxedintval, b}; }
};
bool equal_value_kind(const ValueKind& a, const ValueKind& b);
bool equal_boxed_integer(BoxedInteger a, BoxedInteger b);

// block_shape = value_kind list option
struct BlockShape {
  bool some = false;
  Slice<ValueKind> kinds;
};

struct Primitive {
  enum class K : std::uint8_t {
    Pbytes_to_string, Pbytes_of_string, Pignore,
    Pgetglobal, Psetglobal,
    Pmakeblock, Pmakelazyblock, Pfield, Pfield_computed, Psetfield, Psetfield_computed, Pfloatfield,
    Psetfloatfield, Pduprecord,
    Prunstack, Pperform, Presume, Preperform,
    Pccall,
    Praise,
    Psequand, Psequor, Pnot,
    Pnegint, Paddint, Psubint, Pmulint, Pdivint, Pmodint, Pandint, Porint, Pxorint, Plslint, Plsrint, Pasrint,
    Pintcomp, Pphyscomp,
    Pcompare_ints, Pcompare_floats, Pcompare_bints,
    Poffsetint, Poffsetref,
    Pintoffloat, Pfloatofint, Pnegfloat, Pabsfloat, Paddfloat, Psubfloat, Pmulfloat, Pdivfloat, Pfloatcomp,
    Pstringlength, Pstringrefu, Pstringrefs, Pbyteslength, Pbytesrefu, Pbytessetu, Pbytesrefs, Pbytessets,
    Pmakearray, Pduparray, Parraylength, Parrayrefu, Parraysetu, Parrayrefs, Parraysets,
    Pisint, Pisout, Pcheckbound,
    Pbintofint, Pintofbint, Pcvtbint, Pnegbint, Paddbint, Psubbint, Pmulbint, Pdivbint, Pmodbint, Pandbint,
    Porbint, Pxorbint, Plslbint, Plsrbint, Pasrbint, Pbintcomp,
    Pbigarrayref, Pbigarrayset, Pbigarraydim,
    Pstring_load_16, Pstring_load_32, Pstring_load_64, Pbytes_load_16, Pbytes_load_32, Pbytes_load_64,
    Pbytes_set_16, Pbytes_set_32, Pbytes_set_64,
    Pbigstring_load_16, Pbigstring_load_32, Pbigstring_load_64, Pbigstring_set_16, Pbigstring_set_32,
    Pbigstring_set_64,
    Pctconst,
    Pbswap16, Pbbswap,
    Pint_as_pointer,
    Patomic_load,
    Popaque,
    Pdls_get,
    Ppoll
  };
  K kind;
  Ident::t id = nullptr;                   // Pgetglobal / Psetglobal
  long n = 0;  // Pmakeblock tag, Pfield/Psetfield/Pfloatfield/Psetfloatfield index, Pduprecord size,
               // Poffsetint/Poffsetref delta, Pbigarrayref/set #dims, Pbigarraydim n
  MutableFlag mut = MutableFlag::Immutable;  // Pmakeblock / Pfield / Pmakearray / Pduparray
  BlockShape shape;                          // Pmakeblock
  LazyBlockTag lazy_tag = LazyBlockTag::Lazy_tag;                    // Pmakelazyblock
  ImmediateOrPointer ptr = ImmediateOrPointer::Pointer;              // Pfield / Psetfield(_computed)
  InitializationOrAssignment init = InitializationOrAssignment::Assignment;  // Psetfield(_computed) / Psetfloatfield
  RecordRepresentation repr;                                         // Pduprecord
  const PrimitiveDescription* ccall = nullptr;                       // Pccall
  RaiseKind raise = RaiseKind::Raise_regular;                        // Praise
  IsSafe safe = IsSafe::Safe;                                        // Pdivint / Pmodint / Pdivbint / Pmodbint
  IntegerComparison icmp = IntegerComparison::Ceq;                   // Pintcomp / Pbintcomp
  PhysicalComparison pcmp = PhysicalComparison::CPeq;                // Pphyscomp
  FloatComparison fcmp = FloatComparison::CFeq;                      // Pfloatcomp
  ArrayKind array = ArrayKind::Pgenarray;                            // Pmakearray ... Parraysets
  BoxedInteger bi = BoxedInteger::Pnativeint;  // all the bint ops; Pcvtbint source; Pcompare_bints; Pbbswap
  BoxedInteger bi2 = BoxedInteger::Pnativeint;                       // Pcvtbint destination
  bool unsafe = false;  // Pbigarrayref/set, the 16/32/64-bit loads and sets
  BigarrayKind ba_kind = BigarrayKind::Pbigarray_unknown;
  BigarrayLayout ba_layout = BigarrayLayout::Pbigarray_unknown_layout;
  CompileTimeConstant ctconst = CompileTimeConstant::Big_endian;     // Pctconst
};
// the constant and simple constructors
inline Primitive prim(Primitive::K k) { return Primitive{k}; }
bool equal_primitive(const Primitive& a, const Primitive& b);

struct StructuredConstant {
  enum class Kind : std::uint8_t {
    Const_int, Const_char, Const_float, Const_int32, Const_int64, Const_nativeint, Const_block,
    Const_float_array, Const_immstring
  };
  Kind kind;
  long i = 0;                   // Const_int, Const_char (the char code), Const_block tag
  std::int64_t boxed = 0;       // Const_int32 / Const_int64 / Const_nativeint
  const void* box = nullptr;    // the box's identity when it is a typed constant's (typedtree.hpp)
  std::string_view s;           // Const_float / Const_immstring
  Slice<const StructuredConstant*> fields;  // Const_block
  Slice<std::string_view> floats;           // Const_float_array
};
const StructuredConstant* const_int(long n);
const StructuredConstant* const_unit();

enum class TailcallAttribute : std::uint8_t { Tailcall_expectation_true, Tailcall_expectation_false, Default_tailcall };
struct InlineAttribute {  // Always_inline | Never_inline | Hint_inline | Unroll of int | Default_inline
  enum class Kind : std::uint8_t { Always_inline, Never_inline, Hint_inline, Unroll, Default_inline };
  Kind kind = Kind::Default_inline;
  long unroll = 0;
};
bool equal_inline_attribute(const InlineAttribute& a, const InlineAttribute& b);
enum class SpecialiseAttribute : std::uint8_t { Always_specialise, Never_specialise, Default_specialise };
bool equal_specialise_attribute(SpecialiseAttribute a, SpecialiseAttribute b);
enum class LocalAttribute : std::uint8_t { Always_local, Never_local, Default_local };
enum class PollAttribute : std::uint8_t { Error_poll, Default_poll };
enum class FunctionKind : std::uint8_t { Curried, Tupled };
enum class LetKind : std::uint8_t { Strict, Alias, StrictOpt };
enum class MethKind : std::uint8_t { Self, Public, Cached };
bool equal_meth_kind(MethKind a, MethKind b);

struct FunctionAttribute {
  InlineAttribute inline_;
  SpecialiseAttribute specialise = SpecialiseAttribute::Default_specialise;
  LocalAttribute local = LocalAttribute::Default_local;
  PollAttribute poll = PollAttribute::Default_poll;
  bool is_a_functor = false;
  bool stub = false;
  bool tmc_candidate = false;
  bool may_fuse_arity = true;
};
FunctionAttribute default_function_attribute();
FunctionAttribute default_stub_attribute();

// ---- lambda ------------------------------------------------------------------
enum class LK : std::uint8_t {
  Lvar, Lmutvar, Lconst, Lapply, Lfunction, Llet, Lmutlet, Lletrec, Lprim, Lswitch, Lstringswitch,
  Lstaticraise, Lstaticcatch, Ltrywith, Lifthenelse, Lsequence, Lwhile, Lfor, Lassign, Lsend, Levent, Lifused
};
struct Lambda {
  LK kind;
};
using lambda = const Lambda*;

template <class T>
const T* as(lambda l) {
  return l && l->kind == T::K ? static_cast<const T*>(l) : nullptr;
}

struct Param {  // Ident.t * value_kind
  Ident::t id;
  ValueKind kind;
};
struct LFunction {  // lfunction (a private record: built by lfunction')
  FunctionKind kind;
  Slice<Param> params;
  ValueKind return_;
  lambda body;
  FunctionAttribute attr;
  ScopedLocation loc;
};
struct RecBinding {
  Ident::t id;
  const LFunction* def;
};
struct LambdaApply {
  lambda ap_func;
  Slice<lambda> ap_args;
  ScopedLocation ap_loc;
  TailcallAttribute ap_tailcall = TailcallAttribute::Default_tailcall;
  InlineAttribute ap_inlined;
  SpecialiseAttribute ap_specialised = SpecialiseAttribute::Default_specialise;
};
struct SwitchCase {
  long key;
  lambda action;
};
struct LambdaSwitch {
  long sw_numconsts;
  Slice<SwitchCase> sw_consts;
  long sw_numblocks;
  Slice<SwitchCase> sw_blocks;
  lambda sw_failaction;  // nullptr = None
};
enum class EventKind : std::uint8_t { Lev_before, Lev_after, Lev_function, Lev_pseudo };
struct IntRef {
  long contents;
};
struct LambdaEvent {
  ScopedLocation lev_loc;
  EventKind lev_kind;
  TypeExpr* lev_after_type = nullptr;  // Lev_after
  IntRef* lev_repr = nullptr;          // int ref option
  env::t lev_env;
};

#define LAMBDA_CTOR(Name) \
  struct Name : Lambda {  \
    static constexpr LK K = LK::Name;
LAMBDA_CTOR(Lvar) Ident::t id; };
LAMBDA_CTOR(Lmutvar) Ident::t id; };
LAMBDA_CTOR(Lconst) const StructuredConstant* c; };
LAMBDA_CTOR(Lapply) LambdaApply ap; };
LAMBDA_CTOR(Lfunction) const LFunction* f; };
LAMBDA_CTOR(Llet) LetKind str; ValueKind k; Ident::t id; lambda arg; lambda body; };
LAMBDA_CTOR(Lmutlet) ValueKind k; Ident::t id; lambda arg; lambda body; };
LAMBDA_CTOR(Lletrec) Slice<RecBinding> decl; lambda body; };
LAMBDA_CTOR(Lprim) Primitive p; Slice<lambda> args; ScopedLocation loc; };
LAMBDA_CTOR(Lswitch) lambda arg; LambdaSwitch sw; ScopedLocation loc; };
struct StringCase {
  std::string_view s;
  lambda action;
};
LAMBDA_CTOR(Lstringswitch) lambda arg; Slice<StringCase> cases; lambda def; ScopedLocation loc; };
LAMBDA_CTOR(Lstaticraise) long i; Slice<lambda> args; };
LAMBDA_CTOR(Lstaticcatch) lambda body; long i; Slice<Param> params; lambda handler; };
LAMBDA_CTOR(Ltrywith) lambda body; Ident::t exn; lambda handler; };
LAMBDA_CTOR(Lifthenelse) lambda cond; lambda ifso; lambda ifnot; };
LAMBDA_CTOR(Lsequence) lambda l1; lambda l2; };
LAMBDA_CTOR(Lwhile) lambda cond; lambda body; };
LAMBDA_CTOR(Lfor) Ident::t id; lambda lo; lambda hi; parsetree::DirectionFlag dir; lambda body; };
LAMBDA_CTOR(Lassign) Ident::t id; lambda e; };
LAMBDA_CTOR(Lsend) MethKind k; lambda met; lambda obj; Slice<lambda> args; ScopedLocation loc; };
LAMBDA_CTOR(Levent) lambda l; const LambdaEvent* ev; };
LAMBDA_CTOR(Lifused) Ident::t id; lambda l; };
#undef LAMBDA_CTOR

// constructor functions (fresh nodes)
lambda lvar(Ident::t id);
lambda lmutvar(Ident::t id);
lambda lconst(const StructuredConstant* c);
lambda lapply(const LambdaApply& ap);
lambda lfunction_node(const LFunction* f);
lambda llet(LetKind str, ValueKind k, Ident::t id, lambda arg, lambda body);
lambda lmutlet(ValueKind k, Ident::t id, lambda arg, lambda body);
lambda lletrec(Slice<RecBinding> decl, lambda body);
lambda lprim(const Primitive& p, Slice<lambda> args, const ScopedLocation& loc);
lambda lswitch(lambda arg, const LambdaSwitch& sw, const ScopedLocation& loc);
lambda lstringswitch(lambda arg, Slice<StringCase> cases, lambda def, const ScopedLocation& loc);
lambda lstaticraise(long i, Slice<lambda> args);
lambda lstaticcatch(lambda body, long i, Slice<Param> params, lambda handler);
lambda ltrywith(lambda body, Ident::t exn, lambda handler);
lambda lifthenelse(lambda c, lambda a, lambda b);
lambda lsequence(lambda a, lambda b);
lambda lwhile(lambda c, lambda b);
lambda lfor(Ident::t id, lambda lo, lambda hi, parsetree::DirectionFlag dir, lambda body);
lambda lassign(Ident::t id, lambda e);
lambda lsend(MethKind k, lambda met, lambda obj, Slice<lambda> args, const ScopedLocation& loc);
lambda levent(lambda l, const LambdaEvent* ev);
lambda lifused(Ident::t id, lambda l);

struct Program {
  Ident::t module_ident;
  long main_module_block_size;
  IdentSet required_globals;
  lambda code;
};

// ---- functions of lambda.ml ---------------------------------------------------
std::optional<lambda> make_key(lambda e);
lambda lambda_unit();
lambda lambda_of_const(const typedtree::Constant& c);
lambda dummy_constant();
lambda name_lambda(LetKind str, lambda arg, const std::function<lambda(Ident::t)>& fn);
lambda name_lambda_list(Slice<lambda> args, const std::function<lambda(Slice<lambda>)>& fn);
lambda lfunction(FunctionKind kind, Slice<Param> params, ValueKind return_, lambda body,
                 const FunctionAttribute& attr, const ScopedLocation& loc);
const LFunction* lfunction_(FunctionKind kind, Slice<Param> params, ValueKind return_, lambda body,
                            const FunctionAttribute& attr, const ScopedLocation& loc);
void iter_head_constructor(const std::function<void(lambda)>& f, lambda l);
void shallow_iter(const std::function<void(lambda)>& tail, const std::function<void(lambda)>& non_tail, lambda l);
lambda transl_prim(std::string_view modname, std::string_view field);
bool is_evaluated(lambda l);
IdentSet free_variables(lambda l);
lambda transl_module_path(const ScopedLocation& loc, env::t env, Path::t path);
lambda transl_value_path(const ScopedLocation& loc, env::t env, Path::t path);
lambda transl_extension_path(const ScopedLocation& loc, env::t env, Path::t path);
lambda transl_class_path(const ScopedLocation& loc, env::t env, Path::t path);
template <class T, class F>
lambda make_sequence(F&& fn, const T& l) {
  // [] -> lambda_unit | [x] -> fn x | x :: rem -> Lsequence (fn x, ...)
  std::size_t n = l.size();
  if (n == 0) return lambda_unit();
  std::vector<lambda> parts;
  for (auto& x : l) parts.push_back(fn(x));  // `let lam = fn x in ...`: left to right
  lambda acc = parts.back();
  for (std::size_t k = n - 1; k-- > 0;) acc = lsequence(parts[k], acc);
  return acc;
}
using UpdateEnv = std::function<env::t(Ident::t, const ValueDescription*, env::t)>;
lambda subst(const UpdateEnv& update_env, bool freshen_bound_variables, const IdentMap<lambda>& s, lambda lt);
lambda rename(const IdentMap<Ident::t>& idmap, lambda lt);
const LFunction* duplicate_function(const LFunction* f);
lambda map(const std::function<lambda(lambda)>& f, lambda l);
const LFunction* map_lfunction(const std::function<lambda(lambda)>& f, const LFunction* lf);
lambda shallow_map(const std::function<lambda(lambda)>& f, lambda l);
lambda bind(LetKind str, Ident::t var, lambda exp, lambda body);
lambda bind_with_value_kind(LetKind str, Ident::t var, ValueKind k, lambda exp, lambda body);
PhysicalComparison negate_physical_comparison(PhysicalComparison c);
IntegerComparison negate_integer_comparison(IntegerComparison c);
IntegerComparison swap_integer_comparison(IntegerComparison c);
FloatComparison negate_float_comparison(FloatComparison c);
FloatComparison swap_float_comparison(FloatComparison c);
bool function_is_curried(const LFunction* f);
std::optional<Slice<lambda>> find_exact_application(FunctionKind kind, long arity, Slice<lambda> args);
long max_arity();
long tag_of_lazy_tag(LazyBlockTag t);
lambda make_atomic_loc(const ScopedLocation& loc, lambda arg, lambda field);
long next_raise_count();
lambda staticfail();
bool is_guarded(lambda l);
lambda patch_guarded(lambda patch, lambda l);
std::string_view raise_kind(RaiseKind k);
std::optional<InlineAttribute> merge_inline_attributes(const InlineAttribute& a, const InlineAttribute& b);
void reset();

// structural equality (OCaml's polymorphic `=` on lambda terms, as Simplif and
// the switch sharing use it; events compare by their fields)
bool equal_lambda(lambda a, lambda b);
bool equal_structured_constant(const StructuredConstant* a, const StructuredConstant* b);

}  // namespace lambda
}  // namespace cppcaml::typing
