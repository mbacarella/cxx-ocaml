#!/usr/bin/env bash
# Stage-10 oracle (TYPECHECKER.md): the Lambda the lambda/ port produces
# against ocamlc's, as text -- `-drawlambda` (Translmod's output) and
# `-dlambda` (after Simplif) -- byte for byte, stamps included (the
# typing port allocates them as ocamlc does).  Each file is compiled alone
# in a scratch directory against the stdlib, by ocamlc.opt and by
# c++ocamlc's new translation path (CPPCAML_NEWLAMBDA=1).
#
# Usage: lambda_port_parity.sh [file.ml ...]   (JOBS=, DUMP=drawlambda|dlambda,
#   FLAGS= extra flags for both compilers, e.g. absolute -I dirs)
#   no args: cxx/harness/stamp_probes
#   SAME / DIFF / CFAIL (the port failed or produced nothing) / OFAIL
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
DUMP="${DUMP:-drawlambda}"
FLAGS="${FLAGS:-}"
export DUMP FLAGS
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
OUT=/tmp/lambda_port_parity

if [ "${1:-}" == "--worker" ]; then
  f="$2"; b=$(basename "$f"); key=$(echo "$f" | tr '/' '_')
  w=$(mktemp -d); mkdir "$w/o" "$w/c"; cp "$f" "$w/o/"; cp "$f" "$w/c/"
  i="${f%.ml}.mli"
  if [ -f "$i" ]; then  # a sibling interface: each compiler compiles it first
    cp "$i" "$w/o/"; cp "$i" "$w/c/"
    ( cd "$w/o" && timeout 120 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -w -a $FLAGS -c "${b}i" ) >/dev/null 2>&1
    ( cd "$w/c" && timeout 120 "$CPP" -I "$ROOT/stdlib" -w -a $FLAGS -c "${b}i" ) >/dev/null 2>&1
  fi
  ( cd "$w/o" && timeout 120 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -w -a $FLAGS -"$DUMP" -c "$b" ) \
      >/dev/null 2>"$OUT/$key.o"; orc=$?
  ( cd "$w/c" && CPPCAML_NEWLAMBDA=1 timeout 120 "$CPP" -I "$ROOT/stdlib" -w -a $FLAGS -"$DUMP" -stop-after lambda -c "$b" ) \
      >/dev/null 2>"$OUT/$key.c"; crc=$?
  rm -rf "${w:?}"
  if [ $orc -ne 0 ]; then printf 'OFAIL %s\n' "$f"
  elif [ $crc -ne 0 ] || [ ! -s "$OUT/$key.c" ]; then printf 'CFAIL %s\n' "$f"
  elif cmp -s "$OUT/$key.o" "$OUT/$key.c"; then printf 'SAME %s\n' "$f"
  else printf 'DIFF %s\n' "$f"; fi
  exit 0
fi

rm -rf "$OUT"; mkdir -p "$OUT"
if [ $# -gt 0 ]; then files=("$@"); else mapfile -t files < <(ls cxx/harness/stamp_probes/*.ml); fi
ulimit -v 8000000
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.lambda_port_parity_results
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "files %d: SAME %d  DIFF %d  CFAIL %d  OFAIL %d\n", NR, f["SAME"], f["DIFF"], f["CFAIL"], f["OFAIL"] }'
echo "per-file results: /tmp/.lambda_port_parity_results (dumps in $OUT: <file>.o ocamlc, <file>.c port)"
