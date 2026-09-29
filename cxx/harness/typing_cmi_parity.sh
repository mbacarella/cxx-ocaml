#!/usr/bin/env bash
# Stage-1 oracle for the typing/ port (cxx/PORTING.md): decode every .cmi in
# the tree with the port (c++typing-dump) and with compiler-libs
# (typing_dump.ml, run on the bytecode runtime), and compare the two
# structural dumps byte for byte.
#
# Usage: typing_cmi_parity.sh [file.cmi ...]   (JOBS= overridable)
#   no args: every .cmi under the repo (excluding _build/ and testsuite/)
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
BIN=/tmp/typing_cmi_parity/bin
DUMP_ML=$BIN/typing_dump
CPP=${CPP:-$ROOT/cxx/build-release/c++typing-dump}

build_oracle() {
  src=$ROOT/cxx/harness/typing_dump.ml
  [ -f "$DUMP_ML" ] && [ "$DUMP_ML" -nt "$src" ] && return 0
  mkdir -p "$BIN" && cp "$src" "$BIN/typing_dump.ml"
  ./ocamlc.opt -nostdlib -I stdlib -I compilerlibs -I utils -I typing -I parsing \
    -I file_formats -I driver compilerlibs/ocamlcommon.cma "$BIN/typing_dump.ml" \
    -o "$DUMP_ML" || { echo "FATAL: typing_dump build failed" >&2; exit 1; }
}

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  o=$(runtime/ocamlrun "$DUMP_ML" "$f" 2>/dev/null); orc=$?
  c=$("$CPP" "$f" 2>/dev/null); crc=$?
  if [ $orc -ne 0 ]; then printf 'OFAIL %s\n' "$f"
  elif [ $crc -ne 0 ]; then printf 'CFAIL %s\n' "$f"
  elif [ "$o" == "$c" ]; then printf 'SAME %s\n' "$f"
  else printf 'DIFF %s\n' "$f"; fi
  exit 0
fi

build_oracle
if [ $# -gt 0 ]; then files=("$@")
else mapfile -t files < <(find . -name '*.cmi' -not -path './_build/*' \
                           -not -path './testsuite/*' | sort)
fi
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | sort -k2 > /tmp/.typing_cmi_parity_results
printf '%s\n' "$res" | awk '
  {n[$1]++; t++}
  END{ printf "cmis: %d   SAME: %d   DIFF: %d   c++-fail: %d   oracle-fail: %d\n",
       t, n["SAME"], n["DIFF"], n["CFAIL"], n["OFAIL"] }'
echo "per-file results: /tmp/.typing_cmi_parity_results"
