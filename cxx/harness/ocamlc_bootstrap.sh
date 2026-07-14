#!/usr/bin/env bash
# THE COMPILER BOOTSTRAP: build the reference OCaml compiler's own OCaml sources
# (ocamlcommon + ocamlbytecomp = the whole bytecode compiler front+back end) with
# c++ocamlc against our ENTIRE self-built (all-ours) stdlib, link the result with
# c++link + the runtime into a working bytecode `ocamlc`, then use THAT ocamlc to
# compile a program -- proving the c++-compiled compiler runs.
#
# c++ocamlc is a C++ program and cannot literally compile itself; the achievable
# bootstrap is: c++ocamlc + all-ours stdlib  --->  a working OCaml `ocamlc`.
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
source cxx/harness/_require_fresh.sh; require_fresh c++ocamlc c++link
CPP=$ROOT/cxx/build/c++ocamlc
LINK=$ROOT/cxx/build/c++link
RUN=$ROOT/runtime/ocamlrun
STD=$ROOT/stdlib

WD=$(mktemp -d) || exit 1
KEEP=${KEEP:-0}
trap '[ "$KEEP" = 1 ] || rm -rf "$WD"' EXIT
echo "WD=$WD"

gname() { case "$1" in camlinternal*|stdlib) echo "$1";;
  *) cap="$(tr '[:lower:]' '[:upper:]' <<< ${1:0:1})${1:1}"; echo "stdlib__$cap";; esac; }
flags() { case "$1" in camlinternalFormatBasics|stdlib) echo "-nopervasives";; *) echo "";; esac; }
needs_awk() { case "$1" in stdlib|*Labels) return 0;; *) return 1;; esac; }

STDORDER="camlinternalFormatBasics stdlib either sys obj type atomic mutex condition \
semaphore camlinternalLazy lazy seq option pair result bool char uchar list int array \
iarray bytes string unit marshal float int32 int64 nativeint lexing parsing repr set \
map stack queue buffer camlinternalFormat printf arg printexc domain fun gc in_channel \
out_channel digest bigarray random hashtbl weak scanf callback camlinternalOO oo \
dynarray format camlinternalMod pqueue ephemeron filename complex effect \
arrayLabels bytesLabels listLabels stringLabels moreLabels stdLabels"

# ---- 1. all-ours stdlib (.cmi then .cmo) -----------------------------------
for phase in cmi cmo; do
  for m in $STDORDER; do
    s=$(gname "$m")
    for ext in mli ml; do
      [ -f "$STD/$m.$ext" ] || continue
      if needs_awk "$m"; then awk -f "$STD/expand_module_aliases.awk" "$STD/$m.$ext" > "$WD/$s.$ext"
      else cp "$STD/$m.$ext" "$WD/$s.$ext"; fi
    done
    if [ "$phase" = cmi ]; then [ -f "$WD/$s.mli" ] || continue; src="$WD/$s.mli"
    else src="$WD/$s.ml"; [ -f "$src" ] || continue; fi
    ( cd "$WD" && "$CPP" -c $(flags "$m") -stdlib "$WD" -I "$WD" "$(basename "$src")" ) 2>"$WD/err" \
      || { echo "FAIL: stdlib $(basename "$src")"; sed 's/^/  /' "$WD/err"; exit 1; }
  done
done
cp "$STD/std_exit.ml" "$WD/"; ( cd "$WD" && "$CPP" -c -stdlib "$WD" -I "$WD" std_exit.ml ) 2>/dev/null
echo "stdlib: built"

# ---- 2. compiler sources, in dependency order ------------------------------
# Authoritative module order (ocamlcommon then ocamlbytecomp). .mll/.mly use the
# in-tree generated .ml/.mli.  Comments mark interface-only modules.
CL_COMMON="utils/config.mli utils/config.ml
utils/build_path_prefix_map.mli utils/build_path_prefix_map.ml
utils/format_doc.mli utils/format_doc.ml
utils/misc.mli utils/misc.ml
utils/identifiable.mli utils/identifiable.ml
utils/numbers.mli utils/numbers.ml
utils/arg_helper.mli utils/arg_helper.ml
utils/local_store.mli utils/local_store.ml
utils/load_path.mli utils/load_path.ml
utils/clflags.mli utils/clflags.ml
utils/profile.mli utils/profile.ml
utils/terminfo.mli utils/terminfo.ml
utils/ccomp.mli utils/ccomp.ml
utils/warnings.mli utils/warnings.ml
utils/consistbl.mli utils/consistbl.ml
utils/linkdeps.mli utils/linkdeps.ml
utils/strongly_connected_components.mli utils/strongly_connected_components.ml
utils/targetint.mli utils/targetint.ml
utils/int_replace_polymorphic_compare.mli utils/int_replace_polymorphic_compare.ml
utils/domainstate.mli utils/domainstate.ml
utils/binutils.mli utils/binutils.ml
utils/lazy_backtrack.mli utils/lazy_backtrack.ml
utils/diffing.mli utils/diffing.ml
utils/diffing_with_keys.mli utils/diffing_with_keys.ml
utils/stable_matching.mli utils/stable_matching.ml
utils/compression.mli utils/compression.ml
parsing/location.mli parsing/location.ml
parsing/unit_info.mli parsing/unit_info.ml
parsing/asttypes.mli parsing/asttypes.ml
parsing/longident.mli parsing/longident.ml
parsing/parsetree.mli
parsing/docstrings.mli parsing/docstrings.ml
parsing/syntaxerr.mli parsing/syntaxerr.ml
parsing/ast_helper.mli parsing/ast_helper.ml
parsing/ast_iterator.mli parsing/ast_iterator.ml
parsing/builtin_attributes.mli parsing/builtin_attributes.ml
parsing/camlinternalMenhirLib.mli parsing/camlinternalMenhirLib.ml
parsing/parser.mli parsing/parser.ml
parsing/lexer.mli parsing/lexer.ml
parsing/pprintast.mli parsing/pprintast.ml
parsing/parse.mli parsing/parse.ml
parsing/printast.mli parsing/printast.ml
parsing/ast_mapper.mli parsing/ast_mapper.ml
parsing/attr_helper.mli parsing/attr_helper.ml
parsing/ast_invariants.mli parsing/ast_invariants.ml
parsing/depend.mli parsing/depend.ml
typing/annot.mli
typing/value_rec_types.mli
typing/ident.mli typing/ident.ml
typing/path.mli typing/path.ml
typing/type_immediacy.mli typing/type_immediacy.ml
typing/outcometree.mli
typing/primitive.mli typing/primitive.ml
typing/shape.mli typing/shape.ml
typing/types.mli typing/types.ml
typing/data_types.mli typing/data_types.ml
typing/rawprinttyp.mli typing/rawprinttyp.ml
typing/gprinttyp.mli typing/gprinttyp.ml
typing/btype.mli typing/btype.ml
typing/oprint.mli typing/oprint.ml
typing/subst.mli typing/subst.ml
typing/predef.mli typing/predef.ml
typing/datarepr.mli typing/datarepr.ml
typing/typing_recovery.mli typing/typing_recovery.ml
file_formats/cmi_format.mli file_formats/cmi_format.ml
typing/persistent_env.mli typing/persistent_env.ml
typing/env.mli typing/env.ml
typing/errortrace.mli typing/errortrace.ml
typing/typedtree.mli typing/typedtree.ml
typing/signature_group.mli typing/signature_group.ml
typing/printtyped.mli typing/printtyped.ml
typing/ctype.mli typing/ctype.ml
typing/out_type.mli typing/out_type.ml
typing/printtyp.mli typing/printtyp.ml
typing/errortrace_report.mli typing/errortrace_report.ml
typing/includeclass.mli typing/includeclass.ml
typing/mtype.mli typing/mtype.ml
typing/envaux.mli typing/envaux.ml
typing/includecore.mli typing/includecore.ml
typing/tast_iterator.mli typing/tast_iterator.ml
typing/tast_mapper.mli typing/tast_mapper.ml
typing/stypes.mli typing/stypes.ml
typing/shape_reduce.mli typing/shape_reduce.ml
file_formats/cmt_format.mli file_formats/cmt_format.ml
typing/cmt2annot.mli typing/cmt2annot.ml
typing/typing_recovery_state.mli typing/typing_recovery_state.ml
typing/untypeast.mli typing/untypeast.ml
typing/includemod.mli typing/includemod.ml
typing/signature_matching.mli typing/signature_matching.ml
typing/includemod_errorprinter.mli typing/includemod_errorprinter.ml
typing/typetexp.mli typing/typetexp.ml
typing/printpat.mli typing/printpat.ml
typing/patterns.mli typing/patterns.ml
typing/parmatch.mli typing/parmatch.ml
typing/typedecl_properties.mli typing/typedecl_properties.ml
typing/typedecl_variance.mli typing/typedecl_variance.ml
typing/typedecl_unboxed.mli typing/typedecl_unboxed.ml
typing/typedecl_immediacy.mli typing/typedecl_immediacy.ml
typing/typedecl_separability.mli typing/typedecl_separability.ml
lambda/debuginfo.mli lambda/lambda.mli
typing/typeopt.mli typing/typeopt.ml
typing/typedecl.mli typing/typedecl.ml
typing/value_rec_check.mli typing/value_rec_check.ml
typing/typecore.mli typing/typecore.ml
typing/typeclass.mli typing/typeclass.ml
typing/typemod.mli typing/typemod.ml
lambda/debuginfo.mli lambda/debuginfo.ml
lambda/lambda.mli lambda/lambda.ml
lambda/printlambda.mli lambda/printlambda.ml
lambda/switch.mli lambda/switch.ml
lambda/matching.mli lambda/matching.ml
lambda/value_rec_compiler.mli lambda/value_rec_compiler.ml
lambda/translobj.mli lambda/translobj.ml
lambda/translattribute.mli lambda/translattribute.ml
lambda/translprim.mli lambda/translprim.ml
lambda/translcore.mli lambda/translcore.ml
lambda/translclass.mli lambda/translclass.ml
lambda/translmod.mli lambda/translmod.ml
lambda/tmc.mli lambda/tmc.ml
lambda/simplif.mli lambda/simplif.ml
lambda/runtimedef.mli lambda/runtimedef.ml
file_formats/cmo_format.mli
file_formats/cmx_format.mli
file_formats/cmxs_format.mli
bytecomp/meta.mli bytecomp/meta.ml
bytecomp/opcodes.mli bytecomp/opcodes.ml
bytecomp/bytesections.mli bytecomp/bytesections.ml
bytecomp/dll.mli bytecomp/dll.ml
bytecomp/symtable.mli bytecomp/symtable.ml
driver/pparse.mli driver/pparse.ml
driver/compenv.mli driver/compenv.ml
driver/main_args.mli driver/main_args.ml
driver/compmisc.mli driver/compmisc.ml
driver/makedepend.mli driver/makedepend.ml
driver/compile_common.mli driver/compile_common.ml"

CL_BYTE="bytecomp/opnames.mli bytecomp/opnames.ml
bytecomp/instruct.mli bytecomp/instruct.ml
bytecomp/bytegen.mli bytecomp/bytegen.ml
bytecomp/printinstr.mli bytecomp/printinstr.ml
bytecomp/emitcode.mli bytecomp/emitcode.ml
bytecomp/bytelink.mli bytecomp/bytelink.ml
bytecomp/bytelibrarian.mli bytecomp/bytelibrarian.ml
bytecomp/bytepackager.mli bytecomp/bytepackager.ml
driver/errors.mli driver/errors.ml
driver/compile.mli driver/compile.ml
driver/maindriver.mli driver/maindriver.ml
driver/main.mli driver/main.ml"

# Only WD is on the include path: every module (sources flattened into WD) is
# compiled there in dependency order, so each dep's freshly-built .cmi is present
# when needed.  Adding the in-tree source dirs would let a module resolve a dep's
# STALE real .cmi (a different interface CRC) before WD's exists -> the linker's
# consistency check then rejects the mix.  WD-only keeps every CRC self-consistent.
INCS="-I $WD"

ok=0; fail=0; order=""
compile_list() {
  for f in $1; do
    base=$(basename "$f"); [ -f "$ROOT/$f" ] || { echo "MISSING $f"; exit 1; }
    cp "$ROOT/$f" "$WD/$base"
    if ( cd "$WD" && "$CPP" -c -stdlib "$WD" $INCS "$base" ) >"$WD/cerr" 2>&1; then
      ok=$((ok+1)); [ "${base##*.}" = ml ] && order="$order ${base%.ml}"
    else
      fail=$((fail+1)); echo "[FAIL $f]"; sed 's/^/   /' "$WD/cerr" | head -6
    fi
  done
}
echo "--- compiling ocamlcommon ---"; compile_list "$CL_COMMON"
echo "--- compiling ocamlbytecomp ---"; compile_list "$CL_BYTE"
echo "=== compiler: ok=$ok fail=$fail ==="
[ "$fail" = 0 ] || exit 1

# ---- 3. link the bytecode ocamlc -------------------------------------------
stdobjs=""; for m in $STDORDER; do f="$WD/$(gname "$m").cmo"; [ -f "$f" ] && stdobjs="$stdobjs $f"; done
clobjs=""; for n in $order; do clobjs="$clobjs $WD/$n.cmo"; done
if ! "$LINK" -nostdlib -runtime "$RUN" $stdobjs $clobjs "$WD/std_exit.cmo" -o "$WD/ocamlc" 2>"$WD/lerr"; then
  echo "FAIL: link"; sed 's/^/  /' "$WD/lerr"; exit 1
fi
echo "linked: $WD/ocamlc"
# Persist the compiler .cmo link order so swap/relink/multistage/ddc harnesses
# find it without re-deriving from CL_COMMON/CL_BYTE (DDC footgun 6).
printf '%s\n' $order > "$WD/.cl_order"

# ---- 3.5 stdlib.cma + runtime-launch-info: the bootstrapped ocamlc's OWN
# bytelink auto-loads stdlib.cma (even under -nostdlib) and reads
# runtime-launch-info from the stdlib dir, so the smoke test's LINK step needs
# both in WD.  Like the real distribution, std_exit.cmo stays OUTSIDE the
# archive (bytelink auto-appends it from the search path).
if ! "$CPP" -a -nostdlib -I "$WD" $stdobjs -o "$WD/stdlib.cma" 2>"$WD/aerr"; then
  echo "FAIL: stdlib.cma archive"; sed 's/^/  /' "$WD/aerr" | head -5; exit 1
fi
cp "$ROOT/stdlib/runtime-launch-info" "$WD/"
echo "archived: $WD/stdlib.cma"

# ---- 4. smoke test: the bootstrapped ocamlc must actually COMPILE and the
# produced program must RUN.  (Testing only `-version` hides codegen bugs that
# crash on real input -- the compiler can start up fine yet segfault compiling.)
echo 'let () = Printf.printf "hello from bootstrapped ocamlc: %d\n" (List.fold_left (+) 0 [1;2;3;4])' > "$WD/hello.ml"
if ( cd "$WD" && "$RUN" ./ocamlc -nostdlib -I "$WD" hello.ml -o hello.exe ) 2>"$WD/serr" \
   && out=$("$RUN" "$WD/hello.exe" 2>&1) && [ "$out" = "hello from bootstrapped ocamlc: 10" ]; then
  echo "OK: bootstrapped ocamlc compiled+ran a program -> $out"
else
  echo "FAIL: bootstrapped ocamlc did not compile+run a program"
  sed 's/^/  /' "$WD/serr" 2>/dev/null | head -5
  exit 1
fi
