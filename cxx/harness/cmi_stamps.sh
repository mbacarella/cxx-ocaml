#!/usr/bin/env bash
# Does a .cmi's first Ident stamp land where ocamlc's does?  Scored over the
# testsuite corpus.
#
# A saved signature's stamps are FRESH: Env.save_signature runs
# Subst.reset_for_saving and rename_bound_idents then renames every bound ident
# in signature order (subst.ml:594), so the stamps are contiguous and the whole
# question is their BASE -- `274 + <the idents TYPING allocated>`, since
# currentstamp is 273 once the initial environment is built.  Everything else in
# a .cmi can be right and the file still differ in every ident, so this is its
# own gate; cmi_bytes.sh scores the bytes.
#
#   cmi_stamps.sh [N]       JOBS=, CPP=
#                           V=1 lists every mismatch with its delta
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
  if [ "$(ls "$td"/r/*.cmi 2>/dev/null | wc -l)" -ne 1 ]; then
    rm -rf "$td"; echo "SKIP $f"; exit 0
  fi
  ( cd "$td/o" && timeout $TIMEOUT "$CPP" \
      -nostdlib -I "$ROOT/stdlib" -c "$b" ) >/dev/null 2>&1
  if [ "$(ls "$td"/o/*.cmi 2>/dev/null | wc -l)" -ne 1 ]; then
    rm -rf "$td"; echo "CPPERR $f"; exit 0
  fi
  # The first signature item's Ident stamp: in the marshalled (name, sign) pair
  # that is the second string of the dump followed by its stamp.
  base() {
    timeout 60 perl "$ROOT/cxx/harness/mdump.pl" "$1" 2>/dev/null |
      perl -ne 's/^\s+//; if (/^"/) { $k++; next }
                print and exit if $k >= 2 && /^-?\d+$/'
  }
  rb=$(base "$(ls "$td"/r/*.cmi)"); ob=$(base "$(ls "$td"/o/*.cmi)")
  rm -rf "$td"
  if [ -z "$rb" ] || [ -z "$ob" ]; then echo "EMPTY $f"; exit 0; fi
  if [ "$rb" = "$ob" ]; then echo "OK $f $rb $ob"
  else echo "DIFF $f $rb $ob"; fi
  exit 0
fi

LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")
res=$(printf '%s\n' "${files[@]}" \
      | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | sort > /tmp/.cmi_stamps_results
[ "${V:-0}" = 1 ] &&
  awk '$1=="DIFF"{printf "%+d\t%s\t(ref %s ours %s)\n", $4-$3, $2, $3, $4}' \
      /tmp/.cmi_stamps_results | sort -n
awk '$1=="OK"{k++} $1=="DIFF"{d++; s=$4-$3; h[s]++}
  $1=="CPPERR"{e++} $1=="SKIP"{p++} $1=="EMPTY"{m++}
  END{printf "cmi stamp base: judged %d   MATCH %d (%.1f%%)   DIFF %d" \
             "   (cpp-fail %d, skip %d, empty-sig %d)\n",
             k+d, k, (k+d) ? k*100.0/(k+d) : 0, d, e, p, m;
      n=0; for (s in h) n++;
      printf "  %d distinct deltas; commonest:", n;
      for (i=0;i<6;i++) { bs=""; bc=0;
        for (s in h) if (h[s]>bc) { bc=h[s]; bs=s }
        if (bc==0) break; printf "  %+d x%d", bs, bc; delete h[bs] }
      printf "\n"}' /tmp/.cmi_stamps_results
