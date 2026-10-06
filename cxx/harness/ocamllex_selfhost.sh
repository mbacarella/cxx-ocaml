#!/usr/bin/env bash
# THE SELF-HOSTING FIXPOINT: build the real ocamllex (lex/*.{mli,ml}) with
# c++ocamlc against our ENTIRE self-built (all-ours) stdlib, link with c++ocamlc,
# and use the resulting bytecode ocamllex to regenerate ocamllex's OWN lexer
# (lex/lexer.mll) -- then check the output is byte-identical (modulo the output
# filename in `# line` directives) to the reference lex/ocamllex.opt.
#
# This exercises the whole toolchain (parser, type/kind inference, lambda,
# bytecode gen, linker) AND the whole stdlib, on a real, non-trivial program that
# stresses GC.  A green run means c++ocamlc + the all-ours stdlib reproduce the
# reference compiler's tool, end to end.
set -u
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
CPP=$ROOT/cxx/build/c++ocamlc
RUN=$ROOT/runtime/ocamlrun
STD=$ROOT/stdlib
ORACLE=$ROOT/lex/ocamllex.opt   # reference (native) ocamllex

[ -x "$ORACLE" ] || { echo "SKIP: no reference lex/ocamllex.opt"; exit 0; }

WD=$(mktemp -d) || exit 1
trap 'rm -rf "$WD"' EXIT

gname() { case "$1" in camlinternal*|stdlib) echo "$1";;
  *) cap="$(tr '[:lower:]' '[:upper:]' <<< ${1:0:1})${1:1}"; echo "stdlib__$cap";; esac; }
flags() { case "$1" in camlinternalFormatBasics|stdlib) echo "-nopervasives";; *) echo "";; esac; }
needs_awk() { case "$1" in stdlib|*Labels) return 0;; *) return 1;; esac; }

# Whole stdlib in dependency order (mutex/condition/semaphore hoisted before lazy).
STDORDER="camlinternalFormatBasics stdlib either sys obj type atomic mutex condition \
semaphore camlinternalLazy lazy seq option pair result bool char uchar list int array \
iarray bytes string unit marshal float int32 int64 nativeint lexing parsing repr set \
map stack queue buffer camlinternalFormat printf arg printexc domain fun gc in_channel \
out_channel digest bigarray random hashtbl weak scanf callback camlinternalOO oo \
dynarray format camlinternalMod pqueue ephemeron filename complex effect"
# ocamllex's own modules, in dependency order.
LEXORDER="cset syntax parser lexer table lexgen compact common exhaustiveness output outputbis main"

# Build the all-ours stdlib (.cmi then .cmo).
for phase in cmi cmo; do
  for m in $STDORDER; do
    s=$(gname "$m")
    for ext in mli ml; do
      [ -f "$STD/$m.$ext" ] || continue
      if needs_awk "$m"; then awk -f "$STD/expand_module_aliases.awk" "$STD/$m.$ext" > "$WD/$s.$ext"
      else cp "$STD/$m.$ext" "$WD/$s.$ext"; fi
    done
    if [ "$phase" = cmi ]; then [ -f "$WD/$s.mli" ] || continue; src="$WD/$s.mli"
    else src="$WD/$s.ml"; [ -f "$src" ] || continue; fi
    ( cd "$WD" && "$CPP" -c $(flags "$m") -I "$WD" "$(basename "$src")" ) 2>"$WD/err" \
      || { echo "FAIL: stdlib $(basename "$src")"; sed 's/^/  /' "$WD/err"; exit 1; }
  done
done
cp "$STD/std_exit.ml" "$WD/"; ( cd "$WD" && "$CPP" -c -I "$WD" std_exit.ml ) 2>/dev/null

# Build ocamllex's modules against the all-ours stdlib.
for m in $LEXORDER; do
  for ext in mli ml; do [ -f "$ROOT/lex/$m.$ext" ] && cp "$ROOT/lex/$m.$ext" "$WD/$m.$ext"; done
  for ext in mli ml; do
    [ -f "$WD/$m.$ext" ] || continue
    ( cd "$WD" && "$CPP" -c -I "$WD" "$m.$ext" ) 2>"$WD/err" \
      || { echo "FAIL: lex/$m.$ext"; sed 's/^/  /' "$WD/err"; exit 1; }
  done
done

# Link: all stdlib objs + lex objs + std_exit.
objs=""; for m in $STDORDER; do f="$WD/$(gname "$m").cmo"; [ -f "$f" ] && objs="$objs $f"; done
for m in $LEXORDER; do objs="$objs $WD/$m.cmo"; done
if ! "$CPP" -nopervasives -use-runtime "$RUN" -I "$STD" $objs "$WD/std_exit.cmo" -o "$WD/ocamllex" 2>"$WD/err"; then
  echo "FAIL: link"; sed 's/^/  /' "$WD/err"; exit 1
fi

# Run our ocamllex AND the oracle on lexer.mll; compare (normalising the output
# filename that appears in `# line` directives).
"$RUN" "$WD/ocamllex" -q -o "$WD/ours.ml" "$ROOT/lex/lexer.mll" || { echo "FAIL: our ocamllex rc=$?"; exit 1; }
"$ORACLE" -q -o "$WD/ref.ml" "$ROOT/lex/lexer.mll" 2>/dev/null
sed "s|$WD/ours.ml|OUT|g" "$WD/ours.ml" > "$WD/ours.norm"
sed "s|$WD/ref.ml|OUT|g"  "$WD/ref.ml"  > "$WD/ref.norm"
if diff "$WD/ours.norm" "$WD/ref.norm" >/dev/null; then
  echo "MATCH: self-hosted ocamllex regenerates lexer.mll byte-identical to the oracle (whole toolchain + all-ours stdlib)"
  exit 0
else
  echo "DIFF: self-hosted ocamllex output differs from oracle"; diff "$WD/ours.norm" "$WD/ref.norm" | head; exit 1
fi
