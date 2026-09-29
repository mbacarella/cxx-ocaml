#!/usr/bin/env bash
# Parse-tree parity over a source tree (cxx/PORTING.md): every .ml under the
# given directories parsed by compiler-libs (typing_dump.ml `parse`, built by
# typing_parse_parity.sh) and by the C++ parser (c++typing-dump `parse`),
# the dumps compared.  For third-party code (opam package sources): dune's
# bootstrap found five parser divergences the testsuite did not.
# Usage: parse_dir_parity.sh DIR...   (JOBS=)
#   SAME / DIFF / CFAIL (only the port fails) / OFAIL (only compiler-libs) / BOTHFAIL
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
DUMP_ML=/tmp/typing_cmi_parity/bin/typing_dump
CPP=${CPP:-$ROOT/cxx/build-release/c++typing-dump}
[ -x "$DUMP_ML" ] || { echo "run typing_parse_parity.sh first (it builds $DUMP_ML)" >&2; exit 1; }
export ROOT DUMP_ML CPP
if [ "${1:-}" == "--worker" ]; then
  f="$2"
  o=$("$ROOT/runtime/ocamlrun" "$DUMP_ML" parse "$f" 2>/dev/null); orc=$?
  c=$("$CPP" parse "$f" 2>/dev/null); crc=$?
  if [ $orc -ne 0 ] && [ $crc -ne 0 ]; then echo "BOTHFAIL $f"
  elif [ $orc -ne 0 ]; then echo "OFAIL $f"
  elif [ $crc -ne 0 ]; then echo "CFAIL $f"
  elif [ "$o" = "$c" ]; then echo "SAME $f"
  else echo "DIFF $f"; fi
  exit 0
fi
ulimit -v 8000000
res=$(find "$@" -name '*.ml' | sort | xargs -P "${JOBS:-12}" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.parse_dir_parity_results
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "files %d: SAME %d  DIFF %d  CFAIL %d  OFAIL %d  BOTHFAIL %d\n", NR, f["SAME"], f["DIFF"], f["CFAIL"], f["OFAIL"], f["BOTHFAIL"] }'
echo "per-file results: /tmp/.parse_dir_parity_results"
