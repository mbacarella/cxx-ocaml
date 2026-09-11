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

# Marshalled SIGNATURE length (the `data_len` word of the intext header at
# offset 16): the part of a .cmi the exe`s CRCS digest is taken over, and the
# only part physical sharing can shrink.  The file size would also count the
# import list, where the .ml path is short by design.
hdrlen() { perl -e 'open my $h,"<:raw",$ARGV[0] or exit; read $h,my $b,20;
                     print 20 + unpack("N", substr($b,16,4))' "$1"; }

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
  if cmp -s "$O" "$R"; then
    s=$(hdrlen "$R"); rm -rf "$td"; echo "SAME $f 0 $s $s"; exit 0
  fi
  M="$ROOT/cxx/harness/mdump.pl"
  d=$(diff <(timeout 60 perl "$M" "$O" --expand --norm-ids 2>/dev/null) \
           <(timeout 60 perl "$M" "$R" --expand --norm-ids 2>/dev/null) \
       | grep -cE '^[<>]')
  os=$(hdrlen "$O"); rs=$(hdrlen "$R")
  rm -rf "$td"
  echo "DIFF $f $d $os $rs"
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
  $1=="SAME"||$1=="DIFF"{ob+=$4; rb+=$5; ad+=($4>$5?$4-$5:$5-$4)}
  END{printf "cmi judged: %d   BYTE-IDENTICAL: %d   DIFF: %d" \
             "   value-tree diff lines: %d   (cpp-fail %d, skip %d)\n",
             s+d, s, d, t, e, k;
      printf "cmi SIGNATURE BYTES: ours %d   ref %d   excess %+d (%+.1f%%)" \
             "   |per-file gap| %d\n",
             ob, rb, ob-rb, rb ? (ob-rb)*100.0/rb : 0, ad}' \
    /tmp/.cmi_bytes_results
