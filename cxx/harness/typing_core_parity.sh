#!/usr/bin/env bash
# Stage-4c oracle for the typing/ port (TYPECHECKER.md): Typecore.  Types
# the `let` / eval structure items of an implementation (Typecore.
# type_binding / type_expression in Env.initial + open Stdlib, then the
# delayed checks), with compiler-libs (typing_dump.ml `core`) and with the
# port (c++typing-dump `core`), and compares the full type graph of every
# bound value (levels, scopes, links, abbreviation memos, canonical ids) or
# the error (kind and location).  Both stop at the first other item (STOP)
# until Typemod exists; the port prints UNSUPPORTED when an unported forward
# (Typemod / Typeclass) is reached.
#
# Usage: typing_core_parity.sh [file.ml ...]   (JOBS= overridable)
#   no args: cxx/harness/core_probes, stdlib and testsuite/tests
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
BIN=/tmp/typing_cmi_parity/bin
DUMP_ML=$BIN/typing_dump
CPP=${CPP:-$ROOT/cxx/build-release/c++typing-dump}
DIRS=stdlib:otherlibs/unix:otherlibs/str:otherlibs/systhreads:otherlibs/dynlink:otherlibs/runtime_events

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  W=/tmp/typing_core_parity; mkdir -p $W
  k=$(echo "$f" | tr '/' '_')
  runtime/ocamlrun "$DUMP_ML" core $DIRS "$f" > $W/$k.o 2>/dev/null; orc=$?
  ( ulimit -s unlimited; "$CPP" core $DIRS "$f" > $W/$k.c 2>/dev/null ); crc=$?
  n=$(grep -c '^val \|^eval ' $W/$k.o)
  if [ $orc -ne 0 ] && [ $crc -ne 0 ]; then printf 'BOTHFAIL %s\n' "$f"
  elif [ $orc -ne 0 ]; then printf 'OFAIL %s\n' "$f"
  elif [ $crc -ne 0 ]; then printf 'CFAIL %s\n' "$f"
  elif grep -q '^UNSUPPORTED' $W/$k.c; then printf 'UNSUP %s %s\n' "$f" "$n"
  elif cmp -s $W/$k.o $W/$k.c; then
    if grep -q '^ERR' $W/$k.o; then printf 'SAME-ERR %s %s\n' "$f" "$n"
    else printf 'SAME %s %s\n' "$f" "$n"; fi
  else printf 'DIFF %s %s\n' "$f" "$n"
  fi
  exit 0
fi

src=$ROOT/cxx/harness/typing_dump.ml
if ! [ -f "$DUMP_ML" ] || ! [ "$DUMP_ML" -nt "$src" ]; then
  mkdir -p "$BIN" && cp "$src" "$BIN/typing_dump.ml"
  ./ocamlc.opt -nostdlib -I stdlib -I compilerlibs -I utils -I typing -I parsing \
    -I file_formats -I driver compilerlibs/ocamlcommon.cma "$BIN/typing_dump.ml" \
    -o "$DUMP_ML" || { echo "FATAL: typing_dump build failed" >&2; exit 1; }
fi
rm -rf /tmp/typing_core_parity
if [ $# -gt 0 ]; then files=("$@")
else mapfile -t files < <( (ls cxx/harness/core_probes/*.ml stdlib/*.ml; \
    find testsuite/tests -name '*.ml') | sort -u)
fi
ulimit -v 8000000
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | sort > /tmp/.typing_core_parity_results
printf '%s\n' "$res" | awk '
  {f[$1]++; if (NF>=3) it[$1]+=$3}
  END{ printf "files: SAME %d (%d items)  SAME-ERR %d  DIFF %d  UNSUP %d  |  c++-fail %d  oracle-fail %d  both-fail %d\n",
       f["SAME"], it["SAME"], f["SAME-ERR"], f["DIFF"], f["UNSUP"], f["CFAIL"], f["OFAIL"], f["BOTHFAIL"] }'
echo "per-file results: /tmp/.typing_core_parity_results (outputs in /tmp/typing_core_parity)"
