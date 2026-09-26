# Type checking in the C++ rewrite

**Read this before touching anything typing-related in `cxx/`.**

## The goal

c++ocamlc must type-check exactly as ocamlc does.  It accepts what ocamlc
accepts, rejects what ocamlc rejects, and infers the types ocamlc infers.  The
only way to get there is a **faithful port of ocamlc's `typing/`**: `Types`,
`Btype`, `Ident`/`Path`, `Subst`, `Env`, `Ctype` (expand / unify / generalize /
instance / moregen), `Typetexp`, `Typecore`, `Typedecl`, `Typemod`,
`Includemod`, `Mtype`, `Typeclass`.  That is the parser, lambda and bytecode
method again: transcribe ocamlc's algorithms and verify them against ocamlc.

**No C++ checker in the tree does this yet.**  Everything below is deprecated
as a type checker.

## Status (2026-09-26)

c++ocamlc **does not reject ill-typed programs**.  `let x = 1 + "a"` compiles
to a .cmo, exit 0.  The only error it reports is an unbound module.

Measured with the opt-in `CPPCAML_TYPECHECK=1` (runs the strict pass below
after the outputs are written and removes them on rejection):

| What | Result |
|---|---|
| compiler corpus (139 modules, all valid) | **70 falsely rejected** |
| `cxx/harness/false_accept.sh` (137 ill-typed one-liners) | **94 falsely accepted** |
| `cxx/harness/valid_reject.sh` (6531 ocamlc-valid stamp probes) | **41 falsely rejected** |
| testsuite `reject_parity.sh` / `accept_parity.sh` | 4 false rejects / 38 false accepts |

The testsuite gates look good only because their files are small and their
ill-typed tests exercise advanced features.  Don't read them as soundness.

## Deprecated checkers (do not extend as type checkers)

- **`cxx/src/typer.cpp` (`type_structure`, `c++type` default mode)** is a
  *transcriber*.  It rebuilds the typedtree's shape (names, stamps,
  locations) to match `ocamlc -dtypedtree` on programs that are already
  valid.  It does not unify and rejects nothing: it accepts all 137 battery
  programs.  "Typed-tree dump parity 100%" measures transcription, not
  typing.
- **`cxx/src/infer_check.cpp` `Checker` + `cxx/src/infer.cpp` `Engine`** is
  a best-effort algorithm-W approximation over the parsetree.  It matches
  constructor paths by their last component, has no real `Env`, unifies
  application arguments softly, and in strict mode reports only "certain"
  clashes.
  - *Lenient / value-kinds passes:* still what codegen and the .cmi writer
    consume, and DDC plus the effid/cmi gates verify that OUTPUT.  Keep them
    working until the ported typer replaces them, but fix output bugs only.
    Don't grow them into a checker.
  - *Strict pass (`structure_typecheck`, `c++type --check`):* **deprecated.**
    Don't harden it false-reject by false-reject.  That approach was tried
    and rejected (2026-09-26).
- **Harnesses that score the deprecated checkers:** `reject_parity.sh`,
  `accept_parity.sh`, `typedtree_parity.sh`, `sig_parity.sh`.  They remain
  useful as regression tripwires for the output pipeline.  They are not
  progress metrics for type checking.  `TYPER-PARITY-ROADMAP.md` records
  that deprecated effort.

## What counts as progress

The ported typer, validated against ocamlc:

1. **Accept/reject parity.**  0 false rejects on the compiler corpus, stdlib,
   testsuite and probes (`valid_reject.sh`); 0 false accepts on
   `false_accept.sh`, which should keep growing.
2. **Same error messages** as ocamlc.
3. **Same types:** the `-dtypedtree` dumps' types, then the .cmi.

Staging: until its typedtree is trusted, the ported typer only decides
accept/reject in c++ocamlc, so .cmo/.cmi output (and DDC) cannot regress.
Replacing `infer_check` as codegen's source of types comes after that.

## Port plan (decided 2026-09-26)

**Layout.**  Namespace `cppcaml::typing`.  One C++ module per OCaml module,
same name: `cxx/include/cppcaml/typing/<m>.hpp` + `cxx/src/typing/<m>.cpp`
(`ident`, `path`, `types`, `btype`, `subst`, `predef`, `env`, `ctype`,
`typetexp`, `typecore`, ...).  Functions keep ocamlc's names and structure,
and comments cite the OCaml source (`ctype.ml unify3`).  Deviate only where
C++ forces it, and say so at the site.

**Memory.**  OCaml's heap becomes arenas (`typing/zone.hpp`).  `type_expr`
nodes, descs, paths, idents and persistent-map nodes are allocated in a zone
and never freed individually.  Cached cmis live in a long-lived zone; each
compilation unit's typing lives in its own zone, dropped after the unit.
(ocamlc allocates ~430 MB in total typing typecore.ml, but most of that is
short-lived lists and closures that become C++ locals.)  Immutable OCaml
values stay immutable; OCaml lists inside them become zone `Slice`s.

**Representation.**  `type_expr` is `transient_expr {desc, level, scope,
id}`, and `desc` points to an immutable `type_desc` node (one struct per
constructor, switch on `kind`), so the trail can log and restore old descs
exactly as `types.ml` does.  `row_field`, `field_kind` and `commutable`
keep their mutable indirection cells.  The trail (`Types.snapshot` /
`backtrack`) is ported verbatim.

**Loading cmis.**  `typing/cmi_format` decodes the marshal arena
(`marshal.hpp`, which already reconstructs sharing and cycles) straight
into `typing::Types`, value by value: that is `input_value`.  It doesn't go
through the deprecated `cmi.hpp` model.

**Stage 1 oracle.**  `cxx/harness/typing_cmidump.ml` (compiler-libs) prints a
structural dump of a cmi's `Types` graph: every constructor, every field,
sharing as first-visit numbering.  The C++ `c++typing-dump` prints the same
from the port.  The two must be identical on every .cmi in the tree
(`cxx/harness/typing_cmi_parity.sh`).

**Stages.**
1. `Ident`, `Path`, `Types` (+ trail), `Cmi_format` read: dump parity on all cmis.
2. `Btype`, `Subst`, `Predef`, `Persistent_env`, `Env` (lookups, lazy
   components, strengthening via `Mtype`).
3. `Ctype`: levels, `newvar`, `instance`/`copy`, `generalize`, `expand_head`
   and abbreviation memos, `unify`, `moregen`, `eqtype`, `filter_arrow`, ...
4. `Typetexp`, `Typecore` (core expressions and patterns, `Parmatch`), with
   ocamlc's error messages.  `CPPCAML_TYPECHECK` switches from the
   deprecated strict pass to the port here.
5. `Typedecl`, `Typemod`, `Includemod`, `Includecore`, `Mtype`, then `Typeclass`.
