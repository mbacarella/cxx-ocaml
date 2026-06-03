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

## Progress dashboard  (updated 2026-06-03)

**Conceptual stages** (the pipeline; one binary grown stage by stage)
- ● **Lexer** — **100% token-identical** vs trunk oracle (1853 / 1853 testsuite files, incl. matching error positions on the 6 error-path files)
- ◐ **Parser** — vs `ocamlc -dparsetree`: **83.5%** of the full 1853-file corpus (1548 byte-identical), or **85.8%** over the 1804 files the oracle actually parses (≈49 corpus files are toplevel-directive scripts the oracle itself rejects as implementations — the harness now reports both). Day-1+2 (2026-06-02→03) climb **65.1 → 83.5**. Beyond the day-1 list below: extended/DOTOP index operators (`let (.!()) =`, `h.%{a;b}` get/set) + HASHOP infix; `and[@attr]`/`let%ext`; `fun[@attr]`; `#class` types (Ptyp_class); labeled/optional arrow types (`?x:t ->`); `(type a b c)` multi-newtype params; existential ctor patterns `Constr (type a) p`; type aliases `(t as 'a)` (Ptyp_alias); local-open patterns `M.(P)` (Ppat_open); class-in-signature (Psig_class); destructive type subst (`type t := …`); **explicit polymorphism** `'a 'b. t` (Ptyp_poly) in let/val/method/class-field annotations; package types in `(module M : S)` expr/pat; and **`Pmty_functor`/`Pmod_functor` locs start at the arg `(` not the `functor` keyword** (per `mk_functor_typ`'s per-arg startpos). Earlier day-1 detail: full 1853-file corpus was **77.4%** (1434/1853 byte-identical). *Always headline the full-corpus number (`parse_parity.sh 0`); the 400-subset flatters.* **Day-1 (2026-06-03) climb 65.1 → 77.4** via the parse-error/diff-bucket loop: unary `+`/`+.` signs + keyword-exprs (`match`/`fun`/…) as infix operands; **trailing-`;` seq_expr** handling (consume it; it extends the *enclosing* item span and the **Pexp_sequence** node's own span via `last_seq_end_` — the sequence-span fix alone cleared +69 files); module item-attributes; binding-level type constraints on pattern bindings (`let p : t = e`); signed-literal & `p | exception q` patterns; **`fun`/`let f p..` body `= function` merges into one Pexp_function** (Pfunction_cases directly, +32); package types in `(module M : S)` patterns; generalized `open struct…end`/`open M(X)`; `[@attr]` on qualified-path/constructor exprs; destructive type substitution `type t := …` (Psig_typesubst). Also a **harness correctness fix**: the `-dparsetree` filter anchored on `^\[$` and silently dropped empty-structure `[]` dumps, mis-scoring 47 already-identical files (true score was higher all along). The **docstring side-channel is done** (`ocaml.doc`/`ocaml.text`): ported lexer.mll's token/EOL/docstring state machine + docstrings.ml's pre/post/floating/extra tables; the trailing-`;;` structure-boundary fix alone cleared ~150 files. **printast Format quirk** understood: printast emits literal `\n` (never `@\n`), so Format's column counter never resets and every `@ ` break in a list (e.g. `<type>` newtype-univars) always fires → one element per line at column 0. **Next levers** (remaining 324 parse-errors / 94 diffs): class declarations in signatures (`Psig_class`, `class c : object…end`), module/modtype destructive subst (`module M := X`), `#line` directives (lexer-level), `##`/extended-index operators, quoted extensions `{%%M.foo|…|}`. Broad surface incl. GADTs, poly-variants, type extensions, newtype params, attributes/extensions, modules/signatures/functors (`Pmod_apply` too), first-class modules `(module M)` + package types `Ptyp_package`, coercions, `[%ext]`, `let*`, `e#m`, setfield/array-set, the missing patterns, the full **object/class** subsystem, parenthesised operators `(+)`, labelled/optional params, constrained function bindings, `let f : type a. …`, type manifest/kind redefinition + `constraint` clauses, the **signature** item subsystem, **recursive modules**, and module-type **with-constraints / `module type of` / functor arrows**. local opens `M.(e)`, qualified/bigarray field access, effect-handler patterns, type-attributes, refutation cases, and the **docstring subsystem**. Full-corpus climb this session: 41.6 → … → 50.2 → 51.8 → 54.0 → 54.8 → 56.9 → **65.1** (the last jump is docstrings). Broad-implement-then-corpus-diff loop. Key recurring fix: **menhir computes a compound node's loc from the *symbol* span (incl a parenthesized child's parens), not the child node's loc**. Remaining: the docstring side-channel (→ `ocaml.doc`/`ocaml.text`), local opens `M.(e)`, type-with-attributes `(t [@attr])`, more module-expr forms, signatures-with-class.
- ☐ Typer  ☐ Lambda  ☐ Bytecode  ☐ Native

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
