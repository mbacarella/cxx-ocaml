#!/usr/bin/env bash
# Stage-8 oracle (cxx/PORTING.md): the .cmi the typing/ port writes
# (Env.save_signature) against ocamlc's.  Every .mli of the compiler corpus,
# and every .ml without one, is compiled by c++ocamlc in a directory holding
# ocamlc's .cmi of every other unit (the effid.sh staging, /tmp/effid_ref),
# with effid.sh's flags; its .cmi is compared with ocamlc's by the cmi dump
# with ident stamps / type ids renumbered (`c++typing-dump cmic`: the Types
# graph, sharing included) and byte for byte.
#
# Usage: cmi_port_parity.sh [unit.mli|unit.ml ...]   (JOBS=, REF= overridable)
#        cmi_port_parity.sh --standalone [file.ml ...]
#          single .ml files typed against the stdlib alone, ocamlc writing the
#          reference .cmi next to it (default: cxx/harness/stamp_probes)
#   SAME-BYTES / SAME-GRAPH (graph equal, bytes differ) / DIFF / CFAIL
#   (/ OFAIL: ocamlc rejects the file too)
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
JOBS="${JOBS:-8}"
REF="${REF:-/tmp/effid_ref}"
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
DUMP="$ROOT/cxx/build-release/c++typing-dump"
FLAGS="-strict-sequence -strict-formats -w +a-4-9-40-41-42-44-45-48-70"
OUT=/tmp/cmi_port_parity

if [ "${1:-}" == "--worker" ]; then
  f="$2"; base="${f%.*}"
  w=$(mktemp -d)
  for x in "$REF"/*.cmi; do
    [ "$(basename "$x")" = "$base.cmi" ] || ln -s "$x" "$w/"
  done
  cp "$REF/$f" "$w/"
  ( cd "$w" && CPPCAML_TYPECHECK=1 CPPCAML_TYPECHECK_DEBUG=1 timeout 120 "$CPP" -I "$ROOT/stdlib" $FLAGS \
      -stop-after typing -c "$f" ) > "$OUT/$f.log" 2>&1
  # an .ml is only typed with -stop-after typing, and its .cmi is still written
  if [ "${f##*.}" = mli ]; then
    ( cd "$w" && CPPCAML_TYPECHECK=1 timeout 120 "$CPP" -I "$ROOT/stdlib" $FLAGS -c "$f" ) >> "$OUT/$f.log" 2>&1
  fi
  if [ ! -f "$w/$base.cmi" ] || grep -q "type checker" "$OUT/$f.log"; then
    printf 'CFAIL %s\n' "$f"
  else
    "$DUMP" cmic "$REF/$base.cmi" > "$OUT/$f.o" 2>&1
    "$DUMP" cmic "$w/$base.cmi" > "$OUT/$f.c" 2>&1
    # the self CRC (the first digest of the crcs) follows the bytes: mask it
    mask() { sed -E '/^crcs/ s/[0-9a-f]{32}/SELF/' "$1"; }
    if cmp -s "$REF/$base.cmi" "$w/$base.cmi"; then printf 'SAME-BYTES %s\n' "$f"
    elif cmp -s <(mask "$OUT/$f.o") <(mask "$OUT/$f.c"); then printf 'SAME-GRAPH %s\n' "$f"
    else printf 'DIFF %s\n' "$f"; fi
  fi
  rm -rf "${w:?}"
  exit 0
fi

if [ "${1:-}" == "--sa-worker" ]; then
  f="$2"; b=$(basename "$f"); base="${b%.*}"
  w=$(mktemp -d); mkdir "$w/o" "$w/c"; cp "$f" "$w/o/"; cp "$f" "$w/c/"
  key=$(echo "$f" | tr '/' '_')
  ( cd "$w/o" && timeout 120 "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -w -a -stop-after typing -c "$b" ) >/dev/null 2>&1
  ( cd "$w/c" && CPPCAML_TYPECHECK_DEBUG=1 timeout 120 "$CPP" -I "$ROOT/stdlib" -w -a -stop-after typing -c "$b" ) > "$OUT/$key.log" 2>&1
  if [ ! -f "$w/o/$base.cmi" ]; then printf 'OFAIL %s\n' "$f"
  elif [ ! -f "$w/c/$base.cmi" ] || grep -q "type checker" "$OUT/$key.log"; then printf 'CFAIL %s\n' "$f"
  elif cmp -s "$w/o/$base.cmi" "$w/c/$base.cmi"; then printf 'SAME-BYTES %s\n' "$f"
  else
    mask() { sed -E '/^crcs/ s/[0-9a-f]{32}/SELF/' "$1"; }
    "$DUMP" cmic "$w/o/$base.cmi" > "$OUT/$key.o" 2>&1
    "$DUMP" cmic "$w/c/$base.cmi" > "$OUT/$key.c" 2>&1
    if cmp -s <(mask "$OUT/$key.o") <(mask "$OUT/$key.c"); then printf 'SAME-GRAPH %s\n' "$f"
    else printf 'DIFF %s\n' "$f"; fi
  fi
  rm -rf "${w:?}"
  exit 0
fi

if [ "${1:-}" == "--standalone" ]; then
  shift
  rm -rf "$OUT"; mkdir -p "$OUT"
  if [ $# -gt 0 ]; then files=("$@"); else mapfile -t files < <(ls cxx/harness/stamp_probes/*.ml); fi
  ulimit -v 8000000
  res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --sa-worker {} | sort -k2)
  printf '%s\n' "$res" > /tmp/.cmi_port_parity_results
  printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "files %d: SAME-BYTES %d  SAME-GRAPH %d  DIFF %d  CFAIL %d  OFAIL %d\n", NR, f["SAME-BYTES"], f["SAME-GRAPH"], f["DIFF"], f["CFAIL"], f["OFAIL"] }'
  echo "per-file results: /tmp/.cmi_port_parity_results (dumps in $OUT)"
  exit 0
fi

[ -f "$REF/main.cmo" ] || { echo "no $REF (run cxx/harness/effid.sh first)" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
if [ $# -gt 0 ]; then files=("$@")
else mapfile -t files < <(cd "$REF" && { ls *.mli; for m in *.ml; do [ -f "${m}i" ] || echo "$m"; done; } | sort)
fi
ulimit -v 8000000
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {} | sort -k2)
printf '%s\n' "$res" > /tmp/.cmi_port_parity_results
printf '%s\n' "$res" | awk '{f[$1]++} END{ printf "units %d: SAME-BYTES %d  SAME-GRAPH %d  DIFF %d  CFAIL %d\n", NR, f["SAME-BYTES"], f["SAME-GRAPH"], f["DIFF"], f["CFAIL"] }'
echo "per-file results: /tmp/.cmi_port_parity_results (dumps in $OUT)"
