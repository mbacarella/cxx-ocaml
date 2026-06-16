#!/usr/bin/env bash
# The widest all-ours milestone: compile EVERY stdlib module from source up
# through Filename with c++ocamlc, link a program that exercises the advanced
# modules (immediate objects, Dynarray, Format, Filename, Hashtbl, effects,
# classes) against ONLY our objects, run it under ocamlrun, and check the output.
#
# This is the "almost the whole stdlib" bootstrap point: everything but the
# *Labels aliases / effect's deepest corners.  It depends on the atomic-location
# API, optional-arg arrow labels, named-modtype submodules, the predef-exception
# fixes, and the effect/OO/package-type-through-match lowerings all being right.
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
CPP=$ROOT/cxx/build/c++ocamlc
LINK=$ROOT/cxx/build/c++link
RUN=$ROOT/runtime/ocamlrun
STD=$ROOT/stdlib

ORDER="camlinternalFormatBasics stdlib either sys obj type atomic mutex condition \
semaphore camlinternalLazy lazy seq option pair result bool char uchar list int \
array iarray bytes string unit marshal float int32 int64 nativeint lexing parsing \
repr set map stack queue buffer camlinternalFormat printf arg printexc domain fun \
gc in_channel out_channel digest bigarray random hashtbl weak scanf callback \
camlinternalOO oo dynarray format camlinternalMod pqueue ephemeron filename complex effect"

gname() { case "$1" in camlinternal*|stdlib) echo "$1";;
  *) cap="$(tr '[:lower:]' '[:upper:]' <<< ${1:0:1})${1:1}"; echo "stdlib__$cap";; esac; }
flags() { case "$1" in camlinternalFormatBasics|stdlib) echo "-nopervasives";; *) echo "";; esac; }
needs_awk() { case "$1" in stdlib|*Labels) return 0;; *) return 1;; esac; }

BD=$(mktemp -d) || exit 1
trap 'rm -rf "$BD"' EXIT

for phase in cmi cmo; do
  for m in $ORDER; do
    s=$(gname "$m")
    for ext in mli ml; do
      [ -f "$STD/$m.$ext" ] || continue
      if needs_awk "$m"; then awk -f "$STD/expand_module_aliases.awk" "$STD/$m.$ext" > "$BD/$s.$ext"
      else cp "$STD/$m.$ext" "$BD/$s.$ext"; fi
    done
    if [ "$phase" = cmi ]; then [ -f "$BD/$s.mli" ] || continue; src="$BD/$s.mli"
    else src="$BD/$s.ml"; [ -f "$src" ] || continue; fi
    if ! ( cd "$BD" && "$CPP" -c $(flags "$m") -I "$BD" "$(basename "$src")" ) 2>"$BD/err"; then
      echo "FAIL: c++ocamlc -c $(basename "$src")"; sed 's/^/  /' "$BD/err"; exit 1
    fi
  done
done
cp "$STD/std_exit.ml" "$BD/std_exit.ml"
( cd "$BD" && "$CPP" -c -I "$BD" std_exit.ml ) 2>/dev/null

cat > "$BD/app.ml" <<'EOF'
(* an immediate object created at module init (the dynarray pattern) *)
let _obj = let r = ref None in let o = object val x = r end in r := Some o; o
module H = Hashtbl.Make (struct type t = string let equal = String.equal let hash = Hashtbl.hash end)
type _ Effect.t += Ask : int Effect.t
let () =
  let d = Dynarray.create () in
  Dynarray.add_last d 1; Dynarray.add_last d 2; Dynarray.add_last d 3;
  let h = H.create 8 in H.replace h "a" 10; H.replace h "b" 20;
  let r = (try Some (Hashtbl.find (Hashtbl.create 1) "z") with Not_found -> None) in
  let eff = Effect.Deep.match_with (fun () -> 1 + Effect.perform Ask) ()
    { retc = Fun.id; exnc = raise;
      effc = (fun (type a) (e : a Effect.t) -> match e with
        | Ask -> Some (fun (k : (a, _) Effect.Deep.continuation) -> Effect.Deep.continue k 40)
        | _ -> None) } in
  Printf.printf "dyn=%s sum=%d hfind=%d nf=%b\n"
    (String.concat "," (List.map string_of_int (Dynarray.to_list d)))
    (Dynarray.fold_left (+) 0 d) (H.find h "b") (r = None);
  Printf.printf "fmt=%s file=%s/%s eff=%d\n"
    (Format.asprintf "<%d:%s>" 7 "x") (Filename.dirname "/p/q.ml")
    (Filename.basename "/p/q.ml") eff
EOF
( cd "$BD" && "$CPP" -c -I "$BD" app.ml ) 2>/dev/null || { echo "FAIL: app.ml"; exit 1; }

objs=""; for m in $ORDER; do f="$BD/$(gname "$m").cmo"; [ -f "$f" ] && objs="$objs $f"; done
if ! "$LINK" -nostdlib -runtime "$RUN" $objs "$BD/app.cmo" "$BD/std_exit.cmo" -o "$BD/prog" 2>"$BD/err"; then
  echo "FAIL: link (all-ours)"; sed 's/^/  /' "$BD/err"; exit 1
fi

out=$("$RUN" "$BD/prog" 2>&1); rc=$?
want='dyn=1,2,3 sum=6 hfind=20 nf=true
fmt=<7:x> file=/p/q.ml eff=41'
echo "--- output (rc=$rc) ---"; printf '%s\n' "$out"
if [ "$out" = "$want" ] && [ "$rc" = 0 ]; then
  echo "MATCH: Dynarray/Format/Filename/Hashtbl.Make/effects/immediate-objects/try-Not_found all run on the fully self-built stdlib (through Filename)"
  exit 0
else
  echo "DIFF: expected"; printf '%s\n' "$want"; exit 1
fi
