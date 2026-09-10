#!/usr/bin/env bash
# The "qmark" detector over testsuite/tests/*/*.ml: every `?name/N` an
# unresolved identifier leaves in our -dlambda output, one line per (file,
# ident).  A slice is gated on this SET being unchanged against the hooked
# (reverted) binary -- compare `sort`ed outputs, the scan runs in parallel.
#   BIN=<compiler> OUT=<file> bash cxx/harness/qmark.sh
R="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
BIN=${BIN:?}; OUT=${OUT:?}; : > "$OUT"
scan() { local f=$1 d; d=$(mktemp -d)
  cp "$f" "$d/m.ml" 2>/dev/null || { rm -rf "$d"; return; }
  ( cd "$d" && timeout 10 "$BIN" -nostdlib -I "$R/stdlib" -dlambda -c m.ml 2>/dev/null ) \
    | grep -oE '\?[A-Za-z_][A-Za-z0-9_]*/[0-9]+' | sort -u | sed "s|^|$f |" >> "$OUT"
  rm -rf "$d"; }
export -f scan; export R BIN OUT
ls $R/testsuite/tests/*/*.ml | xargs -P 16 -I{} bash -c 'scan "$@"' _ {}
echo "files=$(awk '{print $1}' "$OUT" | sort -u | wc -l) lines=$(wc -l < "$OUT")"
