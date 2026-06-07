#!/usr/bin/env bash
# Bytecode-instruction parity: diff c++instr against `ocamlc -dinstr -c` over the
# corpus, with labels (L<n>) normalized by first appearance.  The oracle writes a
# .cmo as a side effect, so each file is compiled in a throwaway temp dir to keep
# the testsuite clean.  Run inside the nix dev shell (needs ./ocamlc.opt).
#
# Usage: instr_parity.sh [N]      (JOBS=, CPP_TIMEOUT=, CACHE= overridable)
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
CPP=./cxx/build/c++instr
JOBS="${JOBS:-4}"
TIMEOUT="${CPP_TIMEOUT:-10}"
CACHE="${CACHE:-/tmp/instr_oracle_cache}"
mkdir -p "$CACHE"
key() { printf '%s' "$1" | tr '/' '%'; }
# normalize: map each distinct L<n> to a sequential id by first appearance; trim.
norm() { perl -0777 -pe 'BEGIN{%m=();$n=0} s{L(\d+)}{ "L" . ($m{$1} //= ++$n) }ge; s/\s+\z//'; }

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  ck="$CACHE/$(key "$f")"
  if [ ! -f "$ck" ]; then
    td=$(mktemp -d)
    cp "$f" "$td/$(basename "$f")"
    timeout "$TIMEOUT" ./ocamlc.opt -nostdlib -I stdlib -dinstr -c "$td/$(basename "$f")" \
      2>"$ck" 1>/dev/null || true
    rm -rf "$td"
    # keep only the instruction lines (start with a tab or a label)
    grep -E '^(	|L[0-9])' "$ck" > "$ck.f" 2>/dev/null; mv -f "$ck.f" "$ck" 2>/dev/null || true
  fi
  [ -s "$ck" ] || { printf 'SKIP\n'; exit 0; }   # oracle produced no instructions
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
    printf "instr-producing files: %d   MATCH: %d   DIFF: %d   (cpp-err %d, timeout %d, skip %d)\n", judged, m, d, e, t, s
    if (judged) printf "parity: %.1f%%\n", 100*m/judged
  }'
printf '%s\n' "$res" | awk '$1=="DIFF"{print $2}' > /tmp/.instr_diff_files
