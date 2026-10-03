#!/usr/bin/env bash
# Cross-toolchain determinism gate: the compiler's OUTPUT must not depend on
# the C++ compiler that built c++ocamlc.
#
# Why this exists: on 2026-09-07 a gcc-11 build of c++ocamlc wrote different
# .cmi/.cmo from the clang-20 build for 109 of the 139 compiler modules -- a
# functor application's result signature lost the body's written type-variable
# names (`Map.Make(String).fold : ('a -> 'b -> 'b)` instead of `'acc`).  Cause:
# two recursive conversions sharing a var counter sat in ONE argument list, and
# argument evaluation order is unspecified (clang left-to-right, gcc
# right-to-left); only the first-converted var occurrence carried its name.  A
# single pinned toolchain had hidden it for months.  This gate builds c++ocamlc
# with a SECOND toolchain, compiles the DDC corpus with both, and requires every
# artifact to be cmp-identical.  It is also part of the DDC story: the diverse
# compiler's result does not depend on the diverse compiler's compiler.
#
# Toolchain A = cxx/build (cxx/Makefile's compiler, g++, via require_fresh).
# Toolchain B = cxx/build-alt: built here by cxx/Makefile with the SYSTEM
# clang++ in a clean environment (the system's tools only: another toolchain's
# linker on PATH could inject its rpaths into the link), mimalloc off.  Override with ALT_BUILD=<dir>
# (a directory under cxx/) or CXX_B=<compiler> to pick the second compiler.
#
# Usage: bash cxx/harness/toolchain_determinism.sh
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
source cxx/harness/_require_fresh.sh; require_fresh c++ocamlc
A=$ROOT/cxx/build/c++ocamlc
ALT_BUILD=${ALT_BUILD:-build-alt}
CXX_B=${CXX_B:-clang++}
die() { echo "FATAL: $*" >&2; exit 1; }

# 1. Toolchain B build (incremental).
mkdir -p "cxx/$ALT_BUILD"
env -i PATH=/usr/bin:/bin HOME="$HOME" make -C cxx -j"$(nproc)" O="$ALT_BUILD" CXX="$CXX_B" MIMALLOC=no \
  c++ocamlc >"cxx/$ALT_BUILD/make.log" 2>&1 \
  || { grep -E 'error' "cxx/$ALT_BUILD/make.log" | head -5 >&2; die "toolchain B build failed"; }
B=$ROOT/cxx/$ALT_BUILD/c++ocamlc
# Environment-independent check (ldd would resolve against the CURRENT shell's
# libraries): a link with the system's tools carries no RUNPATH.
readelf -d "$B" 2>/dev/null | grep -qE 'RPATH|RUNPATH' \
  && die "$B carries an RPATH/RUNPATH -- linked with a non-system toolchain?"

# 2. The DDC corpus (the compiler's own sources), compiled by both.
extract_cl() { awk '
  /^CL_COMMON="/{g=1; sub(/^CL_COMMON="/,"")}
  /^CL_BYTE="/{g=1; sub(/^CL_BYTE="/,"")}
  g{l=$0; sub(/"[[:space:]]*$/,"",l); n=split(l,a,/[[:space:]]+/);
    for(i=1;i<=n;i++) if(a[i]!="") print a[i];
    if($0 ~ /"[[:space:]]*$/) g=0}' cxx/harness/ocamlc_bootstrap.sh; }
STAGE=$(mktemp -d /tmp/tcdet_stage.XXXXXX)
while read -r f; do [ -z "$f" ] && continue; cp "$ROOT/$f" "$STAGE/$(basename "$f")"; done < <(extract_cl)
ORDER=$(cd "$STAGE" && "$ROOT/tools/ocamldep.opt" -sort ./*.mli ./*.ml 2>/dev/null | sed 's|\./||g')
[ -n "$ORDER" ] || die "ocamldep -sort produced no order"
FLAGS="-strict-sequence -strict-formats -w +a-4-9-40-41-42-44-45-48-70"
compile_all() {  # compiler outdir
  mkdir -p "$2"; cp "$STAGE"/*.ml "$STAGE"/*.mli "$2"/
  ( cd "$2"; for f in $ORDER; do "$1" -I . -I "$ROOT/stdlib" $FLAGS -c "$f" 2>/dev/null; done )
}
OA=$(mktemp -d /tmp/tcdet_a.XXXXXX); OB=$(mktemp -d /tmp/tcdet_b.XXXXXX)
compile_all "$A" "$OA"; compile_all "$B" "$OB"

# 3. Every artifact cmp-identical.
fail=0; n=0; : > /tmp/.toolchain_diff_files
for o in "$OA"/*.cmo "$OA"/*.cmi; do
  b=$(basename "$o"); n=$((n+1))
  if [ ! -f "$OB/$b" ]; then echo "MISSING $b" >> /tmp/.toolchain_diff_files; fail=$((fail+1))
  elif ! cmp -s "$o" "$OB/$b"; then echo "DIFF $b" >> /tmp/.toolchain_diff_files; fail=$((fail+1)); fi
done
echo "toolchain determinism: $((n-fail))/$n artifacts identical between $(basename "$(dirname "$A")") and $ALT_BUILD  (diffs: $fail)"
[ "$fail" -eq 0 ] && { echo "PASS"; rm -rf "$STAGE" "$OA" "$OB"; exit 0; }
head -20 /tmp/.toolchain_diff_files; echo "FAIL  (A=$OA B=$OB kept)"; exit 1
