#!/usr/bin/env bash
# Effective-identity metric (Path A) for the cmo project.
#
# Compiles the 139 compiler modules with OUR c++ocamlc, then compares each
# module's disassembly against a FRESH ocamlc.opt reference using
# `tools/dumpobj -effid` -- a canonical form emitted straight from the decoded
# bytecode: branch/switch/closure targets are the RAW relative displacements,
# and the address column, debug events and optimization hints are suppressed.
# Two modules with identical code produce byte-identical output; only genuine
# codegen divergences show as a diff.  Reports "effectively-identical / 139" --
# the cmo analog of cmi's "byte-identical modulo uids" ceiling.
#
#   REF=<dir>   reference dir of ocamlc.opt-built .cmo (default /tmp/effid_ref,
#               built on demand -- delete the dir to force a rebuild)
#   V=1         list every divergent module with its diff-line count
set -u
SELF="$(readlink -f "$0")"; ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"; cd "$ROOT" || exit 1
RUN=$ROOT/runtime/ocamlrun; DUMP="$ROOT/runtime/ocamlrun $ROOT/tools/dumpobj -effid"
OUR=$ROOT/cxx/build-release/c++ocamlc
REF=${REF:-/tmp/effid_ref}
FLAGS="-strict-sequence -strict-formats -w +a-4-9-40-41-42-44-45-48-70"
BOOTSTRAP=$ROOT/cxx/harness/ocamlc_bootstrap.sh

extract_cl(){ awk '
  /^CL_COMMON="/{g=1; sub(/^CL_COMMON="/,"")}
  /^CL_BYTE="/{g=1; sub(/^CL_BYTE="/,"")}
  g{l=$0; sub(/"[[:space:]]*$/,"",l); n=split(l,a,/[[:space:]]+/);
    for(i=1;i<=n;i++) if(a[i]!="") print a[i];
    if($0 ~ /"[[:space:]]*$/) g=0}' "$BOOTSTRAP"; }

STAGE=$(mktemp -d)
while read -r f; do [ -z "$f" ] && continue; cp "$ROOT/$f" "$STAGE/$(basename "$f")"; done < <(extract_cl)
# ocamldep -sort emits ONE line; strip every ./ prefix (an anchored sed only
# fixed the first file) so the bare filename lands in Assert_failure payloads.
ORDER=$(cd "$STAGE" && "$ROOT/tools/ocamldep.opt" -sort ./*.mli ./*.ml 2>/dev/null | sed 's|\./||g')
OUT=$(mktemp -d); cp "$STAGE"/*.mli "$STAGE"/*.ml "$OUT"/ 2>/dev/null
( cd "$OUT"; for f in $ORDER; do $OUR -I . -I "$ROOT/stdlib" $FLAGS -c "$f" 2>/dev/null; done )

# Reference dir: ocamlc.opt-built .cmo, compiled the SAME way (bare filenames,
# its own cmi stage).  Built on demand when the REF dir is absent.
if [ ! -f "$REF/main.cmo" ]; then
  mkdir -p "$REF"; cp "$STAGE"/*.mli "$STAGE"/*.ml "$REF"/ 2>/dev/null
  ( cd "$REF"; for f in $ORDER; do "$ROOT/ocamlc.opt" -nostdlib -I . -I "$ROOT/stdlib" $FLAGS -c "$f" 2>/dev/null; done )
fi

ident=0; tot=0; diffs=$(mktemp)
for a in "$OUT"/*.cmo; do
  b=$(basename "$a"); [ -f "$REF/$b" ] || continue; tot=$((tot+1))
  d=$(diff <($DUMP "$a" 2>/dev/null) <($DUMP "$REF/$b" 2>/dev/null) | grep -cE '^[<>]')
  if [ "$d" -eq 0 ]; then ident=$((ident+1)); else echo "DIFF $b $d" >> "$diffs"; fi
done
[ "${V:-0}" = 1 ] && sort -t' ' -k3 -n "$diffs"
echo "effectively-identical=$ident/$tot  REF=$REF  OUT=$OUT"
rm -f "$diffs"
