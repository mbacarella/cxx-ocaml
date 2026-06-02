#!/usr/bin/env bash
# Parser parity: diff c++parse against `ocamlc -dparsetree` over the corpus.
# Run inside the nix dev shell (needs ./ocamlc.opt). Usage: parse_parity.sh [N]
set -u
cd "$(dirname "$0")/../.." || exit 1

CPP=./cxx/build/c++parse
LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")

oracle() { ./ocamlc.opt -nostdlib -I stdlib -stop-after parsing -dparsetree "$1" 2>&1 1>/dev/null; }

total=0 match=0 cpp_err=0
for f in "${files[@]}"; do
  total=$((total + 1))
  o=$(oracle "$f")
  c=$("$CPP" "$f" 2>/dev/null)
  if [ "$o" == "$c" ]; then match=$((match + 1));
  else case "$c" in PARSE_ERROR*) cpp_err=$((cpp_err + 1));; esac
  fi
done
echo "files: $total   parsetree-identical: $match ($(awk "BEGIN{printf \"%.1f\",100*$match/$total}")%)   (c++ parse-errored on $cpp_err)"
