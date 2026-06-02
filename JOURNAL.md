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
| Lexer | 2–4 days | day 0: **98.2% parity**, tail open | maximal-munch + escapes correct on first build |
| Parser | 1.5–3 weeks | — | |
| Typer | 1.5–4 months | — | |
| Lambda | 3–6 weeks | — | |
| Bytecode | 1–2 weeks | — | |

---

## Progress dashboard  (updated 2026-06-02)

**Conceptual stages** (the pipeline; one binary grown stage by stage)
- ◐ **Lexer** — 98.2% token-identical vs trunk oracle (1819 / 1853 testsuite files byte-for-byte)
- ☐ Parser  ☐ Typer  ☐ Lambda  ☐ Bytecode  ☐ Native

**Files converted (OCaml → C++)**

| OCaml source | lines | C++ target | status |
|---|---:|---|---|
| `parsing/lexer.mll` | 1040 | `cxx/src/lexer.cpp` + `include/cppcaml/{token,lexer}.hpp` | 98.2% token-parity |
| `parsing/parser.mly` (%token decls) | 127 | `cxx/include/cppcaml/token.hpp` (124-kind enum) | done |

**In-scope line conversion**
- **1040 / 183,121** hand-written compiler source lines = **0.57%** converted.
- (C++/oracle/harness written so far: 1,145 lines.)

**Lexer tail (the remaining ~1.8%)** — all in deferred areas, 0 over-accepts:
UTF-8 extended idents + encoding validation; invalid-escape / `\u{}`-range
error-path parity; `{%…|…|}` / `{%%…|…|}` quoted-string extensions; line
directives `# n "file"`; a few exotic literals (lib-scanf, lib-bytes binary).

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
