#!/usr/bin/env bash
# Stage-9 oracle (cxx/PORTING.md): `-i`, the signature Printtyp prints --
# Out_type's naming of type variables, aliases and identifiers, and
# Oprint's layout -- against ocamlc.opt's, byte for byte (stdout and the
# exit code; ERR=1 compares stderr too).  Each .ml / .mli is compiled alone
# in a scratch directory by ocamlc.opt and by c++ocamlc.
#
# Usage: intf_parity.sh [file.ml|file.mli ...]   (JOBS=, FLAGS= extra flags
#   for both compilers, e.g. absolute -I dirs)
#   no args: cxx/harness/stamp_probes
#   SAME / DIFF / CFAIL (c++ocamlc failed where ocamlc did not) / CACCEPT
#   (the reverse) / OFAIL (both failed)
set -u
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
FLAGS="${FLAGS:-}"
ERR="${ERR:-0}"
export FLAGS ERR
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
OUT=/tmp/intf_parity

if [ "${1:-}" == "--worker" ]; then
  f="$2"; b=$(basename "$f"); key=$(echo "$f" | tr '/' '_')
  w=$(mktemp -d); mkdir "$w/o" "$w/c"; cp "$f" "$w/o/"; cp "$f" "$w/c/"
  ( cd "$w/o" && timeout 120 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" $FLAGS -i "$b" ) \
      >"$OUT/$key.o" 2>"$OUT/$key.oe"; orc=$?
  ( cd "$w/c" && timeout 120 "$CPP" -I "$ROOT/stdlib" $FLAGS -i "$b" ) \
      >"$OUT/$key.c" 2>"$OUT/$key.ce"; crc=$?
  rm -rf "${w:?}"
  echo "rc=$orc" >>"$OUT/$key.o"; echo "rc=$crc" >>"$OUT/$key.c"
  if [ "$ERR" = 1 ]; then cat "$OUT/$key.oe" >>"$OUT/$key.o"; cat "$OUT/$key.ce" >>"$OUT/$key.c"; fi
  if [ $orc -ne 0 ] && [ $crc -ne 0 ] && [ "$ERR" != 1 ]; then printf 'OFAIL %s\n' "$f"
  elif [ $orc -ne 0 ] && [ $crc -eq 0 ]; then printf 'CACCEPT %s\n' "$f"
  elif cmp -s "$OUT/$key.o" "$OUT/$key.c"; then printf 'SAME %s\n' "$f"
  elif [ $crc -ne 0 ] && [ $orc -eq 0 ]; then printf 'CFAIL %s\n' "$f"
  else printf 'DIFF %s\n' "$f"; fi
  exit 0
fi

rm -rf "$OUT"; mkdir -p "$OUT"
if [ $# -gt 0 ]; then files=("$@"); else lines_into files < <(ls cxx/harness/stamp_probes/*.ml); fi
vm_limit 8000000
res=$(printf '%s\n' ${files[@]+"${files[@]}"} | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.intf_parity_results
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "files %d: SAME %d  DIFF %d  CFAIL %d  CACCEPT %d  OFAIL %d\n", NR, f["SAME"], f["DIFF"], f["CFAIL"], f["CACCEPT"], f["OFAIL"] }'
echo "per-file results: /tmp/.intf_parity_results (outputs in $OUT: <file>.o ocamlc, <file>.c port)"
