#!/usr/bin/env bash
# Diverse Double-Compiling (Wheeler) verification for the OCaml bytecode compiler,
# using c++ocamlc as the diverse, independent implementation.
#
#   diverse path : c++ocamlc(source) = S1   (bytecode ocamlc built by c++ocamlc)
#                  S1(source)        = S2   (real-ocamlc code, built via the
#                                            diverse compiler; regenerates
#                                            OFFICIAL-format .cmi, not c++ocamlc's)
#   official     : ocamlc.opt(source)       = REF
#
# We compile the whole compiler source with BOTH S2 and ocamlc.opt against an
# identical (wrapped) stdlib and compare every .cmi/.cmo byte-for-byte.  If they
# match, the official compiler binary faithfully corresponds to its source: a
# Thompson trusting-trust trojan could not hide, because a from-scratch C++
# reimplementation reproduces the exact compiler bytecode.
#
# SCOPE (be honest): this covers the compiler-source -> bytecode step only.  The
# stdlib is shared between both builds (not yet diversely rebuilt) and the C
# runtime (ocamlrun) executing S1/S2 is the official one.  See the DDC plan in
# the project notes for closing those gaps.
#
# Usage:   [WD=<bootstrap-workdir>] bash cxx/harness/ddc.sh
#   WD unset -> a fresh KEEP=1 bootstrap is run to produce S1.
#
# This harness deliberately bakes in the footguns that made the first manual run
# give wrong answers; see the numbered notes below.
set -u
set -o pipefail                       # FOOTGUN 1: never let a pipe hide a rc

SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
source cxx/harness/_require_fresh.sh; require_fresh c++ocamlc
RUN=$ROOT/runtime/ocamlrun
OPT=$ROOT/ocamlc.opt
CPPC=$ROOT/cxx/build/c++ocamlc   # links too (-nopervasives -use-runtime)
DEPSORT=$ROOT/tools/ocamldep.opt
BOOTSTRAP=$ROOT/cxx/harness/ocamlc_bootstrap.sh
FLAGS="-strict-sequence -strict-formats -w +a-4-9-40-41-42-44-45-48-70"

die() { echo "FATAL: $*" >&2; exit 1; }

for t in "$OPT" "$CPPC" "$DEPSORT" "$RUN"; do [ -x "$t" ] || die "missing tool: $t"; done

# ---------------------------------------------------------------------------
# 0. Obtain S1 = the bytecode ocamlc built by c++ocamlc (a KEEP=1 bootstrap WD).
# ---------------------------------------------------------------------------
if [ -z "${WD:-}" ]; then
  echo "== no WD given: running a fresh KEEP=1 bootstrap to build S1 =="
  boot_log=$(mktemp /tmp/ddc_boot.XXXXXX)
  KEEP=1 bash "$BOOTSTRAP" >"$boot_log" 2>&1 || { sed 's/^/  /' "$boot_log" | tail -20; die "bootstrap failed"; }
  WD=$(sed -n 's/^linked: \(.*\)\/ocamlc$/\1/p' "$boot_log" | tail -1)
  [ -n "$WD" ] || die "could not parse WD from bootstrap log $boot_log"
fi
[ -f "$WD/ocamlc" ] || die "no S1 at $WD/ocamlc"   # bytecode, run via ocamlrun
echo "S1 (c++ocamlc's ocamlc) = $WD/ocamlc"

# .cl_order = the compiler .cmo link order.  FOOTGUN 6: fresh bootstrap WDs may
# lack it; derive it from the harness's CL_COMMON/CL_BYTE lists if absent.
extract_cl() {   # -> repo-relative mli+ml paths, in listing order
  awk '
    /^CL_COMMON="/{g=1; sub(/^CL_COMMON="/,"")}
    /^CL_BYTE="/{g=1; sub(/^CL_BYTE="/,"")}
    g{l=$0; sub(/"[[:space:]]*$/,"",l); n=split(l,a,/[[:space:]]+/);
      for(i=1;i<=n;i++) if(a[i]!="") print a[i];
      if($0 ~ /"[[:space:]]*$/) g=0}
  ' "$BOOTSTRAP"
}
if [ ! -f "$WD/.cl_order" ]; then
  extract_cl | grep '\.ml$' | sed 's|.*/||; s|\.ml$||' > "$WD/.cl_order"
fi

# ---------------------------------------------------------------------------
# 1. Stage all compiler sources flat, and get a TOPOLOGICAL compile order.
#    FOOTGUN 2: the CL_COMMON textual order has forward refs (clflags.mli ->
#    Profile) that only c++ocamlc tolerates; strict ocamlc.opt needs real
#    dependency order.  Always use `ocamldep -sort`, never the listing order.
# ---------------------------------------------------------------------------
STAGE=$(mktemp -d /tmp/ddc_stage.XXXXXX)
while read -r f; do [ -z "$f" ] && continue; cp "$ROOT/$f" "$STAGE/$(basename "$f")"; done < <(extract_cl)
# run in $STAGE so -sort emits BASENAMES, not absolute paths
ORDER=$( cd "$STAGE" && "$DEPSORT" -sort ./*.mli ./*.ml 2>/dev/null | sed 's|^\./||' )
[ -n "$ORDER" ] || die "ocamldep -sort produced no order"
# how many bytecode .ml modules must produce a matching .cmo (the pass threshold)
NMODS=$(extract_cl | grep -c '\.ml$')

# ---------------------------------------------------------------------------
# 2. Build S2 = S1 recompiling the compiler, then linking.
#    FOOTGUN 4: S1 itself carries c++ocamlc's transient codegen quirks (the
#    binutils int64/int32 poly-compare specialization) that WASH OUT at the
#    second compile.  DDC must compare S2, never S1.
#    FOOTGUN 5: always compile into a fresh dir with outputs removed first.
# ---------------------------------------------------------------------------
STDORDER="camlinternalFormatBasics stdlib either sys obj type atomic mutex condition \
semaphore camlinternalLazy lazy seq option pair result bool char uchar list int array \
iarray bytes string unit marshal float int32 int64 nativeint lexing parsing repr set \
map stack queue buffer camlinternalFormat printf arg printexc domain fun gc in_channel \
out_channel digest bigarray random hashtbl weak scanf callback camlinternalOO oo \
dynarray format camlinternalMod pqueue ephemeron filename complex effect \
arrayLabels bytesLabels listLabels stringLabels moreLabels stdLabels"
gname() { case "$1" in camlinternal*|stdlib) echo "$1";;
                       *) c="$(tr '[:lower:]' '[:upper:]' <<< "${1:0:1}")${1:1}"; echo "stdlib__$c";; esac; }

echo "== building S2 = S1(source) =="
S2=$(mktemp -d /tmp/ddc_s2.XXXXXX)
cp "$WD"/*.cmi "$S2"/ 2>/dev/null      # seed interfaces (deps present -> order-free)
while read -r f; do
  [ -z "$f" ] && continue; b=$(basename "$f" .ml)
  [ "$f" = "${f%.ml}" ] && continue                       # skip .mli-only entries
  cp "$ROOT/$f" "$S2/$b.ml"; [ -f "$ROOT/${f%.ml}.mli" ] && cp "$ROOT/${f%.ml}.mli" "$S2/$b.mli"
  rm -f "$S2/$b.cmo"
  "$RUN" "$WD/ocamlc" -nostdlib -I "$S2" $FLAGS -c "$S2/$b.ml" >"$S2/$b.err" 2>&1 \
    || { echo "  [S2 compile FAIL $b]"; sed 's/^/    /' "$S2/$b.err" | head -4; die "S2 build failed"; }
done < <(extract_cl | grep '\.ml$')
# link S2 (reuse WD's stdlib cmos + std_exit; CRC-consistent with WD cmis)
for f in "$WD"/*.cmo; do bb=$(basename "$f"); [ -f "$S2/$bb" ] || cp "$f" "$S2/$bb"; done
cp "$WD/std_exit.cmo" "$WD/runtime-launch-info" "$WD/stdlib.cma" "$S2"/ 2>/dev/null
stdobjs=""; for m in $STDORDER; do o="$S2/$(gname "$m").cmo"; [ -f "$o" ] && stdobjs="$stdobjs $o"; done
clobjs="";  for n in $(cat "$WD/.cl_order"); do clobjs="$clobjs $S2/$n.cmo"; done
"$CPPC" -nopervasives -use-runtime "$RUN" -I "$S2" $stdobjs $clobjs "$S2/std_exit.cmo" -o "$S2/ocamlc" 2>"$S2/lerr" \
  || { sed 's/^/  /' "$S2/lerr" | head; die "S2 link failed"; }
# smoke S2
echo 'let()=print_string"ok\n"' > "$S2/smoke.ml"
( cd "$S2" && "$RUN" ./ocamlc -nostdlib -I "$S2" smoke.ml -o smoke.exe ) >/dev/null 2>&1 \
  && [ "$("$RUN" "$S2/smoke.exe" 2>&1)" = "ok" ] || die "S2 smoke test failed"
echo "S2 built and runs."

# ---------------------------------------------------------------------------
# 3. DDC: build the compiler source with S2 (diverse) and ocamlc.opt (official)
#    against an identical, WRAPPED-ONLY stdlib, then compare.
#    FOOTGUN 3: repo stdlib/ ships BOTH the pre-wrapping lexing.cmi and
#    stdlib__Lexing.cmi; putting *.cmi on the path clashes Lexing.position vs
#    Stdlib.Lexing.position.  Seed ONLY the wrapped modules.
# ---------------------------------------------------------------------------
seed_stdlib() { cp "$ROOT"/stdlib/stdlib.cmi "$ROOT"/stdlib/stdlib__*.cmi \
                   "$ROOT"/stdlib/camlinternal*.cmi "$1"/ 2>/dev/null; }

ddc_build() {   # $1 = "compiler invocation"  $2 = out dir  $3 = label
  local CC="$1" O="$2" tag="$3" fails=0 first=""
  seed_stdlib "$O"
  for base in $ORDER; do
    cp "$STAGE/$base" "$O/$base"; rm -f "$O/${base%.*}.cmo"
    if ! ( cd "$O" && $CC -nostdlib -I "$O" $FLAGS -c "$base" ) >>"$O/log" 2>&1; then
      fails=$((fails+1)); [ -z "$first" ] && first="$base: $(tail -1 "$O/log")"
    fi
  done
  echo "  $tag: build fails=$fails${first:+ (first=$first)}"
}

REF=$(mktemp -d /tmp/ddc_ref.XXXXXX); DIV=$(mktemp -d /tmp/ddc_div.XXXXXX)
echo "== DDC build =="
ddc_build "$OPT"            "$REF" "REF (ocamlc.opt)"
ddc_build "$RUN $S2/ocamlc" "$DIV" "DIV (diverse S2)"

# The only expected failure is cmx_format.mli (native/Clambda), identical in both.
cis=0 cid=0 cos=0 cod=0 cidl="" codl=""
for a in "$REF"/*.cmi; do b=$(basename "$a"); [ -f "$DIV/$b" ] || continue
  if cmp -s "$a" "$DIV/$b"; then cis=$((cis+1)); else cid=$((cid+1)); cidl="$cidl $b"; fi; done
for a in "$REF"/*.cmo; do b=$(basename "$a"); [ -f "$DIV/$b" ] || continue
  if cmp -s "$a" "$DIV/$b"; then cos=$((cos+1)); else cod=$((cod+1)); codl="$codl $b"; fi; done

echo "=============================================================="
echo "DDC RESULT:  cmi same=$cis diff=$cid  |  cmo same=$cos diff=$cod"
[ -n "$cidl" ] && echo "  cmi DIFF:$cidl"
[ -n "$codl" ] && echo "  cmo DIFF:$codl"
echo "  artifacts: REF=$REF DIV=$DIV  (S1=$WD S2=$S2)"
echo "=============================================================="
# A meaningful pass requires ALL bytecode modules compared identical -- not a
# vacuous 0==0 from a build that produced nothing.
if [ "$cid" -eq 0 ] && [ "$cod" -eq 0 ] && [ "$cos" -ge "$NMODS" ]; then
  echo "PASS: diverse and official compilers produce bit-identical bytecode"
  echo "      ($cos/$NMODS bytecode .cmo + $cis .cmi, 0 diffs)."
  exit 0
else
  echo "FAIL: divergence or incomplete build (cmo matched $cos/$NMODS; diffs cmi=$cid cmo=$cod)."
  exit 1
fi
