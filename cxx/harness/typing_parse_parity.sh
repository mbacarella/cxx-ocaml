#!/usr/bin/env bash
# Stage-4a oracle for the typing/ port (cxx/PORTING.md): the Parsetree the
# typer consumes.  Parse each .ml with compiler-libs (Pparse, typing_dump.ml
# `parse`) and with the C++ parser + parsetree::of_ast (c++typing-dump
# `parse`), dump every field of both trees, and compare.  Locations the C++
# parser does not record yet print as "?" on both sides (cxx/PORTING.md).
#
# Usage: typing_parse_parity.sh [file.ml ...]   (JOBS= overridable)
#   no args: testsuite/tests, stdlib and the compiler's own sources
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
BIN=/tmp/typing_cmi_parity/bin
DUMP_ML=$BIN/typing_dump
CPP=${CPP:-$ROOT/cxx/build-release/c++typing-dump}

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  o=$(runtime/ocamlrun "$DUMP_ML" parse "$f" 2>/dev/null); orc=$?
  c=$("$CPP" parse "$f" 2>/dev/null); crc=$?
  if [ $orc -ne 0 ] && [ $crc -ne 0 ]; then printf 'BOTHFAIL %s\n' "$f"
  elif [ $orc -ne 0 ]; then printf 'OFAIL %s\n' "$f"
  elif [ $crc -ne 0 ]; then printf 'CFAIL %s\n' "$f"
  elif [ "$o" == "$c" ]; then printf 'SAME %s\n' "$f"
  else printf 'DIFF %s\n' "$f"; fi
  exit 0
fi

src=$ROOT/cxx/harness/typing_dump.ml
if ! [ -f "$DUMP_ML" ] || ! [ "$DUMP_ML" -nt "$src" ]; then
  mkdir -p "$BIN" && cp "$src" "$BIN/typing_dump.ml"
  ./ocamlc.opt -nostdlib -I stdlib -I compilerlibs -I utils -I typing -I parsing \
    -I file_formats -I driver compilerlibs/ocamlcommon.cma "$BIN/typing_dump.ml" \
    -o "$DUMP_ML" || { echo "FATAL: typing_dump build failed" >&2; exit 1; }
fi
if [ $# -gt 0 ]; then files=("$@")
else mapfile -t files < <( (find testsuite/tests -name '*.ml'; ls stdlib/*.ml \
    utils/*.ml parsing/*.ml typing/*.ml lambda/*.ml bytecomp/*.ml driver/*.ml \
    file_formats/*.ml) | sort -u)
fi
ulimit -v 8000000
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | sort -k2 > /tmp/.typing_parse_parity_results
printf '%s\n' "$res" | awk '
  {n[$1]++; t++}
  END{ printf "files: %d   SAME: %d   DIFF: %d   c++-fail: %d   oracle-fail: %d   both-fail: %d\n",
       t, n["SAME"], n["DIFF"], n["CFAIL"], n["OFAIL"], n["BOTHFAIL"] }'
echo "per-file results: /tmp/.typing_parse_parity_results"
