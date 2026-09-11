#!/usr/bin/env bash
# RAW .cmi byte identity against ocamlc.opt, over the testsuite corpus.
#
# The exe-level gate `compare-bytecode-programs` reduces to this: a linked
# program's CRCS section carries the digest of the unit's OWN .cmi, so the
# remaining EXE_BYTES_DIFFER tests are blocked on the .cmi bytes, not on
# codegen (S431).  Three gaps stack up inside those bytes -- physical sharing,
# Subst type_expr ids and the import list -- so this reports BOTH the raw
# identity count and the VALUE-tree diff (mdump.pl --expand --norm-ids), which
# sees past sharing and ids to the structural content.
#
#   cmi_bytes.sh [N]        JOBS=, CPP=, V=1 (list every non-identical file)
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
CPP=${CPP:-$ROOT/cxx/build-release/c++ocamlc}
JOBS="${JOBS:-8}"
TIMEOUT="${CPP_TIMEOUT:-20}"

if [ "${1:-}" = "--worker" ]; then
  f="$2"
  td=$(mktemp -d) || exit 0
  mkdir -p "$td/r" "$td/o"
  b=$(basename "$f")
  cp "$f" "$td/r/$b"; cp "$f" "$td/o/$b"
  ( cd "$td/r" && timeout $TIMEOUT "$ROOT/ocamlc.opt" \
      -nostdlib -I "$ROOT/stdlib" -c "$b" ) >/dev/null 2>&1
  rc=$(ls "$td"/r/*.cmi 2>/dev/null | wc -l)
  if [ "$rc" -ne 1 ]; then rm -rf "$td"; echo "SKIP $f"; exit 0; fi
  ( cd "$td/o" && timeout $TIMEOUT "$CPP" \
      -nostdlib -I "$ROOT/stdlib" -c "$b" ) >/dev/null 2>&1
  oc=$(ls "$td"/o/*.cmi 2>/dev/null | wc -l)
  if [ "$oc" -ne 1 ]; then rm -rf "$td"; echo "CPPERR $f"; exit 0; fi
  R=$(ls "$td"/r/*.cmi); O=$(ls "$td"/o/*.cmi)
  if cmp -s "$O" "$R"; then rm -rf "$td"; echo "SAME $f"; exit 0; fi
  M="$ROOT/cxx/harness/mdump.pl"
  d=$(diff <(timeout 60 perl "$M" "$O" --expand --norm-ids 2>/dev/null) \
           <(timeout 60 perl "$M" "$R" --expand --norm-ids 2>/dev/null) \
       | grep -cE '^[<>]')
  rm -rf "$td"
  echo "DIFF $f $d"
  exit 0
fi

LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")
res=$(printf '%s\n' "${files[@]}" \
      | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | sort > /tmp/.cmi_bytes_results
[ "${V:-0}" = 1 ] &&
  awk '$1=="DIFF"{print $3"\t"$2}' /tmp/.cmi_bytes_results | sort -n
awk '$1=="SAME"{s++} $1=="DIFF"{d++; t+=$3} $1=="CPPERR"{e++} $1=="SKIP"{k++}
  END{printf "cmi judged: %d   BYTE-IDENTICAL: %d   DIFF: %d" \
             "   value-tree diff lines: %d   (cpp-fail %d, skip %d)\n",
             s+d, s, d, t, e, k}' /tmp/.cmi_bytes_results
