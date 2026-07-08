#!/usr/bin/env bash
# TRUSTING-TRUST instruction parity: compile the reference OCaml compiler's OWN
# .ml sources with both the oracle (`ocamlc.opt -dinstr`) and c++instr, normalize
# the bytecode instruction streams (labels renumbered by first appearance, quoted
# source paths trimmed to basenames), and diff.  This is the hardest real corpus
# available -- the compiler itself -- so byte-identical instruction streams are
# strong evidence that c++ocamlc emits the SAME bytecode the real compiler does.
#
# Requires a completed `KEEP=1 ocamlc_bootstrap.sh` whose WD holds the flattened
# compiler sources + c++-built sibling .cmi.  Pass that WD in.
#   WD=/tmp/xxx bash bootstrap_instr_parity.sh
set -u
SELF="$(readlink -f "$0")"; cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
CPP=$ROOT/cxx/build/c++instr
ORACLE=$ROOT/ocamlc.opt
WD=${WD:?set WD to a KEEP=1 ocamlc_bootstrap dir}
TIMEOUT=${CPP_TIMEOUT:-30}
ODIR=$WD/.oracle          # oracle-built .cmi/.cmo + captured dumps live here
mkdir -p "$ODIR"

INCS="-I $WD -I $ROOT/utils -I $ROOT/parsing -I $ROOT/typing -I $ROOT/lambda \
-I $ROOT/file_formats -I $ROOT/bytecomp -I $ROOT/driver"
# oracle reads REAL stdlib .cmi (its own on-disk format); c++instr reads the
# all-ours WD stdlib.  Both built from identical sources -> identical sig layout.
OINCS="-I $ROOT/stdlib -I $ODIR -I $ROOT/utils -I $ROOT/parsing -I $ROOT/typing -I $ROOT/lambda \
-I $ROOT/file_formats -I $ROOT/bytecomp -I $ROOT/driver"

norm() { perl -0777 -pe 'BEGIN{%m=();$n=0}
  s{L(\d+)}{ "L" . ($m{$1} //= ++$n) }ge;
  s{"[^"]*/([^"/]+\.ml)"}{"$1"}g;
  s/\s+\z//'; }

# module order = every *.ml the bootstrap compiled, in .cl_order if present else
# just glob (order only matters for oracle .cmi deps, so build them in-order).
if [ -f "$WD/.cl_order" ]; then mapfile -t MODS < "$WD/.cl_order"
else mapfile -t MODS < <(cd "$WD" && ls *.ml | sed 's/\.ml$//'); fi

ok=0; diff=0; skip=0; cpperr=0; DIFFS=""
for m in "${MODS[@]}"; do
  src="$WD/$m.ml"; [ -f "$src" ] || { skip=$((skip+1)); continue; }
  # build the oracle .cmi first (compile mli if present, else the ml) so oracle
  # -dinstr sees a consistent sig set; capture the oracle instruction dump.
  cp "$src" "$ODIR/$m.ml"; [ -f "$WD/$m.mli" ] && cp "$WD/$m.mli" "$ODIR/$m.mli"
  ock="$ODIR/$m.oinstr"
  if [ ! -f "$ock" ]; then
    ( cd "$ODIR" && timeout "$TIMEOUT" "$ORACLE" -nostdlib $OINCS -dinstr -c "$m.ml" ) \
      2>"$ock.raw" 1>/dev/null || true
    awk '/^(\t|L[0-9])/{seen=1} seen && /^(\t|L[0-9]| )/ && !/^ *\^+ *$/{print}' "$ock.raw" \
      > "$ock" 2>/dev/null
  fi
  [ -s "$ock" ] || { skip=$((skip+1)); continue; }
  o=$(norm < "$ock")
  mm=$(cd "$WD" && timeout "$TIMEOUT" "$CPP" $INCS "$m.ml" 2>/dev/null); rc=$?
  if [ $rc -ne 0 ] || printf '%s' "$mm" | grep -q TYPE_ERROR; then
    cpperr=$((cpperr+1)); DIFFS="$DIFFS\nCPPERR $m"; continue; fi
  mm=$(printf '%s' "$mm" | norm)
  if [ "$o" = "$mm" ]; then ok=$((ok+1)); else diff=$((diff+1)); DIFFS="$DIFFS\nDIFF  $m"; fi
done

total=$((ok+diff+cpperr))
echo "=== bootstrap instr parity: $ok/$total identical  (diff=$diff cpperr=$cpperr skip=$skip) ==="
[ -n "$DIFFS" ] && printf '%b\n' "$DIFFS"
