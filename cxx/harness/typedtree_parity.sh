#!/usr/bin/env bash
# Typer parity: diff c++type against `ocamlc -dtypedtree` over the corpus, with
# ident stamps normalized on both sides (decision 2026-06-05: normalize now,
# exact stamps later).  Run inside the nix dev shell (needs ./ocamlc.opt).
#
# Usage: typedtree_parity.sh [N]      # N>0 limits to the first N corpus files
#        JOBS=8 typedtree_parity.sh   # override parallelism (default: nproc)
#        REBUILD_CACHE=1 typedtree_parity.sh   # rebuild the oracle dump cache
#
# The oracle dumps are cached once under $CACHE (keyed by file path).  This makes
# the typeable denominator stable (no spurious 20s-timeout flakiness from
# 32-way oracle contention re-running each time) and makes diff runs fast (only
# c++type reruns).  Rebuild the cache after rebuilding ocamlc.opt.
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1

CPP=./cxx/build/c++type
JOBS="${JOBS:-$(nproc)}"
CACHE="${CACHE:-/tmp/ttp_oracle_cache}"
CACHE_TIMEOUT="${CACHE_TIMEOUT:-60}"  # generous; only paid once when caching

key() { printf '%s' "$1" | tr '/' '%'; }

# The -dtypedtree dump (like every -d* dump) goes to stderr and starts at a bare
# "[".  Strip any leading warning preamble by anchoring on ^\[.
oracle_raw() {
  timeout "$CACHE_TIMEOUT" ./ocamlc.opt -nostdlib -I stdlib -stop-after typing \
    -dtypedtree "$1" 2>&1 1>/dev/null | sed -n '/^\[/,$p'
}

# Stamp normalization: ident stamps print as name/NNN, both inside quotes
# ("x/274") and bare (type/module/constructor names: M/276, t/274).  Map each
# DISTINCT numeric stamp to a per-file sequential index by first appearance,
# preserving same-stamp<->same-id (so scope/shadowing bugs still show up) while
# neutralizing the global Stdlib-load offset.  Applied globally: location
# offsets have no "/NNN" form, and any "/NNN" in paths/string-constants is
# identical on both sides, so the rewrite is symmetric and harmless.
normalize() {
  perl -pe 'BEGIN{%m=();$n=0} s{/(\d+)}{ "/" . ($m{$1} //= ++$n) }ge'
}

# Cache-build worker: write one file's raw oracle dump to the cache.
if [ "${1:-}" == "--cache-one" ]; then
  oracle_raw "$2" > "$CACHE/$(key "$2")"
  exit 0
fi

# Diff/baseline worker: read the cached oracle dump, compare with c++type.
if [ "${1:-}" == "--worker" ]; then
  f="$2"
  o=$(cat "$CACHE/$(key "$f")" 2>/dev/null)
  if [ ! -x "$CPP" ]; then            # baseline mode: just oracle-typeable?
    [ -n "$o" ] && printf 'B 1\n' || printf 'B 0\n'
  else                                 # diff mode
    on=$(printf '%s' "$o" | normalize)
    # CPP_TIMEOUT guards against a c++type hang pinning a core during a sweep;
    # to debug a hang, run `c++type <file>` directly (no timeout) under gdb.
    c=$(timeout "${CPP_TIMEOUT:-10}" "$CPP" "$f" 2>/dev/null)
    cn=$(printf '%s' "$c" | normalize)
    if [ "$on" == "$cn" ]; then printf 'M\n'
    elif [ -z "$c" ] || [ "${c#TYPE_ERROR}" != "$c" ]; then
      if [ -z "$o" ]; then printf 'BOTHERR\n'; else printf 'CPPERR\n'; fi
    else printf 'DIFF\n'; fi
  fi
  exit 0
fi

LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")

# Build the oracle cache once (or when forced / missing).
if [ -n "${REBUILD_CACHE:-}" ] || [ ! -d "$CACHE" ]; then
  rm -rf "$CACHE"; mkdir -p "$CACHE"
  echo "building oracle cache ($CACHE) ..." >&2
  printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --cache-one {}
fi

PROG="${PROG:-/tmp/.ttp_progress}"
: > "$PROG"
printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} > "$PROG"
results=$(cat "$PROG")

if [ ! -x "$CPP" ]; then
  awk -v tot="${#files[@]}" '
    $1=="B"{ty+=$2}
    END{
      printf "corpus baseline (c++type not built yet)\n"
      printf "files: %d   oracle-typeable: %d (%.1f%%)\n", tot, ty, 100*ty/tot
    }' <<<"$results"
  exit 0
fi

awk -v tot="${#files[@]}" '
  $1=="M"{m++} $1=="DIFF"{d++} $1=="CPPERR"{ce++} $1=="BOTHERR"{be++}
  END{
    typeable=tot-be
    printf "files: %d   typedtree-identical: %d (%.1f%%)   (c++ errored on %d; %d of those the oracle also rejects)\n", \
           tot, m, 100*m/tot, ce+be, be
    printf "over oracle-typeable files (%d): %.1f%%\n", typeable, 100*m/typeable
  }' <<<"$results"
