#!/usr/bin/env bash
# Lambda parity: diff c++lambda against `ocamlc -dlambda` over the corpus, with
# stamps normalized by first appearance (same convention as the typedtree
# harness).  This is the next stage's dump-diff loop.  Run inside the nix dev
# shell (needs ./ocamlc.opt).
#
# Usage: lambda_parity.sh [N]      (JOBS=, CPP_TIMEOUT=, CACHE= overridable)
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
CPP=./cxx/build/c++lambda
JOBS="${JOBS:-4}"
TIMEOUT="${CPP_TIMEOUT:-10}"
CACHE="${CACHE:-/tmp/lambda_oracle_cache}"
mkdir -p "$CACHE"
key() { printf '%s' "$1" | tr '/' '%'; }
# normalize: /NNN stamps AND static-exception numbers (exit N / with (N) by first
# appearance -- both are arbitrary module-global ids (next_raise_count), so like
# stamps they are normalized rather than matched raw.  Trim trailing whitespace.
norm() { perl -0777 -pe 'BEGIN{%m=();$n=0;%e=();$en=0}
  s{/(\d+)}{ "/" . ($m{$1} //= ++$n) }ge;
  s{\bexit (\d+)}{ "exit " . ($e{$1} //= ++$en) }ge;
  s{\bwith \((\d+)}{ "with (" . ($e{$1} //= ++$en) }ge;
  s/\s+\z//'; }

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  ck="$CACHE/$(key "$f")"
  if [ ! -f "$ck" ]; then
    timeout "$TIMEOUT" ./ocamlc.opt -nostdlib -I stdlib -dlambda -stop-after lambda \
      -c "$f" 2>&1 1>/dev/null | sed -n '/^(/,$p' > "$ck" 2>/dev/null || true
  fi
  [ -s "$ck" ] || { printf 'SKIP\n'; exit 0; }   # oracle produced no lambda
  o=$(norm < "$ck")
  m=$(timeout "$TIMEOUT" "$CPP" "$f" 2>/dev/null | norm)
  rc=$?
  if [ $rc -eq 124 ]; then printf 'TIMEOUT\n'
  elif printf '%s' "$m" | grep -q 'TYPE_ERROR'; then printf 'CPPERR\n'
  elif [ "$o" = "$m" ]; then printf 'M\n'
  else printf 'DIFF %s\n' "$f"; fi
  exit 0
fi

LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | awk '
  $1=="M"{m++} $1=="DIFF"{d++} $1=="CPPERR"{e++} $1=="TIMEOUT"{t++} $1=="SKIP"{s++}
  END{
    judged=m+d
    printf "lambda-producing files: %d   MATCH: %d   DIFF: %d   (cpp-err %d, timeout %d, skip %d)\n", judged, m, d, e, t, s
    if (judged) printf "parity: %.1f%%\n", 100*m/judged
  }'
printf '%s\n' "$res" | awk '$1=="DIFF"{print $2}' > /tmp/.lambda_diff_files
