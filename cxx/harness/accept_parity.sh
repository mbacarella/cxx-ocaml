#!/usr/bin/env bash
# Soundness parity (the inverse of reject_parity.sh).  A correct type-checker
# rejects invalid code, so the key soundness baseline is the FALSE-ACCEPTANCE
# rate: of the files the oracle REJECTS (empty cached -dtypedtree dump), how many
# does `c++type --check` wrongly ACCEPT?  Driving this to 0 == soundness.
#
# Files our own parser cannot handle are excluded (a parser gap, not a soundness
# gap), so this measures the type-checker, not the front end.
#
# Usage: accept_parity.sh [N]      (JOBS=, CPP_TIMEOUT= overridable)
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
source cxx/harness/_require_fresh.sh; require_fresh c++type
CPP=./cxx/build/c++type
JOBS="${JOBS:-4}"
TIMEOUT="${CPP_TIMEOUT:-10}"
CACHE="${CACHE:-/tmp/ttp_oracle_cache}"
# Sibling-module cmis (multi-file tests): the oracle cache is built with this
# context, so c++type --check needs it too (cpptype_main derives the -I dir).
export CPPCAML_SIB_CMI_ROOT="${CPPCAML_SIB_CMI_ROOT:-/tmp/sib_cmi}"
key() { printf '%s' "$1" | tr '/' '%'; }

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  [ -s "$CACHE/$(key "$f")" ] && { printf 'SKIP\n'; exit 0; }  # oracle accepted it
  # expect-tests are toplevel phrase/[%%expect] tests: `ocamlc -dtypedtree` rejects
  # them for the expect extension, not a single-file type error -- out of scope.
  grep -q '%%expect' "$f" && { printf 'SKIP\n'; exit 0; }
  out=$(timeout "$TIMEOUT" "$CPP" --check "$f" 2>/dev/null)
  rc=$?
  if [ $rc -eq 124 ]; then printf 'TIMEOUT\n'
  elif printf '%s' "$out" | grep -q 'TYPE_ERROR'; then printf 'PARSEFAIL\n'  # our parser gap
  elif [ $rc -eq 0 ]; then printf 'ACCEPT %s\n' "$f"   # FALSE acceptance (oracle rejected)
  else printf 'REJECT\n'; fi                            # correct co-rejection
  exit 0
fi

LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | awk '
  $1=="ACCEPT"{a++} $1=="REJECT"{r++} $1=="PARSEFAIL"{p++} $1=="TIMEOUT"{t++} $1=="SKIP"{s++}
  END{
    judged=a+r
    printf "oracle-rejected & we-parse: %d   FALSE-ACCEPTS: %d   co-rejects: %d   (parser-gap: %d, timeout: %d)\n", judged, a, r, p, t
    if (judged) printf "false-acceptance rate: %.1f%%   (correct-rejection rate %.1f%%)\n", 100*a/judged, 100*r/judged
  }'
# Stash the false-accepted file list for bucketing.
printf '%s\n' "$res" | awk '$1=="ACCEPT"{print $2}' > /tmp/.accept_files
