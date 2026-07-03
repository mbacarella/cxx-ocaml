# P2 PLAN: computed module coercions over modsig::Sig

Audience: an implementing agent (Opus). Prescriptive on WHAT and in what ORDER;
you own the HOW at each step. Follow the stage gates strictly — every stage
ends with the five harness gates green before the next begins.

## Mission

Replace the name-list guessing in `coerce_block` with a COMPUTED, verified
coercion (upstream `Includemod.modtypes` → `Tcoerce_*`, replayed by
`translmod.apply_coercion`), using the P1 substrate: `modsig::Sig` (ordered,
namespaced signature items with runtime positions) and `modsig::Env`.

Non-goals for P2: Env-driven path resolution (that is P3); value-type
inference changes (P4); performance work; refactors not on the coercion path.

## Read first (in this order)

1. `JOURNAL.md` sections "PLAN: REAL MODULE SYSTEM" (grep it) and
   "P1 DONE: modsig substrate" — the thesis, the P1 deliverables, and the
   triaged mismatch categories P2 must turn into fixes.
2. `cxx/include/cppcaml/modsig.hpp` — the whole file; it is small.
3. `typing/includemod.ml` lines ~360-440 (`is_runtime_component`,
   `simplify_structure_coercion`) and ~700-830 (`signatures`,
   `signature_components`, `pair_components`); `typing/typedtree.mli`
   `module_coercion` (~line 523). NOTE: coercion replay lives in
   `lambda/translmod.ml` (`apply_coercion`, line ~83), NOT `bytecomp/`.
4. `cxx/src/lambda.cpp`: `coerce_block` (~1962) and every call site
   (grep `coerce_block(`), the `build_module` ascription tail (grep
   `"A signature ascription"`), `msig_of_modtype` / `msig_of_cmi_sig`
   (grep `modsig P1 derivations`), and `pack_coerce` (~1841).
5. `cxx/src/cmi.cpp` `compute_coercion` (~line 500) — a WORKING prototype over
   `cmi::Signature` that P2 generalizes; note its `field_ns` name-guessing
   hack, which `modsig::Sig` makes unnecessary. Its CLI validator:
   `cxx/build/c++cmi src.cmi --coerce tgt.cmi` (see `src/tools/cppcmi_main.cpp`).

## Ground rules (from the project's working method)

- Oracle = `./ocamlc.opt` (check `file ocamlc.opt` first — it gets clobbered;
  relink with `make ocamlc.opt` if it is not an ELF executable).
- Harness scripts lack +x: run `bash cxx/harness/<script>.sh`.
- The five gates and their 2026-07-03 baselines:
  - `ocamlc_bootstrap.sh` — ok=284 fail=0
  - `lambda_parity.sh` — MATCH 411/741 (55.5%); diff list lands in
    `/tmp/.lambda_diff_files`
  - `reject_parity.sh` — exactly 1 false-reject (known foo.ml cmi-env artifact)
  - `stdlib_full_allours.sh` — MATCH
  - `ocamllex_selfhost.sh` — MATCH (byte-identical lexer.mll regeneration)
- `CPPCAML_MODSIG_CHECK=1` turns on the P1 parity asserts
  (`MODSIG-MISMATCH ...` on stderr). Keep them working; they are your canary.
- Do NOT edit `cxx/CMakeLists.txt` unless necessary; if you do, the static-lib
  link can break via a bad `/usr/bin/llvm-ar` — patch `rules.ninja` to the
  wrapper ar/ranlib (see memory `cppcaml-build-ar-gotcha`).
- Corpus sweeps: parallelize (`xargs -P 8`), pattern in `/tmp/msig_sweep.sh`
  from the P1 session, or rewrite it — it is 10 lines.
- Commit per stage with the gate numbers in the message.

## Design

### 1. `modsig::Coercion` (new, in modsig.hpp)

Mirror `Tcoerce_*` but keep it PURE DATA (names/indices/prim descriptors);
the replayer consults the Translator for Lambda values. Sketch:

```cpp
struct Coercion;
using CoercionPtr = std::shared_ptr<Coercion>;
struct Coercion {
  // One entry per TARGET runtime field, in target order.
  struct Field {
    enum class From : unsigned char {
      SrcField,   // read src block field `src_pos`   (Tcoerce_none/_structure)
      PrimStub,   // materialize an external as an eta-stub (Tcoerce_primitive)
      AliasValue, // materialize an elided module alias      (Tcoerce_alias)
    } from = From::SrcField;
    int src_pos = -1;          // SrcField: index in the SOURCE runtime block
    std::string name;          // the member name (all kinds; replay lookup key)
    std::string prim; int prim_arity = 0;  // PrimStub
    CoercionPtr sub;           // recursive coercion for a submodule (else null)
  };
  std::vector<Field> fields;
  bool identity = false;  // src == tgt field-for-field AND same runtime length
  bool ok = true;         // false: a required member is absent/ambiguous
  std::string error;      // dotted path of the offending member when !ok
};
```

### 2. `compute_coercion(const Sig& src, const Sig& tgt)` (modsig.hpp or a new
   modsig.cpp — header-only is fine, it needs no Translator state)

Semantics, matching `includemod.signatures`:

- Iterate `tgt.items`; skip non-runtime items EXCEPT check they exist in src
  by (ns, name) — for P2 it is acceptable to only pair runtime items and
  ignore type/modtype presence checking (the typer owns rejection; the back
  end owns layout). Document this divergence.
- For each runtime tgt item, find the src item by `(ns, name)` using
  `Sig::find` (last occurrence wins — shadowing already applied by `push`).
  - `NS::Unknown` on EITHER side pairs by name alone (the P1 flat-splice
    escape hatch). Count these pairings — see "trust gating" below.
  - Found, src runtime: `Field{SrcField, src->pos}`. If both are Modules with
    non-null `sub` Sigs, recurse: `sub = compute_coercion(*ssub, *tsub)`;
    propagate `!ok` upward with `error = name + "." + sub.error`; drop `sub`
    if the recursion yields identity.
  - Found, src `is_prim` value, tgt runtime value: `Field{PrimStub, prim,
    arity}` — requires prim name/arity on the Item. **Extend `modsig::Item`
    with `std::string prim; int prim_arity;`** and populate it in
    `msig_of_cmi_sig` (from `SigValue::prim/prim_arity`), in
    `msig_of_modtype`'s `Psig_primitive` branch (from `pd.prims[0]` + arity
    from the arrow type — copy the arity walk from `build_module`'s
    `Pstr_primitive` handler), and in `build_module` for struct-side
    externals (currently NOT pushed into cursig at all — add a no-slot
    `Item{Value, name, runtime=false, is_prim=true, prim, arity}` in the
    `Pstr_primitive` branch so the coercion can see it).
  - Found, src no-slot Module (elided alias, `runtime=false`):
    `Field{AliasValue, name}` — replay reads `module_alias_[name]`.
  - Not found: `ok=false, error=name`. NEVER silently emit a raw block —
    but see trust gating: at the CALL SITE, `!ok` falls back to the legacy
    path in early stages, then becomes a hard error at the end of P2.
- `identity` per `simplify_structure_coercion` + PR#5098: fields are
  `0,1,2,...` with no subs AND `src.runtime_len() == tgt.runtime_len()`.

Unit-test this pure function FIRST (stage 1) before touching integration.

### 3. Replay: `apply_msig_coercion` (Translator method in lambda.cpp)

Given `LamPtr mv`, the coercion, and the context, produce the coerced value.
CRITICAL: emit the SAME Lambda shapes the existing `coerce_block` emits, or
lambda-parity churns:

- General case (translmod `apply_coercion` Tcoerce_structure):
  `(let (id = mv) (makeblock 0 f0 f1 ...))` where `fi` is
  `Pfield(pos, Mutable)` — in our Lam that is `Prim::FieldMut` with
  `prim_arg = pos` (copy from the existing coerce_block, ~line 1962+).
- The FUSION optimization: when `mv` is a freshly built
  `(let <binds> (makeblock 0 v0 v1 ..))`, rebuild the makeblock args in
  target order instead of materializing a second block. `coerce_block`
  already does this (lines ~1975-2019) — lift that logic, do not duplicate.
- PrimStub fields: `prim_stub({prim, arity})` (existing helper, ~4526).
- AliasValue fields: `module_alias_[name]` (the build_module tail already
  does exactly this — read that code).
- Sub-coercions: recurse on the field read.
- `identity` → return `mv` unchanged.

### 4. Trust gating (the migration safety valve)

A computed coercion is TRUSTED only when derived from fully-namespaced Sigs:

```cpp
bool trusted(const Sig& s) {  // no Unknown items, recursively
  for (auto& it : s.items) {
    if (it.ns == NS::Unknown) return false;
    if (it.sub && !trusted(*it.sub)) return false;
  }
  return true;  // note: empty Sig is trusted-but-useless; treat as untrusted
}
```

Call-site policy per stage:
- Stage 2 (shadow): compute + replay into a SCRATCH value, structurally
  compare with the legacy path's output (compare the Lam trees; write a
  small lam_equal or print-and-compare using the existing dump printer),
  report `COERCE-DIVERGE <site> <path>` to stderr under
  `CPPCAML_COERCE_CHECK=1`. ALWAYS return the legacy result.
- Stage 3+ (switch): if trusted && ok → use computed; else legacy fallback,
  counting fallbacks under the env flag so you can measure coverage.
- End state: untrusted sigs should be rare (flat imports only); absent
  member on a trusted pair = compile error with the member path.

## Stages

### Stage 0 — re-baseline

Run all five gates, record numbers (they drift; do not trust the ones above
blindly). Rebuild first: `cd cxx/build && ninja c++ocamlc c++lambda c++cmi`.
Also run the P1 sweep (`CPPCAML_MODSIG_CHECK=1` over `testsuite/tests`) and
save the mismatch list; P2 stages must not GROW it.

### Stage 1 — Coercion type + compute_coercion + unit tests

- Add `modsig::Coercion`, `compute_coercion`, `trusted`, and the Item prim
  extension (populate at the three derivation sites listed above).
- Tests. Two layers:
  a. Pure C++ cases — hand-build src/tgt Sigs covering: identity; reorder;
     narrowing (drop members); prim→val eta-stub; alias materialization;
     nested submodule reorder (sub non-identity); cross-ns same-name pairs
     (`class c` + `let c` in src, tgt wants the value `c` → must pick the
     VALUE by namespace, never the class); Unknown-side pairing; absent
     member (`ok=false`, dotted error path); PR#5098 (identity fields but
     src longer than tgt → NOT identity). Wire into whatever test binary
     exists (`c++infer-test` pattern in `cxx/build/`) or a tiny new
     `modsig_test` main — if adding a CMake target, remember the ar gotcha.
  b. Oracle-shape validation: extend `c++cmi src.cmi --coerce tgt.cmi` to run
     modsig-based coercion (via `msig_of_cmi_sig` — needs a CmiFile, so this
     lives in cppcmi_main or a lambda.cpp-exposed helper) NEXT TO the legacy
     `cmi::compute_coercion`, and diff the two on stdlib .cmi pairs, e.g.
     compile a small `module M : SIG = struct ... end` pair of .cmis with the
     oracle. Divergences from the field_ns hack (ambiguous names) should
     resolve in modsig's favor — verify by hand against `ocamlc -dlambda`.
- Gate: gates unchanged (nothing is wired in yet); unit tests green.
- Commit.

### Stage 2 — shadow mode at the build_module ascription tail

The build_module tail (`if (coerce)` block, grep `"A signature ascription"`)
is the richest, best-understood site: src Sig = `cursig` (exact, just built),
tgt Sig = derive from what produced `coerce`:

- `Pstr_module`/unit-level `.mli` coercion passes `coerce_sig`
  (a `cmi::Signature*`) → tgt = `msig_of_cmi_sig`.
- `(struct : S)` / `module M : S =` pass `coerce` derived from
  `sig_layout(mt)` → thread the `ModuleType*` (or the already-derived
  `modsig::SigPtr`) down as a NEW optional parameter next to `coerce`
  (P1 already added `msig_out`; same pattern). Do NOT re-derive from the
  name list — that would re-import the flat semantics.

Under `CPPCAML_COERCE_CHECK=1`: compute, replay, compare with the legacy
result, report divergences. Expect divergences EXACTLY where P1's triage
found latent bugs (cross-ns collapse: value/class, module/exception,
module/typext). For each divergence, confirm which side matches
`ocamlc -dlambda` on a minimal repro before classifying. Keep a list.

- Gate: all five gates unchanged (shadow mode must not change behavior);
  self-host paths (stdlib_full, ocamllex, bootstrap) run with
  `CPPCAML_COERCE_CHECK=1` report ZERO divergences (they were MODSIG-clean in
  P1, so any divergence there is a replay bug — fix before proceeding).
- Commit.

### Stage 3 — switch the ascription tail; fix the cross-ns collapse

Two coupled changes, one commit, because the second is what the first fixes:

1. Ascription tail uses the computed coercion when trusted && ok (legacy
   fallback otherwise, counted).
2. Make the natural export block itself namespaced: `export_names`' flat
   erase-shadowing in `add_export_val` collapses cross-ns same-names
   (`class c` + `let c` → one slot; oracle emits two — verified in P1, see
   JOURNAL). Change the shadow-erase in `add_export_val` to only erase when
   `cursig` says the incoming item SHADOWS the old one (same ns, or Unknown
   pairing rule) — i.e. drive `exports`/`export_names` bookkeeping from
   cursig instead of flat name equality. `cursig` and `export_names` must
   stay index-aligned (P1's end-of-build assert enforces this — keep it on
   in your sweeps).

   COUPLED SURFACES to update in the same commit, or the .cmi and the block
   drift apart:
   - the .cmi WRITER's shadowing rule: `cmi::cmiw::dedup_shadowed_fields`
     (name-keyed today) must become namespace-aware, or the .cmi field list
     for `class c + let c` stays one short while the block has two fields.
     Find every writer-side consumer (grep `dedup_shadowed_fields`).
   - `struct_export_names` (the static AST walk, grep it) and
     `sig_layout`'s `dedup_keep_last` — same rule, same fix, OR route their
     consumers through msig-derived layouts. Prefer deriving from msig and
     deleting the flat walk where feasible; keep the P1 asserts as the
     equivalence proof for the paths you do not convert.
- Repros that MUST flip to oracle-identical `-dlambda` (add each as a
  micro-test file, keep them under `cxx/harness/` or a tests dir):
  - `class c = object method m = 1 end let c = 42 let use () = (new c)#m + c`
    (P1 verified: oracle 3-field block, ours was 2 and miswired `use`)
  - `module E = struct let v = 1 end exception E let f () = raise E
     let g () = E.v` (oracle 4 fields)
  - `module F = struct end type t = .. type t += F` (typext/module pair)
- Gate: five gates — lambda parity must NOT drop; expect it flat or +small
  (most cross-ns files are expect-style SKIPs; wins show on the micro-tests
  and on `patmatch.ml` if it flips). reject stays at the 1 known artifact.
  stdlib/ocamllex/bootstrap green. P1 sweep: build_module mismatch lines for
  the cross-ns files DISAPPEAR (the layouts now agree with msig).
- Commit.

### Stage 4 — remaining coerce_block call sites, one commit each

Convert in this order (each: derive src/tgt Sigs, computed coercion when
trusted, legacy fallback, gates, commit):

1. `(struct .. : S)` / `(me : S)` module-constraint paths in
   `compile_module_expr` (~13153-13185, grep `"(struct .. : S)"`) — src =
   `msig_out` from the inner `build_module` / `msig_of_module_expr` for
   non-struct me; tgt = `msig_of_modtype(*pc->mt)`.
2. mli-driven functor-result coercion (grep `pending_functor_coerce_`) —
   tgt available only as a name list from `mli_functor_results_`; P2 option:
   derive tgt from the unit's own .mli cmi (`coerce_sig` machinery) instead
   of the name list; if that is too tangled, leave on legacy and note it.
3. Functor parameter coercion (two sites, grep `nested_cmi_sig(psig, nm)`
   and the struct-literal-member reorder above it, ~12847/12899) — tgt =
   `msig_of_modtype(param sig)` or `msig_of_cmi_sig(param cmi sig)`;
   src = argument's Sig: from `menv_.lookup_module` for a local module path,
   `msig_out` for a struct literal, `msig_of_cmi_sig` for a stdlib path.
   This is where P1's Env starts paying: prefer Env lookup over
   `layout_vec`/`arg_layout` and fall back when Env misses (count misses —
   they are P3's worklist).
4. `pack_coerce` (~1841, first-class modules) — tgt =
   `msig_of_modtype` of the package type; src = `msig_of_module_expr`.
   The targetint external-stubbing special case collapses into PrimStub
   fields — delete the ad-hoc loop once the computed path covers it (verify
   with `testsuite/tests/*targetint*` and grep JOURNAL for `targetint`).
5. Recursive-module ascriptions (grep `coerce_block(body,
   module_result_layout(*rm.bodyme)`) — src = `msig_of_module_expr(bodyme)`,
   tgt = `msig_of_modtype(*rm.sig)`.

After all sites: `coerce_block`'s name-list body should be reachable ONLY
from fallbacks. Leave it in place (P3 removes it), but add a counter/flag
log so its remaining traffic is visible.

### Stage 5 — the menhir gate

- Differential dumpobj: compile `parsing/parser.ml` (menhir-generated) with
  c++ocamlc and with the oracle; `tools/dumpobj` both .cmo files; diff
  (normalize as the harnesses do — see `lambda_parity.sh`'s `norm()` for the
  stamp convention). Grep JOURNAL for `menhir wild jump` / `MAKEBLOCK
  15-vs-23` / `INCREMENTAL_ENGINE` for the historical failure signatures —
  submodule MAKEBLOCK arity/order must now match the oracle.
- The plan's "bootstrap `-c` crash" (from the 2026-06-26 plan text) may
  already be fixed — `ocamlc_bootstrap.sh` is green at ok=284. Verify by
  reading the harness to see whether menhir-path files are in those 284; if
  a known crash repro exists in JOURNAL, re-run it; otherwise note "n/a".
- Gate: dumpobj diff clean on parser.cmo (or diffs explained + strictly
  smaller than at stage 0); all five gates green.

### Stage 6 — wrap up

- Full P1+P2 sweep (`CPPCAML_MODSIG_CHECK=1 CPPCAML_COERCE_CHECK=1`) over
  the corpus: mismatch/divergence list must be ⊆ stage 0's, minus the
  cross-ns and clobbering categories fixed in stage 3/4.
- Update `JOURNAL.md` ("P2 DONE" section: what changed, gate numbers,
  fallback counters, what remains for P3) and the memory
  `module-system-active-goal`.
- Final commit.

## Pitfalls (each burned someone at least once)

- **Runtime-component rule**: prim values (`external`), types, modtypes,
  class types, and elided (`Mp_absent`) module aliases take NO field. The
  AST-signature path distinguishes `Psig_value` from `Psig_primitive` —
  already handled in P1; keep it that way.
- **Namespaces that share a lexical space**: exceptions/typext ctors are
  capitalized like modules; classes are lowercase like values. Pair by
  (ns, name) ONLY; name-only pairing is the bug being fixed.
- **Shadowing is keep-LAST at its LAST position** (not first), per namespace.
  `cursig`/`export_names` index alignment is asserted at build_module exit —
  run sweeps with `CPPCAML_MODSIG_CHECK=1` after every stage.
- **`module_result_layout`'s empty-ascription fallback**: `X : sig end = ...`
  falls back to the struct's fields (JOURNAL P1 section, MPR7761 analysis).
  When deriving tgt Sigs, an EMPTY ascribed sig means an EMPTY layout — do
  not inherit this fallback into msig-derived targets; but also do not "fix"
  the legacy function itself in P2 (it feeds non-coercion consumers; P3).
- **`module_layout_` is globally keyed and gets clobbered** (MPR7761): never
  add new dependencies on it; prefer `menv_`/Sig navigation for anything new.
- **Blocks read with Mutable semantics**: coercion field reads are
  `Pfield(pos, Mutable)` = `Prim::FieldMut`, not immutable field reads —
  the oracle's `-dlambda` shows `field_mut`; using `field_imm` breaks parity.
- **Do not re-order evaluation**: the fusion path must keep the original
  binding order and seq segments; only the makeblock ARG order changes.
- **`cmi::CmiFile::load` caches by path**; `mt_sig` pointers borrow from the
  loaded file — keep the CmiFile alive while you hold `cmi::Signature*`
  (see `mt_sig_x`'s `keep` vector for the cross-unit pattern).
- **stdlib .mli in-place compiles corrupt gitignored core .cmi files** —
  never compile `stdlib/*.mli` in place; copy to a temp dir (memory
  `stdlib-inplace-compile-corruption` has the repair recipe).
- **Depth guards**: recursive module types exist (`strongly_connected_
  components`-style). Every new recursion over Sig/ModuleType needs a depth
  cap (P1 used 24) — source.ml WILL find unguarded recursion.
- **is_a_functor replay**: functor coercions eta-expand
  (`translmod.apply_coercion_result`). P2 only needs STRUCTURE coercions at
  the listed sites; if you find yourself needing Tcoerce_functor replay for
  the param sites, coerce the ARGUMENT value (structure coercion) as the
  existing code does, rather than wrapping the functor.

## Definition of done

1. All five gates ≥ stage-0 baselines; lambda parity not lower.
2. The three cross-ns micro-repros compile to oracle-identical `-dlambda`.
3. Ascription/constraint/functor-param/pack/recmodule coercions go through
   `compute_coercion` on trusted Sigs; fallback counter near zero on the
   self-host paths (stdlib, ocamllex, bootstrap) — print it in the logs.
4. An absent member on a trusted pair is a hard, path-qualified error, never
   a silent raw block.
5. Differential dumpobj on menhir parser.cmo clean (or strictly improved,
   documented).
6. JOURNAL + memory updated; per-stage commits with gate numbers.
