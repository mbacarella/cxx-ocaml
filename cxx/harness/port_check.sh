#!/usr/bin/env bash
# `CHECKER --check FILE` for the type checker (TYPECHECKER.md): runs
# c++ocamlc -stop-after typing on a copy of FILE in a scratch directory (so
# neither its neighbours' .cmi files nor its own outputs interfere).  Exit
# status 0 = accepted, 2 = type error.  For the false_accept.sh /
# valid_reject.sh harnesses (their default CPP).
set -u
[ "${1:-}" == "--check" ] && shift
f="$1"
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
cp "$f" "$w/"
cd "$w" && "$ROOT/cxx/build-release/c++ocamlc" -I "$ROOT/stdlib" -w -a -stop-after typing -c "$(basename "$f")"
