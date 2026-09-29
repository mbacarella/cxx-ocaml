#!/usr/bin/env bash
# Stage-4b oracle for the typing/ port (cxx/PORTING.md): Typetexp.  For
# every `val` / `external` of an interface, run transl_type_scheme in
# Env.initial + open Stdlib + open the unit's own cmi, with compiler-libs
# (typing_dump.ml `typexp`) and with the port (c++typing-dump `typexp`), and
# compare the typed core types (every node with its type) or the error.
# Values whose types need Typemod (first-class modules, M.(t)) print
# UNSUPPORTED on the C++ side until stage 5.
#
# Usage: typing_typexp_parity.sh [file.mli ...]   (JOBS= overridable)
#   no args: stdlib, the compiler's interfaces and testsuite/tests
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
BIN=/tmp/typing_cmi_parity/bin
DUMP_ML=$BIN/typing_dump
CPP=${CPP:-$ROOT/cxx/build-release/c++typing-dump}
REF=/tmp/effid_ref
DIRS=stdlib:$REF

modname() {  # the cmi to open for an interface ("-": none)
  local f=$1 b
  b=$(basename "$f" .mli)
  case "$f" in
    stdlib/stdlib.mli) echo Stdlib ;;
    stdlib/camlinternal*) echo "${b^}" ;;
    stdlib/*) echo "Stdlib__${b^}" ;;
    testsuite/*) echo - ;;
    *) if [ -f "$REF/$b.cmi" ]; then echo "${b^}"; else echo -; fi ;;
  esac
}

if [ "${1:-}" == "--worker" ]; then
  f="$2"; m=$(modname "$f")
  W=/tmp/typing_typexp_parity; mkdir -p $W
  k=$(echo "$f" | tr '/' '_')
  runtime/ocamlrun "$DUMP_ML" typexp $DIRS "$m" "$f" > $W/$k.o 2>/dev/null; orc=$?
  "$CPP" typexp $DIRS "$m" "$f" > $W/$k.c 2>/dev/null; crc=$?
  if [ $orc -ne 0 ] && [ $crc -ne 0 ]; then printf 'BOTHFAIL %s\n' "$f"
  elif [ $orc -ne 0 ]; then printf 'OFAIL %s\n' "$f"
  elif [ $crc -ne 0 ]; then printf 'CFAIL %s\n' "$f"
  else
    python3 - "$W/$k.o" "$W/$k.c" "$f" <<'PY'
import sys
o = open(sys.argv[1], encoding='latin-1').read().splitlines()
c = open(sys.argv[2], encoding='latin-1').read().splitlines()
f = sys.argv[3]
if len(o) != len(c):
    print('LENDIFF', f)
else:
    for a, b in zip(o, c):
        if b.endswith(' => UNSUPPORTED'): print('UNSUP', f, a.split(' => ')[0])
        elif a == b: print('SAME', f, a.split(' => ')[0])
        else: print('DIFF', f, a.split(' => ')[0])
PY
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
[ -d $REF ] || { echo "no $REF: run cxx/harness/effid.sh first" >&2; exit 1; }
rm -rf /tmp/typing_typexp_parity
if [ $# -gt 0 ]; then files=("$@")
else mapfile -t files < <( (ls stdlib/*.mli utils/*.mli parsing/*.mli typing/*.mli \
    lambda/*.mli bytecomp/*.mli driver/*.mli file_formats/*.mli; \
    find testsuite/tests -name '*.mli') | sort -u)
fi
ulimit -v 8000000
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | sort > /tmp/.typing_typexp_parity_results
printf '%s\n' "$res" | awk '
  $1=="SAME"||$1=="DIFF"||$1=="UNSUP" {n[$1]++; v++; next}
  {f[$1]++}
  END{ printf "values: %d   SAME: %d   DIFF: %d   unsupported: %d   |  files c++-fail: %d  oracle-fail: %d  both-fail: %d  count-mismatch: %d\n",
       v, n["SAME"], n["DIFF"], n["UNSUP"], f["CFAIL"], f["OFAIL"], f["BOTHFAIL"], f["LENDIFF"] }'
echo "per-value results: /tmp/.typing_typexp_parity_results"
