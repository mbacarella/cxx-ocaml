#!/usr/bin/env bash
# Per-module effid table: "<norm>\t<raw>\t<module>.cmo", totals on stderr.
#
# effid.sh reports the RAW dumpobj diff and a module count; this adds the
# NORMALISED count (digits -> N), which is the metric the cmo slices are scored
# on -- a pure operand/offset shift counts 0 there, so |norm| tracks structural
# divergence while raw tracks everything.
#
#   OUR=<compiler>   default cxx/build-release/c++ocamlc
#   REF=<dir>        ocamlc.opt reference tree (default /tmp/effid_ref, built
#                    on demand by effid.sh -- run that first after a /tmp wipe)
#   KEEP=<dir>       build the corpus there instead of a temp dir (so two
#                    compilers' trees can be cmp'd module by module)
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"; cd "$ROOT"
OUR=${OUR:-$ROOT/cxx/build-release/c++ocamlc}
REF=${REF:-/tmp/effid_ref}
DUMP="$ROOT/runtime/ocamlrun $ROOT/tools/dumpobj -effid"
FLAGS="-strict-sequence -strict-formats -w +a-4-9-40-41-42-44-45-48-70"
extract_cl(){ awk '
  /^CL_COMMON="/{g=1; sub(/^CL_COMMON="/,"")}
  /^CL_BYTE="/{g=1; sub(/^CL_BYTE="/,"")}
  g{l=$0; sub(/"[[:space:]]*$/,"",l); n=split(l,a,/[[:space:]]+/);
    for(i=1;i<=n;i++) if(a[i]!="") print a[i];
    if($0 ~ /"[[:space:]]*$/) g=0}' cxx/harness/ocamlc_bootstrap.sh; }
STAGE=$(mktemp -d)
while read -r f; do [ -z "$f" ] && continue
  cp "$ROOT/$f" "$STAGE/$(basename "$f")"; done < <(extract_cl)
ORDER=$(cd "$STAGE" && "$ROOT/tools/ocamldep.opt" -sort ./*.mli ./*.ml 2>/dev/null \
        | sed 's|\./||g')
OUT=${KEEP:-$(mktemp -d)}
rm -rf "$OUT"; mkdir -p "$OUT"; cp "$STAGE"/*.mli "$STAGE"/*.ml "$OUT"/ 2>/dev/null
( cd "$OUT"; for f in $ORDER; do $OUR -I . -I "$ROOT/stdlib" $FLAGS -c "$f" 2>/dev/null
  done )
T=$(mktemp -d); tn=0; tr=0; ident=0; tot=0
for a in "$OUT"/*.cmo; do
  b=$(basename "$a"); [ -f "$REF/$b" ] || continue; tot=$((tot+1))
  $DUMP "$a" 2>/dev/null > "$T/o"; $DUMP "$REF/$b" 2>/dev/null > "$T/r"
  raw=$(diff "$T/o" "$T/r" | grep -cE '^[<>]')
  perl -pe 's/\d+/N/g' "$T/o" > "$T/on"; perl -pe 's/\d+/N/g' "$T/r" > "$T/rn"
  norm=$(diff "$T/on" "$T/rn" | grep -cE '^[<>]')
  printf '%s\t%s\t%s\n' "$norm" "$raw" "$b"
  tn=$((tn+norm)); tr=$((tr+raw)); [ "$raw" -eq 0 ] && ident=$((ident+1))
done
rm -rf "$T" "$STAGE"
echo "identical=$ident/$tot total_norm=$tn total_raw=$tr  OUT=$OUT" >&2
