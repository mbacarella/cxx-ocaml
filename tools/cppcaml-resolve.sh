#!/bin/sh
# c++caml development aid: resolve the bytecode offsets in a CPPCAML_FIELDTRACE
# crash dump to "module+offset", using the linker's <exe>.linkmap sidecar.
#
# Usage:
#   c++ocamlc-bytecode-exe is run with CPPCAML_FIELDTRACE=1; on a crash it prints
#   a dump containing `pc=<n>` / `retpc=<n>` offsets.  Pipe that dump in and pass
#   the executable (or its .linkmap) so the offsets become legible:
#
#     CPPCAML_FIELDTRACE=1 ocamlrun ./ocamlc -c foo.ml 2>dump.txt
#     tools/cppcaml-resolve.sh ./ocamlc < dump.txt
#
# (The .linkmap is written automatically next to every c++link output.)

set -eu

exe="${1:?usage: cppcaml-resolve.sh <exe-or-linkmap> < dump}"
case "$exe" in
  *.linkmap) map="$exe" ;;
  *)         map="$exe.linkmap" ;;
esac
[ -f "$map" ] || { echo "no linkmap: $map (link with c++link to generate it)" >&2; exit 1; }

# For each `pc=<n>` / `retpc=<n>` token on a line, append its module+offset.
awk -v mapfile="$map" '
  BEGIN {
    n = 0
    while ((getline line < mapfile) > 0) {
      split(line, a, " ")
      start[n] = a[1] + 0; name[n] = a[2]; n++
    }
  }
  function resolve(off,   i, best, bi) {
    best = -1; bi = -1
    for (i = 0; i < n; i++)
      if (start[i] <= off && start[i] > best) { best = start[i]; bi = i }
    if (bi < 0) return "?"
    return name[bi] "+" (off - best)
  }
  {
    out = $0
    if (match($0, /(pc|retpc)=[0-9]+/)) {
      # annotate every offset on the line
      line = $0; ann = ""
      while (match(line, /(pc|retpc)=[0-9]+/)) {
        tok = substr(line, RSTART, RLENGTH)
        sub(/.*=/, "", tok)
        ann = ann " [" resolve(tok + 0) "]"
        line = substr(line, RSTART + RLENGTH)
      }
      out = $0 ann
    }
    print out
  }
'
