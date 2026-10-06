#!/usr/bin/env bash
# -pack oracle (cxx/PORTING.md stage 10): each scenario builds a packed unit
# (and a program using it) twice in scratch directories -- once with
# ocamlc.opt, once with c++ocamlc -- and compares the pack's .cmo and .cmi
# byte for byte, then the program's output.  A second pass (PACKER-ONLY)
# compiles the members with ocamlc.opt for both and packs with each
# compiler, isolating Bytepackager / Typemod.package_units.
#
# Usage: pack_parity.sh [scenario ...]   (G=-g adds -g everywhere;
#   BIN_ANNOT=1 adds -bin-annot and compares the .cmt / .cmti too -- the
#   compilers then run through same-named symlinks, as argv is recorded)
#   SAME / DIFF / CFAIL (c++ocamlc failed) / OFAIL (ocamlc.opt failed)
#   per scenario and pass; outputs are kept in /tmp/pack_parity/<scenario>.
set -u
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
OCAMLC="$ROOT/ocamlc.opt"
OUT=/tmp/pack_parity
G="${G:-}"
FL="-nostdlib -I $ROOT/stdlib -w -a $G"
BIN_ANNOT="${BIN_ANNOT:-}"
[ -n "$BIN_ANNOT" ] && FL="$FL -bin-annot"

# ---- scenarios: files, then the build (in terms of $C, the member compiler,
# and $P, the packer); the pack's artifacts are p.cmo / p.cmi unless PACK=
# names another stem, and `./prog` is run when built ----

sc_basic() {
  cat > a.ml <<'E'
let x = 42
let greet s = "hello " ^ s
E
  cat > b.ml <<'E'
let y = A.x + 1
let () = print_endline (A.greet "b")
E
  cat > main.ml <<'E'
let () = Printf.printf "%d %d\n" P.A.x P.B.y
E
  $C -for-pack P -c a.ml && $C -for-pack P -c b.ml && $P -pack -o p.cmo a.cmo b.cmo &&
    $C -c main.ml && $C p.cmo main.cmo -o prog
}

sc_mli_members() {
  printf 'type t\nval make : int -> t\nval get : t -> int\n' > a.mli
  printf 'type t = int\nlet make x = x * 2\nlet get x = x\n' > a.ml
  printf 'val v : A.t\n' > b.mli
  printf 'let v = A.make 21\n' > b.ml
  printf 'let () = print_int (P.A.get P.B.v); print_newline ()\n' > main.ml
  $C -for-pack P -c a.mli && $C -for-pack P -c a.ml && $C -for-pack P -c b.mli && $C -for-pack P -c b.ml &&
    $P -pack -o p.cmo a.cmo b.cmo && $C -c main.ml && $C p.cmo main.cmo -o prog
}

sc_exceptions() {
  cat > a.ml <<'E'
exception E of int
exception F
let raise_it n = if n > 0 then raise (E n) else raise F
E
  cat > b.ml <<'E'
exception G of string
let f () = try A.raise_it 3 with A.E n -> n
E
  cat > main.ml <<'E'
let () =
  Printf.printf "%d\n" (P.B.f ());
  (try P.A.raise_it 0 with e -> print_endline (Printexc.to_string e));
  (try raise (P.B.G "x") with e -> print_endline (Printexc.to_string e))
E
  $C -for-pack P -c a.ml && $C -for-pack P -c b.ml && $P -pack -o p.cmo a.cmo b.cmo &&
    $C -c main.ml && $C p.cmo main.cmo -o prog
}

sc_pack_mli() {
  printf 'module A : sig val x : int end\nmodule B : sig val y : int val secret : int end\n' > p.mli
  printf 'let x = 1\nlet hidden = 5\n' > a.ml
  printf 'let y = A.x + 10\nlet secret = 7\nlet other = 8\n' > b.ml
  printf 'let () = Printf.printf "%%d %%d\\n" P.A.x P.B.y\n' > main.ml
  $C -c p.mli && $C -for-pack P -c a.ml && $C -for-pack P -c b.ml && $P -pack -o p.cmo a.cmo b.cmo &&
    $C -c main.ml && $C p.cmo main.cmo -o prog
}

sc_nested() {
  printf 'let a = "a"\n' > a.ml
  printf 'let b = A.a ^ "b"\n' > b.ml
  printf 'let c = Q.B.b ^ "c"\n' > c.ml
  printf 'let () = print_endline P.C.c; print_endline P.Q.A.a\n' > main.ml
  $C -for-pack P.Q -c a.ml && $C -for-pack P.Q -c b.ml && $P -for-pack P -pack -o q.cmo a.cmo b.cmo &&
    $C -for-pack P -c c.ml && $P -pack -o p.cmo q.cmo c.cmo && $C -c main.ml && $C p.cmo main.cmo -o prog
}

sc_intf_member() {
  printf 'type color = Red | Green\nmodule type S = sig val name : string end\n' > i.mli
  printf 'let name (c : I.color) = match c with I.Red -> "red" | I.Green -> "green"\n' > a.ml
  printf 'let () = print_endline (P.A.name P.I.Green)\n' > main.ml
  $C -for-pack P -c i.mli && $C -for-pack P -c a.ml && $P -pack -o p.cmo i.cmi a.cmo &&
    $C -c main.ml && $C p.cmo main.cmo -o prog
}

sc_classes_functors() {
  cat > a.ml <<'E'
class counter init = object
  val mutable n = init
  method incr = n <- n + 1
  method get = n
end
module type S = sig type t val show : t -> string end
module Make (X : S) = struct
  let show_all l = String.concat "," (List.map X.show l)
end
E
  cat > b.ml <<'E'
module I = A.Make (struct type t = int let show = string_of_int end)
let c = new A.counter 10
let () = c#incr
E
  cat > main.ml <<'E'
let () = print_endline (P.B.I.show_all [1; 2; 3]); print_int P.B.c#get; print_newline ()
E
  $C -for-pack P -c a.ml && $C -for-pack P -c b.ml && $P -pack -o p.cmo a.cmo b.cmo &&
    $C -c main.ml && $C p.cmo main.cmo -o prog
}

sc_one_invocation() {
  printf 'let x = [1; 2; 3]\n' > a.ml
  printf 'let s = List.fold_left ( + ) 0 A.x\n' > b.ml
  printf 'let () = print_int P.B.s; print_newline ()\n' > main.ml
  $P -for-pack P -pack -o p.cmo a.ml b.ml && $C -c main.ml && $C p.cmo main.cmo -o prog
}

sc_stdlib_refs() {
  cat > a.ml <<'E'
let tbl = Hashtbl.create 16
let () = Hashtbl.replace tbl "k" 1
let buf = Buffer.create 10
let v = Int64.add 3L 4L
let f = 2.5
E
  cat > b.ml <<'E'
let show () = Printf.sprintf "%d %Ld %g %s" (Hashtbl.find A.tbl "k") A.v A.f (Buffer.contents A.buf)
E
  printf 'let () = print_endline (P.B.show ())\n' > main.ml
  $C -for-pack P -c a.ml && $C -for-pack P -c b.ml && $P -pack -o p.cmo a.cmo b.cmo &&
    $C -c main.ml && $C p.cmo main.cmo -o prog
}

sc_pr8769() {
  cp "$ROOT/testsuite/tests/regression/pr8769/nocrypto.mli" "$ROOT/testsuite/tests/regression/pr8769/fortuna.ml" \
     "$ROOT/testsuite/tests/regression/pr8769/rng.ml" .
  PACK=nocrypto
  $C -c nocrypto.mli && $C -for-pack Nocrypto -c fortuna.ml && $C -for-pack Nocrypto -c rng.ml &&
    $P -pack -o nocrypto.cmo fortuna.cmo rng.cmo
}

sc_link_test() {
  local d="$ROOT/testsuite/tests/link-test"
  cp "$d"/{aliases,external_for_pack,external,submodule,test,use_in_pack}.ml "$d"/external.mli \
     "$d"/external_for_pack.mli .
  $C -no-alias-deps -c submodule.ml && $C -c aliases.ml && $C -c external.mli && $C -c external.ml &&
    $C -c external_for_pack.mli && $C -c external_for_pack.ml && $C -c test.ml &&
    $C -a -no-alias-deps submodule.cmo aliases.cmo external.cmo external_for_pack.cmo -o mylib.cma &&
    $C -no-alias-deps -for-pack P -c use_in_pack.ml && $P -no-alias-deps -pack -o p.cmo use_in_pack.cmo &&
    $C -no-alias-deps mylib.cma p.cmo test.cmo -o prog
}

sc_errors() {
  printf 'let x = 1\n' > a.ml
  printf 'let y = A.x\n' > b.ml
  $C -for-pack P -c a.ml && $C -for-pack P -c b.ml && { $P -pack -o p.cmo b.cmo a.cmo; true; }
}

ALL="basic mli_members exceptions pack_mli nested intf_member classes_functors one_invocation stdlib_refs pr8769 link_test errors"

run_one() {  # scenario pass dir member-compiler packer
  # built in one directory for both compilers (the -g debug directories
  # recorded in a .cmo name it), then moved to [dir]
  local sc="$1" dir="$3" w="$OUT/$1/$2/w"
  mkdir -p "$w"
  local cm="$4" pk="$5"
  if [ -n "$BIN_ANNOT" ]; then  # same argv[0] for both compilers
    mkdir -p "$OUT/bin_m" "$OUT/bin_p"
    ln -sfn "$4" "$OUT/bin_m/ocamlc"; ln -sfn "$5" "$OUT/bin_p/ocamlc"
    cm="$OUT/bin_m/ocamlc"; pk="$OUT/bin_p/ocamlc"
  fi
  ( cd "$w" && C="$cm $FL" P="$pk $FL" PACK=p && "sc_$sc" >build.log 2>&1; echo $? > rc
    [ -f prog ] && "$ROOT/runtime/ocamlrun" ./prog > run.out 2>&1 )
  mv "$w" "$dir"
}

rm -rf "$OUT"; mkdir -p "$OUT"
[ $# -gt 0 ] && ALL="$*"
vm_limit 8000000
for sc in $ALL; do
  for pass in full packer; do
    o="$OUT/$sc/$pass/o"; c="$OUT/$sc/$pass/c"
    if [ $pass = full ]; then
      run_one "$sc" $pass "$o" "$OCAMLC" "$OCAMLC"; run_one "$sc" $pass "$c" "$CPP" "$CPP"
    else
      run_one "$sc" $pass "$o" "$OCAMLC" "$OCAMLC"; run_one "$sc" $pass "$c" "$OCAMLC" "$CPP"
    fi
    stem=p; [ "$sc" = pr8769 ] && stem=nocrypto
    res=SAME; why=""
    if [ "$(cat "$o/rc")" != 0 ] && [ "$sc" != errors ]; then res=OFAIL
    elif [ "$(cat "$c/rc")" != 0 ] && [ "$sc" != errors ]; then res=CFAIL
    else
      for f in $stem.cmo $stem.cmi q.cmo q.cmi $( [ -n "$BIN_ANNOT" ] && cd "$o" && ls *.cmt *.cmti 2>/dev/null); do
        if [ -f "$o/$f" ] || [ -f "$c/$f" ]; then
          cmp -s "$o/$f" "$c/$f" || { res=DIFF; why="$why $f"; }
        fi
      done
      if [ -f "$o/run.out" ] || [ -f "$c/run.out" ]; then
        cmp -s "$o/run.out" "$c/run.out" || { res=DIFF; why="$why run.out"; }
      fi
      if [ "$sc" = errors ]; then  # both reject, nothing left behind
        { [ -f "$o/p.cmo" ] || [ -f "$c/p.cmo" ]; } && { res=DIFF; why="$why p.cmo-left"; }
        [ "$(cat "$o/rc")" = "$(cat "$c/rc")" ] || { res=DIFF; why="$why rc"; }
      fi
    fi
    printf '%-6s %-18s %-7s%s\n' "$res" "$sc" "$pass" "$why"
  done
done | tee /tmp/.pack_parity_results
awk '{f[$1]++} END{printf "pack scenarios x passes %d: SAME %d  DIFF %d  CFAIL %d  OFAIL %d\n", NR, f["SAME"], f["DIFF"], f["CFAIL"], f["OFAIL"]}' /tmp/.pack_parity_results
