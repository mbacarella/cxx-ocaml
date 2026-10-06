#!/usr/bin/env bash
# Stage-10 oracle (cxx/PORTING.md): the Lambda the lambda/ port produces
# against ocamlc's, as text -- `-drawlambda` (Translmod's output) and
# `-dlambda` (after Simplif) -- byte for byte, stamps included (the
# typing port allocates them as ocamlc does).  Each file is compiled alone
# in a scratch directory against the stdlib, by ocamlc.opt and by
# c++ocamlc's new translation path (CPPCAML_NEWLAMBDA=1).
#
# DUMP=dinstr compares Bytegen's instructions (Printinstr) the same way.
# NATIVE=1: the native compiler's Lambda (Translmod.transl_store_implementation
# and native_code's branches) -- ocamlopt.opt against c++ocamlopt, both
# stopped after lambda.
#
# NATIVE=1 DUMP=dclambda compares the Closure middle end's output.
# NATIVE=1 DUMP=drawflambda (an --enable-flambda tree) compares flambda's
# closure conversion; DUMP=dflambda-verbose the flambda passes: the port
# prints the program before each pass up to the first one not ported yet,
# compared with the same prefix of ocamlopt's dump.
#
# Usage: lambda_port_parity.sh [file.ml ...]   (JOBS=, DUMP=drawlambda|dlambda|dinstr,
#   FLAGS= extra flags for both compilers, e.g. absolute -I dirs)
#   no args: cxx/harness/stamp_probes
#   SAME / DIFF / CFAIL (the port failed or produced nothing) / OFAIL
set -u
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
DUMP="${DUMP:-drawlambda}"
FLAGS="${FLAGS:-}"
STOP="-stop-after lambda"
DFLAG="-$DUMP"
[ "$DUMP" = dinstr ] && STOP=""
[ "$DUMP" = cmo ] && { STOP=""; DFLAG=""; }  # compare the .cmo files' bytes
[ "$DUMP" = S ] && { STOP=""; DFLAG="-S"; }    # NATIVE=1: compare the .s files
[ "$DUMP" = cmx ] && { STOP=""; DFLAG=""; }    # NATIVE=1: compare the .cmx files' bytes
[ "$DUMP" = o ] && { STOP=""; DFLAG=""; }      # NATIVE=1: compare the .o files' bytes
NATIVE="${NATIVE:-0}"
if [ "$NATIVE" = 1 ]; then
  REFC="$ROOT/ocamlopt.opt"; OSTOP="$STOP"
  CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlopt}"
else
  REFC="$ROOT/ocamlc.opt"; OSTOP=""
  CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
fi
# DUMP=dclambda|drawclambda (NATIVE=1): the Closure middle end's Clambda;
# ocamlopt.opt compiles on, the port stops after its dump (the back end
# past Clambda is not ported yet: its exit status is not checked)
CLAMBDA=0
case "$DUMP" in dflambda-verbose|drawflambda|dclambda|drawclambda|dcmm|dsel|dcombine|dcse|dlive|dspill|dsplit|dinterf|dprefer|dalloc|dreload|dlinear|S) CLAMBDA=1; STOP=""; OSTOP="" ;; esac
export DUMP FLAGS STOP DFLAG NATIVE REFC OSTOP CLAMBDA
OUT=/tmp/lambda_port_parity

if [ "${1:-}" == "--worker" ]; then
  f="$2"; b=$(basename "$f"); key=$(echo "$f" | tr '/' '_')
  # Both compilers run in the same directory, one after the other: -g's
  # debug section records it (debug_dirs).
  w=$(mktemp -d); mkdir "$w/x"
  i="${f%.ml}.mli"
  prep() { rm -rf "${w:?}/x"; mkdir "$w/x"; cp "$f" "$w/x/"; [ -f "$i" ] && cp "$i" "$w/x/"; }
  prep
  if [ -f "$i" ]; then  # a sibling interface: each compiler compiles it first
    ( cd "$w/x" && timeout 120 "$REFC" -nostdlib -I "$ROOT/stdlib" -w -a $FLAGS -c "${b}i" ) >/dev/null 2>&1
  fi
  ( cd "$w/x" && timeout 120 "$REFC" -nostdlib -I "$ROOT/stdlib" -w -a $FLAGS $DFLAG $OSTOP -c "$b" ) \
      >/dev/null 2>"$OUT/$key.o"; orc=$?
  if [ "$DUMP" = cmo ]; then cp "$w/x/${b%.ml}.cmo" "$OUT/$key.o" 2>/dev/null || orc=1; fi
  if [ "$DUMP" = S ]; then cp "$w/x/${b%.ml}.s" "$OUT/$key.o" 2>/dev/null || orc=1; fi
  if [ "$DUMP" = cmx ] || [ "$DUMP" = o ]; then cp "$w/x/${b%.ml}.$DUMP" "$OUT/$key.o" 2>/dev/null || orc=1; fi
  prep
  if [ -f "$i" ]; then
    ( cd "$w/x" && timeout 120 "$CPP" -I "$ROOT/stdlib" -w -a $FLAGS -c "${b}i" ) >/dev/null 2>&1
  fi
  ( cd "$w/x" && CPPCAML_NEWLAMBDA=1 timeout 120 "$CPP" -I "$ROOT/stdlib" -w -a $FLAGS $DFLAG $STOP -c "$b" ) \
      >/dev/null 2>"$OUT/$key.c"; crc=$?
  if [ "$DUMP" = cmo ]; then cp "$w/x/${b%.ml}.cmo" "$OUT/$key.c" 2>/dev/null || crc=1; fi
  if [ "$DUMP" = S ]; then cp "$w/x/${b%.ml}.s" "$OUT/$key.c" 2>/dev/null || crc=1; fi
  if [ "$DUMP" = cmx ] || [ "$DUMP" = o ]; then cp "$w/x/${b%.ml}.$DUMP" "$OUT/$key.c" 2>/dev/null || crc=1; fi
  [ "$CLAMBDA" = 1 ] && sed -i.orig -e '/: the native back end (.*) is not supported yet$/d' \
    -e '/: the flambda middle end (.*) is not ported yet$/d' "$OUT/$key.c" && rm -f "$OUT/$key.c.orig"
  if [ "$DUMP" = dflambda-verbose ]; then  # the port's dump: a prefix of ocamlopt's
    head -c "$(file_size "$OUT/$key.c")" "$OUT/$key.o" > "$OUT/$key.o.prefix"
    mv "$OUT/$key.o.prefix" "$OUT/$key.o"
  fi
  rm -rf "${w:?}"
  if [ $orc -ne 0 ]; then printf 'OFAIL %s\n' "$f"
  elif { [ $crc -ne 0 ] && [ "$CLAMBDA" = 0 ]; } || [ ! -s "$OUT/$key.c" ]; then printf 'CFAIL %s\n' "$f"
  elif cmp -s "$OUT/$key.o" "$OUT/$key.c"; then printf 'SAME %s\n' "$f"
  else printf 'DIFF %s\n' "$f"; fi
  exit 0
fi

rm -rf "$OUT"; mkdir -p "$OUT"
if [ $# -gt 0 ]; then files=("$@"); else lines_into files < <(ls cxx/harness/stamp_probes/*.ml); fi
vm_limit 8000000
res=$(printf '%s\n' ${files[@]+"${files[@]}"} | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.lambda_port_parity_results
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "files %d: SAME %d  DIFF %d  CFAIL %d  OFAIL %d\n", NR, f["SAME"], f["DIFF"], f["CFAIL"], f["OFAIL"] }'
echo "per-file results: /tmp/.lambda_port_parity_results (dumps in $OUT: <file>.o ocamlc, <file>.c port)"
