#!/usr/bin/env bash
# Bisect the bootstrapped-ocamlc startup crash: link std + the first N compiler
# .cmo objects + a trivial `let () = exit 0` main, and report the run rc.  Every
# linked module's init runs at startup before main, so a crash with prefix N but
# not N-1 localizes the bad module to position N.  Usage: WD=... bisect.sh N
set -u
SELF="$(readlink -f "$0")"; cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
LINK=$ROOT/cxx/build/c++link
RUN=$ROOT/runtime/ocamlrun
CPP=$ROOT/cxx/build/c++ocamlc
WD=${WD:?set WD to a KEEP=1 bootstrap dir}
N=${1:?usage: bisect.sh N}

STDORDER="camlinternalFormatBasics stdlib either sys obj type atomic mutex condition \
semaphore camlinternalLazy lazy seq option pair result bool char uchar list int array \
iarray bytes string unit marshal float int32 int64 nativeint lexing parsing repr set \
map stack queue buffer camlinternalFormat printf arg printexc domain fun gc in_channel \
out_channel digest bigarray random hashtbl weak scanf callback camlinternalOO oo \
dynarray format camlinternalMod pqueue ephemeron filename complex effect"
gname() { case "$1" in camlinternal*|stdlib) echo "$1";;
  *) cap="$(tr '[:lower:]' '[:upper:]' <<< ${1:0:1})${1:1}"; echo "stdlib__$cap";; esac; }

# compiler module order = .ml basenames present as .cmo in WD, in the order file
ORDER_FILE=$WD/.cl_order
if [ ! -f "$ORDER_FILE" ]; then echo "need $ORDER_FILE (list of compiler module basenames in link order)"; exit 1; fi
mapfile -t CL < "$ORDER_FILE"

echo 'let () = print_string "init-ok\n"; exit 0' > "$WD/zmain.ml"
( cd "$WD" && "$CPP" -c -I "$WD" zmain.ml ) 2>/dev/null

stdobjs=""; for m in $STDORDER; do f="$WD/$(gname "$m").cmo"; [ -f "$f" ] && stdobjs="$stdobjs $f"; done
clobjs=""; i=0
for n in "${CL[@]}"; do [ $i -ge "$N" ] && break; clobjs="$clobjs $WD/$n.cmo"; i=$((i+1)); done
last="${CL[$((N-1))]}"

"$LINK" -nostdlib -runtime "$RUN" $stdobjs $clobjs "$WD/zmain.cmo" -o "$WD/zboot" 2>"$WD/zlerr" \
  || { echo "N=$N link FAIL (last=$last)"; sed 's/^/  /' "$WD/zlerr" | head; exit 2; }
"$RUN" "$WD/zboot" >/dev/null 2>&1; rc=$?
echo "N=$N last=$last rc=$rc"
