#!/usr/bin/env bash
# The crash scan over testsuite/tests/*/*.ml: every file our compiler dies
# on with a signal (rc >= 128), one line per file.  A slice is gated on this
# SET being unchanged against the hooked (reverted) binary -- compare
# `sort`ed outputs, the scan runs in parallel.
#   BIN=<compiler> OUT=<file> bash cxx/harness/crash.sh
R="$(cd "$(dirname "$0")/../.." && pwd -P)"
. "$R/cxx/harness/portable.sh"
BIN=${BIN:?}; OUT=${OUT:?}; : > "$OUT"
scan() { local f=$1 d rc; d=$(mktemp -d)
  cp "$f" "$d/m.ml" 2>/dev/null || { rm -rf "$d"; return; }
  ( cd "$d" && timeout 20 "$BIN" -nostdlib -I "$R/stdlib" -c m.ml >/dev/null 2>&1 ); rc=$?
  [ "$rc" -ge 128 ] && echo "$f rc=$rc" >> "$OUT"
  rm -rf "$d"; }
export -f scan; export R BIN OUT
ls $R/testsuite/tests/*/*.ml | xargs -P 16 -I{} bash -c 'scan "$@"' _ {} 2>/dev/null
echo "crashing=$(($(wc -l < "$OUT")))"
