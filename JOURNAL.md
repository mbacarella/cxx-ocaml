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
| Parser | 1.5–3 weeks | — | |
| Typer | 1.5–4 months | — | |
| Lambda | 3–6 weeks | — | |
| Bytecode | 1–2 weeks | — | |

---

## Progress dashboard  (updated 2026-06-05 — parser complete)

**Conceptual stages** (the pipeline; one binary grown stage by stage)
- ● **Lexer** — **100% token-identical** vs trunk oracle (1853 / 1853 testsuite files, incl. matching error positions on the 6 error-path files)
- ● **Parser** — vs `ocamlc -dparsetree`: **100.0% over every oracle-parseable file** (1801/1801 byte-identical) and **52/52 correct co-rejections** on the files the oracle itself rejects (toplevel scripts + deliberate syntax-error tests). Full-corpus number is **97.2%** (1801/1853) — and that is now the structural ceiling: the other 52 files have *no parsetree to match*, so the only remaining axis is error-message parity (different harness). **Day-3 finish (2026-06-05):** after `source.ml`, swept every remaining oracle-parseable diff file — `type_external`, `w53`, `open`, `attributes`, `warning`, `exotic`, `shortcut_ext_attr`, `deprecated`, `test`, and the docstring-placement torture test `docstrings.ml` — then closed the last 3 over-accepts by adding strictness (reject `(t,t)\`A.t`, singleton `(x:t)` labeled tuple, bare-uppercase type path `int -> T`). The docstring work ported the empty-`(**)`-drops-attribute rule, class-body `pre_extra`/`floating`/`post_extra` text, empty-body docstrings keyed at the closing token, and once-only `ocaml.text` emission. Earlier same-day: **96.9%** of the full 1853-file corpus (1796 byte-identical), or **99.6%** over 1804 oracle-parseable. **Day-3 (2026-06-05):** drove `parsetree/source.ml` (7561-line grammar torture test) to **byte-for-byte identical**, then cleared 6 more files — `type_external`, `w53`, `open`, `attributes`, `warning` (and `source`). Recurring theme this day: **a node's `$sloc`/`$endpos` includes a trailing `[@attr]` even though the attribute does not extend the child's own loc** — fixed for ghost `Pexp_function` (let-fun body), match/`function` arms (`Pfunction_cases` loc), `Pcl_fun` body, and functor-type codomains over a paren. Other day-3 wins: `new%ext`/`type%ext … +=`/plain `module rec` in sigs + per-decl `[@@attr]`; info-docstrings on polyvariant tags, object methods, and arrow args; **trailing `[@attr]` binds to the whole cons-pattern / application / module-application, not the last operand** (mid-application attrs are syntax errors); `with`-constraint manifests are `core_type_no_attr` so a trailing `[@a]` is the package/`Pmty_with`'s; `true 0` → `Pexp_construct(_, Some 0)`; type local-open `M.(module S)`; `Ptype_external` `%S` escaping; `Ptyp_poly` tyvar keyword-escaping (`'\#let`); Genarray `a.{…}<-v` index-array loc. Earlier (day-1+2) climb **65.1 → 87.6 → 96.5**. (≈49 corpus files are toplevel-directive scripts the oracle itself rejects as implementations — the harness now reports both.) Day-1+2 (2026-06-02→03) climb **65.1 → 87.6**. Latest wins: polyvariant `` `Tag`` as an app-arg atom; local-open in types `M.(t)` (Ptyp_open); module aliases in signatures `module B = A` (Pmty_alias); record-field attributes `[@atomic]` (pld_attributes); **let/letop/fun bodies are seq_expr** so their node loc extends through a trailing `;` (+20, sibling of the Pexp_sequence trailing-`;` fix); GADT inline records `A : { f } -> t`; lowercase module-type names & idents; and **functor-application type paths `F(X).t`** (Lapply; `kind_start` excludes `UIDENT (` so it's a manifest not a constructor). Remaining tail (175 real parse-gaps / ~50 diffs): method `: type a. t` (the `wrap_type_annotation` newtype+`varify` desugaring), labeled tuples `(~x, y)`, exotic type forms (`true|false` ctors, `(::)`, `external "t"`), and module/modtype destructive subst. Beyond the day-1 list below: extended/DOTOP index operators (`let (.!()) =`, `h.%{a;b}` get/set) + HASHOP infix; `and[@attr]`/`let%ext`; `fun[@attr]`; `#class` types (Ptyp_class); labeled/optional arrow types (`?x:t ->`); `(type a b c)` multi-newtype params; existential ctor patterns `Constr (type a) p`; type aliases `(t as 'a)` (Ptyp_alias); local-open patterns `M.(P)` (Ppat_open); class-in-signature (Psig_class); destructive type subst (`type t := …`); **explicit polymorphism** `'a 'b. t` (Ptyp_poly) in let/val/method/class-field annotations; package types in `(module M : S)` expr/pat; and **`Pmty_functor`/`Pmod_functor` locs start at the arg `(` not the `functor` keyword** (per `mk_functor_typ`'s per-arg startpos). Earlier day-1 detail: full 1853-file corpus was **77.4%** (1434/1853 byte-identical). *Always headline the full-corpus number (`parse_parity.sh 0`); the 400-subset flatters.* **Day-1 (2026-06-03) climb 65.1 → 77.4** via the parse-error/diff-bucket loop: unary `+`/`+.` signs + keyword-exprs (`match`/`fun`/…) as infix operands; **trailing-`;` seq_expr** handling (consume it; it extends the *enclosing* item span and the **Pexp_sequence** node's own span via `last_seq_end_` — the sequence-span fix alone cleared +69 files); module item-attributes; binding-level type constraints on pattern bindings (`let p : t = e`); signed-literal & `p | exception q` patterns; **`fun`/`let f p..` body `= function` merges into one Pexp_function** (Pfunction_cases directly, +32); package types in `(module M : S)` patterns; generalized `open struct…end`/`open M(X)`; `[@attr]` on qualified-path/constructor exprs; destructive type substitution `type t := …` (Psig_typesubst). Also a **harness correctness fix**: the `-dparsetree` filter anchored on `^\[$` and silently dropped empty-structure `[]` dumps, mis-scoring 47 already-identical files (true score was higher all along). The **docstring side-channel is done** (`ocaml.doc`/`ocaml.text`): ported lexer.mll's token/EOL/docstring state machine + docstrings.ml's pre/post/floating/extra tables; the trailing-`;;` structure-boundary fix alone cleared ~150 files. **printast Format quirk** understood: printast emits literal `\n` (never `@\n`), so Format's column counter never resets and every `@ ` break in a list (e.g. `<type>` newtype-univars) always fires → one element per line at column 0. **Next levers** (remaining 324 parse-errors / 94 diffs): class declarations in signatures (`Psig_class`, `class c : object…end`), module/modtype destructive subst (`module M := X`), `#line` directives (lexer-level), `##`/extended-index operators, quoted extensions `{%%M.foo|…|}`. Broad surface incl. GADTs, poly-variants, type extensions, newtype params, attributes/extensions, modules/signatures/functors (`Pmod_apply` too), first-class modules `(module M)` + package types `Ptyp_package`, coercions, `[%ext]`, `let*`, `e#m`, setfield/array-set, the missing patterns, the full **object/class** subsystem, parenthesised operators `(+)`, labelled/optional params, constrained function bindings, `let f : type a. …`, type manifest/kind redefinition + `constraint` clauses, the **signature** item subsystem, **recursive modules**, and module-type **with-constraints / `module type of` / functor arrows**. local opens `M.(e)`, qualified/bigarray field access, effect-handler patterns, type-attributes, refutation cases, and the **docstring subsystem**. Full-corpus climb this session: 41.6 → … → 50.2 → 51.8 → 54.0 → 54.8 → 56.9 → **65.1** (the last jump is docstrings). Broad-implement-then-corpus-diff loop. Key recurring fix: **menhir computes a compound node's loc from the *symbol* span (incl a parenthesized child's parens), not the child node's loc**. Remaining: the docstring side-channel (→ `ocaml.doc`/`ocaml.text`), local opens `M.(e)`, type-with-attributes `(t [@attr])`, more module-expr forms, signatures-with-class.
- ◐ **Typer** — *in progress; mechanical surface broad, inference engine built, dump-parity plateaued ~25%*. **301 byte-identical typedtrees** (typeable ~1183; oracle dumps CACHED, harness file-based so 10 MB dumps don't melt bash). Prerequisite **`.cmi` reader complete** (Marshal → `Types.signature`). `c++type` (`cxx/src/{typer,typedtree_print,marshal,cmi,infer,infer_check}.cpp`) transcribes: constants/let(+rec)/apply/function(+cases)/if/match/try/seq/construct/array/assert/for/while/lazy; patterns var/any/const/construct/tuple/or/alias/exception; type decls (predef `int/1!`), external, typext, exception, open(+stdlib scope), module bindings + **module types/signatures** + **module-exprs (functor/apply/constraint)** + **`let module/open/exception in e`**, attributes (+`[@@@attr]`), constraints, **records** (field registry), and **format strings** (common plain directives; conservative bail on flags/width).
  **INFERENCE ENGINE BUILT (Slices 1–3):** real minimal Hindley-Milner — mutable `type_expr` with `Tlink` union-find + levels, `unify`/`instantiate`/`generalize` (`infer.{hpp,cpp}`, unit-tested), and a best-effort algorithm-W pass (`infer_check.cpp`) that infers correct polymorphic types (`let id z = z` ⇒ `'a -> 'a`; compare-driven `'a -> 'a -> 'a`) — see `c++type --infer`. Ran over all 1853 files: 0 hangs, 0 new crashes. Wired into the dump for `Texp_match (Partial)` exhaustiveness (Slice 3, conservative → no false-positives).
  **PIVOTAL FINDING (2026-06-06):** building the engine yielded **~0 dump-parity gain**. The `-dtypedtree` dump prints no inferred types, so it barely *exposes* inference — only match-exhaustiveness and ambiguous disambiguation, both rare: **only 2 oracle-typeable files contain a `(Partial)` match at all**. So the dump is ~entirely reproducible WITHOUT the type engine; the 65K-line `ctype.ml` core is **nearly invisible in this projection** — the strongest confirmation of the thesis spread. Consequence: **dump parity has plateaued (~25%)**, gated by a mechanical long tail (format flags, float hex formatting, qualified module paths, 10 MB list-literal files) + the multi-blocker dynamic — NOT by inference. The inference engine's value is therefore downstream (lambda/bytecode) and in the real finish line (emit `.cmi`/`.cmo`, **reject ill-typed programs** — which a heuristic typer cannot do, and which the dump cannot measure). **Validation must shift** off `-dtypedtree` for the typer: the genuine next signal is type-error/rejection parity. Resolution via `Env` (locals + Stdlib from the cmi reader). Pre-existing parser bug: 6 lexing/encoding files SIGABRT in parse_structure (both modes).
  **KEY THESIS RESULT (sharpened):** the climb to 299 needed *zero HM inference*. `printtyped` dumps no inferred types, so the whole `-dtypedtree` surface is reproducible by resolution + scope + faithful transcription + a couple of registries — even **records** (the supposed frontier) fell to a field registry, not unification. The 6.7K-line `ctype.ml` core is almost *invisible* in this projection.
  **OVERFITTING WATCH (concern raised 2026-06-06):** most of this is faithful modeling, NOT test-fitting — we implement *constructs*, never special-case individual files, and a construct either matches across all corpus files using it or doesn't. The one genuine hack is **format-string detection by a hardcoded Printf/Format/Scanf function-name + arg-position heuristic** (real rule is type-directed via expected type) — explicitly tagged in-code as **scaffolding to be replaced by inference**, not a permanent design. Conservative bail-outs (format flags, unique-field-only records) are honest *gaps* (safe DIFFs), not wrong answers. The structural anti-overfit safeguard: dump parity is a *proxy* with a known blind spot (no inferred types shown), so the real finish line — byte-identical `.cmi`/`.cmo` **and rejecting ill-typed programs** — will expose anything faked (a heuristic typer cannot reject bad code).
  **Remaining:** mechanical — `Pstr_modtype`/signatures (in progress), functor/apply/constraint module-exprs, `Pexp_struct_item`, format flags. Genuinely needs inference — ambiguous field/ctor disambiguation, `Texp_match (Partial)` exhaustiveness, true format detection. **Plan:** bank the mechanical points, then bite the inference bullet as a focused milestone (minimal HM core: `type_expr`+`Tlink` union-find, levels, unify/instantiate/generalize) — it's the load-bearing capability for the real-artifact goal and the actual experiment.
  **LOAD POST-MORTEM (2026-06-06):** a long agent-driven session melted the machine. Cause: orphaned background `until…sleep` poll-loops accumulating (the result-channel kept dropping outputs, so I re-dispatched waiters that never exited) × overlapping 32-way `ocamlc` harness sweeps (couldn't tell a sweep had finished, relaunched it). Fixes: NO background poll-loops (run synchronously, check once); never re-dispatch on a transport error without checking; `JOBS=8` not 32; oracle cache removes per-run ocamlc fan-out. See [[parallel-corpus-harness]].  ☐ Lambda  ☐ Bytecode  ☐ Native

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
