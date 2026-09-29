#!/usr/bin/env bash
# The native compiler built by c++ocamlopt (cxx/PORTING.md): the tree's own
# commands for ocamlopt.opt (`make -n -W utils/misc.ml ocamlopt.opt`: every
# compiler source compiled, compilerlibs/ocamlcommon.cmxa and
# ocamloptcomp.cmxa, the link), run once with ocamlopt.opt and once with
# c++ocamlopt, and every artifact compared byte for byte: the .cmx, .o and
# .cmi, the two libraries and their .a, the ocamlopt.opt executable.
#
# Both builds run in the same scratch copy of the tree, one after the
# other (-g records the directory, -absname the paths).  The copy's
# compiler outputs are deleted first, so a missing output is a failure.
#
# Usage: native_self_build.sh   (WD= a scratch directory; KEEP=1 keeps it)
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlopt}"
REF="$ROOT/ocamlopt.opt"
WD="${WD:-$(mktemp -d)}"; mkdir -p "$WD"
KEEP="${KEEP:-0}"
trap '[ "$KEEP" = 1 ] || rm -rf "$WD"' EXIT
echo "WD=$WD"
ulimit -v 16000000

# the commands, as make would run them after utils/misc.ml changed
make -n V=1 -W utils/misc.ml ocamlopt.opt 2>/dev/null | grep '^\./boot/ocamlrun \./ocamlopt ' > "$WD/cmds" || exit 1
echo "commands: $(wc -l < "$WD/cmds")"
DIRS="utils parsing typing bytecomp file_formats lambda middle_end asmcomp driver toplevel tools"

build() {  # build <compiler> <outdir>
  local c="$1" out="$2" n=0 fail=0
  rm -rf "$WD/t"; mkdir "$WD/t"
  for d in $DIRS runtime stdlib otherlibs compilerlibs boot; do cp -a "$ROOT/$d" "$WD/t/"; done
  ( cd "$WD/t" && find $DIRS -name '*.cmx' -o -name '*.o' -o -name '*.cmt' -o -name '*.cmti' | xargs rm -f
    rm -f compilerlibs/*.cmxa compilerlibs/*.a ocamlopt.opt )
  while IFS= read -r line; do
    n=$((n + 1))
    cmd="$c ${line#./boot/ocamlrun ./ocamlopt }"
    if ! ( cd "$WD/t" && eval "$cmd" ) > "$WD/log.$out.$n" 2>&1; then
      echo "FAIL ($out) #$n: ${cmd:0:200}..."; sed 's/^/  /' "$WD/log.$out.$n" | head -20
      fail=1; break
    fi
  done < "$WD/cmds"
  rm -rf "$WD/$out"; mv "$WD/t" "$WD/$out"
  return $fail
}

build "$REF" ref || { echo "the reference build failed"; exit 1; }
build "$CPP" port

# compare every compiler artifact
cd "$WD/ref" || exit 1
same=0; diff=0; missing=0
: > "$WD/diffs"
for f in $(find $DIRS compilerlibs -name '*.cmx' -o -name '*.o' -o -name '*.cmi' -o -name '*.cmxa' -o -name '*.a' | sort) ocamlopt.opt; do
  if [ ! -e "$WD/port/$f" ]; then missing=$((missing + 1)); echo "MISSING $f" >> "$WD/diffs"
  elif cmp -s "$f" "$WD/port/$f"; then same=$((same + 1))
  else diff=$((diff + 1)); echo "DIFF $f" >> "$WD/diffs"; fi
done
echo "artifacts: SAME $same  DIFF $diff  MISSING $missing"
head -20 "$WD/diffs"
cmp -s ocamlopt.opt "$WD/port/ocamlopt.opt" && echo "ocamlopt.opt: IDENTICAL" || echo "ocamlopt.opt: DIFFERENT"
