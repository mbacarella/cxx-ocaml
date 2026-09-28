#!/usr/bin/env bash
# ppx oracle (TYPECHECKER.md, Driver options): c++ocamlc against
# ocamlc.opt on the two ways an AST reaches the compiler from outside --
#   ppx  `-ppx CMD`: Pparse writes the parsed AST (with Ast_mapper's ppx
#        context), runs the rewriter, reads its output back
#   ast  a binary AST file (what a dune ppx driver hands the compiler as
#        `-impl foo.pp.ml` / `-intf foo.pp.mli`), written by mkast
#        (Pparse.parse_* + Pparse.write_ast from compiler-libs)
# The rewriters are ppx/ppx_tests.ml's Ast_mapper mappers (id ext derive
# attr ghost none bad), built with the tree's ocamlc.opt against its
# compiler-libs.  Each unit (and a sibling .mli first) is compiled with
# -bin-annot by both compilers through one symlink `ocamlc` (argv[0] is in
# the .cmt), in the same directory; the .cmo / .cmi / .cmt / .cmti, stderr
# (a -ppx command line's temporary file names normalized) and the exit
# code are compared byte for byte.
#
# Usage: ppx_parity.sh [file.ml|dir ...]   (JOBS=, MODE=ast|ppx|both (default
#   both) or src (the sources themselves, no rewriter: the baseline) or
#   written (the AST file each compiler writes for a rewriter, compared:
#   SHARING = the same tree, Marshal's sharing aside), MAP= the mapper for named files (default id), W= the warning
#   flags (default "-w -a"), FLAGS= extra flags for both, OROOT= the built
#   OCaml tree, default this one)
#   no args: every cxx/harness/ppx/tests/<mapper>_*.ml with its mapper
#   SAME / DIFF (<what differs>) / BOTHFAIL (neither compiled: an error
#   report, compared) -- a DIFF lists the artifacts that differ
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
MODE="${MODE:-both}"
MAP="${MAP:-id}"
W="${W--w -a}"
FLAGS="${FLAGS:-}"
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
# the built OCaml tree (ocamlc.opt, stdlib, compiler-libs, runtime): this
# one, or OROOT= (e.g. a worktree of the C++ sources only)
OROOT="${OROOT:-$ROOT}"
REF="${REF:-$OROOT/ocamlc.opt}"
OUT=/tmp/ppx_parity
TOOLS=/tmp/ppx_parity_tools
export W FLAGS CPP REF OUT TOOLS ROOT OROOT

if [ "${1:-}" == "--worker" ]; then
  mode="$2"; map="$3"; f="$4"
  b=$(basename "$f"); base=${b%.ml}; key="$mode.$map.$(echo "$f" | tr '/' '_')"
  w=$(mktemp -d); mkdir "$w/bin" "$w/x"
  if [ "$mode" = written ]; then
    # the AST file each compiler writes for a rewriter (a copying one)
    for side in o c; do
      if [ $side = o ]; then ln -sfn "$REF" "$w/bin/ocamlc"; else ln -sfn "$CPP" "$w/bin/ocamlc"; fi
      rm -rf "${w:?}/x"; mkdir "$w/x"; cp "$f" "$w/x/"
      ( cd "$w/x" && AST_COPY="$OUT/$key.$side.ast" timeout 120 "$w/bin/ocamlc" -nostdlib -I "$OROOT/stdlib" $W $FLAGS \
          -ppx "$TOOLS/copy.sh" -c "$b" ) > /dev/null 2>&1
    done
    rm -rf "${w:?}"
    o="$OUT/$key.o.ast"; c="$OUT/$key.c.ast"
    if [ ! -f "$o" ] && [ ! -f "$c" ]; then printf 'BOTHFAIL %s %s %s\n' "$mode" "$map" "$f"
    elif cmp -s "$o" "$c"; then printf 'SAME %s %s %s\n' "$mode" "$map" "$f"
    else
      "$OROOT/runtime/ocamlrun" "$TOOLS/asttree" "$o" > "$o.tree" 2>&1
      "$OROOT/runtime/ocamlrun" "$TOOLS/asttree" "$c" > "$c.tree" 2>&1
      if cmp -s "$o.tree" "$c.tree"; then printf 'SHARING %s %s %s\n' "$mode" "$map" "$f"
      else printf 'DIFF %s %s %s ( content )\n' "$mode" "$map" "$f"; fi
    fi
    exit 0
  fi
  ppx="$TOOLS/ppx.sh $map"
  arts=""
  for side in o c; do
    rm -rf "${w:?}/x"; mkdir "$w/x"
    cp "$f" "$w/x/"; [ -f "${f}i" ] && cp "${f}i" "$w/x/"
    if [ $side = o ]; then ln -sfn "$REF" "$w/bin/ocamlc"; else ln -sfn "$CPP" "$w/bin/ocamlc"; fi
    (
      cd "$w/x" || exit 1
      if [ "$mode" = ast ]; then
        # as dune: the .pp.ml's unit is named by -o
        [ -f "$base.mli" ] && "$TOOLS/mkast.sh" -intf -map "$map" "$base.mli" "$base.pp.mli"
        "$TOOLS/mkast.sh" -map "$map" "$b" "$base.pp.ml"
        if [ -f "$base.pp.mli" ]; then
          timeout 120 "$w/bin/ocamlc" -nostdlib -I "$OROOT/stdlib" $W -bin-annot $FLAGS -o "$base.cmi" -c -intf "$base.pp.mli" || exit $?
        fi
        timeout 120 "$w/bin/ocamlc" -nostdlib -I "$OROOT/stdlib" $W -bin-annot $FLAGS -o "$base.cmo" -c -impl "$base.pp.ml"
      else
        # src: the source itself, no rewriter (the baseline of the two others)
        if [ "$mode" = ppx ]; then pp=(-ppx "$ppx"); else pp=(); fi
        if [ -f "$base.mli" ]; then
          timeout 120 "$w/bin/ocamlc" -nostdlib -I "$OROOT/stdlib" $W -bin-annot $FLAGS "${pp[@]}" -c "$base.mli" || exit $?
        fi
        timeout 120 "$w/bin/ocamlc" -nostdlib -I "$OROOT/stdlib" $W -bin-annot $FLAGS "${pp[@]}" -c "$b"
      fi
    ) > "$OUT/$key.$side.out" 2> "$OUT/$key.$side.err"
    echo $? > "$OUT/$key.$side.rc"
    sed -E -i 's/camlppx[0-9a-f]{6}/camlppxXXXXXX/g' "$OUT/$key.$side.err"
    for a in "$w"/x/*.cmo "$w"/x/*.cmi "$w"/x/*.cmt "$w"/x/*.cmti; do
      [ -f "$a" ] || continue
      cp "$a" "$OUT/$key.$side.$(basename "$a")"
      arts="$arts $(basename "$a")"
    done
  done
  rm -rf "${w:?}"
  diffs=""
  for a in $(printf '%s\n' $arts | sort -u); do
    cmp -s "$OUT/$key.o.$a" "$OUT/$key.c.$a" || diffs="$diffs $a"
  done
  cmp -s "$OUT/$key.o.err" "$OUT/$key.c.err" || diffs="$diffs stderr"
  cmp -s "$OUT/$key.o.rc" "$OUT/$key.c.rc" || diffs="$diffs rc"
  rc=$(cat "$OUT/$key.o.rc")
  if [ -n "$diffs" ]; then printf 'DIFF %s %s %s (%s )\n' "$mode" "$map" "$f" "$diffs"
  elif [ "$rc" != 0 ]; then printf 'BOTHFAIL %s %s %s\n' "$mode" "$map" "$f"
  else printf 'SAME %s %s %s\n' "$mode" "$map" "$f"; fi
  exit 0
fi

# the tools: mkast and ppx_tests over compiler-libs, run by the tree's ocamlrun
build_tools() {
  rm -rf "$TOOLS"; mkdir -p "$TOOLS"
  cp cxx/harness/ppx/*.ml "$TOOLS/"
  local inc="-I $OROOT/stdlib -I $OROOT/utils -I $OROOT/parsing -I $OROOT/typing -I $OROOT/driver -I $OROOT/file_formats"
  ( cd "$TOOLS" &&
    "$OROOT/ocamlc.opt" -nostdlib $inc -w -a "$OROOT/compilerlibs/ocamlcommon.cma" ppx_tests.ml mkast.ml -o mkast &&
    "$OROOT/ocamlc.opt" -nostdlib $inc -w -a "$OROOT/compilerlibs/ocamlcommon.cma" ppx_tests.cmo ppx_main.ml -o ppx_tests &&
    "$OROOT/ocamlc.opt" -nostdlib -I "$OROOT/stdlib" -w -a "$OROOT/stdlib/stdlib.cma" asttree.ml -o asttree ) ||
    { echo "ppx_parity.sh: cannot build the tools" >&2; exit 2; }
  printf '#!/bin/sh\nexec %s/runtime/ocamlrun %s/mkast "$@"\n' "$OROOT" "$TOOLS" > "$TOOLS/mkast.sh"
  printf '#!/bin/sh\nexec %s/runtime/ocamlrun %s/ppx_tests "$@"\n' "$OROOT" "$TOOLS" > "$TOOLS/ppx.sh"
  printf '#!/bin/sh\ncp "$1" "$AST_COPY" && cp "$1" "$2"\n' > "$TOOLS/copy.sh"
  chmod +x "$TOOLS/mkast.sh" "$TOOLS/ppx.sh" "$TOOLS/copy.sh"
}
build_tools
rm -rf "$OUT"; mkdir -p "$OUT"
modes=(ast ppx)
case "$MODE" in ast) modes=(ast) ;; ppx) modes=(ppx) ;; src) modes=(src) ;; written) modes=(written) ;; esac
jobs=()
if [ $# -gt 0 ]; then
  # a directory argument: its .ml files (testsuite-like trees: recursively)
  files=()
  for a in "$@"; do
    if [ -d "$a" ]; then mapfile -t -O "${#files[@]}" files < <(find "$a" -name '*.ml' | sort); else files+=("$a"); fi
  done
  for f in "${files[@]}"; do for m in "${modes[@]}"; do jobs+=("$m $MAP $f"); done; done
else
  for f in cxx/harness/ppx/tests/*.ml; do
    map=$(basename "$f"); map=${map%%_*}
    for m in "${modes[@]}"; do
      # a misbehaving rewriter is a -ppx matter only
      case "$m:$map" in ast:fail|ast:notfound|ast:nooutput|ast:junk) continue ;; esac
      jobs+=("$m $map $f")
    done
  done
fi
ulimit -v 8000000
res=$(printf '%s\n' "${jobs[@]}" | xargs -P "$JOBS" -L 1 bash "$SELF" --worker | sort -k4)
printf '%s\n' "$res" > /tmp/.ppx_parity_results
printf '%s\n' "$res" | grep -v '^SAME\|^BOTHFAIL\|^SHARING'
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "units %d: SAME %d  SHARING %d  BOTHFAIL %d  DIFF %d\n", NR, f["SAME"], f["SHARING"], f["BOTHFAIL"], f["DIFF"] }'
echo "per-unit results: /tmp/.ppx_parity_results (artifacts in $OUT)"
