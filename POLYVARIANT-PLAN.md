# Sub-plan: put polymorphic-variant rows to bed

Goal: close the poly-variant cluster in `sig_parity` (the last big lever after the
routing harvest). Grounded in the actual 12 corpus DIFF files (below), not theory.

## Two prior negative results this plan must respect

1. **Conjunctive types are unsound in the STRICT pass.** A `Kind::Variant` that
   unifies shared-tag args false-rejects (`` `B of int & string `` when a variant
   is only matched). *Mitigation:* ALL row work stays NON-STRICT (signature +
   value-kinds). The strict reject pass keeps returning a fresh var, so no
   false-reject is possible regardless of row (in)accuracy. reject_parity is
   structurally protected.
2. **Recursive rows crash the merge/show** (`[> `A of 'a] as 'a` → cyclic graph →
   infinite recursion → core dump; mixin.ml). *Key reframing:* the corpus REQUIRES
   the recursive `as 'a` display (nested.ml: `[> `A of 'a ] as 'a`), so cycle
   detection is not merely a crash guard — it is Phase 2's feature. Handle it once,
   correctly, and both problems dissolve.

## The 12 files, by tier (what each actually needs)

| Tier | Files | Shape needed |
|---|---|---|
| **1 simple matched/constructed** | pr10664, pr6899_second_bad, exotic | `[< `A \| `B ]` from patterns / `[> `A \| `B ]` from construction, no recursion |
| **2 recursive `as 'a`** | nested, recursive_module_init | `[> `A of 'a ] as 'a` — cyclic row, `as` display |
| **3 bounded `[< L > P ]`** | ref_spec, pr6836 | present-tag set (`[< `Bar\|`Baz\|`Foo of float > `Bar `Foo]`) — presence vars |
| **4 mapping + objects (stretch)** | morematch, mixin, mixin2, mixin3 | matched-`[<`→constructed-`[>]` flow, row VARIABLES, objects+`#ops`, deep recursion |

Realistic reach: **Tiers 1–3 (~7 files)**. Tier 4 is deep (row unification variables
+ object rows + mutual recursion) and may stay a documented residue.

## Phases (each independently committable + measured)

### Phase 0 — Cycle-safe rows (unblocks everything)
- Add a visited-set (by `Type*`) to `show_rec` for **Variant AND Object**: on
  re-entry to a node already on the print stack, emit a back-reference `'aN` and,
  at the node's first occurrence, wrap it `(... as 'aN)`. This is exactly ocamlc's
  `as` display and kills the infinite recursion.
- Guard the variant **merge** in `unify` and any variant walk (occurs/generalize/
  instantiate) against cycles (visited-set or depth cap) so building a recursive
  row can't loop.
- **Gate:** mixin*.ml must PRODUCE a dump (not crash). Add a crash check to the
  per-file harness loop for the whole plan. reject 0.0%, lambda flat.

### Phase 1 — Simple matched/constructed rows
- Re-land the reverted Ppat_variant → `[< tag [of t]]` (variant_kind=1), now safe.
- Keep Pexp_variant → `[> tag [of t]]` (variant_kind=0, already shipped).
- Merge rule (union tags; result `[<` iff both `[<`, else `[>]`), alpha-sorted tags
  in show. Exact `[ tags ]` when the row comes from an annotation.
- **Targets:** pr10664 (`[ `A | `B ]` via its annotation), pr6899_second_bad
  (`[< `Test ]`), exotic (`[> `A | `B ]`). Measure; expect ~3 flips.

### Phase 2 — Recursive rows
- With Phase 0's `as 'aN` printing, verify a recursive construction
  `let rec r () = `A (r ())` displays `unit -> [> `A of 'a ] as 'a`.
- **Targets:** nested, recursive_module_init. Alpha-numbering of the `as 'a` must
  match ocamlc (first-appearance order — reuse the existing var-naming map).

### Phase 3 — Bounded rows `[< L > P ]`
- Add a `present` tag subset to the Variant type (tags known to be actually present
  vs merely allowed). `[< allowed > present ]`. Pattern matching sets `allowed`;
  construction/`> ` sets `present`.
- **Targets:** ref_spec, pr6836. This is the presence-variable slice; hardest of
  the reachable tiers — attempt last, revert cleanly if it destabilises.

### Phase 4 — STRETCH (likely defer)
- morematch (matched→constructed mapping, needs a row VARIABLE to carry the open
  tail), mixin/mixin2/mixin3 (object rows + `#ops` + mutual recursion). Assess
  after 1–3; document as residue if the machinery balloons.

## Discipline (unchanged from the session's rhythm)
Per phase: build → isolated repro matches oracle → `sig_parity` for the number →
**crash-check every corpus file** (new) → reject_parity 0.0% (must, structurally
safe but verify) → lambda_parity 54.2% flat → per-file no-regression diff → commit.
Revert any phase that can't hit those (as the two prior attempts were reverted).

## STATUS (2026-07-01)

- **Phase 0 (cycle safety) — DONE & committed.** show_rec `on_stack` + `(.. as
  'aN)` back-refs; unify merge links-first. **0 crashes corpus-wide** (was a
  core-dump). This is the key result: the open-ended crash risk that made
  poly-variants "not put to bed" is eliminated; rows are now a safe, bounded
  feature.
- **Phase 1 (simple rows) — DONE & committed.** Ppat_variant → `[< ..]`;
  from_coretype builds a row from `[ .. ]`/`[< ..]`/`[> ..]` annotations
  (variant_kind exact 2 / `[<` 1 / `[>` 0); merge keeps the tighter kind (max);
  alpha-sorted tags. **pr10664 flips** (first poly-variant file to match). sig
  85.3%→85.5%, reject 0.0%, lambda flat, 0 crashes, 0 regressions.
- **Phase 2 (recursive `as 'a`) — ATTEMPTED, REVERTED.** Allowed occurs guarded
  recursion + cycle-safe generalize/demote/instantiate; the `as 'a` display DID
  appear (nested printed `[> `A of unit -> 'a ] as 'a`), but: (a) 0 corpus flips
  (nested's nested-recursion arg structure is subtler than the display alone —
  `unit -> 'a` vs `'a`), and (b) a NEW segfault on typing-objects/Exemples.ml
  (object recursion reached an unguarded walk). Net-negative → reverted per the
  plan's revert rule. Recursive rows need per-shape recursion precision + more
  walk-guarding than the visited-sets covered; a dedicated effort.
- **Tier 3 (bounded `[< L > P ]`) — scoped, not started.** ref_spec is CLOSE now
  (we print `[< `Bar | `Baz | `Foo of float ]`; missing only the `> `Bar `Foo`
  present-tags and `as 'a`). Needs a `present` tag set on the Variant + merge
  handling + non-recursive alias preservation.
- Remaining reachable-ish singletons each need one more feature: pr6899 `bar`
  (non-recursive `as 'a` alias preservation on an annotation), exotic
  (binding-level coercion `let x : T1 :> T2`).

**Outcome so far:** the cluster is de-risked (no crashes) and the tractable floor
(annotated/simple rows) is landing. The recursive + bounded + object tiers remain
bounded residue — exactly the "convert open-ended risk into a bounded residue"
goal, minus the last tiers.

- **Instantiate row-sharing — ATTEMPTED, REVERTED (3rd sub-finding).** Root-caused
  recursive_module_init: `instantiate` COPIES a Variant/Object node, but a row
  accumulates tags by LINKING its node during unify, so copying a monomorphic
  function-param row (`([> `A] -> int) -> ..`) breaks accumulation across
  `f `A; f `B; f `C` -- we got only the first tag.  Fix (share a row with no
  generic var; preserve variant_kind) made the ISOLATED cases correct
  (`direct`/`viaid`/check's tags all merge to `[> `Mod|`Nonrec|`Self]`).  BUT:
  (a) `contains_generic` recurses unboundedly -> STACK-OVERFLOW segfault on huge/
  deep types (parsetree/source.ml, mixin.ml); (b) 0 corpus flips -- even with
  tags merged, recursive_module_init still needs the CROSS-OCCURRENCE `as 'a`
  SHARING display (the same row appears in both `stub:` and the `f` param;
  ocamlc names it `([> ..] as 'a) -> .. -> ('a -> int)`, we print it twice).
  Reverted.  So this file needs THREE stacked features: (1) crash-safe
  monomorphic-row sharing in instantiate, (2) cross-occurrence `as 'a` naming in
  show (a real rework: assign a shared name to any row/type node printed 2+
  times, not just cyclic ones), (3) alpha ordering already done.

## DEDICATED PROJECT DELIVERED (2026-07-01, committed)

Did the "well-scoped, bounded" project (crash-safe instantiate + `as`-sharing show
+ present-tags).  Three clean flips, 0 crashes, 0 regressions, reject 0.0% / lambda
54.2% throughout.  sig 447 (pre-poly) -> **451 (85.9%)**:

1. **instantiate SHARE-UNCHANGED** — copy returns the ORIGINAL node when no child
   changed (correct HM), so a monomorphic function-param row stays ONE node and
   its tags accumulate across `f `A; f `B; f `C` (was copied -> only 1st tag).
   Replaced the crash-prone contains_generic pass.  (foundation; 0 flips alone)
2. **cross-occurrence `as 'a` show** — a count_refs pre-pass; a row reached 2+
   times (shared OR cyclic) is named `as 'aN` at first print, back-ref'd after
   (a `printed` set replaces on_stack).  => **recursive_module_init flips**.
3. **present-tags `[< L > `P ]`** — stored (Variant.present) from the annotation,
   rendered after the tags.  => **ref_spec flips** (its last missing piece).
Plus **pr10664** from phases 0-1.  TOTAL poly-variant flips: 3 (pr10664,
recursive_module_init, ref_spec).

REVERTED within this project (1-file, delicate plumbing): `named_alias` (preserve
a source single-occurrence `(row as 'a)` for pr6899 `bar`) — the flag is dropped
by instantiate-copy / merge (neither carries present/named_alias), fiddly for 1
file.

REMAINING poly-variant residue, each its own hard feature: bar (source
single-occurrence alias + instantiate/merge carrying present+named_alias), exotic
(binding-level coercion `let x:T1:>T2`), pr6836 (conjunctive `[< int u > `A]`),
nested (nested-recursion arg precision), morematch (input `[<]` -> DIFFERENT
output `[>]` rows, needs row VARIABLES not pass-through sharing), mixin×3
(objects+`#ops`+deep recursion), test2/frame-pointers (effect GADT univar).

### 4th sub-finding — weak-`as` (value restriction) for `bar` — ATTEMPTED, REVERTED (2026-07-01)

`bar` (pr6899_second_bad) is the CLOSEST residue file: the ONLY diff is oracle
`val bar : ([< `Test ] as '_weak1) -> unit` vs ours `[< `Test ] -> unit`.
signorm canonicalises `'_weak1`→`'a`, so the sole real difference is the
`(.. as ..)` wrapper.  ocamlc emits it because `bar = wrap ()` is NOT a syntactic
value → its row's implicit tail variable stays WEAK (non-generic), and ocamlc
names weak vars even at a single occurrence.  Root cause of our omission: we don't
track a level on the Variant node, so we can't tell `bar`'s weak row from
`foo`/`wrap`'s generic ones.

Made Variant rows **level-aware**: `variant_type` stamps `level`; `generalize`
promotes the node to GENERIC; `demote`/`occurs_and_lower` lower it; `instantiate`
gives a GENERIC row a FRESH weak node (so one use's value-restriction can't weaken
the shared scheme); `show` prints `as '_weakN` when a variant row is non-generic.
This **flipped `bar`** (sig 451→452, foo/wrap correctly stayed un-`as`'d) — BUT
reintroduced a **segfault on mixin.ml** (was rc=0, the plan's key "0 crashes"
result).  Cause: promoting variant nodes to GENERIC makes `instantiate` deep-COPY
any structure containing them; mixin's row is a heavily-shared arrow-DAG
(use-count 14505) whose copy explodes.  Added full-memoised copy (dedup shared
subtrees + cycle-guard) → crash MOVED to an unguarded cyclic-OBJECT walk reached
via the new level flow.  Every fix relocates the crash to the next unguarded walk
— precisely the plan's "needs more walk-guarding than the visited-sets covered; a
dedicated effort".  +1 flip at the cost of the sacred 0-crash invariant is
net-negative → reverted per the plan's revert rule.  **Lesson:** even the single
closest file needs level-aware rows threaded through generalize/demote/occurs/
instantiate AND a uniformly crash-safe (memoised, cycle-guarded) instantiate for
objects too — the same dedicated multi-feature project, not a one-file win.

**DECISION (2026-07-01): STOP the poly-variant grind after phases 0-1.** FOUR
sub-attempts (recursive occurs, instantiate sharing, the earlier pattern-row, and
the weak-`as` level-plumbing above) each confirm: every remaining corpus file
stacks 2-3 features AND the engine changes are crash-prone on deep/recursive/object
types.  The floor is landed
(pr10664, crashes gone).  The residue is now precisely understood (above) and is
a dedicated multi-feature project, not a session's incremental wins.  Best next
poly-variant investment if resumed: FIRST make `instantiate`/`generalize`/`demote`/
`occurs_and_lower` uniformly crash-safe on cyclic OBJECT and VARIANT rows (a
memoised, cycle-guarded copy that pre-registers every node before recursing) — the
4th sub-finding shows this walk-guarding is the shared blocker under every row
feature.  Only THEN layer the display features (weak-`as` for `bar`, present+
named_alias carried through instantiate/merge, nested-recursion arg precision).

## NEW WAYS FORWARD (start here after a /clear)

Four sub-attempts all died the same way: a display feature needs level-aware rows,
level-awareness forces `instantiate` to COPY rows, and copying a deep/cyclic
object+variant graph crashes. Every point fix relocates the crash to the next
unguarded walk. So the residue is NOT a set of display tweaks — it is ONE
substrate bug (walks aren't uniformly cycle/DAG-safe) wearing many hats. The ways
forward below are ordered by risk-adjusted value.

### Orientation (cold-start facts)
- Baseline: **sig 451/525 (85.9%)**, reject **0.0%**, lambda **54.2%**, **0 crashes**.
- Harnesses (each one command, run from repo root): `bash cxx/harness/sig_parity.sh`,
  `reject_parity.sh`, `lambda_parity.sh`. Build: `cd cxx/build && ninja c++type`.
- All row work lives in `cxx/src/infer.cpp`: `variant_type`/`object_type` (ctors),
  `generalize`/`demote`/`occurs_and_lower` (level walks), `instantiate` (the `copy`
  lambda), `show_rec`/`count_refs` (display). Rows are only produced in the
  non-strict passes, so reject_parity is structurally protected — but **crashes are
  not**: add a crash-check (`for f in corpus: c++type --infer $f; rc==139?`) to any
  attempt. mixin*.ml, parsetree/source.ml, typing-objects/Exemples.ml are the
  known deep/cyclic canaries.

### Way 1 — PREREQUISITE: one cycle/DAG-safe walk substrate (do this ALONE first)
The single highest-leverage move. Introduce a shared memo/visited helper and route
EVERY type-graph walk through it: `instantiate` copy (memoise by `Type*`,
pre-register a node's fresh shell BEFORE recursing so cycles resolve to the
in-progress copy — this also collapses shared-DAG subtrees to one copy), plus
`generalize`/`demote`/`occurs_and_lower` (visited-set so a recursive object/variant
can't loop). Expected corpus effect: **0 flips, 0 regressions, 0 crashes** — it is
pure hardening. Commit it on its own with that gate. Rationale: the 4th sub-finding
proved memoising ONLY the variant path just moves the crash to objects; do it once,
everywhere. This substrate is ALSO required by the [[cppcaml-cmi-goal]] (marshalling
a variant/object type needs a cycle-safe walk), so it is not display-only work —
fund it from the .cmi budget, not the poly-variant budget.

### Way 2 — THEN retry weak-`as` for `bar`, bounded (small win on a stable base)
On top of Way 1, re-land the level-aware variant rows (the reverted 4th attempt:
`variant_type` stamps `level`; `generalize` promotes; `demote`/`occurs` lower;
`show` prints `as '_weakN` for a non-generic variant row). Add a guard so
`instantiate` only fresh-copies a generic row that is SMALL and ACYCLIC (bounded
DFS, e.g. ≤64 nodes, no back-edge); otherwise share the node unchanged (giving up
weak-`as` on entangled rows the corpus doesn't need). `bar`'s `[< `Test ]` is a
leaf → fresh-copied → flips; mixin's DAG → over cap → shared → no crash. Target:
pr6899_second_bad (+1). Keep the bounded-copy guard even after Way 1 as a
belt-and-suspenders cost cap.

### Way 3 — PARALLEL, engine-free probe: pr6836 via the annotation path
pr6836 wants `val a : [< int u > `A ]` / `val b : [< t s > `B ]` — a row whose
allowed-set carries a *type application* (`int u`) plus present-tags. Its rows come
from source annotations, so this may be reachable purely in `from_coretype` +
`show` (parse `Ptyp_variant` rows whose fields are constructor applications; store
them; render), with NO touch to generalize/instantiate — i.e. no crash surface.
Verify first that a/b are annotation-driven, not inference-driven; if so this is a
clean flip independent of Ways 1–2. (If it turns out to need conjunctive-arg
unification in the row, stop — that reopens the unsound-conjunctive result #1.)

### Way 4 — DE-SCOPED (needs the row-VARIABLE model; a separate project)
morematch (input `[<]` must flow to a DIFFERENT output `[>]` row — pass-through
node-sharing can't express this; needs a real row tail variable), nested
(nested-recursion arg precision), mixin×3 (object rows + `#ops` + mutual
recursion), exotic (`let x:T1:>T2` coercion — an unrelated feature). These are the
"model rows as `{fields; tail_var}`" rework. Do NOT start these as a session task;
they are the dedicated project. If ever funded, they too sit on top of Way 1.

### Suggested first session after /clear
Do **Way 1** end-to-end (substrate hardening, its own commit, gate on 0-everything),
then **Way 2** (bar, +1) as the proof the substrate unblocks display features.
Optionally spike **Way 3** in parallel. Leave Way 4 documented.

## SESSION 2026-07-01 (b): Ways 1 & 2 DONE & committed; Way 3 assessed

- **Way 1 (cycle/DAG-safe walk substrate) — DONE & committed** (`infer: cycle/DAG-safe
  walk substrate (Way 1)`). Routed `occurs_and_lower`/`generalize`/`demote` through
  visited-guards (composite nodes) and `instantiate` copy through a `memo` (dedup
  shared-DAG subtrees to one copy) + `on_stack` guard (cyclic back-edge shares the
  original). Share-unchanged preserved (row accumulation intact). **Pure hardening:
  sig 451 flat, reject 0.0%, lambda 54.2%, 0 crashes / 1853 files.** This is the
  substrate the earlier 4 attempts kept crashing without; also the prerequisite for
  the [[cppcaml-cmi-goal]] variant/object marshalling.
- **Way 2 (level-aware variant rows -> weak-`as`) — DONE & committed** (`infer:
  level-aware variant rows`). `variant_type` stamps the row's tail level;
  `generalize` promotes to GENERIC, `demote`/`occurs` lower, merge takes min level;
  `instantiate` gives a GENERIC row a FRESH weak node per use, **bounded** to small
  acyclic rows (64-node DFS) so mixin's giant DAG is shared (no blow-up); `show`
  names a non-generic Variant row `as` (Objects untouched — not level-stamped).
  **sig 451->452 (86.1%), reject 0.0%, lambda 54.2%, 0 crashes; match-set diff =
  exactly +1 (pr6899_second_bad `bar`), 0 regressions.** Proof the substrate unblocks
  the level-plumbing display features that were net-negative before.
- **Way 3 (pr6836 `[< int u > `A ]`) — ASSESSED, NOT the clean from_coretype+show
  flip; deferred.** The annotation `[< int u]` is a `Rinherit` row (an inherited
  *type* `int u`, not a `` `Tag ``); from_coretype currently defers it
  (infer_check.cpp:456 `simple=false`). Getting `[< int u > `A ]` needs (1) a new
  inherited-type row component on the Variant, (2) from_coretype to populate it, (3)
  the present `` `A `` to flow from the body construction through the **unify merge**
  (soft_unify(te, annot)), and (4) show to render inherited types before `>`. So it
  is NOT engine-free — it touches unify merge (combining an inherited-carrying `[<`
  row with a constructed `[>` tag). Real multi-part feature, 1-file payoff; a
  dedicated slice, not a session spike. (No conjunctive unification needed — `int u`
  is displayed unexpanded, so result #1 is not reopened; the blocker is purely the
  inherited-type row representation + merge.)

**Cluster status: sig 452/525 (86.1%), reject 0.0%, lambda 54.2%, 0 crashes.** The
crash substrate (Way 1) is now permanent, so future row features are display-only
plumbing on a stable base. Best next: Way 3's inherited-type row (from_coretype +
merge + show) or Way 4's row-VARIABLE model (morematch/mixin), both dedicated slices.

## SESSION 2026-07-01 (c): Way 3 DONE & committed

- **Way 3 (inherited-type variant rows `[< int u > `A ]`) — DONE & committed**
  (`infer: inherited-type variant rows (Way 3)`). It DID turn out reachable on top
  of the Way-1 stable base, as a display-plumbing slice (no new crash surface). A
  `[< int u]` annotation is a `Rinherit` row; from_coretype had deferred it, so we
  lost the annotation and printed the bare construction `[> `A ]`.
  - Type gains an `inherited` vector (unexpanded row types = allowed-set bound).
  - from_coretype builds a Variant carrying the inherited types; the sig pass keeps
    abbreviations folded (`fold_abbrevs_`), so `int u`/`t s` display unexpanded like
    the oracle (no expansion-to-`[`A|`B]` problem to solve).
  - unify merge unions inherited; a row WITH an inherited bound routes incoming tags
    to `present` (labels stay empty — the allowed set is the inherited type, tags
    only mark presence). Merge now also PRESERVES `present` across the union (it was
    silently dropped before; net-neutral on the corpus, correct-er).
  - show renders inherited types before the tags/`>` present block.
  - instantiate copy carries `inherited` (ground types int/t: shared unexpanded).
  - **pr6836 flips** (`[< int u > `A ]` / `[< t s > `B ]`). sig 452->453 (86.3%),
    reject 0.0%, lambda 54.2% flat, 0 crashes / 1853 files, match-set delta = exactly
    +1 (pr6836), 0 regressions.

**Cluster status: sig 453/525 (86.3%), reject 0.0%, lambda 54.2%, 0 crashes.**
Ways 1, 2, 3 all landed. Only Way 4 (the row-VARIABLE model: morematch, mixin×3,
nested-recursion arg precision, exotic coercion) remains — a dedicated project, not
a session slice, and the last poly-variant residue.

## SESSION 2026-07-01 (d): exotic coercion picked off (engine-free)

- **`exotic.ml` (binding-level coercion) — DONE & committed** (`infer: binding-level
  coercion let x:T1:>T2 -> target type`). This was mislabelled as Way-4 residue but
  is NOT a row-variable feature at all — it's `let x : [`A] :> [> `A | `B ] = ..`,
  where the bound name takes the coercion TARGET, same as a `(e : T1 :> T2)`
  expression. The non-recursive binding path only handled `Pvc_constraint`; a
  `Pvc_coercion` binding fell through to the body's (narrower) type. Adopt the
  target for display (mirror the existing Pexp_coerce case); no unify of target
  into body (`:>` widens). Pure display-path, off the row substrate entirely — no
  crash surface. **sig 453->454 (86.5%), reject 0.0%, lambda 54.2% flat, 0 crashes /
  1853 files, match-set delta exactly +1, 0 regressions.**

**Cluster status: sig 454/525 (86.5%), reject 0.0%, lambda 54.2%, 0 crashes.** The
TRUE remaining residue is now purely the row-VARIABLE model (Way 4): morematch
(input `[<]` -> different output `[>]` rows), mixin×3 (object rows + `#ops` + mutual
recursion), nested (nested-recursion arg precision). These genuinely need the
`{fields; tail_var}` row rework — a dedicated project, not a session slice. No
engine-free flips remain in the cluster.

## SESSION 2026-07-01 (e): closure re-verified against live corpus

Re-scanned the full corpus after a /clear (sig 454/525 = 86.5% confirmed). Pulled
the 71 DIFF files and isolated the ones whose ORACLE sig contains backtick tags —
the exhaustive set of remaining poly-variant DIFFs is exactly **5**: morematch,
nested, mixin, mixin2, mixin3. Read each precise diff to check for a mislabelled
engine-free freebie (as `exotic` turned out to be). **There is none left** — each
genuinely needs the row-VARIABLE model:

- **morematch** — we emit `([<..] as 'c) -> [> `A of 'c | `B of 'c]` (one shared
  `'c`); oracle splits the input into DISTINCT output rows `[> `A of [> `Cons of 'a
  | `Nil ] | `B of [> `Snoc of 'b ]]`. Pass-through node-sharing cannot express a
  split — needs real row tail variables.
- **nested** — recursive `as 'a` binds at the ARROW level (`'a = unit -> [> `A of
  'a]`) over a `[>` constructed row; we emit `[< `A of unit -> '_60] as 'a` (wrong
  kind, untied recursion arg). Deep-recursion arg precision + alias placement.
- **mixin/mixin2/mixin3** — we emit huge fully-EXPANDED object/row types where the
  oracle folds to named abbreviations (`'a lambda`, `([> var], var) ops`, `#ops`).
  Needs object-row abbreviation folding + `#ops` ON TOP of the row model. mixin2 is
  lost entirely (`val lambda : 'a`).

Also noted (NON-poly-variant, out of this plan's scope but surfaced by the same
scan — candidate future slices): abbreviation-fold prefs `Lazy.t`↔`lazy_t`,
`format6`↔`format4`, abstract-alias display `HW.key`↔`SW.data` / `Pos.t`↔`int`
(ephetest3, patmatch, test_generator), and GADT univar over-specialization
(`int Effect.t` vs `'a Effect.t` in effects.ml; `int -> int` vs `'a -> 'b` variant
match scrutinees in patmatch). These are separate axes, not row work.

**Verdict: the poly-variant cluster is CLOSED to session-sized work.** Five engine-
free/plumbing flips landed this project (pr10664, recursive_module_init, ref_spec,
pr6836, bar, exotic → sig 447→454, +7, 0 crashes throughout). The residue is the
Way-4 row-variable rework — a dedicated, token-heavy, crash-prone multi-feature
project the plan explicitly says NOT to start as a session task. Next session should
NOT re-scan for freebies (this section is the proof there are none); it should
either (a) fund Way-4 as its own project, or (b) pivot to the non-row axes above.

## SESSION 2026-07-01 (f): PIVOT off poly-variants -> `let rec` generalization (+10)

Per the (e) closure verdict, pivoted to the non-row axes the corpus scan surfaced.
The first one was a genuine single-root BUG, not a display-naming quirk:

- **`let rec` bindings were never generalized.** `infer_bindings`'s recursive
  branch inferred bodies + unified the pre-bound recursion vars but (unlike the
  non-recursive branch) never raised the level or generalized the bound names. So
  every `let rec f` stayed monomorphic at the outer level and any LATER use in the
  same module pinned it: `let rec map f = ..;; map succ [1]` inferred
  `(int -> int) -> int list -> int list`. Fix mirrors the non-rec path:
  enter_level before pre-bind, leave_level after bodies, generalize each bound
  scheme under the value restriction (non-expansive -> generalize, else demote).
  Recursion stays monomorphic during body inference (shared non-generic pre-bound
  var); generalize once, after. **sig 454->464 (86.5%->88.4%), reject 0.0%, lambda
  54.2% flat, 0 crashes / 1853, match-set delta = exactly +10, 0 regressions**
  (streams, terms, hamming, sieve, sorts, join, pingpong, semantic,
  tupled_function, pr6323 -- all `let rec` fns used at a concrete type later).
  Committed `infer: generalize \`let rec\` bindings`.

**Baseline now: sig 464/525 (88.4%), reject 0.0%, lambda 54.2%, 0 crashes.**

### Next non-row clusters identified (for a future session, NOT started)
- **Type-path naming preference (~9 files, ARCHITECTURALLY BLOCKED).** Oracle keeps
  the path a type was ACCESSED through: `Result.Ok`->`Result.t` (contexts_1/2/3),
  `Float.classify_float`->`Float.fpclass` (fma), `String.compare`->`String.t`
  (qsort), `Bigarray.kind` (specialized), `Scanf.Scanning` (tscanf2_io) -- and the
  reverse for the `lazy` primitive: `lazy_t` not `Lazy.t` (lazy7, test_module). We
  canonicalize to the base name. BLOCKER: our unifier matches on the FINAL path
  component, so producing `Result.t` would not unify with `result` (this is exactly
  why rendering `lazy_t` was tried and REVERTED, infer.cpp:2683 -- reject 0.0->0.1).
  Matching needs recording the access-path on the constr node AND a display-time
  normalization that survives unify. A coherent project, regression-prone, deferred.
- **GADT/object over-specialization (niche, hard).** `int Effect.t` vs `'a Effect.t`
  (effects, test2 -- locally-abstract `type a` should stay polymorphic); `< .. >`
  vs `'a` (Tests, runtime-objects); `#castable` vs `'a` (cast). Need real
  locally-abstract-type + object-row handling.
- **format4<->format6 abbreviation folding** (lib-scanf test.ml `pr`); functor-result
  and first-class-module type-path one-offs (pr6944_ok `Map.Make(String).t`,
  pr6982_ok `(module S with type t = ..)`). Assorted one-offs, low leverage.

## SESSION 2026-07-01 (g): labelled-arrow bug (+2); lazy_t re-confirmed blocked

Continued off the (f) pivot into the non-row axes. Full re-scan of the 61 DIFF
files (categorised in scratchpad) to separate genuine BUGS from display-preference
residue.

- **Var-callee application dropped argument labels — GENUINE BUG, +2, committed**
  (`infer: carry argument labels through the var-callee apply fallback`). When a
  call's callee has an unknown arrow spine, `infer_apply`'s positional fallback
  built `eng.arrow(dom, r)` with NO label. Two corpus shapes hit this: a `let rec`
  self-call with a labelled arg (`g ~first:false` -> lost `first:`) and a labelled
  higher-order parameter (`f ~a ~b` -> lost `a:`/`b:`). Fix: tag the built arrow
  with the argument's `(lk, nm)`. Safe because arrow-unify only recurses dom/cod
  (labels aren't compared), so it can never false-reject against a known callee.
  **sig 464->466 (88.8%), reject 0.0%, lambda 54.2% flat, 0 crashes / 1853,
  match-set delta = exactly +2 (partial_application, alloc), 0 regressions.**

- **lazy_t vs Lazy.t — ATTEMPTED, REVERTED (re-confirms the path-naming blocker).**
  Made `lazy e` emit `lazy_t` (display pass only; strict still returns `any()`) and
  added a unify canonicalisation `lazy_t == Lazy.t` (last-component "lazy_t"->"t")
  so the two access paths coexist. Isolated cases were perfect (bare `let l = lazy e`
  -> `lazy_t`; `Lazy.force`/annotations -> `Lazy.t`; cross-path
  `Lazy.force (lazy 1)` unifies). Corpus: **+3 (lazy7, lazy_, test_module) but -1
  (hamming)**. hamming's `lazy` value flows into a `Lazy.t` context (`map : _ lcons
  Lazy.t -> ..`), so the oracle prints `Lazy.t`; we print `lazy_t`. Tried an
  in-`unify` rename (`lazy_t` node adopts the `Lazy.t` path on contact) — it FIRES
  but on `instantiate`-COPIES of the row node, never the binding's stored scheme
  node, so hamming stays `lazy_t`. Node identity is not stable across
  generalize/instantiate, which is exactly the access-path-tracking machinery the
  "type-path naming" cluster needs. Net +3/-1 can't hit the sacred 0-regression bar
  -> reverted. **Verdict: lazy_t is NOT a freebie; it is the same blocked path-naming
  project (record the access path on the constr node, survive unify+instantiate),
  now with a concrete failing witness (hamming).**

- **GADT-match result over-specialization (w04_failure, measure_runtime_arity) —
  confirmed the (f) "niche, hard" cluster, not attempted.** `match r1,r2,t with ..
  -> ()` should give result `unit`; we give `'b`. Root: `infer_function`/match GADT
  handling windows each branch (`soft_unify(rhs, rt); undo_to`) and the rollback
  discards the OUTER result unification too. Fixing needs distinguishing a branch
  result that mentions the abstract type (keep local) from a ground one (unify
  outward) — real GADT escape-scope work, deferred.

**Baseline now: sig 466/525 (88.8%), reject 0.0%, lambda 54.2%, 0 crashes.** The
remaining reachable clusters are all the two blocked ones (type-path naming incl.
lazy_t; GADT/object over-spec) plus the Way-4 row-variable poly-variant rework —
each a dedicated project, none a session-sized freebie.

## SESSION 2026-07-01 (h): three engine-free/contained flips (+3, sig 89.3%)

Re-scanned the live 59 DIFF files myself (not trusting (g)'s categorisation) and
found three genuine bugs OUTSIDE the blocked clusters, each a clean +1:

- **Binding-operator val names printed bare — display, +1.** `cpptype_main.cpp`
  decided operator-parenthesisation from the FIRST char only, so symbolic ops
  (`*.`) got `( *. )` but binding operators (`let+`, `and+`) — first char a letter
  — printed `val let+ :`. ocamlc prints `( let+ )`. Fix: any name with a
  non-identifier char (not alnum/`_`/`'`) is an operator. **+1 (shape-index/
  index_bindingops).** Committed `infer: parenthesise binding-operator val names`.

- **Type-var display names collided past `'z` — display, +1.** `show_rec` minted a
  var name as a single letter (`'a'+i%26`), so the 27th var reprinted `'a`; under
  the harness's first-appearance normalisation a collision merges two distinct
  vars, so any >26-variable signature could never match. Added `tvar_letter(i)`
  (letter i%26 + numeric suffix i/26, empty when 0 => `'a..'z,'a1..'z1,'a2..`,
  exactly ocamlc). **+1 (syntactic-arity/max_arity).** Committed `infer: type-var
  display names past 'z go 'a1,'b1,..`.

- **Format-type expectation not pushed through `if` — contained inference, +1.** A
  format string wrapped in an `if` (`Printf.printf (if b then "a\n" else "b\n")`)
  typed each branch as plain `string`, so printf's result param never resolved
  (`passed : bool -> 'a` vs oracle `bool -> unit`). A direct literal already worked
  via the bidirectional format-literal path; the `if` bypassed it. Fix:
  `infer_expr_expected`, when expected is a format and the expr is `if`-then-else,
  recurses into both branches with the format expectation. Scoped to formats (no
  change to ordinary `if`). **+1 (c-api/test_c_thread_has_lock).** Committed
  `infer: push format-type expectation through if branches`.

All three: reject 0.0%, lambda 54.2% flat, 0 crashes / 1853, match-set delta
exactly +1 each, 0 regressions.

**Baseline now: sig 469/525 (89.3%), reject 0.0%, lambda 54.2%, 0 crashes.**

### Assessed, NOT taken (fails the 0-regression session bar)
- **`prim-revapply/apply.ml` `_f : int->int->int` vs `int->int`** — optional-arg
  erasure via `%apply` (`bump @@ x`, `bump : ?cap:int -> int -> int` used where
  `'a -> 'b` expected). The erasure machinery exists (`infer_expr_expected`,
  ~line 2441) but only RECORDS slots for the Lambda back end; it returns the
  UN-erased type, so the inferred type keeps the extra arrow. Making erasure change
  the returned type is an engine-level change affecting every optional-arg
  application — risky for a single intentionally-unused (`_f`) binding. Deferred.
- The rest of the 59 DIFFs are the known blocked clusters: type-path naming
  (`Module.t`↔`t`, `lazy_t`↔`Lazy.t`, functor-result paths, Bigarray submodule
  qualifiers), format4↔format6 folding, GADT/object over-spec (`int Effect.t`,
  `< .. >`, `#castable`), first-class-module/`(module S with ..)` type recovery,
  generalized-open scope, and the Way-4 poly-variant row-variable rework.

## SESSION 2026-07-01 (i): polymorphic record fields + format-arg preservation (+1)

Re-scanned the live 56 DIFF files; the residue is overwhelmingly the known
blocked clusters (path-naming `Module.t`↔`t`, lazy_t↔Lazy.t, format4↔format6,
GADT/object over-spec, first-class-module type recovery, Way-4 rows). Found ONE
genuine reachable bug outside them: **polymorphic record fields**.

- **`{pf}` patterns over a poly field failed entirely — genuine bug, +1.** A
  universally-quantified field (`type pf = { pf : 'a. ('a,_,_) format -> 'a }`)
  is SKIPPED by register_record_decl (a single mono value scheme would clash
  across the field's uses), so a `{pf}` pattern couldn't even resolve its record
  TYPE (`test : int -> _ -> 'a`, oracle `int -> pf -> unit`). Two linked fixes:
  1. **poly-field record-type registry.** Record the owning record type per
     unique poly-field label (`poly_field_rec_`, ambiguity-filtered like
     `finalize_fields`). The `{pf}` pattern resolves to `pf` and binds the field
     variable to the field's GENERALIZED type (`from_coretype` mints generic
     vars) via a new `bind_poly_field` — so each body use instantiates fresh
     (the poly-record-field feature) instead of collapsing to Any. Isolated
     proof: `type poly = {p:'x.'x->'x}; let k {p} = p 3` now gives `poly -> int`.
  2. **format-arg preservation in from_coretype.** With (1) binding `pf`
     correctly, `pf "@]"` still gave `'a`: `from_coretype` collapsed a format
     annotation to a NULLARY `format6`, dropping the args, so the format
     literal's result param never unified. `from_cmi` already PRESERVES format
     args — made from_coretype consistent (keep args under the canonical
     `format6` name; `show` already renders a 3-arg format6 as `format`). This
     is the general fix for a format-typed function parameter (`g : (..) format
     -> 'a`) resolving its result from a literal — NOT the (still-blocked)
     format4↔format6 DISPLAY-name folding, which is orthogonal.
  **lib-format/domains.ml flips.** sig 469->470 (89.5%), reject 0.0%, lambda
  54.2% flat, 0 crashes / 1853, match-set delta exactly +1, 0 regressions.
  Committed `infer: polymorphic record fields + format-arg preservation`.

**Baseline now: sig 470/525 (89.5%), reject 0.0%, lambda 54.2%, 0 crashes.**
The poly-record-field substrate (registry + `bind_poly_field` + generic field
type) is now available for the other poly-field files (e.g. msg.ml's `wkind =
{ f : 'a. 'a tag -> 'a kind }`), though those additionally need extensible-GADT
constructor typing (a separate blocked cluster). No other engine-free flips
surfaced in the scan; residue is the documented blocked clusters.

## SESSION 2026-07-01 (j): the "blocked" path-naming cluster CRACKED via per-access-path (+10)

Re-scanned the live 55 DIFF files. The residue is dominated by the type-path
naming cluster that (f)/(g)/(h) all filed as "architecturally blocked, regression-
prone" (our unifier compares constr paths by LAST component, so `Result.t` can't
unify with `result`). **That framing was too pessimistic.** ocamlc's rule is
*per-occurrence*: a type keeps the PATH THROUGH WHICH IT WAS ACCESSED. Reproducing
that access-path faithfully SIDESTEPS the unification problem entirely — you never
have to unify `Result.t` with `result`, because within a file the same value is
always reached the same way. Ten flips landed this session, each a clean +N, 0
regressions, reject 0.0% / lambda 54.2% / 0 crashes throughout, sig **470 -> 480
(89.5% -> 91.4%)**:

1. **Module-nested variant type paths (+2: w33, topeval).** Variant ctor result
   types used the bare type name (`t`) while records already baked `mod_prefix_`
   (`N.t`). A ctor defined in `module N` used from outside (`g N.(A|B)`) printed
   `t` where ocamlc prints `N.t`. One-char fix (add `mod_prefix_`); unify still
   compares last component so `N.t`≡`t`.
2. **4-arg format6 -> `format4` abbreviation (+1: locale).** show already folded
   3-arg -> `format`; added the analogous 4-arg -> `format4`. Arg count comes from
   the source annotation (inference inherits it; fresh nodes are nullary), so it
   unambiguously selects the abbreviation.
3. **`open M` registers M's cmi variant ctors (+1: testarg).** `Arg.[ Unit f; Set
   r ]` left the bare `Unit`/`Set` as Any -> element typed `_`. Added
   open_module_ctors (load M's cmi, register each variant ctor as a generalised
   scheme into a scoped cenv; the local-`M.(..)` Pexp_struct_item path now pushes/
   pops a cenv scope too).
4. **Module-alias display `MP.t` (+2: minor_no_postpone, start_stop).** `module MP
   = Gc.Memprof` then `MP.t`: ocamlc keeps the alias, we expanded to
   `Gc.Memprof.t`. Record each `module M = Long.Path` alias, rewrite the target
   prefix back to the alias in the emitted signature. Restricted to EXTERNAL
   targets (head not locally bound): a LOCAL target (`Std2.M`) can be reached both
   directly and via the alias in one file and ocamlc keeps each occurrence's own
   path — a uniform rewrite corrupted gatien_baron's `Std2.M.t` result (caught by
   the diff-set check, fixed by the external-only guard).
5. **Qualified CONSTRUCTOR keeps its module's type path (+3: contexts_1/2/3;  +1
   pattern side: fma).** `Result.Ok`/`Result.Error` resolved via the bare name
   `Ok`/`Error` -> the re-exported base `result`; ocamlc follows the access path
   and prints `Result.t`. When a construct/pattern id is `Ldot` and M's cmi yields
   the ctor, prefer the qualified scheme (already built with result path `M.t`) by
   nulling the bare `sch` so the existing qualified branch runs (it also pins the
   args, so `Result.Error "x"` -> `(_, string) Result.t`). Non-strict only; bare
   uses still print `result`, exactly matching ocamlc's per-occurrence rule.
   Applied to both the expression and pattern Ppat/Pexp_construct handlers.

**Baseline now: sig 480/525 (91.4%), reject 0.0%, lambda 54.2%, 0 crashes.**

### What's STILL genuinely blocked in path-naming (distinct mechanisms, do NOT retry as freebies)
- **Alias UNIFIES with a differently-pathed base (qsort `string` vs `String.t`).**
  `%s` forces `string`; `String.compare`'s arg is `String.t`; these are the same
  type but differ in last component, so our unify-by-last-component treats them as
  distinct (lenient no-op) and the `%s`-side `string` wins the display. The
  ISOLATED `String.compare a b` already prints `String.t` correctly — the failure
  is ONLY when a foreign path unifies in. This is the real "record the access path
  on the constr node, survive unify" machinery; still deferred.
- **from_cmi doesn't qualify a TOP-LEVEL cmi type (specialized `kind` vs
  `Bigarray.kind`).** A submodule type already qualifies (`Bigarray.Array1.t` is
  right) but a type at the loaded module's TOP level comes back bare. A from_cmi
  path-construction slice (cmi_mod_prefix_ not firing for opened-submodule value
  resolution), not the ctor axis — dedicated, broader risk surface.
- **lazy_t<->Lazy.t** (reverse direction, keyword primitive not a qualified
  access; the session-g hamming regression stands), **functor-result paths**
  (ephetest3 `HW.key`/`SW.data`, pr6944 `Map.Make(String).t`), **qualified
  functional record update field typing** (intern/exception_callback lose
  `MP.allocation` from `{ MP.null_tracker with .. }`).
- Non-path residue unchanged: GADT/object over-spec (`int Effect.t`, `#castable`),
  first-class-module type recovery (pr6982/pr6954/compiling/syntactic_arity),
  sscanf-return typing (lib-seq/test, gen_test), weak-var propagation (testerror),
  generalized-open scope, and the Way-4 poly-variant row-variable rework.

**Verdict:** the access-path INSIGHT (reproduce ocamlc's per-occurrence path;
don't fight the unifier) converted the biggest "blocked" cluster into ten clean
flips. The remaining path-naming files each need a genuinely distinct mechanism
(constr-node access-path tracking through unify; from_cmi top-level
qualification), each a dedicated slice — not session freebies. Best next path-
naming investment: the from_cmi top-level-type qualification (specialized), the
most self-contained of the three.

## SESSION 2026-07-01 (k): from_cmi parent-module qualification (specialized, +1)

Took session (j)'s recommended next slice — the "from_cmi top-level-type
qualification" (specialized) — and it landed clean.

- **`specialized.ml` (`('a,'b) kind` -> `Bigarray.kind`) — DONE & committed**
  (`infer: qualify parent-module cmi types via enclosing scopes`). The sole diff:
  `test`'s param printed `('a,'b) kind` vs oracle `('a,'b) Bigarray.kind`. Root:
  `kind` is a TOP-LEVEL type of Bigarray but is referenced from the SUBMODULE value
  `Array1.create` (via `open Bigarray`). module_values set a single
  (cmi_types_ctx_, cmi_mod_prefix_) = (Array1's types, "Bigarray.Array1"); the
  Pident `kind` isn't in Array1's type list, so the qualification loop found
  nothing and left it bare (whereas Array1's OWN `t` qualified fine).
  - Fix: module_values now records EACH module level's `(types, cumulative-prefix)`
    as an enclosing scope (`cmi_scopes_`, outermost..innermost); from_cmi's
    qualification step searches them innermost->outermost, so a parent-owned type
    resolves to its own module (`Bigarray.kind`, not `Bigarray.Array1.kind`).
    Abbreviation-expansion still uses the innermost ctx only (unchanged surface).
    Other call sites (functor_result, load_module_record_fields) leave cmi_scopes_
    empty and hit the original single-ctx fallback.
  - **sig 480->481 (91.6%), reject 0.0%, lambda 54.2% flat, 0 crashes / 1853,
    match-set delta exactly +1 (specialized.ml), 0 regressions** (verified per-file
    via stash-and-rebuild, not just aggregate counts).

**Baseline now: sig 481/525 (91.6%), reject 0.0%, lambda 54.2%, 0 crashes.** Two
of session (j)'s three named path-naming residues remain (each distinct): the
constr-node access-path-through-unify machinery (qsort `string`↔`String.t`,
lazy_t↔Lazy.t with the hamming witness) and functor-result paths (ephetest3, pr6944).
Both need node-identity-stable access-path tracking through generalize/instantiate —
a dedicated slice, not a session freebie.

## SESSION 2026-07-01 (l): cmi ctor-arg qualification (+1) + local-module exceptions

Re-scanned the live 44 DIFF files. Two genuine bugs outside the blocked clusters,
both in cmi constructor handling:

- **cmi ctor-ARG types weren't qualified (`Seq.t`) — +1, committed.** A constructor
  read from a cmi (`Cons of 'a * 'a t` in Seq) had its ARGUMENT come back bare
  `'a t` instead of `'a Seq.t`: `qualified_ctor_scheme` / `open_module_ctors` called
  `from_cmi` with NO module context, so the same-unit qualification step (the one
  session (k) built for RESULT types) never fired for args. Fix: set
  `cmi_types_ctx_`/`cmi_mod_prefix_` with `fold_abbrevs_=true` (qualify but don't
  EXPAND — `t` stays `Seq.t`, not `unit -> 'a node`) around the from_cmi calls, in
  BOTH the explicit `M.C` path and the `open M`/`M.(..)` local-open path (lib-seq's
  `infinite` is built via `Seq.(Cons (.., infinite ..))`, so the open path mattered).
  **lib-seq/test flips.** sig 481->482 (91.8%), reject 0.0%, lambda 54.2% flat, 0
  crashes / 1853, match-set delta exactly +1, 0 regressions (serial stash-and-rebuild
  per-file diff).

- **local-module exception/typext ctors were never registered — correctness, 0 flip.**
  `process_item` had no `Pstr_exception`/`Pstr_typext` branch, so a ctor inside
  `let module M = struct exception E of int .. end` left `E x`/`M.E x` unconstrained
  (afl `fresh_exception : 'a -> unit` vs oracle `int -> unit`). register_types_rec
  only descends TOP-LEVEL modules, never an expression's `let module`. Added the
  branch, guarded by a per-node `ext_ctor_registered_` set so the two registration
  paths don't double-register (which would spuriously mark the ctor ambiguous).
  afl's fresh_exception now matches; the file still DIFFs on the unrelated blocked
  lazy_t axis, so no sig flip -- but a clean latent-bug fix, 0 regressions.

**Baseline now: sig 482/525 (91.8%), reject 0.0%, lambda 54.2%, 0 crashes.** The
cmi-arg qualification generalises session (k)'s per-occurrence-path insight from
RESULT types to ARGUMENT types; residue is still the two blocked path-naming
mechanisms + Way-4 rows + GADT/object over-spec + first-class-module recovery.

## SESSION 2026-07-01 (m): `%a`/`%t` printer tying + cmi submodule-head qualification (+1)

Re-scanned the live 43 DIFF files. Most are the known blocked clusters (lazy_t↔Lazy.t,
GADT/object over-spec, first-class-module recovery, weak-var/relaxed-value-restriction,
Way-4 rows). `change_layout.ml` stood out: its `val` diffs were a MIX of a genuine
inference bug AND path-naming — clearing BOTH flipped it.

- **`%a`/`%t` format printers didn't tie their element type — genuine bug, 0 flip
  alone but correctness.** `format_arrow` (the arg-arrow built for a format literal
  so a consumer flows value-kinds) pushed TWO untied `any()` for `%a` (fn + value).
  ocamlc ties them: `%a`'s printer is `chan -> 'v -> pres` and its value arg is `'v`
  (same `'v`); `%t`'s printer is `chan -> pres`. Fixed: `%a` mints a fresh `'v`,
  builds `arrow(chan, arrow('v, pres))` + `'v`; `%t` builds `arrow(chan, pres)`.
  `chan`/`pres` come from the expected format6's params [1]/[2] (Format.formatter /
  unit for fprintf), passed in via the full args vector. Isolated proof:
  `fprintf ppf "%a" pp a` now gives `pp:fmt->'a->unit, a:'a` (was `_`/`_`); cleared
  ALL of change_layout's inference diffs (print_array/print_index/report), leaving
  only path-naming.
- **cmi Pdot type whose head is a submodule wasn't qualified — path-naming, +1.**
  A top-level `Bigarray.reshape` (reached bare via `open Bigarray`) returns
  `Genarray.t` (an Ldot in the cmi); we printed `Genarray.t`, oracle
  `Bigarray.Genarray.t`. Session (k)'s scope-qualification only handled Pident (bare)
  type names. Added a parallel `cmi_mod_scopes_` (submodule lists + prefix per scope,
  set alongside `cmi_scopes_` in resolve_module_values_comps); from_cmi now, for a
  Pdot path whose innermost head Pident is a submodule of the value's owning module,
  prepends that module's prefix (searched innermost->outermost). Explicit `Sub.f`
  access is unaffected (it resolves through the `Bigarray.Sub` prefix path already;
  cmi_mod_scopes_ is only set in the open/bulk-load path).
  **change_layout flips.** sig 482->483 (92.0%), reject 0.0%, lambda 54.2% flat, 0
  crashes / 1853 files, match-set delta exactly +1, 0 regressions.

**Baseline now: sig 483/525 (92.0%), reject 0.0%, lambda 54.2%, 0 crashes.** The `%a`
tie is the general fix for any `%a`-printer function param resolving its element from
the value (a printer-combinator idiom); the Pdot qualification generalises the
per-occurrence path insight to submodule types reached via a top-level value. Residue
unchanged: the two remaining path-naming mechanisms (constr-node access-path-through-
unify: qsort/lazy_t; functor-result paths), GADT/object over-spec, first-class-module
recovery, weak-var/relaxed-value-restriction (testerror), Way-4 rows.

## SESSION 2026-07-01 (n): tuple-pattern let generalization (+1)

Re-scanned the live 42 DIFF files. Residue is overwhelmingly the known blocked
clusters (lazy_t↔Lazy.t, GADT/object over-spec, first-class-module recovery,
functor-result paths, sscanf-return typing, weak-var, Way-4 rows). Found ONE
genuine reachable bug: **tuple-pattern `let` bindings never generalized their
components.**

- **`let a, b = (e1, e2)` bound each name monomorphically — genuine bug, +1.**
  `bind_pattern_scheme`'s non-var fallback did `try_unify(infer_pat(p), te)`:
  infer_pat minted FRESH outer-level vars for the pattern's variables and unified
  them with the (generalized) RHS tuple type, trapping every component at the
  current level → monomorphic, pinned by later use (`let id2, id3 = (fun x->x),
  (fun y->y); id2 1` gave `id2:int->int, id3:'_weak` vs oracle `'a->'a` both).
  Fix: a Ppat_tuple over a matching-arity Tuple type now recurses component-wise,
  binding each var to its GENERALIZED sub-type (each gets its own scheme). Kept the
  unify fallback for non-tuple patterns (a first attempt that also peeled
  Ppat_constraint/Ppat_alias regressed alloc.ml — `let (_:int) = f ~a ~b` dropped
  the `int` annotation that constrained `f`'s result; the fallback's infer_pat is
  what unifies a constraint, so only the pure-structural tuple case is special-cased).
  Second half: made a local open `M.(e)` (Pexp_struct_item wrapping an open)
  non-expansive iff its body is, so `let a,b = Format.(x,y)` generalizes like the
  bare tuple (matches ocamlc is_nonexpansive on Texp_open). **pp_print_custom_break
  flips** (its `let fprintf, printf, list = Format.(..)` now stays polymorphic).
  sig 483→484 (92.2%), reject 0.0%, lambda 54.2% flat, 0 crashes / 1853, match-set
  delta exactly +1, 0 regressions. Committed `infer: generalize tuple-pattern let
  bindings + local-open non-expansiveness`.

**Baseline now: sig 484/525 (92.2%), reject 0.0%, lambda 54.2%, 0 crashes.** No
other engine-free flips surfaced; residue is the documented blocked clusters
(functor-result/first-class-module recovery, GADT/object over-spec, sscanf-return,
weak-var, lazy_t↔Lazy.t constr-node access-path, module-shadow `Stdlib/2` display,
Way-4 rows).

## SESSION 2026-07-01 (o): functional record update field typing (+3)

Re-scanned the live 41 DIFF files; all mapped to documented blocked clusters, but
the statmemprof trio (restart, intern, exception_callback) shared ONE root and was
reachable — the "qualified functional record update field typing" residue named in
(j).

- **Functional record update dropped the overridden field's type — genuine bug, +3.**
  `{ M.null_tracker with alloc_minor }` (`M = Gc.Memprof`) inferred each overridden
  field value in isolation and discarded it, so the punned `alloc_minor` leaked a
  free var (`('a -> 'b) -> (..) M.tracker` vs oracle `(M.allocation -> 'a option) ->
  ..`). We ALREADY resolved the base `M.null_tracker : ('a,'b) M.tracker` correctly —
  the only miss was constraining the field. Two linked fixes:
  1. **update branch ties fields.** The plain-construction path already unifies each
     field value to its `field_scheme` (arrow(recTy, fieldType)); do the same in the
     `{ base with .. }` branch — unify the scheme's record-type (dom) with the base
     (sharing type params) and the value against the field type (cod).
  2. **load record fields for `module M = ExternalPath` aliases** (only `open`/`include`
     did before), so the field label resolves; the existing alias display rewrite maps
     the loaded `Gc.Memprof.` prefix back to `M.`. Guarded load_module_record_fields
     against double-loading (a module both referenced and aliased would push each label
     twice -> spuriously ambiguous; unified the two prior guard sites into one internal
     guard).
  **restart/intern/exception_callback flip.** sig 484->487 (92.8%), reject 0.0%,
  lambda 54.2% flat, 0 crashes / 1853, match-set delta exactly +3, 0 regressions.
  Committed `infer: constrain overridden fields in functional record update`.

**Baseline now: sig 487/525 (92.8%), reject 0.0%, lambda 54.2%, 0 crashes.** All 38
remaining DIFFs are the documented blocked clusters (functor-result/first-class-module
recovery incl. modular-explicits `(module P : Print)`, GADT/object over-spec incl.
`int Effect.t`/`#castable`/`< .. >`, extensible-GADT ctor typing (msg), lazy_t<->Lazy.t
+ `String.t`<->`string` constr-node access-path, scanf-return, weak-var, Stdlib/2
shadow-disambiguation, Way-4 rows). No other engine-free flip surfaced in this scan.

## SESSION 2026-07-01 (p): GADT-match ground result recovered (+1)

Re-scanned the live 38 DIFFs (did NOT trust (o)'s "all blocked" verdict -- (j)
cracked a cluster (o)-style scans had filed as blocked). Confirmed each file needs
one hard mechanism; scanf-return is ALSO blocked at the substrate level (we fold
format6 to a nullary `'a format6`, discarding the param structure, so the receiver
arrow can't propagate -- a real format6-param-tracking rework, not a session flip).
The one reachable BUG was inside the "GADT over-spec" cluster:

- **A GADT `match` never recovered a non-abstract result -- genuine bug, +1.** A
  GADT match types each arm inside a rolled-back window (so `a := int` in one arm
  can't leak to the next), which ALSO discarded the arm-result unification, leaving
  the whole match a fresh var (`w04_failure : 't repr -> 't repr -> 't -> 'b` vs
  oracle `-> unit`). Fix: after the window rolls back the refinement, if EVERY arm
  yields the SAME ground type it is the genuine result (all arms `()` -> `unit`) and
  is unified outward. Arms that disagree on a ground type (`Int -> 100` : int vs
  `Ptr -> p` : int list) or any non-ground arm leave the result OPEN, so a `: a`
  return annotation still pins it -- this is what keeps register_typing /
  register_typing_switch at `'a typ -> int list -> 'a` (both regressed on a first,
  cruder "any ground arm -> pin" attempt; caught by the diff-set check).
  - KEY GOTCHA: the agreement check must be a DIRECT structural compare
    (`ground_types_equal`, last-component paths like the unifier), NOT
    `Engine::unify` -- the signature pass runs `lenient`, so a real int-vs-int-list
    clash is silently accepted and would over-specialise the result. (First fix used
    unify-in-a-try and silently mis-pinned; the lenient flag was the culprit.)
  **w04_failure flips.** sig 487->488 (93.0%), reject 0.0%, lambda 54.2% flat, 0
  crashes / 1853, match-set delta exactly +1, 0 regressions. Committed `infer:
  unify a GADT match result outward when all arms agree on one ground type`.

**Baseline now: sig 488/525 (93.0%), reject 0.0%, lambda 54.2%, 0 crashes.** The
partial GADT-result recovery also improved measure_runtime_arity's
`arity_description` (now `'a arity -> string`) though that file stays DIFF on the
separate `runtime_arity` arg-to-result tie. Residue is the SAME hard clusters, each
still a dedicated project: full GADT escape-scope (effects `'a Effect.t`,
measure_runtime_arity's `runtime_arity`), scanf-return (blocked on opaque-format6),
lazy_t/String.t access-path-through-unify, functor-result/first-class-module
recovery, objects (`#castable`, `< .. >`, pr14554_1), relaxed value restriction
(testerror), Stdlib/2 shadow, %apply optarg erasure, Way-4 poly-variant rows.

## SESSION 2026-07-01 (q): annotation type-var scoping + local-struct GADT window (+1)

Re-scanned the live 37 DIFFs. Residue is the documented blocked clusters, but
`measure_runtime_arity.ml` was reachable via THREE linked scoping bugs, all
genuine (not display-naming):

- **Named type vars weren't shared across a binding's annotations — genuine bug.**
  `let f (x:'a) (y:'a) : 'a list = ..` minted a FRESH `'a` per annotation
  (each `from_coretype` got its own map), so siblings and param↔return diverged
  (`'a -> 'b -> 'a * 'b`, `('a->'b) -> ('c->'d) box`). It only SHOWED when nothing
  else tied them — a non-existential return ctor (`list`) unified them anyway, but
  an existential GADT return (`box`, `arity`) hid the tie, exposing the bug. Fix:
  a per-binding `annot_vars_` map (set in infer_bindings, threaded through
  `Ppat_constraint` / the function return constraint / the `let x:T` constraint),
  so one `'a` spans all of a binding's annotations (OCaml's structure-item var
  scoping). Fixed `runtime_arity`'s line + isolated `f (x:'a) (y:'a)`, nested-fun,
  param↔return cases.
- **Local-struct GADT ctors were never registered — genuine bug.** `is_function`'s
  GADT type lives in `let open struct type _ is_function = .. Is_function : (_->_)
  is_function end in match is_function x with ..`. register_types_rec never
  descends an expression's `open struct`, so `Is_function` missed `gadt_ctors` →
  the match wasn't windowed → its branch-local `a := _->_` refinement leaked into
  the result (`('a->'b) arity`). Added `register_local_gadt_markers` (gadt name/ctor
  markers only — no stamps/errors, so reject stays safe) in the Pexp_struct_item
  open handler. Mirrors session (l)'s local-exception registration.
- **`newtype_vars` wasn't scoped — genuine bug.** The map is flat/global; the inner
  `is_function (type a)` CLOBBERED the outer `maybe_runtime_arity (type a)`, so the
  outer's return annotation `a arity option` resolved `a` to the inner node
  (`'a -> 'b arity option`). Fix: infer_function saves/restores the newtype names
  it introduces, so a nested `(type a)` can't leak out.

All three were required for the flip. **measure_runtime_arity flips.** sig 488->489
(93.1%), reject 0.0%, lambda 54.2% flat, 0 crashes / 1853, match-set delta exactly
+1, 0 regressions. Committed `infer: scope annotation type-vars + newtypes; window
local-struct GADT matches`.

**Baseline now: sig 489/525 (93.1%), reject 0.0%, lambda 54.2%, 0 crashes.** The
annotation-var-sharing and newtype-scoping fixes are general (any binding with
repeated/tied annotation vars, any nested `(type a)`) though only this file flipped
on them. Residue unchanged: lazy_t↔Lazy.t & String.t↔string constr-node access-path,
full GADT/object escape-scope (`int Effect.t`, `#castable`, `< .. >`), scanf-return
(opaque format6), functor-result/first-class-module recovery, extensible-GADT ctor
(msg), weak-var/abbrev-expansion-in-unify (testerror `Arg.anon_fun`), abstract-alias
display (Pos.t/Buffer.t), Stdlib/2 shadow, generalized-open sig restriction, %apply
optarg erasure, Way-4 poly-variant rows.

## SESSION 2026-07-02 (r): Way-4 FUNDED AND (mostly) LANDED — morematch, nested, +pr14554_1 (+3, sig 93.7%)

The user explicitly funded the Way-4 project. Verdict up front: **the plan's
"row-VARIABLE model required" framing was wrong for 2 of the 5 residue files.**
morematch needed typecore's build_as_type variant case (per-occurrence fresh
rows); nested needed the occurs relaxation + cyclic instantiation + cycle-aware
display. Neither needed `{fields; tail_var}`. Three commits, each gated
(reject 0.0%, lambda 54.2% flat, 0 crashes / 1853, exact match-set delta, 0
regressions):

1. **morematch (+1)** — `infer: variant-pattern alias rebuilds a fresh [> row;
   #t patterns bound [< by their abbreviation`. (a) build_as_type gains the
   Tpat_variant case: `| `Nil | `Cons _ as x -> `A x` binds x to a FRESH open
   `[>` row (tag args shared with the scrutinee row's) — input `[<` and output
   `[>` rows become independent, exactly ocamlc. (b) `#t` patterns (Ppat_type,
   previously falling to fresh var) build `[< t's tags-with-DECLARED-args ]`
   from the abbreviation manifest (maf's `#recurs_type_expr` ties `` `TConstr
   of type_expr list ``).
2. **nested (+1)** — `infer: recursive rows through occurs; cyclic-instance
   copy; arrow/exact-row as display`. Four pieces: (a) occurs_and_lower allows
   an occurrence whose path passes THROUGH a row node (OCaml's no-rectypes
   rule; strict passes build no rows → reject structurally protected); (b)
   instantiate cycle-preserving ccopy for small (≤64) all-generic-row cyclic
   scheme regions (fresh shells pre-registered in the memo; each use gets its
   own cycle so a match can close the instance without mutating the scheme);
   (c) merge rule: `[>` meeting `[<` records constructed tags as present, and
   present covering the allowed set closes the row EXACT (`let (`A x) = r ()`
   → `[ `A of 'a ]`); (d) show: cyclic ARROWS `as`-named via a find_cycles
   pass; count_refs guards arrows by DFS STACK only (cycle edges don't inflate
   the inner row's rc, DAG-shared arrows still re-count — preserves
   recursive_module_init); a row ON a cycle is named even when EXACT.
   **Mid-session catch:** the exact-cyclic-row combination initially crashed
   mixin (exact rows were excluded from as-naming → infinite print); the cyc
   set fixed it and mixin's subst1 now prints the oracle's exact recursive row.
3. **pr14554_1 (+1 BONUS, outside the cluster)** — `infer: object-type
   annotations + cyclic constr/tuple as naming`. Ptyp_object was entirely
   unhandled in from_coretype (annotation dropped); cyclic CONSTR/TUPLE nodes
   now `as`-name like arrows (`< bark : 'a -> unit > t as 'a`). The flip
   stacked on session (q)'s per-binding annot map ('this ties across
   annotations) + the occurs relaxation (object cycle) — three sessions'
   features composing.

**Cluster status: sig 492/525 (93.7%), reject 0.0%, lambda 54.2%, 0 crashes.**
The exhaustive remaining poly-variant residue (oracle sig contains backticks)
is exactly the 3 mixins:

- **mixin** — needs variant-abbreviation display FOLDING (`'a lambda`/`var`
  for structural rows). Blocked underneath by alias equality (`Names.elt` ≡
  `string` via functor — the same access-path/alias cluster as qsort
  String.t↔string), plus true row-variable input→output tail relationships
  for map_lambda's `[>` output kind.
- **mixin2** — total inference loss (`val lambda : 'a`): lazy fixpoints
  through object rows, plus `[ | 'a lambda ] as 'a` inherited-row display.
- **mixin3** — object-row abbreviation folding to `(.., ..) ops` + `#ops`
  (open object type) display on top of everything mixin needs.

These stack 2-4 features each, half of them on the BLOCKED alias-equality
cluster — genuinely the dedicated project the plan describes, and much smaller
now (3 files). **The poly-variant plan is otherwise put to bed: 9 of the
original 12 DIFF files landed** (pr10664, recursive_module_init, ref_spec,
pr6836, bar/pr6899_second_bad, exotic, morematch, nested + the adjacent
pr14554_1), with the crash substrate permanent and every gate green throughout.

## SESSION 2026-07-02 (s): mixin.ml funded — abbreviation-carrying rows landed, 21→9 diff lines, flip blocked

Attempted the recommended mixin.ml slice. The KEY INSIGHT held: ocamlc doesn't
structurally FOLD rows back to abbreviations — it expands an annotation's
abbreviation for unification but REMEMBERS it on the type node for display.
Reproducing that (an `abbrev`/`abbrev_args` stamp on Variant nodes) sidesteps
the `Names.elt`≡`string` equality problem entirely for the folded positions.
Committed `infer: abbreviation-carrying variant rows + function-cases
constraints` — all gates green (sig 492 flat, reject 0.0%, lambda 54.2%, 0
crashes, 0 regressions):

- from_coretype display-pass expansion of variant-abbreviation constrs with the
  abbrev stamp; carried through merge (dropped if the tag set grows) /
  instantiate copy / ccopy; count_refs + find_cycles walk abbrev_args.
  TWO crash bugs found & fixed by the per-change canary run: copy(abbrev_args)
  must be memo-registered BEFORE recursing (recursive abbrev arg → infinite
  recursion), and a fixpoint row (`'a lambda as 'a`) must print UNFOLDED (also
  what ocamlc does — free1/free2 etc.).
- **infer_function bug (general, beyond mixin): a `function`-cases constraint
  annotates the whole `arg -> rt` arrow**, not rt (`let f ~x : (t1 -> t2) =
  function ..`) — it was silently dropped (lenient arrow-vs-result no-op).
- build_as_type `#t as x` rebuilds an OPEN `[>` row (typecore as-types), fixing
  map_lambda's output-row kind.

**Result: mixin.ml went 21 → 9 differing val lines** (subst_var, free_var,
free_lambda, map_lambda, free_expr, map_expr, free/subst/eval, print,
free2/subst2/eval2 all now match). The remaining 9 are NOT display: they are
per-occurrence arg-path products (`Subst.key` vs `string` vs `Names.elt`
occupying arg slots — the blocked access-path-through-unify cluster, in both
directions on different lines) plus one deep inference tie (subst_lambda's
`free` applied to both the input's arg and the subst-map elements through the
`Subst.fold` labelled-closure chain; the basic var-callee double-application
tie DOES work in isolation) and eval_lambda/eval_expr row-kind/sharing shape
divergences. **Verdict: mixin.ml cannot flip until the access-path cluster is
funded; the abbreviation machinery (this slice) was its display half and is
done.** mixin2/mixin3 remain the class-system project (unchanged assessment).

## SESSION 2026-07-02 (t): the access-path cluster CRACKED — lazy_t + String.t axes (+5, sig 94.7%)

Funded the cluster blocking mixin.ml's residue and 5 corpus files. The method:
probe ocamlc's behavior into a decision matrix (12 witness probes), then
engineer to the matrix — NOT to half-remembered ctype.ml internals (two hours
of unify3 archaeology produced contradictions; the probes settled everything).

ocamlc's semantics, as probed: a construction carries the PRIMITIVE path
(`lazy 1` : lazy_t, `%s` : string); a LIVE value flowing into an
abbreviation-typed context during inference ADOPTS the abbreviation (hamming's
rec-group Lazy.t, qsort's String.t); a FINALIZED binding is never flipped by
later uses (instance-copying protects it). Implementation (2 commits, all
gates green):

1. **Primitive/abbreviation family table in unify** ({lazy_t,string,bytes} ×
   {Lazy,CamlinternalLazy,String,StringLabels,Bytes,BytesLabels}): compatible,
   arg-unifying, and the primitive-pathed node RELINKS to the
   abbreviation-pathed one. Stamped (shadowing) types excluded.
2. **Finalized-head protection**: generalize/demote stamp family-primitive
   constr heads GENERIC; instantiate fresh-copies a stamped head per use
   (args shared — constraints still flow), so later-use relinks hit the copy.
3. **instantiate share-unchanged bug** (the trap that killed session (g)'s
   attempt): the copied-child comparison used the ORIGINAL arg pointer, but a
   link-wrapped child (var resolved to concrete) reprs to a different node —
   spuriously "changed", re-copying shareable structure per use, so relinks
   landed on copies. Fixed: compare against the repr.

**+5: afl, lazy7, lazy_, test_module, qsort. hamming verified protected.**
sig 492→497 (94.7%), reject 0.0%, lambda 54.2%, 0 crashes, 0 regressions.

Remaining access-path axis (mixin.ml's `Subst.key`, ephetest3's `HW.key`,
pr6944's `Map.Make(String).t`): FUNCTOR-RESULT abbreviations — the same
per-occurrence rule but the abbreviation is a functor instance, so the family
table doesn't cover it; needs the functor-instantiation machinery to stamp
paths on its result types. That is now the cluster's last mechanism.

## SESSION 2026-07-02 (u): functor-instance abbreviations — the family model completed (+1, sig 94.9%)

Funded the third access-path axis (`HW.key`/`Subst.key`). Hardest slice of the
cluster: FIVE mid-flight corrections, each caught by the probe/witness suite
(ref_spec's shared-row split, boxedints' annotation flip, fill_hw's capture
timing, lib-seq's folded tail, nested's zip-cycle) — the per-change witness
discipline carried the whole thing. Committed `infer: functor-instance
abbreviations join the family relink`:

- from_cmi marks manifest-carrying functor-result types `functor_abbrev`.
- unify's family rule generalized to a PRIORITY model: functor abbrev (2) >
  stdlib abbrev (1) > predef primitive (0); lower relinks to higher on live
  contact; 2-2 ties link first→second. All scalar primitives + their stdlib
  modules participate now (int/Int64.t etc., not just lazy/string/bytes).
- TWO finalization flavors: annotation-written paths + toplevel bindings stamp
  GENERIC (unify guard: never relink); SCHEME heads (cmi/functor/external)
  also mark scheme_head → instantiate fresh-copies per use (live instances
  adopt; scheme nodes never change). Copying annotation heads instead
  cascaded `changed` and split ref_spec's `as 'a` row.
- occurs_and_lower finalizes heads captured by an OUTER binding's var
  (ephetest3's `hw` keeps `SW.data` through later HW.key contacts).
- rec-binding display ZIPS te's arrow doms (adopted param paths — fill_hw :
  HW.key) with tv's tail (folded abbreviations — infinite : .. Seq.t).

**ephetest3 flips.** sig 497→498 (94.9%), reject 0.0%, lambda 54.2%, 0 crashes,
0 regressions. mixin.ml: 9→8 diff lines (its Subst.key positions improved).

**Access-path cluster status: all three mechanisms landed** (stdlib families,
per-occurrence paths, functor instances). Cluster residue: pr6944's APPLICATIVE
PATH display (`Map.Make(String).t` for an unnamed application's abstract t — a
display-only naming feature), and mixin.ml's last mixed lines (the Subst.fold
labelled-closure tie + eval row shapes). Known imperfection: an if-branch 2-2
tie converges where ocamlc keeps slots separate (synthetic probe only; no
corpus witness).

## Honest scope notes
- The `.cmi` bridge currently makes Variant opaque (a fresh var). This plan
  improves the DISPLAY/sig metric; emitting correct variant `.cmi`s is a separate
  later step (the sig metric is the proxy, not the artifact).
- Success = Tiers 1–3 land (~7 files, sig ~85.3%→~86.5%) with zero regression and
  zero crashes; Tier 4 documented if deferred. That "puts to bed" the tractable
  poly-variant work and converts the open-ended risk into a bounded residue.
