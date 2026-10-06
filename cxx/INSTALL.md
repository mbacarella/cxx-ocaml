# An opam switch whose `ocamlc` and `ocamlopt` are c++ocamlc and c++ocamlopt

*This document, like the C++ implementation it installs, was generated
with an AI assistant (Claude Code).*

With the `ocaml-option-cxx` option, `ocaml-variants` built from this
repository also builds c++ocamlc and c++ocamlopt and installs them as
`bin/ocamlc.opt` and `bin/ocamlopt.opt` (`bin/ocamlc` and `bin/ocamlopt`
point at them), keeping the stock compilers as `bin/ocamlc.stock` and
`bin/ocamlopt.stock`.  Every package installed in the switch afterwards is
compiled by the C++ port, to bytecode and to native code; the other tools
(ocamldep, the toplevel, ...) are the stock ones.  c++ocamlopt's back end
is the non-flambda amd64 one on Linux: for any other configuration the
stock ocamlopt stays (with a warning in the build log).

```sh
opam switch create cxx --empty
opam pin add -n ocaml-variants.5.6.0+trunk 'git+https://github.com/mbacarella/cxx-ocaml#cxx-trunk'
opam pin add -n ocaml-option-cxx.1        'git+https://github.com/mbacarella/cxx-ocaml#cxx-trunk'
opam install ocaml-option-cxx ocaml-variants
```

Requirements, besides OCaml's own: `g++` 13 or newer (`CXX` selects
another C++23 compiler, clang++ 18 or newer works too) and -- when OCaml is
built with zstd, the default -- libzstd's development files
(`ocaml-option-cxx.opam` lists them as depexts).  It builds with GNU make,
like OCaml (`cxx/Makefile`).  The native compiler is needed (not
`ocaml-option-bytecode-only`): c++ocamlc's configuration is read from the
installed OCaml by a native program.  c++ocamlc links the mimalloc
allocator, vendored in `cxx/vendor/mimalloc`, statically.

macOS (verified on Apple Silicon; nothing in it is arm64-specific): the
Xcode command line tools are enough -- Apple clang (`g++` is clang there;
`-std=c++2b` where clang predates `c++23`), the system's make and shell --
plus libzstd when OCaml is built with it.  c++ocamlc serves a macOS switch; c++ocamlopt does not yet (its
back end is amd64 Linux's), so the switch keeps the stock ocamlopt.  In a
source tree configured by hand, `cxx/harness/gen_driver_tables.sh`
regenerates c++ocamlc's configuration tables (checked in for amd64 Linux)
for it before `make -C cxx`.

What the build does (`cxx/opam/build-cxx.sh`, in `ocaml-variants`' build
stage): it installs the tree into a stage (`make install
DESTDIR=`: the installation is `--with-relative-libdir`, so the stage
resolves its standard library as the real one will); generates
c++ocamlc's driver tables (Config, the option list, warning descriptions)
from the staged OCaml (`cxx/harness/gen_driver_tables.sh INSTALL=`);
builds c++ocamlc and c++ocamlopt, linking the very libzstd file the
runtime loads (the same compressor code, hence the same compressed bytes);
checks in the stage that `-config` is identical to the stock compilers',
that a program compiles and runs, and that c++ocamlopt's `.cmx`, `.o` and
executable are byte-identical to the stock ocamlopt's; and has opam
install them as `bin/ocamlc.opt` and `bin/ocamlopt.opt` through
`ocaml-variants.install`.  A failed check fails the build.

To check which compiler a switch has, run `ocamlc -cxx-version` or
`ocamlopt -cxx-version`: the C++ port prints `c++ocamlc (the C++ port of
ocamlc), OCaml <version>` and exits 0; the stock compilers reject the
option (`unknown option '-cxx-version'`, exit 2).

To compile with the stock compilers for a comparison, use
`ocamlc.stock` / `ocamlopt.stock` directly; to go back for good, `opam remove
ocaml-option-cxx` (opam rebuilds `ocaml-variants` without it).

c++ocamlc keeps pre-decoded images of the `.cmi` files it reads in
`$XDG_CACHE_HOME/c++ocamlc/cmi` (default `~/.cache/c++ocamlc/cmi`, at most
2 GB, oldest first out): `CPPCAML_CMI_CACHE=0` disables it,
`CPPCAML_CMI_CACHE=<dir>` moves it, `CPPCAML_CMI_CACHE_VERIFY=1` checks
each image's bytes (a few percent slower).

Packages for this development version: ppxlib's release does not know
OCaml 5.6's syntax tree; pin its main branch:
`opam pin add ppxlib 'git+https://github.com/ocaml-ppx/ppxlib#main'`.
Trunk's runtime defines `caml_int_clz` / `caml_int_ctz`, which
ocaml_intrinsics_kernel v0.17's C stubs define too, so native executables
linking it (the Jane Street ppx drivers) fail to link -- with the stock
compiler as well.  Until it is fixed upstream, pin a copy of its `v0.17`
branch with those two stubs renamed (in `src/int_stubs.c` and `src/int.ml`).

Tested (2026-09-29): a switch created this way (c++ocamlc and
c++ocamlopt), dune 3.24.2 bootstrapped by c++ocamlopt, ctypes,
ppx_inline_test and their dependencies (base, ppxlib, the Jane Street ppx
stack, integers, dune-configurator, ...) installed, and
mbacarella/mpg123 (C stubs, dune's ctypes stanza, inline tests run as
bytecode and native) built with its tests passing.  Every package rebuilt
with the stock compilers at the same paths: all 3292 .cmi, .cmo, .cma,
.cmx, .o, .cmxa, .a, .cmxs and executables identical; 262 .cmt files have
identical contents but a different Marshal sharing.
