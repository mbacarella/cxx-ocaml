#!/usr/bin/env bash
# .cmi parity: does c++ocamlc write the same INTERFACE ARTIFACT as ocamlc?
#
# For each corpus file both compilers `-c` a private copy (sibling .mli first
# when one exists -- then the .cmi under test is the .mli's, both sides), and
# BOTH resulting .cmi go through ONE decoder: cmiprint (compiler-libs
# Cmi_format.read_cmi + Printtyp.signature, built from cxx/harness/cmiprint.ml
# with the tree's own ocamlc.opt).  A diff is therefore a real signature
# difference, not a printing artifact; byte identity is NOT required (the
# project bar is valid + self-consistent -- CRCs/imports/flags are not diffed).
#
# Categories per file:
#   MATCH / DIFF  -- both sides produced a decodable .cmi; dumps equal or not
#   OERR          -- oracle -c failed: file is outside the cmi corpus
#   CERR          -- c++ocamlc exited nonzero (type error, codegen crash, timeout)
#   NOCMI         -- c++ocamlc succeeded but wrote no .cmi (best-effort writer bailed)
#   READERR       -- our .cmi exists but the oracle-side reader rejects it
#
# DIFF dumps are kept in /tmp/cmi_parity/diffs/<mangled>.{oracle,ours} for triage.
# Per-file labels land in /tmp/.cmi_results; the DIFF file list in /tmp/.cmi_diff_files.
#
# Usage: cmi_parity.sh [N]          (JOBS= overridable; N = limit corpus size)
#        cmi_parity.sh --one FILE   (single file, prints the dump diff)
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
CPP=$ROOT/cxx/build/c++ocamlc
ORACLE=$ROOT/ocamlc.opt
RUN=$ROOT/runtime/ocamlrun
JOBS="${JOBS:-$(nproc)}"
TIMEOUT="${TIMEOUT:-15}"

BIN=/tmp/cmi_parity/bin
WORK=/tmp/cmi_parity/work
DIFFS=/tmp/cmi_parity/diffs
CMIPRINT=$BIN/cmiprint

INCS="-I otherlibs/unix -I otherlibs/str -I otherlibs/systhreads \
-I otherlibs/runtime_events -I otherlibs/dynlink"

# Sibling-module cmis (multi-file tests) -- same context on both sides.
export CPPCAML_SIB_CMI_ROOT="${CPPCAML_SIB_CMI_ROOT:-/tmp/sib_cmi}"

build_cmiprint() {
  src=$ROOT/cxx/harness/cmiprint.ml
  [ -x "$CMIPRINT" ] && [ "$CMIPRINT" -nt "$src" ] && return 0
  mkdir -p "$BIN" && cp "$src" "$BIN/cmiprint.ml"
  "$ORACLE" -nostdlib -I stdlib -I compilerlibs -I utils -I typing -I parsing \
    -I file_formats compilerlibs/ocamlcommon.cma "$BIN/cmiprint.ml" \
    -o "$CMIPRINT" || { echo "FATAL: cmiprint build failed" >&2; exit 1; }
}

if [ "${1:-}" == "--worker" ] || [ "${1:-}" == "--one" ]; then
  verbose=0; [ "$1" == "--one" ] && { verbose=1; build_cmiprint; }
  f="$2"
  base=$(basename "$f" .ml)
  sib=""; sd="$CPPCAML_SIB_CMI_ROOT/$(dirname "$f")"; [ -d "$sd" ] && sib="-I $sd"
  mkdir -p "$WORK"
  td=$(mktemp -d "$WORK/w.XXXXXX") || exit 1
  trap 'rm -rf "$td"' EXIT
  mkdir -p "$td/o" "$td/u"
  src="$base.ml"; cp "$f" "$td/o/"; cp "$f" "$td/u/"
  if [ -f "${f%.ml}.mli" ]; then           # the .mli owns the .cmi on both sides
    cp "${f%.ml}.mli" "$td/o/"; cp "${f%.ml}.mli" "$td/u/"; src="$base.mli"
  fi
  if ! timeout "$TIMEOUT" "$ORACLE" -nostdlib -I stdlib $INCS $sib \
       -c "$td/o/$src" >/dev/null 2>&1; then
    printf 'OERR %s\n' "$f"; exit 0
  fi
  if ! timeout "$TIMEOUT" "$CPP" $INCS $sib -c "$td/u/$src" >"$td/cerr" 2>&1; then
    [ $verbose = 1 ] && sed 's/^/  c++ocamlc: /' "$td/cerr"
    printf 'CERR %s\n' "$f"; exit 0
  fi
  [ -f "$td/u/$base.cmi" ] || { printf 'NOCMI %s\n' "$f"; exit 0; }
  "$RUN" "$CMIPRINT" "$td/o/$base.cmi" > "$td/o.dump" 2>/dev/null \
    || { printf 'ODECODE %s\n' "$f"; exit 0; }   # oracle cmi unreadable: harness bug
  if ! "$RUN" "$CMIPRINT" "$td/u/$base.cmi" > "$td/u.dump" 2>"$td/rerr"; then
    [ $verbose = 1 ] && sed 's/^/  reader: /' "$td/rerr"
    printf 'READERR %s\n' "$f"; exit 0
  fi
  if cmp -s "$td/o.dump" "$td/u.dump"; then
    printf 'MATCH %s\n' "$f"
  else
    mkdir -p "$DIFFS"; m="${f//\//__}"
    cp "$td/o.dump" "$DIFFS/$m.oracle"; cp "$td/u.dump" "$DIFFS/$m.ours"
    [ $verbose = 1 ] && diff "$td/o.dump" "$td/u.dump" | sed 's/^/  /'
    printf 'DIFF %s\n' "$f"
  fi
  exit 0
fi

build_cmiprint
[ -d "$CPPCAML_SIB_CMI_ROOT" ] \
  || echo "note: $CPPCAML_SIB_CMI_ROOT missing (run sib_cmis.sh) -- multi-file tests will be OERR" >&2
rm -rf "$WORK" "$DIFFS"; mkdir -p "$WORK"

LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" > /tmp/.cmi_results
printf '%s\n' "$res" | awk '$1=="DIFF"{print $2}' | sort > /tmp/.cmi_diff_files
printf '%s\n' "$res" | awk '
  $1=="MATCH"{m++} $1=="DIFF"{d++} $1=="CERR"{ce++} $1=="NOCMI"{nc++}
  $1=="READERR"{re++} $1=="ODECODE"{od++} $1=="OERR"{oe++}
  END{
    corpus=m+d+ce+nc+re+od
    printf "files: %d   oracle-cmi corpus: %d\n", NR, corpus
    printf "cmi-identical: %d / %d   (%.1f%%)\n", m, corpus, corpus?100*m/corpus:0
    printf "DIFF %d   c++-err %d   no-cmi %d   unreadable %d   odecode %d\n", d, ce, nc, re, od
  }'
echo "diff dumps: $DIFFS/   labels: /tmp/.cmi_results"
