#!/usr/bin/env bash
# The effective-identity PROBE BATTERY, in the repo.
#
# Each probe is a self-contained .ml under cxx/harness/effid_probes/, compiled
# by ocamlc.opt and by c++ocamlc and compared three ways: the NORMALISED
# `dumpobj -effid` diff (digits -> N, so a pure operand shift counts 0), the RAW
# diff, and the output of running the linked program.  A probe pins one shape:
# either a divergence a slice closed (it must stay 0/0) or a known residual
# (its counts are the recorded value).
#
# It lives here rather than in /tmp because /tmp does not survive a session on
# every machine -- a 648-probe battery was lost that way on 2026-09-09, which
# is what this file exists to prevent.  A probe is CHEAP; add one for every
# shape a slice touches, both the mover and its controls.
#
# Optional companions, compiled and linked AHEAD of the probe (the only way to
# give a probe imported modules): <x>n.ml, <x>m.ml, and their .mli.
#
#   BIN=<compiler>            default cxx/build-release/c++ocamlc
#   HOOKENV="env NOFOO=1"     run OUR side under a revert hook (needs the
#                             leading `env`)
#   ONLY=s636                 run one probe
#   V=1                       list every probe, not just the non-zero ones
#
# Usage: bash cxx/harness/effid_battery.sh [tag]
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
SRC="$ROOT/cxx/harness/effid_probes"
BIN=${BIN:-$ROOT/cxx/build-release/c++ocamlc}
TAG=${1:-run}
D="$ROOT/runtime/ocamlrun $ROOT/tools/dumpobj -effid"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

probes=$(cd "$SRC" && ls *.ml 2>/dev/null | grep -vE '[nm]\.ml$' | sed 's/\.ml$//' | sort -V)
[ -n "${ONLY:-}" ] && probes=$ONLY
[ -n "$probes" ] || { echo "no probes in $SRC"; exit 1; }

nz=0; tot=0; xd=0
for x in $probes; do
  tot=$((tot+1))
  w="$WORK/$x"; mkdir -p "$w/o" "$w/c"
  aux=""
  for s in n m; do
    [ -f "$SRC/$x$s.ml" ] || continue
    cp "$SRC/$x$s.ml" "$w/o/"; cp "$SRC/$x$s.ml" "$w/c/"
    [ -f "$SRC/$x$s.mli" ] && { cp "$SRC/$x$s.mli" "$w/o/"; cp "$SRC/$x$s.mli" "$w/c/"; }
    aux="$aux $x$s.ml"
  done
  cp "$SRC/$x.ml" "$w/o/"; cp "$SRC/$x.ml" "$w/c/"
  [ -f "$SRC/$x.mli" ] && { cp "$SRC/$x.mli" "$w/o/"; cp "$SRC/$x.mli" "$w/c/"; }
  # -nostdlib keeps the DEFAULT stdlib dir off the search path; the implicit
  # stdlib.cma / std_exit.cmo still link (Bytelink gates them on -nopervasives),
  # so both command lines below are the same modulo the compiler.
  ( cd "$w/o" && "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" $aux "$x.ml" -o o.exe ) \
    >"$w/o.log" 2>&1
  ( cd "$w/c" && ${HOOKENV:-env} "$BIN" -nostdlib -I "$ROOT/stdlib" $aux "$x.ml" -o c.exe ) \
    >"$w/c.log" 2>&1
  if [ ! -f "$w/o/$x.cmo" ]; then echo "$TAG $x ORACLE-FAIL"; nz=$((nz+1)); continue; fi
  if [ ! -f "$w/c/$x.cmo" ]; then echo "$TAG $x CPP-FAIL"; nz=$((nz+1)); continue; fi
  norm=$(diff <($D "$w/c/$x.cmo" 2>/dev/null | perl -pe 's/\d+/N/g') \
              <($D "$w/o/$x.cmo" 2>/dev/null | perl -pe 's/\d+/N/g') | grep -cE '^[<>]')
  raw=$(diff <($D "$w/c/$x.cmo" 2>/dev/null) <($D "$w/o/$x.cmo" 2>/dev/null) | grep -cE '^[<>]')
  oo=$( (cd "$w/o" && timeout 10 "$ROOT/runtime/ocamlrun" ./o.exe 2>&1); echo "rc=$?")
  co=$( (cd "$w/c" && timeout 10 "$ROOT/runtime/ocamlrun" ./c.exe 2>&1); echo "rc=$?")
  ex=OK; [ "$oo" = "$co" ] || { ex="EXEC-DIFF"; xd=$((xd+1)); }
  [ "$raw" -eq 0 ] || nz=$((nz+1))
  if [ "${V:-0}" = 1 ] || [ "$raw" -ne 0 ] || [ "$ex" != OK ]; then
    echo "$TAG $x effid=$norm raw=$raw $ex"
    [ "$ex" = OK ] || { echo "    oracle: $oo"; echo "    ours  : $co"; }
  fi
done
echo "$TAG: $tot probes, $((tot-nz)) byte-identical, $nz divergent, $xd exec-diff"
