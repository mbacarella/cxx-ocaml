#!/usr/bin/env bash
# DEPRECATED AS A TYPE-CHECKING METRIC (2026-09-26) -- see TYPECHECKER.md.
# This scores the deprecated approximating inference (c++type --infer);
# keep it as a regression tripwire only.
# Signature-inference parity: does c++ infer the same top-level VALUE signatures
# as `ocamlc -i`?  This is the metric that gates separate compilation / .cmi
# emission (and thus building real projects): to write a correct .cmi for a
# module without a .mli, the inferred interface must match the oracle's.
#
# We compare `ocamlc -i` (oracle inferred interface) against `c++type --infer`,
# restricted to `val` lines (type/module/etc. decls are a separate axis), with
# type-variable names canonicalised by first appearance on each side (so 'a/'b
# and our '_NNN stamps line up structurally).  Lenient v1: ignores the weak vs
# generalised distinction and multi-line wraps -- both are follow-up axes.
#
# Usage: sig_parity.sh [N]      (JOBS= overridable)
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
source cxx/harness/_require_fresh.sh; require_fresh c++type
CPP=./cxx/build/c++type
ORACLE=./ocamlc.opt
JOBS="${JOBS:-$(nproc)}"
TIMEOUT="${TIMEOUT:-15}"

# val-lines only, type-var tokens -> a canonical per-line first-appearance seq.
norm() { perl "$(dirname "$SELF")/signorm.pl"; }

# Sibling-module cmis (multi-file tests) -- same context on both sides.
export CPPCAML_SIB_CMI_ROOT="${CPPCAML_SIB_CMI_ROOT:-/tmp/sib_cmi}"

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  sib=""; sd="$CPPCAML_SIB_CMI_ROOT/$(dirname "$f")"; [ -d "$sd" ] && sib="-I $sd"
  o=$(timeout "$TIMEOUT" "$ORACLE" -nostdlib -I stdlib -I otherlibs/unix \
      -I otherlibs/str -I otherlibs/systhreads -I otherlibs/runtime_events \
      -I otherlibs/dynlink $sib -i "$f" 2>/dev/null); orc=$?
  [ $orc -ne 0 ] && { printf 'OERR\n'; exit 0; }          # oracle didn't type it
  on=$(printf '%s\n' "$o" | norm)
  [ -z "$on" ] && { printf 'NOVAL\n'; exit 0; }            # no value bindings to compare
  c=$(timeout "$TIMEOUT" "$CPP" --infer "$f" 2>/dev/null); crc=$?
  { [ $crc -ne 0 ] || printf '%s' "$c" | grep -q 'TYPE_ERROR'; } && { printf 'CERR\n'; exit 0; }
  cn=$(printf '%s\n' "$c" | norm)
  [ "$on" == "$cn" ] && printf 'MATCH\n' || printf 'DIFF\n'
  exit 0
fi

LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | awk '
  $1=="MATCH"{m++} $1=="DIFF"{d++} $1=="CERR"{ce++} $1=="OERR"{oe++} $1=="NOVAL"{nv++}
  END{
    judged=m+d
    printf "files: %d   oracle-typed-with-vals: %d   c++ errored: %d\n", NR, judged+ce, ce
    printf "signature-identical: %d / %d   (%.1f%%)\n", m, judged, judged?100*m/judged:0
  }'
printf '%s\n' "$res" > /tmp/.sig_results 2>/dev/null || true
