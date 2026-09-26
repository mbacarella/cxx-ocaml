#!/usr/bin/env bash
# False-reject brake over a directory of ocamlc-VALID programs (default: the
# stamp probes).  Counts files `c++type --check` wrongly REJECTS; a file the
# oracle rejects too (an invalid probe) is SKIPped.
#
# Usage: valid_reject.sh        (CPP=, JOBS=, CPP_TIMEOUT= overridable; HOOKENV=
#                                prefixes the checker, e.g. "env NOSHARE635=1")
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
CPP="${CPP:-./cxx/build/c++type}"
JOBS="${JOBS:-8}"
TIMEOUT="${CPP_TIMEOUT:-10}"
DIR="${DIR:-cxx/harness/stamp_probes}"

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  ${HOOKENV:-} timeout "$TIMEOUT" "$CPP" --check "$f" >/dev/null 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then printf 'ACCEPT %s\n' "$f"
  elif [ $rc -eq 124 ]; then printf 'TIMEOUT %s\n' "$f"
  else
    w=$(mktemp -d); cp "$f" "$w/"
    if (cd "$w" && "$OLDPWD/ocamlc.opt" -nostdlib -I "$OLDPWD/stdlib" -w -a -c "$(basename "$f")" >/dev/null 2>&1)
    then printf 'REJECT %s\n' "$f"; else printf 'SKIP %s\n' "$f"; fi
    rm -rf "$w"
  fi
  exit 0
fi

res=$(ls $DIR/*.ml | sort | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.valid_reject_results
printf '%s\n' "$res" | awk '
  $1=="ACCEPT"{a++} $1=="REJECT"{r++} $1=="TIMEOUT"{t++} $1=="SKIP"{s++}
  END{ printf "valid programs: %d   accepted: %d   FALSE-REJECTS: %d   timeouts: %d   (oracle-invalid skipped: %d)\n", a+r+t, a, r, t, s }'
echo "per-file results: /tmp/.valid_reject_results"
