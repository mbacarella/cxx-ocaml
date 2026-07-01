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

**DECISION (2026-07-01): STOP the poly-variant grind after phases 0-1.** Three
sub-attempts (recursive occurs, instantiate sharing, and the earlier pattern-row)
each confirm: every remaining corpus file stacks 2-3 features AND the engine
changes are crash-prone on deep/recursive/object types.  The floor is landed
(pr10664, crashes gone).  The residue is now precisely understood (above) and is
a dedicated multi-feature project, not a session's incremental wins.  Best next
poly-variant investment if resumed: the CROSS-OCCURRENCE `as 'a` show rework
(no engine/crash risk, unlocks the shared-row display that several files need)
paired with a crash-safe (iterative/memoised) monomorphic-row-share in instantiate.

## Honest scope notes
- The `.cmi` bridge currently makes Variant opaque (a fresh var). This plan
  improves the DISPLAY/sig metric; emitting correct variant `.cmi`s is a separate
  later step (the sig metric is the proxy, not the artifact).
- Success = Tiers 1–3 land (~7 files, sig ~85.3%→~86.5%) with zero regression and
  zero crashes; Tier 4 documented if deferred. That "puts to bed" the tractable
  poly-variant work and converts the open-ended risk into a bounded residue.
