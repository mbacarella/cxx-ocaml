#!/usr/bin/env bash
# The .cmi FIRST-STAMP probe battery, in the repo (the cmi_stamps.sh twin of
# effid_battery.sh).
#
# Each probe is a self-contained .ml under cxx/harness/stamp_probes/, compiled
# by ocamlc.opt and by c++ocamlc with `-nostdlib -I stdlib -c`, and scored the
# way cmi_stamps.sh scores the testsuite: the first Ident stamp of the saved
# signature, ours minus ocamlc's.  A negative delta is an UNDER-count (idents
# typing allocated that the model does not charge), a positive one an
# OVER-count.  `pins.txt` records the delta each probe measured when it was
# added, so a run reports every probe that MOVED and whether it moved toward
# zero (BETTER), away from it (WORSE), or crossed it (OVER).
#
# It lives here rather than in /tmp because /tmp does not survive a session:
# a 4134-probe battery was lost that way on 2026-09-14 (and a 648-probe effid
# battery on 2026-09-09).  A probe is CHEAP; add one for every shape a slice
# touches, both the mover and its controls, and re-pin after the slice ships.
#
#   BIN=<compiler>            default cxx/build-release/c++ocamlc
#   HOOKENV="env NOFOO=1"     run OUR side under a revert hook (needs the
#                             leading `env`)
#   ONLY=xtapp_a1             run one probe
#   V=1                       list every probe, not just the movers
#   PIN=1                     rewrite pins.txt from this run
#
# Usage: bash cxx/harness/stamp_battery.sh [tag]
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
SRC="$ROOT/cxx/harness/stamp_probes"
PINS="$SRC/pins.txt"
BIN=${BIN:-$ROOT/cxx/build-release/c++ocamlc}
TAG=${1:-run}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The first signature item's Ident stamp: in the marshalled (name, sign) pair
# that is the second string of the dump followed by its stamp.
base() {
  timeout 60 perl "$ROOT/cxx/harness/mdump.pl" "$1" 2>/dev/null |
    perl -ne 's/^\s+//; if (/^"/) { $k++; next }
              print and exit if $k >= 2 && /^-?\d+$/'
}

probes=$(cd "$SRC" && ls *.ml 2>/dev/null | sed 's/\.ml$//' | sort -V)
[ -n "${ONLY:-}" ] && probes=$ONLY
[ -n "$probes" ] || { echo "no probes in $SRC"; exit 1; }

tot=0; fail=0; moved=0; worse=0; over=0; newpins=""
for x in $probes; do
  tot=$((tot+1))
  w="$WORK/$x"; mkdir -p "$w/o" "$w/c"
  cp "$SRC/$x.ml" "$w/o/"; cp "$SRC/$x.ml" "$w/c/"
  ( cd "$w/o" && timeout 20 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -c "$x.ml" ) \
    >"$w/o.log" 2>&1
  ( cd "$w/c" && timeout 20 ${HOOKENV:-env} "$BIN" -nostdlib -I "$ROOT/stdlib" -c "$x.ml" ) \
    >"$w/c.log" 2>&1
  if [ ! -f "$w/o/$x.cmi" ]; then echo "$TAG $x ORACLE-FAIL"; fail=$((fail+1)); continue; fi
  if [ ! -f "$w/c/$x.cmi" ]; then echo "$TAG $x CPP-FAIL"; fail=$((fail+1)); continue; fi
  rb=$(base "$w/o/$x.cmi"); ob=$(base "$w/c/$x.cmi")
  if [ -z "$rb" ] || [ -z "$ob" ]; then echo "$TAG $x EMPTY-SIG"; fail=$((fail+1)); continue; fi
  d=$((ob-rb)); newpins="$newpins$x $d"$'\n'
  pin=$(awk -v x="$x" '$1==x{print $2}' "$PINS" 2>/dev/null)
  st=OK
  if [ -z "$pin" ]; then st=UNPINNED
  elif [ "$pin" -ne "$d" ]; then
    moved=$((moved+1))
    ad=${d#-}; ap=${pin#-}
    if [ "$d" -gt 0 ] && [ "$pin" -le 0 ]; then st=OVER; over=$((over+1))
    elif [ "$ad" -gt "$ap" ]; then st=WORSE; worse=$((worse+1))
    else st=BETTER; fi
  fi
  if [ "${V:-0}" = 1 ] || [ "$st" != OK ]; then
    printf '%s %s %+d (pinned %s) %s\n' "$TAG" "$x" "$d" "${pin:-none}" "$st"
  fi
done
[ "${PIN:-0}" = 1 ] && { printf '%s' "$newpins" | sort -V > "$PINS"; echo "pinned $tot probes"; }
echo "$TAG: $tot probes, $fail failed, $moved moved ($worse WORSE, $over OVER)"
