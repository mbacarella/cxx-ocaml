// The Lambda intermediate representation (OCaml's lambda/lambda.mli) and the
// `-dlambda` printer.  Stage after typing: translate the parsetree (with the
// inference results that are load-bearing here -- value kinds, etc.) into Lambda
// and validate byte-for-byte against `ocamlc -dlambda` over the corpus, the same
// dump-parity loop that drove lex/parse/type.
//
// Slice 1: top-level value bindings of constants/simple exprs -> the module's
// (setglobal L<name>! (let (...) (makeblock 0 ...))) form.
#pragma once

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "cppcaml/ast.hpp"
#include "cppcaml/dbgenv.hpp"

namespace cppcaml::lambda {

// An unresolvable module reference: the head module is not bound locally and
// has no .cmi on the include path.  ocamlc's Env raises "Unbound module" here;
// carrying the head and source span lets the driver print the identical report
// (File/line/chars + excerpt + carets) and exit 2 like ocamlc, instead of
// silently compiling garbage (bootstrap #13: typeopt.ml compiled with no
// lambda.cmi emitted unresolved-ctor code that crashed the built compiler).
struct UnboundModuleError : std::runtime_error {
  std::string head;
  int line;                  // 1-based source line of the reference
  int col_start, col_end;    // 0-based columns within that line
  UnboundModuleError(std::string h, int l, int cs, int ce)
      : std::runtime_error("Unbound module " + h),
        head(std::move(h)), line(l), col_start(cs), col_end(ce) {}
};

// Value representation kind (lambda/lambda.mli value_kind), as printed by
// -dlambda: Pintval -> "[int]", Pfloatval -> "[float]", Pgenval -> (nothing).
enum class ValueKind { Gen, Int, Float, Boxedint32, Boxedint64, Nativeint };

struct Lam;

// Non-owning handle to a Lam node.  Every Lam lives in a process-lifetime bump
// arena (see lambda.cpp) that is never freed mid-compile, so ownership is
// meaningless: a Lam is alive for the whole process regardless of how many
// handles point at it.  Dropping shared_ptr removes the atomic refcount on
// every copy (the translation + simplify passes copy handles constantly), the
// per-node control block, and ~Lam teardown.  The interface mirrors the
// shared_ptr subset the codebase actually used (operator->/*/bool/==, .get(),
// .reset(), null default-construction) so the use sites compile unchanged; only
// the factory sites that used to make_shared<Lam> now call lam_alloc/
// lam_alloc_copy below.
struct LamPtr {
  Lam* p_ = nullptr;
  constexpr LamPtr() noexcept = default;
  constexpr LamPtr(std::nullptr_t) noexcept {}
  explicit constexpr LamPtr(Lam* p) noexcept : p_(p) {}
  Lam* operator->() const noexcept { return p_; }
  Lam& operator*() const noexcept { return *p_; }
  Lam* get() const noexcept { return p_; }
  explicit constexpr operator bool() const noexcept { return p_ != nullptr; }
  void reset() noexcept { p_ = nullptr; }
  friend constexpr bool operator==(LamPtr a, LamPtr b) noexcept { return a.p_ == b.p_; }
  friend constexpr bool operator!=(LamPtr a, LamPtr b) noexcept { return a.p_ != b.p_; }
  friend constexpr bool operator<(LamPtr a, LamPtr b) noexcept { return a.p_ < b.p_; }
};

// Allocate a Lam from the process-lifetime arena (see lambda.cpp).  lam_alloc
// default-constructs; lam_alloc_copy copy-constructs from an existing node.
LamPtr lam_alloc();
LamPtr lam_alloc_copy(const Lam& src);

// An identifier with a stamp (normalized by first dump appearance, like the
// typedtree harness).  name "" for compiler temporaries shown as *match*.
struct Ident {
  std::string name;
  int stamp = 0;
  bool temp = false;  // a compiler-generated binder (printed *name*/stamp)
  // ocamlc's Ident.create_scoped class (module/class binders, unpack pattern
  // vars) -- Ident.compare sorts Scoped BEFORE Local regardless of stamp, so
  // a closure's free-var layout puts these first (bytegen Ident.Set.elements).
  bool scoped = false;
  // A Translcore-phase temp whose stamp ocamlc allocates only AFTER every source
  // ident of the unit (typing stamps all source idents first).  Our single fresh()
  // counter interleaves the two, so such a temp gets an early stamp; flag it so a
  // captured one sorts after the source locals in a closure's free-var layout,
  // matching ocamlc's source-before-translation-temp stamp order.
  bool late = false;
  // Where this binder sits in a closure's free-var layout, when that is NOT its
  // own stamp.  ocamlc's matcher does not always mint the binder it uses for a
  // materialized match column: name_pattern hands back the first row's own
  // variable, whose stamp was allocated with the source binders.  We keep a
  // distinct ident (ours also carries the arm's exit wiring) and record only
  // the layout position here.  0 = sort by stamp, as everything else does.
  int fv_order = 0;
};

// Lambda primitives we emit so far (printlambda spelling in the comment).
enum class Prim {
  Addint,    // +
  Subint,    // -
  Mulint,    // *
  Field,     // field n
  FieldImm,  // field_imm n
  Makeblock, // makeblock tag
  Setglobal, // setglobal id
  Global,    // global id  (as an arg of field)
  NotEqInt,  // !=
  EqInt,     // ==
  Makemutable, // makemutable tag (shape)  (ref / mutable record)
  FieldInt,    // field_int n   (deref of an immediate-contents ref)
  FieldMut,    // field_mut n   (deref of a pointer-contents ref)
  SetfieldImm, // setfield_imm n  (:= into an immediate-contents ref)
  SetfieldPtr, // setfield_ptr n  (:= into a pointer-contents ref)
  Offsetref,   // +:=n   (incr/decr)
  Offsetint,   // n+      (incr/decr on a mutable-local ref: assign r (1+ *r))
  Ccall,       // a C external call, printed by its C name (prim_id)
  IntCmp,      // integer comparison; spelling (< > <= >= == !=) in prim_id
  Raise,       // (raise e)
  Reraise,     // (reraise e)  (exception handler fall-through)
  RaiseNotrace,// (raise_notrace e)  (%raise_notrace: no backtrace recorded)
  Makelazyblock, // makelazyblock (Lazy_tag 246) / makeforwardblock (Forward_tag 250)
  Send,            // (send obj tag)        -- method dispatch (args: [obj, tag])
  FieldComputed,   // (field_computed o id) -- read an instance var by id (args: [obj, id])
  SetfieldComputed,// (setfield_imm_computed o id v) / setfield_ptr_computed (spelling in prim_id)
  Floatfield,      // (floatfield n r)      -- flat float-record field read
  SetFloatfield,   // (setfloatfield n r v) -- flat float-record field write
};

struct Lam {
  enum class K { Var, Mutvar, ConstInt, ConstChar, ConstFloat, ConstString, ConstBlock,
                 Apply, Function, Let, Letrec, Prim, Assign, IfThenElse, Sequence,
                 Switch, For, While, Try, Catch, Staticraise };
  K k;

  Ident var;                       // Var
  long long int_val = 0;           // ConstInt
  std::string str_val;             // ConstFloat (verbatim) / ConstString

  LamPtr fn;                       // Apply
  std::vector<LamPtr> args;        // Apply / Prim

  // Function
  std::vector<std::pair<Ident, ValueKind>> params;
  ValueKind ret_kind = ValueKind::Gen;
  LamPtr body;
  std::string inline_attr;  // "never_inline"/"always_inline" from [@inline ...], or ""
  // The binding carried [@tail_mod_cons]: this function is a TMC candidate and
  // the late Tmc.rewrite pass gives it a destination-passing twin.
  bool tmc = false;

  // Let: a group of bindings (kind shown as =[kind]) then a body
  // alias: the Llet Alias kind (printed `=a`), used for pattern-variable bindings
  // to a sub-term of the scrutinee (e.g. `Some y` where y is used more than once).
  // mut: the Llet Mutable kind (printed `=mut`), a mutable local variable (an
  // un-escaping `ref`); read via Mutvar (`*x`) and written via Assign.
  struct Binding { Ident id; ValueKind kind; LamPtr val; bool alias = false; bool mut = false;
                   bool strict_opt = false; };  // strict_opt: the StrictOpt kind, printed `=o`
  std::vector<Binding> bindings;

  // Prim
  Prim prim;
  int prim_arg = 0;                // field index / makeblock tag / offsetref delta
  std::string prim_id;             // global name (e.g. "Stdlib") / Ccall name / cmp op
  std::vector<ValueKind> blk_shape;  // makemutable block_shape (per-field kinds)
  bool downto_ = false;            // For: counts down

  // IfThenElse
  LamPtr cond, then_, else_;

  // Switch (Lswitch): scrutinee in `cond`; integer-constant and block-tag arms;
  // sw_default null => exhaustive (printed "switch*"), else "switch".
  // sw_numconsts/sw_numblocks: the matched TYPE's total const/block ctor counts
  // (ocamlc's sw_numconsts/sw_numblocks) -- the bytecode jump tables are sized
  // by these, with tags absent from the case lists jumping to the failaction.
  // -1 (the default) = dense: every tag 0..len-1 is present in the list, so the
  // list length is the table size (all pre-existing producers).
  // arity: how many constructor arguments upstream's cell for this tag binds
  // (one Alias let each, before Simplif substitutes them away).  Only the
  // max_raw budget of Lambda.make_key needs it -- see oc_make_key.  -1 = the
  // producer did not record it, 0 = a constant ctor or a failaction slot.
  struct SwitchCase { int tag; LamPtr body; int arity = -1; };
  std::vector<SwitchCase> sw_consts, sw_blocks;
  LamPtr sw_default;
  int sw_numconsts = -1, sw_numblocks = -1;
  // The source term this switch was built from -- Lambda.make_key keeps an
  // Lswitch's loc (see make_lam_key_rec), so switch identity is SOURCE
  // identity, not node identity.  Our matcher can compile one sub-matrix more
  // than once (upstream compiles it once and shares), and those copies must
  // key alike.  Set from the head pattern the switch discriminates; 0 = unset,
  // and then the node address stands in, as before.
  uintptr_t sw_srcid = 0;

  // Catch (Lstaticcatch): protected body in `cond`, handler in `then_`, static
  // exception id in `prim_arg`, handler-bound vars in `catch_vars` (with their
  // value kinds in catch_var_kinds when known, for the printer).
  // Staticraise (Lstaticraise): exit id in `prim_arg`, args in `args`.
  std::vector<Ident> catch_vars;
  std::vector<ValueKind> catch_var_kinds;
  // keep_catch: a static catch that must NOT be simplified away even if its exit
  // is raised only once.  ocamlc's expand_stringswitch builds its shared default
  // behind a catch inside Bytegen (after Simplif), so a single-use string-switch
  // default reaches the code generator as a live `(exit N)` -> `branchif`.  Our
  // string_switch builds the same structure during lambda generation, so this
  // flag exempts it from simplify_static_catches to match that layout.
  bool keep_catch = false;
  // gm_str_dflt: gmatch's string-column default catch (the fresh exit
  // expand_stringswitch's make_catch would create in Bytegen).  If arm wiring
  // leaves its handler a BARE `(exit j)` (the default arm was multi-use, so
  // upstream's Simplif would not have inlined it into the stringswitch fail
  // slot and make_catch would have reused the bare exit directly),
  // collapse_str_dflt_catches retargets the tree at j and drops the catch.
  bool gm_str_dflt = false;
  // gm_str_bind: gmatch's string-column arg binding (Bytegen bind_sw).  The
  // tree is built against an internal `switch` var so the enclosing split sees
  // exactly ONE nominal use (the stringswitch node's arg slot).
  // resolve_str_binds settles it where Bytegen would, at the very end of the
  // pipeline and so after every Simplif-level substitution: value still a Var
  // -> strip the let (bind_sw's Lvar no-op); otherwise a real Strict `switch`
  // let.  Until then inline_var_aliases must step over the node.
  bool gm_str_bind = false;
  // gm_str_node / gm_str_arm: skeleton of an expanded string-test tree
  // (Matching.do_make_string_test_tree).  gm_str_node marks every node the
  // expansion itself built; gm_str_arm additionally marks the nodes whose
  // `else_` is one of the switch's ACTIONS, in `sw` order.  The pair lets
  // share_string_trees walk an already-expanded tree back to its action slots
  // and apply Matching.share_actions_tree there -- upstream shares those
  // actions before Bytegen expands the node, but our leaves are still
  // `(exit aid)` arm placeholders at expansion time.
  bool gm_str_node = false;
  bool gm_str_arm = false;
  // gm_str_share: one of share_actions_tree's shared-action handler catches.
  // Like the gm_str_dflt catch it is built by a pass that upstream runs
  // OUTSIDE Bytegen's expansion, so a materialized bind_sw let sinks under it.
  bool gm_str_share = false;
  // gm_chunk: a half-match chunk catch (comp_match_handlers).  Upstream inlines
  // a single-use chunk exit only in Simplif -- AFTER Matching's bind_check/
  // lower_bind ran against the catch node (so column binds do NOT sink past the
  // chunk boundary).  We keep the catch through construction for the same
  // reason and inline single-use handlers in a late pass (inline_chunk_catches).
  bool gm_chunk = false;
  // gm_chunkc: a chunk catch that SURVIVED inline_chunk_catches (which clears
  // gm_chunk on the way).  wire_garms needs the provenance afterwards: a
  // following pm's catch wraps OUTSIDE the first pm's or-handlers.
  bool gm_chunkc = false;
  // gm_orp: a PENDING or-handler catch created at the or-alternation expansion
  // point in gmatch (matching.ml's precompile_or wraps the handler catch around
  // the compile of the pm at the depth where the or-column is consumed).  The
  // handler body and catch vars are filled in by wire_garms; if the arm turns
  // out single-use/unreachable (or the node sits at the body root, where the
  // wire-time root placement is already upstream's), the node is spliced out.
  bool gm_orp = false;
  // gm_orp_rest: the pending catch is for a BINDING col-0 or-row that still
  // had remaining columns when consumed (and is not a row-level alternation).
  // Upstream then keeps the trailing catch-all in do_split's `no` cluster, so
  // the idef catch wraps OUTSIDE the or-handler (`catch (catch sw with (5
  // arg) act) with (4) 0`, typedecl_immediacy).  A single-column or-row
  // (printpat's pretty_arg, cmt2annot's Tpat_var|Tpat_alias) and a row-level
  // `(P1,P2)|(P3,P4)` alternation (parmatch's compat: upstream's rest is
  // empty) instead absorb the catch-all into the or-matrix, ending with the
  // or-catch OUTERMOST; a var-free multi-column or can go either way
  // (parmatch's le_pat vs a plain 2-col probe), so only the binding case --
  // where every observed placement is inner -- sets this.  wire_garms uses it
  // to decide whether the pending catch may stay under the shared-default
  // catch.
  bool gm_orp_rest = false;
  // gm_deidc: the top-level shared-default catch (gmatch_top's deid).  Upstream
  // creates this catch at do_split's cons_next, OUTSIDE the whole or-pm compile
  // for a binding multi-column or-row -- a pending or-handler catch
  // (gm_orp_rest) found directly under it is at upstream's placement already
  // and must NOT be spliced out to re-wrap above it.
  bool gm_deidc = false;
  // from_alias: this node was substituted in place of a single-use pattern
  // binder (wrap_binders' inline of an alias let).  At the corresponding point
  // in ocamlc's pipeline (Matching/for_let, BEFORE Simplif) this position held
  // the binder's Lvar -- consumers that replicate pre-Simplif decisions (e.g.
  // assign_pat's non-var tuple-column binding in tail_tuple_exit) must treat a
  // tagged node as the variable it stood for.
  bool from_alias = false;
  // fused_strict: our translation elided a construct here that upstream's RAW
  // lambda still holds as an OPAQUE node when Matching's lower_bind runs (a
  // tuple-pattern let compiles through assign_pat/for_let into a staticcatch,
  // `_ -> true` to approx_present; only Simplif collapses it away).  Consumers
  // replaying pre-Simplif presence decisions treat a tagged node as present.
  bool fused_strict = false;
  // gm_facc: a pattern-matrix column whose lambda is a DEFERRED field access
  // (matching.ml passes get_expr_args' field-read expressions down unevaluated;
  // each compile entry then binds its first arg via arg_to_var/bind_match_arg).
  // gmatch materializes such a column when it reaches head position: bound to a
  // fresh var for that level, then Simplif's Alias count rule (0 drop / 1
  // substitute a fresh read / >=2 keep the let), so ctor arms and split-off
  // catch-all rows re-read the field independently like upstream's per-sub-pm
  // binding.  gm_facc_kind: the value kind for the materialized binding.
  bool gm_facc = false;
  ValueKind gm_facc_kind = ValueKind::Gen;
  // gm_guard_aid: a guarded row's `(if guard (exit aid ..) next)` test, built at
  // the gmatch leaf when the arm's action is aid-shared.  Upstream's leaf binds
  // the row's pattern vars around the WHOLE guarded action -- `(let binds (if g
  // rhs next))` -- and multi-use Alias binds survive Simplif in place, so when
  // the arm turns out single-use wire_garms rebinds the exit args above this
  // node instead of inline_exit's at-site rebind (`if g (let binds rhs) next`).
  int gm_guard_aid = -1;
  // gm_garm: a gmatch arm's leaf exit `(exit aid vars..)`, standing for the
  // arm's not-yet-wired handler.  Upstream compiles a single-use row's rhs
  // INLINE before the column binds are lowered around it, so lower_bind's
  // approx_present sees the real handler body (usually opaque); our exit
  // placeholder is transparent (args only) and would let a bind sink past the
  // point upstream stops at.  Materialization treats a flagged exit as opaque
  // when the bound var's only use is one of its args.
  bool gm_garm = false;
  // gm_dgrp_test: a call_switcher test that separates the FAIL interval (the
  // default chunk's own exit, mk_failaction_pos) from a case interval.
  // simplify_exits may later merge those two exits into one, and Simplif never
  // collapses an `if` with equal arms -- so collapse_equal_if must leave this
  // test alone however alike its arms end up looking (see mark_fail_tests).
  bool gm_dgrp_test = false;
};

// Alpha-normalized structural key of a Lambda term (the analog of
// Lambda.make_key): two terms with the same key are shared as one switch action
// by the bytecode emitter, matching ocamlc's Bytegen.Storer.  Returns "" for
// terms that ocamlc treats as Not_simple (closures / letrec / for / while, or
// more than max_raw nodes), so those are never shared.  Bound-var stamps are
// used literally (so distinct-var arms are conservatively NOT merged), which
// under-shares relative to ocamlc but never over-shares.  The node bound is the
// looser of the two: upstream re-counts an Alias-substituted body at each use
// where we count each node once.
//
// `exit_aware` controls whether static exits are distinguished by target.  The
// default (false) deliberately ignores Lstaticraise targets -- two exits "look
// alike" -- which is what the DEFAULT-exit-sharing call sites rely on (they
// compare arms against one known shared default).  Pass true wherever a
// switch/collapse dedups arbitrary action bodies that may BE or CONTAIN static
// raises: there, exit-blind keying would wrongly merge dispatches to DISTINCT
// handlers (a real miscompile -- ocamlc's Lambda.make_key keeps the exit id).
// Resolved once: both gates are read per Let / per Switch node.
inline const bool kNoKeyAlpha = cppcaml::dbg_env("NOKEYALPHA") != nullptr;
inline const bool kNoKeySwId = cppcaml::dbg_env("NOKEYSWID") != nullptr;
// NOSWSRCID keys a switch by node identity even when its source term is known.
inline const bool kNoSwSrcId = cppcaml::dbg_env("NOSWSRCID") != nullptr;
inline std::string make_lam_key_rec(const LamPtr& l, bool exit_aware, int& budget,
                                    std::map<int, int>& vmap, int& nv) {
  if (!l) return "_";
  using K = Lam::K;
  // make_key counts every node it walks and gives up past `max_raw`
  // (lambda.ml:441,450), so an oversized action is never shared -- upstream
  // stores it under a fresh index and duplicates it.
  if (--budget < 0) return "";
  switch (l->k) {
    case K::Function: case K::Letrec: case K::For: case K::While: return "";
    case K::Var: case K::Mutvar: {
      auto it = vmap.find(l->var.stamp);
      if (it != vmap.end())
        return std::string(l->k == K::Var ? "v$" : "m$") +
               std::to_string(it->second);
      return (l->k == K::Var ? "v" : "m") + l->var.name + "#" +
             std::to_string(l->var.stamp);
    }
    case K::ConstInt: return "i" + std::to_string(l->int_val);
    case K::ConstChar: return "c" + std::to_string(l->int_val);
    case K::ConstFloat: return "f" + l->str_val;
    case K::ConstString: return "s" + l->str_val;
    default: break;
  }
  std::string r = "(" + std::to_string((int)l->k);
  if (l->k == K::Prim) r += ":" + std::to_string((int)l->prim) + ":" + l->prim_id + ":" + std::to_string(l->prim_arg);
  // A ConstBlock's tag lives in prim_arg; without it `[0: 0]` and `[1: 0]`
  // (same fields, different tag) key alike and the switch Storer wrongly
  // merges their arms -- a miscompile (Ok () vs Error 0 collapsing to one).
  if (l->k == K::ConstBlock) r += ":" + std::to_string(l->prim_arg);
  if (exit_aware && l->k == K::Staticraise) r += ":X" + std::to_string(l->prim_arg);
  auto add = [&](const LamPtr& c) { if (c) { std::string k = make_lam_key_rec(c, exit_aware, budget, vmap, nv); if (k.empty()) { r = ""; } else if (!r.empty()) r += " " + k; } };
  // Lambda.make_key (lambda.ml:469) renames every let binder, so two arms that
  // differ only in their binder STAMPS key alike.  Value first (outer scope),
  // then the binder, then the body.
  if (l->k == K::Let && !kNoKeyAlpha) {
    // (stamp, shadowed index) -- -1 when the stamp was not already bound.
    std::vector<std::pair<int, int>> saved;
    bool bad = false;
    for (auto& b : l->bindings) {
      std::string vk = make_lam_key_rec(b.val, exit_aware, budget, vmap, nv);
      if (vk.empty()) { bad = true; break; }
      int y = nv++;
      r += " b" + std::to_string(y) + " " + vk;
      auto it = vmap.find(b.id.stamp);
      saved.push_back({b.id.stamp, it != vmap.end() ? it->second : -1});
      vmap[b.id.stamp] = y;
    }
    std::string bk;
    if (!bad) {
      bk = make_lam_key_rec(l->body, exit_aware, budget, vmap, nv);
      if (bk.empty()) bad = true;
    }
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
      if (it->second < 0) vmap.erase(it->first);
      else vmap[it->first] = it->second;
    }
    return bad ? std::string() : r + " " + bk + ")";
  }
  add(l->fn); add(l->cond); add(l->then_); add(l->else_); add(l->body); add(l->sw_default);
  for (auto& a : l->args) add(a);
  for (auto& b : l->bindings) { if (!r.empty()) r += " b" + std::to_string(b.id.stamp); add(b.val); }
  for (auto& sc : l->sw_consts) { if (!r.empty()) r += " C" + std::to_string(sc.tag); add(sc.body); }
  for (auto& sc : l->sw_blocks) { if (!r.empty()) r += " B" + std::to_string(sc.tag); add(sc.body); }
  if (r.empty()) return "";
  // make_key normalizes every location EXCEPT Lswitch's (lambda.ml:480), so two
  // switches built from different source terms never key equal however alike
  // they look.  The source term (sw_srcid) is this port's analog of that loc;
  // where the producer did not record one, node identity stands in -- and it is
  // what still separates two arms once the binder stamps stop separating them.
  if (l->k == K::Switch && !kNoKeySwId) {
    uintptr_t sid = l->sw_srcid && !kNoSwSrcId
                      ? l->sw_srcid : reinterpret_cast<uintptr_t>(l.get());
    r += ":@" + std::to_string(sid);
  }
  return r + ")";
}
inline std::string make_lam_key(const LamPtr& l, bool exit_aware = false) {
  int budget = 32;  // lambda.ml:441 `let max_raw = 32`
  std::map<int, int> vmap; int nv = 0;
  return make_lam_key_rec(l, exit_aware, budget, vmap, nv);
}

// make_lam_key with SCOPED static-exit ids.  An exit whose target Catch lies
// INSIDE the keyed term is canonicalized to that catch's binding order -- two
// separate compiles of one source action differ only in such fresh internal
// ids and must still key alike.  A FREE exit (an enclosing matcher's arm or
// default placeholder) keeps its literal id: keying those blind merges
// dispatches to DISTINCT handlers -- a real miscompile (a `W [Ta]` and a
// `W [Tb]` leaf differing only in their arm exits collapse to one action and
// the second arm vanishes).
inline std::string make_lam_key_scoped_rec(const LamPtr& l,
                                           std::map<int, int>& bound, int& next,
                                           std::map<int, int>& vmap, int& nv) {
  if (!l) return "_";
  using K = Lam::K;
  switch (l->k) {
    case K::Function: case K::Letrec: case K::For: case K::While: return "";
    case K::Var: case K::Mutvar: {
      // A binder normalized below (Lambda.make_key's make_key_generator);
      // anything else is free and keeps its literal ident.
      auto it = vmap.find(l->var.stamp);
      if (it != vmap.end())
        return std::string(l->k == K::Var ? "v$" : "m$") +
               std::to_string(it->second);
      return (l->k == K::Var ? "v" : "m") + l->var.name + "#" +
             std::to_string(l->var.stamp);
    }
    case K::ConstInt: return "i" + std::to_string(l->int_val);
    case K::ConstChar: return "c" + std::to_string(l->int_val);
    case K::ConstFloat: return "f" + l->str_val;
    case K::ConstString: return "s" + l->str_val;
    default: break;
  }
  std::string r = "(" + std::to_string((int)l->k);
  if (l->k == K::Prim) r += ":" + std::to_string((int)l->prim) + ":" + l->prim_id + ":" + std::to_string(l->prim_arg);
  if (l->k == K::ConstBlock) r += ":" + std::to_string(l->prim_arg);
  if (l->k == K::Staticraise) {
    auto it = bound.find(l->prim_arg);
    r += it != bound.end() ? ":B" + std::to_string(it->second)
                           : ":X" + std::to_string(l->prim_arg);
  }
  auto add = [&](const LamPtr& c) { if (c) { std::string k = make_lam_key_scoped_rec(c, bound, next, vmap, nv); if (k.empty()) { r = ""; } else if (!r.empty()) r += " " + k; } };
  if (l->k == K::Let && !kNoKeyAlpha) {
    // Lambda.make_key (lambda.ml:469) renames every let binder to a generated
    // ident and keys the body under it, so two arms that differ only in their
    // binder STAMPS key alike -- which is how ocamlc shares the two textually
    // identical `let op = sub_lexeme .. in PREFIXOP op` arms of lexer.ml.
    // Value first (outer scope), then the binder, then the body.
    // (stamp, shadowed index) -- -1 when the stamp was not already bound.
    std::vector<std::pair<int, int>> saved;
    bool bad = false;
    for (auto& b : l->bindings) {
      std::string vk = make_lam_key_scoped_rec(b.val, bound, next, vmap, nv);
      if (vk.empty()) { bad = true; break; }
      int y = nv++;
      r += " b" + std::to_string(y) + " " + vk;
      auto it = vmap.find(b.id.stamp);
      saved.push_back({b.id.stamp, it != vmap.end() ? it->second : -1});
      vmap[b.id.stamp] = y;
    }
    std::string bk;
    if (!bad) {
      bk = make_lam_key_scoped_rec(l->body, bound, next, vmap, nv);
      if (bk.empty()) bad = true;
    }
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
      if (it->second < 0) vmap.erase(it->first);
      else vmap[it->first] = it->second;
    }
    return bad ? std::string() : r + " " + bk + ")";
  }
  if (l->k == K::Catch) {
    // the id is in scope in the protected body (`cond`) only, not the handler
    auto prev = bound.find(l->prim_arg);
    int saved = prev != bound.end() ? prev->second : -1;
    bound[l->prim_arg] = next++;
    add(l->cond);
    if (saved >= 0) bound[l->prim_arg] = saved; else bound.erase(l->prim_arg);
    add(l->fn); add(l->then_); add(l->else_); add(l->body); add(l->sw_default);
  } else {
    add(l->fn); add(l->cond); add(l->then_); add(l->else_); add(l->body); add(l->sw_default);
  }
  for (auto& a : l->args) add(a);
  for (auto& b : l->bindings) { if (!r.empty()) r += " b" + std::to_string(b.id.stamp); add(b.val); }
  for (auto& sc : l->sw_consts) { if (!r.empty()) r += " C" + std::to_string(sc.tag); add(sc.body); }
  for (auto& sc : l->sw_blocks) { if (!r.empty()) r += " B" + std::to_string(sc.tag); add(sc.body); }
  if (r.empty()) return "";
  if (l->k == K::Switch && !kNoKeySwId) {  // see make_lam_key_rec
    uintptr_t sid = l->sw_srcid && !kNoSwSrcId
                      ? l->sw_srcid : reinterpret_cast<uintptr_t>(l.get());
    r += ":@" + std::to_string(sid);
  }
  return r + ")";
}
inline std::string make_lam_key_scoped(const LamPtr& l) {
  std::map<int, int> bound; int next = 0;
  std::map<int, int> vmap; int nv = 0;
  return make_lam_key_scoped_rec(l, bound, next, vmap, nv);
}

// Translate a structure into the module's Lambda term (the setglobal form).
// `module_name` is the capitalized file basename (e.g. "L0").
LamPtr translate_implementation(const ast::Structure& s, const std::string& module_name,
                                const std::string& stdlib_dir = "stdlib",
                                const std::string& file_name = "",
                                std::vector<std::string>* required_globals = nullptr,
                                const std::vector<std::string>* directive_files = nullptr);

// Print in -dlambda format (stamps normalized by first appearance).
void print_dlambda(const LamPtr& code, std::ostream& out);

// Render a constant node (ConstInt/Char/Float/String/Block) in printlambda's
// structured_constant syntax -- used by the bytecode `const` instruction printer.
std::string structured_constant(const LamPtr& c);

// Render a full bytecode `const` instruction line, matching printinstr.ml's
// `@[<10>\tconst@ %a@]` box: a wide constant breaks after `const` and indents
// to column 10, with internal wrapping that depends on that start column.
std::string const_instruction(const LamPtr& c);

// Extra -I directories searched (after the stdlib pattern) for a separately
// compiled local module's .cmi -- enables cross-module separate compilation.
void set_module_dirs(std::vector<std::string> dirs);
// -nopervasives: Stdlib is NOT implicitly opened, so a predefined exception
// resolves to its Predef global directly (GETGLOBAL) rather than through the
// Stdlib re-export field (GETGLOBALFIELD Stdlib, N).
void set_nopervasives(bool b);

}  // namespace cppcaml::lambda
