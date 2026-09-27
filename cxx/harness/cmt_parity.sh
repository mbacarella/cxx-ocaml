#!/usr/bin/env bash
# -bin-annot oracle (TYPECHECKER.md, Driver options): the .cmt / .cmti
# c++ocamlc writes against ocamlc.opt's.  cmt_args records argv (argv[0]
# included) and cmt_builddir the working directory, so each file is
# compiled by both compilers through the SAME path (a symlink `ocamlc` in a
# scratch bin directory, retargeted between the runs) with the same
# arguments, in the same directory.  A sibling .mli is compiled first (its
# .cmti compared too).  A unit that fails to type-check leaves a partial
# .cmt / .cmti, compared the same way.
#
#   SAME    the bytes are identical
#   SHARING the contents are (cmtdump.ml: every cmt_infos field, the typed
#           tree through Printtyped) but the bytes are not: Marshal's sharing
#   DIFF    the contents differ
#   CFAIL / OFAIL  c++ocamlc / ocamlc.opt wrote no .cmt where the other did
#
# Usage: cmt_parity.sh [file.ml|file.mli ...]   (JOBS=, FLAGS= extra flags
#   for both, W= the warning flags, default "-w -a")
#   no args: cxx/harness/stamp_probes
#   --stdlib    the stdlib, compiled as stdlib/Makefile does (Compflags,
#               stdlib__X targets, its COMPFLAGS incl. -g) against the built
#               stdlib's .cmi, one unit at a time
#   --compiler  the compiler's 139 modules (ocamlc_bootstrap.sh's CL_COMMON
#               / CL_BYTE, flattened as effid.sh does) with the Makefile's
#               OC_COMMON_COMPFLAGS, in ocamldep -sort order, each compiler
#               against the .cmi it wrote itself
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

# judge KEY NAME: the worst of the unit's .cmt / .cmti ($OUT/KEY.o|c.ext)
judge() {
  local key="$1" res="" r o c ext
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
    case "$res:$r" in
      :*) res=$r ;;
      *:DIFF|*:CFAIL|*:OFAIL) res=$r ;;
      SAME:SHARING) res=SHARING ;;
    esac
  done
  [ -z "$res" ] && res=OFAIL
  printf '%s %s\n' "$res" "$2"
}

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
  judge "$key" "$f"
  exit 0
fi

if [ "${1:-}" == "--stdlib-worker" ]; then
  m="$2"; base=${m%.ml}; key=stdlib_$base
  case "$base" in
    camlinternal*|std_exit|stdlib) tgt=$base ;;
    *) tgt=stdlib__$(echo "${base:0:1}" | tr a-z A-Z)${base:1} ;;
  esac
  cd "$ROOT/stdlib" || exit 1
  cflags=$(sh ./Compflags "$tgt.cmo" | sed "s|\./expand_module_aliases.awk|$ROOT/stdlib/expand_module_aliases.awk|")
  iflags=$(sh ./Compflags "$tgt.cmi" | sed "s|\./expand_module_aliases.awk|$ROOT/stdlib/expand_module_aliases.awk|")
  COMP="-strict-sequence -absname -w +a-4-9-41-42-44-45-48 -g -warn-error +A -alert @ocaml_deprecated_cli -bin-annot -nostdlib -principal"
  w=$(mktemp -d); mkdir "$w/bin" "$w/x"
  for side in o c; do
    rm -rf "${w:?}/x"; mkdir "$w/x"
    cp "$m" "$w/x/"; [ -f "$base.mli" ] && cp "$base.mli" "$w/x/"
    if [ $side = o ]; then ln -sfn "$REF" "$w/bin/ocamlc"; else ln -sfn "$CPP" "$w/bin/ocamlc"; fi
    ( cd "$w/x"
      if [ -f "$base.mli" ]; then eval "$w/bin/ocamlc $COMP $iflags -I $ROOT/stdlib -o $tgt.cmi -c $base.mli" || exit 1; fi
      eval "$w/bin/ocamlc $COMP $cflags -I $ROOT/stdlib -o $tgt.cmo -c $m" ) >/dev/null 2>&1
    for ext in cmt cmti; do [ -f "$w/x/$tgt.$ext" ] && cp "$w/x/$tgt.$ext" "$OUT/$key.$side.$ext"; done
  done
  rm -rf "${w:?}"
  judge "$key" "stdlib/$m"
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
export -f judge; export AWK=awk
ulimit -v 8000000

if [ "${1:-}" == "--stdlib" ]; then
  res=$(cd stdlib && ls *.ml | xargs -P "$JOBS" -I{} bash "$SELF" --stdlib-worker {} | sort -k2)
elif [ "${1:-}" == "--compiler" ]; then
  BOOTSTRAP=$ROOT/cxx/harness/ocamlc_bootstrap.sh
  srcs=$(awk '
    /^CL_COMMON="/{g=1; sub(/^CL_COMMON="/,"")}
    /^CL_BYTE="/{g=1; sub(/^CL_BYTE="/,"")}
    g{l=$0; sub(/"[[:space:]]*$/,"",l); n=split(l,a,/[[:space:]]+/);
      for(i=1;i<=n;i++) if(a[i]!="") print a[i];
      if($0 ~ /"[[:space:]]*$/) g=0}' "$BOOTSTRAP")
  COMP="-g -strict-sequence -principal -absname -w +a-4-9-40-41-42-44-45-48 -alert @ocaml_deprecated_cli -bin-annot -strict-formats"
  w=/tmp/cmt_parity_compiler; rm -rf "$w"; mkdir -p "$w/bin" "$w/x"
  for f in $srcs; do cp "$ROOT/$f" "$w/x/"; done
  ORDER=$(cd "$w/x" && "$ROOT/tools/ocamldep.opt" -sort ./*.mli ./*.ml 2>/dev/null | sed 's|\./||g')
  cp -r "$w/x" "$w/src"
  for side in o c; do
    rm -rf "${w:?}/x"; cp -r "$w/src" "$w/x"
    if [ $side = o ]; then ln -sfn "$REF" "$w/bin/ocamlc"; else ln -sfn "$CPP" "$w/bin/ocamlc"; fi
    ( cd "$w/x"; for f in $ORDER; do "$w/bin/ocamlc" -nostdlib -I . -I "$ROOT/stdlib" $COMP -c "$f" >/dev/null 2>&1; done )
    for f in "$w"/x/*.cmt "$w"/x/*.cmti; do [ -f "$f" ] || continue
      b=$(basename "$f"); cp "$f" "$OUT/compiler_${b%.*}.$side.${b##*.}"; done
  done
  res=$(for f in $ORDER; do case "$f" in *.ml) echo "compiler_${f%.ml} $f" ;; *.mli) [ -f "$w/src/${f%.mli}.ml" ] || echo "compiler_${f%.mli} $f" ;; esac; done |
        while read -r key f; do judge "$key" "$f"; done | sort -k2)
else
  if [ $# -gt 0 ]; then files=("$@"); else mapfile -t files < <(ls cxx/harness/stamp_probes/*.ml); fi
  res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
fi
printf '%s\n' "$res" > /tmp/.cmt_parity_results
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "files %d: SAME %d  SHARING %d  DIFF %d  CFAIL %d  OFAIL %d\n", NR, f["SAME"], f["SHARING"], f["DIFF"], f["CFAIL"], f["OFAIL"] }'
echo "per-file results: /tmp/.cmt_parity_results (files in $OUT: <key>.o|c.cmt[i], .dump for the differing)"
