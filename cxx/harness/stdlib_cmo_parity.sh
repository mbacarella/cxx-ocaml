#!/usr/bin/env bash
# stdlib .cmo parity: compile each stdlib unit as stdlib/Makefile does (Compflags, stdlib__X
# targets) with ocamlc.opt and with c++ocamlc, one after the other in one scratch
# dir (-g records it) against the built stdlib's .cmi; compare the .cmo bytes.  G=-g adds -g.
SELF="$(readlink -f "$0")"; ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
OUT=${OUT:-/tmp/stdlib_cmo}
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$ROOT/stdlib"
COMP="-strict-sequence -absname -w -a -nostdlib -principal ${G:-}"
worker() {
  m="$1"; base=${m%.ml}
  case "$base" in
    camlinternal*|std_exit|stdlib) tgt=$base ;;
    *) tgt=stdlib__$(echo "${base:0:1}" | tr a-z A-Z)${base:1} ;;
  esac
  cflags=$(sh ./Compflags "$tgt.cmo" | sed "s|\./expand_module_aliases.awk|$ROOT/stdlib/expand_module_aliases.awk|")
  iflags=$(sh ./Compflags "$tgt.cmi" | sed "s|\./expand_module_aliases.awk|$ROOT/stdlib/expand_module_aliases.awk|")
  # both compilers in the SAME directory, one after the other (-g records it)
  w=$(mktemp -d)
  for d in o c; do
    rm -f "$w"/*
    cp "$m" "$w/"; [ -f "$base.mli" ] && cp "$base.mli" "$w/"
    if [ $d = o ]; then C="$ROOT/ocamlc.opt"; else C="$CPP"; fi
    ( cd "$w"
      if [ -f "$base.mli" ]; then eval "$C $COMP $iflags -I $ROOT/stdlib -o $tgt.cmi -c $base.mli" || exit 1; fi
      eval "$C $COMP $cflags -I $ROOT/stdlib -o $tgt.cmo -c $m" ) >"$OUT/$m.$d.log" 2>&1
    cp "$w/$tgt.cmo" "$OUT/$m.$d" 2>/dev/null
  done
  if [ ! -f "$OUT/$m.o" ]; then echo "OFAIL $m"
  elif [ ! -f "$OUT/$m.c" ]; then echo "CFAIL $m"
  elif cmp -s "$OUT/$m.o" "$OUT/$m.c"; then echo "SAME $m"
  else echo "DIFF $m"; fi
  rm -rf "${w:?}"
}
export -f worker; export ROOT CPP OUT COMP; export AWK=awk
ulimit -v 8000000
ls *.ml | xargs -P 12 -I{} bash -c 'worker {}' | sort -k2 > "$OUT/results"
awk '{f[$1]++} END{printf "SAME %d DIFF %d CFAIL %d OFAIL %d SKIP %d\n", f["SAME"],f["DIFF"],f["CFAIL"],f["OFAIL"],f["SKIP"]}' "$OUT/results"
