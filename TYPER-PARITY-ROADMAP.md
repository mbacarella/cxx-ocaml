# Typer-parity roadmap

Goal: full type-checker parity with `ocamlc` for the C++ reimplementation
(`c++type` / the strict pass in `infer_check.cpp`).

## Dashboard (current)

| Goal | Harness | Start of project | **Now** |
|---|---|---|---|
| **Completeness** — never reject valid code | `reject_parity.sh` | 0.1% false-reject | **0.0% (744/744)** ✅ |
| **Soundness** — reject invalid code | `accept_parity.sh` | 13.6% false-accept (93) | **5.3% (36 files)** |
| Front-end parsing | `parse_parity.sh` | ~100% | **100%** |
| Lambda back end | `lambda_parity.sh` | 52.8% | 55.6% |
| **Typed-tree dump** — produce the exact typed tree | `typedtree_parity.sh` | 25.4% | **39.7% (735/1853; 98.7% of the 745 oracle-typed)** |

> Denominator fix (2026-07-04): the old "1222 typeable" figure was an artifact —
> the harness counted c++-accepted-but-oracle-rejected files both as "typeable"
> and as DIFF.  Only 745 corpus files produce a real oracle dump; the harness
> now classes the rest as dump-mode false-accepts (soundness territory,
> already tracked by `accept_parity.sh`).  Remaining dump work (2026-07-06):
> 1 c++-err (pr11887, imported #type polyvariant tags) + 9 DIFF files.  Closed
> most recently (734 -> 735): fragile_matching — a record PATTERN's fields print
> in the record's DECLARATION order (lbl_num), not source order (`{ b; a }` over
> `type t = { a; b }` prints a then b).  c++ kept source order; now stable-sorts
> the Tpat_record fields by their index in field_registry's decl-order list
> (local records only; cmi-only records left alone).  Closed
> earlier (733 -> 734): backtrace_deprecated — `x |> function ..` is NOT
> rewritten to `(function ..) x`.  typecore only collapses `%revapply` (`|>`)
> when the function operand is `is_inferred` (an expr typed without an expected
> type: ident/apply/field/constraint/coerce/send/new/pack-with-annot, or a
> seq/if/local-open ending in one).  A `function`/`fun`/`match` is NOT inferred
> (it needs an expected type), so `|>` stays a plain applied `Texp_ident
> Stdlib.|>`.  Added `is_inferred` and gated the rev==1 (%revapply) collapse on
> it (%apply/`@@` has no such gate).  Closed
> earlier (732 -> 733): pr6922 — a self-send whose method is inherited from
> a class we can't enumerate (here `Basic.agent`, a cross-module functor result)
> now still resolves to `Tmeth_val` (a stamped method ident) rather than bare
> `Tmeth_name`.  In OCaml EVERY self-send carries a method ident; the class'
> meths table only pre-registers locally-declared/enumerable-inherited methods,
> so for a missing method we now mint a fresh shared ident on first send and
> cache it in the table.  Fixed a 58-line stamp cascade (the missing `put` stamp
> shifted every subsequent stamp by one) in one edit.  Closed
> earlier (731 -> 732): morematch — polyvariant-argument exhaustiveness
> (`A (`A|`C)` over the closed row `[ `A | `C | `D ]` leaves `A `D` unmatched
> -> Partial).  A dump-only `compute_partial` extension: for each covered ctor,
> substitute the scrutinee's actual type args into the ctor's per-type scheme
> and, for any argument position that is a closed polyvariant, flag Partial when
> a row tag is matched by no branch and none wildcards.  Sound and conservative
> (skips the strict pass; bails on any unanalyzable position).  Closed
> this sub-session (729 -> 731): fstclassmod (a `module type` brought into scope
> by `open M` where M is a module now resolves as `M.T` (Pdot) not a fresh local
> Pident — modtype names flow through ModExports/OpenEntry, and nested module
> structures stop leaking their modtypes into the outer scope), and test_iarray
> (an array literal `[|..|]` whose expected type is `iarray` prints as
> `Texp_array Immutable` — type-directed, recorded by a dump-only inference
> side-table `iarray_lits` at the three expected-type sites: application args,
> `(e : t)`, and `let x : t = e`).  Earlier: Closed
> this sub-session (724 -> 729): toplevel_lets + yamagata (class-ident coercion
> wrap moved to the ident; self-N counter advances per class DECLARATION incl.
> non-structure bodies), w53 (module_expr/module_type attributes, incl. the
> strengthening wrapper duplicating them), exotic (let `:>` coercion binding,
> virtual class-type method poly loc, `method : type t.` newtype extra
> relocation), t02 (module type of preserves member stamps -> `T with type u :=`
> reuses M's u stamp).  Remaining 15 DIFF: pr7284/robustmatch/morematch (GADT
> abstract-index exhaustiveness), accepted_batch (functor-app type paths),
> attributes.ml (`include (module type of M) with type t :=` STAMP offsets +
> package_type span), apply (shadowed `A.@@` resolved as builtin %apply),
> woodyatt (format-through-method `y#x "str"`), fstclassmod (module-rec
> package_type path Typ.PAIR), pr6922 (poly object), test_iarray (immutable-
> array Texp), cast/pr7657/index_aliases/fragile_matching/backtrace_deprecated.
> The
> remaining tail is genuinely hard and type-directed: GADT abstract-index
> exhaustiveness (pr7284/robustmatch/morematch — a partial match the type index
> can't refute), functor-application type paths (`Set.Make(Bool).t`,
> accepted_batch), `module type of` stamp propagation (t02), the shadowed
> apply operator `A.@@` (apply.ml — two independent roots, investigated
> 2026-07-06: (1) `revapply_kind` inlines any local `%apply` external, but
> typecore's `check_apply_prim_type` only rewrites `f @@ x` -> `f x` when the
> external's declared type is the fully-generic `('a->'b)->'a->'b` shape; a
> monomorphic redefinition `external (@@): f -> x -> int` must stay a plain
> `Texp_apply`.  Adding that type-shape gate is easy and correct.  (2) but the
> REAL blocker: inside `A.(succ @@ zero)` the `open A` should shadow the outer
> top-level generic `@@`, yet `resolve_value` searches the whole `scopes` stack
> BEFORE `opens`, so the outer `@@` wins and gets inlined.  Fixing this needs a
> unified scope/open ordering (a monotonic sequence number per scope-frame and
> open, resolving to the most-recent match) — a general resolution change.
> apply.ml ALSO has a separate `Optional "cap"` omitted-arg ghost, so it needs
> all three.), attribute preservation on core_type /
> module_type / signature-item nodes (w53, attributes), object rows
> (woodyatt/yamagata), class-level `Tcl_let` placement (toplevel_lets), and
> immutable-array `Texp` nodes (test_iarray).
> Optional-arg elimination ghosts — CLOSED this session for direct callees,
> functor-parameter members (htbl: `H : Hashtbl.SeededS`; pr7601: a local
> modtype), and `let rec` self-calls (optargs).
> Blockers cleared this session (714 -> 724): external all-float record ->
> Record_float (bigarrays); include-aware signature-simplify implicit
> Tmod_constraint (pr5164 + 4 more); functor-param member optional-arg
> desugaring (htbl, pr7601{,a}); let-rec self-call optional-arg desugaring
> (optargs).
> Earlier blockers cleared: structure-level include, first-class-module
> pack coercion layers + local-alias strengthening, `type nonrec` outer
> resolution, inherited class instvars/methods + `as super`, class-parameter
> val-vs-method scoping, member exports through module types (cmi walker),
> `<def_rec_dynamic>`, functor-param Mp_present + inline-record repr
> (index_functor), match-case constraint-extra placement + type-open shadowing
> (index_types), outer-constraint cons order (pr7036), or-with-catchall
> irrefutability, and Tfunction_cases (Partial) exhaustiveness (pr10338).
| `c++type` crashes | — | 7 (SIGABRT) | **0** ✅ |

Two of the three original goals are essentially closed: **completeness is perfect**
(the last false-reject was a *lexer* bug, not a typer gap) and the front end
parses everything the oracle does. The remaining frontiers are **soundness** (66
hard files) and **typed-tree dump parity** (the deep "real artifacts" goal,
barely moved — it's a different track, see below).

## Done

- **Completeness → 0%.** Every valid program the oracle accepts, we accept.
- **Soundness 13.6% → 9.6%** via focused, always-an-error syntactic checks, each
  verified two-sided (accept down, reject flat), back end provably untouched
  (all strict-gated): or-pattern var clashes; nullary-builtin/arrow annotation
  clashes; let-operator typing; cyclic & non-regular `module rec` types;
  duplicate module-type / class / class-type names; misplaced effect patterns;
  guarded value+exception mixes; unknown instance-variable overrides; incomplete
  record construction; stronger `Includemod` type-decl checks (arity, ctor-arg,
  field-type); illegal operator-shaped value names (`~##`/`#~#`).
- **Robustness.** `c++type` no longer crashes (catches LexError + any
  std::exception); fixed a comment-lexer bug (`'` after an identchar is an
  identifier prime).

## Negative result (recorded so it isn't repeated)

**Polymorphic-variant rows, naive version, is UNSOUND.** A `Kind::Variant`
(tag→argtype) that unifies shared-tag arguments false-rejects valid code:
OCaml uses **conjunctive types** (`` `B of int & string ``) for variants that are
only *matched* (never constructed), so per-use argument types are *conjoined*,
not unified. Confirmed reject_parity 0.1% → 0.7%; reverted. Real poly-variant
typing needs presence/conjunction variables and the `[<]`/`[>]`/exact
distinction — a much harder design.

## What's left in soundness (the 66, by what they need)

Bucketed by the machinery required — this is the honest map of why each is hard:

- **Hard inference cores (~16)** — no shortcut, each a real feature:
  - `Includemod` through functors / first-class modules (7: "Signature
    mismatch", "Modules do not match", "creates fresh types").
  - Value restriction / weak-var escape, GADT+functor soundness (a few).
  - Object rows + subtyping (objects-bugs: class-type matching, self-types).
  - Poly-variant conjunctive rows (see negative result).
- **Risky validations (~20)** — small file counts, one false positive regresses
  reject_parity on ubiquitous valid code:
  - Unbound module (~8) — entangled with separate compilation; conservative Any
    is INTENTIONAL for a drop-in compiler.
  - Format-string validity (~5: "invalid format", "Unknown modifier").
  - `let rec` value-recursion RHS (3).
  - Unbound type constructor (2) — needs a complete type-ctor env.
- **Possibly-tractable focused wins (~7)** — worth a careful look next:
  - Uninterpreted extension nodes (5: `[%foo]`, `[%tuple]`, `[%empty_polyvar]`,
    `[%M.foo]`) — `[%ext]` reaching typing without ppx is an error; needs a sound
    native-extension allowlist.
  - "expression has type int but expected …" (2) — type clashes we currently
    miss; investigate whether a reliable check catches them.
- **Blocked by `module rec` strict-suppression** — pr5918 (record completeness),
  t14 (with-constraint cycle), recursive class types: the check exists but the
  body is typed with errors suppressed.

## Forward tracks

Three independent directions; pick by appetite for risk vs. depth.

1. **Squeeze the focused soundness wins (small, safe).** The ~7 possibly-tractable
   files (extension nodes first). Same focused-check rhythm; bounded upside
   (soundness ~9.6% → ~8.5%). Good for a short session.

2. **Typed-tree dump parity ("Slice 3", the deep goal — LARGEST remaining lever).**
   Today the dump (`type_structure`) emits resolved names but NOT inferred types;
   it's stuck at ~25% while the inferencer (`infer_check`) computes real types it
   never routes into the dump. Wiring inferred types into the typedtree is the
   path from 25% toward the "byte-identical typed tree / real .cmt artifacts"
   end goal. This is the highest-value remaining work and is mostly orthogonal to
   the hard soundness features.

3. **The hard inference cores (large, unlock soundness AND dump parity).** Real
   row types (conjunctive, done right), object subtyping, value restriction,
   `Includemod` through functors. Each is a scoped multi-step feature; sequence
   by which most helps track 2. Start with whichever the dump-parity work most
   depends on.

Recommendation: **track 2 (typed-tree dump parity) is the most valuable next
project** — completeness and parsing are done, soundness via focused checks is
near its floor, and the dump is the metric that's barely moved while being
closest to the project's stated end goal (emitting real artifacts). Track 1 is
the natural "short session" filler; track 3 is the heavy feature work that both
others eventually need.
