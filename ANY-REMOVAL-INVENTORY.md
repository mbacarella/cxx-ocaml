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

## Progress (2026-07-06 latest: track-2 dump parity RE-CLOSED — 886/886 identical)

The reopened dump-detail frontier (DIFF 113 / err 28 on the 141 new lib files)
is closed in two commits (`9f41329d16`, `8b22e61dfe`), all other gates flat:

- **One root cause was 121 of the 141**: typer.cpp's name-level cmi walker
  (`cmi_unit`) only knew the stdlib naming pattern, so `open Unix` imported
  nothing (the 28 errs) and qualified uses dumped as `Stdlib!.Unix.pipe`
  (most of the 113 DIFFs). `cmi_unit` now falls back to the otherlibs `-I`
  dirs (new `infer_module_dirs()` getter) and `resolve_module` roots such a
  unit as its own global ident (`Unix!`).
- The 13 residuals were n-ary cmi ctor/type shapes: typext arity
  (`Unix.Unix_error (a,b,c)` flattens), `C _` → N `Tpat_any`
  (`construct_any_arity` side-table), `_ MP.tracker` → N `Ttyp_any`
  (`type_any_arity` + `qualified_type_arity`, alias-rerouted), and
  `Ppat_open` opening the module's ctors (`Unix.(Unix_error (ENOENT,_,_))`).

New baseline: typedtree **886/886 (100.0%)** over oracle-typed; reject 1,
accept 37, sig 614/11, lambda 327 unchanged. Remaining lib-corpus frontier =
the 11 sig DIFFs (display work).

## Progress (2026-07-06: bucket A's otherlibs population WIRED — corpus 745 → 886)

No site-count change (A ×3 still `any()`), but the **separate-compilation
blocker's biggest half fell**: the typer harness oracles + `c++type` now share
the testsuite's otherlibs `-I` set (`unix`/`str`/`systhreads`/`runtime_events`/
`dynlink`), so `Unix.*`/`Thread.*`/... resolve to REAL cmi types instead of
falling into site `1186`. The oracle cache was regenerated with the same
context: **141 lib-test files entered the typer corpus** (745 → 886
oracle-typed). Migration exposed 9 real false-rejects, all fixed
(`477752df7b`): qualified EXCEPTION ctors (`Dynlink.Error` hit result's
`Error` via the bare-name registry — typext resolution added +
prefer-qualified now runs in strict), strict schemes built expanded
(folded `Seq.t` clashed with its own expansion), and local-open record-field
loading (`Unix.LargeFile.(..).st_size` read the parent's `int`, not the
submodule's `int64`). Also repaired a clobbered `stdlib/camlinternalOO.cmi`
(from `boot/`) that was poisoning the oracle.

New baselines: reject **1/886** (the foo.ml artifact only), accept 37
(+`test_unixlabels`, a genuine Includemod hard case), sig 614/625, typedtree
745 identical / **DIFF 113 / err 28** over 886 (the new files' dump-detail
parity = the reopened track-2 frontier), lambda DIFF set unchanged.

What's left of A: the **test-local `.ml` sibling population** (Store, M/A/B,
Waitgroup, ...) needs sibling compilation; then re-size the three A sites
(ANY_A_DBG recipe) and evaluate the flip.

## Progress (2026-07-06 later: 6 → 5 sites — bucket E site CLEARED)

Commits `21efe918ea` + bonus. The known-unsound trap was dissected rather than
solved whole: conjunctive semantics apply to the **matched** side only (`[<`
rows conjoin shared-tag args), but site `5317` was the **construction** side,
where unifying shared-tag args IS OCaml's semantics — `` [`A 1; `A "x"] `` is a
genuine error (now rejected). Strict's `Ppat_variant` still returns a fresh var
(infer_check.cpp `~4307`), so no `[<` row ever reaches strict unify and the
conjunctive counter-example (two matchers with different `` `A `` arg types,
both applied to one value) stays accepted — verified directly. Bonus:
`builtin_clash` knows a Variant row is never a scalar builtin, and
`Pexp_variant` joined the trustworthy strict argument forms
(``print_string (`A 1)`` rejects).

NB the FULL conjunctive model (typing the matched side in strict — presence/
conjunction vars) remains future work, but it is a *completeness* item now,
not an `any()` site: the strict pattern fresh-var is the remaining guard.

Left after E: A ×3 + F ×1 (deferred — separate-compilation), J catch-all
(`5329`, last). **No typer-dentable `any()` sites remain** — the counter can
only move again via separate-compilation infrastructure (A/F) and then J.

## Progress (2026-07-06 later: 9 → 6 sites — bucket D CLEARED)

The three D sites are gone and, more importantly, the **strict pass now has a
class/object model**. Commits `37d30fa441..9d7a0642c6`, each gate-checked
(reject 1, accept 36, sig 524/1, typedtree 745/745, lambda DIFF 327 — same sets):

- **`Pexp_new`**: class lookup (`class_types_` / `class_ctor_types_`) now runs
  in strict; unknown class → per-occurrence fresh var (B/C argument).
- **`Pexp_send`**: the class-Constr reroute + object-row method lookup now run
  in strict; unknown receiver/absent method → fresh var.
- **`Pexp_object`**: strict types the body via `infer_object_body` like the
  other passes (it was written strict-aware all along — see its `if (strict)`
  poly-annotation branch).
- **The real wall was elsewhere**: `process_item`'s `Pstr_class` branch was
  wholly `!strict`-gated, so strict had NO class model (`class_types_` empty —
  the three flips above were vacuous for class-typed receivers until this).
  Lifted, along with the inner `class_ctor_types_` / `class_instvars_` gates.
- **Soundness win**: `Pexp_send` joined the trustworthy argument forms for the
  strict reliable-callee `builtin_clash` check. `print_string o#m` with
  `m : int` now rejects — for object literals, `new c`, argument and
  annotation positions — while every corpus gate stayed identical.

The four object-related false-accepts (`pr3968_bad` class-type coercion,
`pr4018_bad` virtual + self-type param, `pr4824a_bad`,
`illegal_reference_to_recursive_class`) remain: they need the FULL class model
(coercion checking against a class type, virtuals/inheritance in the row,
recursive class types), not the concrete-method row.

Left after D: A ×3 + F ×1 (deferred — separate-compilation), E ×1
(`5317`, known-unsound conjunctive rows), J catch-all (`5328`, last).

## Progress (2026-07-06: 14 → 9 sites — buckets B + C CLEARED)

The **B/C frontier is done**: all five records/fields/ctors fallbacks now return
a fresh var instead of the absorbing `any()`, each corpus-revalidated with every
gate identical (reject 1, accept 36, sig 524/1, typedtree 745/745, lambda DIFF
327 — same sets). Commits (2026-07-06):

- **C — unknown expr ctor** (`Pexp_construct`, unresolvable bare/qualified name):
  fresh var; unification against context pins the ctor's real type.
- **C — unknown ctor pattern** (`Ppat_construct`): mirror of the above in every
  pass (the kind pass already returned fresh var).
- **B — unresolved record pattern** (`Ppat_record`, no label resolves): fresh var.
- **B — unresolved record construct** (`Pexp_record`) + **unresolved field access**
  (`Pexp_field`): fresh vars.

Why safe (not just lucky): each *syntactic occurrence* mints its OWN fresh var,
so two uses of the same unknown ctor/label never clash through one var — only a
single occurrence flowing into two incompatible contexts clashes, which is a
genuine error the oracle also rejects. `any()`'s absorbing property was therefore
unnecessary conservatism here, not load-bearing (contrast bucket A, where it *is*
load-bearing — an unknown module member must instantiate fresh per use).

Left after B/C: A ×3 (`1186`/`1191`/`1193`, deferred — separate-compilation),
F ×1 (`4956`, deferred with A), D ×3 (`4969` new / `5306` send / `5329` object,
hard object-row core), E ×1 (`5319`, known-unsound conjunctive rows), J catch-all
(`5331`, last). NB: the catch-all flip to fresh var *also* held all gates on the
corpus but was **reverted on purpose** — it is the genuine "unhandled form" net
and must stay absorbing (bucket J) until every form is really typed.

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

### Bucket A sizing + negative result (2026-07-06)

Instrumented the three A-sites across the 1853-file corpus: **346 files hit
site 1186** (unresolvable qualified module → Any), 3939 hits; site 1191
(resolved module, absent value) 126 hits; site 1193 (`Lapply` value path) ~0.
Site 1186 splits into two populations:

- **otherlibs** — `Unix` (1013), `Thread` (392), `Dynlink` (305), `Str` (170),
  `Runtime_events` (56): real modules whose `.cmi` exists but is off our search
  path, and whose oracle dumps were generated *with* the test's `-I +unix`-style
  include context. Resolving them needs the per-test include set, i.e. real
  **separate-compilation** wiring, not a typer edit.
- **test-local `.ml` siblings** — `Store`, `Waitgroup`, `M`/`A`/`B`, `User`,
  `Key`, `Callbacks`, `Plugin_*` …: modules defined in *other files* of a
  multi-file test. Fundamentally need sibling compilation.

**Negative result — do NOT retry without the inference core.** The one purely
internal sub-case (local-open `Bigarray.(Array1.init ..)` / `Scanf.(Scanning..)`
not registering the opened module's submodules for reroute, unlike top-level
`open`) *looked* safe. Wiring it (mirror the structure-level `load_open_type_quals`
/ `open_module_ctors` into the `Pexp_struct_item`+`Pstr_open` branch, scoped with
snapshot/restore) resolved the minimal repros but **regressed reject-parity 1 → 5**:
`lib-seq/test.ml` is a genuine **unify clash** (real `Seq` ctor types conflict
where `any()` absorbed — the load-bearing wall, unfixable without full HM ctor
inference), and `fuzzy.ml` / `inline_traversal_test.ml` / `marshal_bigarray.ml`
threw **"Unbound module"** in the *strict* pass (the submodule reroute at
`~1177` fires in the display pass but not strict). Reverted. Confirms empirically
what the table's "High risk" note asserted: **bucket A cannot be dented by a
typer edit** — it is blocked on separate-compilation infrastructure, so it should
be de-prioritized behind the HM-value-inference buckets (B/C/H/I) despite being
"unblocked" by P1–P3 on paper.

## What the buckets add up to

23 sites collapse into ~9 features across three bodies of work already on the roadmap:

- **Module System (P1–P3)** → clears **A**, enables **F**.
- **Full HM value inference (P4 core)** → clears **B, C, H, I** (the bulk),
  re-validating every guard against the corpus.
- **Two hard row-type cores** → **D** (object rows), then **E** (conjunctive
  variants — the known-unsound trap).

The engine mechanism (**J**) is deletable only once all 23 call sites are gone;
removing it earlier just relocates the fallbacks.

## Execution plan (dependency order, refreshed 2026-07-06)

State: **5 sites left** in `infer_check.cpp` (re-grep: `grep -cE '\bany\(\)'`).
G/H/I and part of D closed in the 2026-07-03 sweep; **B + C, then D, then the
E site all closed 2026-07-06** (see Progress above). Module System P1–P3 is
done, but the 2026-07-06 sizing (below) shows **A/F are NOT typer-dentable** —
they are blocked on separate-compilation infrastructure (per-test `-I` include
context + sibling-`.ml` compilation), not on more inference. **All remaining
sites are A/F (deferred) + J (last): the next counter movement requires the
separate-compilation front.**

Live site lines (2026-07-06, post-E): A = `1186`/`1191`/`1193`; F = `4956`;
J catch-all = `5329`. Re-grep before editing.

Tracked as tasks #1–#6:

1. **[#3] B + C — records/fields + unqualified ctors** — ✅ **DONE 2026-07-06**
   (14 → 9). Five fresh-var flips, all gates identical. No further B/C sites.
2. **[#1] A — module-path resolution** (×3, `1186`/`1191`/`1193`). **DEFERRED —
   blocked on separate-compilation, not inference** (see the 2026-07-06 negative
   result). Do NOT attempt as a typer edit: real types here false-reject
   (`lib-seq` unify clash). Revisit only once sibling-`.ml` / `-I` include
   loading exists.
3. **[#2] F — first-class modules** (×1, `4956`). Deferred with A (same
   separate-compilation dependency for a pack's signature).
4. **[#4] D — object rows** — ✅ **DONE 2026-07-06** (9 → 6). The
   concrete-method row model + strict `Pstr_class` processing sufficed; no
   subtyping machinery was needed to hold the gates (see Progress). The full
   class model (coercions, virtuals, inheritance rows) remains future work,
   tracked by the four object false-accepts.
5. **[#5] E — conjunctive polymorphic-variant rows** — ✅ **site DONE
   2026-07-06** (6 → 5). Construction rows unify shared-tag args soundly; the
   conjunctive trap is confined to the matched side, which stays a fresh var
   in strict (a completeness item, no longer an `any()` site).
6. **[#6] J — delete the engine mechanism** (`any()` singleton + `infer.cpp`
   absorb branch) and the catch-all (`5329`). **Last** — deletable only when the
   count hits 0. (The catch-all flip to fresh var held all gates on the corpus
   but was reverted on purpose — it must stay the absorbing "unhandled" net until
   every form is typed.) Also fix the `.cmi` bridge (`K::Any → cmiw::ty_var`) to
   emit real types. Blocked by #1–#5.

### How progress is measured (every removal)

Work counter (down): `grep -cE '\bany\(\)' cxx/src/infer_check.cpp` — 5 → 0.
Gates that must stay flat, compared as **file SETS** not totals:
`reject_parity.sh` 744/744 (0.0%), `sig_parity.sh` 525/525,
`typedtree_parity.sh` 745/745, 0 crashes/1853, `lambda_parity.sh` DIFF set.
The metric that should improve: `accept_parity.sh` (36 false-accepts → fewer).
Per-site loop: snapshot failing sets → flip one site → rebuild → run the five
gates → confirm sets identical + note any accept shrink → decrement this doc →
commit.

## Guardrails

Whatever order, removal must hold the invariants `Any` currently guarantees:
**reject_parity 0.0% (744/744)**, **0 crashes (1853 files)**, **sig_parity 100%
(525/525)**. Signatures reached parity *with* `Any` as fallback — real inference
must reproduce those exact displayed types without it. Also fix the `.cmi` bridge
(`infer_check.cpp:6920`, `K::Any → cmiw::ty_var`) to emit real types, not opaque
vars, for the `.cmi` goal.
