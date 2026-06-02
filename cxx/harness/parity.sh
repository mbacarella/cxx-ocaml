#!/usr/bin/env bash
# Lexer parity harness: diff c++lex against the trunk oracle token-for-token
# over a corpus of real .ml files, and report the match rate.
#
# Usage: cxx/harness/parity.sh [N]      # N = max files (default: all)
set -u
cd "$(dirname "$0")/../.." || exit 1

ORACLE=./cxx/oracle/dump_tokens
CPPLEX=./cxx/build/c++lex
LIMIT="${1:-0}"

mapfile -t files < <(find testsuite/tests -name '*.ml' 2>/dev/null | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")

total=0 match=0 differ=0 both_err=0 only_cpp_err=0 only_ora_err=0
mismatches=()

for f in "${files[@]}"; do
  total=$((total+1))
  o=$("$ORACLE" "$f" 2>/dev/null); orc=$?
  c=$("$CPPLEX" "$f" 2>/dev/null); crc=$?
  if [ "$o" == "$c" ]; then
    match=$((match+1))
    # classify clean-match vs both-errored-identically
    [[ "$o" == *$'\nERROR\t'* || "$o" == ERROR$'\t'* ]] && both_err=$((both_err+1))
  else
    differ=$((differ+1))
    [ "$crc" -ne 0 ] && [ "$orc" -eq 0 ] && only_cpp_err=$((only_cpp_err+1))
    [ "$orc" -ne 0 ] && [ "$crc" -eq 0 ] && only_ora_err=$((only_ora_err+1))
    [ "${#mismatches[@]}" -lt 12 ] && mismatches+=("$f")
  fi
done

echo "corpus files      : $total"
echo "token-identical   : $match  ($(awk "BEGIN{printf \"%.1f\", 100*$match/$total}")%)"
echo "  of which both-errored alike: $both_err"
echo "differing         : $differ"
echo "  c++ errored, oracle ok     : $only_cpp_err"
echo "  oracle errored, c++ ok     : $only_ora_err"
echo ""
echo "first mismatching files:"
printf '  %s\n' "${mismatches[@]}"
