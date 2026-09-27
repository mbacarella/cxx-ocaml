#!/usr/bin/env bash
# Stage-9c oracle (TYPECHECKER.md): the warnings and alerts c++ocamlc
# prints -- their text, their order, warnings-as-errors and the exit code --
# against ocamlc.opt's, byte for byte (stderr and the exit code).  Each file
# is compiled alone in a scratch directory by ocamlc.opt and by c++ocamlc
# (a full compile: some warnings come from the translators; a sibling .mli
# is compiled first, its own warnings included).
#
# Usage: warning_parity.sh [file.ml|file.mli ...]   (JOBS=, W= the warning
#   flags, default none = ocamlc's default warnings (W="-w +a" for all),
#   FLAGS= extra flags for both compilers)
#   no args: cxx/harness/warning_probes
#   SAME (stderr + rc identical) / DIFF / QUIET (both print nothing, rc 0)
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
FLAGS="${FLAGS:-}"
W="${W:-}"
export FLAGS W
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
OUT=/tmp/warning_parity

if [ "${1:-}" == "--worker" ]; then
  f="$2"; b=$(basename "$f"); key=$(echo "$f" | tr '/' '_')
  w=$(mktemp -d); mkdir "$w/o" "$w/c"; cp "$f" "$w/o/"; cp "$f" "$w/c/"
  : >"$OUT/$key.o"; : >"$OUT/$key.c"
  case "$b" in
    *.ml)
      i="${f%.ml}.mli"
      if [ -f "$i" ]; then
        cp "$i" "$w/o/"; cp "$i" "$w/c/"
        ( cd "$w/o" && timeout 60 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" $W $FLAGS -c "${b}i" ) >/dev/null 2>>"$OUT/$key.o"
        ( cd "$w/c" && timeout 60 "$CPP" -I "$ROOT/stdlib" $W $FLAGS -c "${b}i" ) >/dev/null 2>>"$OUT/$key.c"
      fi ;;
  esac
  ( cd "$w/o" && timeout 60 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" $W $FLAGS -c "$b" ) \
      >/dev/null 2>>"$OUT/$key.o"; orc=$?
  ( cd "$w/c" && timeout 60 "$CPP" -I "$ROOT/stdlib" $W $FLAGS -c "$b" ) \
      >/dev/null 2>>"$OUT/$key.c"; crc=$?
  rm -rf "${w:?}"
  quiet=0; [ ! -s "$OUT/$key.o" ] && [ ! -s "$OUT/$key.c" ] && [ $orc -eq 0 ] && [ $crc -eq 0 ] && quiet=1
  echo "$orc" >> "$OUT/$key.o"; echo "$crc" >> "$OUT/$key.c"
  if [ $quiet -eq 1 ]; then printf 'QUIET %s\n' "$f"
  elif cmp -s "$OUT/$key.o" "$OUT/$key.c"; then printf 'SAME %s\n' "$f"
  else printf 'DIFF %s\n' "$f"; fi
  exit 0
fi

rm -rf "$OUT"; mkdir -p "$OUT"
if [ $# -gt 0 ]; then files=("$@")
else mapfile -t files < <(ls cxx/harness/warning_probes/*.ml 2>/dev/null); fi
ulimit -v 8000000
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.warning_parity_results
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "files %d: SAME %d  DIFF %d  QUIET %d\n", NR, f["SAME"], f["DIFF"], f["QUIET"] }'
echo "per-file results: /tmp/.warning_parity_results (stderr+rc in $OUT: <file>.o ocamlc, <file>.c port)"
