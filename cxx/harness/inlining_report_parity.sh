#!/usr/bin/env bash
# -inlining-report oracle (flambda tree, cxx/PORTING.md): each file is
# compiled with -inlining-report by ocamlopt.opt and by c++ocamlopt, each in
# a directory of its own, and every <file>.<round>.inlining.org compared
# byte for byte (the same set of rounds too).
#
# Usage: inlining_report_parity.sh [file.ml ...]  (JOBS=, FLAGS= for both,
#   e.g. -O3 for three rounds)  no args: cxx/harness/stamp_probes
#   SAME / DIFF / CFAIL (the port failed) / OFAIL (ocamlopt.opt failed)
set -u
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
JOBS="${JOBS:-8}"
FLAGS="${FLAGS:-}"
OUT=/tmp/inlining_report_parity
rm -rf "${OUT:?}"; mkdir -p "$OUT"
ulimit -v 8000000
one() {
  f="$1"; key=$(echo "$f" | tr '/' '_'); b=$(basename "$f" .ml)
  for who in o c; do
    d="$OUT/$key.$who"; mkdir -p "$d"; cp "$f" "$d/"
    if [ $who = o ]; then C="$ROOT/ocamlopt.opt -nostdlib -I $ROOT/stdlib"; else C="$ROOT/cxx/build-release/c++ocamlopt -I $ROOT/stdlib"; fi
    # the interface first, by ocamlopt.opt for both
    if [ -f "${f}i" ]; then cp "${f}i" "$d/"; ( cd "$d" && timeout 120 $ROOT/ocamlopt.opt -nostdlib -I $ROOT/stdlib -w -a $FLAGS -c "$b.mli" ) >/dev/null 2>&1; fi
    ( cd "$d" && timeout 120 $C -w -a $FLAGS -inlining-report -c "$b.ml" ) >/dev/null 2>&1
    echo $? > "$d/rc"
  done
  o="$OUT/$key.o"; c="$OUT/$key.c"
  if [ "$(cat $o/rc)" != 0 ]; then echo "OFAIL $f"; return; fi
  if [ "$(cat $c/rc)" != 0 ]; then echo "CFAIL $f"; return; fi
  lo=$(cd "$o" && ls *.inlining.org 2>/dev/null); lc=$(cd "$c" && ls *.inlining.org 2>/dev/null)
  if [ "$lo" != "$lc" ]; then echo "DIFF $f"; return; fi
  for x in $lo; do cmp -s "$o/$x" "$c/$x" || { echo "DIFF $f"; return; }; done
  echo "SAME $f"
}
export -f one; export ROOT FLAGS OUT
if [ $# -eq 0 ]; then set -- "$ROOT"/cxx/harness/stamp_probes/*.ml; fi
printf '%s\n' "$@" | xargs -P "$JOBS" -I{} bash -c 'one "$@"' _ {} > /tmp/.inlining_report_parity_results
r=/tmp/.inlining_report_parity_results
echo "files $(wc -l < $r): SAME $(grep -c ^SAME $r)  DIFF $(grep -c ^DIFF $r)  CFAIL $(grep -c ^CFAIL $r)  OFAIL $(grep -c ^OFAIL $r)"
echo "per-file results: $r (outputs in $OUT/<file>.o / .c)"
