#!/usr/bin/env bash
# Soundness + completeness over expect-tests, which are self-contained (no build
# context) and carry their own oracle verdict: a `[%%expect{|...|}]` block whose
# text contains "Error" marks a deliberately-INVALID file; otherwise the phrases
# are all VALID.  We strip the expect blocks and the TEST header, leaving the
# concatenated phrases, and run `c++type --check`:
#   - invalid file we ACCEPT  -> FALSE-ACCEPT (a soundness gap)
#   - valid   file we REJECT  -> FALSE-REJECT (a completeness gap)
# Files our parser can't handle are excluded (front-end gap, not type-checker).
#
# Usage: expect_soundness.sh [N]    (JOBS=, CPP_TIMEOUT= overridable)
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
CPP=./cxx/build/c++type
JOBS="${JOBS:-4}"
TIMEOUT="${CPP_TIMEOUT:-10}"

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  grep -q '%%expect' "$f" || { printf 'SKIP\n'; exit 0; }     # not an expect-test
  # Does any expect block announce an error?  -> the file is invalid.
  blocks=$(perl -0777 -ne 'print "$1\n" while /\[%%expect[^{]*\{\|(.*?)\|\}\s*\]/sg' "$f")
  invalid=0; printf '%s' "$blocks" | grep -Eq 'Error|Exception|Warning [0-9]+ \[.*\]:.*fatal' && invalid=1
  # Strip the TEST header and all expect blocks, leaving the phrases.
  tmp=$(mktemp /tmp/exsnd.XXXXXX.ml)
  perl -0777 -pe 's/\(\*\s*TEST.*?\*\)//s; s/\[%%expect[^{]*\{\|.*?\|\}\s*\]//sg' "$f" > "$tmp"
  out=$(timeout "$TIMEOUT" "$CPP" --check "$tmp" 2>/dev/null); rc=$?
  rm -f "$tmp"
  if [ $rc -eq 124 ]; then printf 'TIMEOUT\n'; exit 0; fi
  printf '%s' "$out" | grep -q 'TYPE_ERROR' && { printf 'PARSEFAIL\n'; exit 0; }
  if [ "$invalid" -eq 1 ]; then
    [ $rc -eq 0 ] && printf 'FACCEPT %s\n' "$f" || printf 'COREJECT\n'
  else
    [ $rc -eq 0 ] && printf 'OK\n' || printf 'FREJECT %s\n' "$f"
  fi
  exit 0
fi

LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | awk '
  $1=="FACCEPT"{fa++} $1=="COREJECT"{cr++} $1=="OK"{ok++} $1=="FREJECT"{fr++}
  $1=="PARSEFAIL"{p++} $1=="TIMEOUT"{t++}
  END{
    inv=fa+cr; val=ok+fr;
    printf "INVALID expect-tests we parse: %d   false-accepts: %d   co-rejects: %d\n", inv, fa, cr
    if (inv) printf "  false-acceptance (soundness): %.1f%%   (correct-rejection %.1f%%)\n", 100*fa/inv, 100*cr/inv
    printf "VALID   expect-tests we parse: %d   accepts: %d   false-rejects: %d\n", val, ok, fr
    if (val) printf "  false-rejection (completeness): %.1f%%   (accept %.1f%%)\n", 100*fr/val, 100*ok/val
    printf "(parser-gap: %d, timeout: %d)\n", p, t
  }'
printf '%s\n' "$res" | awk '$1=="FACCEPT"{print $2}' > /tmp/.faccept_files
printf '%s\n' "$res" | awk '$1=="FREJECT"{print $2}' > /tmp/.freject_files
