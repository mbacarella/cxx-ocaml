#!/usr/bin/env bash
# Stage-9 oracle, multi-file errors (TYPECHECKER.md): scenarios that need
# several compilations -- inconsistent assumptions, corrupted / misnamed /
# -rectypes .cmi, interface mismatch, missing files, -pack -- comparing
# the last command's stderr and exit code, ocamlc.opt vs c++ocamlc, byte
# for byte.  Each scenario runs in a fresh scratch directory per compiler.
set -u
SELF="$(readlink -f "$0")"
R="$(cd "$(dirname "$SELF")/../.." && pwd)"
CPP="${CPP:-$R/cxx/build-release/c++ocamlc}"
same=0; diff_=0
run() {  # name script ($C is the compiler with its stdlib flags)
  name=$1; script=$2
  local out
  out=$(mktemp -d)
  for side in o c; do
    d=$(mktemp -d)
    if [ $side = o ]; then C="$R/ocamlc.opt -nostdlib -I $R/stdlib"; else C="$CPP -I $R/stdlib"; fi
    ( cd "$d" && eval "$script" ) >/dev/null 2>"$out/$side"; echo $? >> "$out/$side"
    rm -rf "${d:?}"
  done
  if cmp -s "$out/o" "$out/c"; then echo "SAME $name"; same=$((same+1))
  else echo "DIFF $name"; diff "$out/o" "$out/c" | head -12; diff_=$((diff_+1)); fi
  rm -rf "${out:?}"
}
run inconsistent 'echo "val x : int" > a.mli; $C -c a.mli; echo "let y = A.x" > b.ml; $C -c b.ml; echo "val x : string" > a.mli; $C -c a.mli; echo "let z = A.x let w = B.y" > c.ml; $C -c c.ml'
run not_interface 'echo garbage > a.cmi; echo "let y = A.x" > b.ml; $C -c b.ml'
run illegal_renaming 'echo "val x : int" > a.mli; $C -c a.mli; cp a.cmi b.cmi; echo "let y = B.x" > c.ml; $C -c c.ml'
run need_rectypes 'echo "type t = t list" > a.mli; $C -rectypes -c a.mli; echo "let y = (Obj.magic 0 : A.t)" > c.ml; $C -c c.ml'
run unbound_module_file 'echo "let y = Nosuch.x" > c.ml; $C -c c.ml'
run intf_mismatch 'echo "val x : int" > a.mli; $C -c a.mli; echo "let x = true" > a.ml; $C -c a.ml'
run missing_file 'echo "let x = 1" > a.ml; $C -c b.ml'
run impl_required 'echo "val x : int" > a.mli; $C -c a.mli; $C -pack -o p.cmo a.cmi'
run forward_reference_pack 'echo "let x = B.y" > a.ml; echo "let y = 1" > b.ml; $C -for-pack P -c b.ml; $C -for-pack P -c a.ml; $C -pack -o p.cmo a.cmo b.cmo'
echo "scenarios $((same+diff_)): SAME $same  DIFF $diff_"
