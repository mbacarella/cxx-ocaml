# An opam switch whose `ocamlc` is c++ocamlc

With the `ocaml-option-cxx` option, `ocaml-variants` built from this
repository also builds c++ocamlc and installs it as `bin/ocamlc.opt`
(`bin/ocamlc` points at it), keeping the stock compiler as
`bin/ocamlc.stock`.  Every package installed in the switch afterwards is
compiled to bytecode by c++ocamlc; `ocamlopt` and the other tools are the
stock ones.

```sh
opam switch create cxx --empty
opam pin add -n ocaml-variants.5.6.0+trunk 'git+file:///path/to/c++caml#cpp-rewrite'
opam pin add -n ocaml-option-cxx.1        'git+file:///path/to/c++caml#cpp-rewrite'
opam install ocaml-option-cxx ocaml-variants
```

Requirements, besides OCaml's own: `clang++` 18 or newer (`CXX` selects
another C++23 compiler), `cmake`, `ninja`, and -- when OCaml is built with
zstd, the default -- libzstd's development files (`ocaml-option-cxx.opam`
lists them as depexts).  The native compiler is needed (not
`ocaml-option-bytecode-only`): c++ocamlc's configuration is read from the
installed OCaml by a native program.  opam downloads mimalloc's sources
(a checksummed `extra-source`); c++ocamlc links its allocator statically.

What the build does (`cxx/opam/build-cxx.sh`, in `ocaml-variants`' build
stage): it builds mimalloc; installs the tree into a stage (`make install
DESTDIR=`: the installation is `--with-relative-libdir`, so the stage
resolves its standard library as the real one will); generates
c++ocamlc's driver tables (Config, the option list, warning descriptions)
from the staged OCaml (`cxx/harness/gen_driver_tables.sh INSTALL=`);
builds c++ocamlc, linking the very libzstd file the runtime loads (the
same compressor code, hence the same compressed bytes); checks in the
stage that `ocamlc -config` is identical to the stock compiler's and that
a program compiles and runs; and has opam install c++ocamlc as
`bin/ocamlc.opt` through `ocaml-variants.install`.  A failed check fails
the build.

To compile with the stock compiler for a comparison, use
`ocamlc.stock` directly; to go back for good, `opam remove
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

Tested (2026-09-28, trunk b9dac8cc84): a switch created this way, ctypes, ppx_inline_test
and their dependencies (base, ppxlib, the Jane Street ppx stack, integers,
dune-configurator, ...) installed, and mbacarella/mpg123 (C stubs, dune's
ctypes stanza, inline tests) built with its tests passing.  Its build
artifacts are identical to the stock compiler's except `.cmt` files, whose
contents are identical but whose Marshal sharing sometimes differs.
