# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

This is the **OCaml compiler distribution** (the compilers, runtime, standard library, and core tools), here as a fork. The default branch for PRs is `rust-runtime-nightly`; `trunk` mirrors upstream OCaml development. `git remote` shows experimental `rust-runtime` branches — on `trunk` itself the runtime is the stock C runtime under `runtime/`.

This is a self-bootstrapping compiler: pre-built bytecode images of `ocamlc`/`ocamllex` live in `boot/` and are used to compile a fresh compiler. See `BOOTSTRAP.adoc`.

## AI contribution policy

`AI.md` is binding here. Key points: you (the contributor) take responsibility for *every part* of a contribution and must have read and reviewed it. Significant AI-generated code, PR text, or review comments **must be disclosed** (which tool, for what). Never submit code you don't understand.

## Build

The tree is **not configured** by default — `./configure` must be run before any `make`.

```sh
./configure              # add --enable-ocamltest to run the testsuite, --enable-warn-error to match CI
make -j 4                # or: make world.opt
```

- A development build is recognized by the `+dev` suffix in `VERSION`. Release builds disable ocamltest and warning-as-error by default, so pass the `--enable-*` flags above when working from a release branch.
- The actual version is defined in `build-aux/ocaml_version.m4`; after editing it run `tools/autogen` to regenerate `VERSION` and `configure`.
- After building a native `ocamlc.opt`, `cp ocamlc.opt boot/` to speed up later builds. `make bootstrap` reverts `boot/` to the slow bytecode compiler.
- `make depend` regenerates `.depend` — run it whenever you add a dependency between source files.
- `make foo V=1` shows full command lines instead of `OCAMLC`-style abbreviations (useful for learning the exact flags to rebuild one file by hand).
- `make partialclean` cleans OCaml output but keeps compiled C objects. `make runtop` builds and runs the distribution's toplevel (`make runtop-with-otherlibs` to get `Unix` etc.).

## Tests

```sh
make tests                              # full testsuite from the root (a few minutes)
make -C testsuite parallel              # faster, needs GNU parallel
make -C testsuite one DIR=tests/foo     # one directory
make -C testsuite one TEST=tests/foo/bar.ml   # one test file
make -C testsuite promote DIR=tests/foo # accept current output as new reference
```

Tests are driven by `ocamltest` (see `ocamltest/OCAMLTEST.adoc`). A test is a `.ml` file with a `TEST` block. Two common kinds:
- **file-based**: run produces a `.result` compared against committed `.reference` file(s) (which can vary by bytecode/native/flambda).
- **expect-style**: toplevel tests with inline `[%%expect {|...|}]` blocks holding expected output. Prefer these when the behavior is observable from the bytecode toplevel.

When a reference/output diff is *intended* (e.g. a changed error message or source location), use `make promote ...` rather than hand-editing, then `git diff` carefully to confirm it's not a regression before committing.

Useful env vars: `KEEP_TEST_DIR_ON_SUCCESS=1` keeps temp output; `OCAMLTESTDIR=/tmp/foo` redirects it.

## Hygiene / CI checks to run before pushing

- `./tools/check-typo-since trunk` — fast typographical check on changed files only (full `./tools/check-typo` is slow; `.gitattributes` `typo.*` attributes opt files out).
- Most patches require a `Changes` file entry (the `Changes updated` CI check enforces this); see `CONTRIBUTING.md`.
- To reproduce the default CI build locally: `bash -ex tools/ci/actions/runner.sh configure`.
- Some CI jobs are opt-in via PR labels: `CI: Full matrix`, `CI: Skip testsuite`, `run-thread-sanitizer`, `run-multicoretests`, `run-crosscompiler-tests`.

## Code architecture — the compilation pipeline

OCaml ships two compilers (`ocamlc` → bytecode interpreted by a C VM; `ocamlopt` → native code). They share the front end and diverge at the back end.

- **`driver/`** — the compilers' `main`: parses CLI args and sequences the passes below. Ppx/Camlp4 preprocessing lives here (`pparse.ml`), *not* in `parsing/`.
- **`parsing/`** — lexer/parser producing the AST (`parsetree.mli` is well-commented). See `parsing/HACKING.adoc`.
- **`typing/`** — type-checks the AST into the typed tree (`typedtree.mli`). The largest and most intricate part of the front end. See `typing/HACKING.adoc`.
- **`lambda/`** — the untyped intermediate `Lambda` representation that both back ends consume.
- **`bytecomp/`** — bytecode back end and linker.
- **`middle_end/`** — Flambda optimization passes (native only).
- **`asmcomp/`** — native code generation and linking (architecture-specific code lives here).

Supporting trees:
- **`runtime/`** — the C runtime: GC, OS/IO interaction, low-level primitives. Headers are in `runtime/caml/`. `Makefile` variables `runtime_COMMON_C_SOURCES`, `runtime_BYTECODE_ONLY_C_SOURCES`, `runtime_NATIVE_ONLY_C_SOURCES` classify which files apply to which compiler. See `runtime/HACKING.adoc`.
- **`stdlib/`** — the standard library; files are largely independent of each other.
- **`otherlibs/`** — `unix`, `threads`, `dynlink`, `str`, etc.
- **`compilerlibs/`** — packages the compiler internals as a library.
- **`toplevel/`** — the interactive REPL. **`debugger/`** — the replay debugger.
- **`lex/`** (`ocamllex`), **`yacc/`** (`ocamlyacc`) — bundled generator tools. The compiler's own grammar uses menhir (`Makefile.menhir`, `.depend.menhir`).
- **`utils/`**, **`file_formats/`** — shared utility and on-disk-format modules.

There is no top-level dune build for the Makefile workflow; the `dune-project` exists only for the experimental dune build path described next.

## Working with merlin / editor tooling

Mid-development, compiled artifacts (`.cmi`/`.cmt`) use a format incompatible with released OCaml, so merlin can't read Makefile-built artifacts. Build with dune instead, using an *older* installed OCaml + dune:

```sh
./configure              # once
make clean-for-dune && dune build @libs
```

merlin then reads `_build/`. Re-run the `dune build` after changing any module interface. Run `make clean-for-dune` again before switching back from a `make` build to a `dune` build.

## Style

Match the style of surrounding code; there is no enforced formatter. OCaml code follows `ocp-indent`; `.editorconfig` covers basic whitespace rules. Keep changes minimal and local to what you're modifying.
