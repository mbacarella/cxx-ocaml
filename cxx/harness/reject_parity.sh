#!/usr/bin/env bash
# Error-rejection parity (toward the full inferencer).  A correct type-checker
# never rejects valid code, so the key baseline is the FALSE-REJECTION rate: of
# the files the oracle ACCEPTS (non-empty cached -dtypedtree dump), how many does
# `c++type --check` wrongly reject?  Driving this to 0 == completing the engine.
#
# Usage: reject_parity.sh [N]      (JOBS=, CPP_TIMEOUT= overridable)
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
source cxx/harness/_require_fresh.sh; require_fresh c++type
CPP=./cxx/build/c++type
JOBS="${JOBS:-8}"
TIMEOUT="${CPP_TIMEOUT:-10}"
CACHE="${CACHE:-/tmp/ttp_oracle_cache}"
# Sibling-module cmis (multi-file tests): the oracle cache is built with this
# context, so c++type --check needs it too (cpptype_main derives the -I dir).
export CPPCAML_SIB_CMI_ROOT="${CPPCAML_SIB_CMI_ROOT:-/tmp/sib_cmi}"
key() { printf '%s' "$1" | tr '/' '%'; }

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  [ -s "$CACHE/$(key "$f")" ] || { printf 'SKIP\n'; exit 0; }  # oracle rejected/untypeable
  timeout "$TIMEOUT" "$CPP" --check "$f" >/dev/null 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then printf 'ACCEPT\n'
  elif [ $rc -eq 124 ]; then printf 'TIMEOUT\n'
  else printf 'REJECT %s\n' "$f"; fi   # false rejection (oracle accepted it)
  exit 0
fi

LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | awk '
  $1=="ACCEPT"{a++} $1=="REJECT"{r++} $1=="TIMEOUT"{t++} $1=="SKIP"{s++}
  END{
    acc=a+r+t
    printf "oracle-accepted files: %d   c++ accepts: %d   FALSE-REJECTS: %d   timeouts: %d\n", acc, a, r, t
    if (acc) printf "false-rejection rate: %.1f%%   (accept rate %.1f%%)\n", 100*r/acc, 100*a/acc
  }'
# Stash the false-rejected file list for bucketing.
printf '%s\n' "$res" | awk '$1=="REJECT"{print $2}' > /tmp/.reject_files
