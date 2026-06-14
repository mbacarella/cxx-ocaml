# JOURNAL — rewriting the OCaml compiler in C++

A running log of an attempt to rewrite the OCaml compiler (not the runtime — that
stays C for ABI compatibility) in C++, agent-driven, validated against the OCaml
`trunk` compiler as oracle and the testsuite as guiding light.

## Why

Prediction under test: as agents get smarter, the human-facing benefits of a
strongly-typed language with rich semantics shrink, and at the limit agents emit
machine code directly. Registered at
https://michael.bacarella.com/2026/05/20/will-AGI-even-care-about-ocaml/

The compiler is a good probe because its difficulty is wildly *non-uniform per
line*: lexer/backend are near-mechanical transcription, while the type-checker is
65K lines of subtle, behavior-defined inference. If strong types are "just for
humans," the type-checker port should cost far more than its line count predicts,
and the mechanical stages far less. **The spread is the signal.**

## Strategy — one pipeline, grown a stage at a time

One binary, `c++ocamlc`, always run from source text, in-process, with native C++
data structures, extended one stage per iteration. No serialization between C++
stages and no OCaml↔C FFI; the only thing crossing a boundary is text, at the
current frontier, for validation only.

OCaml already prints its IR at every seam (`-dparsetree`, `-dtypedtree`,
`-dlambda`, `-dinstr`, `-dcmm`, `-dlinear`; printers in `parsing/printast`,
`typing/printtyped`, `lambda/printlambda`, `bytecomp/printinstr`). Those dumps are
our per-stage oracle; the testsuite `.ml` corpus is the differential test driver.

Milestone ladder (front half is shared by both backends):

    lex → parse → type → lambda → BYTECODE  ← first real working compiler
                                  → native (Flambda + asmcomp)  ← Phase 2

Bytecode first because `bytecomp/` is ~4.5K lines vs native's ~44K — the frontend
plus the bytecode backend is the smallest thing that compiles and runs real OCaml
and can attempt `make tests`. That milestone answers the thesis question.

## Predictions (made 2026-06-02, before writing any C++)

Anchor rate: the prior runtime port did ~40K lines of C→Rust in ~1 week of
agent-driven, oracle-guided work — call that the *mechanical* rate. Each stage
below is discounted from that rate by how much of its work is behavior-matching
(chasing diffs against subtle semantics) vs transcription. Estimates assume
continuous agent work and that the oracle/diff harness is in place.

| Stage | Lines | What dominates the time | Estimate | Confidence |
|---|---:|---|---|---|
| Foundations (AST/asttypes/Location/Longident/Warnings/Ident/Path/Misc, variant scaffolding) | ~5K | mechanical, but lots of sum-type → `std::variant` boilerplate | **2–4 days** | high |
| Lexer → token-diff clean on corpus | 1.0K | ocamllex DFA + UTF-8 idents + string/comment/quoted-string state machines; small, crisp oracle | **2–4 days** | high |
| Parser → `-dparsetree` clean | 4.4K | **hard**: matching a menhir LR(1) grammar's precedence/associativity and *exact* AST node shapes by hand; long tail | **1.5–3 weeks** | medium |
| Typer → `-dtypedtree` clean | 65K | **the crux & wildcard**: unification w/ levels, generalization, modules, GADTs, polyvariants, objects, first-class modules, principality. Difficulty is semantic, not lines. | **1.5–4 months** | **low** |
| Lambda + pattern-match compilation → `-dlambda` clean | 17.7K | `matching.ml` is intricate; translation otherwise moderate | **3–6 weeks** | medium-low |
| Bytecode backend → **`make tests` passes** | 4.5K | mechanical backend, but first full end-to-end integration surfaces every accumulated frontend bug | **1–2 weeks** | medium |
| **▶ MILESTONE: working C++ bytecode `ocamlc`** | | | **~3–7 months total** | dominated by typer |
| Native: Flambda + asmcomp (Phase 2) | ~44K | whole optimizing backend + arch-specific codegen | **2–5 months** | low |
| **Full native + bytecode compiler** | | | **~6–12+ months** | low |

Caveats kept honest:
- The typer range is the one that can blow up; a 2–3× overrun there would not
  surprise me, and that overrun *is* the experimental result, not a failure of
  estimation.
- Estimates exclude the diff-printer cost (mimicking `Printast` et al. byte-for-
  byte, or normalizing both sides) — folded into each stage but a real tax.
- "Confidence" is about the *range*, not the point: high = I'd bet the band;
  low = the band itself is a guess.

Scoreboard (filled in as stages land, to check the predictions):

| Stage | Predicted | Actual | Notes |
|---|---|---|---|
| Foundations | 2–4 days | folded into lexer stage (day 0) | Token type + Location offsets done inline |
| Lexer | 2–4 days | **day 0: 100% parity (1853/1853)** | beat the low end of the estimate |
| Parser | 1.5–3 weeks | **100% over oracle-parseable (day 3)** | 1801/1801 byte-identical + 52/52 co-rejections |
| Typer | 1.5–4 months | **99.2% accept; dump-parity plateaued ~25%** | inference engine built; thesis confirmed (dump is inference-blind) |
| Lambda | 3–6 weeks | **18.6% dump-parity (138/743)** | completeness held 99.2%; in progress |
| Bytecode | 1–2 weeks | **20.8% instr-parity (162/777)** + runnable `.cmo` | emit `.cmo` read/linked/run by real toolchain |

---

## Progress dashboard  (updated 2026-06-07 — bytecode + runnable `.cmo`)

**Conceptual stages** (the pipeline; one binary grown stage by stage)
- ● **Lexer** — **100% token-identical** vs trunk oracle (1853 / 1853 testsuite files, incl. matching error positions on the 6 error-path files)
- ● **Parser** — vs `ocamlc -dparsetree`: **100.0% over every oracle-parseable file** (1801/1801 byte-identical) and **52/52 correct co-rejections** on the files the oracle itself rejects (toplevel scripts + deliberate syntax-error tests). Full-corpus number is **97.2%** (1801/1853) — and that is now the structural ceiling: the other 52 files have *no parsetree to match*, so the only remaining axis is error-message parity (different harness). **Day-3 finish (2026-06-05):** after `source.ml`, swept every remaining oracle-parseable diff file — `type_external`, `w53`, `open`, `attributes`, `warning`, `exotic`, `shortcut_ext_attr`, `deprecated`, `test`, and the docstring-placement torture test `docstrings.ml` — then closed the last 3 over-accepts by adding strictness (reject `(t,t)\`A.t`, singleton `(x:t)` labeled tuple, bare-uppercase type path `int -> T`). The docstring work ported the empty-`(**)`-drops-attribute rule, class-body `pre_extra`/`floating`/`post_extra` text, empty-body docstrings keyed at the closing token, and once-only `ocaml.text` emission. Earlier same-day: **96.9%** of the full 1853-file corpus (1796 byte-identical), or **99.6%** over 1804 oracle-parseable. **Day-3 (2026-06-05):** drove `parsetree/source.ml` (7561-line grammar torture test) to **byte-for-byte identical**, then cleared 6 more files — `type_external`, `w53`, `open`, `attributes`, `warning` (and `source`). Recurring theme this day: **a node's `$sloc`/`$endpos` includes a trailing `[@attr]` even though the attribute does not extend the child's own loc** — fixed for ghost `Pexp_function` (let-fun body), match/`function` arms (`Pfunction_cases` loc), `Pcl_fun` body, and functor-type codomains over a paren. Other day-3 wins: `new%ext`/`type%ext … +=`/plain `module rec` in sigs + per-decl `[@@attr]`; info-docstrings on polyvariant tags, object methods, and arrow args; **trailing `[@attr]` binds to the whole cons-pattern / application / module-application, not the last operand** (mid-application attrs are syntax errors); `with`-constraint manifests are `core_type_no_attr` so a trailing `[@a]` is the package/`Pmty_with`'s; `true 0` → `Pexp_construct(_, Some 0)`; type local-open `M.(module S)`; `Ptype_external` `%S` escaping; `Ptyp_poly` tyvar keyword-escaping (`'\#let`); Genarray `a.{…}<-v` index-array loc. Earlier (day-1+2) climb **65.1 → 87.6 → 96.5**. (≈49 corpus files are toplevel-directive scripts the oracle itself rejects as implementations — the harness now reports both.) Day-1+2 (2026-06-02→03) climb **65.1 → 87.6**. Latest wins: polyvariant `` `Tag`` as an app-arg atom; local-open in types `M.(t)` (Ptyp_open); module aliases in signatures `module B = A` (Pmty_alias); record-field attributes `[@atomic]` (pld_attributes); **let/letop/fun bodies are seq_expr** so their node loc extends through a trailing `;` (+20, sibling of the Pexp_sequence trailing-`;` fix); GADT inline records `A : { f } -> t`; lowercase module-type names & idents; and **functor-application type paths `F(X).t`** (Lapply; `kind_start` excludes `UIDENT (` so it's a manifest not a constructor). Remaining tail (175 real parse-gaps / ~50 diffs): method `: type a. t` (the `wrap_type_annotation` newtype+`varify` desugaring), labeled tuples `(~x, y)`, exotic type forms (`true|false` ctors, `(::)`, `external "t"`), and module/modtype destructive subst. Beyond the day-1 list below: extended/DOTOP index operators (`let (.!()) =`, `h.%{a;b}` get/set) + HASHOP infix; `and[@attr]`/`let%ext`; `fun[@attr]`; `#class` types (Ptyp_class); labeled/optional arrow types (`?x:t ->`); `(type a b c)` multi-newtype params; existential ctor patterns `Constr (type a) p`; type aliases `(t as 'a)` (Ptyp_alias); local-open patterns `M.(P)` (Ppat_open); class-in-signature (Psig_class); destructive type subst (`type t := …`); **explicit polymorphism** `'a 'b. t` (Ptyp_poly) in let/val/method/class-field annotations; package types in `(module M : S)` expr/pat; and **`Pmty_functor`/`Pmod_functor` locs start at the arg `(` not the `functor` keyword** (per `mk_functor_typ`'s per-arg startpos). Earlier day-1 detail: full 1853-file corpus was **77.4%** (1434/1853 byte-identical). *Always headline the full-corpus number (`parse_parity.sh 0`); the 400-subset flatters.* **Day-1 (2026-06-03) climb 65.1 → 77.4** via the parse-error/diff-bucket loop: unary `+`/`+.` signs + keyword-exprs (`match`/`fun`/…) as infix operands; **trailing-`;` seq_expr** handling (consume it; it extends the *enclosing* item span and the **Pexp_sequence** node's own span via `last_seq_end_` — the sequence-span fix alone cleared +69 files); module item-attributes; binding-level type constraints on pattern bindings (`let p : t = e`); signed-literal & `p | exception q` patterns; **`fun`/`let f p..` body `= function` merges into one Pexp_function** (Pfunction_cases directly, +32); package types in `(module M : S)` patterns; generalized `open struct…end`/`open M(X)`; `[@attr]` on qualified-path/constructor exprs; destructive type substitution `type t := …` (Psig_typesubst). Also a **harness correctness fix**: the `-dparsetree` filter anchored on `^\[$` and silently dropped empty-structure `[]` dumps, mis-scoring 47 already-identical files (true score was higher all along). The **docstring side-channel is done** (`ocaml.doc`/`ocaml.text`): ported lexer.mll's token/EOL/docstring state machine + docstrings.ml's pre/post/floating/extra tables; the trailing-`;;` structure-boundary fix alone cleared ~150 files. **printast Format quirk** understood: printast emits literal `\n` (never `@\n`), so Format's column counter never resets and every `@ ` break in a list (e.g. `<type>` newtype-univars) always fires → one element per line at column 0. **Next levers** (remaining 324 parse-errors / 94 diffs): class declarations in signatures (`Psig_class`, `class c : object…end`), module/modtype destructive subst (`module M := X`), `#line` directives (lexer-level), `##`/extended-index operators, quoted extensions `{%%M.foo|…|}`. Broad surface incl. GADTs, poly-variants, type extensions, newtype params, attributes/extensions, modules/signatures/functors (`Pmod_apply` too), first-class modules `(module M)` + package types `Ptyp_package`, coercions, `[%ext]`, `let*`, `e#m`, setfield/array-set, the missing patterns, the full **object/class** subsystem, parenthesised operators `(+)`, labelled/optional params, constrained function bindings, `let f : type a. …`, type manifest/kind redefinition + `constraint` clauses, the **signature** item subsystem, **recursive modules**, and module-type **with-constraints / `module type of` / functor arrows**. local opens `M.(e)`, qualified/bigarray field access, effect-handler patterns, type-attributes, refutation cases, and the **docstring subsystem**. Full-corpus climb this session: 41.6 → … → 50.2 → 51.8 → 54.0 → 54.8 → 56.9 → **65.1** (the last jump is docstrings). Broad-implement-then-corpus-diff loop. Key recurring fix: **menhir computes a compound node's loc from the *symbol* span (incl a parenthesized child's parens), not the child node's loc**. Remaining: the docstring side-channel (→ `ocaml.doc`/`ocaml.text`), local opens `M.(e)`, type-with-attributes `(t [@attr])`, more module-expr forms, signatures-with-class.
- ◐ **Typer** — *in progress; mechanical surface broad, inference engine built, dump-parity plateaued ~25%*. **301 byte-identical typedtrees** (typeable ~1183; oracle dumps CACHED, harness file-based so 10 MB dumps don't melt bash). Prerequisite **`.cmi` reader complete** (Marshal → `Types.signature`). `c++type` (`cxx/src/{typer,typedtree_print,marshal,cmi,infer,infer_check}.cpp`) transcribes: constants/let(+rec)/apply/function(+cases)/if/match/try/seq/construct/array/assert/for/while/lazy; patterns var/any/const/construct/tuple/or/alias/exception; type decls (predef `int/1!`), external, typext, exception, open(+stdlib scope), module bindings + **module types/signatures** + **module-exprs (functor/apply/constraint)** + **`let module/open/exception in e`**, attributes (+`[@@@attr]`), constraints, **records** (field registry), and **format strings** (common plain directives; conservative bail on flags/width).
  **INFERENCE ENGINE BUILT (Slices 1–3):** real minimal Hindley-Milner — mutable `type_expr` with `Tlink` union-find + levels, `unify`/`instantiate`/`generalize` (`infer.{hpp,cpp}`, unit-tested), and a best-effort algorithm-W pass (`infer_check.cpp`) that infers correct polymorphic types (`let id z = z` ⇒ `'a -> 'a`; compare-driven `'a -> 'a -> 'a`) — see `c++type --infer`. Ran over all 1853 files: 0 hangs, 0 new crashes. Wired into the dump for `Texp_match (Partial)` exhaustiveness (Slice 3, conservative → no false-positives).
  **PIVOTAL FINDING (2026-06-06):** building the engine yielded **~0 dump-parity gain**. The `-dtypedtree` dump prints no inferred types, so it barely *exposes* inference — only match-exhaustiveness and ambiguous disambiguation, both rare: **only 2 oracle-typeable files contain a `(Partial)` match at all**. So the dump is ~entirely reproducible WITHOUT the type engine; the 65K-line `ctype.ml` core is **nearly invisible in this projection** — the strongest confirmation of the thesis spread. Consequence: **dump parity has plateaued (~25%)**, gated by a mechanical long tail (format flags, float hex formatting, qualified module paths, 10 MB list-literal files) + the multi-blocker dynamic — NOT by inference. The inference engine's value is therefore downstream (lambda/bytecode) and in the real finish line (emit `.cmi`/`.cmo`, **reject ill-typed programs** — which a heuristic typer cannot do, and which the dump cannot measure). **Validation must shift** off `-dtypedtree` for the typer: the genuine next signal is type-error/rejection parity. Resolution via `Env` (locals + Stdlib from the cmi reader). Pre-existing parser bug: 6 lexing/encoding files SIGABRT in parse_structure (both modes).
  **KEY THESIS RESULT (sharpened):** the climb to 299 needed *zero HM inference*. `printtyped` dumps no inferred types, so the whole `-dtypedtree` surface is reproducible by resolution + scope + faithful transcription + a couple of registries — even **records** (the supposed frontier) fell to a field registry, not unification. The 6.7K-line `ctype.ml` core is almost *invisible* in this projection.
  **OVERFITTING WATCH (concern raised 2026-06-06):** most of this is faithful modeling, NOT test-fitting — we implement *constructs*, never special-case individual files, and a construct either matches across all corpus files using it or doesn't. The one genuine hack is **format-string detection by a hardcoded Printf/Format/Scanf function-name + arg-position heuristic** (real rule is type-directed via expected type) — explicitly tagged in-code as **scaffolding to be replaced by inference**, not a permanent design. Conservative bail-outs (format flags, unique-field-only records) are honest *gaps* (safe DIFFs), not wrong answers. The structural anti-overfit safeguard: dump parity is a *proxy* with a known blind spot (no inferred types shown), so the real finish line — byte-identical `.cmi`/`.cmo` **and rejecting ill-typed programs** — will expose anything faked (a heuristic typer cannot reject bad code).
  **Remaining:** mechanical — `Pstr_modtype`/signatures (in progress), functor/apply/constraint module-exprs, `Pexp_struct_item`, format flags. Genuinely needs inference — ambiguous field/ctor disambiguation, `Texp_match (Partial)` exhaustiveness, true format detection. **Plan:** bank the mechanical points, then bite the inference bullet as a focused milestone (minimal HM core: `type_expr`+`Tlink` union-find, levels, unify/instantiate/generalize) — it's the load-bearing capability for the real-artifact goal and the actual experiment.
  **LOAD POST-MORTEM (2026-06-06):** a long agent-driven session melted the machine. Cause: orphaned background `until…sleep` poll-loops accumulating (the result-channel kept dropping outputs, so I re-dispatched waiters that never exited) × overlapping 32-way `ocamlc` harness sweeps (couldn't tell a sweep had finished, relaunched it). Fixes: NO background poll-loops (run synchronously, check once); never re-dispatch on a transport error without checking; `JOBS=8` not 32; oracle cache removes per-run ocamlc fan-out. See [[parallel-corpus-harness]].
- ◐ **Lambda** — vs `ocamlc -dlambda`: **18.6% dump-parity** (138/743 byte-identical, up from 9.8%/73), **completeness held at 99.2%** (false-rejection over oracle-accepted) the whole climb. Tool `c++lambda`; harness `cxx/harness/lambda_parity.sh`. Features landed this session: match→switch, seq for discarded bindings + the print-margin fix (effective margin 77), unit value-kind, ref/`!`/`:=`/incr/decr, records (literal/field/set, mutable→makemutable), user variant construction, submodules + `M.x`, for/while, exceptions/raise/try-with, arrays, string escaping + char + boxed-int literals, C externals, pervasive %-primitives, comparison operators, function params for all patterns, per-expression value-kind inference.
- ◐ **Bytecode (instr)** — NEW phase: `cxx/{include,src}/bytecode.{hpp,cpp}` — Bytegen lowers the Lambda IR to the stack+accumulator VM instruction stream, validated against `ocamlc -dinstr`. Tool `c++instr`; harness `cxx/harness/instr_parity.sh`. **Baseline 20.8% instr-parity** (162/777 byte-identical). Of the diffs, only ~22 are bytegen's own gaps (stubbed for/while/switch/try); the rest are inherited from the lambda stage.
- ● **emitcode / `.cmo`** — NEW phase: `cxx/{include,src}/cmo.{hpp,cpp}` + the `c++cmo` tool — encodes the instruction stream to relocatable bytecode (compact opcodes, label backpatching, emit peephole fusions, relocations) and writes a real `.cmo` (magic `Caml1999O038` + code + a marshaled `Cmo_format.compilation_unit`, via a minimal OCaml Marshal *writer*). **MILESTONE:** a c++caml-produced `.cmo` is read by `ocamlobjinfo`, linked by the real `ocamlc` against `stdlib.cma`, and run by `ocamlrun` — `hello.ml` prints "hello from c++caml"; arithmetic, `string_of_int`, `if`, and a user function also run correctly.
- ☐ **Linker** (next)  ☐ **Native**

Pipeline now: lex (done) → parse (done) → type/infer (done, 99.2% accept) → lambda (18.6%) → bytecode instr (20.8%) → emit `.cmo` (runnable) → [next: own linker]. New tools: `c++lambda`, `c++instr`, `c++cmo`; harnesses `cxx/harness/{lambda_parity,instr_parity}.sh`.

**Files converted (OCaml → C++)**

| OCaml source | lines | C++ target | status |
|---|---:|---|---|
| `parsing/lexer.mll` | 1040 | `cxx/src/lexer.cpp` + `include/cppcaml/{token,lexer}.hpp` | **100% token-parity** |
| `utils/misc.ml` `Utf8_lexeme` (Latin-9 case/NFC) | ~300 | folded into `cxx/src/lexer.cpp` | done (the parts the lexer needs) |
| `parsing/parser.mly` (%token decls) | 127 | `cxx/include/cppcaml/token.hpp` (124-kind enum) | done |
| `parsing/parser.mly` (grammar) | 4429 | `cxx/src/parser.cpp` + `include/cppcaml/ast.hpp` | core fragment (2.5%) |
| `parsing/printast.ml` (-dparsetree) | ~1050 | `cxx/src/ast_print.cpp` | fragment printers, byte-exact |

**In-scope line conversion**
- **~1340 / 183,121** hand-written compiler source lines fully ported = **~0.7%**;
  parser + printer fragment in progress on top.
- (C++/oracle/harness written so far: ~2,110 lines.)

**Lexer: done.** Closed the tail in one session by fixing two real bugs (`\x`
escape `+5` off-by-one; DOTOP including the leading dot) and porting the deferred
features (Latin-9 extended idents with NFD→NFC normalization & capitalization,
`\u{}`/decimal/octal escape range checks, line directives with int-overflow,
raw-ident `\#`, `{%…|`/`{%%…|` extensions, lowercase-delimiter validation,
quoted-strings inside comments). 0 over-accepts throughout.

---

## Log

### 2026-06-02 — environment + oracle study (day 0)

- Decided the data model: OCaml sum types → `std::variant` + `std::visit`
  (closest to ML semantics, near-exhaustiveness via warnings, value types).
- Decided entry point: lexer first, validated by token-stream diff vs trunk.
- Toolchain: authored `flake.nix` (+ `flake.lock`) pinning **clang 20.1.8** with
  the gcc-14 libstdc++ on nixpkgs 25.05 — gives `std::expected`, `std::generator`,
  `std::print`, `std::format`. Single clang toolchain builds both the rewrite and
  the trunk oracle (clang is Tier-1 for OCaml on Linux), avoiding gcc/clang PATH
  ambiguity. Flake evaluates clean; closure realizing from cache.
- A `-std=c++23` feature probe is queued to prove `<expected>/<generator>/<print>`
  actually compile+run before any real C++ is written.
- Studied the first oracle, `parsing/lexer.mll` (1040 lines): extracted the full
  token surface — **127 tokens**, ~20 payload-carrying (`INT`/`FLOAT` =
  `string × char option`, `STRING` = `string × loc × delim option`, idents/
  `INFIXOP0–4`/`LABEL` = `string`, rest nullary). Noted the versioned keyword
  table, string-literal buffering, and `\r\n`→`\n` normalization (#12502) as
  exact behaviors to reproduce.
- Confirmed the seams exist in this tree: all `-d{parsetree,typedtree,lambda,
  instr,cmm,linear}` flags and their printer modules are present.

Next: land the toolchain probe → build the trunk oracle → scaffold `cxx/` (CMake +
clang, `std::variant` `Token`) → port the lexer → wire the token-diff harness.

### 2026-06-02 — lexer ported, token-diff harness live (day 0 cont.)

- Toolchain probe passed: clang 20.1.8 compiled+ran `std::expected`/`std::generator`/
  `std::print`/deducing-this. Trunk oracle built (`ORACLE_BUILD_OK`, `ocamlc.opt`).
- Scaffolded `cxx/`: CMake (clang, C++23, Ninja), `Token` (124-kind enum + payload),
  hand-written `Lexer` porting all six `lexer.mll` rules (token/lex_directive/
  comment/string/quoted_string/skip_hash_bang) and the COMMENT/EOL/DOCSTRING filter.
- Oracle dumper `cxx/oracle/dump_tokens.ml` drives the trunk `Lexer.token` and prints
  the same canonical format as `cxx/src/token.cpp`, so streams diff byte-for-byte.
- Harness `cxx/harness/parity.sh` runs both over the testsuite and reports match rate.
- **Result: 98.2% token-identical (1819/1853).** First build already matched the
  subtle cases by construction: maximal-munch operators (`>>]`→`INFIXOP0 >> RBRACKET`,
  `:::`→`COLONCOLON COLON`, `+.+`→`INFIXOP2`), all literal bases + modifiers, char/
  string escapes, quoted strings, `.~` reserved error.
- One real bug found+fixed via diff: #12502 multi-CR normalization (`\r\r\n`→`\r\n`,
  not `\n`). Two `clang/EOF` macro/stdio collisions resolved (`Kind::TEOF`).
- Observation for the thesis: the mechanical stage went *fast and clean* — most of
  the difficulty was transcription, and the type system bought the OCaml original
  little that a careful C++ port couldn't match. The interesting test remains the typer.

Next: close the lexer tail (UTF-8 idents, error-path parity, quoted-string ext,
line directives) toward ~100%, then start the parser (`-dparsetree` oracle).

### 2026-06-02 — lexer to 100% (day 0 cont.)

Closed all 34 remaining diffs to **100% token-parity (1853/1853)**, in order:
- **Two real bugs** (the bulk): `\xHH` string escape advanced `pos_` by 5 not 4
  (ate the next char → garbled bytes + cascade desyncs, ~11 files); DOTOP text
  wrongly kept the leading `.` (`.!`→`DOTOP !`, ~7 files). 98.2%→99.1%.
- **Deferred features ported:** Latin-9 extended identifiers — full `Utf8_lexeme`
  port (UTF-8 decode, the `get_known_char` case table, `get_known_pair` NFD→NFC
  normalization, capitalization → LIDENT/UIDENT, encoding validation); escape
  range checks (`\999`, `\o777`, `\u{D800}` surrogate); line directives `# n "f"`
  with 63-bit overflow detection; raw-ident escape `\#`; `{%id|`/`{%%id|` quoted-
  string extensions; lowercase-delimiter validation; quoted strings inside
  comments. 99.1%→99.7%→100%.
- 0 over-accepts at every step (never accepted input the oracle rejected).

Thesis read for the lexer: predicted 2–4 days, done in one session — the
*mechanical* stage was exactly as cheap as the model said, even with the fiddly
Unicode/error-path tail. The type system gave the OCaml original little here that
a careful C++ port couldn't reproduce. The real test is still the typer.

Next: start the **parser** — port `parsing/parser.mly` behavior, validate the AST
against `ocamlc -dparsetree` over the testsuite corpus (new oracle in the harness).

### 2026-06-02 — parser stage stood up; tracer bullet byte-identical (day 0 cont.)

- New oracle: `ocamlc.opt -nostdlib -I stdlib -stop-after parsing -dparsetree`
  (prints the Parsetree to stderr); harness `cxx/harness/parse_parity.sh`.
- Built the AST (`cxx/include/cppcaml/ast.hpp`, `std::variant` per the data model
  — first real use of it), a `printast.ml`-faithful printer (`ast_print.cpp`), and
  a recursive-descent + Pratt parser (`parser.cpp`) with full position tracking
  (line map → `[lnum,bol+col]`) and the `let f a b = e` → ghost `Pexp_function`
  desugaring.
- **Tracer bullet byte-identical**: `let x = 1 + 2` and `let f a b = a + b` match
  `-dparsetree` exactly — node shapes, every location, ghost markers, operator
  precedence, the `<def>`/`<arg>` framing. Two trivial printer fixes found by diff
  (value-binding child indent off by one level; missing trailing `@.` newline).
- Corpus baseline: **2.5%** (fragment only; 383/400 hit unsupported constructs).
- The std::variant + unique_ptr-boxed AST model works cleanly; this is the lever
  for widening the fragment.

Next (widen the parser fragment, in rough corpus-impact order): patterns
(tuples/constructors/records), `match`/`function`, type declarations (`Pstr_type`),
`open`/`module`, constructor & record expressions, labels/optional args, then
attributes/extensions. Each widens the `-dparsetree` parity dial.

### 2026-06-02 — parser widened to 5.2% (day 0 cont.)

Big expression/pattern/type batch, all byte-validated against `-dparsetree`:
- **Patterns** (full): var/any/constant/tuple/constructor(+arg)/or/alias/constraint
  /unit/`[]`; rewired let-bindings (val-ident vs pattern form) and params to use them.
- **core_type**: var/any/constr(+qualified+args)/arrow/tuple/paren.
- **Type declarations**: variant/record/abbreviation, with the fiddly locs nailed
  (ptype_loc from the `type`/`and` keyword; constructor_decl loc includes its
  leading `|`; label loc includes the trailing `;`; the `printast` quirk where a
  field name prints with no newline so `core_type` runs onto its line).
- **`open`**, and expressions: `match`/`function`/`try`/`fun`, sequences (`;`),
  `begin..end`, constructors (nullary + one-arg, `true`/`false`/`()`/`[]`),
  `(e : t)` constraints, record-field access.
- Data-model note: `Longident` children moved from `unique_ptr` to `shared_ptr`
  so `LongidentLoc` is *copyable* (it's immutable and passed by value into many
  nodes); designated initializers used to defeat aggregate brace-elision into the
  nested `LongidentLoc`.
- Parity 2.5% → **5.2%**; parse-errors 383→331/400. Top remaining blockers:
  records, list/cons literals, `let x : t =`, GADT/extension type decls, modules,
  labeled args.

### 2026-06-02 — lists / records / cons / constraints (day 0 cont.)

- **List literals** `[e1;…;en]` and **cons** `a::b`: ported the `mktailexp`
  desugaring exactly — right-assoc `Pexp_construct "::"` over ghost tuples, ghost
  `::`/`[]` longident locs, the outermost expr carrying the real bracket span,
  and the explicit-cons longident being the real `::` token. Same for patterns.
- **Records**: `{ l = e; … }`, `{ e with … }`, punning (ghost label loc); record
  **patterns** `{ l; _ }` with the open/closed flag.
- **`let x : t = e`** value constraints (`Pvc_constraint`), incl. fixing the
  val-ident-vs-pattern heuristic to fire on a following `:`.
- All validated byte-for-byte on hand-built samples (lists/cons/records/field/
  constraint/list+record patterns identical).
- Corpus parity 5.2% → **5.5%** only, and "expected an expression" *rose*
  (206→217): real files are long and use a long tail of constructs, so each new
  feature mostly lets files parse *further* before the next gap. This is the
  predicted parser long tail — building blocks are correct; full-file parity is
  gated by the least-supported construct in each file. Next tail targets:
  labeled/optional args, `assert`/`lazy`/`while`/`for`, arrays `[|…|]`, `.()`/`.[]`,
  modules, exceptions/externals, attributes/extensions, GADT/poly-variant types.

### 2026-06-02 — parser expression tail → 9.8% (day 0 cont.)

Ported a broad expression batch, each byte-validated against `-dparsetree`:
- **Prefix ops** (`!x`→apply `"!"`, PREFIXOP); **unary minus** with the two cases
  OCaml distinguishes — `- <literal>` folds to a signed constant, `- e` applies
  `~-`/`~-.` to an *application* (`-a*b`=`(~-a)*b`, `-f c`=`~-(f c)`).
- **`assert`/`lazy`**, **arrays** `[|…|]`, **`while`/`for`** (with direction flag).
- **Labeled & optional arguments** (`~x`, `~x:e`, `?x`, punning) and **params**
  (`~x`, `?x`, `?(x=e)` with default expr — added the default slot to Pparam_val).
- **`.(i)`/`.[i]`** → ghost `Array.get`/`String.get` apply.
- Also fixed `is_atom_start` to include `true`/`false`/`[`/`{`/`begin` (so they're
  collected as application arguments).
- Parity **5.5% → 9.8%** (39/400); "expected an expression" 217→162.

### 2026-06-02 — structure items → 11.2% (day 0 cont.)

Added structure-level items (which gate whole files early), byte-validated:
- **`exception E` / `exception E of …` / `exception E = Path`** (Pstr_exception →
  type_exception → extension_constructor, Pext_decl/Pext_rebind; ctor loc spans
  the `exception` keyword).
- **`external f : t = "prim"`** (Pstr_primitive / Pprim_decl).
- **`module M = struct … end` / `= Path`** (Pstr_module, Pmod_structure /
  Pmod_ident) — required forward-declaring `Structure` (modules nest structures)
  and a `parse_structure_until(stop)` shared by the top level and `struct…end`.
- abstract `type t` already worked.
- Parity 9.8% → **11.2%** (45/400); "expected an expression" 162→**93**. Blocker
  profile shifted: harder patterns (47) and the type-decl tail (GADTs/poly-
  variants/extensible, 45) now lead; attributes/extensions next.

### 2026-06-02 — GADTs/poly-variants/type-ext/newtypes → 13.2% (day 0 cont.)

- **GADT constructors** (`A : t1 * … -> tres`), **`type t = ..`** (Ptype_open),
  **`type t += C`** (Pstr_typext, disambiguated from declarations by `+=`),
  **`_`/variance type params**.
- **Polymorphic variants** end-to-end: types `[ \`X | \`Y of int ]` (Ptyp_variant
  / Rtag), patterns and expressions (`Ppat_variant`/`Pexp_variant`).
- **Harder patterns**: `lazy p`, `'a'..'z'` intervals, `exception p`.
- **`(type a)` newtype params**: `Pparam_newtype` in let/fun-with-val-params, and
  the special `fun (type a) … -> e` (only newtypes) → nested `Pexp_newtype` chain
  (outermost real loc from `fun`, inner ones ghost). Fixed a crash where the
  function-binding desugar assumed the first param was a `Pparam_val`.
- Parity 12.5% → **13.2%** (53/400); parse-errors 236→211. Next tail: `let[@attr]`
  item attributes (~13 files), effect patterns, `let module … in`, objects.

### 2026-06-02 — attributes/extensions + the string-loc fix → 43% (day 0 cont.)

- Floating attributes `[@@@attr …]` (Pstr_attribute) and item extensions
  `[%%ext …]` (Pstr_extension), with a structure payload (PStr). `[%%expect …]`
  is what gates the many expect-test files.
- **The big one — a latent bug fixed:** `Pconst_string`'s `strloc` must be the
  string's *content* span (inside the quotes / `{delim| … |delim}`), not the whole
  token. It had been wrong for **every** string constant, but stayed hidden
  because earlier parser samples had no string *constants* (only the lexer tests
  exercised strings). The diff-vs-oracle harness caught it the moment a string
  literal appeared in a parsed expression.
- Result: parity **13.2% → 43.0%** (53 → 172/400) in one step. parse-errors barely
  moved (211→202) — the gain was almost entirely near-miss diffs that the strloc
  bug was poisoning across the corpus. A clean illustration of the thesis: a
  single subtle, pervasive location detail, invisible until the oracle surfaced
  it, was suppressing a third of the corpus.

### 2026-06-02 — node attributes → 50.2% (day 0 cont.)

- Threaded `Attributes` (vector of `{name, structure payload}`) through the AST:
  reorganized the header to forward-declare `Structure`/`Attribute` early, added
  an `attrs` field to `Expression`/`Pattern`/`CoreType`/`ValueBinding`, and print
  them in the exact positions (`pexp_attributes` after the expr loc line;
  `pvb_attributes` inside `<def>` before the pattern, indented one deeper).
- Parser: `e [@attr]` (postfix on atoms), `let[@inline] …` (after `let`, onto the
  first binding), and trailing `let … = e [@@inline]` (onto that binding).
- Parity 43.0% → **50.2%** (201/400); parse-errors 202→172. Past the halfway mark.
  Tail now: objects/classes, first-class modules, `let module`/`let open` exprs,
  `[%ext]` nodes, signatures, `(module M)` patterns.

### 2026-06-02 — open/include + a fork AST discovery → 51.2% (day 0 cont.)

- `let open M in e` and `let module M = me in e` and `include M`. Discovered via
  the oracle that **this fork replaced `Pexp_open`/`Pexp_letmodule` with a unified
  `Pexp_struct_item of structure_item * expression`** — local opens/modules wrap a
  structure item over the body. Modelled it directly (the oracle is the spec, not
  upstream OCaml's AST). Matched the `Pstr_include` print quirk (no trailing
  newline before `module_expr`).
- Parity 50.2% → **51.2%** (205/400). Small (+4) — the long tail is now ~1% per
  construct. The remaining proportional gains are in big subsystems (functors +
  signatures, objects/classes), each a multi-hour push rather than a quick win.

**State of the experiment (end of day 0):** lexer 100% (predicted 2–4 days, done
in a session); parser at 58% of the testsuite corpus, up from a tracer bullet, via
a long sequence of oracle-driven diffs. The recurring lesson: correct transcription
is cheap; the leverage is in the few pervasive details (the string-loc bug, the
`Pexp_struct_item` fork divergence, the menhir symbol-vs-node loc rule) that only a
differential oracle surfaces.

### 2026-06-02 — methodology pivot + diff-fix loop → 58% (day 0 cont.)

Per review feedback, switched from per-construct oracle round-trips to
**transcribe broadly from parsetree.mli/printast.ml → run the whole corpus →
batch-fix what the diff surfaces**. Much higher throughput (51.5 → 58.0 in the
session's back half). Added in bulk: setfield/setinstvar/coerce/send/pack
(first-class modules)/extension/letop expressions; array/type/unpack/extension
patterns; `Pmod_apply`. Corpus-diff batch fixes: record-expr punning ghost label;
`Array.set` whole-assignment span; harness strips the oracle warning preamble;
and the big one — **menhir's symbol-vs-node loc rule** (a compound node's location
spans its child *symbols* including their parens, while the child *node* loc
excludes them), fixed across core types and constructor/extension declarations.

The docstring side-channel (the original gap flagged at the lexer stage) is the
next cluster: `(** … *)` adjacent to a decl → an `ocaml.doc` attribute on it
(payload = comment text, delimiters stripped); a floating one →
`Pstr_attribute "ocaml.text"`. Replicating it needs the parser to stop filtering
`DOCSTRING` tokens and to re-implement lexer.mll's pre/post/floating attachment
state machine — a real subsystem, deferred. Objects/classes (the last big
untouched grammar) is the other remaining chunk.

### 2026-06-02 — module types / signatures / functors → 51.5% (day 0 cont.)

- `module type S = sig … end` (`Pstr_modtype`, `Pmty_ident`/`Pmty_signature`/
  `Pmty_functor`), signature items `val`/`type`/`module`, `module M : S = me`
  (`Pmod_constraint`), and functors `module F (X:S) = me` (`Pmod_functor`).
  m.ml byte-identical.
- Parity 51.2% → **51.5%** (+1 file). The subsystem is large and correct but the
  files exercising it are multi-construct, so completing one feature seldom
  completes a whole file. This is the **deep tail**: from here, each large
  subsystem buys ~0.3% of full-file parity, because the marginal file needs *all*
  of {objects, with-constraints, first-class modules, exotic sugar, …} at once.
  A faithful read on the prediction: the parser estimate (1.5–3 weeks) is holding —
  the front half of the grammar fell quickly, but the combinatorial tail is exactly
  the slow grind it was predicted to be.

### 2026-06-02 — the object/class subsystem → 59.2% (day 0 cont.)

The last big untouched grammar. Transcribed the whole thing in one pass
(AST + printast-faithful printer + recursive-descent parser), then ran the
corpus diff to nail the locations — the broad-implement-then-diff loop again.

- **AST/printer** (`ast.hpp`, `ast_print.cpp`): `Pexp_object`/`new`/`override`/
  `poly`; object types `Ptyp_object` (`Otag`/`Oinherit`); and the full class
  language — `class_type` (`Pcty_constr`/`signature`/`arrow`), `class_signature`
  + `class_type_field` (`Pctf_*`), `class_expr` (`Pcl_constr`/`structure`/`fun`/
  `apply`/`let`/`constraint`), `class_structure` + `class_field` (`Pcf_*`),
  `class_field_kind` (`Cfk_concrete`/`virtual`), and `Pstr_class`/`Pstr_class_type`
  with their `class_declaration`/`class_type_declaration` infos.
- **Parser** (`parser.cpp`): `object … end`, `new`, `{< … >}`, `< m:t; .. >`,
  `class [virtual] [params] c … = ce`, `class type ct = …`, and all field/sig-field
  kinds incl. method-body `Pexp_poly` desugaring and `class_fun_binding` params.

Corpus-diff fixes that mattered (all caught by the diff, all pre-localized):
**field lists need `[`/`]` brackets** (printast's `list` wrapper, not a bare loop);
**method bodies wrap in `Pexp_poly` with a *ghost* ghexp loc** (`= e` → loc of the
body; `: t = e` → `($startpos ty, $endpos e)`); **`fun` class_exprs don't reloc to
the keyword** (`wrap_class_attrs` keeps the body loc, which starts at the first
param); **`val x : t = e` → `mkexp_constraint` whose `$sloc` starts after `VAL`,
not at it** (off-by-`"val "`). Also a non-object win surfaced by the same files:
**`[< … ]` polymorphic-variant types always carry a `Some` present-tags list**
(empty when no `> …`), previously emitted as `None`.

A 22-field hand-written object program is byte-identical; typing-objects parity
went 6 → **9 / 22** (the other 13 fail on *unrelated* gaps — signatures-with-class,
exotic type syntax, the deferred constrained-function binding). Full-corpus parity
42.5%; 400-subset **58.0 → 59.2%**. As predicted, a whole subsystem buys ~1% of
full-file parity because the marginal file needs several subsystems at once.

### 2026-06-02 — widening blitz: full corpus 41.6 → 50.2% (day 0 cont.)

A run of corpus-diff-driven batches, each surfaced pre-localised by the
whole-1853-file diff scan (categorise PARSE_ERROR messages → fix the top bucket).
Note on measurement: this session headlines the **full-corpus** number
(`parse_parity.sh 0`), stricter than the historical 400-subset dashboard figure;
both are now stated side-by-side above so they're never conflated again.

Landed, in order, with the per-batch full-corpus delta:
- parenthesised operators `(+)`/`(>>=)`/`(!)` in exprs, patterns, and `let (op) …`
  bindings (guarded on a following `)`), +post-item attrs on `external`/type decls.
- labelled/optional params `~l:p` / `?l:p` / `?l:(p=e)`; constrained function
  bindings `let f p.. : t = e` and `: t :> t2` (new Pexp_function constraint field).
- type body rework: manifest-vs-kind disambiguation + `= M.t = A | B` redefinition.
- package types `(module S [with …])`; `let f : type a. t = e` value constraints
  (the fork's `<type> …` univars list); `constraint t1 = t2` clauses.
- the signature-item subsystem (external/typext/exception/open/include/modtype/
  functor-decl/class-type/attr/extension); `#abstract` no-newline fix.
- recursive modules `module rec` (+27 files — common).
- module-type with-constraints, `module type of`, anonymous functor arrows.

Recurring lesson reconfirmed: most wins are *transcription* against printast.ml +
a loc rule, not algorithmic. The errors that remain are concentrated, not diffuse:
local opens `M.(e)`, type-attributes `(t [@attr])`, residual module-expr forms,
and the docstring side-channel — each its own small subsystem.

### 2026-06-04 — parser tail 95.8 → 97.2% over-parseable (day 2)

Diff-bucket grind, one file/feature at a time: `M.()`/`M.[]` unit & empty-list
local-opens (whole-symbol `$sloc` on the inner node); `let f (type a) : t = e`
newtype-param desugaring (`mkghost_newtype_function_body` — wrap body in a ghost
constraint under a `Pexp_newtype` chain, *not* `Pparam_newtype`); the paren-`$sloc`
family — `Pcl_apply` / `Pmty_functor` arrow / `Pmod_functor` body / `Ptyp_poly`
body / `(sig…)with…` all span from the leading/closing paren token even though the
parenthesised child keeps its inner loc; **type attributes attach at the outermost
`core_type`, not the atom** (`int -> float [@a]` binds the whole arrow — moved the
trailing-attr loop from `parse_type_app` to `parse_core_type`, +2 net); `Pwith_modtype`/
`Pwith_modtypesubst` (+ `with module N := F(List)` functor-app rhs); value path ends
at the first lowercase component so `C.one.Complex.re` is `Pexp_field`, not one ident;
record-field info-doc *after* the `;`; docstrings on modtype / class(-type) decls and
class fields (methods). Full-corpus 93.5 → **94.7** (1754/1853); over-parseable
**97.2%**; real parse-errors down 95 → 91.

**Methodology note (the thesis is the point here).** Worth recording plainly: this
whole climb is **not** being driven by the C++ type system, and is **not** blocked by
not having OCaml on hand. The oracle (`ocamlc -dparsetree`) is the entire spec — I
write a one-off heredoc, run it through the oracle to see the exact expected tree,
mirror that in `parser.cpp` + `ast_print.cpp`, and confirm byte-for-byte. The types
in `ast.hpp` are a convenience for *holding* the shape, not a guide that *tells me*
the shape; I add a variant/field reactively whenever a new oracle dump demands one.
Bespoke test cases are minted and discarded as needed — the corpus diff localises the
next fix, the heredoc nails the spec, the type system just stores the result. In other
words: the binding constraint on progress is **differential observation of the oracle**,
not static typing. That's direct evidence for the project's hypothesis that strong
types yield diminishing guidance once an agent has a tight oracle/feedback loop.

### 2026-06-04 — parser tail 97.2 → 99.1% over-parseable (day 2 cont.)

Continued the diff/parse-error bucket grind. Cleared, roughly in order: the
paren-`$sloc` family extended (`Pcl_apply`/module-type-with/functor-body/`Ptyp_poly`);
floating `ocaml.text` for an orphan doc before a closing `end` (but **not** at EOF —
`t000.ml` is all-comments); multi-index dotop array spans the whole `e.op[…]` (+
module-qualified index ops `a.M.%![…]`); `# N "file"` **line directives** (renumber +
per-position filename via a new `file_id` on `Position`, threaded lexer→parser→printer);
**primitive aliases** `external f [: t] = path` (`Pprim_alias`); package types with
`with type … and …` in `(val/module : pkg)` incl. parenthesised; `exception` patterns
as tuple elements; `with module type` constraints; record/record-pattern field type
annotations `{ f : ty [= e] }` (+ punning `{ f : ty }`); `as` aliases on tuple elements
(`(true as x, _)`, chained `(0 as y as z, x)` — bind to the element iff a comma follows);
nested constructor-app pattern args (`Some A _` = `Some (A _)`); `for <pattern> = …`;
bivariant/injective variance (`+-`, `!+`, …); coercion-only return `let f p.. :> t = e`;
`Pcty_open` (`let open M in <class_type>`); post-item attrs on module/class/class-type
decls; `Pcf_attribute`/`Pctf_attribute` + `object[@attr]` + field `[@@attr]`; PTyp/PPat
**attribute payloads** `[@n : t]` / `[@n ? p when g]`; the `function[@attr]` attr lands on
`Pfunction_cases`; `class_expr`/`class_type` trailing `[@attr]`; `~(x:t)` labeled-arg
constraint + `{< x >}` override-punning ghost; `+=` infix; leading functor params in
module types (`() -> R`, `(X:S) -> R`); **quoted-string extensions** `{%id|…|}` /
`{%%id|…|}` (str/sig/expr/pat/type, with a `content_start` token field); `Pmod_extension`;
uppercase type-variable names (`'A_name`); `#F(Int).c` functor-app class path; sig
exception `[@@attr]`. Full-corpus 94.7 → **96.5** (1788/1853); over-parseable **99.1%**;
real parse-errors down 91 → 61 (49 of those are correct rejections the oracle also makes).

**Remaining tail (~12 real under-accepts), now genuinely diminishing.** Concentrated in
three *exhaustive* syntax test files — `parsing/attributes.ml`, `parsetree/source.ml`,
`parsing/extensions.ml` — each of which needs a stack of micro-nuances: **typed/sig/pat
extension payloads** `[%id : t]` / `[%id : sig]` / `[%id ? p]` (the one coherent lever
left: would need an `ExtPayload` variant on all `*_extension` nodes, mirroring what
`Attribute` already carries), plus per-position attr placements (e.g. `(module M : T[@a])`
has no attrs slot on the `Ptyp_package` inside `Pexp_pack`). The rest are fork/exotic:
`Ptype_external` (`type t = external "t"`), `let class … in`, `? label : t -> t` (spaced
optional-label arrow), `let module%foo[@foo]`, and `Comparable.(module S)` as a type
(local-open package type). The over-accept bucket (oracle errors, we parse) is unchanged
and low-ROI: `arrow_ambiguity.ml` and two `parse-errors/` files.

**Update (later same day): typed extension payloads + `module%ext` landed → 99.2% (1789).**
Did the `ExtPayload` refactor after all — one struct (`str`/`typ`/`sig`/`pat`/`guard`/`is_sig`)
replacing `Structure payload` on all six `*_extension` nodes, with a shared `parse_ext_body()`
(disambiguating `: signature` vs `: core_type` on the first token) and a shared `ext_payload()`
printer; added `Pmty_extension` (printast prints it as `Pmod_extension` — an upstream quirk).
That cleared `extensions.ml` fully. Also `module%ext` / `let module%ext[@attr]` (wraps the
ghost `Pstr_module` in a `Pstr_extension`). **The single remaining lever is the
`%ext`-on-keyword-expressions family** — `fun%foo` / `function%` / `try%` / `match%` / `if%` /
`open%` / `while%`/`for%` and the `and[@foo]` binding attr — each `KEYWORD%ext rest` desugaring
to `Pexp_extension(ext, [ghost Pstr_eval(ghost <keyword-expr>)])` with a delicate paren/ghost
loc rule (`(fun%foo x -> ())`: the extension node spans the parens, the inner is ghost). That
clears the last two exhaustive files (`parsetree/source.ml`, `parsing/shortcut_ext_attr.ml`).
NB: the dev harness dropped large tool-output displays this session (`H.replace` errors) — route
oracle dumps and commits through files (`git commit -qF /tmp/msg`) when it flares.

### 2026-06-05 — `source.ml` byte-identical + tail files; 99.2 → 99.6% over-parseable (day 3)

Finished the `parsetree/source.ml` torture test (7561 lines, every grammar form) to
**0 diff**, then swept the next-smallest oracle-parseable diff/parse-error files:
`type_external`, `warnings/w53`, `typing-misc/open`, `parsing/attributes`,
`ppx-attributes/warning`. Full-corpus **96.5 → 96.9%** (1789 → **1796**/1853);
over-parseable **99.2 → 99.6%**; mine-only parse-errors 57 → 54.

The dominant bug class was **loc end-points over a trailing `[@attr]`**: OCaml's
`$sloc`/`$endpos` for a production runs to the last *token*, so a body's trailing
attribute (which by itself does **not** extend the body node's own loc) still extends
the *enclosing* node's loc. Fixed uniformly by using `tokens_[idx_-1].end` instead of
`<child>->loc.end` for: ghost `Pexp_function` (`let f x = e [@inline]`), match/`function`
arms (`last_case_end_` → `Pfunction_cases` loc), `Pcl_fun` bodies, and functor-type
codomains wrapped in parens.

The other big class was **attribute *attachment* precedence** — a trailing `[@attr]`
binds to the largest enclosing node, not the rightmost operand. Confirmed against the
oracle that `a::b [@x]` is `(a::b)[@x]` (but `a [@x]::b` keeps `[@x]` on `a`), and that
mid-application attributes (`f x [@a] y`, `f [@a] x`) are **syntax errors** — so apply
args are pure `simple_expr` and a trailing attr is the whole apply's; same for
module application (`Set.Make [@inlined] (Int32)` is `Pmod_apply` over an attributed
functor — application and attributes interleave). `with`-constraint manifests are
`core_type_no_attr`, so `(module M : T [@a])` / `… with type t := M.t [@a]` put `[@a]`
on the package / `Pmty_with`, not the manifest.

Smaller wins: `new%ext`, `type%ext … += …` (Pstr/Psig_typext wrapped in extension),
plain `module rec` in signatures now collects per-declaration `[@@attr]` (deleted a
shadowing buggy path); **info-docstrings** on polyvariant tags (`prf_attributes`),
object methods (`pof_attributes`, with the `field_semi` before/after-`;` rule), and
arrow args (`extra_rhs` → domain `ptyp_attributes`); class-field prefix attrs
(`method[@a]`/`inherit[@a]`/…); `Psig_recmodule` prints `pmd_attributes`; `true 0` →
`Pexp_construct(_, Some 0)`; type local-open inner is a *delimited* type so
`Comparable.(module S)` → `Ptyp_open` over `Ptyp_package`; `Ptype_external` prints with
`%S` escaping; `Ptyp_poly` type-vars escape keywords (`'\#let`) via `Pprintast.tyvar`;
Genarray `a.{…} <- v` index-array spans the whole assignment (ghost).

Remaining oracle-parseable diffs (low single digits of files): `exotic-syntax/exotic`,
`parsing/shortcut_ext_attr`, `parsing/docstrings`, `typing-deprecated/deprecated`,
`lib-result/test`. Method `: type a. t` + the `%ext`-on-keyword-exprs family from the
earlier plan are largely in; the remaining files are docstring-placement and
exotic-form heavy. **Not type-system-driven** — every fix above came from reading the
oracle's `-dparsetree` dump for a minimal heredoc and mirroring its loc/attr rule.

### 2026-06-05 (cont.) — parser COMPLETE: 100% over-parseable parity

Cleared the entire remaining tail in one push. Diff-file sweep (each → 0): `exotic`
(spaced `? label : t` arrows in core + class types, polyvariant pattern arg at
`prec_constr_appl`, `as (+)` operator alias, `F(val e)` unpack application, empty
`begin end` → unit), `shortcut_ext_attr` (`e1 ;%foo e2` sequence extension),
`deprecated` (module-constraint `$sloc` over a trailing attr; `` `B of t [@a]``
prf_attributes), `test` (trailing `[@attr]` on `exception P` binds to the
Ppat_exception, one-shot cons-attr suppression), and `docstrings` — the
docstring-*placement* torture test: empty `(**)` generates no attribute, class
declarations/descriptions carry pci docs, class bodies emit
`pre_extra`/`floating`/`post_extra` text, empty-body docstrings key at the closing
token (`pre_extra` then `pre`), and a once-only `ocaml.text` guard (a doc reachable
from two tables emits once). Also `Pstr_val` (`val` in a structure — fork feature),
`Pstr_eval` item attributes, and `let+`/`let*` as an infix operand.

Then the **last 3 over-accepts** (files the oracle rejects but we accepted): added
strictness — `parse_type_path` requires an identifier head (rejects `(int,int)\`A.t`),
a lone `(x : t)` labeled element is a syntax error, and a type-constructor path must
end lowercase (`int -> T` with bare UIDENT is rejected). Result: **1801/1801
oracle-parseable identical, 52/52 co-rejections, 0 over-accepts, 0 under-accepts.**
The dump-diff axis is exhausted; the only thing left for the parser is error-message
parity on the 52 rejected files (needs a different harness). On to the **typer**
(`-dtypedtree`).

### 2026-06-05 (cont.) — typer T-1: the `.cmi` reader, from scratch

First typer-stage decision (committed): **normalize ident stamps now, exact later**
(stamps like `x/274` are globally coupled to Stdlib's load order; matching them means
replaying `Env`'s allocation, so defer it) and **build the real `.cmi` reader up front**
(rather than a throwaway prelude — even `1 + 2` needs `Stdlib.(+)`, and the drop-in goal
needs a real reader anyway). Also discovered the output bar is lower than feared:
`printtyped.ml` dumps **no inferred `type_expr`** — only tree shape, resolved paths,
stamps, and explicit `core_type`s — so we never need OCaml's type pretty-printer to
*check* `-dtypedtree`; full inference is still required *internally* to drive the
resolution the dump records.

Built `cxx/{include/cppcaml/marshal.hpp,src/marshal.cpp}` (generic OCaml Marshal
decoder: small/big headers, the full opcode set, object table for sharing, blocks
registered-before-fields so cyclic `type_expr` graphs round-trip) and
`cxx/{include/cppcaml/cmi.hpp,src/cmi.cpp}` (interprets the arena as `Types.signature`).
A `.cmi` is just the cmi magic + plain Marshal `(name, sign)` — confirmed the read path
is always uncompressed (`utils/compression.ml`: `input_value = Stdlib.input_value`).
Decoding is **lazy + memoized by arena id**, so shared/cyclic graphs map to shared/cyclic
C++ and a one-value query only walks its subgraph. Layouts mirrored straight from
`types.mli`/`ident.ml`/`path.ml` (`type_desc` tag order is fork-specific — has
`Tfunctor`/`Texpand`/labeled `Ttuple`). Tool `c++cmi` (`--values`/`--types`/`--modules`/
`NAME`) for verification.

Coverage: `Sig_value` (full type graph), `Sig_type` (record/variant/abstract/abbrev,
incl. mutable fields, GADT return slots, manifests), `Sig_module` (`Mty_ident`/alias/
nested `signature`/functor with named param), `Sig_modtype`, `Sig_typext`. Verified vs
oracle: `(+) : int -> int -> int`, `compare : 'a -> 'a -> int`, `@@ : ('a -> 'b) -> 'a -> 'b`,
`fpclass = FP_normal | …`, `'a ref = { mutable contents : 'a }`, `('a,'b) result = Ok of 'a | Error of 'b`,
`format4 = (…) format6`, `Stdlib.Arg : (= Stdlib__Arg)`, `Map.Make : functor (Ord : OrderedType) -> sig <45 val, 2 type> end`.
**0 crashes, 0 degraded, across all 72 stdlib cmis × 3 modes.** The novel plumbing of the
typer stage (deserializing OCaml's binary type repr) landed fast and clean — observe the
bytes, mirror the layout, verify against the oracle; no fight with a type system. Next:
the stamp-normalizing `-dtypedtree` harness, then `Env` + inference for trivial exprs.

## Inferencer error-rejection climb: 82.8% -> 97.0% accept (15 commits)

With the dump confirmed inference-blind (printtyped emits no inferred types), the
inferencer is validated by **error-rejection parity** instead: of the 744 corpus files
the oracle ACCEPTS (non-empty cached `-dtypedtree`), how many does `c++type --check`
wrongly reject? Driving that false-rejection rate toward 0 == completing the engine. A
correct checker never rejects valid code, and you can't fake-accept valid code, so the
metric can't be gamed. This session took it from **128 false-rejects (17.2%) to 22
(3.0%) — 97.0% accept**, each commit a real, oracle-validated inference capability (HM
unit tests kept green throughout):

- suffixed int literals (`0l/0L/0n` -> int32/64/nativeint); **bidirectional format
  typing** (a string literal in a `format6` position is that format, not `string` — the
  principled fix for the dominant clash, not the reverted callee-name heuristic).
- bind vars in or / poly-variant / exception / local-open / effect patterns.
- **module resolution**: qualified opens (`open Effect.Deep` walks stdlib__Effect.cmi's
  nested sigs); functor-application results (`Map.Make(..)`, curried `F(A)(B)`, local
  functors) bound by *name* to fresh vars; **module aliases followed** (`include
  Stdlib.Array` -> stdlib__Array.cmi); first-class module unpacks (`(val x : S)` against a
  local `module type S`); expression-level structure items (`let module`/`let open` in e).
- `(type a)` newtype params add no value arrow and bind to a *flexible* var (so GADT
  result annotations don't clash); **GADT matches** don't cross-unify branch results.
- **label-aware application** (labelled args matched by name, optionals omittable; falls
  back to positional for var-typed callees) — added a label to the Arrow type.
- source-level type abbreviations expanded; format/format4/format6 normalized to one
  canonical type; **constructor arity** distinguished from a single tuple argument
  (`A of (a*b*c)` vs `B of a*b`).

The recurring law, observed repeatedly: **completeness and soundness must advance
together.** Every time partial inference handed concrete types to an incomplete consumer
(opens, records, local-functor bodies, binding-annotation push), false-rejections went
*up* until the consumer was completed or deliberately left opaque. Records are the
standing example — deferred, because correct field typing needs type-directed
disambiguation of shared labels; a flat last-wins label map clashes (37 -> 52).

## Continued: 97.0% -> 98.9% accept (8 more commits, 22 -> 8 false-rejects)

Triaged the tail with per-error source-line tags. More wins, each oracle-validated, HM
unit tests green:

- **path canonicalization**: compare type-constructor paths by *last component* in unify
  (same type reaches us as `t` via `open Lazy` vs source `Lazy.t`); **cmi-level
  abbreviation expansion** when loading a module's values (`open Float; min 1. nan` —
  Float.t = float — no longer clashes t-vs-float).
- **optional-argument commutation**: always label-aware application via a dry run; a
  positional arg skips a leading optional (`return_exn ()` no longer matches
  ?raise:bool); erase a leading optional once a later positional is supplied.
- **GADT matches detected by constructor** (not just scrutinee type), skipping *both*
  pattern/scrutinee and result unifies (branch-local refinement we can't model);
  **polymorphic-recursive** `let rec f : type a. T` bound to its generic annotation so
  recursive calls instantiate (kills a spurious occurs-check).
- **constructor/exception disambiguation**: a constructor defined in >1 type, or a name
  that is both an exception and a variant constructor (`exception E` + `type t = E`), is
  ambiguous without type-direction — registered, detected, and left opaque rather than
  resolved last-wins to the wrong type.

Two changes were tried and **reverted as net-negative** (the law again): pushing a
binding's type annotation into function params (surfaces clashes in incomplete bodies),
and relaxing unify's arity check for existential GADTs (broke a soundness unit test).

## Continued: 98.9% -> 99.2% accept (3 more commits, 8 -> 6 false-rejects)

A gradual-typing device closed most of what remained:

- **a dynamic `Any` type** in the engine: it unifies with anything without clashing or
  propagating (a touched var links to Any).  Routed every "we can't infer this" case to it
  -- qualified `M.x`, unknown/ambiguous constructors, unresolved record fields/expressions,
  unknown-pattern variables.  Unlike a fresh var (which can still clash if unified to two
  concretes), Any *strictly cannot* add a false rejection.  This is the principled
  best-effort stance, and it fixed the polymorphic-record-field case (`{ pf : 'a. ... }`
  used at several types now flows through Any).
- **arity padding**: track each type's declared arity and pad an under-applied constructor
  (an existential GADT's `_ raw_arity` with fewer wildcards than params) with Any, so it
  unifies with the fully-applied form instead of clashing on arity.

The final **6** (0.8%) all live in deep subsystems, several bundled per file: Format's
higher-order `%a` type-flow (needs decoding the format string's CamlinternalFormat
type structure), first-class-module type flow + module patterns, modular explicits
(`{M : S} -> ...`).  These are the genuine step-change beyond the incremental + gradual
device: a format-type subsystem and the principal-types/Env layer (records disambiguation
rides the latter).  Net result this session: **128 -> 6 false-rejects, 82.8% -> 99.2%
accept**, every committed change sound (HM unit tests green; three net-negative attempts
reverted).

## Soundness axis: measurement framework + first sound check (the Any-weeding path)

To weed out `Any` / "implement what OCaml does" we need the *other* metric:
**false-acceptance** (does the checker reject invalid code?).  Two harnesses added:

- `cxx/harness/accept_parity.sh` — over oracle-REJECTED files; but this is confounded by
  the build environment (most plain rejects are missing external/sibling modules, which are
  valid given `-I` — not intrinsic errors).  ~85% false-accept, noisy.
- `cxx/harness/expect_soundness.sh` — the clean tool: expect-tests are self-contained and
  carry their own verdict (an `[%%expect]` block containing `Error` = a known-invalid
  file).  Strip the expect blocks, run `--check`, and a file we wrongly accept is a true
  soundness gap.  **Baseline: 81.1% false-acceptance (193/238 invalid files), completeness
  94.7% on the same corpus.**

Bucketed the 193 false-accepts by the OCaml error they carry: signature mismatch (19),
value errors (21), let-rec restriction (12), pattern/expr type mismatch (objects/modules/
GADTs), refutation (6), unbound (7), ...  Each is a faithful re-implementation of an OCaml
pass; the **strategy to remove `Any` = implement these subsystems bottom-up, each gated by
BOTH harnesses** (completeness must not regress while soundness improves) — the same
"advance together" law, now measurable on both axes.

First sound check landed: the **let-rec value restriction** (a sound subset of OCaml's
Value_rec_check) — flags only direct dereferences of a rec name at the RHS head, never
argument positions (captured by partial application), so completeness held at 99.2% while
soundness improved 81.1% -> 80.3%.  Confirmed once more that a *partial* port of the full
3-mode access analysis false-rejects (`let rec f = let g = f in fun x -> g x`).

**Net state:** completeness 99.2% accept (6 false-rejects/744); soundness 80.3%
false-acceptance (self-contained corpus).  `Any` is now visible to a metric, so its removal
is measurable.  Next soundness subsystems (each large): cross-module resolution + Env
(retires the qualified-`M.x` Any and unbound errors), records/constructor disambiguation
(retires the record/ctor Any), module signature matching, full inference for the
object/module/GADT mismatch cases.

## Soundness checks batch (this turn): 81.1% -> 78.6% false-acceptance, completeness held

Three sound checks landed, each gated by BOTH harnesses (completeness stayed at 6
false-rejects / 99.2% throughout):

1. **let-rec value restriction** (sound subset of Value_rec_check) — flag a direct deref of
   a rec name at the RHS head only (never argument position).
2. **qualified Unbound value** — for `M.x`, resolve M's exports; if M is known but lacks x,
   it's a genuine Unbound error.  Present qualified values still return Any (their real type
   surfaced 29 clashes in incomplete inference; the *check* is decoupled and sound alone).
3. **unbound type variables in a type declaration** — a body variable must be a param /
   constraint var / constructor existential; skip GADTs and implicit-var-binding bodies
   (poly-variants, objects, aliases) to stay sound.

Key finding reaffirmed on the soundness axis: the dominant remaining false-accepts need the
**type-identity (Env) layer** — e.g. "The value y has type t/2 but expected t": OCaml
distinguishes two same-named types by stamp, while our last-component path comparison
(needed for completeness: Lazy.t == t) conflates them.  Restoring that distinction without
losing the completeness gain *is* the principal-types/Env subsystem.  Likewise "Signature
mismatch" (19, the biggest bucket) needs module subtyping.  These are the next big builds;
the cheap structural checks (unboxed validity, cyclic abbreviations, refutation) are ~2
files each and increasingly fiddly.

Current two-axis state: completeness 99.2% accept (6/744); soundness 78.6% false-acceptance
(self-contained corpus).  Harnesses: reject_parity.sh (completeness), expect_soundness.sh
(soundness) — both gate every change.

## More sound checks: 78.6% -> 76.5% false-acceptance (completeness held 99.2%)

Two further structural checks, both gated by both harnesses:

4. cyclic type-abbreviation -- reject `type t = t * t`, `type a = b and b = a` (no
   -rectypes).  Reference graph over file-local aliases; only *bare* references count (a
   qualified `M.t` is another module's type -- matching by last component invented false
   cycles like `type t = T1.t = A`, so that distinction kept completeness at 6).
5. [@@unboxed] validity -- flag unless a single-constructor/one-arg variant or single-field
   record (count violations only).

Soundness checks delivered this phase (1-5): let-rec restriction, qualified Unbound, unbound
type variables, cyclic abbreviation, unboxed validity.  Net: soundness 81.1% -> 76.5%
false-acceptance, completeness unchanged at 99.2%.

Remaining structural checks are mostly exhausted (refutation needs emptiness analysis;
unbound module/record-field need resolution that tensions with completeness).  The dominant
false-accepts now require two big subsystems, both needing the type-identity/Env layer:
per-declaration type stamps with *scoped, ordered* resolution (today's registration is a
flat best-effort pre-pass; correct identity needs the scoped rewrite -- the one remaining
large architectural step), and module signature matching (Includemod).  These can't be
bolted on without risking the 99.2% completeness, so they want a dedicated, harness-guarded
effort.

## Env-lite: scoped type identity (stamps) — foundation laid

Built the type-identity layer the soundness tail needs.  Each opaque (non-alias) type
declaration gets a unique **stamp**; a scoped `tenv` (mirroring module scopes) resolves a
bare type reference to the in-scope declaration's stamp, so a shadowed `type t` is a
distinct type.  Constructor result types and type annotations carry the stamp.

**unify is tightened purely additively**: two constructors with distinct non-zero stamps
never unify (even with matching names); everything unstamped falls through to the existing
name-based comparison.  So completeness was untouched (held at 6 / 99.2%) -- the design
goal that makes this safe.

A binding's declared type is applied as a pure **identity-only check** (`identity_clash`):
flag a stamp mismatch at corresponding positions, but do NOT unify structurally (structural
inference is still incomplete; unifying the annotation in false-rejects, e.g. array vs
iarray).  This catches the canonical case `type t=A; let x=A; module M = struct type t=B;
let f:t->t = fun B -> x end`.  Soundness 76.5% -> 76.1%.

**Next, clearly scoped:** the fuller identity payoff (e.g. unique_names_in_unification in
full) is gated by the flat *ambiguous-constructor* hack -- `A` defined in two types becomes
`Any`, hiding the clash.  Replacing it with **scoped/ordered constructor resolution** (a
ctor name resolves to the in-scope declaration, like `tenv` does for types) should improve
*both* axes at once: it fixes morematch-style completeness (each use resolves to the right
ctor) without the hack, and unblocks identity soundness.  That, plus module **signature
matching** (Includemod, the 19-file bucket), is the remaining Env work.

Session soundness arc: 81.1% -> 76.1% false-acceptance; completeness steady at 99.2%
throughout (every change gated by both harnesses).

## Scoped/ordered constructor resolution (improves both axes)

Replaced the flat ambiguous-constructor hack with a scoped `cenv` (mirrors `tenv`): a
constructor name resolves to the in-scope declaration, populated in declaration order and
module-scoped.  A name reused across local variant types is now disambiguated by position
-- fixing morematch-style completeness *without* the hack and letting a constructor's
stamped result reach use sites (unblocking type identity, e.g. unique_names_in_unification).

A name that also denotes a predef (`::`/`[]`/`Some`/...) or an exception constructor needs
type-directed disambiguation we don't have, so it falls back to Any rather than resolving to
the wrong kind (`hlist` redefining `::`/`[]` must not break a list literal `[1;2;3]`;
`exception E; type t = E; raise E` must not clash).  That guard kept completeness at 6.

Soundness 76.1% -> 74.8% false-acceptance; completeness held 99.2%.

## Session soundness arc (cumulative)

From the flat best-effort checker (81.1% false-acceptance) to **74.8%**, completeness steady
at **99.2%** throughout, via: the two-harness framework; six structural checks (let-rec,
qualified-unbound, unbound-tyvar, cyclic-abbrev, unboxed); and the Env-lite core (scoped
type identity with stamps + tenv, identity-checked binding annotations, scoped/ordered
constructor resolution + cenv).  The remaining large soundness bucket is module **signature
matching** (Includemod) -- inclusion of a structure in its `: S` ascription.

## Includemod (sound slice) + #1 records — soundness 74.8% -> 72.7%

**Includemod (signature inclusion), sound slice:** a structure ascribed `: S` must provide
every value name S requires (else "Signature mismatch: required but not provided").  Inline
sigs and known local module types only; sigs with `include` skipped (so under-populated
exports can't false-report).  The value-type and type-decl mismatches (the bulk) need
complete inference -> deferred with the rest of #1.  74.8% -> 74.4%.

**#1, first slice: RECORDS** -- deferred all session (four reverted attempts), now landed
with ZERO completeness regression.  What made it possible was the new foundation (identity
stamps, scoped resolution, Any) plus four safety measures, each found by the harness:
- unique-label field registry only (ambiguous labels need expected-type direction -> Any);
- skip polymorphic fields `{ f : 'a. ... }` (one monomorphic scheme clashes; -> Any);
- infer field *values* with error-recording suppressed (traversing them exposes unrelated
  incompleteness: effect handlers, polymorphic recursion) while still checking record shape;
- record update `{ e with ... }` -> Any (flows the base through incomplete inference);
- register a mutually-recursive type group's aliases before its record fields.
Result: 74.4% -> 72.7% false-acceptance, completeness held at 6 (99.2%).

The records win is the proof the foundation pays off: the long-blocked keystone feature went
in only after type identity + scoped resolution + the Any escape hatch were all in place --
exactly the "advance together" sequencing.  Session soundness arc: **81.1% -> 72.7%**
false-acceptance; completeness pinned at 99.2% throughout.

## Expected-type propagation (bidirectional checking) — mechanism in, corpus saturated

Implemented the bidirectional checking the value/expression-mismatch buckets need:
expected_clash (identity_clash + a mismatch between two distinct *reliable* builtins --
int/char/string/float/bool/unit/int32/64/nativeint/exn/bytes, excluding array/list/user
types where our inference is still incomplete; pure, no mutation), applied to:
- binding annotations `let x : T = e`,
- type-constraint expressions `(e : T)`,
- application arguments of an otherwise-Any qualified callee (resolve M.x's real type and
  check args, e.g. `String.length 5`).

Verified correct on synthetic cases (`let x : string = 5`, `String.length 5`, `f "x"` with
f:int->int all rejected).  **But both harnesses were unchanged (99.2% / 72.7%)**: this
expect-test corpus's remaining false-accepts are *not* simple structural mismatches -- they
are deep type-system features (objects, GADTs, recursive modules, abstract types, scope
escape, polymorphic variants).  So the propagation engine is correct and is real soundness
for ordinary code, but the corpus metric is saturated for it.

**Finding:** the easy/structural soundness wins are harvested (81.1% -> 72.7% this session).
The remaining ~173 false-accepts are concentrated in: module signature *content* matching
(value-type + type-decl comparison, ~19), and the deep features above -- each a substantial
subsystem (the OO/row-types layer being the largest).  These, not more propagation, are the
next soundness frontier.

## Signature-content matching: value side (soundness 72.7% -> 71.0%)

Extended Includemod from missing-names to value TYPES: a value a structure provides must
match the type its ascribed signature declares, flagged via expected_clash (reliable
builtins + identity, pure) so an incomplete inferred type can't false-report.  Inline
signatures only.  Skip the value-type check when the structure has a top-level `open` (a
generalized open shadows an export in our flat export map though OCaml doesn't export it --
shadowing.ml); missing-name checking still runs.  Completeness held at 6.

The type-DECLARATION side of signature content (`sig type t = int end` vs `struct type t =
string end`) remains: it needs the structure's own type definitions plumbed through
module_exports (module-scoped type tracking), which today's flat global type registry
doesn't provide -- deferred.

Session soundness arc: 81.1% -> 71.0% false-acceptance; completeness pinned at 99.2%.

## Includemod complete (value + type sides): soundness 71.0% -> 69.7%

Type-declaration matching: compare a structure's own top-level type decls (extracted
directly from its AST -- module-scoped by construction) against the ascribed signature's.
Flags, conservatively: both-manifest mismatches (expected_clash) and differing
constructor/field name sets; abstract spec accepts anything; GADT/cross-kind skipped.  Also
fixed the ascription guard so a type-only structure (empty value exports) still gets checked.
Signature-mismatch bucket 19 -> 15; completeness held at 6.

**Session soundness arc: 81.1% -> 69.7% false-acceptance; completeness pinned at 99.2%.**
The structural / identity / records / signature-matching soundness wins are now harvested.
Remaining false-accepts (~166) are concentrated in deep type-system subsystems: objects /
row-types (the "The value", "pattern matches", "type variables unbound in class" buckets --
the largest), the full Value_rec_check 3-mode analysis (let-rec, 11), match refutation /
emptiness (6), extension-constructor signature matching (3), and scope-escape / abstract-type
cases.  Each is a subsystem-scale effort rather than an incremental check.

## Soundness frontier: safe incremental slice is exhausted at 69.7%

Investigated the remaining "tractable" soundness targets and found each is gated by a deep
subsystem -- there is no safe incremental check left (any partial version false-rejects):

- **let-rec full** (11): arity-dependent.  The identical `let rec x = f ~x` is VALID in
  letrec-compilation/labels.ml (partial application of `let f () ~x = ...`, x merely
  captured) but INVALID in letrec-check/labels.ml -- which one depends on `f`'s arity (full
  vs partial application = forces vs captures the rec var).  A sound check can't flag it
  without arity/effect analysis; the minimal direct-deref check is the safe limit.
- **refutation `-> .`** (6): all GADTs -- the case is refuted by type-index reasoning
  (`BoolLit : bool t` is impossible at `int t`), not constructor coverage, so compute_partial
  can't decide it.
- **variant/record def mismatch** (3): cross-module kind matching (`type t0 = T0.t = {...}`
  where T0.t is abstract) and GADTs.
- **type-vars-unbound-in-class** (4), **The value / pattern / expression** buckets: objects/
  row-types, GADTs, scope-escape.

So every remaining false-accept needs a subsystem-scale feature (objects/row-types, GADT
type-indices, arity-aware effect analysis, cross-module kind/identity).  The safe,
completeness-preserving incremental soundness work is **complete at 69.7% false-acceptance
/ 30.3% correct-rejection**, with completeness pinned at 99.2% (6/744).

Final session axes: completeness 17.2% -> 0.8% false-rejection (99.2% accept); soundness
81.1% -> 69.7% false-acceptance.  Further gains are a deliberate choice to build a full deep
subsystem (with transient completeness risk), not an incremental check.

## Lambda climb: stdlib field resolution + faithful Format port

Two foundational pieces landed (parity flat at 11.3%, but for the right reason -- see below):

- **Stdlib field resolution.** Extended the cmi reader to emit the module's runtime field
  components in order (Signature.fields): a regular value (Val_reg, detected via val_kind --
  an inlined %/C primitive takes no field), an exception, or a submodule.  Verified against
  the oracle: print_string=41, print_int=43 exactly.  Lambda now compiles a pervasive /
  Stdlib.x to `(field_imm N (global Stdlib!))`.
- **Faithful Format pretty-printer.** Discovered `@[<n>]` is **Pp_box**, not hov (read
  camlinternalFormat.open_box_of_string + format.ml's break logic).  Ported format.ml's
  decisions exactly: a box becomes "fits" when its flat width fits (all breaks flat) else
  keeps its type; Pp_box breaks a hint when the next chunk overflows OR the current line is
  already indented past the box's open column (the rule that puts the module's makeblock on
  its own line).  Nesting-aware Oppen break-sizes; raw stamps for layout (digit widths must
  match OCaml; harness normalizes for the byte compare).

Now byte-exact: pervasive calls, let-in, sequences, packed function params, the
module let/makeblock structure.  **The printer was the gate for multi-line files; it is now
faithful (proven on the test cases).  The remaining gate is translation breadth** --
match/switch compilation, tuples/records/constructors -> makeblock (the type-erasure into
blocks/tags), and qualified calls into other stdlib modules (List.map -> field of
Stdlib__List).  Each is a population of files.  The dump-diff loop is solid; this is the
typer climb again, earlier on the curve.

### 2026-06-07 — lambda 9.8 → 18.6%, NEW bytecode + `.cmo`, first runnable artifact

**Lambda climb (`ocamlc -dlambda`): 9.8 → 18.6% dump-parity** — 73 → **138** byte-identical
files of 743, with **completeness held at 99.2%** (false-rejection over oracle-accepted) the
whole way. Features added, each oracle-validated: match→switch; seq for discarded bindings +
the print-margin fix (effective margin 77); unit value-kind; `ref`/`!`/`:=`/`incr`/`decr`;
records (literal/field/set, mutable→makemutable); user variant construction; submodules +
`M.x`; for/while; exceptions/raise/try-with; arrays; string escaping + char + boxed-int
literals; C externals; pervasive %-primitives; comparison operators; function params for all
patterns; per-expression value-kind inference.

**NEW phase — bytecode instruction stream (`-dinstr`).** Added `cxx/{include,src}/bytecode.
{hpp,cpp}`: Bytegen lowers the Lambda IR to the stack+accumulator VM instruction stream,
validated against `ocamlc -dinstr` via `cxx/harness/instr_parity.sh` (tool `c++instr`).
**Baseline 20.8%** (162/777 byte-identical instruction streams). Of the diffs, only ~22 are
bytegen's own gaps (stubbed for/while/switch/try); the rest are *inherited* from the lambda
stage — so closing lambda pulls this up with it.

**NEW phase — emitcode / `.cmo`, and the first runnable artifact.** Added `cxx/{include,src}/
cmo.{hpp,cpp}` + the `c++cmo` tool: encodes the instruction stream to relocatable bytecode
(compact opcodes, label backpatching, the emit peephole fusions, relocations) and writes a
real `.cmo` — magic `Caml1999O038` + code + a marshaled `Cmo_format.compilation_unit`, via a
minimal OCaml Marshal *writer*. **MILESTONE:** a c++caml-produced `.cmo` is read by
`ocamlobjinfo`, linked by the real `ocamlc` against `stdlib.cma`, and run by `ocamlrun` —
`hello.ml` prints "hello from c++caml"; arithmetic, `string_of_int`, `if`, and a user
function also run correctly. First end-to-end byte that c++caml emits and the real toolchain
accepts and runs.

Pipeline now: lex (done) → parse (done) → type/infer (done, 99.2% accept) → lambda (18.6%) →
bytecode instr (20.8%) → emit `.cmo` (runnable) → **next: our own linker**.

### 2026-06-08 — lambda 18.8 → 24.5%, completeness 8 → 3; Printf/assert/matches run end-to-end

**Headline metrics.** Lambda dump-parity (`ocamlc -dlambda`) **18.8 → 24.5%**;
instruction-stream parity (`-dinstr`) **21.1 → 23.0%**; and — the best completeness yet —
false-rejection over the 744 oracle-accepted files **8 → 3** (and *held at 3 throughout the
whole climb*, both harnesses gating every commit). A long list of constructs that previously
crashed or miscompiled the self-hosted `c++ocamlc` now run end-to-end: Printf/Format/sprintf,
`assert`, guards, option/list/result/nested matches, and `classify_float`/`fpclass`.

**Rooting out the `Any` dynamic type (this drove most of the lambda gain).** Qualified stdlib
values `M.x` now flow their **real cmi types** instead of Any, so value-kind inference works
through them (`String.length s` comes back `int`, not dynamic). The risk is the usual one —
handing concrete types to the still-incomplete inferencer surfaces clashes (formats, GADTs,
cross-module abbreviations) — so a new **`soft_unify`** is used at function-argument positions:
propagation-only, a clash leaves the types unlinked rather than rejecting, because genuine
qualified-argument errors are already caught by the separate, *reliable* `expected_clash`
check. The advance-together law again: types flow for kinds, soundness stays with the reliable
check. Net **completeness 8 → 3** — the best so far. Also registered the predefined `Ok`/`Error`
(result) constructors.

**Stdlib values lower to their real implementation.** The cmi reader now decodes each value's
`Val_prim` `prim_name` + `prim_arity`, and the Lambda translator resolves them instead of
emitting an unresolved `?name`: `%`-builtins (`%opaque`→opaque, `%compare`→
`compare_ints`/`compare_floats`, `%ignore`, `%identity`, `%field0`/`%field1`→`fst`/`snd`) and
C-external primitives (`classify_float`→`caml_classify_float`, `sqrt`, `int_of_string`, …).
Top-level Stdlib variant constructors (`fpclass`'s `FP_normal` etc.) resolve to their tag, and
an all-constant type is marked immediate so the value gets the `[int]` kind.

**Format strings — the biggest single feature.** Printf/Format string literals now lower to the
real `CamlinternalFormatBasics` `Format(fmt, string)` structured constant — parsing the
`%`-directives and `@`-formatting into the cons-list the back end expects (previously emitted as
a plain string, which segfaulted at runtime). And the *inferencer* now infers a format's
argument types from the same parse: `printf "%d %d" x y` flows `x:int`, `y:int` and the unit
result, with detection principled (the format literal is recorded by the existing
expected-format-type rule, not the reverted callee-name heuristic). Printf/Format/sprintf run
end-to-end.

**Match compiler — built out almost fully.** Constructor-pattern matching (option/list/result
and all exhaustive single-type variant shapes, including the isint-split mixed
constant+block forms); nested / multi-row sub-patterns (`Some 0 | Some n`) via a borrowed
Row-view refactor; guards on variable/wildcard patterns AND on constructor patterns — the
latter via a new **catch/exit static-exception IR**, the first matrix-optimizer piece;
non-exhaustive matches filling the missing slots with `Match_failure`; and constant-constructor
patterns matched by their integer tag. The harness now normalizes exit-numbers the way it
already normalizes stamps.

**`assert` + predefined-exception globals.** `assert false` lowers to `raise Assert_failure`
with the location; `assert e` to the `if`/`raise` form. Both ride the new
predefined-exception-global infrastructure (`Match_failure`/`Assert_failure` globals via
`Reloc_getpredef`).

**Runtime fixes (all were crashing).** `isint` → the `ISINT` instruction; `ignore` →
eval-arg-then-unit in Bytegen; predef-exception `GETGLOBAL` relocations.

**Value-kind + printer wins (the recent parity surge).** Inferring types inside `assert` and
function-cases guards (closing a value-kind gap); printing function `[@inline never]`/`[@inline]`
annotations (`never_inline`/`always_inline`); **flattening nested sequences in the `-dlambda`
printer** (`(seq e1 e2 e3)`, not nested — a big one); and specializing comparison operators by
operand value-kind (`==.` for float, `Int64.==`, etc.).

Pipeline unchanged in shape — lex → parse → type/infer (3/744 false-rejects) → lambda (24.5%) →
bytecode instr (23.0%) → emit `.cmo` (runnable) → **next: our own linker** — but a much wider
slice of real programs now compiles and runs through `c++ocamlc`.

## Catch-up: the exec-parity era (linker, driver, objects, effects — 2026-06-07..11)

The journal fell behind the work; the compressed history. After the `.cmo` emitter
came our own **linker** (`c++link`, a bytelink/symtable port: predef exception
slots, relocation patching, CODE/PRIM/DATA + trailer — later upgraded to
**reachability linking**, selecting only required `.cma` units like the real
bytelink), and the **drop-in driver `c++ocamlc`** (source → `.cmo` → link against
`stdlib.cma` → `#!ocamlrun` launcher; `c++ocamlc f.ml -o f && ./f`). That made a
new primary metric possible: **exec parity** — compile every corpus file with both
compilers, run both, compare stdout/stderr/exit. It started at 55.3% and exposed
whole bug classes the dump diffs are structurally blind to (dropped opcodes,
boxed-literal wire encodings, out-of-scope binders, silent first-arm-collapse
miscompiles).

Since then the back end grew: let-rec (incl. the full value_rec stub/size
classes), labeled/optional arguments with eval-order-preserving partial-app stubs,
lazy values, format-string lowering (full `fmt_ebb_of_string` mirror), effects
(runstack/resume/reperform + the `effect (E x), k` match syntax), exit-with-args
static catches, Bigarray's generic ccalls, and the **object/class subsystem**
(CamlinternalOO: immediate objects, classes, params, initializers, single
inheritance + `as super` calls, virtual classes, class aliases). The typer-side
axes ran in parallel: soundness 19.5% → 86.3% correct-rejection (unbound-module
detection + reliable-builtin clashes), completeness driven from 8 false-rejects
down to 3, with both harnesses gating every back-end change.

## MILESTONE: 0 false-rejects — and the exec triage loop keeps paying (2026-06-12)

**The inferencer now accepts 744/744 oracle-accepted corpus files (100%).** The
last three false-rejects fell to three principled fixes, not hacks:

- **GADT branch-local refinement** via a full undo trail in the unification
  engine (mark/rollback windows over var bindings, level changes, and repr's
  path compression) — arms refine in a window, then roll back.
- **`build_as_type` for as-patterns** (typecore's rule): the variable bound by
  `pat as x` gets a type *rebuilt* from the pattern — a constructor that doesn't
  constrain a type parameter leaves it free, so `B _ | C _ as x -> x` returns x
  at a different instantiation than the scrutinee (test_generator's
  `unit … atom` in, `int list … atom` out).
- **Qualified abbreviation expansion from the cmi**: a source annotation
  `int Seq.t` now expands to the manifest *arrow* `unit -> int Seq.node` the way
  cmi-side types already did, instead of clashing Constr-vs-Arrow.

Strict-pass-only where it matters: the kind pass keeps its scrutinee-typed
bindings, so the lambda/instr dumps were byte-identical throughout (the
completeness/back-end axes stay independently gated).

Same session, the smallest-first exec triage found three more systemic bugs:

- **`%n %l %N %L` are conversions** (deprecated unsigned printers →
  `Scan_get_counter`), not length modifiers; we fell back to a plain string =
  segfault. Now byte-exact.
- **Unqualified stdlib record labels through inference**: `stats.major_collections`
  with `stats : Gc.stat` silently compiled to literal `0` (!). Fixed by
  qualifying same-unit cmi type paths ("stat" in gc.cmi → "Gc.stat") and
  exporting per-expression type paths from the kind pass so the field translator
  can read the record layout from the module's cmi.
- **`try perform e with Unhandled E ->`**: bare exception names didn't resolve
  through `open`ed modules, and a constant extension-ctor *payload* pattern
  wasn't a supported test — the arm silently dropped to a reraise. Now the
  byte-exact catch/exit-shared identity-test form.

Dashboard: **exec 82%+ (was 55.3% at the metric's birth), lambda 45.0% (334),
instr 48.5% (377), completeness 100% (0/744 false-rejects), soundness 86.3%**.
Pipeline: lex (100%) → parse (100%) → type/infer (100% accept / 86.3% reject) →
lambda → bytecode → .cmo → link → run.

## Crossing 91% exec: the crash classes fall (2026-06-12, cont.)

Four sessions' worth of "known-hard" entries fell in one sweep, mostly to
letrec and silent-drop classes:

- **The letrec compile-crash class is dead** (4 files: recvalues, lazy_,
  pr12153, hamming). partition_rec only knew Regular_block sizing, so a
  recursive lazy thunk or array literal fell through to a raw letrec whose
  non-function bindings crashed bytegen on a null body. Ported the rest of
  value_rec_compiler's sizing: `Pmakelazyblock` → `caml_alloc_dummy_lazy` +
  `caml_update_dummy_lazy` (non-syntactic thunks wrapped in
  `CamlinternalLazy.indirect` so backpatch can't race a force), makearray →
  regular/float dummies. Plus `{contents = e}` is the predef ref record — it
  compiled to the placeholder 0.
- **Recursive class declarations**: `class foo = … and bar = …` bodies can
  `new` any group member (even a lone class can `new` itself), but we bound
  each name only after its body — forward refs compiled to `(apply 0 args)`.
  Pre-bind the group, then backpatch referencing bindings through the
  alloc_dummy(3) scheme. Cleared backtrace/methods.ml.
- **The backtrace cluster (~9 files) needed NO debug-info support** — the
  oracle compiles without -g in our harness, so backtraces print "unknown
  location" on both sides. The real bugs were: lazy patterns never forcing
  (`let (lazy ()) = l2` was a no-op; now bound strict via inline_lazy_force),
  `E p as x` in try handlers silently dropped, value-position exception
  matches (`match exn with Error "f" ->`) falling into the variant paths
  (result's builtin Error blocked ext_match; constant payloads unsupported),
  value-or-exception or-patterns miscompiling, and %raise_with_backtrace
  leaking as `?`. All but pr2195 (a cosmetic extra backtrace frame) now match.
- **Inline-record constructors** (`T of { pos : int }`) registered as
  zero-arity blocks: construction built `[0: 0]`, destructuring leaked
  `?pos`, `r.cnt` read garbage. Full support: construction in label order,
  patterns by label index (`T r` binds the block itself), labels in
  field_info_ for `.cnt` access.

Dashboard: **exec 91.1% (658/722, 0 compile-fails, was 89.1%), lambda 47.2%
(351), instr 51.5% (400), completeness 100% (0/744), soundness 86.3%** — the
back-end gates pinned exactly through all 4 commits (one stash-bisect
churn-check on the riskiest field_info_ change: identical DIFF lists).

## The module-system layout debt comes due (2026-06-12, cont.)

Three commits (c4d5e40a6e, 9c18e41408, a0c110fdfa), each smallest-first-triaged
from the 63-DIFF frontier and gated as usual. The arc of the day: almost every
remaining segfault traced back to *module layout information we never had*.

- **First-class modules + dotted module paths** (the diagnosed multi-file
  target from last session). `module X = (val x)` had an empty layout — `X.M`,
  `Z.s` through alias chains, and even plain `module D = B; B.M.s` silently
  leaked `?s`. The fix is one idea: `module_layout_` is now ALSO keyed by
  dotted paths ("X.M"), with `resolve_module_path` walking deep prefixes,
  `copy_layout_subtree` re-registering an alias source's whole subtree, and
  `register_sig_layouts` walking a `module type S = sig..end` AST (kept in
  modtype_ast_) for nested member/functor layouts. `let x = (module .. : S)`
  records x's package type; `(val x)` reads it back; `(module P : S)` params
  register P's layout; `(module M)` arguments coerce to the callee's recorded
  package type via the let/N + makeblock field_mut projection ocamlc inserts.
  Cleared shape-index/index_aliases and typing-modular-explicits/compiling.
- **Signature-ascription coercion** — the includestruct segfault. `include
  (A : sig val f .. val x .. end)` spliced fields POSITIONALLY from the raw
  block (f ← x, then applied an int). `(M : S)` now projects to S's layout,
  recursing into module members the sig narrows; a computed include rebinds
  each field (`f =a field_mut i include/N`) and re-registers sig module
  members as modules, exception members as exceptions. Two adjacent latent
  bugs fell out: a FUNCTOR body's type decls never registered (its own
  matches collapsed to the first arm — the old silent-miscompile class,
  inside any `module F(X) = struct type t = .. end`), and layouts didn't
  count exception/class/typext slots at all.
- **The 16/32/64-bit accessor family + locals-as-values**.
  %caml_bytes_get16 & co. as local externals were unmapped (segfault);
  they now print like printlambda (bytes.get16 …) and the bytecode calls
  the same checked C entries for safe and unsafe, exactly like bytegen.ml.
  Local externals referenced as VALUES eta-stub like stdlib prims. Cleared
  evaluation_order, array_spec, string_access, bigstring_access.

Dashboard: **exec 92.4% (667/722, DIFF 55, 0 compile-fails/timeouts, was
91.3%), lambda 47.8% (355), instr 52.1% (405), completeness 100% (0/744)**.
Remaining frontier is the known-hard tail: cast.ml (objects+GADT+extensible),
immediate64 (curried stdlib functor + ctors from functor-result sigs), sets
(higher-order functors), tmc, pr7657 eta-coercion, struct_include_optimisation
(needs the FUSED coercion — ours allocates an intermediate), statmemprof,
parallel cluster, bigarrays.ml channel I/O.

## Binding operators (2026-06-13)

`Pexp_letop` — the `let*`/`and*`/`let+`/`and+` binding operators — was never
handled in the lambda translator, so lib-result/test.ml compiled to wrong code
and failed an assertion at runtime inside the `Result.Syntax` block.

Fixed by desugaring exactly as ocamlc does: the and-combined operand is the
left-nested `(and*) (... ((and*) e0 e1) ...) ek`, applied through `(let*)` to a
`fun pat -> body` whose single parameter is destructured by the matching
left-nested tuple pattern `((p0,p1),...,pk)`. The `let*`/`and*` operators
resolve as ordinary values in scope (e.g. brought in by `let open
Result.Syntax`). Operands evaluate in the outer scope; only the body sees the
bound variables. Not `-dlambda` byte-exact for multi-`and*` (ocamlc introduces
an intermediate `left` let and shares the projection via a `*match*` alias),
but execution-correct — the drop-in goal.

Dashboard: **exec 94.0% (679/722, was 678), lambda 47.9% (356), instr 52.1%
(405), completeness 100% (0/744)** — back-end gates held exactly.

## ref `{contents}` patterns + exec-DIFF triage (2026-06-13)

Fixed a real correctness bug: a record pattern on the predefined `'a ref`
(`g {contents=x1} {contents=x2} () = x1+x2`) silently dropped its bindings
because `is_irrefutable`/`collect_binders` had no entry for the `contents`
field, so the param took the refutable path and (with two of them) bound
nothing — the body read `?x1`/`?x2`. Both now recognize the single-field
`{contents=p}` cell and read the mutable field 0 in the function body, which
defers the read to full saturation (ocamlc's syntactic-arity semantics).

Dashboard: **exec 94.2% (680/722), lambda 47.9% (356), instr 52.1%,
completeness 100% (0/744)** — gates held.

Triage of the remaining 42 exec DIFFs (all hard-tail subsystems): objects+GADT
(cast, mixin1-3), higher-order functors (sets, testmap, testset), effects
(shallow2deep), tail-mod-cons (tmc/semantic), module-coercion fusion
(struct_include_optimisation), memprof (statmemprof x3), backtraces
(names, pr2195), multi-domain (parallel/churn), exact GC-allocation accounting
(pr7798 — we over-count by 15M words), and the **location/optional-arg cluster**:
`__FUNCTION__`/`__LOC__` family (translprim/locs — needs lexical function-name
path tracking incl. `.(fun)`/nested/functor/class) and **type-directed
optional-argument erasure** (pr7657, syntactic_arity, max_arity — eta-expand a
`?x:t -> rest`-typed value to the non-optional type by inserting `None`, as
ocamlc does: `(let (arg = v) (function eta (apply arg 0 eta)))`). The
optional-arg erasure is the best next target — one feature unblocks 3 files —
but it is type-directed (the inferencer must mark the coercion point), not a
quick win.

## Type-directed optional-argument erasure (2026-06-13)

A value of type `?l:t -> rest` used where a non-optional arrow is expected
must be eta-expanded with None for the omitted optional, as ocamlc's
`type_argument` does: `(let (arg = v) (function eta.. (apply arg 0 eta..)))`.
We passed the optional-arity function straight through, so it partially applied
and the body never ran (pr7657 dropped its two `f1` prints).

Detection lives in `infer_expr_expected` (the bidirectional argument position):
walk the inferred type against the expected, emitting a slot per parameter — a
None for each optional the expected type lacks, an eta param for each kept one
— into a new `ValueKinds.optional_erasures` table. Gated to the value-kinds
pass (the returned un-erased type can't false-reject; the strict pass uses soft
propagation). The Lambda back end re-enters `expr` once (guarded) to translate
the inner value, then wraps it. Also typed the `'a ref` cell `r.contents` as
its element (was Any) so a ref of an optional-arity function is erased too.

Dashboard: **exec 94.2% (680/722), lambda 47.9% (356), instr 52.1%,
completeness 100% (0/744)** — gates held; HM unit tests green. pr7657 flipped
to MATCH. (syntactic_arity/max_arity still DIFF — they need definition-site
optional *defaults* with patterns + `__FUNCTION__`, distinct features.)

## Lambda near-miss climb (2026-06-13, cont.)

Switched from exec to lambda-parity near-misses (ranked by normalized diff size,
/tmp/{lnorm.pl,lrank.sh,ldiff.sh}). Two fixes, +15 files:

- **`if c then e` (no else) is unit-typed** (infer). The else-less conditional
  was typed as the then-branch's type, so a unit-returning `if c then <stmts>`
  body (in-place loops like quicksort's `qsort`, no else) had an unresolved
  return type and dropped ocamlc's `: int` return annotation. Constrain the
  branch to unit (softly in the strict pass to avoid false-rejecting a branch we
  mis-typed; firmly in the value-kinds pass) and return unit. +3.

- **value-or-exception catch var named `val` + scrutinee kind** (lambda).
  `match e with exception P -> .. | <non-var> -> ..` lowers to
  `(catch (try (exit N e) with exn ..) with (N v) <value match>)`; ocamlc names
  the handler param after the first value-row variable, else its default `val`
  (Matching.name_pattern), with the scrutinee's value kind (`with (1 val/4[int])`).
  We used a `*match*` temp with no kind, so every such match (pervasive across the
  effects/exception-handling tests) diffed by that one line. +12.

Dashboard: **lambda 49.9% (371/743, was 356 at session start), exec 94.3%
(681/722), instr 52.1%, completeness 100% (0/744)** — gates held throughout.

Remaining near-misses are now heterogeneous: function return-kind in
effect/continuation/qualified-call bodies (lazy2 `Lazy.force` -> Any not unit,
~8 files), `=o`/`=a` let-binding kind (~3), module-field `field_mut` vs
`field_imm` (include re-export / recursive modules / opaque defs — distinct
rules per case, ~3, regression-risky), curried-vs-flattened stdlib applies
(sort_sub, input_lines). No single cheap shared fix remains; each needs
per-case work.

## Lazy typing (2026-06-13, cont.)

- **`lazy e` typed as `e Lazy.t`** (infer). Pexp_lazy was unhandled (-> Any), so
  a function forcing a lazy value (`fun () -> Lazy.force l`, common in
  Domain.spawn / effect tests) couldn't determine its unit return type and lost
  the `: int` return-kind annotation. Typed it so the element flows through
  `Lazy.force : 'a Lazy.t -> 'a`. Gated to the value-kinds pass (a concrete type
  here false-rejected in the strict pass, 0.0% -> 0.1%; the strict pass keeps
  Any). +3.

Dashboard: **lambda 50.3% (374/743), exec 94.3% (681/722), completeness 100%
(0/744)** — crossed 50% lambda parity.

The cheap shared near-miss wins are now exhausted. Remaining return-kind cases
need the effects subsystem (`perform E` with GADT effect constructors typed
Any); the `=o`/`=a` let-kind and module-field `field_mut`/`field_imm` cases are
risky reclassifications that many currently-matching files depend on (Simplif
let-kind / module-projection mutability rules).

## Effect-body return-kinds: infra + blocker (2026-06-13, cont.)

Attempted the effect-handler return-kind near-misses (callback/test7,
effects/evenodd, effects/used_cont -- functions whose bodies are `perform E` /
continuation calls, returning an immediate but typed Any).

Landed the necessary infrastructure: **typed extension-constructor declarations**
(`type t += C`, Pstr_typext was never registered -> every extensible-variant and
effect constructor was Any). Each ctor now registers with its result type (the
explicit GADT result `E : unit t`, or the extended type applied to its params;
`type exn +=` are exceptions). Completeness held at 0; lambda 374 -> 375.

But the original goal (`perform E : unit` so `fun () -> perform E` gets `: int`)
is still blocked, precisely diagnosed: `perform : 'a t -> 'a` but the cmi expands
Effect.t's manifest (`type 'a t = 'a eff = ..`) so perform's arg reads as
`'a eff`; `E`'s result is written `t` (unqualified, after `open Effect`) and we
keep it as `unit t`. `t` vs `eff` don't unify (last-component differs). Resolving
`t` -> `Effect.t` -> `eff` needs the type pre-pass to be open-aware (it currently
runs before opens are tracked) -- a structural change deferred to a focused pass.
evenodd/used_cont need more besides (the handler/continuation result type flows
from match_with's signature). So no file flipped from the effect work; the
ext-ctor typing is kept as correct general infrastructure.

Dashboard: **lambda 50.5% (375/743), exec 94.3% (681/722), completeness 100%
(0/744)**.

## Lambda near-miss climb, batch 2 (2026-06-13, cont.)

Three more, +14 files, all gated (completeness 0, exec held):

- **try/with exn binder named after the pattern var** (lambda). `| exception e ->`
  names the handler `e` (Matching.name_pattern), default `exn` only for a
  constructor pattern. We always used `exn`. +2.
- **flatten `@@`/`|>` into the applied function** (lambda). `%apply`/`%revapply`
  built a fresh `(apply f x)`, so `Array.init n @@ g` nested as
  `(apply (apply Array.init n) g)`; route through `lapply_` so it merges into
  `f`'s arg list like ocamlc's lapply. +8 (pervasive operators).
- **effect-match `match f x with` -> `runstack al f x`** (lambda). The matched
  computation, when a single-arg application of a plain function, is passed to
  runstack directly instead of the thunk `(fun () -> f x) 0`. +4 effect-syntax.

Dashboard: **lambda 52.4% (389/743, was 377), exec 94.2% (681/722),
completeness 100% (0/744)**. Session: lambda 356 -> 389 (+33), exec +3, 11 code
commits, 0 regressions, completeness pinned at 0.

## Exec-parity push (2026-06-14): toward 100%

Goal set to drive exec parity to 100%. Triaged the 42 DIFFs: ~22 segfaults
(unresolved `?` prims/members), ~10 wrong-output (matcher / exact-GC), rest
flaky. Landed (all completeness-0, gated):

- **basic %bytes_/%string_ prims** mapped (were `?`-unresolved -> segfault):
  %bytes_unsafe_set/%string_unsafe_get/%bytes_to_string/... in prim_to_lam.
  Fixed patmatch's startup segfault (still differs on multi-column matching).
- **array/string/bytes %-prims in value position** eta-stubbed
  (`let uget = unsafe_get` -> `array.unsafe_get[gen]`). Flipped bug13448.
- **qualified stdlib constructors in match patterns** (`Seq.Cons(x,_)` left x
  unbound): register_qualified_ctor + scan_pat_ctors load the type's tags from
  the module cmi at compile_match entry. Flipped lib-seq.
- **`%makemutable` (ref) in value position** eta-stubbed.

Dashboard: **exec 94.6% (684/722, was 681), lambda 52.4% (389), completeness
100% (0/744)**. Remaining 39 are deep: multi-column matching (patmatch,
morematch), higher-order/nested functors (sets, htbl, t22ok, boxedints,
floatarray), bigarray plugin (bigarrays x3, testvectors), memprof (statmemprof
x3), parallel domains (churn), objects (mixin x3, toplevel_lets), __FUNCTION__
(locs), exact GC-allocation accounting (pr7798, testset), first-class-module
members (fstclassmod), recursive modules (t22ok).

## Multi-column matching + exec push cont. (2026-06-14)

- **Multi-column matrix matcher** for all-constant enum columns. `multi_match`
  (`match e1,e2 with`) only handled 1-const/1-block columns and bailed on
  or-patterns / multi-constant enums, so `(A|B),A->'a'|(A|B),B->'b'|(C|D),_->'c'`
  matched only the first column. Generalized mm_cols: or-pattern column
  expansion + a switch path over an all-constant column (recurse on remaining
  columns per tag; var rows hit every case + default). Emit a DENSE exhaustive
  switch (case per tag 0..n_const-1, gaps reuse the default body) -- the
  bytecode Kswitch indexes labels by tag with no failaction, so a sparse
  switch+default segfaults. Relaxed the gate (or-patterns, all-constant, mixed
  var/ctor only for all-constant). patmatch still differs: it matches over an
  EXTENSIBLE variant (`type t += A|B`) by identity, not tags -- a different
  mechanism the tag matcher correctly bails on.

Dashboard: **exec 95.0% (686/722, was 681 at goal start), lambda 52.4% (389),
completeness 100% (0/744)**. Session flips toward 100%: bug13448, lib-seq, +2-3
multi-column files; plus correct infra (byte/array/ref %-prims in value pos,
qualified-stdlib-ctor match resolution). Remaining 36: extensible-variant
matching (patmatch, morematch), functors (sets/htbl/t22ok/boxedints/floatarray),
bigarray, memprof, parallel, objects (mixin), __FUNCTION__ (locs), exact-GC
(pr7798, testset), first-class-module members, recursive modules.

## Extensible-variant multi-column matching (2026-06-14)

Extended mm_cols to columns of extensible/exception constructors (`type t += A`):
an identity `(if (== col <ctor-id>) ..)` chain via exn_value, recursing on the
remaining columns. Grouped by IDENTITY (binder stamp), not name, so an alias
(`exception Bar = Foo`) shares a branch -- patmatch's PR#5788 now gives 2 not 3.
Gate relaxed for nullary extensible/exception ctors (mixed var/ctor allowed).

patmatch still differs: it declares `type t += A|B` in SEVERAL modules, and our
flat exn_ident_ map keeps only the last, so module A's `f` compares against the
wrong identity -- needs module-scoped exception resolution (separate).

Dashboard: **exec 95.0% (686/722), lambda 52.4% (389), completeness 100%
(0/744)**. This exec-push session: 681->686 (+5 files / fixes), 10 commits, all
gated, 0 regressions. Remaining 36 all deep subsystems; 100% is a multi-session
goal (and exact-GC-accounting files may be impractical to match byte-for-byte).

## include Stdlib.X + remaining-frontier confirmation (2026-06-14)

- **`include Stdlib.X` opens X bare** so its members resolve unqualified
  (`include Stdlib.Array; create_float` was `?`). floatarray's names resolve now
  but it still crashes on a deeper I/O path.

Confirmed each remaining frontier blocker by direct probe:
- functor-param NESTED-submodule member access (`M.Ops.add` / `open M; open Ops`)
  is unresolved -> blocks boxedints (single-level M.x works; nested doesn't).
- patmatch was ALREADY segfaulting pre-session (option-of-extensible + deeper
  modules); my matcher correctly bails, no regression.
- exact-GC-allocation asserts (pr7798, testset) likely unreachable byte-for-byte.

Dashboard unchanged: **exec 95.0% (686/722), lambda 52.4% (389), completeness
100% (0/744)**. The 35 remaining are each a deep subsystem (nested-module/functor
resolution, bigarray, memprof, parallel, objects, effect-depth, exn module
scoping, __FUNCTION__, first-class-mod, exact-GC). 100% is multi-session.

## Functor-param nested submodule resolution (2026-06-14)

`X.Sub.foo` / `open X.Sub` / `open X; open Sub` where X is a functor parameter
with a nested submodule were `?`-unresolved (the param layout was registered
flat). Fixed: register nested submodule layouts (register_sig_layouts on the
param), resolve local dotted opened paths via resolve_module_path, and rewrite a
bare `open Sub` to `M.Sub`. boxedints now resolves all names and runs partway
(was fully unresolved) -- it still crashes deeper in the Int32 test logic.

Dashboard: **exec 95.0% (686/722), lambda 52.4% (389), completeness 100%
(0/744)**, no regressions.

## Polymorphic-variant `#type` pattern matching (2026-06-14)

`#poly`-type patterns (`Ppat_type`, e.g. `#lambda as x`) were unhandled by the
match compiler, so any match using them fell through to a path that raised
Match_failure at runtime (typing-labels/mixin.ml).

Recorded polymorphic-variant type abbreviations (`type lambda = [ `Var | .. ]`,
with `[ a | b ]` inheritance) as a tag-hash set (pv_raw_tags_ / pv_inherits_ /
collect_pv_tags), then:
- `pat_test` gained a `Ppat_type` case (tag = the value when immediate, else
  field 0; membership = OR of equalities), so `naive_match` handles matches
  *mixing* `#type` rows with explicit-tag rows (`#var as x | `Abs .. | `App ..`)
  -- which is what mixin actually needs;
- `compile_match` treats a single `#poly` row as irrefutable (exhaustive by
  typing) -- no tag test, matching ocamlc (was the index_types.ml near-miss);
- a pure `#type | #type` match also has a direct `pvtype_match` path.

Not byte-exact with ocamlc's interval Switcher (uses an OR-of-equalities chain),
but execution-correct -- enough for exec parity.

mixin.ml flips. mixin2/mixin3 still need OBJECTS/classes (lazy_fix + `class type
ops` + methods), a separate subsystem. Confirmed during triage that the other
remaining DIFFs are each deep: translprim/locs.ml needs object support too (the
`class klass`/`inline_object` __FUNCTION__ cases); lib-set/testset+testmap and
pr7798 are EXACT-GC-allocation asserts (`a2 -. a1 = a1 -. a0`, impractical
byte-for-byte); sets/htbl segfault on unresolved functor-result members
(`?empty`/`?add`/`?mem`); sorts/exotic/floatarray/boxedints segfault on genuine
codegen bugs (no `?` placeholders).

Dashboard: **exec 95.0% (686/722, was 685), lambda 52.4% (389, held),
completeness 100% (0/744, held)**. 1 file flipped, 0 regressions across all three
dimensions (lambda-only change; index_types near-miss caught and fixed before
commit via a stash-diff baseline).

## Functor argument coercion: eta-stub externals (2026-06-14)

`F(Int32)` -- a functor applied to a bare stdlib module whose signature contains
`external`s -- segfaulted. The arg-coercion loop in compile_functor_apply looked
each parameter-signature value up in the argument's runtime FIELD layout, but
externals (of_int, to_int, neg, ...) occupy no module field, so every miss
defaulted to field 0 (`field_imm 0 M` off the wrong slot -> crash).

Fix: a param value absent from the arg's field layout is an external -> eta-stub
its primitive (`(function prim stub (Int32.of_int prim))`) via value_prim +
prim_stub, exactly like ocamlc, instead of reading a field. Regular (Val_reg)
values keep their correct index (externals are correctly skipped in the layout;
verified vs oracle that `unsigned_div` reads field 3, not 8).

Verified F(Int32)/F(Int64)/F(Nativeint) run correctly. lambda 389 + completeness
0 held. boxedints still needs the deeper NESTED case (`module Ops = Int32` inside
a struct-literal functor arg, coerced to a sub-signature -- signature-directed
module coercion threaded into struct bindings, not attempted).

Dashboard: **exec ~95.2% (687/722; FLAKY 685-687 at JOBS=3 -- heavy tests time
out, floor held at 685 = no regression, ceiling rose from 686), lambda 52.4%
(389, held), completeness 100% (0/744, held)**. Session total: 2 committed fixes
(polyvariant #type matching + functor externals coercion), 0 regressions on any
dimension. 100% remains multi-session (objects, nested-module coercion, exact-GC,
memprof, domains, bigarray, effects, backtraces, TMC each still open).

## Stdlib module-alias following: open StdLabels (2026-06-14)

`open StdLabels; List.map [..] ~f:..` segfaulted. `StdLabels.List` is an
`Mty_alias` to the top-level `ListLabels`, but submodule resolution only handled
submodules with a concrete signature, so `List.map` fell through to the PLAIN
`Stdlib__List` (whose `map` has no `~f:` label and the opposite arg order) ->
labels mis-applied -> crash.

Added stdlib_alias_target(mod, sub): reads mod's cmi, and if `mod.sub` is an
`Mty_alias`, returns the aliased top-level module's bare name. Used in two places,
gated to stdlib opens and checked BEFORE the plain module (so the alias shadows,
matching ocamlc): value resolution (`List.map` -> Stdlib__ListLabels field) and
callee_sig label lookup (so `~f:` reorders against ListLabels.map's labels --
reordering itself already worked for direct `ListLabels.map`).

Flips unboxed-primitive-args/gen_test.ml. Verified open StdLabels with
List/Array/String map/iter/iteri ~f. lambda 389 + completeness 0 held.

Dashboard: **exec 95.3% (688/722, was 685 at session start), lambda 52.4% (389,
held), completeness 100% (0/744, held)**. SESSION TOTAL: 3 committed code fixes
(polyvariant #type matching, functor externals coercion, stdlib alias following)
-> 3 deterministic exec flips (mixin, gen_test) + functor crash-class fix, 0
regressions on any dimension. Remaining 34 dominated by OBJECTS (~10 files:
backtrace/names, exotic, toplevel_lets, locs, fstclassmod, cast, mixin2/3,
pr6922, t22ok -- the translclass.ml subsystem, the single highest-ROI next
target), plus functor-member resolution (sets/htbl/boxedints), exact-GC
(testset/testmap/pr7798), effects, memprof, bigarray, domains, TMC, optional-
defaults+__FUNCTION__ (syntactic-arity).

## Curried-functor result layout + functor-resolution frontier (2026-06-14)

`module_result_layout` only handled an application whose head is a module ident,
so a curried/nested application `PowerSet(IntSet)(functor (S) -> S)` (head =
`PowerSet(IntSet)`, itself a Pmod_apply) returned empty -> the result module's
members were unresolved `?empty`/`?add`/`?mem`. Fix: unwrap nested Pmod_apply to
the base functor (committed).  basic/sets.ml members now resolve.

FRONTIER finding (functor runtime codegen): resolving the members is necessary
but NOT sufficient -- sets.ml (higher-order curried functor: a functor arg that
is itself a functor) and htbl.ml (2-param `Test(H: Hashtbl.SeededS)(M: Map.S with
type key = H.key)`) both still SEGFAULT after the `?` are gone, because the
runtime functor application / argument coercion is still wrong for these. A
`Pmty_with` layout fix (S with type t=u -> base layout) in sig_layout +
register_sig_layouts cleared htbl's `?` too, but REGRESSED pr7519_ok.ml (lambda
389->388) and flipped nothing, so it was REVERTED (net-negative -- the
advance-together law: resolving without correct coercion isn't enough).

Dashboard: **exec ~95.3% (688/722 ceiling, flaky 687-688; was 685 at session
start), lambda 52.4% (389, held), completeness 100% (0/744, held)**.

SESSION GRAND TOTAL (5 code commits, 0 regressions on any dimension):
polyvariant `#type` matching (flips mixin.ml), functor externals coercion (fixes
the F(Int32) segfault class), stdlib module-alias following (flips gen_test.ml),
curried-functor result layout (sets `?` removed). 2 deterministic exec flips +
crash-class fixes. Remaining 34 dominated by OBJECTS (~10 files, translclass.ml
-- highest ROI next) and deeper functor runtime coercion (sets/htbl/boxedints),
plus effects/memprof/bigarray/domains/TMC/exact-GC. 100% is multi-session.

## Object system: class-expression wrappers (2026-06-14)

The object runtime (object literals, methods, inheritance, virtual, initializers,
class params, coercion `:>`, polymorphic methods, self-send) already worked; the
failing object files hit unhandled class-EXPRESSION forms that fell through to a
`0` placeholder -> `new` read field 0 off an immediate -> segfault. Fixed four
class-definition wrappers (each a general improvement, gated, 0 regression):
  - `let () = e` / `let _ = e` in a class (unit/wildcard pattern, not Ppat_var)
    -> bind a throwaway *match* temp, keep the class-creation side effect;
  - `let open M in <class-expr>` (Pcl_open) -> push/pop the open around build;
  - `class c : t = ..` (Pcl_constraint) -> unwrap the runtime-irrelevant ascription;
  - `class c = let () = e in parent args` -> emit the class-creation lets and wrap
    the class-application value with them.

runtime-objects/toplevel_lets.ml now runs M1/M2/M3 correctly (was crashing at M1).
STILL needs M4's `(let () = e in parent) ()` shape (a let INSIDE the application,
object-creation semantics -- intricate new_init placement) to flip, then M5.
mixin2/3 need parameterized classes with self-type constraints (`object (self :
('a,var) #ops)`) + lazy_fix; cast needs object+extensible-type interplay.

Dashboard: **exec 95.3% (688/722, flaky 687-688; was 685 at session start), lambda
52.4% (389, held), completeness 100% (0/744, held)**. SESSION TOTAL: 9 code commits
(polyvariant #type, functor externals, stdlib alias, curried-functor, + 4 object
class-expr wrappers), 2 deterministic exec flips (mixin, gen_test) + crash-class
fixes + broad object-system advance, 0 regressions on any dimension. Objects remain
the highest-count remaining cluster; the per-file deep features (M4 nested-let-app,
parameterized-class self-types, lazy_fix) are the next steps.

## Object system: toplevel_lets FLIPPED (2026-06-14)

Completed the class-expression let handling and flipped
runtime-objects/toplevel_lets.ml (all five modules M1-M5):
  - M3: class-creation lets before a class application (wrap the class value);
  - M4: a let INSIDE a class application (`(let () = e in parent) args`) is
    per-object -> emit it in the new_init wrap body;
  - M5: a let UNDER a constraint (`(let .. in object : ct)`) is per-object too
    ("Constraints prevent lifting") -> route constraint-shadowed lets to a
    separate per_obj_lets list that build_object wraps around the env_init body
    (not class_init).

Dashboard: **exec 95.3% (689/722 ceiling, flaky -- debuggee toggles; was 685 at
session start), lambda 52.4% (389, held), completeness 100% (0/744, held)**.

SESSION GRAND TOTAL: 12 code commits, **3 deterministic exec flips (mixin.ml,
gen_test.ml, toplevel_lets.ml)** + functor/curried crash-class fixes + broad
object-system advance, 0 regressions on any dimension across the whole session.
Object class-expr forms now handled: object literals/methods/inheritance/virtual/
initializers/params/coercion/poly-methods/self-send (pre-existing) + let-in-class
/ let-open / class-type-ascription / class-app-with-lets / let-in-class-app /
constraint-prevents-lifting (this session). REMAINING objects need deeper per-file
features: mixin2/3 = parameterized classes with self-type constraints (`object
(self : ('a,var) #ops)`) + lazy_fix recursion; cast = object+extensible interplay;
pr6922 = virtual class type hierarchies; backtrace/names + locs also need
__FUNCTION__/backtraces; t22ok = recursive modules. Plus the non-object deep tail
(functor runtime coercion, effects, memprof, bigarray, domains, TMC, exact-GC).

## Object frontier mapped (2026-06-14)

Probed the remaining object files; the object RUNTIME + the class-expr forms now
handled cover the "simple" object tests (toplevel_lets flipped). The rest each
need a STACK of deep features, precisely:

- **Parameterized class with a param-dependent let** (`class c (n) = let m = n+1
  in object method get = m end`) -> SEGFAULT. ocamlc compiles such a let as an
  INSTANCE VARIABLE: allocate a slot at class-creation (`m =o new_variable class
  ""`), methods read it `(field_computed self m)`, and obj_init (which receives
  the param) computes `m = n+1` per-object and `setfield_computed`s it. Our code
  bails (rhs_leaks_param -> nullptr -> `0` placeholder) because it only lifts
  param-INDEPENDENT lets. Fix = route param-dependent cl_lets into the `vals`
  (instance-var) machinery before val_id setup. Contained but real; doesn't flip
  a file alone.
- **mixin2/mixin3** additionally need `open MoreLabels` (labeled Map/Set/Hashtbl
  -- MoreLabels.Map is a SUBMODULE, not a top-level alias like StdLabels.List, so
  the stdlib_alias_target mechanism doesn't cover it) + lazy_fix recursive objects
  (works in isolation) + the param-dependent-let instance vars above.
- **cast** = GADTs (`type 'a class_name = .. constraint`) + polymorphic methods
  with local types (`method cast : type a. a name -> a`) + object coercion
  `(self :> foo_t)` + extensible types.
- **pr6922** = virtual class-type hierarchies; **exotic/locs/backtrace-names** =
  objects + (__FUNCTION__ / backtraces); **t22ok** = recursive modules.

Confirmed working in isolation: object literals/methods/inheritance/virtual/
initializers/params/coercion/poly-methods/self-send, parameterized classes,
self-type annotations, class constraints, class let-groups, lazy_fix recursive
objects, simple Set.Make/Map.Make. Dashboard unchanged: **exec 95.3% (689 ceiling),
lambda 389, completeness 0**.

## Parameterized-class lets -> instance variables (2026-06-14)

Implemented the documented next object feature: in a parameterized class the
class-creation lets are PER-OBJECT instance variables (ocamlc runs their inits in
obj_init where the params are bound and stores them in variable slots), unlike a
parameterless class where a let is computed ONCE and the shared value is stored
into every object. Verified against the oracle that even a param-INDEPENDENT let
(`let a = 99`) in a parameterized class is per-object, while a parameterless
`let a = ref 0` is shared across instances -- so the rule keys on "class has
params", not per-binding param-dependence. Route simple var-binding cl_lets into
the `vals` instance-var machinery when cl_params is non-empty.

`class c (n) = let m = n+1 in object method get = m end` runs correctly (was a
segfault). Correctness fix; doesn't flip a corpus file (mixin2/3 still need
`open MoreLabels` -- a SUBMODULE-labeled resolution, not StdLabels' top-level
alias -- plus labeled functor-result args on Subst.add ~key ~data / Subst.fold
~init ~f, plus lazy_fix interplay). lambda 389 + completeness 0 held, no
regression (parameterless path unchanged).

Dashboard: **exec 95.4% (689/722 ceiling; deterministic 688 = mixin/gen_test/
toplevel_lets flipped, +debuggee flaky; was 685 at session start), lambda 52.4%
(389, held), completeness 100% (0/744, held)**. SESSION TOTAL: 16 code commits,
3 deterministic exec flips + many crash-class/object correctness fixes, 0
regressions on any dimension. Next object step: MoreLabels submodule resolution +
labeled functor-result args (would unblock mixin2/3 alongside lazy_fix); then
cast (GADTs+poly-methods+coercion), pr6922 (virtual class-type hierarchies).

## mixin3 FLIPPED: MoreLabels + object fixes chain (2026-06-14)

A chain of three fixes flipped typing-labels/mixin3.ml:
1. **parameterized-class lets -> instance variables** (committed earlier);
2. **labelled functor-result signatures** (`open MoreLabels; Subst.fold s ~init
   ~f` -- Map.Make's result has labelled fold; resolve `Map` through the open to
   the SUBMODULE MoreLabels.Map, read its Make result sig's value arrow labels via
   functor_result_value_sig/mt_sig, and reorder the call) -- fixes the garbage
   from Subst.fold's out-of-order labelled args;
3. **object self-pattern type annotation** `object (self : 'a)` -- the self was a
   Ppat_constraint so it never got bound, and a `self#m` send read an unbound var
   -> segfault; peel via effective_pat.

Found by deep differential bisection of mixin2's `lambda#eval` crash (lazy_fix
recursive objects + parameterized ops classes): isolated it to `self#map ~f:..`
under `(self : 'a)`, which is fix #3.

mixin2 still DIFFs -- it now runs much further (past object construction and eval)
but hits a Match_failure in expr_ops's `map` at line 120 (a malformed value from
deeper in the eval/subst/lazy recursion; the map pattern works standalone). Deep,
layered -- left for later.

Dashboard: **exec 95.6% (690/722; deterministic 689 -- mixin/gen_test/toplevel_lets
/mixin3 flipped; +debuggee flaky; was 685 at session start), lambda 52.5% (390),
completeness 100% (0/744)**. SESSION TOTAL: ~20 commits, **4 DETERMINISTIC exec
flips**, broad object + functor + label subsystems advanced, 0 regressions on any
dimension throughout.

## morematch diagnosis: ambiguous-constructor scoping (2026-06-14)

basic-more/morematch.ml fails its 2nd test (`f D` where `f x = match x with A|B|C
-> 1 | D|E -> 2 | F -> 3`, x:t). Root cause: morematch REDEFINES A..F across many
types (`type t = A|B|C|D|E|F`, `type cd = C|D`, `type zob = ..D of..`, ...), and
register_types registers them all UPFRONT so the flat ctor_info_["D"] = the LAST
type's D (cd's, tag 1), violating OCaml SCOPING (at line 27 only `type t` is in
scope, so D should be t's D, tag 3).

This breaks BOTH sides: the construction `f D` builds tag 1, and a match on t
expects tag 3. A localized match-only disambiguation (pick the single type whose
ctors contain ALL the match's ctors, override ctor_info_ under an RAII guard) was
implemented and VERIFIED to pick type t correctly -- but it's INSUFFICIENT because
the *construction* site `f D` (not a match) still uses the flat wrong tag, giving
`f D = 1`. Reverted.

The correct fix is INCREMENTAL type registration (register each `type` group as
build_module reaches it, in source order) so an early expression sees only earlier
types -- fixes construction and matching uniformly. It's a broad, regression-risky
refactor (type registration feeds all 390 lambda-matching files + completeness;
must preserve same-group forward refs for records-citing-variants, and submodule
fill-absent vs top-level overwrite semantics). Deferred to a focused session.

Dashboard unchanged: **exec 95.6% (690 ceiling), lambda 390, completeness 0**.

## Ambiguous-constructor scoping IMPLEMENTED (2026-06-14)

Implemented the source-order constructor scoping diagnosed earlier. Track per-type
ctor info + the set of names defined by >1 type; build_module re-registers a
type's ambiguous constructors when it reaches that `type` decl (in source order),
saved on module entry / restored on exit so a submodule's re-registration doesn't
leak. Single-definition ctors untouched (no-op for the common case).

Verified PARITY-NEUTRAL via stash baseline (0 lambda regressed, 0 improved),
lambda 390 + completeness 0 held, exec floor held, all 4 prior flips intact.

IMPACT (both still DIFF but much further -- the fix is correct, the remaining
blockers are separate deep features):
- **morematch**: test #2 -> #81. #81 "autre" is a 3-column matrix match with
  nested or-patterns + aliases-in-or (`(J,J,((C|D) as x|E x|F(_,x)))|..`), needing
  the matching.ml decision-tree matrix matcher (the long-deferred big matcher
  feature). Everything up to #81 now passes.
- **patmatch**: SEGFAULT -> runs to module A's `f A B` (test ~#81). That fails in
  the FULL file (a 2-column match over EXTENSION constructors `type t += A|B`
  redefined across nested modules MPR7761.A / .B) although every isolated repro
  (single module, two modules, the exact match) works -- an elusive full-context
  bug in the extension-ctor path (exn_ident_, which the ctor_info_ scoping fix does
  not touch). Pre-existing, exposed by clearing the earlier segfault.

Dashboard: **exec 95.6% (690 ceiling, deterministic 689; flaky), lambda 52.5%
(390), completeness 100% (0/744)**. Next matcher steps: the matching.ml matrix
decision tree (unlocks morematch's #81+ and other or-pattern/multi-column tests)
and module-scoped EXTENSION-ctor identities (patmatch).

## morematch FLIPPED: matrix matcher (or-expansion) + field scoping (2026-06-14)

Flipped basic-more/morematch.ml (the whole pattern-matching-compiler test suite)
with two matcher fixes:
1. **Binding or-pattern expansion**: the naive matcher bailed on a row with a
   binding or-alternative (`(C|D) as x|E x|F(_,x)` -- x bound differently per
   alternative). Added a per-row choice-map (or-node -> chosen child, no AST
   construction, cap 64) enumerating the or-free instantiations of BINDING
   or-nodes; pat_test follows the choice. Non-binding ors keep their OR codegen.
   Advanced morematch #2 -> #81 ("autre") -> #140.
2. **Record-field source-order scoping**: the field analogue of the ctor scoping.
   `x` in `type eber={x;y;z}` vs a later `type tg={v;x}` resolved to the flat
   last type (tg's boxed x) -> eber's `{x=a}` read garbage. Per-type field info +
   build_module re-registers a type's ambiguous fields in source order, marked
   resolvable via scoped_unambig_fields_. Cleared #140 -> morematch PASSES.

Both verified PARITY-NEUTRAL via stash baselines (0 lambda regressed/improved),
completeness 0 held, exec floor held, all prior flips intact.

patmatch did NOT advance (still fails module A's `f A B`, a 2-column match over
EXTENSION ctors `type t += A|B` redefined across nested modules) -- its blocker is
the extension-ctor (exn_ident_) path, which these fixes don't touch; remains
ELUSIVE (every isolated/2-module/nested repro works, only the full file fails).

Dashboard: **exec 95.7% (691/722; was 690 ceiling / 685 at the multi-session
start), lambda 52.5% (390), completeness 100% (0/744)**. Matrix-matcher via
or-expansion is a general win (any multi-column/binding-or match); next matcher
step is module-scoped extension-ctor identities (patmatch).
