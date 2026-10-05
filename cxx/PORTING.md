# The faithful port

**Read this before changing anything in `cxx/`.**

c++ocamlc must match ocamlc's semantics exactly: accept what ocamlc accepts,
reject what it rejects with the same messages and warnings, infer the same
types, and write the same .cmi, .cmo, .cmt, executables and libraries, byte
for byte.  The only way there is a **faithful port**: `cxx/src/typing/` holds
ports of ocamlc's `typing/`, `lambda/`, `bytecomp/`, `driver/` and the
`parsing/` / `utils/` / `file_formats/` modules they use, each checked
against ocamlc by an oracle harness (below).  The parser (`cxx/src/lexer.cpp`,
`parser.cpp`) is a recursive-descent transcription of menhir's grammar,
converted to the port's Parsetree by `typing/parsetree_of_ast.cpp`.
`cxx/INSTALL.md`: an opam switch whose `ocamlc` is c++ocamlc.

## Method

**Layout.**  Namespace `cppcaml::typing`.  One C++ module per OCaml module,
same name: `cxx/include/cppcaml/typing/<m>.hpp` + `cxx/src/typing/<m>.cpp`
(big ones split by area: `ctype_*.cpp`, `typecore_*.cpp`, `typemod_*.cpp`).
Functions keep ocamlc's names and structure; comments cite the OCaml source
(`ctype.ml unify3`).  Deviate only where C++ forces it, and say so at the
site.

**Representation.**  `type_expr` is `transient_expr {desc, level, scope,
id}`; `desc` points to an immutable `type_desc` node (one struct per
constructor, switch on `kind`), so the trail logs and restores descs exactly
as `types.ml` does.  `row_field`, `field_kind`, `commutable` and a
`Texpand`'s abbrev record keep their mutable cells.  The trail
(`Types.snapshot` / `backtrack`) is ported verbatim.

**Memory.**  OCaml's heap becomes zones (`typing/zone.hpp`): type nodes,
descs, paths, idents and persistent maps are never freed one by one; OCaml
lists inside immutable values become zone `Slice`s.  Where OCaml's garbage
is well delimited the port reclaims explicitly (the trail's zone, Matching's
and Parmatch's scratch zones, the copy scopes' Tsubst descs, the C++
parser's tree once converted).  c++ocamlopt types a unit in a zone of its
own and drops it once the unit is in Lambda (the OCaml GC reclaims the
typed tree then): `typing/evacuate.hpp` first copies out what the back end
reads, one copy per object so that the writers' sharing is kept, and
`CPPCAML_ZONE_PROTECT=1` makes the dropped zone fault on any later use (the
check for a missed reference).  Not when a later source file of the
invocation or `-pack` types again: the typer's caches live on in the zone
that filled them.  Hot lookups keep OCaml's `try ... with
Not_found` off the C++ exception path.  Lifetime changes are checked with an
AddressSanitizer build over the probes, the compiler and the testsuite.

**Evaluation order.**  OCaml evaluates the arguments of a constructor, a
tuple, a list literal or a function application right to left, and a record
literal's fields right to left in the record type's *definition* order,
whatever order the source writes them in (`{b; a; c}` of `{a; b; c}` runs
c, b, a).  `let .. and ..` and `List.map` run left to right, `let`
sequences explicitly, and a `match (a, b, c)` scrutinee's components run
left to right (only a tuple *value* is right to left).  Where those
arguments have effects (fresh type ids, levels, the trail, marks, warnings,
Env lookups -- forcing a module's components copies its signature), the
port evaluates them in the same order, spelled out with locals: C++ leaves
the order of `f(g(), h())` and of `==`'s operands unspecified.  Otherwise
fresh ids, and with them variable names, set orders and the .cmt/-g bytes,
drift from ocamlc.  (Found this way: `compare_type_path` expands its second
path first.)

**Object identity.**  Marshal writes sharing, so the port keeps OCaml's
physical identity wherever ocamlc's graph shares: `input_value`'s sharing
(the .cmi Reader memoizes idents, paths, strings, lists and abbrev records
per marshaled block); strings (`zborrow` borrows zone strings, a copied ""
has its own identity, a unit's equal string literals are one object via
`OCAML_LIT`); identity tokens for declarations' `Some` blocks (`SomeToken`),
uids, record representations and shapes' uid options; location records (one
per parser location, kept by the typer's copies; positions and filenames
shared as the lexer shares them); a boxed-integer literal's box; attributes
by pointer.  Idents and types ocamlc creates while its modules initialize
(Shape, Parmatch, Typeclass, `Env.initial`) are created at startup in link
order, so stamps and ids line up.

**Loading and writing .cmi.**  `typing/cmi_format.cpp` decodes the marshal
arena (`cmi_marshal.hpp`: a lean node graph) straight into `typing::Types`:
that is `input_value`.  `cmi_writer.hpp` is its inverse, shared by the .cmi,
the .cmt and the `-g` debug events.  `cmi_image.{hpp,cpp}` caches a decoded
.cmi as an image of its zone objects mapped back at its recorded address
(`CPPCAML_CMI_CACHE*`; the gates run with it off, cold and warm).

## The driver

c++ocamlc's command line is ocamlc's: `driver/main_args.ml`'s bytecode
option table, `Config.print_config` and `Warnings.descriptions` are
generated from ocamlc's own modules by `cxx/harness/gen_driver_tables.sh`;
the actions are ported in `typing/main_args.cpp`, stdlib Arg in `arg.cpp`,
Compenv (deferred actions, output prefixes, OCAMLPARAM, the configuration
file) in `compenv.cpp`, Maindriver's sequence in `tools/cppocamlc_main.cpp`.
The linker and librarian are ports of Bytelink, Symtable, Bytelibrarian,
Dll, Ccomp, Linkdeps and Binutils (launcher header, every section,
`-custom` / `-output-obj` / `-output-complete-*` through the C toolchain);
`-pack` is Bytepackager's; binary ASTs and `-ppx` are Pparse's AST half with
Ast_mapper's context and Ast_invariants.

**Driver options.**
- Implemented (their effects as ocamlc's): everything the typer, translators,
  Emitcode, Bytepackager, the linker and the librarian read; `-a`, `-i`,
  `-i-variance`, `-short-paths`, `-bin-annot`, `-config`, `-dlambda` /
  `-drawlambda` / `-dinstr` / `-dparsetree`, `-dno-locations`, `-dump-into-file`,
  `-for-pack`, `-g`, `-H`, `-keywords`, `-open`, `-pack`, `-pp`, `-ppx`,
  `-stop-after`, `-w` / `-warn-error` / `-alert`, `-color` / `-error-style`,
  the runtime-selection flags, `-args` / `-args0`, OCAMLPARAM.
- Accepted, no effect to reproduce: `-bin-annot-occurrences` (the .cmt's
  cmt_ident_occurrences stay []), `-dno-canonical-ids`,
  `-dparsetree-loc-ghost-invariants`, `-safe-string`.
- Refused where their effect would take place (`option -X is not supported
  yet`, exit 2): `-annot` / `-dtypes`, `-dsource`, `-dtypedtree`, `-dshape`,
  `-dmatchcomp`, `-dcanonical-ids`, `-compat-32`, `-dtimings` / `-dprofile`,
  `-depend`; `-dparsetree` / `-dsource` of a binary or rewritten AST.
- c++ocamlc's own: `-stdlib <dir>` (the bootstrap harnesses build against a
  stdlib being built).  Development switches: `CPPCAML_PROFILE=1` (phase
  timers, RSS; `bench.sh PHASES=1`), `CPPCAML_TYPECHECK_DEBUG=1` /
  `CPPCAML_REPORT_DEBUG=1` (details of an internal failure),
  `CPPCAML_NO_FASTEXIT=1` (normal teardown, for leak checkers).

**Compressed Marshal (zstd).**  An OCaml configured with zstd writes the
.cmi header, .cmt and the .cmo debug/hint sections with
`Compression.output_value`; c++ocamlc does the same when its installation's
runtime has zstd (`gen_driver_tables.sh` asks; `CPPCAML_ZSTD` links libzstd,
which must be the installation's version or the compressed bytes differ).
It reads compressed values whatever its configuration.
`cxx/harness/zstd_reference.sh` builds a zstd-configured reference.

**`-bin-annot`.**  `typing/cmt_format.cpp` ports save_cmt: `clear_env`'s
Tast_mapper rebuild (called in the mapper's evaluation order),
`index_declarations` (Tast_iterator's order into a Uid.Tbl bucketed by a
`caml_hash` port), comments, argv, load path, digests, imports, `Uid.Deps`,
and the shape reduced by `Shape_reduce.local_reduce`.  A failed
implementation writes the partial .cmt from Cmt_format's saved types.

## The native compiler

`c++ocamlopt` is the same driver built with `CPPCAML_OCAMLOPT`
(Optmaindriver / Optcompile): `native_code` set before the arguments,
ocamlopt's option table (`optmain_args_table.inc`, generated like the
bytecode one) with Default.Optmain's actions and Arch's `-fPIC`/`-fno-PIC`,
the native Clflags (dumps, inlining parameters through `arg_helper.hpp`,
the -O2/-O3/classic argument sets), and `Translmod.transl_store_implementation`.
Oracle: `NATIVE=1 lambda_port_parity.sh` (`-drawlambda` / `-dlambda` under
`-stop-after lambda`, against ocamlopt.opt): 6550/6550 probes.

The Closure middle end is ported (`closure.cpp`, `compilenv.cpp`, the IR
in `clambda.hpp`, the .cmx reader in `cmi_format.cpp`): `NATIVE=1
DUMP=dclambda lambda_port_parity.sh` compares `-dclambda` (inlining across
units through the stdlib's .cmx included) -- 6550/6550 probes (also with
`-g`, `-O3`, `-inline 200 -unsafe`), 787/787 compilable testsuite files,
208/208 compiler sources.  `c++cmxinfo` prints a .cmx as ocamlobjinfo
does (262/262 of the tree's).  Closure consumes fresh identifiers, raise
counts and constant labels in ocamlopt's evaluation order: arguments right
to left, except a tuple matched on at once (`match (a, b) with`), whose
components are let-bound left to right.

Cmmgen is ported (`cmm.cpp`, `printcmm.cpp`, `cmm_helpers.cpp` with
Cmmgen_state and Strmatch, `cmmgen.cpp`; amd64's Arch constants):
`DUMP=dcmm` -- 6550/6550 probes (also `-g`, `-unsafe -inline 200`, `-g
-compact`), 787/787 testsuite files, 272/272 compiler sources.  The Cmm
switch stores are AVL maps on `compare_key` as OCaml's (the exit-sharing
key's comparison is not an order, so lookups depend on the tree).

Selection is ported (`mach.cpp`: Reg, Mach, amd64 Arch/Proc, Printmach;
`selection.cpp`: Selectgen with amd64's selector folded in, Polling,
Dataflow): `DUMP=dsel` -- 6550/6550 probes (also `-g`, `-unsafe -inline
200`, `-nodynlink`, `-g -O3`), 787/787 testsuite files, 208/208 compiler
sources; the `[@poll error]` reports identical.  Register stamps follow
ocamlopt's: Proc's 29 hard registers first (so a function's first
pseudo-register is 29), then every `Reg.create`/`at_location` in
Selectgen's order.

The Mach passes are ported (`mach_passes.cpp`: Comballoc, CSE,
Liveness, Deadcode, Spill, Split, Interf, Coloring, Reload, with the
regalloc loop in the driver): `DUMP=dcombine` / `dcse` / `dlive` /
`dspill` / `dsplit` / `dinterf` / `dprefer` / `dalloc` / `dreload` --
6550/6550 probes each (and `-dreload` with `-g`, `-unsafe -inline 200`,
`-compact`), 208/208 compiler sources (`dreload`, `dlive`, `dinterf`),
787/787 testsuite files.  Record updates and constructors evaluate their
fields right to left; CSE and Reload create registers inside them.

Linearize (with amd64's Stackframe analysis) is ported
(`linearize.cpp`): `DUMP=dlinear FLAGS="-stop-after scheduling"` --
6550/6550 probes, 208/208 compiler sources, 789/789 testsuite files.
(Without `-stop-after scheduling` ocamlopt emits each function before
linearizing the next, and Emit allocates labels: `-dlinear` matches only
once Emit is ported.)

Emit is ported (`emit.cpp`: amd64's emit.mlp, Emitaux's frame tables
and debug info, the X86 DSL and GAS printer; `hashtbl.hpp`'s
`hash_value` for the frame tables' Hashtbl iteration orders):
`DUMP=S` compares the `.s` files -- 6550/6550 probes (also `-g`,
`-unsafe -inline 200`, `-compact`, `-g -O3`, `-nodynlink`,
`-function-sections`), 208/208 compiler sources (and `-g`), 787/787
testsuite files (and `-g`); `-dlinear` now matches without `-stop-after`.
c++ocamlopt writes the `.s` (with `-S`) and assembles the `.o` with `as`.

The .cmx writer is ported (`cmi_format.cpp`'s `write_unit_info`:
Compilenv.save_unit_info's output_value with ocamlopt's sharing):
`DUMP=cmx` -- 6553/6553 probes (also `-g`), 208/208 compiler sources
(and `-g`), 787/787 testsuite files.  Sharing follows ocamlopt's heap: values ocamlopt allocates
statically (the literals of an inlining-built compiler, such as
primitive.ml's `""` native name, Pignore's `0`, `Pphyscomp`'s comparisons)
are singletons, constants and primitives carry identity tokens, and
debuginfo lists keep their shared tails.  `c++ocamlopt -c` now writes the
`.s`, `.o` and `.cmx`.

Asmgen (`asmgen.cpp`: compile_phrases, compile_unit and the assembler
command), Asmlink (`asmlink.cpp`: the startup module -- Cmm_helpers'
generic functions, `caml_program`, the global, frame and segment tables,
the marshaled `caml_globals_map` -- and the C link) and Asmlibrarian
(`asmlibrarian.cpp`, the .cmxa reader/writer in `cmi_format.cpp`) are
ported.  `native_link_parity.sh` compares the kept startup assembly
(`-dstartup`), a `-d` dump of the link and the executable's bytes:
6531/6531 linkable probes (`-dcmm`), and with `MODE=onestep` (compile and
link in one run) 6531/6531 (`-dlinear`, and `-g`), 697/697 testsuite
files.
`native_link_scenarios.sh`: libraries (`-a`, `-linkall`, C options),
`-output-obj`, `-output-complete-obj`, `-shared` (Asmlink.link_shared,
the marshaled plugin header), `-pack` (Asmpackager, with
Translmod.transl_store_package: nested packs, a packed .mli, `-S`, `-g`,
one-step) and the link and pack errors -- 30/30 identical.  `c++ocamlopt -a` rebuilds the tree's stdlib.cmxa and
stdlib.a identically, and `c++cmxinfo --roundtrip` reads and rewrites
the tree's 11 .cmxa files identically.  A link-only run first creates the
idents ocamlopt's modules create while they initialize (their stamps
reach `-dcmm`).

`native_self_build.sh` runs the tree's own build commands for
ocamlopt.opt (`make -n -W utils/misc.ml ocamlopt.opt`: 260 compiles with
`-g -absname -bin-annot -function-sections`, ocamlcommon.cmxa and
ocamloptcomp.cmxa, the link) once with ocamlopt.opt and once with
c++ocamlopt: all 845 artifacts (.cmx, .o, .cmi, .cmxa, .a) and the
ocamlopt.opt executable are byte-identical.

The opam switch (`cxx/INSTALL.md`) installs c++ocamlopt as `ocamlopt`:
dune, 30 packages and mpg123 build, and every artifact is identical to the
stock compilers' (only .cmt sharing differs).  `parse_dir_parity.sh`
compares parse trees over any source tree (the switch's package sources
found parser divergences the testsuite had not).

Flambda (branch `cxx-flambda`, a tree configured with `--enable-flambda`):
c++ocamlopt applies Optcompile.flambda's settings and
Translmod.transl_implementation_flambda, then the flambda middle end and
back end: closure conversion, every pass of every round
(`flambda_middle_end.cpp`), Build_export_info, Flambda_to_clambda and
Un_anf, then Cmmgen and emit as for Closure, and the .cmx with its
Export_info.  The passes run in transient zones: the program they leave
is copied out (`flambda_evacuate.cpp`) and the zone dropped, and
Inline_and_simplify promotes what is live between the program's
definitions (a nursery), as the OCaml GC reclaims the rest.  Identifiers,
debuginfo lists, import caches are in the permanent zone.  The .cmx is
written with ocamlopt's sharing: the port's objects are ocamlopt's (Map
and Set nodes included); a block the port keeps as a struct carries its
identity (Export_info approximations, consts), and the literal blocks of
an OCaml source file are one per file and value (FLAMBDA_INT_LITERAL,
CLAMBDA_PRIM_LITERAL, OCAML_LIT ...).  -pack and -inlining-report (its
.inlining.org files: inlining_report_parity.sh) are ported; not ported:
-clambda-checks and -dflambda-invariants (Flambda_invariants after every pass) are ported.

Gates (NATIVE=1, lambda_port_parity.sh): DUMP=drawflambda,
dflambda-verbose (the program before each pass), drawclambda, dclambda,
dcmm, S, o and cmx -- 6553/6553 probes at default, -Oclassic, -O2,
-g -O3 and -unsafe, 787/787 testsuite files and 272/272 compiler sources
(also at -O3), but for two .cmx whose .cmi CRC differs (private row
types: their .cmo differ too).  CPPCAML_FLAMBDA_EVACUATE=always with
CPPCAML_ZONE_PROTECT evacuates at every point and faults on a dangling
use.  Variables, symbols, export ids and backend idents are created in
ocamlopt's order (OCaml evaluates a constructor's arguments right to
left): their stamps are in the dumps and the .cmx.

## Oracles and gates

Every harness compares against the tree's `ocamlc.opt` (the stdlib DDC
against the bytecode `./ocamlc`, whose -g output differs from the native
one's).  Build `cxx/build-release` first (`make -C cxx`; `make -C cxx
debug` for `cxx/build`; settings in `cxx/Makefile`); never edit sources or
run two builds while a chain runs; cap memory (`ulimit -v`).  Results as of the
2026-09-28 trunk catch-up:

| Harness | Compares | Result |
|---|---|---|
| `error_parity.sh` | stderr + exit, `error_probes/` + false-accept probes | 335/335 |
| `error_parity_multi.sh` | missing/stale .cmi, inconsistent assumptions, -pack | 9/9 |
| `warning_parity.sh` | stderr + exit, `warning_probes/` (default, `W="-w +a"`) | 66/66 |
| `intf_parity.sh ERR=1` | `-i` (stdout, stderr, exit) on `stamp_probes/` | 6554/6554 |
| `lambda_port_parity.sh DUMP=cmo` | .cmo bytes (and `FLAGS=-g`) on probes | 6553/6553 |
| `cmi_port_parity.sh` | compiler .mli / `--standalone` probes | 146/146, 6557/6557 |
| `cmt_parity.sh` | .cmt bytes: probes, testsuite | 6557/6557, 1816/1816 |
| `cmt_parity.sh --stdlib / --compiler` | .cmt bytes | 72/72, 146/146 |
| `stdlib_cmo_parity.sh` | the stdlib built as its Makefile does (and `G=-g`) | 72/72 |
| `pack_parity.sh` | -pack scenarios (and `BIN_ANNOT=1`) | 24/24 |
| `cli_parity.sh` | command lines: stdout, stderr, exit, files | 153/153 |
| `link_parity.sh` | executables, .cma, C outputs, link errors | 47 + 6 nondet |
| `exec_parity.sh` | testsuite programs' output | 734/734 |
| `ppx_parity.sh` | `-ppx` and binary-AST inputs | 14 + 14 same errors |
| `typing_parse_parity.sh` | Parsetree dumps (compiler-libs vs port) | 2040/2040 |
| `dune_parity.sh` | dune builds, artifact by artifact | 0 DIFF (73 .cmt sharing) |
| `effid.sh` | the compiler's .cmo, compiled by c++ocamlc | 140/140 bytes |
| `ddc.sh` + `stdlib_ddc.sh` | diverse double-compiling (`cxx/DDC.md`) | PASS: 271 .cmo + 355 .cmi (bytecode and native compiler sources) |
| `testsuite_delta.sh` | ocamltest with c++ocamlc as ocamlc | the delta (below) |

Also: `false_accept.sh` / `valid_reject.sh` (through `port_check.sh`),
`typing_cmi_parity.sh` / `typing_env_parity.sh` / `typing_ctype_parity.sh` /
`typing_core_parity.sh` / `typing_typexp_parity.sh` (the stage 1-5
structural oracles, via `typing_dump.ml` and `c++typing-dump`),
`bench.sh` (below).

## Known gaps

`testsuite_delta.sh`'s regressions are all of the kinds below: the
typing-recovery tests (`-typing-recovery` is merlin's), the refused options
(`-annot`, `-dtypedtree`, `-dsource`, `-depend`, `-compat-32`) and the
menhir error points.

- Three syntax errors that menhir's LALR automaton detects at another point
  than the recursive-descent parser (generated-parse-errors, a singleton
  labeled tuple type, arrow_ambiguity): accepted, `error_parity.sh`'s KNOWN
  list.  `typing-misc/conjunctive_types.ml` overflows the stack in both.
- Parser locations not recorded yet, built as `gap_loc()` and masked ("?")
  by the Parsetree dumps: `Ptyp_poly` / `pcd_vars` / `Pext_decl` variable
  names, `prf_loc`, `pof_loc`, `Rtag` labels, `pvb_loc`, floating
  attributes' names, `ppt_loc` of packages outside a core type, class and
  type-extension parameter variances, and every `*_loc_stack` except
  `Pexp_assert`'s.  They reach error positions and the AST a `-ppx`
  rewriter sees, not the compiled output.
- Not ported: typing recovery (`-typing-recovery`), the
  `-bin-annot-occurrences` index, Printast / Pprintast / Printtyped as
  printers (hence the refused `-d*` options), the native back end.

## Performance

`cxx/harness/bench.sh` times c++ocamlc against ocamlc.opt unit by unit
(`BASELINE=cxx/harness/bench_baseline.tsv`, `MAXRATIO=1.0` fails a slower
corpus).  Baseline 2026-09-28 (release build with mimalloc, warm .cmi image
cache): startup 0.66x, small 0.63x, compiler 0.57x, stdlib 0.65x of
ocamlc.opt's time; peak RSS 1.5-2.7x.  The release build must link mimalloc
(vendored in `cxx/vendor/mimalloc`, on unless `MIMALLOC=no`): glibc's malloc
costs ~25%.

## Following trunk

The port tracks upstream trunk (the next release, 5.6).  A catch-up merges
upstream, then ports every commit that touches `parsing/`, `typing/`,
`lambda/`, `bytecomp/`, `utils/`, `file_formats/` or `driver/` (`git diff
<old-base> <new-base> -- ...`, reviewed against the C++ sources).  Then:
- rebuild the tree (`make world.opt`: the oracles) and delete the harness
  caches built by the old tree (`/tmp/typing_cmi_parity`,
  `/tmp/cmt_parity_tools`, `/tmp/effid_ref`, `/tmp/exec_oracle_cache`,
  `/tmp/dune_parity_stage`, ...);
- regenerate the tables: `gen_driver_tables.sh` writes its .inc files in
  place; `gen_builtin_prims.sh` prints to stdout (redirect it to
  `include/cppcaml/builtin_prims.hpp`);
- follow the Makefile's `COMPILERLIBS` in `ocamlc_bootstrap.sh`'s module
  lists (DDC, effid);
- new and modified upstream tests: file-based ones are covered by
  `exec_parity.sh` and `testsuite_delta.sh`; expect tests are split into
  standalone probes (one per phrase, after the phrases ocamlc accepted) and
  run through every gate; the new tests' probes join `stamp_probes/`
  (accepted) or `error_probes/` (rejected);
- run every gate above, and the opam switch (`cxx/INSTALL.md`).

## Port history

The port landed in stages, which source comments cite:
1. `Ident`, `Path`, `Types` (+ trail), `Cmi_format` read.
2. `Btype`, `Subst`, `Datarepr`, `Predef`, `Persistent_env`, `Env`,
   `Mtype` strengthening.
3. `Ctype` (all of ctype.ml) with `Errortrace`.
4. The Parsetree from the C++ parser (4a); `Typedtree`, `Typetexp` (4b);
   `Typecore`, `Parmatch`, `Value_rec_check` (4c).
5. `Typedecl` and its satellites, `Includecore`, `Includeclass`,
   `Includemod`, `Mtype`, `Shape`, `Typemod`, `Typeclass`.
6. c++ocamlc type-checks with the port.
7. The approximating legacy typers deleted.
8. The .cmi written from the port (`Env.save_signature`).
9. Messages: `Format_doc`, `Oprint`, `Out_type`, `Printtyp`, `-i` (9a);
   every `report_error` (9b); warnings and alerts (9c).
10. Code generation: `lambda/` onto a faithful Lambda IR, Bytegen,
    Emitcode, Bytepackager; then the linker, the librarian, `-ppx`,
    `-bin-annot` and the driver's option table.
