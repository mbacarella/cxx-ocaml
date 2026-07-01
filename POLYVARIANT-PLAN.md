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

## Honest scope notes
- The `.cmi` bridge currently makes Variant opaque (a fresh var). This plan
  improves the DISPLAY/sig metric; emitting correct variant `.cmi`s is a separate
  later step (the sig metric is the proxy, not the artifact).
- Success = Tiers 1–3 land (~7 files, sig ~85.3%→~86.5%) with zero regression and
  zero crashes; Tier 4 documented if deferred. That "puts to bed" the tractable
  poly-variant work and converts the open-ended risk into a bounded residue.
