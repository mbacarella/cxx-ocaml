# `Any`-removal inventory — the work-list to retire `Type::Kind::Any`

`Type::Kind::Any` is the inferencer's best-effort escape hatch: an **absorbing**
type that unifies with anything and drags plain variables into itself so they
"can't later clash" (`infer.cpp:242-247`). It's what keeps completeness perfect
(reject_parity 0.0%, 744/744) and crashes at 0 while inference is incomplete.
Retiring it = **finishing the inferencer**. This file turns that into a countable
work-list. Removal is JOURNAL phase **P4** ("real principal-types HM to retire
value-level `Any`"), sequenced *after* the Module System (P1–P3).

## Two roles (both must be replaced)

- **fallback** — the site genuinely cannot compute the type yet (a missing
  feature). Disappears when the feature lands.
- **guard** — the *strict* pass emits `Any` on purpose so an INCOMPLETE check
  can't false-reject a concrete type. Disappears as the strict pass becomes
  complete; each one is either a soundness win or a completeness regression when
  removed, so **every guard needs corpus re-validation** (hold reject 0.0%).

## Where `Any` is produced

23 call sites in `cxx/src/infer_check.cpp` + the engine mechanism in
`cxx/src/infer.cpp`. (`lambda.cpp`'s "Any" mentions are unrelated: a
value-representation class name + comments.)

| # | Bucket / feature needed | Sites (`infer_check.cpp`) | Role | Depends on | Risk |
|---|---|---|---|---|---|
| **A** | **Module path resolution** — real `Env` w/ loaded signatures; unresolved → `Unbound`, not `Any` | `1123`/`1128`/`1130` (qualified `Pexp_ident`, module we can't load) | fallback | **P1–P3** (Module System) | High — entangled w/ separate compilation; conservative `Any` is *intentional* for a drop-in compiler |
| **B** | **Records & fields** — full label/field inference beyond the unique-label registry | `3356` (record pat), `4056` (field access), `4200` (record construct), part of `4280` | fallback | HM value inference | Med — ambiguous labels leak `Any` by design today |
| **C** | **Constructors / exceptions** — resolve unqualified ctor to its type without the scrutinee | `3274` (pattern ctor), `3735` (`Pexp_construct` unqualified/exn) | fallback | HM + ctor env | Med |
| **D** | **Objects / self-types / methods** — object-row inference + subtyping in the strict pass | `4255` (`o#m` send), `4280` (`Pexp_object`), `bind_pat_any` `2977`/`2979` | both | **Hard core: object rows** | High — strict self/instance-var model absent |
| **E** | **Polymorphic variants** — conjunctive rows done right (presence/conjunction vars) | `4268` (`Pexp_variant`, strict) | guard | **Hard core: conjunctive rows** | High — naive version proven *UNSOUND* (reverted); see negative result in TYPER-PARITY-ROADMAP.md |
| **F** | **First-class modules** — infer a pack's type from its signature | `3934` (unconstrained `Pexp_pack`) | fallback | P1–P3 + module-type inference | Med |
| **G** | **Format strings** — full `format6` typing | `3458`, `3459` (chan/pres defaults), `3524` (unknown directive) | fallback | format typer | Low — self-contained; ~5 accept-parity files |
| **H** | **Strict-completeness shims** — `Any` only because an incomplete strict pass would clash on a concrete type | `3958` (lazy→`lazy_t`), `3974` (assert→`unit`) | guard | HM value inference (completeness) | Low mechanically; each is a completeness landmine |
| **I** | **Type-ctor arity** — pad missing args with the real type, not `Any` | `865` (strict arity padding) | guard | correct type-ctor env | Low |
| **J** | **Engine mechanism** — the `any()` singleton + the absorb branch in `unify` | `infer.cpp:19-22`, `infer.cpp:246` | — | *everything above* | Removed **last** |

Line numbers are as of 2026-07-02 (branch `cpp-rewrite`); re-grep before editing:
`grep -nE '\bany\(\)|Kind::Any' cxx/src/infer_check.cpp`.

## Progress (2026-07-03: 23 → 14 sites, all gates flat each step)

Closed, each corpus-revalidated (reject 1 known artifact, accept 36 identical
set, sig 525/525, typedtree 473, lambda 412 identical DIFF set):

- **G DONE** (`a274f7b4c0`): format_arrow's chan/pres defaults and unknown
  directive are fresh vars; NEW `%[...]` scanf-set case types `string`.
  (The soundness half — format validity — had closed in the typer sessions.)
- **H + I DONE** (`ac5abae8b9`): strict commits to `lazy_t`, `unit` (assert),
  fresh-var arity padding.  Plus `10953e01e7`: assert pins its condition to
  `bool` in strict too.
- **D (part)** (`f43fef7aa2`): bind_pat_any binds fresh vars; the enabling fix
  was running the polymorphic-record-field scheme binding in STRICT as well
  (domains.ml's `{pf}` at two format types was the counter-example).
- **setinstvar** (`d491c57e34`): `n <- e` is `unit`.

The remaining 14 sites are the REAL features: A ×3 (typer-side module env —
the Any is load-bearing: an unknown module's member must instantiate freshly
per use, so a swap false-rejects; needs actual resolution), B ×4 (records:
recTy fallbacks, field access, catch-all), C ×2 (unqualified ctor/exn
resolution), D ×3 (objects: new/send/object-body in strict), E ×1 (variant
conjunctive rows — the known-unsound trap), F ×1 (unconstrained pack), plus
the catch-all and J (engine, last).

## What the buckets add up to

23 sites collapse into ~9 features across three bodies of work already on the roadmap:

- **Module System (P1–P3)** → clears **A**, enables **F**.
- **Full HM value inference (P4 core)** → clears **B, C, H, I** (the bulk),
  re-validating every guard against the corpus.
- **Two hard row-type cores** → **D** (object rows), then **E** (conjunctive
  variants — the known-unsound trap).

The engine mechanism (**J**) is deletable only once all 23 call sites are gone;
removing it earlier just relocates the fallbacks.

## Suggested order (respects dependencies)

1. **G — format strings.** Independent of everything above, low risk, already on
   the accept-parity list (~5 files). The cheapest real dent; doesn't wait on P1–P3.
2. **P1–P3 Module System** → clears **A**, enables **F**.
3. **HM value inference** → clears **B, C, H, I**; re-validate each guard.
4. **Hard cores** → **D** (object rows), then **E** (conjunctive variants; needs
   presence/conjunction vars — a naive row is unsound).
5. **J** — delete `any()` and the absorb branch. `Any` is gone.

## Guardrails

Whatever order, removal must hold the invariants `Any` currently guarantees:
**reject_parity 0.0% (744/744)**, **0 crashes (1853 files)**, **sig_parity 100%
(525/525)**. Signatures reached parity *with* `Any` as fallback — real inference
must reproduce those exact displayed types without it. Also fix the `.cmi` bridge
(`infer_check.cpp:6920`, `K::Any → cmiw::ty_var`) to emit real types, not opaque
vars, for the `.cmi` goal.
