#!/usr/bin/env bash
# Native link oracle (cxx/PORTING.md): Asmlink's startup module and the
# executable, c++ocamlopt against ocamlopt.opt, byte for byte.
#
# MODE=link (default): each file is compiled by ocamlopt.opt (-c), then
#   both compilers link the .cmx (`-dstartup -o x.exe x.cmx`).
# MODE=onestep: both compile and link in one run (`-dstartup -o x.exe
#   x.ml`): the startup module follows the unit's compilation (its label
#   and symbol counters carry over).
# DUMP=dcmm|dsel|dlinear...: also compare that dump of the link (stderr).
#
# Compared: the kept startup assembly (x.exe.startup.s) and the executable.
# Usage: native_link_parity.sh [file.ml ...]  (JOBS=, FLAGS= for both)
#   no args: cxx/harness/stamp_probes
#   SAME / DIFF (startup|exe|dump) / CFAIL (the port failed) / OFAIL
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
MODE="${MODE:-link}"
DUMP="${DUMP:-}"
FLAGS="${FLAGS:-}"
REFC="$ROOT/ocamlopt.opt"
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlopt}"
export MODE DUMP FLAGS REFC CPP
OUT=/tmp/native_link_parity

if [ "${1:-}" == "--worker" ]; then
  f="$2"; b=$(basename "$f"); m="${b%.ml}"; key=$(echo "$f" | tr '/' '_')
  i="${f%.ml}.mli"
  w=$(mktemp -d)
  DF=""; [ -n "$DUMP" ] && DF="-$DUMP"
  # run <compiler> <dir> <extra -I...>: the link (or compile + link), in
  # the same directory for both compilers (-g records it: DW_AT_comp_dir),
  # then moved to dir
  run() {
    local c="$1" d="$2" rc; shift 2
    rm -rf "${w:?}/x"; mkdir "$w/x"; cp "$f" "$w/x/"; [ -f "$i" ] && cp "$i" "$w/x/"
    ( cd "$w/x" || exit 1
      [ -f "$i" ] && { timeout 120 "$c" "$@" -w -a $FLAGS -c "${b}i" >/dev/null 2>&1 || exit 1; }
      if [ "$MODE" = onestep ]; then
        timeout 120 "$c" "$@" -w -a $FLAGS $DF -dstartup -o "$m.exe" "$b" >/dev/null 2>dump
      else
        timeout 120 "$REFC" -nostdlib -I "$ROOT/stdlib" -w -a $FLAGS -c "$b" >/dev/null 2>&1 || exit 1
        timeout 120 "$c" "$@" -w -a $FLAGS $DF -dstartup -o "$m.exe" "$m.cmx" >/dev/null 2>dump
      fi ); rc=$?
    mv "$w/x" "$d"
    return $rc
  }
  run "$REFC" "$w/o" -nostdlib -I "$ROOT/stdlib"; orc=$?
  run "$CPP" "$w/c" -I "$ROOT/stdlib"; crc=$?
  for s in o c; do
    cp "$w/$s/$m.exe.startup.s" "$OUT/$key.startup.$s" 2>/dev/null
    cp "$w/$s/dump" "$OUT/$key.dump.$s" 2>/dev/null
  done
  if [ $orc -ne 0 ] || [ ! -f "$w/o/$m.exe" ]; then r="OFAIL"
  elif [ $crc -ne 0 ] || [ ! -f "$w/c/$m.exe" ]; then r="CFAIL"
  elif ! cmp -s "$w/o/$m.exe.startup.s" "$w/c/$m.exe.startup.s"; then r="DIFF(startup)"
  elif [ -n "$DUMP" ] && ! cmp -s "$w/o/dump" "$w/c/dump"; then r="DIFF(dump)"
  elif ! cmp -s "$w/o/$m.exe" "$w/c/$m.exe"; then r="DIFF(exe)"
  else r="SAME"; fi
  rm -rf "${w:?}"
  printf '%s %s\n' "$r" "$f"
  exit 0
fi

rm -rf "$OUT"; mkdir -p "$OUT"
if [ $# -gt 0 ]; then files=("$@"); else mapfile -t files < <(ls cxx/harness/stamp_probes/*.ml); fi
ulimit -v 8000000
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.native_link_parity_results
printf '%s\n' "$res" | awk '{k=$1; sub(/\(.*/, "", k); f[k]++} END{ printf "files %d: SAME %d  DIFF %d  CFAIL %d  OFAIL %d\n", NR, f["SAME"], f["DIFF"], f["CFAIL"], f["OFAIL"] }'
echo "per-file results: /tmp/.native_link_parity_results (in $OUT: <file>.startup.o / .c, <file>.dump.o / .c)"
