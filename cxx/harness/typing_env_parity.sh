#!/usr/bin/env bash
# Stage-2 oracle for the typing/ port (TYPECHECKER.md): name every item of a
# set of cmis (values, types, constructors, labels, modules, module types,
# classes, one level of submodules, plus functor applications), look each up
# with Env.find_*_by_name in Env.initial + open Stdlib, and compare the
# dumped results of compiler-libs (typing_dump.ml) and the port
# (c++typing-dump) line by line.  Type ids and ident stamps are renumbered by
# first visit.
#
# Usage: typing_env_parity.sh            stdlib + the ocamlc.opt-built
#                                        compiler cmis in /tmp/effid_ref
#        DIRS=a:b CMIS="x.cmi .." typing_env_parity.sh
set -u
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
BIN=/tmp/typing_cmi_parity/bin
DUMP_ML=$BIN/typing_dump
CPP=${CPP:-$ROOT/cxx/build-release/c++typing-dump}
W=/tmp/typing_env_parity; mkdir -p $W

src=$ROOT/cxx/harness/typing_dump.ml
if ! [ -f "$DUMP_ML" ] || ! [ "$DUMP_ML" -nt "$src" ]; then
  mkdir -p "$BIN" && cp "$src" "$BIN/typing_dump.ml"
  ./ocamlc.opt -nostdlib -I stdlib -I compilerlibs -I utils -I typing -I parsing \
    -I file_formats compilerlibs/ocamlcommon.cma "$BIN/typing_dump.ml" \
    -o "$DUMP_ML" || { echo "FATAL: typing_dump build failed" >&2; exit 1; }
fi

REF=/tmp/effid_ref
[ -d $REF ] || { echo "no $REF: run cxx/harness/effid.sh first" >&2; exit 1; }
DIRS=${DIRS:-stdlib:$REF}
CMIS=${CMIS:-$(ls stdlib/stdlib.cmi stdlib/stdlib__*.cmi stdlib/camlinternal*.cmi $REF/*.cmi)}
# shellcheck disable=SC2086
# (operator names containing parentheses would read as applications)
runtime/ocamlrun $DUMP_ML gen $CMIS | grep -v '[()]' > $W/queries.txt
cat >> $W/queries.txt <<'Q'
module Map.Make(String)
type Map.Make(String).t
value Map.Make(String).empty
value Map.Make(String).add
module Set.Make(Int)
value Set.Make(Int).elements
type Hashtbl.Make(Int).t
value Hashtbl.Make(String).create
module Misc.Stdlib.String.Map
value Misc.Stdlib.String.Map.find
type Path.Map.t
value Path.Map.add
module Ident.Map
value Types.TransientTypeHash.create
module Ephemeron.K1.Make(Int)
Q
runtime/ocamlrun $DUMP_ML env $DIRS $W/queries.txt > $W/ocaml.txt || echo "oracle failed" >&2
$CPP env $DIRS $W/queries.txt > $W/cpp.txt || echo "c++ failed" >&2
# one result per query: compare query by query
python3 - "$W" <<'PY'
import sys
w = sys.argv[1]
def results(f):
    out, cur = [], None
    for line in open(f, encoding='latin-1'):
        if ' => ' in line and not line.startswith(' '):
            if cur is not None: out.append(cur)
            cur = line
        else:
            cur = (cur or '') + line
    if cur is not None: out.append(cur)
    return out
o, c = results(w + '/ocaml.txt'), results(w + '/cpp.txt')
same = sum(1 for a, b in zip(o, c) if a == b)
diff = [a.split(' => ')[0] for a, b in zip(o, c) if a != b]
print(f"queries: {len(o)}   SAME: {same}   DIFF: {len(diff) + abs(len(o) - len(c))}")
for q in diff[:20]: print("  DIFF", q)
PY
