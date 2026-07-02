#!/usr/bin/env bash
# Slice-3 diff categorizer.  For each oracle-typeable corpus file that c++type
# ALSO types (no error), diff the normalized typed-tree dumps and, for the
# mismatches, record: filepath + the first differing line pair.  Writes
# per-file diffs under $OUT and prints a bucketed summary.
#
# Reuses the same normalization and oracle cache as typedtree_parity.sh.
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1

CPP=./cxx/build/c++type
JOBS="${JOBS:-$(nproc)}"
CACHE="${CACHE:-/tmp/ttp_oracle_cache}"
OUT="${OUT:-/tmp/ttp_diffs}"
key() { printf '%s' "$1" | tr '/' '%'; }

normalize() {
  perl -0777 -pe 'BEGIN{%m=();$n=0} s{/(\d+)}{ "/" . ($m{$1} //= ++$n) }ge; s/\s+\z//'
}

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  ocache="$CACHE/$(key "$f")"
  [ -s "$ocache" ] || exit 0            # not oracle-typeable
  rawc=$(mktemp)
  timeout "${CPP_TIMEOUT:-20}" "$CPP" "$f" >"$rawc" 2>/dev/null
  { [ ! -s "$rawc" ] || [ "$(head -c 10 "$rawc")" = "TYPE_ERROR" ]; } && { rm -f "$rawc"; exit 0; }
  no=$(mktemp); nc=$(mktemp)
  normalize < "$ocache" > "$no"
  normalize < "$rawc" > "$nc"
  if ! cmp -s "$no" "$nc"; then
    d="$OUT/$(key "$f").diff"
    diff "$no" "$nc" > "$d"
    # First differing oracle-side line (the '<' side), trimmed, as the bucket key.
    firstl=$(grep -m1 '^< ' "$d" | sed 's/^< *//; s/[0-9][0-9]*//g; s/  */ /g; s/^ *//; s/ *$//')
    printf '%s\t%s\n' "$firstl" "$f"
  fi
  rm -f "$rawc" "$no" "$nc"
  exit 0
fi

rm -rf "$OUT"; mkdir -p "$OUT"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} > "$OUT/index.tsv"

echo "=== total DIFF files (oracle-typeable, c++ also types): $(wc -l < "$OUT/index.tsv") ==="
echo "=== top first-divergence buckets ==="
cut -f1 "$OUT/index.tsv" | sort | uniq -c | sort -rn | head -40
