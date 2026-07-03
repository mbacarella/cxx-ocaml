# P3 PLAN: Env-driven module resolution + msig_of_module_expr

Audience: the implementing agent (Fable). Prescriptive on WHAT and in what
ORDER; the HOW is owned at each step. Every stage ends with the five harness
gates green before the next begins.

## Mission

Finish what P2 started: make the namespaced `modsig` substrate the *source of
truth* instead of a shadow of the flat heuristics.

1. `msig_of_module_expr` — a namespaced Sig for an ARBITRARY module
   expression (today only structures-in-construction, modtypes, and .cmi
   signatures have one).
2. Namespaced `include` splice — the dominant producer of `NS::Unknown`
   items, which un-trusts `cursig` and forces the ascription tail's counted
   legacy fallback (stage-0 bootstrap traffic: 126 computed / 129 legacy /
   5 constraint-legacy).
3. Convert the four coerce sites P2 deferred (functor-param, functor-result,
   pack/constraint, recursive-module) onto `compute_coercion` +
   `apply_msig_coercion`.
4. Env-scoped path resolution: `menv_` lookup becomes the first (then only)
   resolver for dotted module paths, killing the unscoped `module_layout_`
   bare-key clobbering (MPR7761), the source.ml abstract-modtype
   heuristic-vs-heuristic divergence, and the M.M self-nesting corner —
   the 9 files / 35 lines left in the P1 sweep.
5. Retire the UPDATE-70 `module_head_resolvable` fail-fast: `Unbound module`
   falls out of Env/cmi lookup failure, not a bolted-on presence check.

Non-goals for P3: value-level type inference (P4 / Any-removal), performance,
removing `module_layout_` writes that feed non-resolution consumers (only
retire READERS that Env can serve; delete the map wholesale only if it falls
out naturally).

## Read first

1. `JOURNAL.md` "P1 DONE" + "P2 DONE" sections — esp. the P1 mismatch
   categories 2/3 (env clobbering, abstract-modtype) which are P3's targets,
   and "DEFERRED TO P3 (Stage 4 items 2-5)".
2. `cxx/include/cppcaml/modsig.hpp` — whole file (Sig/Env/Coercion/
   compute_coercion/trusted).
3. `cxx/src/lambda.cpp` landmarks (line numbers at P2-end, commit 19d3d4a76c):
   - `module_layout_` (573), `menv_` (603), coverage counters (610),
     `coerce_report` (624).
   - `module_head_resolvable` (1705), `unbound_module` (1729) and their
     callers (10557 field-read resolution, 13467 include resolution).
   - `register_sig_layouts` (2098), `sig_layout` (12419), `msig_of_cmi_sig`
     (12515), `msig_of_modtype` (12582), `msig_of_cmi_signature` (12692),
     `apply_msig_coercion` (12734), `try_computed_constraint` (12821).
   - Deferred coerce sites: functor-param `coerce_block` (13028),
     constraint tail (13345), `module_result_layout` (13515),
     functor-result (14603), include splice pushing `NS::Unknown`
     (14656-14692), recursive-module reorders (14921/14928), inner
     submodule reorders inside `coerce_block` (2023-2064).
   - Env frames: push 13622, pop 15013; binds 13807/14314/14389;
     the one `lookup_module_path` consumer 14319 (P1 check only).
4. Upstream mirrors: `typing/env.ml` (`lookup_module`, scoping),
   `lambda/translmod.ml` (`transl_module` on Tmod_apply/Tmod_constraint —
   what src Sigs mean per module-expr shape).

## Ground rules (unchanged from P2)

- Oracle = `./ocamlc.opt`; check `file ocamlc.opt` before trusting it.
- Harnesses: `bash cxx/harness/<script>.sh`; corpus sweeps with `xargs -P 8`.
- The five gates and 2026-07-03 baselines (re-verified at P3 stage 0):
  bootstrap ok=284 fail=0; lambda parity MATCH 411/741 (55.5%);
  reject_parity exactly 1 false-reject; stdlib_full_allours MATCH;
  ocamllex_selfhost byte-identical.
- Stage-0 coercion coverage (bootstrap path, `CPPCAML_COERCE_CHECK=1
  CPPCAML_COERCE_LOG=...`): 126 computed / 129 legacy / 5 constraint-legacy.
- Stage-0 modsig sweep: 35 mismatch lines / 9 files, all triaged (JOURNAL P2
  section): env-clobbering + source.ml abstract-modtype.
- `CPPCAML_MODSIG_CHECK=1` asserts stay green on self-host paths at every
  stage.
- Don't edit `cxx/CMakeLists.txt` casually (llvm-ar link breakage; see memory
  `cppcaml-build-ar-gotcha`). Never compile `stdlib/*.mli` in place.
- Commit per stage with gate numbers.

## Design

### 1. `msig_of_module_expr(const ModuleExpr&, int depth)` (lambda.cpp)

Returns `modsig::SigPtr` (or null = "don't know"; callers treat null/untrusted
as legacy-fallback). Shapes:

- `Pmod_ident P` — resolve P via `menv_` first (scoped; handles functor
  params and local submodules), then cmi (`msig_of_cmi_sig` on the loaded
  unit / dotted navigation into nested Module items). This is the same
  resolution ladder `sig_layout`'s Pmty_ident case walks, but SCOPED.
- `Pmod_structure` — already exists: `build_module` threads the built
  `cursig` out via `msig_out`. Reuse; do not re-derive.
- `Pmod_constraint (me : mt)` — `msig_of_modtype(mt)` (the constraint wins).
- `Pmod_apply F(A)` — `msig_of_module_expr(F)`'s Item must be a functor;
  return its `functor_result`. (Generative `F()` same.) No substitution of
  the argument into the result in P3 — if the result Sig depends on the
  param (rare for LAYOUT purposes), return null rather than guess.
- `Pmod_functor (X : MT) -> body` — a Sig containing the functor Item
  (param = msig_of_modtype(MT), result = msig_of_module_expr(body)).
- `Pmod_unpack` — from the package modtype when annotated, else null.

Depth-guard every recursion (P1 used 24). Shadow-check: with
`CPPCAML_MODSIG_CHECK=1`, at each `module_result_layout(me)` call site where
a msig is also derivable, assert `msig->runtime_names() == layout` (module
`module_result_layout`'s KNOWN empty-ascription fallback: an empty ascribed
sig legitimately disagrees — skip the assert there, per the P2 pitfall).

### 2. Namespaced include splice

At the `Pstr_include` site (14656+): derive `msig_of_module_expr(pin->expr)`.
When non-null and trusted, push each included item into `cursig` with its
REAL namespace (and `sub`/prim metadata carried over) instead of
`NS::Unknown`; keep the Unknown path as fallback. `add_export` /
`add_export_val` grow a namespaced variant that accepts the full Item (they
already take an NS since P2 — extend to carry sub-Sig + prim so shadowing and
later coercion see the same fidelity as directly-declared members).

This is the highest-leverage change: it converts `cursig` from untrusted to
trusted for every structure containing an include, flipping the ascription
tail from legacy to computed. EXPECT the legacy counter to drop sharply;
measure and record.

### 3. Deferred coerce sites → computed

One commit each, in this order (least to most entangled):

1. **Functor result** (14603): src = body's `msig_out` (a structure) or
   `msig_of_module_expr`, tgt = `msig_of_modtype(result_mt)`.
2. **Functor param** (13028): the coercion applied to the ARGUMENT value at
   apply time: src = `msig_of_module_expr(arg)`, tgt = param's modtype Sig
   (registered at 13807 already). Structure coercion only — do not wrap the
   functor (P2 pitfall).
3. **Pack / constraint** (13345 + pack_coerce): extend
   `try_computed_constraint` coverage to the pack site (src Sig from
   msig_of_module_expr of the packed expr).
4. **Recursive modules** (14921/14928 + snapshot machinery 13108/13131):
   src/tgt both derivable from the rec-binding's ascribed modtypes; the
   snapshot/restore of `module_layout_` stays until Stage 4.

Each conversion: computed when both Sigs trusted AND every field replays
(`apply_msig_coercion` non-null), else counted legacy (`coerce_report` with a
site tag, e.g. `functor-result-legacy`). ZERO-divergence shadow first where
cheap: compute both, compare the emitted Lambda (the P2 S2 pattern), then
switch.

### 4. Env-scoped path resolution

- Bind EVERY module-shaped binding into `menv_` (P1 binds structures,
  functor params' modtypes, rec modules; audit for gaps: module aliases,
  `open` — an open pushes the opened Sig's Module items into the frame,
  include — ditto after (2)).
- New resolver `resolve_module_sig(dotted) -> SigPtr`: `menv_.
  lookup_module_path` first, then the cmi ladder. Route the READERS of
  `module_layout_` on compile paths (1675-1775 block, 10524/10544,
  13766-13768, 13886) through it; keep `module_layout_` as a shadow-checked
  fallback until the sweep is clean, then make Env authoritative where the
  two disagree (MPR7761: Env is RIGHT, the flat map is clobbered).
- `module_head_resolvable` (1705) becomes: Env lookup || cmi exists. The
  ad-hoc ladder (module_ident_/module_alias_/opened_ scans) collapses into
  Env bindings added above. `unbound_module` unchanged in message, now
  triggered by lookup failure. reject_parity MUST stay at exactly 1
  false-reject — the known foo.ml artifact — and accept rate 99.9%.
- Targets to confirm fixed: patmatch MPR7761 (`[A B f]` vs clobbered
  `[f g]`), source.ml abstract-modtype recursion, pr6416/pr10693_bad/gpr1506
  M.M dotted keys, PR_4261.U'. Sweep must go 35/9 → (near) 0, and every
  remaining line individually triaged.

### 5. Trust widening

`modsig::trusted()` currently refuses any Sig containing `NS::Unknown`. After
(2) and (4), audit remaining Unknown producers (grep `NS::Unknown`); each
either gets a real namespace or a documented reason to stay Unknown. Goal:
self-host paths (bootstrap + stdlib + ocamllex) fallback counters ~0; print
the residue in the JOURNAL.

## Stages

### Stage 0 — re-baseline  [DONE 2026-07-03]

All five gates re-run at 19d3d4a76c: identical to P2-end baselines.
Coverage counters captured (above). `/tmp/p3_coerce_stage0.log` kept.

### Stage 1 — msig_of_module_expr, shadow mode

- Implement Design 1 + the shadow asserts under `CPPCAML_MODSIG_CHECK=1`.
- Corpus sweep + self-host paths: triage every mismatch; only the documented
  empty-ascription-fallback class may remain (skip-listed, not silenced).
- No behavior change; five gates identical. Commit.

### Stage 2 — namespaced include splice

- Design 2. Gates green; `CPPCAML_MODSIG_CHECK` sweep ⊆ stage 0's.
- Re-run coverage: record computed/legacy movement (expect the bulk of the
  129 legacy to flip). ZERO `COERCE-DIVERGE` under `CPPCAML_COERCE_CHECK=1`
  on self-host paths. Commit.

### Stage 3 — deferred coerce sites (four commits)

- Design 3 order. Per site: shadow compare where cheap, then switch, gates,
  commit with the site's counter movement in the message.

### Stage 4 — Env-scoped resolution

- Design 4. This is the risky stage: land it in two commits —
  (a) Env-first with shadow-checked flat fallback + sweep triage,
  (b) Env-authoritative + `module_head_resolvable` retirement.
- Gates after each; lambda parity may only go UP (411 is the floor); the
  9-file sweep must shrink; reject_parity pinned at 1.

### Stage 5 — trust widening + wrap-up

- Design 5 audit; final coverage numbers on all self-host paths.
- Full-corpus `CPPCAML_MODSIG_CHECK=1 CPPCAML_COERCE_CHECK=1` sweep;
  remaining lines individually explained in the JOURNAL.
- JOURNAL "P3 DONE" section (what exists, gate numbers, counter movement,
  what P4 needs); update memory `module-system-active-goal`. Final commit.

## Pitfalls (carried from P2 + new)

- All P2 pitfalls stand (runtime-component rule; (ns,name) pairing only;
  keep-LAST shadowing; FieldMut reads; no evaluation reorder; CmiFile
  lifetime for borrowed `Signature*`; depth guards — source.ml WILL find
  unguarded recursion; stdlib in-place .mli corruption).
- `module_result_layout`'s empty-ascription fallback: when Env/msig becomes
  authoritative, an empty ascribed sig means EMPTY — expect and WANT
  divergence from the legacy function there; handle at the consumer, don't
  "fix" the legacy fn while legacy callers remain.
- Env frames vs the GLOBAL maps: bindings added for opens/includes must pop
  with their frame. A binding leaking out of a struct body reintroduces
  exactly the clobbering class this phase deletes.
- `Pmod_apply` result Sigs: do NOT attempt param substitution; null out.
  (Applicative-functor type equalities matter to the TYPER, not to layout.)
- The include-splice change alters `export_ns` alignment invariants —
  `cursig`/`export_names` index-alignment is asserted at build_module exit;
  run the asserts after every edit.
- Recursive modules: the snapshot/restore of `module_layout_` (13108/13131)
  interacts with Stage 4; convert the coerce site (Stage 3) WITHOUT touching
  the snapshot, then delete the snapshot only when Env serves those reads.

## Definition of done

1. Five gates ≥ stage-0; lambda parity ≥ 411 (expect gains from the sweep
   fixes); reject_parity exactly 1.
2. modsig sweep (P1 asserts): 0 lines on self-host paths AND the 9-file
   corpus residue eliminated or individually re-triaged as
   oracle-divergence-free.
3. All coerce_block consumers route through compute_coercion on trusted
   Sigs; self-host fallback counters ~0 with the residue printed and
   explained.
4. `module_head_resolvable`'s ladder replaced by Env/cmi lookup;
   MPR7761-class clobbering impossible by construction (scoped frames).
5. JOURNAL + memory updated; per-stage commits with gate numbers.
