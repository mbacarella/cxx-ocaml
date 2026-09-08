# Burn ledger — hand-off to the juice session (2026-09-06)

Work done on the laptop **burn** while juice (≈9 days ahead of this checkout,
effid ~99%) was offline.  Everything here is on branch `burn/testsuite`, off
`cpp-rewrite` @ `5f4b62c7c2`, in two commits that touch only
`cxx/src/tools/cppocamlc_main.cpp` and a new `cxx/harness/testsuite_delta.sh`.
Nothing in `lambda.cpp` / `infer_check.cpp` was changed.  Merge order:
juice's `cpp-rewrite` first, then this branch on top.

## 1. What was verified on burn

Every committed gate reproduces at `5f4b62c7c2`: exec 722/722, DDC 139 .cmo +
216 .cmi, stdlib-DDC 65+71, effid 125/139, typedtree 1015/1017 (same 2 DIFF),
cmi 1011/1035, sig 689/693, lambda 500/744, parse 1801, false rejects 4,
false accepts 38.  The +1 denominators come from the sibling-cmi probe finding
43 dirs where the journal recorded 42.

Environment facts worth keeping:
- Run everything inside `nix develop`; outside it native and `-custom` links
  fail (system ld vs nix glibc 2.40, `__isoc23_strtol`).
- **Never `make` in this tree.** It is configured `--disable-ocamltest`, and
  `parsing/parser.ml` + `camlinternalMenhirLib.cmi` carry Jun-19 mtimes the
  rest of the tree lacks, so make wants to rebuild parsing/typing with
  `boot/ocamlc` -- and fails on an inconsistent `Stdlib__Lexing`.  It would
  overwrite the oracle.  `testsuite_delta.sh` builds ocamltest from the
  ocamltest/-scoped lines of `make -n` instead.
- `/tmp` does not survive a machine move: gate baselines (`/tmp/base_*.txt`),
  oracle caches and the per-slice probe batteries (`/tmp/s3xx/bx`) were lost
  in the restore.  Batteries that matter should live in the repo.
- `exec_parity.sh` needs `CPP_TIMEOUT=60` on an 8-core box, else four slow
  GC/lazy tests time out on the ORACLE side and silently leave the
  denominator (722 -> 718).
- `gate_check.sh` hardcodes JOBS=24 and its baseline comments are stale
  (lambda DIFF is 244, not 325).

## 2. The testsuite run (`cxx/harness/testsuite_delta.sh`)

OCaml's own testsuite via ocamltest with `c++ocamlc` standing in for
`ocamlc` (shadow `OCAMLSRCDIR`, `ocamlrun` shim), against the same suite run
with the real `ocamlc`.  ~6 min for both passes.

| | real ocamlc | c++ocamlc |
|---|---|---|
| tests passed | 982 | 205 |
| failing only `compare-bytecode-programs` (exe bytes vs ocamlc.opt; compile, run, output all correct) | — | 435 |
| **behaviourally passing** | 982 | **640** |

Regressions by first failing action: 127 expected-error tests c++ocamlc exits
0 on, 47 `.c` stub inputs, 42 compiler-output text, 33 program-output diffs,
19 parse-error wording, 18 run failures, 14 ocamlobjinfo, ~20 flags.
Full list with first compiler-output line: `/tmp/testsuite_delta/regressions.tsv`
after a run.  Expect/toplevel tests fail identically on both sides
(`testsuite/tools/expect` unbuilt) and cancel.

Three driver bugs had to be fixed before a single test could be judged
(commit `4e0b69014a`): `-use-runtime <file>` was classed boolean (every link
took the runtime path as an input), `-with-runtime` the inverse, and
`-nostdlib` implied `-nopervasives` (Bytelink.link gates only on the latter).

## 3. Items for juice (lambda.cpp / infer_check.cpp territory)

### 3a. A multi-module miscompile no gate had seen
`tests/basic-modules/main.ml` prints `70368541778324` where the reference says
`1`.  exec_parity compiles files one at a time, so it never ran this test.
Reproduce: `cd testsuite/tests/basic-modules; c++ocamlc -I ../../../stdlib
-nostdlib <modules per TEST block> main.ml -o m && ocamlrun ./m`.

### 3b. The driver never runs the strict type check -- DECISION NEEDED
`c++type --check` rejects every `_bad` file probed; `c++ocamlc -c` accepts
them.  Of the 127 expected-error tests c++ocamlc accepts, **96 would be
rejected by the checker c++ocamlc already links** -- a wiring gap in
`cppocamlc_main.cpp`, not 96 inference holes.  Wiring it in has a cost: the
checker's 4 known false rejects (`basic/patmatch.ml`,
`typing-extensions/cast.ml`, `typing-modules-bugs/pr7036_ok.ml`,
`typing-modules/inclusion_errors_with_renamed_source_file/foo.ml`) become
compile failures -- and, measured on 2026-09-07, **the checker rejects 76 of
the 139 DDC compiler modules**, 64 of them one mechanism: an abbreviation is
not expanded across cmis ("type constructor mismatch: Warnings.loc vs
Location.t", "Misc.modname vs string", "list vs Parsetree.attributes", ...);
the rest are unbound module (LargeFile), a value-restriction report
(printtyp.ml) and "cannot unify" in camlinternalMenhirLib.  Also the checker
exits 1 where ocamlc exits 2, and its messages lack the `File "..", line N`
header and source excerpt, so the 96 tests would move from FALSE_ACCEPT to
exit-status/text mismatches rather than pass.  Conclusion: `c++ocamlc -c`
SHOULD run the strict check -- that is what ocamlc does -- but not until the
checker is at 0 false rejects over the DDC corpus; wiring it in today breaks
DDC.  The 76 are the work-list (`c++type --check` over the DDC staging dir with
the sibling-cmi root pointed at it reproduces them).

### 3c. 31 genuine checker false accepts (24 already in accept_parity's 38)
tests/generalized-open/funct_body.ml lib-fun/test_stdlib_todo_flag_error.ml
shadow_include/cannot_shadow_error.ml tool-ocamlc-compat32/compat32.ml
tool-ocamlc-error-cleanup/test.ml tool-ocamlc-stop-after/stop_after_scheduling.ml
typing-missing-cmi/test.ml typing-modules-bugs/{pr10693,pr6293,pr6427,pr6752,
pr6899_second,pr6992,pr7112,pr7414_2,pr7414}_bad.ml
typing-objects-bugs/{pr3968,pr4018,pr4824a,pr7284}_bad.ml
typing-polyvariants-bugs/pr5057a_bad.ml typing-recmod/{t12,t14}bad.ml
typing-recovery/{illegal_reference_to_recursive_class,less_general,
partial_tuple_pattern_bad_type,unbound_type_constructor}.ml
typing-rectypes-bugs/{pr5343,pr6174,pr6870}_bad.ml typing-sigsubst/test_locations.ml
(several of the tool-* ones are flag semantics -- -compat32, -stop-after,
missing-cmi -- rather than typing.)

### 3d. Program-output / run divergences not explained by `-g`
- `memory-model/forbidden.ml`, `memory-model/publish.ml`: empty output.
- `statmemprof/comballoc.ml`, `lib-threads/uncaught_exception_handler.ml`,
  `tool-ocamlc-determinism/determinism.ml`.
- `tmc/stack_space.ml` runs out of stack -- touches the Aug-27 TMC slice.
- `tmc/partial_application.ml`, `local-functions/non_local.ml`,
  `translprim/{array_spec,comparison_table,module_coercion,ref_spec}.ml`,
  `functors/functors.ml`, `basic-modules/anonymous.ml`: `-dlambda` text tests.
  Part of the diff is honouring `-dno-unique-ids`/`-dno-locations` in
  print_dlambda (driver now accepts the flags silently; the printer ignores
  them); the rest is real Lambda difference.
- The 20 `backtrace/*` tests, `effects/backtrace.ml` and the 6
  `tool-debugger/*` tests only need `-g` debug events (section 4).

### 3e. Warnings
`warnings/w01 w03 w04 w04_failure w06 w32 w32b w33 w45 w47_inline
w47_ppwarning w50 w51_bis w53 w53_across_cmi w54 w60`, `deprecated_*`,
`no-alias-deps/aliases.ml` (W49): c++ocamlc emits no warnings at all.
`utils/warnings.ml` (option parsing, numbering, letters, printing) is a
self-contained port; the emission points are in the typer.

### 3f. Native back end -- survey only, backed out
`ocamlopt -dlambda` differs from `ocamlc -dlambda` on 696/696 corpus files:
`Translmod.transl_store_*` stores each item into `(global M!)` and
Lambda.substs later references to `field_imm pos (global M!)`; unexported
idents take positions after exported ones; submodule fields are stored flat.
A post-pass over bytecode-form Lambda cannot reproduce it (needs item
boundaries, the coercion map, more_idents) -- the store form belongs in
lambda.cpp's structure translator, plus ~6 `native_code` switches
(matching.ml make_is_nonzero / transl_match_on_option / lazy force,
translcore:765 tuple flattening, translobj method cache, translprim frame
pointers, max_arity 126).  Oracle: amd64, Closure mode (no flambda), `as`.
Insulated alternative if wanted before that: compile bytecode-form Lambda
natively and validate by native exec parity, dump parity later.

### 3g. Toolchain-dependent output -- FIXED on branch `burn/toolchain-determinism`
(two commits on top of burn/testsuite: the conv_cmi_ty fix, and the
`cxx/harness/toolchain_determinism.sh` gate, clang vs gcc 285/285 identical.
The record of the finding follows.)
c++caml builds cleanly with **system g++ 11.4 / cmake 3.22 / ninja, outside
nix, no mimalloc** (2 min; the only edits needed are
`cmake_minimum_required(VERSION 3.22)` and the mimalloc default) -- and the
resulting compiler is deterministic run-to-run and Debug==Release, but
**differs from the clang-20 build on 109/139 compiler .cmo and 75/146 .cmi**.
Root: 12 .cmi differ in content (misc, identifiable, types, numbers, symtable,
btype, shape, ident, includemod, consistbl, path, arg_helper; the other 63
only inherit digests).  The content difference is that a FUNCTOR
APPLICATION's result signature loses the functor body's written type-variable
names under gcc: `module M = Map.Make(String)` writes
`val fold : (key -> 'a -> 'b -> 'b) -> ..` where clang (and ocamlc) write
`'acc`.  Own-.mli names and re-exports (`let g = List.fold_left`) survive on
both; only the functor-instantiation path loses them, so the fault is where
the instance copies the body's vars (infer.cpp:281 copies var_hint;
infer_check.cpp bridge_ty_rec consults ctx.var_names / rigid_name /
var_hint).  Same source, two toolchains, two outputs = unspecified behaviour
(gcc evaluates call arguments right-to-left, clang left-to-right, is the
classic).  gcc -Wall shows nothing relevant.  The clang build is the one the
gates validate, so the gcc build is the wrong one.  Reproduce: build
cxx/ with -DCMAKE_CXX_COMPILER=g++ -DCPPCAML_MIMALLOC=OFF, compile the
one-liner above with both, `cmiprint` both .cmi.  Until fixed, any package
must pin clang or gate the gcc build against the DDC corpus.

## 4. Items burn can take (driver / link / cmo -- files juice has not touched)
- `.c` inputs (47 tests): ocamlc hands `.c` files to the C compiler.
- `-i` (8), `-for-pack` (6), `-open` (3), `-pp`, `-depend`, `-thread`,
  `-output-complete-exe`; `-H` done properly (hidden dirs satisfy
  dependencies only -- the `-I` approximation makes 6 `hidden_includes`
  sub-tests wrongly pass).
- `-g`: debug events in the .cmo (26 tests).
- Dynlink of a c++ocamlc-built `.cmo`/`.cma` dies "Out of memory"
  (`lib-dynlink-*`, 11 tests): a cmo/cma format field Dynlink reads.
- `ocamlobjinfo` on our artifacts (`shape-index/*`, `uid-deps`, `uids`: 14):
  shapes/uids in the .cmi/.cmo.
- Parse-error message wording (19 `parse-errors/*` + `parsing/*`).
- `badly-ordered-deps/main.ml` LINK_ERROR; `tool-ocamldep-modalias`.

## 5. Harness debts noticed along the way
- `exec_parity.sh` ignores the TEST block (`max_domains1.ml` sets
  `ocamlrunparam d=1`; without it the test is a race the oracle itself loses
  1 in 6) and freezes one sample of a nondeterministic oracle run forever.
- Oracle-side timeouts shrink the denominator without a word.
- `ddc.sh`: `cmx_format.mli` fails symmetrically and simply drops out of the
  139 -- should be a declared exclusion.
- `stdlib_ddc.sh` needs `S2DIR=` copied by hand from `ddc.sh`'s output.
- Pinned discussion: rewriting the result-bearing harnesses (bootstrap, ddc,
  stdlib_ddc, effid) in OCaml with an explicit corpus manifest, failure =
  error not skip, and a machine-readable report -- keeping the final byte
  comparison dumb and independent (`cmp`), since a trusting-trust argument
  should not rest on a comparator only the suspect toolchain can run.
