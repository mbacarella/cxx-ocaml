#!/usr/bin/env bash
# Accept/reject parity of the ported type checker with ocamlc (cxx/PORTING.md
# stage 6): each .ml is checked by `c++ocamlc -stop-after typing` and by
# ocamlc.opt, in a scratch copy.  Reports SAME-OK / SAME-ERR (with the error
# location compared) / FALSE-REJECT / FALSE-ACCEPT, and the files where the
# port hit an unported part or an internal failure (they are accepted).
#
# Usage: port_parity.sh [file.ml ...]   (JOBS= overridable)
#   no args: cxx/harness/stamp_probes, cxx/harness/false_accept, stdlib
set -u
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  w=$(mktemp -d); cp "$f" "$w/"; b=$(basename "$f")
  c=$(cd "$w" && CPPCAML_TYPECHECK_DEBUG=1 timeout 60 "$ROOT/cxx/build-release/c++ocamlc" -I "$ROOT/stdlib" -w -a \
        -stop-after typing -c "$b" 2>&1); crc=$?
  o=$(cd "$w" && "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -w -a -stop-after typing -c "$b" 2>&1); orc=$?
  rm -rf "$w"
  tag=""
  if printf '%s' "$c" | grep -q 'type checker: unported'; then tag=" UNPORTED"
  elif printf '%s' "$c" | grep -q 'type checker failed'; then tag=" INTERNAL"; fi
  if [ $crc -eq 124 ]; then printf 'TIMEOUT %s\n' "$f"
  elif [ $crc -eq 0 ] && [ $orc -eq 0 ]; then printf 'SAME-OK %s%s\n' "$f" "$tag"
  elif [ $crc -ne 0 ] && [ $orc -ne 0 ]; then
    if [ "$(printf '%s' "$c" | grep -m1 '^File')" == "$(printf '%s' "$o" | grep -m1 '^File')" ]
    then printf 'SAME-ERR %s\n' "$f"; else printf 'SAME-ERR-LOC %s\n' "$f"; fi
  elif [ $crc -ne 0 ]; then printf 'FALSE-REJECT %s\n' "$f"
  else printf 'FALSE-ACCEPT %s%s\n' "$f" "$tag"; fi
  exit 0
fi

if [ $# -gt 0 ]; then files=("$@")
else lines_into files < <(ls cxx/harness/stamp_probes/*.ml cxx/harness/false_accept/*.ml stdlib/*.ml | sort -u)
fi
vm_limit 8000000
res=$(printf '%s\n' ${files[@]+"${files[@]}"} | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.port_parity_results
printf '%s\n' "$res" | awk '
  {f[$1]++} / UNPORTED/{u++} / INTERNAL/{i++}
  END{ printf "files %d: SAME-OK %d  SAME-ERR %d (location differs %d)  FALSE-REJECT %d  FALSE-ACCEPT %d  TIMEOUT %d  |  accepted via unported part %d, internal failure %d\n",
       NR, f["SAME-OK"], f["SAME-ERR"]+f["SAME-ERR-LOC"], f["SAME-ERR-LOC"], f["FALSE-REJECT"], f["FALSE-ACCEPT"], f["TIMEOUT"], u, i }'
echo "per-file results: /tmp/.port_parity_results"
