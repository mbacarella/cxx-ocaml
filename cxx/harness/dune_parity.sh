#!/usr/bin/env bash
# dune oracle: a dune build with c++ocamlc as the installation's ocamlc
# against the same build with the stock one, artifact for artifact.
#
# This tree is staged once (`make install DESTDIR=$STAGE`; reused when
# present).  Each project is copied to the SAME work directory and built
# twice with the SAME installation path -- once with the stock
# bin/ocamlc.opt, once with c++ocamlc copied over it (bin/ocamlc is a
# symlink to it) -- so the paths the artifacts record (debug directories,
# .cmt build dirs and argv[0], BUILD_PATH_PREFIX_MAP) are the same.  Every
# file under _build/default present after the stock build is compared byte
# for byte, and so is dune's own output (stdout, stderr, exit code).
# A differing file is classified: SHARING for a .cmt / .cmti whose contents
# are equal (cxx/harness/cmt/cmtdump.ml, as cmt_parity.sh) though Marshal's
# sharing differs; NONDET when a second stock build shows ocamlc.opt itself
# does not reproduce the file (e.g. -output-complete-exe's C toolchain
# output); DIFF otherwise.
# ocamlopt, ocamllex, ocamlyacc and ocamldep are the stock ones on both
# sides.
#
# Usage: dune_parity.sh [project-dir ...]
#   default: cxx/harness/dune_projects/* (target @default), then the real
#   projects in REAL_SRC (target @install)
# Environment:
#   STAGE=/tmp/dune_parity_stage   the staged installation
#   DUNE=                          the dune binary (default: from PATH, else
#                                  the newest ~/.opam/*/bin/dune)
#   REAL_SRC=~/.opam/5.3.0/.opam-switch/sources
#   REAL="fix.20250919 ..."        the real projects (pure OCaml, no deps
#                                  beyond the stdlib for @install)
#   TARGETS=                       override the dune targets for every project
#   OUT=/tmp/dune_parity           per-project artifacts and reports
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
STAGE="${STAGE:-/tmp/dune_parity_stage}"
OUT="${OUT:-/tmp/dune_parity}"
REAL_SRC="${REAL_SRC:-$HOME/.opam/5.3.0/.opam-switch/sources}"
REAL="${REAL:-fix.20250919 ocaml-version.4.1.1 octavius.1.2.2 domain-name.0.5.0 ohex.0.2.0 spdx_licenses.1.4.0 swhid_core.0.1 tsort.2.2.0 re.1.14.0 base64.3.5.2 menhir.20250912}"
DUNE="${DUNE:-$(command -v dune 2>/dev/null || ls -t "$HOME"/.opam/*/bin/dune 2>/dev/null | head -1)}"
[ -x "$DUNE" ] || { echo "dune_parity.sh: no dune binary (set DUNE=)" >&2; exit 2; }
PFX="$STAGE/usr/local"
if [ ! -x "$PFX/bin/ocamlc.opt" ] || [ ! -f "$STAGE/stock_ocamlc.opt" ]; then
  echo "== staging this tree into $STAGE" >&2
  rm -rf "${STAGE:?}"
  make -C "$ROOT" install DESTDIR="$STAGE" >/dev/null 2>&1 || { echo "dune_parity.sh: make install failed" >&2; exit 2; }
  cp -p "$PFX/bin/ocamlc.opt" "$STAGE/stock_ocamlc.opt"
fi
WORK=/tmp/dune_parity_work   # the one build directory both runs use
mkdir -p "$OUT"
ulimit -v 16000000
# the structural .cmt dumper (shared with cmt_parity.sh)
TOOLS="${TOOLS:-/tmp/cmt_parity_tools}"  # built with this tree's ocamlc.opt
if [ ! -x "$TOOLS/cmtdump" ] || [ "$ROOT/cxx/harness/cmt/cmtdump.ml" -nt "$TOOLS/cmtdump" ]; then
  mkdir -p "$TOOLS"
  ( cd "$TOOLS" && cp "$ROOT/cxx/harness/cmt/cmtdump.ml" . &&
    "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -I "$ROOT/compilerlibs" -I "$ROOT/utils" -I "$ROOT/typing" \
      -I "$ROOT/parsing" -I "$ROOT/file_formats" -I "$ROOT/driver" -I "$ROOT/lambda" \
      "$ROOT/compilerlibs/ocamlcommon.cma" cmtdump.ml -o cmtdump ) >/dev/null 2>&1 ||
    { echo "dune_parity.sh: cannot build cmtdump" >&2; exit 2; }
fi
cmt_contents_equal() {  # <file-a> <file-b>
  "$ROOT/runtime/ocamlrun" "$TOOLS/cmtdump" "$1" > "$OUT/.dump.a" 2>&1 &&
    "$ROOT/runtime/ocamlrun" "$TOOLS/cmtdump" "$2" > "$OUT/.dump.b" 2>&1 &&
    cmp -s "$OUT/.dump.a" "$OUT/.dump.b"
}

projects=()
if [ $# -gt 0 ]; then
  for p in "$@"; do projects+=("$(readlink -f "$p")"); done
else
  for p in "$ROOT"/cxx/harness/dune_projects/*/; do projects+=("${p%/}"); done
  for r in $REAL; do [ -d "$REAL_SRC/$r" ] && projects+=("$REAL_SRC/$r"); done
fi

# the compiler installed as bin/ocamlc.opt (bin/ocamlc points at it)
install_compiler() {
  rm -f "$PFX/bin/ocamlc.opt"
  cp -p "$1" "$PFX/bin/ocamlc.opt"
  ln -sfn ocamlc.opt "$PFX/bin/ocamlc"
}

build() {  # <project> <side> <targets...>
  local p="$1" side="$2"; shift 2
  rm -rf "${WORK:?}"
  cp -a "$p" "$WORK"
  chmod -R u+w "$WORK"
  rm -rf "$WORK/_build"
  ( cd "$WORK" && env -i HOME="$HOME" PATH="$PFX/bin:/usr/bin:/bin" OCAMLLIB="$PFX/lib/ocaml" \
      "$DUNE" build --root . "$@" ) >"$OUT/$name.$side.out" 2>&1
  echo $? > "$OUT/$name.$side.rc"
  rm -rf "${OUT:?}/$name/$side"
  mkdir -p "$OUT/$name"
  [ -d "$WORK/_build/default" ] && cp -a "$WORK/_build/default" "$OUT/$name/$side"
}

total_same=0; total_sharing=0; total_nondet=0; total_diff=0; total_missing=0
printf '%-28s %6s %8s %7s %6s %8s  %s\n' project same sharing nondet diff missing dune >&2
for p in "${projects[@]}"; do
  name=$(basename "$p")
  case "$p" in "$ROOT"/cxx/harness/dune_projects/*) t=(@default) ;; *) t=(@install) ;; esac
  [ -n "${TARGETS:-}" ] && read -r -a t <<< "$TARGETS"
  install_compiler "$STAGE/stock_ocamlc.opt"; build "$p" ref "${t[@]}"
  install_compiler "$CPP"; build "$p" cpp "${t[@]}"
  install_compiler "$STAGE/stock_ocamlc.opt"
  same=0; sharing=0; nondet=0; diff=0; missing=0
  : > "$OUT/$name.report"
  others=()
  if [ -d "$OUT/$name/ref" ]; then
    while IFS= read -r f; do
      if [ ! -e "$OUT/$name/cpp/$f" ]; then missing=$((missing + 1)); echo "MISSING $f" >> "$OUT/$name.report"
      elif cmp -s "$OUT/$name/ref/$f" "$OUT/$name/cpp/$f"; then same=$((same + 1))
      elif case "$f" in *.cmt|*.cmti) cmt_contents_equal "$OUT/$name/ref/$f" "$OUT/$name/cpp/$f" ;; *) false ;; esac; then
        sharing=$((sharing + 1)); echo "SHARING $f" >> "$OUT/$name.report"
      else others+=("$f"); fi
    done < <(cd "$OUT/$name/ref" && find . -type f ! -name '*.log' | sort)
  fi
  if [ ${#others[@]} -gt 0 ]; then  # does ocamlc.opt reproduce these itself?
    install_compiler "$STAGE/stock_ocamlc.opt"; build "$p" ref2 "${t[@]}"
    for f in "${others[@]}"; do
      if [ -e "$OUT/$name/ref2/$f" ] && ! cmp -s "$OUT/$name/ref/$f" "$OUT/$name/ref2/$f"; then
        nondet=$((nondet + 1)); echo "NONDET $f" >> "$OUT/$name.report"
      else diff=$((diff + 1)); echo "DIFF $f" >> "$OUT/$name.report"; fi
    done
  fi
  duneout=same
  cmp -s "$OUT/$name.ref.rc" "$OUT/$name.cpp.rc" && cmp -s "$OUT/$name.ref.out" "$OUT/$name.cpp.out" || duneout=DIFF
  [ "$duneout" = DIFF ] && echo "DUNE-OUTPUT differs (rc $(cat "$OUT/$name.ref.rc") vs $(cat "$OUT/$name.cpp.rc"))" >> "$OUT/$name.report"
  printf '%-28s %6d %8d %7d %6d %8d  %s (rc %s)\n' "$name" "$same" "$sharing" "$nondet" "$diff" "$missing" "$duneout" "$(cat "$OUT/$name.ref.rc")"
  total_same=$((total_same + same)); total_sharing=$((total_sharing + sharing)); total_nondet=$((total_nondet + nondet))
  total_diff=$((total_diff + diff)); total_missing=$((total_missing + missing))
done
echo "artifacts: SAME $total_same  SHARING $total_sharing  NONDET $total_nondet  DIFF $total_diff  MISSING $total_missing   (reports: $OUT/<project>.report, dune output: $OUT/<project>.{ref,cpp}.out)"
