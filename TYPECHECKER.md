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
