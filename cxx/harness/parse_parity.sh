#!/usr/bin/env bash
# Parser parity: diff c++parse against `ocamlc -dparsetree` over the corpus.
# Needs the built tree (./ocamlc.opt). Usage: parse_parity.sh [N]
set -u
cd "$(dirname "$0")/../.." || exit 1

CPP=./cxx/build/c++parse
LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")

# Strip any warning preamble (e.g. bad-module-name on odd filenames) the oracle
# prints to stderr before the dump; the parsetree always starts at a bare "[".
# The parsetree dump starts at a line beginning with `[` — either `[` (non-empty
# structure) or `[]` (empty structure).  Anchor on `^\[` to keep both.
oracle() { ./ocamlc.opt -nostdlib -I stdlib -stop-after parsing -dparsetree "$1" 2>&1 1>/dev/null | sed -n '/^\[/,$p'; }

total=0 match=0 cpp_err=0
both_err=0  # files the oracle ALSO rejects (no parsetree) — correctly unparseable
for f in "${files[@]}"; do
  total=$((total + 1))
  o=$(oracle "$f")
  c=$("$CPP" "$f" 2>/dev/null)
  if [ "$o" == "$c" ]; then match=$((match + 1));
  else
    case "$c" in PARSE_ERROR*)
      cpp_err=$((cpp_err + 1))
      [ -z "$o" ] && both_err=$((both_err + 1))  # oracle produced no dump either
    ;; esac
  fi
done
parseable=$((total - both_err))
echo "files: $total   parsetree-identical: $match ($(awk "BEGIN{printf \"%.1f\",100*$match/$total}")%)   (c++ parse-errored on $cpp_err; $both_err of those the oracle also rejects)"
echo "over oracle-parseable files ($parseable): $(awk "BEGIN{printf \"%.1f\",100*$match/$parseable}")%"
