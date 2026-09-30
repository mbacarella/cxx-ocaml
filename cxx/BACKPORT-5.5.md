# Backport of the C++ port to OCaml 5.5.1 (branch cxx-5.5)

cxx/ on this branch is the port of upstream trunk b9dac8cc84 (cxx-trunk's
base), merged into 5.5.1.  Each upstream file below differs between 5.5.1
and b9dac8cc84; its C++ counterpart must be brought back to 5.5.1's
version (`git diff 5.5.1 b9dac8cc84 -- <file>`, reversed).  The oracle is
this tree's own ocamlc.opt / ocamlopt.opt (5.5.1); the gates are the same
as cxx-trunk's (cxx/PORTING.md).

Shared fixes go to cxx-trunk first and are cherry-picked here, so the two
branches' cxx/ differ only by the backport.

Status: [ ] to do, [x] done, [-] no C++ counterpart / no effect.
Lines = changed lines in the upstream diff.

- [x] typing/typecore.ml (2163)
- [x] typing/ctype.ml (1387)
- [x] typing/typemod.ml (965)
- [x] typing/typedecl.ml (774)
- [x] typing/env.ml (532)
- [ ] utils/utf8_lexeme.ml (321)
- [x] typing/typeclass.ml (321)
- [x] bytecomp/bytegen.ml (266)
- [ ] utils/misc.ml (258)
- [x] typing/out_type.ml (218)
- [x] typing/types.ml (194)
- [x] typing/btype.ml (144)
- [x] typing/typeopt.ml (136)
- [x] typing/errortrace_report.ml (131)
- [x] typing/typetexp.ml (130)
- [x] lambda/value_rec_compiler.ml (129)
- [ ] typing/rawprinttyp.ml (122)
- [x] parsing/parser.mly (118)
- [ ] typing/typing_recovery_state.ml (111)
- [ ] typing/typing_recovery.ml (105)
- [x] lambda/translprim.ml (84)
- [x] lambda/translcore.ml (80)
- [x] asmcomp/cmmgen.ml (71)
- [x] bytecomp/emitcode.ml (70)
- [x] asmcomp/cmm_helpers.ml (68)
- [ ] parsing/lexer.mll (65)
- [ ] parsing/pprintast.ml (55)
- [x] driver/compile_common.ml (54)
- [ ] asmcomp/arm64/arch.ml (52)
- [x] parsing/unit_info.ml (47)
- [ ] lambda/translmod.ml (46)
- [x] bytecomp/bytepackager.ml (45)
- [x] typing/value_rec_check.ml (39)
- [ ] asmcomp/power/selection.ml (38)
- [ ] asmcomp/power/proc.ml (38)
- [ ] parsing/printast.ml (36)
- [x] bytecomp/instruct.ml (36)
- [ ] asmcomp/s390x/selection.ml (36)
- [ ] middle_end/flambda/inlining_decision.ml (35)
- [x] typing/errortrace.ml (33)
- [x] asmcomp/emitaux.ml (33)
- [x] typing/typedtree.ml (32)
- [ ] parsing/ast_mapper.ml (30)
- [ ] asmcomp/thread_sanitizer.ml (30)
- [ ] asmcomp/riscv/proc.ml (30)
- [ ] typing/printtyped.ml (29)
- [ ] parsing/ast_helper.ml (27)
- [ ] asmcomp/arm64/selection.ml (27)
- [ ] asmcomp/riscv/selection.ml (26)
- [x] asmcomp/asmlink.ml (25)
- [ ] typing/primitive.ml (23)
- [ ] typing/untypeast.ml (22)
- [x] typing/tast_mapper.ml (22)
- [x] file_formats/cmt_format.ml (22)
- [x] bytecomp/bytelink.ml (22)
- [ ] parsing/ast_iterator.ml (21)
- [x] typing/tast_iterator.ml (20)
- [x] typing/subst.ml (20)
- [x] asmcomp/amd64/selection.ml (19)
- [x] typing/printpat.ml (18)
- [x] typing/includecore.ml (18)
- [ ] typing/gprinttyp.ml (18)
- [ ] parsing/depend.ml (18)
- [ ] middle_end/flambda/inline_and_simplify_aux.ml (17)
- [x] lambda/lambda.ml (17)
- [ ] asmcomp/power/arch.ml (17)
- [x] bytecomp/printinstr.ml (16)
- [x] typing/typedecl_separability.ml (15)
- [x] typing/predef.ml (13)
- [ ] driver/main_args.ml (13)
- [ ] middle_end/internal_variable_names.ml (12)
- [x] asmcomp/x86_proc.ml (12)
- [x] typing/includemod.ml (11)
- [x] lambda/printlambda.ml (10)
- [ ] typing/persistent_env.ml (9)
- [ ] utils/warnings.ml (8)
- [ ] asmcomp/s390x/arch.ml (8)
- [x] asmcomp/mach.ml (8)
- [x] asmcomp/amd64/arch.ml (8)
- [ ] typing/path.ml (7)
- [x] asmcomp/selectgen.ml (7)
- [ ] asmcomp/riscv/stackframe.ml (7)
- [x] asmcomp/polling.ml (7)
- [x] asmcomp/interf.ml (7)
- [ ] typing/typedecl_variance.ml (6)
- [x] typing/oprint.ml (6)
- [ ] parsing/location.ml (6)
- [x] lambda/matching.ml (6)
- [ ] typing/ident.ml (5)
- [x] lambda/tmc.ml (5)
- [ ] lambda/switch.ml (5)
- [x] bytecomp/bytelibrarian.ml (5)
- [ ] asmcomp/x86_masm.ml (5)
- [ ] asmcomp/x86_gas.ml (5)
- [x] asmcomp/x86_dsl.ml (5)
- [ ] asmcomp/arm64/proc.ml (5)
- [ ] utils/load_path.ml (4)
- [x] middle_end/convert_primitives.ml (4)
- [ ] lambda/translclass.ml (4)
- [ ] bytecomp/dll.ml (4)
- [ ] asmcomp/power/stackframe.ml (4)
- [x] middle_end/semantics_of_primitives.ml (3)
- [ ] bytecomp/bytesections.ml (3)
- [ ] asmcomp/riscv/arch.ml (3)
- [ ] utils/clflags.ml (2)
- [x] typing/parmatch.ml (2)
- [ ] typing/datarepr.ml (2)
- [x] middle_end/printclambda_primitives.ml (2)
- [x] middle_end/clambda_primitives.ml (2)
- [ ] lambda/translobj.ml (2)
- [ ] driver/compmisc.ml (2)
- [ ] asmcomp/s390x/CSE.ml (2)
- [ ] asmcomp/riscv/CSE.ml (2)
- [x] asmcomp/printmach.ml (2)
- [ ] asmcomp/power/CSE.ml (2)
- [ ] asmcomp/asmgen.ml (2)
- [x] asmcomp/amd64/CSE.ml (2)
- [ ] utils/profile.ml (1)
- [ ] driver/compenv.ml (1)
- [x] asmcomp/schedgen.ml (1)
- [x] asmcomp/printcmm.ml (1)
- [ ] asmcomp/power/scheduling.ml (1)
- [x] asmcomp/CSEgen.ml (1)
- [x] asmcomp/cmm.ml (1)

Notes (open items noticed while porting):
- ctype: PatternEnv::save/reset and arrow_spine/arrow_labels are trunk
  additions kept for their callers in typecore; remove with typecore.
- enforce_current_level: 5.5.1's is unify_var env (newvar ()) ty (restored).
- set_object_name takes an Ident (5.5.1): typeclass callers.
- typedecl: try_expand_once_gen_nolink belongs to trunk's well-foundedness
  rewrite (#11648); typedecl gets 5.5.1's check.
- errortrace: Kind_differ and the three First_class_module constructors are
  trunk-only; the C++ Obj/FirstClassModule kinds still have them.
- typedecl: 5.5.1's check_well_founded (TypeMap/parents, deep type_iterators
  check, check_well_founded_manifest), no Subst copies in transl_type_decl,
  Unbound_type_var_ext / Val_in_structure back; typemod check_type_decl has
  no well-foundedness check and check_recmod_typedecls takes abs_env.
  typedecl.ml: rest of the diff (native repr, with_constraint, approx) TODO.
- parser: holes reverted (d107b293f7), external aliases and `{f x with}`
  removed, `with module type T = S -> S` stops at the arrow; lexer `~_:`
  stays (5.5.1 lexes it the same).
- parse tree / binary AST: value_description with pval_prim, no
  Psig_primitive / Pstr_val; typed tree: val_prim, c_cont Ident option.
