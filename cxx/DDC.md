# Diverse double-compiling (DDC): checking the compiler yourself

OCaml builds itself from a binary it ships (`boot/ocamlc`).  A compiler
binary can carry a backdoor that its source does not show and that it
re-inserts every time it compiles itself (Ken Thompson's "trusting trust").
Diverse double-compiling (David A. Wheeler) rules that out: build the
compiler's source with an independent implementation, let that result
rebuild the source, and compare with what the official toolchain produces.
If the two agree bit for bit, the official compiler is exactly what its
source says.

c++ocamlc is the independent implementation: a C++ program, built by a C++
compiler, that shares no binary with OCaml's bootstrap.

## What is checked

```
diverse:   c++ocamlc (C++)  --compiles the compiler source-->  S1  (a bytecode ocamlc)
           S1               --compiles the compiler source-->  S2
official:  ocamlc.opt (built from boot/ocamlc, as the Makefile does)
DDC:       S2 and ocamlc.opt each compile the compiler's source;
           every .cmi / .cmo must be byte-identical
```

The source compared (271 modules, 355 interfaces) is the whole bytecode
compiler (the `ocamlcommon` and `ocamlbytecomp` libraries and its driver)
and the native compiler's sources (`middle_end/`, `asmcomp/` and the
`ocamlopt` driver) compiled to bytecode -- the step that builds
`ocamlopt.byte`, from which `ocamlopt.opt` is built. S2, not S1, is
compared: S1 carries c++ocamlc's code generation, S2 is the real compiler's
code produced through the diverse path.

A second check, `stdlib_ddc.sh`, rebuilds the standard library with S2 and
compares it with the stdlib the official build produced.

What stays trusted: the C++ compiler that builds c++ocamlc, the C toolchain,
and the C runtime (`ocamlrun`) that executes S1 and S2.  The stdlib modules
that need `-pp` are reported as skipped by the stdlib check (S2 cannot run a
preprocessor yet).

## Running it

You need what OCaml's build needs, plus `clang++` 18 or newer (or another
C++23 compiler), `cmake` and `ninja`, and the mimalloc library (or build
with `-DCPPCAML_MIMALLOC=OFF`, which only costs speed).

```sh
# 1. The official toolchain, from this tree's sources.
./configure
make -j world.opt

# 2. c++ocamlc.  The DDC harnesses use the build in cxx/build (and rebuild
#    it themselves if the sources changed).
cmake -S cxx -B cxx/build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C cxx/build c++ocamlc

# 3. The compiler DDC (a fresh bootstrap builds S1; tens of minutes).
ulimit -s unlimited          # the compiler's deepest recursions need it
make ddc                     # = bash cxx/harness/ddc.sh

# 4. The stdlib DDC, with the S2 the previous step printed.
make stdlib-ddc S2DIR=/tmp/ddc_s2.XXXXXX
```

`make ddc WD=<dir>` reuses S1 from an earlier bootstrap working directory
(the `S1=` path a previous run printed) instead of building it again.

## Reading the result

The compiler DDC ends with

```
DDC RESULT:  cmi same=N diff=0  |  cmo same=M diff=0
PASS: diverse and official compilers produce bit-identical bytecode
      (M/M .cmo -- ... bytecode compiler + ... native compiler -- + N .cmi, 0 diffs).
```

and exits 0.  Any `diff=` other than 0, or fewer `.cmo` than modules, is a
FAIL (exit 1) and lists the differing files; the REF and DIV directories it
prints hold both sides for inspection.  The stdlib DDC prints `PASS: diverse
S2 reproduces N .cmi + M .cmo of the stdlib bit-identically` (and the skipped
`-pp` artifacts).

A failure is not proof of a backdoor: a difference can also come from a
reproducibility problem or a bug in c++ocamlc.  A pass is proof that the
official compiler faithfully corresponds to its source, for everything
compared.

## Further reading

- David A. Wheeler, *Countering Trusting Trust through Diverse
  Double-Compiling* (2005), https://dwheeler.com/trusting-trust/
- Courant, Lepiller, Scherer, *Debootstrapping without Archeology:
  Stacked Implementations in Camlboot* (2022), https://arxiv.org/abs/2202.09231
  -- the first DDC of OCaml (version 4.07), which verified `boot/ocamlc`
  through a MiniML interpreter and a Scheme compiler.
- `cxx/harness/ddc.sh` and `cxx/harness/stdlib_ddc.sh`: each header documents
  the harness's pitfalls; `cxx/PORTING.md` describes c++ocamlc itself.
