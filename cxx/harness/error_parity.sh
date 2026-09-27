#!/usr/bin/env bash
# Stage-9 oracle (TYPECHECKER.md): the error report of a rejected program --
# Location's report printer, the source excerpt, each module's report_error
# -- against ocamlc.opt's, byte for byte (stderr and the exit code).  Each
# file is compiled alone in a scratch directory by ocamlc.opt and by
# c++ocamlc (`-w -a -stop-after typing` unless STOP= overrides; a sibling
# .mli is compiled first).  Files ocamlc accepts are counted, not compared.
#
# Usage: error_parity.sh [file.ml|file.mli ...]   (JOBS=, FLAGS= extra flags
#   for both compilers, STOP= the stop flag, default "-stop-after typing")
#   no args: cxx/harness/false_accept + cxx/harness/error_probes
#   SAME / DIFF (both reject, reports differ) / FACCEPT (c++ocamlc accepts
#   what ocamlc rejects) / FREJECT (the reverse) / OK (both accept)
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
FLAGS="${FLAGS:-}"
STOP="${STOP--stop-after typing}"
export FLAGS STOP
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
OUT=/tmp/error_parity

if [ "${1:-}" == "--worker" ]; then
  f="$2"; b=$(basename "$f"); key=$(echo "$f" | tr '/' '_')
  w=$(mktemp -d); mkdir "$w/o" "$w/c"; cp "$f" "$w/o/"; cp "$f" "$w/c/"
  case "$b" in
    *.ml)
      i="${f%.ml}.mli"
      if [ -f "$i" ]; then
        cp "$i" "$w/o/"; cp "$i" "$w/c/"
        ( cd "$w/o" && timeout 60 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -w -a $FLAGS -c "${b}i" ) >/dev/null 2>&1
        ( cd "$w/c" && timeout 60 "$CPP" -I "$ROOT/stdlib" -w -a $FLAGS -c "${b}i" ) >/dev/null 2>&1
      fi ;;
  esac
  ( cd "$w/o" && timeout 60 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -w -a $FLAGS $STOP -c "$b" ) \
      >"$OUT/$key.o.out" 2>"$OUT/$key.o"; orc=$?
  ( cd "$w/c" && timeout 60 "$CPP" -I "$ROOT/stdlib" -w -a $FLAGS $STOP -c "$b" ) \
      >"$OUT/$key.c.out" 2>"$OUT/$key.c"; crc=$?
  rm -rf "${w:?}"
  echo "$orc" >> "$OUT/$key.o"; echo "$crc" >> "$OUT/$key.c"
  if [ $orc -eq 0 ] && [ $crc -eq 0 ]; then printf 'OK %s\n' "$f"
  elif [ $orc -eq 0 ]; then printf 'FREJECT %s\n' "$f"
  elif [ $crc -eq 0 ]; then printf 'FACCEPT %s\n' "$f"
  elif cmp -s "$OUT/$key.o" "$OUT/$key.c"; then printf 'SAME %s\n' "$f"
  else printf 'DIFF %s\n' "$f"; fi
  exit 0
fi

rm -rf "$OUT"; mkdir -p "$OUT"
if [ $# -gt 0 ]; then files=("$@")
else mapfile -t files < <(ls cxx/harness/false_accept/*.ml cxx/harness/error_probes/*.ml* 2>/dev/null); fi
ulimit -v 8000000
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.error_parity_results
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "files %d: SAME %d  DIFF %d  FACCEPT %d  FREJECT %d  OK %d\n", NR, f["SAME"], f["DIFF"], f["FACCEPT"], f["FREJECT"], f["OK"] }'
echo "per-file results: /tmp/.error_parity_results (stderr+rc in $OUT: <file>.o ocamlc, <file>.c port)"
