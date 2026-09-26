#!/usr/bin/env bash
# False-accept battery: small ill-typed programs (cxx/harness/false_accept/,
# every one rejected by ocamlc with a type error).  The testsuite accept gate
# (accept_parity.sh) cannot see this class -- its ill-typed files exercise
# advanced features, while the checker let plain argument/let-bound mismatches
# through.  Counts the files `c++type --check` wrongly ACCEPTS.
#
# Usage: false_accept.sh        (CPP=, JOBS=, CPP_TIMEOUT= overridable; HOOKENV=
#                                prefixes the checker, e.g. "env NOSHARE635=1")
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
CPP="${CPP:-./cxx/build/c++type}"
JOBS="${JOBS:-8}"
TIMEOUT="${CPP_TIMEOUT:-10}"
DIR=cxx/harness/false_accept

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  ${HOOKENV:-} timeout "$TIMEOUT" "$CPP" --check "$f" >/dev/null 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then printf 'ACCEPT %s\n' "$f"
  elif [ $rc -eq 124 ]; then printf 'TIMEOUT %s\n' "$f"
  else printf 'REJECT %s\n' "$f"; fi
  exit 0
fi

res=$(ls $DIR/*.ml | sort | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.false_accept_results
printf '%s\n' "$res" | awk '
  $1=="ACCEPT"{a++} $1=="REJECT"{r++} $1=="TIMEOUT"{t++}
  END{ printf "ill-typed programs: %d   FALSE-ACCEPTS: %d   rejected: %d   timeouts: %d\n", a+r+t, a, r, t }'
echo "per-file results: /tmp/.false_accept_results"
