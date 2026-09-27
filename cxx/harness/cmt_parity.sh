#!/usr/bin/env bash
# -bin-annot oracle (TYPECHECKER.md, Driver options): the .cmt / .cmti
# c++ocamlc writes against ocamlc.opt's.  cmt_args records argv (argv[0]
# included) and cmt_builddir the working directory, so each file is
# compiled by both compilers through the SAME path (a symlink `ocamlc` in a
# scratch bin directory, retargeted between the runs) with the same
# arguments, in the same directory.  A sibling .mli is compiled first (its
# .cmti compared too).
#
#   SAME    the bytes are identical
#   SHARING the contents are (cmtdump.ml: every cmt_infos field, the typed
#           tree through Printtyped) but the bytes are not: Marshal's sharing
#   DIFF    the contents differ
#   CFAIL / OFAIL  c++ocamlc / ocamlc.opt failed or wrote no .cmt
#
# Usage: cmt_parity.sh [file.ml|file.mli ...]   (JOBS=, FLAGS= extra flags
#   for both, W= the warning flags, default "-w -a")
#   no args: cxx/harness/stamp_probes
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
FLAGS="${FLAGS:-}"
W="${W--w -a}"
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
REF="${REF:-$ROOT/ocamlc.opt}"
OUT=/tmp/cmt_parity
TOOLS=/tmp/cmt_parity_tools
export FLAGS W CPP REF OUT TOOLS

if [ "${1:-}" == "--worker" ]; then
  f="$2"; b=$(basename "$f"); key=$(echo "$f" | tr '/' '_')
  w=$(mktemp -d); mkdir "$w/bin" "$w/x"
  base="${b%.*}"
  for side in o c; do
    rm -rf "${w:?}/x"; mkdir "$w/x"
    cp "$f" "$w/x/"
    case "$b" in *.ml) [ -f "${f}i" ] && cp "${f}i" "$w/x/" ;; esac
    if [ $side = o ]; then ln -sfn "$REF" "$w/bin/ocamlc"; else ln -sfn "$CPP" "$w/bin/ocamlc"; fi
    ( cd "$w/x"
      if [ -f "$base.mli" ] && [ "$b" != "$base.mli" ]; then
        timeout 120 "$w/bin/ocamlc" -nostdlib -I "$ROOT/stdlib" $W -bin-annot $FLAGS -c "$base.mli" || exit 1
      fi
      timeout 120 "$w/bin/ocamlc" -nostdlib -I "$ROOT/stdlib" $W -bin-annot $FLAGS -c "$b" ) >/dev/null 2>&1
    for ext in cmt cmti; do [ -f "$w/x/$base.$ext" ] && cp "$w/x/$base.$ext" "$OUT/$key.$side.$ext"; done
  done
  rm -rf "${w:?}"
  res=""
  for ext in cmt cmti; do
    o="$OUT/$key.o.$ext"; c="$OUT/$key.c.$ext"
    [ -f "$o" ] || [ -f "$c" ] || continue
    if [ ! -f "$o" ]; then r=OFAIL
    elif [ ! -f "$c" ]; then r=CFAIL
    elif cmp -s "$o" "$c"; then r=SAME
    else
      "$ROOT/runtime/ocamlrun" "$TOOLS/cmtdump" "$o" > "$o.dump" 2>&1
      "$ROOT/runtime/ocamlrun" "$TOOLS/cmtdump" "$c" > "$c.dump" 2>&1
      if cmp -s "$o.dump" "$c.dump"; then r=SHARING; else r=DIFF; fi
    fi
    # the worst of the unit's files
    case "$res:$r" in
      :*) res=$r ;;
      *:DIFF|*:CFAIL|*:OFAIL) res=$r ;;
      SAME:SHARING) res=SHARING ;;
    esac
  done
  [ -z "$res" ] && res=OFAIL
  printf '%s %s\n' "$res" "$f"
  exit 0
fi

rm -rf "$OUT"; mkdir -p "$OUT" "$TOOLS"
# the structural dumper, over the tree's compiler-libs
if [ ! -x "$TOOLS/cmtdump" ] || [ "$SELF" -nt "$TOOLS/cmtdump" ] || [ "$ROOT/cxx/harness/cmt/cmtdump.ml" -nt "$TOOLS/cmtdump" ]; then
  ( cd "$TOOLS" && cp "$ROOT/cxx/harness/cmt/cmtdump.ml" . &&
    "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -I "$ROOT/compilerlibs" -I "$ROOT/utils" -I "$ROOT/typing" \
      -I "$ROOT/parsing" -I "$ROOT/file_formats" -I "$ROOT/driver" -I "$ROOT/lambda" \
      "$ROOT/compilerlibs/ocamlcommon.cma" cmtdump.ml -o cmtdump ) || { echo "cmt_parity.sh: cannot build cmtdump" >&2; exit 2; }
fi
if [ $# -gt 0 ]; then files=("$@"); else mapfile -t files < <(ls cxx/harness/stamp_probes/*.ml); fi
ulimit -v 8000000
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.cmt_parity_results
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "files %d: SAME %d  SHARING %d  DIFF %d  CFAIL %d  OFAIL %d\n", NR, f["SAME"], f["SHARING"], f["DIFF"], f["CFAIL"], f["OFAIL"] }'
echo "per-file results: /tmp/.cmt_parity_results (files in $OUT: <key>.o|c.cmt[i], .dump for the differing)"
