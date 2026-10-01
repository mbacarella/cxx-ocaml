# An opam switch whose `ocamlc` and `ocamlopt` are c++ocamlc and c++ocamlopt

*This document, like the C++ implementation it installs, was generated
with an AI assistant (Claude Code).*

This is the `cxx-5.5` branch: OCaml 5.5.1 (upstream's `5.5` branch) with
the C++ port following 5.5.1's compiler.  The `cxx-trunk` branch has the
same setup for OCaml's development version (5.6.0+trunk).

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
opam switch create cxx-5.5 --empty
opam pin add -n ocaml-variants.5.5.1 'git+https://github.com/mbacarella/cxx-ocaml#cxx-5.5'
opam pin add -n ocaml-option-cxx.1   'git+https://github.com/mbacarella/cxx-ocaml#cxx-5.5'
opam install ocaml-option-cxx ocaml-variants
opam switch set-invariant --packages=ocaml-variants,ocaml-option-cxx
```

The last step matters: a switch created `--empty` has no invariant, and
opam's solver may then replace the pinned `ocaml-variants` by
`ocaml-base-compiler` (removing `ocaml-option-cxx`) when it installs
packages later -- installing awso's dependencies did exactly that, and
the switch silently went back to the stock compilers.

(From a local clone, pin `git+file:///path/to/clone#cxx-5.5` instead --
with `cxx-5.5` checked out in that clone, or a worktree of it: opam reads
the packages' opam files from the working tree, so a clone on another
branch gives it that branch's `ocaml-option-cxx`.)
`ocaml-option-cxx` on this branch requires `ocaml-variants` 5.5.x, so the
two pins must come from the same branch.

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

Packages: 5.5.1 is a released version, so opam's released packages
(ppxlib included) are the ones to use; nothing needs pinning.

Verified on this branch (2026-09-29), against the stock 5.5.1 compilers
built from the same tree: the standard library's .cmo (with and without
-g) and the compiler's own .cmi/.cmo byte-identical; the native compiler
built by c++ocamlopt (every .cmx, .o, .cmi, library and the ocamlopt.opt
executable) identical; errors, warnings, `-i`, `-dlambda`, `.cmt` and
native executables identical over the corpus; the diverse double-compiling
check passing.  An opam switch built from this branch has not been
exercised yet (the `cxx-trunk` one has: see that branch's INSTALL.md).
