// Port of asmcomp/emitaux.ml and asmcomp/emitenv.mli: the common functions
// for emitting assembly text, as the text-printing back ends (arm64) use
// them.  (amd64's emitter keeps its own, on the X86 DSL.)
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppcaml/typing/linear.hpp"
#include "cppcaml/typing/mach.hpp"

namespace cppcaml::typing::emitaux {

// The assembly text emitted so far (Emitaux.output_channel)
extern std::string output;

void emit_string(std::string_view s);
void emit_int(long n);
void emit_char(char c);
void emit_nativeint(std::int64_t n);
void emit_int32(std::int32_t n);  // 0x%lx
bool macosx();
std::string symbol(std::string_view s);  // what emit_symbol emits
void emit_symbol(std::string_view s);
void emit_string_literal(std::string_view s);
void emit_string_directive(std::string_view directive, std::string_view s);
void emit_bytes_directive(std::string_view directive, std::string_view s);
void emit_float64_directive(std::string_view directive, std::int64_t x);
void emit_float32_directive(std::string_view directive, std::int32_t x);
void emit_size_directive(std::string_view symbol);
void emit_type_directive(std::string_view symbol, std::string_view ty);
void emit_nonexecstack_note();

// Domainstate (runtime/caml/domain_state.tbl): the fields' indices, the
// words of a stack's context
namespace domainstate {
enum Field : long { young_limit = 0, current_stack = 5, exn_handler = 6, c_stack = 8, dls_root = 42, extra_params = 65 };
constexpr long stack_ctx_words = 7;
}  // namespace domainstate
constexpr long stack_threshold = 32;  // Config.stack_threshold (words)

// Record live pointers at call points
struct FrameDebuginfo {  // Dbg_alloc of Debuginfo.alloc_dbginfo | Dbg_raise | Dbg_other of Debuginfo.t
  enum class K : std::uint8_t { Dbg_alloc, Dbg_raise, Dbg_other } k;
  std::vector<mach::AllocDbginfo> alloc;  // Dbg_alloc
  debuginfo::t dbg;                       // Dbg_raise, Dbg_other
};
inline FrameDebuginfo dbg_other(const debuginfo::t& d) { return {FrameDebuginfo::K::Dbg_other, {}, d}; }
inline FrameDebuginfo dbg_raise(const debuginfo::t& d) { return {FrameDebuginfo::K::Dbg_raise, {}, d}; }
inline FrameDebuginfo dbg_alloc(const std::vector<mach::AllocDbginfo>& a) { return {FrameDebuginfo::K::Dbg_alloc, a, {}}; }
void record_frame_descr(long label, long frame_size, std::vector<long> live_offset, FrameDebuginfo debuginfo);

struct EmitFrameActions {
  std::function<void(long)> efa_code_label;
  std::function<void(long)> efa_data_label;
  std::function<void(long)> efa_8;
  std::function<void(long)> efa_16;
  std::function<void(std::int32_t)> efa_32;
  std::function<void(long)> efa_word;
  std::function<void(long)> efa_align;
  std::function<void(long, std::int32_t)> efa_label_rel;
  std::function<void(long)> efa_def_label;
  std::function<void(const std::string&)> efa_string;
};
void emit_frames(const EmitFrameActions& a);

// Detection of functions that can be duplicated between a DLL and the
// main program (PR#4690)
bool is_generic_function(std::string_view name);

// CFI directives
bool is_cfi_enabled();
void cfi_startproc();
void cfi_endproc();
void cfi_remember_state();
void cfi_restore_state();
void cfi_adjust_cfa_offset(long n);
void cfi_def_cfa_offset(long n);
void cfi_offset(long reg, long offset);
void cfi_def_cfa_register(long reg);

// Emit debug information
void reset_debug_info();
void emit_debug_info_gen(const debuginfo::t& dbg,
                         const std::function<void(long file_num, const std::string& file_name)>& file_emitter,
                         const std::function<void(long file_num, long line, long col)>& loc_emitter);
void emit_debug_info(const debuginfo::t& dbg);

void reset();

void emit_named_text_section(std::string_view func_name, char prefix_char);

// Emitenv: the environment for emitting a function
struct GcCall {  // calls to caml_call_gc, emitted out of line
  long gc_lbl;         // Entry label
  long gc_return_lbl;  // Where to branch after GC
  long gc_frame_lbl;   // Label of frame descriptor
};
// Calls to caml_ml_array_bound_error.  In -g mode, we maintain one call per
// bound check site.  Without -g, we can share a single call.
struct BoundErrorCall {
  long bd_lbl;    // Entry label
  long bd_frame;  // Label of frame descriptor
};
struct FloatLiteral {  // pending floating-point literals
  std::int64_t fl;
  long lbl;
};
struct PerFunctionEnv {
  const linear::Fundecl* f;
  long stack_offset = 0;
  std::vector<GcCall> call_gc_sites;               // the newest first
  std::vector<BoundErrorCall> bound_error_sites;   // the newest first
  std::vector<FloatLiteral> float_literals;        // the newest first
};
inline PerFunctionEnv mk_env(const linear::Fundecl& f) { return PerFunctionEnv{&f}; }

}  // namespace cppcaml::typing::emitaux
