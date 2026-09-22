#!/usr/bin/env bash
# The .cmi ident POSITION battery: the same probes as stamp_battery.sh, scored
# on every Local ident of the saved signature -- a constructor's or a label's
# stamp is the counter's value where typing created it (S557), so a charge
# made at the wrong POINT of the walk moves it while the first-stamp score
# (a total) stays put.  Each probe's (name, stamp) sequence, ours against
# ocamlc.opt's, is classed:
#   SAME    identical
#   CPOS    the same names, a constructor's or label's stamp moved (a
#           charge of the count model at the wrong position)
#   ORDER   the same names, only the writer's renamed run ordered differently
#   TOTAL   the first ident differs (the total is off: stamp_battery's class)
#   SHAPE   different ident sequences (the writer emits other items)
#
#   BIN=<compiler>            default cxx/build-release/c++ocamlc
#   HOOKENV="env NOFOO=1"     run OUR side under a revert hook
#   JOBS=12                   probes compiled in parallel
#   ONLY=xcp_a1               one probe
#   OUT=<file>                keep every probe's verdict and both sequences
#   V=1                       list the ORDER, TOTAL and SHAPE probes too
#
# Usage: bash cxx/harness/stamp_positions.sh [tag]
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
SRC="$ROOT/cxx/harness/stamp_probes"
BIN=${BIN:-$ROOT/cxx/build-release/c++ocamlc}
TAG=${1:-run}
JOBS=${JOBS:-12}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ "${1:-}" = "--worker" ]; then
  x=$2; w=$3; mkdir -p "$w/o" "$w/c"
  cp "$SRC/$x.ml" "$w/o/"; cp "$SRC/$x.ml" "$w/c/"
  ( cd "$w/o" && timeout 20 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" \
      -c "$x.ml" ) >/dev/null 2>&1
  ( cd "$w/c" && timeout 20 ${HOOKENV:-env} "$BIN" -nostdlib -I "$ROOT/stdlib" \
      -c "$x.ml" ) >/dev/null 2>&1
  [ -f "$w/o/$x.cmi" ] && [ -f "$w/c/$x.cmi" ] || { echo "FAIL $x"; exit 0; }
  # every Local ident (tag 0 block of a name and a stamp) in dump order
  ids() { perl "$ROOT/cxx/harness/mdump.pl" "$1" 2>/dev/null | awk '
    /^ *\(t0$/ {st=1; next}
    st==1 && /^ *"[^"]*"$/ {nm=$1; st=2; next}
    st==2 && /^ *[0-9]+$/ {
      if ($1+0 < 100000 && $1+0 > 1) printf "%s=%s ", nm, $1; st=0; next}
    {st=0}'; }
  o=$(ids "$w/c/$x.cmi"); r=$(ids "$w/o/$x.cmi")
  if [ "$o" = "$r" ]; then echo "SAME $x"; exit 0; fi
  echo "$o" | awk -v r="$r" -v x="$x" '{
    o=$0; no=o; gsub(/=[0-9]+/,"",no); nr=r; gsub(/=[0-9]+/,"",nr);
    n=split(o,ao," "); split(r,ar," ");
    if (no!=nr) cls="SHAPE";
    else if (ao[1]!=ar[1]) cls="TOTAL";
    else { split(ar[1],b,"="); base=b[2]+0; c=0;
      for (i=1;i<=n;i++) { split(ao[i],p,"="); split(ar[i],q,"=");
        if (p[2]!=q[2] && q[2]+0<base) c++ }
      cls = c ? "CPOS" : "ORDER" }
    print cls " " x; print "  o: " o; print "  r: " r }'
  exit 0
fi

probes=$(cd "$SRC" && ls *.ml 2>/dev/null | sed 's/\.ml$//' | sort -V)
[ -n "${ONLY:-}" ] && probes=$ONLY
[ -n "$probes" ] || { echo "no probes in $SRC"; exit 1; }
mkdir -p "$WORK/v"
printf '%s\n' $probes | HOOKENV="${HOOKENV:-}" BIN="$BIN" \
  xargs -P "$JOBS" -I{} \
    sh -c "bash '$SELF' --worker {} '$WORK/{}' > '$WORK/v/{}.txt'"
cat "$WORK"/v/*.txt > "$WORK/all.txt"
[ -n "${OUT:-}" ] && cp "$WORK/all.txt" "$OUT"
grep "^CPOS " "$WORK/all.txt" | awk -v t="$TAG" '{print t, $0}'
[ "${V:-0}" = 1 ] && grep -E "^(TOTAL|ORDER|SHAPE) " "$WORK/all.txt" |
  awk -v t="$TAG" '{print t, $0}'
n() { grep -c "^$1 " "$WORK/all.txt"; }
echo "$TAG: $(printf '%s\n' $probes | wc -l) probes: SAME $(n SAME)," \
  "CPOS $(n CPOS), ORDER $(n ORDER), TOTAL $(n TOTAL), SHAPE $(n SHAPE)," \
  "FAIL $(n FAIL)"
