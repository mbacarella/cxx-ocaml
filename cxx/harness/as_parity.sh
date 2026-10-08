#!/usr/bin/env bash
# The in-process assembler (cxx/PORTING.md, x86_assembler.cpp) against GNU
# as, byte for byte: each file is compiled to assembly by ocamlopt.opt
# (`-S -c`), then assembled by `as` and by `c++as`, in the same directory
# (-g records it: DW_AT_comp_dir), and the two objects compared.  amd64
# Linux only; the oracle is the local binutils (2.42 matches).
#
# Usage: as_parity.sh [file.ml ...]  (JOBS=, FLAGS= for ocamlopt.opt, e.g.
#   FLAGS=-g, FLAGS="-g -function-sections")
#   no args: cxx/harness/stamp_probes
#   SAME / DIFF / UNSUP (c++as left it to as: unsupported text) / OFAIL
set -u
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
FLAGS="${FLAGS:-}"
REFC="$ROOT/ocamlopt.opt"
CPPAS="${CPPAS:-$ROOT/cxx/build-release/c++as}"
export FLAGS REFC CPPAS
OUT=/tmp/as_parity

if [ "${1:-}" == "--worker" ]; then
  f="$2"; b=$(basename "$f"); m="${b%.ml}"; key=$(echo "$f" | tr '/' '_')
  i="${f%.ml}.mli"
  w=$(mktemp -d)
  cp "$f" "$w/"; [ -f "$i" ] && cp "$i" "$w/"
  ( cd "$w" || exit 1
    [ -f "$i" ] && { timeout 120 "$REFC" -nostdlib -I "$ROOT/stdlib" -w -a $FLAGS -c "${b}i" >/dev/null 2>&1 || exit 1; }
    timeout 120 "$REFC" -nostdlib -I "$ROOT/stdlib" -w -a $FLAGS -S -c "$b" >/dev/null 2>&1 || exit 1
    as -o "$m.as.o" "$m.s" >/dev/null 2>&1 || exit 1 ); orc=$?
  if [ $orc -ne 0 ]; then r="OFAIL"
  else
    ( cd "$w" && timeout 120 "$CPPAS" -o "$m.cx.o" "$m.s" 2>err ); crc=$?
    if [ $crc -eq 3 ]; then r="UNSUP"
    elif [ $crc -ne 0 ] || ! cmp -s "$w/$m.as.o" "$w/$m.cx.o"; then
      r="DIFF"; cp "$w/$m.s" "$OUT/$key.s"; cp "$w/$m.as.o" "$OUT/$key.as.o"; cp "$w/$m.cx.o" "$OUT/$key.cx.o" 2>/dev/null
    else r="SAME"; fi
  fi
  rm -rf "${w:?}"
  printf '%s %s\n' "$r" "$f"
  exit 0
fi

rm -rf "$OUT"; mkdir -p "$OUT"
if [ $# -gt 0 ]; then files=("$@"); else lines_into files < <(ls cxx/harness/stamp_probes/*.ml); fi
vm_limit 8000000
res=$(printf '%s\n' ${files[@]+"${files[@]}"} | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.as_parity_results
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "files %d: SAME %d  DIFF %d  UNSUP %d  OFAIL %d\n", NR, f["SAME"], f["DIFF"], f["UNSUP"], f["OFAIL"] }'
echo "per-file results: /tmp/.as_parity_results (in $OUT: <file>.s, .as.o, .cx.o of the DIFFs)"
