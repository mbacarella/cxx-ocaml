# Slice 3 scope — routing inferred types into the typed-tree dump

**Goal.** Lift `typedtree_parity` past its ~27% / 334-file plateau by giving the
transcriber (`type_structure` in `typer.cpp`) the inferred-type information it
currently lacks, so it can reconstruct the *type-directed* parts of the dump.

**Why now.** The structural-variant work is essentially complete (patterns,
expressions, core types, structure items, the whole class/object subsystem). The
remaining `typedtree_parity` gap is no longer "missing constructs" — the files
*produce dumps* but differ on details that require type information.

## The good news: the scaffolding already exists

- **Inference computes the needed types.** `infer_check.cpp` already infers
  per-expression types and exposes a rich `ValueKinds` (function-return kinds,
  `expr_constr` qualified paths, `optional_erasures`, ...) keyed by `Expression*`.
- **The Lambda back end already reconstructs apply arguments** — `lambda.cpp`
  (~3085–3141) matches a call's args against the callee's labelled/optional
  parameters and inserts `None` for omitted optionals. The same logic is what the
  dump needs.
- **The transcriber already consumes an inference side-table.** `type_structure`
  calls `infer_match_partiality(s)` and threads a `partiality` map
  (`Expression* -> bool`) to set `Texp_match` Partial/Total. The comment there
  literally reads "(Slice 3)". This is the integration pattern to reuse.

So Slice 3 is mostly *plumbing existing inference results into the dump*, not new
type theory.

## The three type-directed sub-problems

### A. Omitted optional / labelled apply arguments — THE BIG ONE (~44 files)

The dominant diff. `f 5` where `f : ?b:int -> int -> int` must dump as:

```
Texp_apply  Texp_ident "f"
[ <arg> Optional "b"  expression (_none_) ghost  Texp_construct "None" []
  <arg> Nolabel       expression (...)           Texp_constant 5 ]
```

i.e. the args are emitted in the *callee's parameter order*, with omitted
optionals filled as ghost `None` and labelled args placed by name. The
transcriber today emits only the written args in source order.

**What it needs:** at each `Pexp_apply`, the callee's labelled/optional parameter
signature, to (1) reorder provided args into parameter order and (2) insert ghost
`None` for omitted optionals.

**Design:** add an inference side-table `apply_args : Expression* -> reconstructed
arg list`, where each entry is `(ArgLabel, provided-expr | omitted-None)`. Compute
it in a value-kinds-style pass (reusing the Lambda back end's existing matching
logic, factored out), then have `type_structure`'s `Pexp_apply` case consume it
exactly like `partiality`. Counts (`Texp_construct` 32 + `Optional` 12) suggest
~30–40 files flip once this is right.

**Risk:** medium. The reconstruction must be exactly correct (arg order, which
optionals are omitted) or the dump diffs; but it's dump-path only, so it cannot
affect soundness/back-end. Build incrementally and gate on `typedtree_parity`.

### B. Qualified paths through `open` (a handful)

`open Bigarray; Array1.of_array` dumps as `Stdlib!.Bigarray.Array1.of_array`, but
we emit `Stdlib!.Array1.of_array` — we drop the `Bigarray` parent when resolving a
name brought in by `open`. This is **path resolution, not inference**: track the
full module path a name was opened from and emit it. Mostly independent of A;
smaller, lower-risk, can be done separately.

### C. Stamp ORDER (a handful)

`M/1.v` vs `M/8.v` after normalization means we allocate a stamp at a different
*first-appearance* point than the oracle (e.g. `printing-types/pr7402`). This is
about matching OCaml's ident-allocation order, not types. Fiddly, low yield;
likely the last thing to chase, or accept as out-of-scope.

## Suggested phasing

1. **Factor the apply-arg matcher** out of `lambda.cpp` into a shared helper that,
   given a callee arrow type (with labels) and the written args, returns the
   ordered/filled arg list. (Refactor; no behaviour change — guard with
   `lambda_parity`/`instr_parity`.)
2. **Expose an `apply_args` side-table** from `infer_check` keyed by `Expression*`
   (the Pexp_apply node), built with that matcher.
3. **Consume it in `type_structure`** at `Pexp_apply`, emitting the reconstructed
   `Texp_apply` arg list (ghost `None` = `Texp_construct "None"` at `_none_`).
   Gate every step on `typedtree_parity` (target: 334 → ~370+).
4. **Path-through-open (B)** as an independent follow-on.
5. **Stamp order (C)** last, or declare out-of-scope.

## Guardrails (unchanged from the rest of track 2)

- Dump-path only: `typer.cpp`/`typedtree` changes can't touch
  `infer_check`/lexer/lambda outputs, so **soundness, parsing, and the back end
  stay provably unaffected**. (The new `infer_check` side-table is additive — it
  only adds a map; the value-kinds/strict results are untouched.)
- Verify each step: `typedtree_parity` must rise, and `lambda_parity` /
  `instr_parity` must stay flat after the step-1 refactor.
- Reuse the `partiality` precedent for the side-table wiring.

## Effort estimate

- Sub-problem **A**: the bulk. Step 1 (refactor) + step 2 (side-table) + step 3
  (consume) — a focused multi-step effort, but on well-trodden ground (the
  matching logic exists; the consumption pattern exists). Highest yield.
- **B**: small, self-contained.
- **C**: small but fiddly, lowest yield.

Net: A is the project; B and C are optional tails. A alone should move
`typedtree_parity` meaningfully (the single biggest remaining cluster).

## Status (2026-07-02)

- **Sub-problem A (apply-arg reconstruction): DONE in a prior session.** The
  pure matcher `applymatch::match` (`apply_match.hpp`), the `apply_plans`
  side-table (`infer_check.cpp record_apply_plan`), and the `type_structure`
  consumer are all wired. No further work here.
- **Format-string literals: DONE this session (+17 files, 448 -> 465).** The
  dominant remaining cluster was *not* apply args but format strings: the oracle
  desugars a format literal to `CamlinternalFormatBasics.Format(...)`; we emitted
  the raw `Const_string`. The inference pass already had a *type-directed* format
  set (`fmt_lits_`, populated at `infer_check.cpp` where a string literal is
  inferred at a `format6` type) and a working tree builder (`fmtlib::make` in
  `typer.cpp`), but the dump only used a weaker *syntactic* heuristic
  (`format_arg_index`, qualified `Printf.`/`Format.`/`Scanf.` calls only). Fix:
  carry `fmt_lits_` into `DumpAux.format_lits` (new lightweight `record_fmt_lits_`
  flag, additive -- does NOT flip the heavier `record_kinds_`), and desugar in
  `type_structure`'s `Pexp_constant` case for any literal in the set. This
  catches unqualified/`open`'d/`let`-bound/user-format-typed literals the
  syntactic path missed. Guardrails held: reject 0.0% (744/744), sig 100%
  (525/525), lambda unaffected.
- **Format-directive extension: DONE 2026-07-03 (+7 files, 466 -> 473).** All 7
  remaining format-cluster files flipped by extending `fmtlib` to the full
  directive set the corpus uses: boxes/tags `@[<...>`/`@{` (`Formatting_gen` /
  `Open_box`/`Open_tag` with the `<...>` sub-format), parametrised breaks
  `@;<w o>`, magic size `@<n>`, `@@`/`@%%`/`%@` escapes, `Scan_indic`, scan
  counters `%n %l %N %L` (+ `%_` ignored forms -> `Ignored_param`), `%a`/`%t`,
  and `%(...%)` (`Format_subst` with an `fmtty` derived by walking the built
  sub-tree). Plus one inference-side addition, gated `record_fmt_lits_`-only
  (dump pass; provably invisible to strict/value-kinds/back end): a
  format-expected match/try/let/sequence records its result-position string
  literals into `fmt_lits_` (oracle type_expect pushes the format type into arm
  bodies -- gen_test's `pr "%(%d%)" (match ... -> "x%d")`).
- **Remaining (29 DIFF files, none regressed):** small singletons: `Tpat_var`
  stamp/naming (3), `alloc_major`/GC-stat expr types (3), module_expr/functor
  cases (3), poly-variant & object row `type_declaration`s, `extra`-node
  ghosts, etc. No dominant cluster remains.
