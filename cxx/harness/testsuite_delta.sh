#!/usr/bin/env bash
# OCaml's own testsuite, driven by ocamltest, with c++ocamlc standing in for
# ocamlc -- and the same suite run with the real ocamlc as the baseline.  The
# report is the DELTA: tests the real compiler passes that c++ocamlc fails,
# bucketed by cause.  This is what `make tests` would say about c++ocamlc, and
# it exercises what the per-file parity gates cannot: each test's TEST block
# (flags, multi-module builds, expected exit statuses, ocamlrunparam, reference
# outputs), so it sees false accepts, driver gaps and multi-module miscompiles.
#
# Mechanism (no edit to ocamltest, no edit to the tree): ocamltest locates every
# tool under $OCAMLSRCDIR, so we point it at a SHADOW tree of symlinks whose
# runtime/ocamlrun is a shim: `ocamlrun <srcdir>/ocamlc args` becomes
# `c++ocamlc args`; anything else is passed to the real ocamlrun.  ocamlc.opt,
# ocamlopt and the toplevel stay real, so native/toplevel actions are controls.
#
# FOOTGUNS baked in:
#  1. The tree is configured --disable-ocamltest, and a top-level `make` of
#     anything would REBUILD THE ORACLE (parsing/ carries newer mtimes than the
#     rest of the tree).  ocamltest is built by running only the ocamltest/-
#     scoped commands of `make -n ocamltest/ocamltest`.  Never `make` here.
#  2. Run with the C toolchain the tree was configured with (Makefile.config's
#     CC): with another one, native and -custom links fail (its ld and libc
#     against the runtime's).  Such failures cancel in the delta but pollute
#     the absolute numbers, so we warn.
#  3. ocamltest interleaves subprocess stderr into its own " ... testing" status
#     lines; the parser strips the junk, else ~150 results silently vanish.
#  4. `compare-bytecode-programs` (exe bytes vs ocamlc.opt's) is the LAST action
#     of a bytecode test; a test failing only there compiled, ran and printed
#     correctly.  It is reported as its own bucket, not folded into "failed".
#
# Scope (honest): expect/toplevel tests need testsuite/tools/expect and the
# toplevel -- toplevel-side, unbuilt here, failing identically in both runs.
#
# Usage: [JOBS=8] [WORK=/tmp/testsuite_delta] [REBUILD_BASELINE=1] [REPORT_ONLY=1] bash cxx/harness/testsuite_delta.sh
# Outputs: $WORK/{baseline,cpp}/results.tsv, $WORK/regressions.tsv (bucket, test,
#          first compiler-output line), /tmp/.testsuite_regressions (sorted test list).
set -u
export LC_ALL=C
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
source cxx/harness/_require_fresh.sh; require_fresh c++ocamlc
JOBS=${JOBS:-8}
WORK=${WORK:-/tmp/testsuite_delta}
OT=$ROOT/ocamltest/ocamltest
CPP=$ROOT/cxx/build/c++ocamlc
mkdir -p "$WORK"
die() { echo "FATAL: $*" >&2; exit 1; }

cc=$(sed -n 's/^CC=//p' "$ROOT/Makefile.config" | awk '{print $1}')
command -v "$cc" >/dev/null ||
  echo "WARNING: the tree's C compiler ($cc, Makefile.config) is not on PATH -- native/-custom links will fail on both sides." >&2

# ---------------------------------------------------------------------------
# 1. ocamltest, hand-built from make's own dry run (FOOTGUN 1).
# ---------------------------------------------------------------------------
ensure_ocamltest() {
  if [ -x "$OT" ] && [ "$OT" -nt ocamltest/main.ml ]; then return; fi
  echo "== building ocamltest from the ocamltest/-scoped steps of 'make -n' =="
  mkdir -p .dep/ocamltest
  make -n ocamltest/ocamltest 2>/dev/null | grep -v '^ ' | grep 'ocamltest/' | grep -v '^echo' \
    > "$WORK/build_ocamltest.sh"
  [ -s "$WORK/build_ocamltest.sh" ] || die "make -n produced no ocamltest steps"
  bash -e "$WORK/build_ocamltest.sh" > "$WORK/build_ocamltest.log" 2>&1 \
    || { tail -5 "$WORK/build_ocamltest.log" >&2; die "ocamltest build failed (see $WORK/build_ocamltest.log)"; }
  [ -x "$OT" ] || die "no $OT after build"
}

# ---------------------------------------------------------------------------
# 2. The shadow source tree with the ocamlrun shim.
# ---------------------------------------------------------------------------
SHADOW=$WORK/shadow
make_shadow() {
  rm -rf "$SHADOW"; mkdir -p "$SHADOW/runtime"
  for e in "$ROOT"/*; do b=$(basename "$e"); [ "$b" = runtime ] && continue; ln -s "$e" "$SHADOW/$b"; done
  for e in "$ROOT"/runtime/*; do b=$(basename "$e"); [ "$b" = ocamlrun ] && continue; ln -s "$e" "$SHADOW/runtime/$b"; done
  cat > "$SHADOW/runtime/ocamlrun" <<SHIM
#!/bin/sh
# ocamltest shim: \`ocamlrun <srcdir>/ocamlc ...\` -> c++ocamlc; everything else -> the real ocamlrun.
case "\$1" in
  */ocamlc|ocamlc) shift; exec "$CPP" "\$@" ;;
esac
exec "$ROOT/runtime/ocamlrun" "\$@"
SHIM
  chmod +x "$SHADOW/runtime/ocamlrun"
}

# ---------------------------------------------------------------------------
# 3. One full suite run: per-directory logs, JOBS dirs at a time.
# ---------------------------------------------------------------------------
run_suite() {  # label srcdir
  local label=$1 srcdir=$2 out="$WORK/$1"
  rm -rf "$out"; mkdir -p "$out/logs" "$out/work"
  ( cd testsuite && OCAMLSRCDIR="$srcdir" "$OT" -find-test-dirs tests ) > "$out/dirs.txt"
  echo "== $label: $(wc -l < "$out/dirs.txt") test dirs, $JOBS at a time =="
  export OT out srcdir
  xargs -P "$JOBS" -I{} bash -c '
    d=$1; log="$out/logs/$(echo "$d" | tr "/" "%").log"
    cd testsuite || exit 1
    export OCAMLSRCDIR="$srcdir" OCAMLTESTDIR="$out/work" TERM=dumb
    { "$OT" -list-tests "$d" | while IFS= read -r t; do
        timeout 600 "$OT" "$d/$t" || echo " ... testing '"'"'$t'"'"' => unexpected error"
      done; } > "$log" 2>&1' _ {} < "$out/dirs.txt"
  # Per-test AGGREGATE status lines -> "dir/file<TAB>status" (FOOTGUN 3).
  for f in "$out"/logs/*.log; do
    d=$(basename "$f" .log | tr '%' '/')
    perl -0777 -pe "s/(testing '[^']*')((?! with | => )[^\n]*)\n(?= => )/\$1/g" "$f" \
    | grep -o "^ \.\.\. testing '[^']*' => [a-z/]*" \
    | sed -E "s|^ \.\.\. testing '([^']*)' => (.*)|$d/\1\t\2|"
  done | sort -u -t $'\t' -k1,1 > "$out/results.tsv"
  awk -F'\t' '{c[$2]++} END{for(s in c) printf "   %s=%d", s, c[s]; print ""}' "$out/results.tsv"
}

# ---------------------------------------------------------------------------
# 4. Bucket one regressed test by its first failing action (FOOTGUN 4).
# ---------------------------------------------------------------------------
bucket() {  # dir/file -> "BUCKET<TAB>first compiler-output line"
  local t=$1 dir file base log line reason out first exp
  dir=$(dirname "$t"); file=$(basename "$t"); base=${file%.*}
  log="$WORK/cpp/logs/$(echo "$dir" | tr '/' '%').log"
  line=$(grep -m1 "^ \.\.\. testing '$file' with .* => failed" "$log")
  reason=${line#*=> failed}
  out="$WORK/cpp/work/$dir/$base/ocamlc.byte/ocamlc.byte.output"
  first=$( [ -f "$out" ] && grep -m1 . "$out" | cut -c1-120 )
  # first declared expected exit status (a file may carry several TEST blocks)
  exp=$(grep -o 'ocamlc_byte_exit_status = "[0-9]*"' "testsuite/$t" 2>/dev/null | head -1 | grep -o '[0-9]*')
  local b
  case "$reason" in
    *"are different"*)            b="EXE_BYTES_DIFFER(compare-bytecode-programs; compile+run+output passed)";;
    *"program output"*differs*)   b="PROGRAM_OUTPUT_DIFF";;
    *"compiler output"*differs*)  b="COMPILER_OUTPUT_TEXT_DIFF";;
    *"Running program"*)          b="PROGRAM_RUN_FAILED";;
    *"file not found"*)           b="FILE_NOT_FOUND";;
    *"Compiling"*)
      case "$first" in
        *"is not supported yet"*)          b="UNSUPPORTED_FLAG:$(echo "$first" | grep -o -- '-[A-Za-z-]* is not' | cut -d' ' -f1)";;
        *"don't know what to do with"*.c)  b="C_STUB_INPUT";;
        *"don't know what to do with"*)    b="UNKNOWN_INPUT";;
        *"ignoring unknown option"*)       b="UNKNOWN_FLAG:$(echo "$first" | grep -o -- '-[A-Za-z_-]*$')";;
        *"link error"*)                    b="LINK_ERROR";;
        *"parse error"*)                   b="PARSE_ERROR_TEXT";;
        "")  if [ "${exp:-0}" != 0 ]; then b="FALSE_ACCEPT(expected exit $exp, got 0)"; else b="COMPILE_FAIL_SILENT"; fi;;
        *)   if [ "${exp:-0}" != 0 ]; then b="ERROR_TEXT_DIFF(expected-error test)"; else b="FALSE_REJECT_OR_ERROR"; fi;;
      esac;;
    *) b="OTHER:$(echo "$line" | sed -E 's/.*\(([^)]*)\) => failed.*/\1/')";;
  esac
  printf '%s\t%s\n' "$b" "$first"
}

# ---------------------------------------------------------------------------
# Main.
# ---------------------------------------------------------------------------
if [ -z "${REPORT_ONLY:-}" ]; then   # REPORT_ONLY=1: re-bucket the last run without re-running
  ensure_ocamltest
  if [ -n "${REBUILD_BASELINE:-}" ] || [ ! -s "$WORK/baseline/results.tsv" ] \
     || [ "$ROOT/ocamlc.opt" -nt "$WORK/baseline/results.tsv" ]; then
    run_suite baseline "$ROOT"
  else
    echo "== baseline: cached in $WORK/baseline (REBUILD_BASELINE=1 to redo) =="
  fi
  make_shadow
  run_suite cpp "$SHADOW"
fi
[ -s "$WORK/cpp/results.tsv" ] || die "no results in $WORK/cpp"

join -t $'\t' -a1 -a2 -e MISSING -o 0,1.2,2.2 "$WORK/baseline/results.tsv" "$WORK/cpp/results.tsv" > "$WORK/joined.tsv"
base_pass=$(awk -F'\t' '$2=="passed"' "$WORK/joined.tsv" | wc -l)
both_pass=$(awk -F'\t' '$2=="passed" && $3=="passed"' "$WORK/joined.tsv" | wc -l)
awk -F'\t' '$2=="passed" && $3!="passed"{print $1}' "$WORK/joined.tsv" > "$WORK/regressed.txt"
awk -F'\t' '$2!="passed" && $3=="passed"{print $1}' "$WORK/joined.tsv" > "$WORK/reverse.txt"

: > "$WORK/regressions.tsv"
while IFS= read -r t; do printf '%s\t%s\n' "$(bucket "$t")" "$t"; done < "$WORK/regressed.txt" \
  | awk -F'\t' '{print $1"\t"$3"\t"$2}' | sort > "$WORK/regressions.tsv"
exe_only=$(grep -c '^EXE_BYTES_DIFFER' "$WORK/regressions.tsv")

echo "=============================================================="
echo "TESTSUITE DELTA (ocamltest, bytecode ocamlc := c++ocamlc)"
echo "  real ocamlc passes:            $base_pass tests"
echo "  c++ocamlc passes:              $both_pass"
echo "  exe-bytes-only failures:       $exe_only   (compile, run, output all correct)"
echo "  behaviourally passing:         $((both_pass + exe_only)) / $base_pass"
echo "  regressions (real pass, c++ fail): $(wc -l < "$WORK/regressed.txt")   reverse: $(wc -l < "$WORK/reverse.txt")"
echo "--- regression buckets ---"
cut -f1 "$WORK/regressions.tsv" | sort | uniq -c | sort -rn
echo "  details: $WORK/regressions.tsv   reverse: $WORK/reverse.txt"
echo "=============================================================="
cut -f2 "$WORK/regressions.tsv" | grep -v '^$' | sort > /tmp/.testsuite_regressions
