#!/usr/bin/env bash
# Normalize-and-diff a single corpus file: c++type vs the oracle -dtypedtree dump.
# Usage: ttp_one.sh <file.ml> [-q]   (-q: just print MATCH/DIFF/first c++ error)
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
f="$1"; quiet="${2:-}"
norm() { perl -0777 -pe 'BEGIN{%m=();$n=0} s{/(\d+)}{"/".($m{$1}//=++$n)}ge; s/\s+\z//'; }
o=$(mktemp); c=$(mktemp)
./ocamlc.opt -nostdlib -I stdlib -I otherlibs/unix -I otherlibs/str \
  -I otherlibs/systhreads -I otherlibs/runtime_events -I otherlibs/dynlink \
  -stop-after typing -dtypedtree "$f" 2>&1 1>/dev/null | sed -n '/^\[/,$p' | norm > "$o"
./cxx/build/c++type "$f" 2>/dev/null | norm > "$c"
if [ "$(head -c 10 "$c")" = "TYPE_ERROR" ]; then
  echo "CPPERR: $(head -1 "$c")"; rm -f "$o" "$c"; exit 2
fi
if cmp -s "$o" "$c"; then echo "MATCH"; rm -f "$o" "$c"; exit 0; fi
echo "DIFF"
[ "$quiet" != "-q" ] && diff "$o" "$c" | head -60
rm -f "$o" "$c"; exit 1
