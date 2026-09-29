#!/usr/bin/env bash
# Native link scenarios (cxx/PORTING.md): libraries (-a, -linkall, C
# options), -output-obj / -output-complete-obj, -shared, -pack, one-step builds, -verbose
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
scen shared 'C=$C; $C -c a.ml b.ml && $C -shared -dstartup -o ab.cmxs a.cmx b.cmx'
scen shared-lib 'C=$C; $C -c a.ml b.ml && $C -a -o ab.cmxa a.cmx b.cmx -cclib -lm && $C -shared -linkall -o ab.cmxs ab.cmxa'
scen shared-onestep 'C=$C; $C -shared -dstartup -o ab.cmxs a.ml b.ml'
scen shared-missing 'C=$C; $C -c a.ml b.ml && $C -shared -o b.cmxs b.cmx'
scen pack 'C=$C; $C -for-pack P -c a.ml b.ml && $C -pack -o p.cmx a.cmx b.cmx && echo "let () = print_int (P.B.g 3)" > u.ml && $C -c u.ml && $C -dstartup -o m.exe p.cmx u.cmx && ./m.exe'
scen pack-mli 'C=$C; echo "module A : sig val x : int end module B : sig val g : int -> int end" > p.mli && $C -c p.mli && $C -for-pack P -c a.ml b.ml && $C -pack -o p.cmx a.cmx b.cmx && echo "let () = print_int (P.B.g 3)" > u.ml && $C -c u.ml && $C -o m.exe p.cmx u.cmx && ./m.exe'
scen pack-S 'C=$C; $C -for-pack P -c a.ml b.ml && $C -S -pack -o p.cmx a.cmx b.cmx'
scen pack-g 'C=$C; $C -g -for-pack P -c a.ml b.ml && $C -g -pack -o p.cmx a.cmx b.cmx'
scen pack-onestep 'C=$C; $C -for-pack P -pack -o p.cmx a.ml b.ml'
scen pack-nested 'C=$C; $C -for-pack Q.P -c a.ml b.ml && $C -for-pack Q -pack -o p.cmx a.cmx b.cmx && $C -pack -o q.cmx p.cmx && echo "let () = print_int (Q.P.B.g 3)" > u.ml && $C -c u.ml && $C -o m.exe q.cmx u.cmx && ./m.exe'
scen pack-lib 'C=$C; $C -for-pack P -c a.ml b.ml && $C -pack -o p.cmx a.cmx b.cmx && $C -a -o p.cmxa p.cmx'
scen pack-forward 'C=$C; $C -for-pack P -c a.ml b.ml && $C -pack -o p.cmx b.cmx a.cmx'
scen pack-wrong-for-pack 'C=$C; $C -c a.ml b.ml && $C -pack -o p.cmx a.cmx b.cmx'
scen pack-renamed 'C=$C; $C -for-pack P -c a.ml && cp a.cmx z.cmx && cp a.o z.o && $C -pack -o p.cmx z.cmx'
scen pack-not-found 'C=$C; $C -pack -o p.cmx nothere.cmx'
scen cmi-file 'C=$C; $B/ocamlc.opt -nostdlib -I $B/stdlib -c a.ml && rm a.cmo && $C -cmi-file a.cmi -c a.ml'
rm -rf $W
