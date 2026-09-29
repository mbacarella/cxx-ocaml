#!/usr/bin/env bash
# Native link scenarios (cxx/PORTING.md): libraries (-a, -linkall, C
# options), -output-obj / -output-complete-obj, one-step builds, -verbose
# and the link errors -- each run by ocamlopt.opt and by c++ocamlopt in the
# same directory; every file left behind (and the output, exit code
# included) compared byte for byte.  V=1 shows the output diffs.
# Usage: native_link_scenarios.sh
set -u
B="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
REF="$B/ocamlopt.opt -nostdlib -I $B/stdlib"
CPP="${CPP:-$B/cxx/build-release/c++ocamlopt} -I $B/stdlib"
W=$(mktemp -d)
setup() {
  rm -rf $W/x; mkdir $W/x; cd $W/x
  echo 'let x = 42 let f y = y + x' > a.ml
  echo 'let g z = A.f z * 2 let () = Printf.printf "b init\n"' > b.ml
  echo 'let () = Printf.printf "%d\n" (B.g 1)' > main.ml
  echo 'let () = ()' > lone.ml
}
# scenario name, commands using $C
scen() {
  local name="$1" cmds="$2"
  for who in o c; do
    setup
    if [ $who = o ]; then C="$REF"; else C="$CPP"; fi
    ( eval "$cmds" ) > out 2>&1; echo "rc=$?" >> out
    sed -i "s|$W/x|W|g" out
    rm -rf $W/$who; mv $W/x $W/$who
  done
  local d=""
  for f in $(cd $W/o && ls); do
    [ -e "$W/c/$f" ] || { d="$d missing:$f"; continue; }
    cmp -s "$W/o/$f" "$W/c/$f" || d="$d $f"
  done
  for f in $(cd $W/c && ls); do [ -e "$W/o/$f" ] || d="$d extra:$f"; done
  if [ -z "$d" ]; then echo "SAME $name"; else echo "DIFF $name:$d"; [ -n "$V" ] && diff $W/o/out $W/c/out; fi
}
V="${V:-}"
scen lib 'C=$C; $C -c a.ml b.ml main.ml && $C -a -o ab.cmxa a.cmx b.cmx -cclib -lm -ccopt -O2 && $C -dstartup -o m.exe ab.cmxa main.cmx && ./m.exe'
scen linkall 'C=$C; $C -c a.ml b.ml lone.ml && $C -a -o ab.cmxa a.cmx b.cmx && $C -linkall -dstartup -o m.exe ab.cmxa lone.cmx && ./m.exe'
scen unused-lib 'C=$C; $C -c a.ml b.ml lone.ml && $C -a -o ab.cmxa a.cmx b.cmx && $C -dstartup -o m.exe ab.cmxa lone.cmx && ./m.exe'
scen lib-linkall-a 'C=$C; $C -c a.ml b.ml lone.ml && $C -a -linkall -o ab.cmxa a.cmx b.cmx && $C -dstartup -o m.exe ab.cmxa lone.cmx && ./m.exe'
scen output-obj 'C=$C; $C -c a.ml b.ml main.ml && $C -output-obj -dstartup -o m.o a.cmx b.cmx main.cmx'
scen output-complete-obj 'C=$C; $C -c a.ml b.ml main.ml && $C -output-complete-obj -dstartup -o m.o a.cmx b.cmx main.cmx'
scen missing-impl 'C=$C; $C -c a.ml b.ml main.ml && $C -o m.exe b.cmx main.cmx'
scen wrong-order 'C=$C; $C -c a.ml b.ml main.ml && $C -o m.exe main.cmx b.cmx a.cmx'
scen inconsistent 'C=$C; $C -c a.ml b.ml main.ml && echo "let x = 1 let f y = y let h = 3" > a.ml && $C -c a.ml && $C -o m.exe a.cmx b.cmx main.cmx'
scen not-found 'C=$C; $C -o m.exe nothere.cmx'
scen not-object 'C=$C; echo junk > j.cmx; $C -o m.exe j.cmx'
scen onestep-multi 'C=$C; $C -dstartup -o m.exe a.ml b.ml main.ml && ./m.exe'
scen onestep-g 'C=$C; $C -g -dstartup -o m.exe a.ml b.ml main.ml && ./m.exe'
scen default-aout 'C=$C; $C a.ml b.ml main.ml && ./a.out'
scen verbose 'C=$C; $C -c a.ml b.ml main.ml && $C -verbose -o m.exe a.cmx b.cmx main.cmx 2>&1 | sed -E "s/camlstartup[a-f0-9]+/camlstartupX/g"'
rm -rf $W
