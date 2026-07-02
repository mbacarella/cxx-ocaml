# Typer-parity roadmap

Goal: full type-checker parity with `ocamlc` for the C++ reimplementation
(`c++type` / the strict pass in `infer_check.cpp`).

## Dashboard (current)

| Goal | Harness | Start of project | **Now** |
|---|---|---|---|
| **Completeness** — never reject valid code | `reject_parity.sh` | 0.1% false-reject | **0.0% (744/744)** ✅ |
| **Soundness** — reject invalid code | `accept_parity.sh` | 13.6% false-accept (93) | **7.5% (51 files)** |
| Front-end parsing | `parse_parity.sh` | ~100% | **100%** |
| Lambda back end | `lambda_parity.sh` | 52.8% | 55.5% |
| **Typed-tree dump** — produce the exact typed tree | `typedtree_parity.sh` | 25.4% | **25.1% (465/1853; 38.1% of the 1222 typeable)** |
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
  field-type).
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
