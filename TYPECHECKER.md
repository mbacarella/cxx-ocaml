# Type checking in the C++ rewrite

**Read this before touching anything typing-related in `cxx/`.**

## The goal

c++ocamlc must match ocamlc's semantics exactly: accept what ocamlc accepts,
reject what ocamlc rejects with the same messages, infer the same types, and
produce the same .cmi and .cmo.  The only way there is a **faithful port**:
ocamlc's `typing/` first (done, stages 1-5), then the compiler parts that
consume its results (the .cmi writer, `lambda/`, `bytecomp/`), each verified
against ocamlc.  That is the parser, lambda and bytecode method again:
transcribe ocamlc's algorithms and check them against ocamlc.

**The typing/ port is c++ocamlc's type checker**: every unit is type-checked
before code generation, as in ocamlc, and the .cmi is the port's (stage 8).
The earlier C++ typers are deleted or scheduled for deletion; see "Legacy
code" below.

## Status (2026-09-26)

c++ocamlc type-checks every unit with the typing/ port before code
generation and rejects what ocamlc rejects (exit 2, nothing written); the
.cmi it writes is the port's, byte-identical to ocamlc's on the compiler's
interfaces.  Its error report is ocamlc's location line and the error's
constructor (`Error: Typecore.Expr_type_clash`) until Printtyp is ported
(stage 9); `-stop-after typing` type-checks only (and, as ocamlc, still
writes an .ml's inferred .cmi).  An internal failure of the port is
reported as one (`CPPCAML_TYPECHECK_DEBUG=1` adds the error's details).

Code generation is the port's too (stage 10): Translmod, Simplif, Bytegen
and Emitcode from the typed tree, as `driver/compile.ml` sequences them.
The .cmo is byte-identical to ocamlc.opt's on the compiler's sources
(145/145), the stdlib built as its Makefile builds it (72/72, the `-pp`
units included) and the stamp probes (6525/6527: the two .cmi leftovers
below reach the .cmo through the crc); without `-g` so far -- the debug
events section is being brought up.

Accept/reject parity with ocamlc (`-stop-after typing`, same flags):

| What | typing/ port | Deleted strict pass (for the record) |
|---|---|---|
| compiler corpus (139 .ml + 146 .mli, in /tmp/effid_ref) | **0 false rejects**, 0 false accepts | 70 false rejects |
| `false_accept.sh` (137 ill-typed one-liners) | **0 false accepts**, 137/137 error locations identical | 94 false accepts |
| `valid_reject.sh` (6531 ocamlc-valid stamp probes) | **0 false rejects** | 41 false rejects |
| `port_parity.sh` over stamp/core/false-accept probes + stdlib + testsuite/tests (8844 files) | 0 false rejects, 2 false accepts, 1353 identical rejections | -- |

The 2 false accepts: stdlib/camlinternalFormatBasics.ml (ocamlc dies with a
Consistbl inconsistency between the tree's cmis, not a type error) and
typing-objects-bugs/pr7284_bad.ml (a warning made an error by
`[@@warnerror "+8"]`; warnings are not ported).  Of the identical rejections, the
error line differs for 60 syntax errors (the C++ parser's messages) and
for files where ocamlc prints a warning or alert first (warnings are not
ported).  `port_check.sh` adapts
`false_accept.sh` / `valid_reject.sh` (`CPP=cxx/harness/port_check.sh`).

## Legacy code, and what is left of it

- **`typer.cpp` (a typedtree transcriber), the `c++type` tool, and the
  harnesses that scored them** (`reject_parity.sh`, `accept_parity.sh`,
  `typedtree_parity.sh`, `sig_parity.sh`, `expect_soundness.sh`,
  `gate_check.sh`, `TYPER-PARITY-ROADMAP.md`): **deleted** (stage 7).
- **`infer_check.cpp`'s strict pass (`structure_typecheck`)** and the
  helpers only it used: **deleted** (stage 7).
- **`infer_check.cpp` + `infer.cpp`** (an approximating algorithm-W
  pass), **`lambda.cpp`** (an AST-to-Lambda translator over its own IR),
  **`bytecode.cpp` / `cmo.cpp`** (bytegen/emitcode over that IR), the
  `c++lambda` / `c++instr` / `c++cmo` / `c++infer-test` tools and the
  harnesses that scored them (`lambda_parity.sh`, `instr_parity.sh`,
  `bootstrap_instr_parity.sh`, `qmark.sh`, `lambda_residue.sh`):
  **deleted** (stage 10).
- **`cmi.cpp`'s .cmi writer** (and `c++cmi`, `modsig.hpp`): still used by
  `-pack` (`link::pack` writes the packed .cmi with it) until Bytepackager
  and `Typemod.package_units` are ported.

## What counts as progress

Parity with ocamlc, stage by stage, each with an oracle:

1. **Accept/reject parity.**  Reached (see Status): 0 false rejects on the
   compiler, stdlib, testsuite and probes; the false accepts left are
   warnings made errors (warnings are not ported).
2. **Same messages:** Printtyp and the report functions; warnings.
3. **Same .cmi:** written from the port's signature (`Env.save_signature`).
4. **Same .cmo:** `lambda/` and `bytecomp/` ported onto the port's typed
   tree; then the legacy inference goes.

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

**Evaluation order.**  OCaml evaluates the arguments of a constructor, a
tuple, a list literal or a function application right to left, and a record
literal's fields right to left in the record type's *definition* order,
whatever order the source writes them in (`{b; a; c}` of `{a; b; c}` runs
c, b, a).  `let .. and ..` and `List.map` run left to right, and `let`
sequences explicitly (all verified with ocamlc).  Where those arguments have effects (fresh
type ids, levels, the trail, marks, warnings), the port evaluates them in
the same order, spelled out with locals.  Otherwise fresh ids, and with them
variable naming and set orders, drift from ocamlc.

**Loading cmis.**  `typing/cmi_format` decodes the marshal arena
(`marshal.hpp`, which already reconstructs sharing and cycles) straight
into `typing::Types`, value by value: that is `input_value`.  It doesn't go
through the deprecated `cmi.hpp` model.

**Stage 1 oracle.**  `cxx/harness/typing_dump.ml` (compiler-libs) prints a
structural dump of a cmi's `Types` graph: every constructor, every field,
sharing as first-visit numbering.  The C++ `c++typing-dump` prints the same
from the port.  The two must be identical on every .cmi in the tree
(`cxx/harness/typing_cmi_parity.sh`).

**Stages.**
1. **DONE:** `Ident`, `Path`, `Types` (+ trail), `Cmi_format` read.
   `typing_cmi_parity.sh`: all 572 tree cmis and 973 testsuite-built cmis
   are dump-identical.
2. **DONE (except the deviations below):** `Btype`, `Subst` (+ lazy),
   `Datarepr`, `Predef`, `Persistent_env`/`Load_path`, `Env`, `Mtype`
   strengthening.  `typing_env_parity.sh`: 15314/15314 `Env.find_*_by_name`
   queries over stdlib and the compiler's cmis, functor applications
   included, are dump-identical.  Deviations still open: no shapes; no
   warnings / alerts / usage tracking; `Env.check_functor_application`
   (Includemod) is not installed yet, so `Map.Make(Map.Make(String)).t`
   resolves where ocamlc reports it unbound; trunk's `Longident` inner
   locations come from the enclosing location (parser gap).
3. **DONE (except the deviations below):** `Ctype` (all of ctype.ml:
   levels and pools, `instance`/`copy`, generalization, `expand_head` and
   abbreviation memos, `unify` incl. GADT pattern mode, `filter_arrow` /
   `filter_method`, class signatures, `moregen`, `eqtype` / `equal`,
   `matches`, class type matching, `subtype` / `enlarge_type`, `nondep_*`,
   `normalize_type`, `arrow_spine`, `immediacy`), with `Errortrace` and a
   `Clflags` subset.  Split by area into `ctype*.cpp`.
   `typing_ctype_parity.sh`: 154607/154607 operations (instance,
   generalize, expand/full_expand, unify, moregeneral, equal, filter_arrow,
   subtype, matches, arrow_labels, nongen_vars, enlarge_type) on every
   value and type of stdlib and the compiler's cmis are dump-identical,
   error traces included.  Deviations still open: the Typemod / Includemod
   hooks `modtype_of_package` and `package_subtype` are not installed (so
   first-class-module types with different package paths can't be compared
   yet; ocamlc's are `assert false` until Typemod loads too); no `Printtyp`.
4a. **DONE (except the location gaps below):** `Parsetree`
   (`typing/parsetree.hpp`, field for field) built from the C++ parser's
   `ast::` by `parsetree::of_ast`.  `typing_parse_parity.sh` dumps every
   field of both trees (compiler-libs `Pparse` vs the port):
   2015/2017 oracle-parseable files of testsuite + stdlib + compiler
   sources identical; the 2 left are doc comments the C++ parser does not
   attach (`ocaml.doc` on a type declaration, typecore.ml / 
   includemod_errorprinter.ml).  Locations the parser does not record yet
   are built as `gap_loc()` and printed masked ("?") by both dumps:
   `Ptyp_poly` /
   `pcd_vars` / `Pext_decl` variable-name locations, `prf_loc`, `pof_loc`,
   `Rtag` label locations, `pvb_loc`, the name
   location of floating attributes, the `ppt_loc` of packages outside a
   core type, the variances of class and type-extension parameters, and
   every `*_loc_stack` except the innermost location of `Pexp_assert` (the
   only one Typecore reads).  They only affect error positions.
4b. **DONE:** `Typedtree` (the expression / pattern / core-type parts;
   the module, signature and class parts come with stage 5) and
   `Typetexp`.  `typing_typexp_parity.sh` runs `transl_type_scheme` on every
   `val` / `external` of stdlib, the compiler and testsuite interfaces (in
   Env.initial + open Stdlib + the unit's own cmi) and compares the typed
   core types, types included, or the error kind and location:
   4788/4791 identical, 0 different; the 3 left need Typemod's forward
   references (first-class modules, `M.(t)`).  Deviation: warnings are not
   ported (`Builtin_attributes.warning_scope` only runs its body).
   The parser now records Longident inner locations (parser.mly `ldot` /
   `lapply`), which Env reports lookup errors at.
4c. **DONE:** `Typecore` (all of typecore.ml: patterns, expressions,
   functions, applications, cases, let, binding operators, the toplevel
   entry points), `Parmatch` (with `Patterns`), `Value_rec_check` (with
   the part of lambda/typeopt.ml it uses) and CamlinternalFormat's
   `fmt_ebb_of_string` for format strings.  `typing_core_parity.sh` types
   the `let` / eval items of an implementation (`type_binding` /
   `type_expression`, then the delayed checks) and compares the whole type
   graph of every bound value (levels, scopes, links, abbreviation memos)
   or the error kind and location: over cxx/harness/core_probes (189
   feature and one-error-per-file probes), stdlib and testsuite/tests,
   1723 files identical (1614 items) + 274 identical errors, 0 different;
   57 files reach a Typemod / Typeclass forward (local opens, objects,
   first-class modules) and are UNSUPPORTED until stage 5.  Both stop at
   the first other structure item, so coverage grows with Typemod.
   Deviations: warnings are not emitted and warning-only computations are
   left out; the checks that only run for an enabled warning
   (Parmatch.check_unused) run under ocamlc's default warning set
   (`typing/warnings.hpp`), since `-w` and `[@warning]` are not
   interpreted yet.  Typing recovery (merlin) and the cmt partial trees are
   not ported.
5. **DONE (except the deviations below):** `Typedecl` (with
   `Typedecl_variance`, `_separability`, `_unboxed`, `_immediacy`,
   `Primitive`), `Includecore`, `Includeclass`, `Includemod`, `Mtype`
   (complete), `Shape` (what Typemod and Includemod create), `Typemod`
   (signatures, structures, functors, recursive modules, first-class
   modules, `with` constraints, compilation-unit entry points) and
   `Typeclass`.  Attribute payloads are carried as OCaml values
   (`parsetree_ovalue`), sharing locations the way ocamlc's parser does.
   `MODE=struct typing_core_parity.sh` types every implementation with
   `Typemod.type_structure` (then `Signature_names.simplify`,
   `check_nongen_signature` and the delayed checks) and compares the whole
   signature graph or the error kind and location: 1103 files identical
   (8262 items) + 1011 identical errors, 1 different, 0 crashes; 61 files
   are syntax errors for both.  The one DIFF
   (typing-modules-bugs/gatien_baron_20131019_ok.ml) is a stamp collision:
   the oracle process's fresh-ident counter happens to equal the stamps of
   hashtbl.cmi's labels, and the port's counter is elsewhere (see below).
   The stage-4c `core` mode is 1719 identical + 397 identical errors,
   0 different, and the struct mode also covers the `core_probes`
   (Typedecl `td_*` / `err_td_*` probes included).
   Deviations: no warnings, typing recovery or cmt (as 4c); the Includecore
   / Includemod error payloads are reduced to what Typemod branches on;
   `Env` has no shape map, so Typemod keeps its own table of module
   shapes; module-init values that OCaml creates at startup (`Ctype.none`,
   Parmatch's `omega`, `Shape.for_unnamed_functor_param`, Typeclass's
   `*undef*` idents) are created lazily, so absolute ident stamps differ
   from ocamlc's process (it matters for .cmi bytes, not for typing).
   C++ parser gaps found by the payload encoder: a `[@@foo: val y : int]`
   payload, `a, r.f <- v` inside a payload and `;; let exception E in ...`
   are rejected, and `functor ... -> sig end with ...` attaches the `with`
   differently.
6. **DONE:** c++ocamlc runs the port (`Typemod.type_implementation`
   / `type_interface` after `Compmisc.init_path` + `initial_env`) under
   `CPPCAML_TYPECHECK` or `-stop-after typing`, replacing the deprecated
   strict pass; see Status for the parity figures.  The error report is
   `typing/error_report` (Location's printer ported, including where
   ocamlc's printers move the location: Duplicate_label at `_none_`,
   Apply_non_function over the application).  When c++ocamlc finds its
   stdlib through `-I`, that directory is not added a second time (ocamlc
   would let the second copy's units shadow the opened Stdlib's).
7. **DONE:** `typer.cpp`, `c++type`, the strict pass and the harnesses and
   roadmap that scored them are deleted; `false_accept.sh` /
   `valid_reject.sh` run the port (`port_check.sh`).  The bootstrap harness
   passes the stdlib's and the compiler's real typing flags (`-principal`,
   `-no-alias-deps`, ...) and compiles to a dependency fixpoint across both
   libraries: its hand-written lists are link orders (obj.mli needs
   Int32, clflags.mli Profile, meta.mli Instruct), which only a type
   checker notices.  `cmx_format.mli` (native back end only) is left out.
8. **DONE (bytes: the remaining sharing classes below):** the .cmi comes
   from the port: `Env.save_signature` (`Btype.cleanup_abbrev_memo`,
   `Subst.for_saving` with `Make_local`, `Persistent_env.make_cmi` /
   `save_cmi`) and `Cmi_format.output_cmi`, whose encoder is the Reader's
   inverse.  Byte identity with ocamlc needed: the idents ocamlc creates
   while its modules initialize (Shape, Parmatch, Typeclass) created at
   startup in link order; the real structure shape passed to
   `Includemod.compunit`; `Env.hashcons_name`; OCaml's object identity
   kept for strings (`zborrow` borrows zone strings instead of copying,
   and a copy of "" has an identity), uids and record representations
   (identity tokens), arg labels, and what `input_value` shares (the
   Reader memoizes idents, paths and strings per marshaled block);
   positions, locations and filenames shared as the lexer shares them;
   the driver's `Compile_common.typecheck_intf` (the interface's
   self-inclusion check forces what ocamlc forces).  `cmi_port_parity.sh`:
   compiler interfaces **145/145 byte-identical** (cmx_format.mli is
   rejected by both); standalone .ml probes (`--standalone`, `-w -a`)
   **6529/6531 byte-identical**.  The two left: an attribute payload whose
   positions the payload encoder shares differently (xpay_b7), and a class
   probe where the port creates 46 more type nodes than ocamlc while typing
   (xrp_d34: a `Tlink` target keeps its creation id through saving).  More
   identity the writer needed along the way: `Some s` options
   (`OptStr.obj`; typetexp's `~name:"_"` is one static constant), methods'
   `Mprivate k`, typedecl's `Unboxed_integer` constants, attributes by
   pointer, and whole lists the Reader decoded (one Slice per marshaled
   list); Subst's `List.filter` of attributes always builds a new list;
   and the warning state (below) decides which checks run.  With the
   port's .cmi on disk the legacy translator needed a private copy of its
   own writer's view of the unit's .mli, until stage 10 removed it.  Type
   checking is on by default; DDC 139/139 .cmo + 216 .cmi and effid
   139/139 with it.
9. **Messages:** `Printtyp` (+ `Out_type`, `Oprint`, `Errortrace_report`),
   the `report_error` functions and `Location`'s reporting; then the
   warnings themselves (their state is ported: `-w` / `-warn-error` /
   `-alert` and `[@warning]` scopes drive `Warnings.is_active`) and the
   checks that emit them.  Oracle: ocamlc's stderr, byte for byte
   (the testsuite's expect outputs are a second oracle).
10. **Done (2026-09-26): code generation from the port.**  `lambda/`
   (Translcore, Translprim, Translattribute, Matching, Switch, Translmod,
   Translobj, Translclass, Value_rec_compiler, Simplif, Tmc, Printlambda
   on a Format engine port) onto a new faithful Lambda IR
   (`typing/lambda.hpp`), and `bytecomp/`'s Instruct, Bytegen, Printinstr
   and Emitcode (the legacy bytegen/emitcode were not faithful and were
   married to the old IR; `link.cpp` stays, reading .cmo files).
   `lambda_port_parity.sh` compares against ocamlc.opt byte for byte:
   `-drawlambda` / `-dlambda` / `-dinstr` (with and without `-g`) 0 DIFF
   on stamp probes 6527, compiler sources 145 and testsuite 777/779 (the
   2: ocamlc's warning lines); `DUMP=cmo` .cmo bytes as in the status
   above.  DDC 139/139 + 216 and stdlib DDC 65 + 71 with the new pipeline
   building S1; effid 139/139 byte-identical.  Identity the .cmo exposed:
   Persistent_env.read (the .mli's .cmi's crcs recorded), the import set's
   first string, one pos_fname per file, Const_immstring keeping its
   string, String.sub of a whole string, a unit's equal string literals
   merged (the reference ocamlc.opt is ocamlopt-built), a boxed-integer
   literal's box shared.  The driver also gained `-open`, `-pp` and
   ocamlc's output prefix (`-o stdlib__Arg.cmo` names the unit).
   Left: the `-g` debug events (live type ids, summary sharing), `-pack`
   (Bytepackager), and deleting `cmi.cpp` after it.
