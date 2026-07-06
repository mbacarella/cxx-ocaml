#!/usr/bin/env bash
# Sibling-.cmi builder (bucket A's second half: test-local sibling modules).
#
# Some testsuite files reference modules defined in SIBLING .ml files of the
# same test directory (multi-file tests: Store, Waitgroup, M/A/B, ...).  The
# real testsuite compiles those siblings first; our per-file oracle calls do
# not, so such files were "oracle-rejected" (Unbound module) and invisible to
# every typer gate -- and c++type kept an absorbing any() for them (site A).
# This script gives BOTH sides the same build context, like the otherlibs -I
# wiring did: for each directory that needs it, compile every sibling .ml/.mli
# to .cmi under $SIBROOT/<dir> (fixpoint, so in-dir dependency chains resolve).
# The oracle then adds `-I $SIBROOT/<dir>` and c++type derives the same dir
# from $CPPCAML_SIB_CMI_ROOT.
#
# Only directories where an oracle-rejected file fails with "Unbound module X"
# AND x.ml exists as a sibling are populated (blast-radius control: dirs whose
# files all type already keep their exact current context).  The dir list is
# persisted in $SIBROOT/DIRS so cache rebuilds are reproducible after the
# sibling files themselves start typing.
#
# Usage: sib_cmis.sh            # probe + build (probe needs the oracle cache)
#        sib_cmis.sh --rebuild  # rebuild cmis for the persisted DIRS only
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT="$PWD"
SIBROOT="${SIBROOT:-/tmp/sib_cmi}"
CACHE="${CACHE:-/tmp/ttp_oracle_cache}"
JOBS="${JOBS:-$(nproc)}"
OTHERLIBS="-I $ROOT/otherlibs/unix -I $ROOT/otherlibs/str \
  -I $ROOT/otherlibs/systhreads -I $ROOT/otherlibs/runtime_events \
  -I $ROOT/otherlibs/dynlink"
key() { printf '%s' "$1" | tr '/' '%'; }

# Probe worker: does this oracle-rejected file fail on a sibling-resolvable
# "Unbound module"?  If so print its directory.
if [ "${1:-}" = "--probe-one" ]; then
  f="$2"
  err=$(timeout 20 ./ocamlc.opt -nostdlib -I stdlib $OTHERLIBS \
        -stop-after typing "$f" 2>&1 1>/dev/null \
        | grep -m1 -oE 'Unbound module [A-Za-z0-9_]+')
  [ -z "$err" ] && exit 0
  mod=${err##* }
  d=$(dirname "$f")
  low=$(printf '%s' "${mod:0:1}" | tr 'A-Z' 'a-z')${mod:1}
  { [ -f "$d/$mod.ml" ] || [ -f "$d/$low.ml" ]; } && echo "$d"
  exit 0
fi

# Build worker: fixpoint-compile one directory's siblings into $SIBROOT/<dir>.
if [ "${1:-}" = "--build-one" ]; then
  d="$2"
  out="$SIBROOT/$d"
  rm -rf "$out"; mkdir -p "$out"
  cp "$d"/*.ml "$out"/ 2>/dev/null
  cp "$d"/*.mli "$out"/ 2>/dev/null
  cd "$out" || exit 1
  prev=-1
  while :; do
    for f in *.mli *.ml; do
      [ -f "$f" ] || continue
      base=${f%.*}
      [ -f "$base.cmi" ] && { [ "${f##*.}" = "mli" ] || [ -f "$base.cmo" ]; } && continue
      timeout 20 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" $OTHERLIBS \
        -I . -c "$f" >/dev/null 2>&1
    done
    n=$(ls *.cmi 2>/dev/null | wc -l)
    [ "$n" -eq "$prev" ] && break
    prev=$n
  done
  exit 0
fi

mkdir -p "$SIBROOT"
DIRS="$SIBROOT/DIRS"
touch "$DIRS"

if [ "${1:-}" != "--rebuild" ]; then
  # Probe: oracle-rejected corpus files = empty entries in the ttp oracle cache.
  if [ ! -d "$CACHE" ]; then
    echo "sib_cmis.sh: no oracle cache at $CACHE -- build it first" >&2; exit 1
  fi
  rejected=$(find testsuite/tests -name '*.ml' | sort | while read -r f; do
    c="$CACHE/$(key "$f")"
    [ -f "$c" ] && [ ! -s "$c" ] && echo "$f"
  done)
  echo "probing $(wc -l <<<"$rejected") oracle-rejected files ..." >&2
  probed=$(xargs -P "$JOBS" -I{} bash "$SELF" --probe-one {} <<<"$rejected" | sort -u)
  sort -u "$DIRS" <(printf '%s\n' "$probed") | grep -v '^$' > "$DIRS.new"
  mv "$DIRS.new" "$DIRS"
fi

echo "building sibling cmis for $(wc -l <"$DIRS") dirs under $SIBROOT ..." >&2
xargs -P "$JOBS" -I{} bash "$SELF" --build-one {} < "$DIRS"
echo "done: $(find "$SIBROOT" -name '*.cmi' | wc -l) cmis" >&2
