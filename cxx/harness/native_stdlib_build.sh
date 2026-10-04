#!/usr/bin/env bash
# The standard library's native build by c++ocamlopt (cxx/PORTING.md): the
# stdlib's own `make allopt`, its compiler (CAMLOPT) once ocamlopt.opt and
# once c++ocamlopt, each in a scratch copy of the tree whose native stdlib
# outputs were deleted first, and every artifact compared byte for byte:
# the .cmx, .o (and .cmt / .cmti), stdlib.cmxa / .a, std_exit.
# (The .cmi stay: ocamlc made them.  Both compilers run through the same
# symlink, as the .cmt record argv.)
#
# Usage: native_stdlib_build.sh   (WD= a scratch directory; KEEP=1 keeps it)
set -u
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlopt}"
REF="$ROOT/ocamlopt.opt"
WD="${WD:-$(mktemp -d)}"; mkdir -p "$WD"
KEEP="${KEEP:-0}"
trap '[ "$KEEP" = 1 ] || rm -rf "$WD"' EXIT
echo "WD=$WD"
ulimit -v 16000000

build() {  # build <compiler> <outdir>
  # (through one symlink path for both: a .cmt records the command line)
  mkdir -p "$WD/bin"; ln -sfn "$1" "$WD/bin/ocamlopt"
  local c="$WD/bin/ocamlopt" out="$2"
  rm -rf "$WD/t"; mkdir "$WD/t"
  ( cd "$ROOT" && cp -a stdlib boot runtime Makefile.common Makefile.config Makefile.build_config Makefile.config_if_required "$WD/t/" ) 2>/dev/null
  cp -a "$ROOT/utils" "$WD/t/" 2>/dev/null
  ( cd "$WD/t/stdlib" && rm -f *.cmx *.o *.cmxa *.a *.cmt *.cmti )
  # the compiler both make rules name: OPTCOMPILER (a prerequisite) and CAMLOPT
  if ! ( cd "$WD/t/stdlib" && make allopt OCAMLRUN= COMPILER="$ROOT/ocamlc" CAMLC="$ROOT/ocamlc.opt" OPTCOMPILER="$c" CAMLOPT="$c" ) > "$WD/log.$out" 2>&1; then
    echo "FAIL ($out)"; tail -20 "$WD/log.$out"; return 1
  fi
  rm -rf "$WD/$out"; mv "$WD/t" "$WD/$out"
}

build "$REF" ref || { echo "the reference build failed"; exit 1; }
build "$CPP" port || exit 1
cd "$WD/ref/stdlib" || exit 1
same=0; diff=0; missing=0
: > "$WD/diffs"
for f in $(ls *.cmx *.o *.cmxa *.a *.cmt *.cmti 2>/dev/null | sort); do
  if [ ! -e "$WD/port/stdlib/$f" ]; then missing=$((missing + 1)); echo "MISSING $f" >> "$WD/diffs"
  elif cmp -s "$f" "$WD/port/stdlib/$f"; then same=$((same + 1))
  else diff=$((diff + 1)); echo "DIFF $f" >> "$WD/diffs"; fi
done
echo "artifacts: SAME $same  DIFF $diff  MISSING $missing"
head -20 "$WD/diffs"
