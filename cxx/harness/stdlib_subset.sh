#!/usr/bin/env bash
# Self-built stdlib subset: compile a set of stdlib modules from their own
# sources (.mli -> .cmi, .ml -> .cmo) with c++ocamlc, build a test program that
# exercises them, link the whole thing with c++link, run it under ocamlrun, and
# compare stdout against the oracle building+running the same program with the
# normal stdlib.
#
# This is the "bootstrap, self-consistent" milestone: every object in the
# program (except the Stdlib core pulled from the oracle's stdlib.cma for
# pervasives/Printf) is produced by our toolchain.  It is NOT a binary drop-in
# for the oracle's stdlib objects -- we inline externals rather than giving them
# module fields, a valid but different convention -- so the modules are linked
# under their bare names, all built by us.
#
# Usage: stdlib_subset.sh
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
CPP=$ROOT/cxx/build/c++ocamlc
LINK=$ROOT/cxx/build/c++link
RUN=$ROOT/runtime/ocamlrun
STD=$ROOT/stdlib

# Leaf-ish modules that depend only on the Stdlib core (and each other).
MODS="int char string list array option result either fun unit bool"

TEST_SRC='let () =
  Printf.printf "int.max=%d\n" (Int.max 3 9);
  Printf.printf "char.code=%d\n" (Char.code (Char.chr 65));
  Printf.printf "string.cat=%s\n" (String.cat "foo" "bar");
  Printf.printf "string.len=%d\n" (String.length (String.concat "," ["a";"bb";"ccc"]));
  let l = List.map (fun x -> x * x) [1;2;3;4;5] in
  Printf.printf "list.sum=%d\n" (List.fold_left (+) 0 l);
  Printf.printf "list.rev=%d\n" (List.hd (List.rev l));
  let a = Array.make 4 0 in Array.set a 2 13;
  Printf.printf "array.get=%d\n" (Array.get a 2);
  Printf.printf "option=%d\n" (Option.value (Option.map succ (Some 41)) ~default:0);
  Printf.printf "result=%d\n" (Result.value (Result.map (( * ) 2) (Ok 21)) ~default:0);
  Printf.printf "either=%b\n" (Either.is_left (Either.Left 1 : (int,int) Either.t));
  Printf.printf "bool=%s\n" (Bool.to_string (Bool.not false));
  Printf.printf "fun.id=%d\n" (Fun.id 7)'

td=$(mktemp -d) || exit 1
trap 'rm -rf "$td"' EXIT
cd "$td"

# --- our side: build the subset from source, then the program, then link ----
ours_objs=""
for m in $MODS; do
  [ -f "$STD/$m.mli" ] && cp "$STD/$m.mli" .
  cp "$STD/$m.ml" .
  [ -f "$m.mli" ] && "$CPP" -c -I "$STD" "$m.mli" 2>/dev/null
  if ! "$CPP" -c -I "$STD" -I . "$m.ml" 2>err; then
    echo "FAIL: c++ocamlc could not compile $m.ml"; sed 's/^/  /' err; exit 1
  fi
  [ -f "$m.cmo" ] || { echo "FAIL: no $m.cmo produced"; exit 1; }
  ours_objs="$ours_objs $m.cmo"
done

printf '%s\n' "$TEST_SRC" > app.ml
if ! "$CPP" -c -I "$STD" -I . app.ml 2>err; then
  echo "FAIL: c++ocamlc could not compile app.ml"; sed 's/^/  /' err; exit 1
fi
if ! "$LINK" -runtime "$RUN" -I "$STD" $ours_objs app.cmo -o prog 2>err; then
  echo "FAIL: c++link could not link the subset"; sed 's/^/  /' err; exit 1
fi
ours_out=$("$RUN" ./prog 2>&1); ours_rc=$?

# --- reference: the SAME program built normally by c++ocamlc against the full
#     stdlib (oracle's Stdlib__* objects from stdlib.cma), in a clean dir so our
#     bare-named .cmi files (module Int, Bool, ...) don't shadow Stdlib__*.  If
#     the subset build matches this, our self-built modules behave identically
#     to the stdlib ones.  (We avoid the oracle ocamlc.opt here: it is sensitive
#     to stale embedded interface CRCs -- see cppcaml-oracle-binary-fragile.)
mkdir -p ref && cp app.ml ref/
( cd ref && "$CPP" -I "$STD" app.ml -o rprog 2>/dev/null )
ref_out=$("$RUN" ref/rprog 2>&1); ref_rc=$?

echo "modules built by c++ocamlc: $(echo $MODS | wc -w)  ($MODS)"
echo "--- our subset output (rc=$ours_rc) ---"; printf '%s\n' "$ours_out"
if [ "$ours_out" = "$ref_out" ] && [ "$ours_rc" = "$ref_rc" ]; then
  echo "MATCH: self-built stdlib subset behaves identically to the full-stdlib build"
  exit 0
else
  echo "DIFF vs full-stdlib reference (rc=$ref_rc):"; printf '%s\n' "$ref_out"
  exit 1
fi
