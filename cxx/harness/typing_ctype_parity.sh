#!/usr/bin/env bash
# Stage-3 oracle for the typing/ port (TYPECHECKER.md): run Ctype operations
# on the types of every value and type constructor of a set of cmis, in
# Env.initial + open Stdlib, with compiler-libs (typing_dump.ml `ctype`) and
# with the port (c++typing-dump `ctype`), and compare the dumped results
# query by query.  Type ids and ident stamps are renumbered by first visit.
#
# Per value V:        inst gen arrow labels nongen enlarge
# Per type T:         expand
# Per value pair:     unify moregen equal subtype match, for (V, V), (V, next
#                     value) and (V, value seven further on)
#
# Usage: typing_ctype_parity.sh     stdlib + the compiler cmis in /tmp/effid_ref
#        DIRS=a:b CMIS="x.cmi .." JOBS=n typing_ctype_parity.sh
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
BIN=/tmp/typing_cmi_parity/bin
DUMP_ML=$BIN/typing_dump
CPP=${CPP:-$ROOT/cxx/build-release/c++typing-dump}
JOBS=${JOBS:-8}
W=/tmp/typing_ctype_parity; rm -rf $W; mkdir -p $W

src=$ROOT/cxx/harness/typing_dump.ml
if ! [ -f "$DUMP_ML" ] || ! [ "$DUMP_ML" -nt "$src" ]; then
  mkdir -p "$BIN" && cp "$src" "$BIN/typing_dump.ml"
  ./ocamlc.opt -nostdlib -I stdlib -I compilerlibs -I utils -I typing -I parsing \
    -I file_formats -I driver compilerlibs/ocamlcommon.cma "$BIN/typing_dump.ml" \
    -o "$DUMP_ML" || { echo "FATAL: typing_dump build failed" >&2; exit 1; }
fi

REF=/tmp/effid_ref
[ -d $REF ] || { echo "no $REF: run cxx/harness/effid.sh first" >&2; exit 1; }
DIRS=${DIRS:-stdlib:$REF}
CMIS=${CMIS:-$(ls stdlib/stdlib.cmi stdlib/stdlib__*.cmi stdlib/camlinternal*.cmi $REF/*.cmi)}
# shellcheck disable=SC2086
# (operator names containing parentheses would read as applications)
runtime/ocamlrun $DUMP_ML gen $CMIS | grep -v '[()]' | grep -v ' Stdlib__' > $W/items.txt
python3 - "$W" <<'PY'
import sys
w = sys.argv[1]
vals, types = [], []
for line in open(w + '/items.txt'):
    kind, _, name = line.strip().partition(' ')
    if kind == 'value': vals.append(name)
    elif kind == 'type': types.append(name)
ops = []
for v in vals:
    for op in ('inst', 'gen', 'arrow', 'labels', 'nongen', 'enlarge'):
        ops.append(f'{op} {v}')
for t in types:
    ops.append(f'expand {t}')
for k, v in enumerate(vals):
    for other in (v, vals[(k + 1) % len(vals)], vals[(k + 7) % len(vals)]):
        for op in ('unify', 'moregen', 'equal', 'subtype', 'match'):
            ops.append(f'{op} {v} {other}')
with open(w + '/ops.txt', 'w') as f:
    f.write('\n'.join(ops) + '\n')
PY
split -n l/$((JOBS * 4)) -d -a 3 $W/ops.txt $W/chunk.
ulimit -v 8000000
# shellcheck disable=SC2016
ls $W/chunk.* | xargs -P "$JOBS" -I{} sh -c '
  runtime/ocamlrun '"$DUMP_ML"' ctype '"$DIRS"' {} > {}.ocaml 2> {}.ocaml.err
  '"$CPP"' ctype '"$DIRS"' {} > {}.cpp 2> {}.cpp.err || echo "c++ failed on {}" >&2'
python3 - "$W" <<'PY'
import sys, glob
w = sys.argv[1]
def lines(f):
    try: return open(f, encoding='latin-1').read().splitlines()
    except FileNotFoundError: return []
total = same = 0
diffs, missing = [], 0
for ch in sorted(glob.glob(w + '/chunk.[0-9][0-9][0-9]')):
    qs = lines(ch)
    o, c = lines(ch + '.ocaml'), lines(ch + '.cpp')
    for k, q in enumerate(qs):
        total += 1
        a = o[k] if k < len(o) else None
        b = c[k] if k < len(c) else None
        if a is None or b is None: missing += 1
        elif a == b: same += 1
        else: diffs.append(q)
with open(w + '/diffs.txt', 'w') as f:
    f.write('\n'.join(diffs) + '\n')
by_op = {}
for q in diffs: by_op[q.split()[0]] = by_op.get(q.split()[0], 0) + 1
print(f"ops: {total}   SAME: {same}   DIFF: {len(diffs)}   missing: {missing}")
if by_op: print("  DIFF by op:", ' '.join(f'{k}={v}' for k, v in sorted(by_op.items())))
for q in diffs[:15]: print("  DIFF", q)
PY
