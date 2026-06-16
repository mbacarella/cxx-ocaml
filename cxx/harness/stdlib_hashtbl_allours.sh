#!/usr/bin/env bash
# Extends stdlib_allours.sh past the core: builds EVERY stdlib module up through
# Hashtbl (which pulls in Domain -- multicore DLS/atomics) from source with
# c++ocamlc, links a Hashtbl/Random/Domain.DLS program against ONLY our objects,
# runs it under ocamlrun, and checks the output.
#
# This is the Domain/Hashtbl bootstrap milestone: the atomic-location API, the
# optional-argument arrow labels in our .cmi, and the submodule impl->intf
# coercion all have to be right for Domain to initialise and Hashtbl to work.
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
CPP=$ROOT/cxx/build/c++ocamlc
LINK=$ROOT/cxx/build/c++link
RUN=$ROOT/runtime/ocamlrun
STD=$ROOT/stdlib

# Dependency order (StdlibModules subset up through hashtbl + random), with
# atomic/mutex/condition/semaphore hoisted before camlinternalLazy/lazy.
ORDER="camlinternalFormatBasics stdlib either sys obj type atomic mutex condition \
semaphore camlinternalLazy lazy seq option pair result bool char uchar list int \
array iarray bytes string unit marshal float int32 int64 nativeint lexing parsing \
repr set map stack queue buffer camlinternalFormat printf arg printexc domain fun \
gc in_channel out_channel digest bigarray random hashtbl"

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
let () =
  let h = Hashtbl.create 16 in
  Hashtbl.replace h "x" 42; Hashtbl.replace h "y" 7; Hashtbl.replace h "x" 99;
  Printf.printf "find=%d mem_z=%b len=%d\n"
    (Hashtbl.find h "x") (Hashtbl.mem h "z") (Hashtbl.length h);
  let k = Domain.DLS.new_key (fun () -> 5) in
  Domain.DLS.set k 17;
  Printf.printf "dls=%d main=%b\n" (Domain.DLS.get k) (Domain.is_main_domain ());
  Random.init 42;
  let a = Random.int 1000 and b = Random.int 1000 in
  Printf.printf "rand_ok=%b\n" (a >= 0 && a < 1000 && b >= 0 && b < 1000 && a <> b)
EOF
( cd "$BD" && "$CPP" -c -I "$BD" app.ml ) 2>/dev/null || { echo "FAIL: app.ml"; exit 1; }

objs=""; for m in $ORDER; do f="$BD/$(gname "$m").cmo"; [ -f "$f" ] && objs="$objs $f"; done
if ! "$LINK" -nostdlib -runtime "$RUN" $objs "$BD/app.cmo" "$BD/std_exit.cmo" -o "$BD/prog" 2>"$BD/err"; then
  echo "FAIL: link (all-ours)"; sed 's/^/  /' "$BD/err"; exit 1
fi

out=$("$RUN" "$BD/prog" 2>&1); rc=$?
want='find=99 mem_z=false len=2
dls=17 main=true
rand_ok=true'
echo "--- output (rc=$rc) ---"; printf '%s\n' "$out"
if [ "$out" = "$want" ] && [ "$rc" = 0 ]; then
  echo "MATCH: Hashtbl/Domain.DLS/Random run on the fully self-built stdlib (incl. Domain/multicore)"
  exit 0
else
  echo "DIFF: expected"; printf '%s\n' "$want"; exit 1
fi
