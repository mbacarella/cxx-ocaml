#!/usr/bin/env bash
# Lexer parity harness: diff c++lex against the trunk oracle token-for-token
# over a corpus of real .ml files, and report the match rate.
#
# Usage: cxx/harness/parity.sh [N]      # N = max files (default: all)
set -u
cd "$(dirname "$0")/../.." || exit 1
. cxx/harness/portable.sh

ORACLE=./cxx/build/dump_tokens
# the oracle: cxx/oracle/dump_tokens.ml on the tree's compiler-libs
if [ ! -x "$ORACLE" ] || [ cxx/oracle/dump_tokens.ml -nt "$ORACLE" ]; then
  T=$(mktemp -d)
  cp cxx/oracle/dump_tokens.ml "$T/"
  ( cd "$T" && "$OLDPWD/ocamlopt.opt" -nostdlib -I "$OLDPWD/stdlib" -I "$OLDPWD/compilerlibs" \
      -I "$OLDPWD/parsing" -I "$OLDPWD/utils" "$OLDPWD/compilerlibs/ocamlcommon.cmxa" \
      dump_tokens.ml -o dump_tokens ) || { echo "parity.sh: building the oracle failed" >&2; exit 2; }
  mkdir -p cxx/build && mv "$T/dump_tokens" "$ORACLE" && rm -rf "${T:?}"
fi
CPPLEX=./cxx/build/c++lex
LIMIT="${1:-0}"

lines_into files < <(find testsuite/tests -name '*.ml' 2>/dev/null | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")

total=0 match=0 differ=0 both_err=0 only_cpp_err=0 only_ora_err=0
mismatches=()

for f in ${files[@]+"${files[@]}"}; do
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
echo "token-identical   : $match  ($(pct "$match" "$total")%)"
echo "  of which both-errored alike: $both_err"
echo "differing         : $differ"
echo "  c++ errored, oracle ok     : $only_cpp_err"
echo "  oracle errored, c++ ok     : $only_ora_err"
echo ""
echo "first mismatching files:"
printf '  %s\n' ${mismatches[@]+"${mismatches[@]}"}
