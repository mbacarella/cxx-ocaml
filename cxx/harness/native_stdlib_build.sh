#!/usr/bin/env bash
# The standard library's and otherlibs' native builds by c++ocamlopt
# (cxx/PORTING.md): their own `make allopt` (stdlib, otherlibs/unix, str,
# runtime_events, systhreads), the native compiler once ocamlopt.opt and once
# c++ocamlopt, each in a scratch copy of the tree whose native outputs were
# deleted first, and every artifact compared byte for byte: the .cmx, .o
# (and .cmt / .cmti), the .cmxa / .a / .cmxs.  Dynlink (built by the root
# Makefile) too: make's commands for it replayed, ./ocamlopt.opt replaced.
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
OTHERLIBS="${OTHERLIBS:-unix str runtime_events systhreads}"

build() {  # build <compiler> <outdir>
  # (through one symlink path for both: a .cmt records the command line)
  mkdir -p "$WD/bin"; ln -sfn "$1" "$WD/bin/ocamlopt"
  local c="$WD/bin/ocamlopt" out="$2"
  rm -rf "$WD/t"; mkdir "$WD/t"
  ( cd "$ROOT" && cp -a stdlib boot runtime otherlibs tools utils parsing typing bytecomp file_formats lambda middle_end \
      asmcomp driver toplevel ocamlc ocamlc.opt ocamlopt ocamlopt.opt \
      Makefile.common Makefile.config Makefile.build_config Makefile.config_if_required Makefile.best_binaries \
      "$WD/t/" ) 2>/dev/null
  ( cd "$WD/t/stdlib" && rm -f *.cmx *.o *.cmxa *.a *.cmt *.cmti )
  for l in $OTHERLIBS; do ( cd "$WD/t/otherlibs/$l" && rm -f *.cmx *.cmxa *.cmxs *.cmt *.cmti $(ls *.ml 2>/dev/null | sed 's/\.ml$/.o/') ); done
  # the compiler both make rules name: OPTCOMPILER (a prerequisite) and CAMLOPT
  if ! ( cd "$WD/t/stdlib" && make allopt OCAMLRUN= COMPILER="$ROOT/ocamlc" CAMLC="$ROOT/ocamlc.opt" OPTCOMPILER="$c" CAMLOPT="$c" ) > "$WD/log.$out" 2>&1; then
    echo "FAIL ($out)"; tail -20 "$WD/log.$out"; return 1
  fi
  # dynlink: the root Makefile's commands (DYNCMDS), ./ocamlopt.opt the compiler
  ( cd "$WD/t/otherlibs/dynlink" && rm -f *.cmx *.o *.cmxa *.a native/*.cmx native/*.o native/*.cmt )
  while IFS= read -r line; do
    case "$line" in ./ocamlopt.opt*) line="$c${line#./ocamlopt.opt}" ;; esac
    if ! ( cd "$WD/t" && eval "$line" ) >> "$WD/log.$out" 2>&1; then
      echo "FAIL ($out, dynlink): ${line:0:200}"; tail -20 "$WD/log.$out"; return 1
    fi
  done < "$WD/dyncmds"
  # otherlibs: BEST_OCAMLOPT (Makefile.best_binaries)
  for l in $OTHERLIBS; do
    if ! ( cd "$WD/t/otherlibs/$l" && make allopt BEST_OCAMLOPT="$c" BEST_OCAMLC="$ROOT/ocamlc.opt" ) >> "$WD/log.$out" 2>&1; then
      echo "FAIL ($out, otherlibs/$l)"; tail -20 "$WD/log.$out"; return 1
    fi
  done
  rm -rf "$WD/$out"; mv "$WD/t" "$WD/$out"
}

# dynlink's commands, as make would run them after its sources changed
( cd "$ROOT" && make -n V=1 -W otherlibs/dynlink/dynlink_config.mli -W otherlibs/dynlink/dynlink_types.mli \
    otherlibs/dynlink/dynlink.cmxa 2>/dev/null | grep '^\./ocaml' ) > "$WD/dyncmds"
echo "dynlink commands: $(wc -l < "$WD/dyncmds")"
build "$REF" ref || { echo "the reference build failed"; exit 1; }
build "$CPP" port || exit 1
cd "$WD/ref" || exit 1
same=0; diff=0; missing=0
: > "$WD/diffs"
for d in stdlib $(for l in $OTHERLIBS; do echo otherlibs/$l; done) otherlibs/dynlink otherlibs/dynlink/native; do
  for f in $(cd $d && ls *.cmx *.o *.cmxa *.cmxs *.a *.cmt *.cmti 2>/dev/null | sort); do
    f=$d/$f
    if [ ! -e "$WD/port/$f" ]; then missing=$((missing + 1)); echo "MISSING $f" >> "$WD/diffs"
    elif cmp -s "$f" "$WD/port/$f"; then same=$((same + 1))
    else diff=$((diff + 1)); echo "DIFF $f" >> "$WD/diffs"; fi
  done
done
echo "artifacts: SAME $same  DIFF $diff  MISSING $missing"
head -20 "$WD/diffs"
