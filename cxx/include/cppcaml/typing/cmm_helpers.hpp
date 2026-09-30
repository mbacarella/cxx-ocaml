// Port of asmcomp/cmm_helpers.ml (the compilation-unit half; the generic
// functions and startup tables Asmlink needs come with the linker),
// asmcomp/cmmgen_state.ml and asmcomp/strmatch.ml, for amd64 (Arch:
// 64-bit little-endian words, unaligned access allowed, division crashes on
// overflow).
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cppcaml/typing/cmm.hpp"
#include "cppcaml/typing/cmx_format.hpp"

namespace cppcaml::typing::cmmgen_state {

enum class IsGlobal : std::uint8_t { Global, Local };
struct Constant {  // Const_closure of is_global * ufunction list * uconstant list | Const_table
  bool is_closure;
  IsGlobal global;
  Slice<const clambda::UFunction*> fundecls;
  Slice<clambda::UConstant> clos_vars;
  std::vector<cmm::DataItem> table;
};
void add_constant(std::string_view sym, Constant cst);
void add_data_items(std::vector<cmm::DataItem> items);
void add_function(const clambda::UFunction* f);
std::map<std::string_view, Constant> get_and_clear_constants();
std::vector<cmm::DataItem> get_and_clear_data_items();
const clambda::UFunction* next_function();  // nullptr: None
bool no_more_functions();
void set_structured_constants(const std::vector<clambda::PreallocatedConstant>& l);
void add_structured_constant(std::string_view sym, const clambda::UStructuredConstant* cst);
const clambda::UStructuredConstant* structured_constant_of_sym(std::string_view s);

}  // namespace cppcaml::typing::cmmgen_state

namespace cppcaml::typing::cmm_helpers {

using cmm::expression;
using Dbg = debuginfo::t;
using Body = std::function<expression(expression)>;

// Arch (amd64)
inline constexpr long size_addr = 8;
inline constexpr long size_int = 8;
inline constexpr long size_float = 8;
inline constexpr bool big_endian = false;
inline constexpr long max_young_wosize = 256;  // Config.max_young_wosize

expression bind(const char* name, expression arg, const Body& fn);
expression bind_load(const char* name, expression arg, const Body& fn);

std::int64_t block_header(long tag, long sz);
std::int64_t black_closure_header(long sz);
std::int64_t infix_header(long ofs);
extern const std::int64_t float_header;
extern const std::int64_t boxedint32_header;
extern const std::int64_t boxedint64_header;
extern const std::int64_t boxedintnat_header;
extern const char* const caml_nativeint_ops;
extern const char* const caml_int32_ops;
extern const char* const caml_int64_ops;
std::int64_t closure_info(long arity, long startenv);
expression alloc_closure_info(long arity, long startenv, const Dbg& dbg);
expression alloc_infix_header(long ofs, const Dbg& dbg);

expression int_const(const Dbg& dbg, long n);
expression natint_const_untagged(const Dbg& dbg, std::int64_t n);
cmm::DataItem cint_const(long n);
expression add_const(expression c, long n, const Dbg& dbg);
expression incr_int(expression c, const Dbg& dbg);
expression decr_int(expression c, const Dbg& dbg);
expression add_int(expression c1, expression c2, const Dbg& dbg);
expression sub_int(expression c1, expression c2, const Dbg& dbg);
expression lsl_int(expression c1, expression c2, const Dbg& dbg);
expression mul_int(expression c1, expression c2, const Dbg& dbg);
expression ignore_low_bit_int(expression c);
expression ignore_high_bit_int(expression c);
expression lsr_int(expression c1, expression c2, const Dbg& dbg);
expression asr_int(expression c1, expression c2, const Dbg& dbg);
expression tag_int(expression i, const Dbg& dbg);
expression untag_int(expression i, const Dbg& dbg);
expression mk_if_then_else(const Dbg& dbg, expression cond, const Dbg& ifso_dbg, expression ifso,
                           const Dbg& ifnot_dbg, expression ifnot);
expression mk_not(const Dbg& dbg, expression cmm);
expression mk_compare_ints(const Dbg& dbg, expression a1, expression a2);
expression mk_compare_floats(const Dbg& dbg, expression a1, expression a2);
expression create_loop(expression body, const Dbg& dbg);
expression safe_div_bi(lambda::IsSafe is_safe, expression c1, expression c2, BoxedInteger bi, const Dbg& dbg);
expression safe_mod_bi(lambda::IsSafe is_safe, expression c1, expression c2, BoxedInteger bi, const Dbg& dbg);
expression test_bool(const Dbg& dbg, expression cmm);
expression box_float(const Dbg& dbg, expression c);
expression unbox_float(const Dbg& dbg, expression c);
expression float_of_float16(const Dbg& dbg, expression c);
expression float16_of_float(const Dbg& dbg, expression c);
expression return_unit(const Dbg& dbg, expression c);
expression remove_unit(expression c);
expression field_address(expression ptr, long n, const Dbg& dbg);
expression get_field_gen(MutableFlag mut, expression ptr, long n, const Dbg& dbg,
                         cmm::MemoryChunk chunk = cmm::MemoryChunk::Word_val);
expression get_tag(expression ptr, const Dbg& dbg);
expression field_address_computed(expression ptr, expression ofs, const Dbg& dbg);
expression mk_load_mut(cmm::MemoryChunk c, expression arg, const Dbg& dbg);
expression mk_load_atomic(cmm::MemoryChunk c, expression arg, const Dbg& dbg);
expression string_length(expression exp, const Dbg& dbg);
expression make_alloc(const Dbg& dbg, long tag, const std::vector<expression>& args);
expression make_float_alloc(const Dbg& dbg, long tag, const std::vector<expression>& args);
expression make_checkbound(const Dbg& dbg, const std::vector<expression>& args);
std::string_view curry_function_sym(long n);
expression bigarray_get(bool unsafe, lambda::BigarrayKind elt_kind, lambda::BigarrayLayout layout, expression b,
                        const std::vector<expression>& args, const Dbg& dbg);
expression bigarray_set(bool unsafe, lambda::BigarrayKind elt_kind, lambda::BigarrayLayout layout, expression b,
                        const std::vector<expression>& args, expression newval, const Dbg& dbg);
expression low_32(const Dbg& dbg, expression x);
expression box_int_gen(const Dbg& dbg, BoxedInteger bi, expression arg);
expression unbox_int(const Dbg& dbg, BoxedInteger bi, expression c);
expression make_unsigned_int(BoxedInteger bi, expression arg, const Dbg& dbg);
expression opaque(expression e, const Dbg& dbg);
clambda::Primitive simplif_primitive(const clambda::Primitive& p);
const PrimitiveDescription* primitive_simple(std::string_view name, long arity, bool alloc);  // Primitive.simple
expression transl_isout(expression h, expression arg, const Dbg& dbg);
expression add_int_caml(expression a1, expression a2, const Dbg& dbg);
expression offsetint(long n, expression arg, const Dbg& dbg);
expression sub_int_caml(expression a1, expression a2, const Dbg& dbg);
expression mul_int_caml(expression a1, expression a2, const Dbg& dbg);
expression div_int_caml(lambda::IsSafe is_safe, expression a1, expression a2, const Dbg& dbg);
expression mod_int_caml(lambda::IsSafe is_safe, expression a1, expression a2, const Dbg& dbg);
expression and_int_caml(expression a1, expression a2, const Dbg& dbg);
expression or_int_caml(expression a1, expression a2, const Dbg& dbg);
expression xor_int_caml(expression a1, expression a2, const Dbg& dbg);
expression lsl_int_caml(expression a1, expression a2, const Dbg& dbg);
expression lsr_int_caml(expression a1, expression a2, const Dbg& dbg);
expression asr_int_caml(expression a1, expression a2, const Dbg& dbg);
expression int_comp_caml(lambda::IntegerComparison cmp, expression a1, expression a2, const Dbg& dbg);

// switch_arg = Tagged of expression | Untagged of expression
expression make_switch(bool tagged, expression arg, Slice<long> cases, const std::vector<cmm::SwitchCase>& actions,
                       const Dbg& dbg);
expression transl_switch_clambda(const Dbg& loc, expression arg, Slice<long> index,
                                 const std::vector<expression>& cases);
expression strmatch_compile(const Dbg& dbg, expression str, expression default_,
                            const std::vector<std::pair<std::string_view, expression>>& cases);

expression ptr_offset(expression ptr, long offset, const Dbg& dbg);
expression direct_apply(std::string_view lbl, const std::vector<expression>& args, const Dbg& dbg);
expression generic_apply(MutableFlag mut, expression clos, const std::vector<expression>& args, const Dbg& dbg);
expression send(lambda::MethKind kind, expression met, expression obj, const std::vector<expression>& args,
                const Dbg& dbg);

expression floatfield(long n, expression ptr, const Dbg& dbg);
expression int_as_pointer(expression arg, const Dbg& dbg);
expression raise_prim(lambda::RaiseKind k, expression arg, const Dbg& dbg);
expression negint(expression arg, const Dbg& dbg);
expression offsetref(long n, expression arg, const Dbg& dbg);
expression arraylength(lambda::ArrayKind kind, expression arg, const Dbg& dbg);
expression bbswap(BoxedInteger bi, expression arg, const Dbg& dbg);
expression bswap16(expression arg, const Dbg& dbg);
expression setfield(long n, lambda::ImmediateOrPointer ptr, lambda::InitializationOrAssignment init, expression arg1,
                    expression arg2, const Dbg& dbg);
expression setfloatfield(long n, lambda::InitializationOrAssignment init, expression arg1, expression arg2,
                         const Dbg& dbg);
expression stringref_unsafe(expression arg1, expression arg2, const Dbg& dbg);
expression stringref_safe(expression arg1, expression arg2, const Dbg& dbg);
expression string_load(clambda::MemoryAccessSize size, lambda::IsSafe unsafe, expression arg1, expression arg2,
                       const Dbg& dbg);
expression bigstring_load(clambda::MemoryAccessSize size, lambda::IsSafe unsafe, expression arg1, expression arg2,
                          const Dbg& dbg);
expression arrayref_unsafe(lambda::ArrayKind kind, expression arg1, expression arg2, const Dbg& dbg);
expression arrayref_safe(lambda::ArrayKind kind, expression arg1, expression arg2, const Dbg& dbg);
expression addr_array_ref(expression arr, expression ofs, const Dbg& dbg);
expression setfield_computed(lambda::ImmediateOrPointer ptr, lambda::InitializationOrAssignment init,
                             expression arg1, expression arg2, expression arg3, const Dbg& dbg);
expression bytesset_unsafe(expression arg1, expression arg2, expression arg3, const Dbg& dbg);
expression bytesset_safe(expression arg1, expression arg2, expression arg3, const Dbg& dbg);
expression arrayset_unsafe(lambda::ArrayKind kind, expression arg1, expression arg2, expression arg3,
                           const Dbg& dbg);
expression arrayset_safe(lambda::ArrayKind kind, expression arg1, expression arg2, expression arg3, const Dbg& dbg);
expression bytes_set(clambda::MemoryAccessSize size, lambda::IsSafe unsafe, expression arg1, expression arg2,
                     expression arg3, const Dbg& dbg);
expression bigstring_set(clambda::MemoryAccessSize size, lambda::IsSafe unsafe, expression arg1, expression arg2,
                         expression arg3, const Dbg& dbg);

// Symbols and data
using Symb = std::pair<std::string_view, cmmgen_state::IsGlobal>;
std::vector<cmm::DataItem> emit_block(const Symb& symb, std::int64_t white_header, std::vector<cmm::DataItem> cont);
std::vector<cmm::DataItem> emit_float_constant(const Symb& symb, double f, std::vector<cmm::DataItem> cont);
std::vector<cmm::DataItem> emit_string_constant(const Symb& symb, std::string_view s, std::vector<cmm::DataItem> cont);
std::vector<cmm::DataItem> emit_int32_constant(const Symb& symb, std::int64_t n, std::vector<cmm::DataItem> cont);
std::vector<cmm::DataItem> emit_int64_constant(const Symb& symb, std::int64_t n, std::vector<cmm::DataItem> cont);
std::vector<cmm::DataItem> emit_nativeint_constant(const Symb& symb, std::int64_t n,
                                                   std::vector<cmm::DataItem> cont);
std::vector<cmm::DataItem> emit_float_array_constant(const Symb& symb, Slice<double> fields,
                                                     std::vector<cmm::DataItem> cont);
std::vector<cmm::DataItem> cdefine_symbol(const Symb& symb);
long fundecls_size(Slice<const clambda::UFunction*> fundecls);
std::vector<cmm::DataItem> emit_constant_closure(const Symb& symb, Slice<const clambda::UFunction*> fundecls,
                                                 std::vector<cmm::DataItem> clos_vars,
                                                 std::vector<cmm::DataItem> cont);
// emit_preallocated_blocks blocks cont: the phrases prepended to cont
std::vector<cmm::Phrase> emit_preallocated_blocks(const std::vector<clambda::PreallocatedBlock>& blocks,
                                                  std::vector<cmm::Phrase> cont);

// ---- The startup module (Asmlink) ----------------------------------------
// Runtimedef.builtin_exceptions
extern const std::vector<std::string_view> builtin_exceptions;
// generic_functions shared units: caml_applyN, caml_sendN, caml_curryN...
std::vector<cmm::Phrase> generic_functions(bool shared, const std::vector<const cmx_format::UnitInfos*>& units);
// entry_point namelist: caml_program, calling each unit's entry in turn
cmm::Phrase entry_point(const std::vector<std::string_view>& namelist);
cmm::Phrase global_table(const std::vector<std::string_view>& namelist);
cmm::Phrase reference_symbols(const std::vector<std::string_view>& namelist);
// global_data name v, v already marshaled (Marshal.to_string v [])
cmm::Phrase global_data(std::string_view name, std::string_view marshaled);
cmm::Phrase frame_table(const std::vector<std::string_view>& namelist);
cmm::Phrase data_segment_table(const std::vector<std::string_view>& namelist);
cmm::Phrase code_segment_table(const std::vector<std::string_view>& namelist);
cmm::Phrase predef_exception(long i, std::string_view name);
cmm::Phrase emit_global_string_constant(std::string_view name, std::string_view value);

// Misc's overflow checks on OCaml's 63-bit ints
bool no_overflow_add(long a, long b);
bool no_overflow_sub(long a, long b);
bool no_overflow_mul(long a, long b);
bool no_overflow_lsl(long a, long k);
long log2(long n);

}  // namespace cppcaml::typing::cmm_helpers
