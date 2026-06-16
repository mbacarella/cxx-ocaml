#!/usr/bin/env bash
# Fully self-built stdlib: compile EVERY stdlib module from source with
# c++ocamlc (the whole core -- CamlinternalFormatBasics, Stdlib, CamlinternalFormat,
# Printf, ...), link a Printf/String/List program against ONLY our objects (no
# oracle stdlib.cma at all), run it under ocamlrun, and check the output.
#
# This is the bootstrap milestone: the runtime + our toolchain + our stdlib, end
# to end.  Modules are built with the Stdlib__ prefix (via filename) so cross-
# module references resolve like the real stdlib build; stdlib.{mli,ml} and the
# *Labels modules get the expand_module_aliases.awk preprocessing.
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
CPP=$ROOT/cxx/build/c++ocamlc
LINK=$ROOT/cxx/build/c++link
RUN=$ROOT/runtime/ocamlrun
STD=$ROOT/stdlib

# Dependency order (StdlibModules), with atomic/mutex/condition/semaphore hoisted
# before camlinternalLazy/lazy for link order (lazy refs Mutex via a local module).
ORDER="camlinternalFormatBasics stdlib either sys obj type atomic mutex condition \
semaphore camlinternalLazy lazy seq option pair result bool char uchar list int \
array iarray bytes string unit marshal float int32 int64 nativeint lexing parsing \
repr set map stack queue buffer camlinternalFormat printf"

gname() { case "$1" in camlinternal*|stdlib) echo "$1";;
  *) cap="$(tr '[:lower:]' '[:upper:]' <<< ${1:0:1})${1:1}"; echo "stdlib__$cap";; esac; }
flags() { case "$1" in camlinternalFormatBasics|stdlib) echo "-nopervasives";; *) echo "";; esac; }
needs_awk() { case "$1" in stdlib|*Labels) return 0;; *) return 1;; esac; }

BD=$(mktemp -d) || exit 1
trap 'rm -rf "$BD"' EXIT

# Build every module's .cmi then .cmo into BD (BD is its own stdlib_dir via -I).
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
module M = Map.Make (Int)
let () =
  Printf.printf "hello %d %s %b\n" 42 "world" true;
  Printf.printf "sum=%d\n" (List.fold_left (+) 0 (List.map (fun x -> x*x) [1;2;3;4]));
  Printf.printf "cat=%s sub=%s\n" (String.concat "-" ["a";"b";"c"]) (String.sub "abcdef" 1 3);
  Printf.printf "sorted=%s\n" (String.concat "," (List.map string_of_int (List.sort compare [3;1;2])));
  let m = M.add 3 "c" (M.add 1 "a" (M.add 2 "b" M.empty)) in
  Printf.printf "map=%s card=%d find2=%s\n"
    (String.concat "" (List.map snd (M.bindings m))) (M.cardinal m) (M.find 2 m)
EOF
( cd "$BD" && "$CPP" -c -I "$BD" app.ml ) 2>/dev/null || { echo "FAIL: app.ml"; exit 1; }

objs=""; for m in $ORDER; do f="$BD/$(gname "$m").cmo"; [ -f "$f" ] && objs="$objs $f"; done
if ! "$LINK" -nostdlib -runtime "$RUN" $objs "$BD/app.cmo" "$BD/std_exit.cmo" -o "$BD/prog" 2>"$BD/err"; then
  echo "FAIL: link (all-ours)"; sed 's/^/  /' "$BD/err"; exit 1
fi

out=$("$RUN" "$BD/prog" 2>&1); rc=$?
want='hello 42 world true
sum=30
cat=a-b-c sub=bcd
sorted=1,2,3
map=abc card=3 find2=b'
echo "--- output (rc=$rc) ---"; printf '%s\n' "$out"
if [ "$out" = "$want" ] && [ "$rc" = 0 ]; then
  echo "MATCH: Printf/String/List/Map run correctly on the fully self-built stdlib"
  exit 0
else
  echo "DIFF: expected"; printf '%s\n' "$want"; exit 1
fi
