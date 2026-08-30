#!/usr/bin/env bash
# "Is this module's remaining .cmo divergence a MATCHER bug or a BYTEGEN bug?"
#
# The effid metric (cxx/harness/effid.sh) compares bytecode, so every residue
# looks alike.  This one splits them: it compiles a module with BOTH compilers
# under -dlambda and compares the two Lambda terms.  A module whose Lambda is
# identical but whose bytecode is not has its residue BELOW the matcher, in
# bytegen/emitcode, and no amount of matcher work will close it.
#
# Two normalizations make the comparison meaningful, and both are load-bearing:
#   * ident stamps (`/301`) and static-exit ids are renumbered by construction
#     and say nothing, so they are erased;
#   * the two pretty-printers WRAP LINES DIFFERENTLY, so a raw whole-module
#     diff of two IDENTICAL terms runs to thousands of lines of pure noise.
#     That is why this is worth a script: all whitespace is flattened first.
# What survives is a real difference in the term.
#
# Usage:
#   cxx/harness/lambda_residue.sh                 # every still-differing module
#   cxx/harness/lambda_residue.sh typecore ctype  # just these
#
#   OUR=<binary>  compiler under test (default cxx/build-release/c++ocamlc)
#   REF=<dir>     ocamlc.opt-built reference tree (default /tmp/lamres_ref,
#                 built on demand -- delete the dir to force a rebuild)
#   WORK=<dir>    our own build tree (default /tmp/lamres_out, ALWAYS rebuilt,
#                 since it depends on the binary under test)
#   V=1           on a difference, print the first 40 differing tokens
#   BINDINGS=1    report per TOP-LEVEL BINDING instead of per module.  Reach
#                 for this: a module is usually a MIX, and the mix is the
#                 point.  In S348 translprim's check_primitive_arity was
#                 lambda-IDENTICAL (its bytecode was not) inside a module
#                 whose other three bindings did differ -- a per-module
#                 verdict alone would have hidden it.
#
# The reference tree is built and used HERE, never in one another harness owns:
# an `ocamlc.opt -c` run from the wrong cwd picks up OUR .cmi files through
# ocamlc's implicit `.` on the load path and silently rewrites the reference.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd); cd "$ROOT"
DUMP="$ROOT/runtime/ocamlrun $ROOT/tools/dumpobj -effid"
OUR=${OUR:-$ROOT/cxx/build-release/c++ocamlc}
REF=${REF:-/tmp/lamres_ref}
WORK=${WORK:-/tmp/lamres_out}
FLAGS="-strict-sequence -strict-formats -w +a-4-9-40-41-42-44-45-48-70"
BOOTSTRAP=$ROOT/cxx/harness/ocamlc_bootstrap.sh

extract_cl(){ awk '
  /^CL_COMMON="/{g=1; sub(/^CL_COMMON="/,"")}
  /^CL_BYTE="/{g=1; sub(/^CL_BYTE="/,"")}
  g{l=$0; sub(/"[[:space:]]*$/,"",l); n=split(l,a,/[[:space:]]+/);
    for(i=1;i<=n;i++) if(a[i]!="") print a[i];
    if($0 ~ /"[[:space:]]*$/) g=0}' "$BOOTSTRAP"; }

# Split a dump into its top-level bindings and normalize each: the printer
# starts
# every one with `     name/stamp =` at a fixed indent.  Keyed by name plus
# occurrence, since a name can be rebound (`prim` twice in
# transl_primitive_application).  Emits "key<TAB>flattened-term" per binding.
bindsplit(){ perl -e '
  my ($cur, %seen, @out, $txt) = ("");
  while (<STDIN>) {
    if (/^ {5}(\w+)\/\d+ =\s*$/) {
      push @out, [$cur, $txt] if defined $txt;
      $cur = $1 . "#" . ++$seen{$1}; $txt = "";
    } elsif (defined $txt) { $txt .= $_ }
  }
  push @out, [$cur, $txt] if defined $txt;
  for my $b (@out) {
    my $t = $b->[1];
    $t =~ s{/\d+}{/N}g; $t =~ s{\(exit \d+}{(exit N}g; $t =~ s{with \(\d+}{with (N}g;
    $t =~ s/\s+/ /g;
    print $b->[0], "\t", $t, "\n";
  }'; }

# Erase what is renumbered by construction, then flatten ALL whitespace so the
# two pretty-printers' line breaks cannot show up as a difference.
flat(){ perl -pe 's{/\d+}{/N}g; s{\(exit \d+}{(exit N}g; s{with \(\d+}{with (N}g' "$1" \
          | tr -s ' \n' '\n\n' | grep -v '^$'; }

STAGE=$(mktemp -d); TMP=$(mktemp -d); trap 'rm -rf "$STAGE" "$TMP"' EXIT
while read -r f; do [ -z "$f" ] && continue; cp "$ROOT/$f" "$STAGE/$(basename "$f")"; done < <(extract_cl)
ORDER=$(cd "$STAGE" && "$ROOT/tools/ocamldep.opt" -sort ./*.mli ./*.ml 2>/dev/null | sed 's|\./||g')

rm -rf "$WORK"; mkdir -p "$WORK"; cp "$STAGE"/*.mli "$STAGE"/*.ml "$WORK"/ 2>/dev/null
( cd "$WORK"; for f in $ORDER; do $OUR -I . -I "$ROOT/stdlib" $FLAGS -c "$f" 2>/dev/null; done )

if [ ! -f "$REF/main.cmo" ]; then
  mkdir -p "$REF"; cp "$STAGE"/*.mli "$STAGE"/*.ml "$REF"/ 2>/dev/null
  ( cd "$REF"; for f in $ORDER; do "$ROOT/ocamlc.opt" -nostdlib -I . -I "$ROOT/stdlib" $FLAGS -c "$f" 2>/dev/null; done )
fi

# Default subject list: the modules whose BYTECODE still differs.  A module
# whose
# bytecode already matches has no residue to classify.
mods=("$@")
if [ ${#mods[@]} -eq 0 ]; then
  for a in "$WORK"/*.cmo; do
    b=$(basename "$a" .cmo); [ -f "$REF/$b.cmo" ] || continue
    d=$(diff <($DUMP "$a" 2>/dev/null) <($DUMP "$REF/$b.cmo" 2>/dev/null) | grep -cE '^[<>]')
    [ "$d" -eq 0 ] || mods+=("$b")
  done
fi
[ ${#mods[@]} -eq 0 ] && { echo "no differing module: nothing to classify"; exit 0; }

bytegen=0; matcher=0
for m in "${mods[@]}"; do
  if [ ! -f "$WORK/$m.ml" ]; then echo "$m -- no such module in the corpus" >&2; continue; fi
  ( cd "$REF"  && "$ROOT/ocamlc.opt" -nostdlib -I . -I "$ROOT/stdlib" $FLAGS -dlambda -c "$m.ml" ) 2>"$TMP/r" >/dev/null
  ( cd "$WORK" && $OUR -I . -I "$ROOT/stdlib" $FLAGS -dlambda -c "$m.ml" ) >"$TMP/o" 2>/dev/null

  if [ "${BINDINGS:-0}" = 1 ]; then
    bindsplit < "$TMP/r" > "$TMP/r.b"; bindsplit < "$TMP/o" > "$TMP/o.b"
    if diff <(cut -f1 "$TMP/r.b") <(cut -f1 "$TMP/o.b") >/dev/null; then
      same=0; diffn=0
      while IFS=$'\t' read -r k t; do
        u=$(awk -F'\t' -v k="$k" '$1==k{print $2; exit}' "$TMP/o.b")
        if [ "$t" = "$u" ]; then same=$((same+1))
        else diffn=$((diffn+1)); echo "  $m.${k%#*}  lambda differs"; fi
      done < "$TMP/r.b"
      echo "$m  bindings: $same identical, $diffn differing"
      continue
    fi
    echo "$m  BINDING SET DIFFERS -- falling back to the per-module verdict"
  fi

  flat "$TMP/r" > "$TMP/r.tok"; flat "$TMP/o" > "$TMP/o.tok"
  n=$(diff "$TMP/o.tok" "$TMP/r.tok" | grep -cE '^[<>]')
  if [ "$n" -eq 0 ]; then
    bytegen=$((bytegen+1)); echo "$m  LAMBDA-IDENTICAL -- residue is BYTEGEN, not the matcher"
  else
    matcher=$((matcher+1)); echo "$m  lambda differs ($n tokens) -- residue is in the front end"
    [ "${V:-0}" = 1 ] && diff "$TMP/o.tok" "$TMP/r.tok" | grep -E '^[<>]' | head -40 | sed 's/^/    /'
  fi
done
[ "${BINDINGS:-0}" = 1 ] || echo "bytegen-residue=$bytegen  matcher-residue=$matcher  REF=$REF  WORK=$WORK"
