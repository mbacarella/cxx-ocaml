#!/usr/bin/env bash
# Diverse Double-Compiling (Wheeler) verification for the STANDARD LIBRARY.
#
# The compiler DDC (cxx/harness/ddc.sh) proves the OCaml *compiler* bytecode
# corresponds to its source but leaves the stdlib SHARED (unverified).  This
# harness closes most of that gap: it rebuilds the stdlib with a DIVERSE compiler
# and diffs every artifact against the officially-built one.
#
#   REF : official bytecode ./ocamlc   (the compiler that actually built the
#                                        shipped stdlib during `make world`)
#   DIV : S2                            (the bytecode ocamlc that c++ocamlc
#                                        bootstrapped, then rebuilt once; its
#                                        compiler .cmo are DDC-proven identical
#                                        to the official ones)
#
# IMPORTANT -- REF must be the BYTECODE ./ocamlc, NOT ocamlc.opt.  The stdlib is
# built with -g, and native (ocamlc.opt) vs bytecode ocamlc emit DIFFERENT -g
# debug sections (~124KB differ for list.cmo), a native/bytecode artifact that
# has nothing to do with source fidelity.  Against the bytecode reference, S2
# reproduces list.cmo bit-identically even with -g.
#
# The module-alias files are preprocessed (`-pp awk -f expand_module_aliases.awk`,
# stdlib/Compflags): S2 runs the same -pp command as the official build, so
# stdlib.cmi, stdlib.cmo and the *Labels.cmi are rebuilt and compared like every
# other artifact.  awk, like the C toolchain, is trusted.
#
# Getting S2: run `make ddc`; it prints and LEAVES S2=/tmp/ddc_s2.XXXXXX.
#   S2DIR=/tmp/ddc_s2.XXXXXX bash cxx/harness/stdlib_ddc.sh
set -u
set -o pipefail

SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
RUN=$ROOT/runtime/ocamlrun
OFFBC=$ROOT/ocamlc          # official bytecode compiler (the DDC reference)

die() { echo "FATAL: $*" >&2; exit 1; }
for t in "$RUN" "$OFFBC"; do [ -e "$t" ] || die "missing tool: $t"; done
[ -n "${S2DIR:-}" ] || die "set S2DIR=<diverse S2 dir> (run 'make ddc' first; it prints S2=...)"
[ -f "$S2DIR/ocamlc" ] || die "no S2 ocamlc at $S2DIR/ocamlc"
"$RUN" "$S2DIR/ocamlc" -version >/dev/null 2>&1 || die "S2 ocamlc does not run"
S2="$RUN $S2DIR/ocamlc"
echo "REF = $RUN $OFFBC (official bytecode)"
echo "DIV = $S2 (diverse S2)"

# --- exact stdlib flags (stdlib/Makefile COMPFLAGS + stdlib/Compflags) --------
COMPFLAGS="-strict-sequence -absname -w +a-4-9-41-42-44-45-48 -g -warn-error +A \
-alert @ocaml_deprecated_cli -bin-annot -nostdlib -principal"
# per-target extra flags (stdlib/Compflags, as the REF make applies them)
# the module-alias files' preprocessor (stdlib/Compflags; AWK from Makefile.config)
AWK=$(sed -n 's/^AWK *= *//p' "$ROOT/Makefile.config"); AWK=${AWK:-awk}
PP="-pp \"$AWK -f ./expand_module_aliases.awk\""
compflags() {   # $1 = output basename (gname'd)   $2 = ext (cmi|cmo)
  case "$1.$2" in
    camlinternalFormatBasics.cmi|camlinternalFormatBasics.cmo) echo "-nopervasives";;
    stdlib__Buffer.cmi|stdlib__Buffer.cmo)                     echo "-w +A";;
    camlinternalFormat.cmi|camlinternalFormat.cmo)             echo "-w +A -w -fragile-match";;
    stdlib__Printf.cmi|stdlib__Printf.cmo|stdlib__Format.cmi|stdlib__Format.cmo|stdlib__Scanf.cmi|stdlib__Scanf.cmo) echo "-w +A -w -fragile-match";;
    stdlib__ArrayLabels.cmo|stdlib__ListLabels.cmo|stdlib__BytesLabels.cmo|stdlib__StringLabels.cmo|stdlib__MoreLabels.cmo|stdlib__StdLabels.cmo) echo "-nolabels -no-alias-deps";;
    stdlib__Float.cmo)              echo "-nolabels -no-alias-deps";;
    stdlib__Oo.cmi)                 echo "-no-principal";;
    stdlib.cmi|stdlib.cmo)          echo "-nopervasives -no-alias-deps -w -49 $PP";;
    stdlib__*Labels.cmi)            echo "$PP";;
    *)                              echo "";;
  esac
}
gname() { case "$1" in camlinternal*|stdlib|std_exit) echo "$1";;
                       *) c="$(tr '[:lower:]' '[:upper:]' <<< "${1:0:1}")${1:1}"; echo "stdlib__$c";; esac; }
MODS=$(awk '
  /^STDLIB_MODULE_BASENAMES/ {inb=1}
  inb { cont=($0 ~ /\\[ \t]*$/); line=$0
        gsub(/=/,"",line); gsub(/\\/,"",line); gsub(/STDLIB_MODULE_BASENAMES/,"",line)
        n=split(line,a," "); for(i=1;i<=n;i++) if(a[i]!="") print a[i]
        if(!cont) exit }' stdlib/StdlibModules)
[ -n "$MODS" ] || die "could not parse STDLIB_MODULE_BASENAMES"

# --- back up the real stdlib artifacts; ALWAYS restore ------------------------
BK=$(mktemp -d /tmp/stdlib_ddc_bk.XXXXXX)
cp -p "$ROOT"/stdlib/*.cm* "$ROOT"/stdlib/*.o "$ROOT"/stdlib/*.a "$BK"/ 2>/dev/null
restore() {
  rm -f "$ROOT"/stdlib/*.cmi "$ROOT"/stdlib/*.cmo "$ROOT"/stdlib/*.cmt "$ROOT"/stdlib/*.cmti 2>/dev/null
  cp -p "$BK"/*.cm* "$ROOT"/stdlib/ 2>/dev/null
  echo "restored stdlib artifacts from $BK"
}
trap restore EXIT
echo "backed up $(ls "$BK" | wc -l) stdlib artifacts to $BK"

# --- REF pass: build the whole stdlib with the OFFICIAL bytecode compiler -----
echo "== REF: building full stdlib with official bytecode ./ocamlc =="
rm -f "$ROOT"/stdlib/*.cmi "$ROOT"/stdlib/*.cmo "$ROOT"/stdlib/*.cmt "$ROOT"/stdlib/*.cmti
make -C "$ROOT/stdlib" -s USE_BOOT_OCAMLC=1 BOOT_OCAMLC="$RUN $OFFBC" OCAMLRUN="$RUN" \
     stdlib.cma std_exit.cmo >"$BK/ref.log" 2>&1 || { sed 's/^/  /' "$BK/ref.log" | tail -15; die "REF stdlib build failed"; }
REFOUT=$(mktemp -d /tmp/stdlib_ddc_ref.XXXXXX)
cp -p "$ROOT"/stdlib/*.cmi "$ROOT"/stdlib/*.cmo "$REFOUT"/ 2>/dev/null
echo "  REF built $(ls "$REFOUT"/*.cmo 2>/dev/null | wc -l) .cmo / $(ls "$REFOUT"/*.cmi 2>/dev/null | wc -l) .cmi"

# --- DIV pass: rebuild each artifact with S2, compare to REF ------------------
# Each module is rebuilt in the SAME interface environment REF used: all official
# cmis are present (from REFOUT), and after comparing a module we restore its
# official artifacts before the next one.
echo "== DIV: rebuilding every artifact with S2 =="
cp -p "$REFOUT"/* "$ROOT"/stdlib/ 2>/dev/null
cd "$ROOT/stdlib"
cis=0 cid=0 cos=0 cod=0 cidl="" codl=""
try_build() {   # $1=basename $2=g $3=ext(cmi|cmo) $4=srcfile
  local b="$1" g="$2" ext="$3" src="$4"
  local out="$g.$ext"
  local extra
  extra=$(compflags "$g" "$ext")
  rm -f "$out"
  if [ "$g" = "$b" ]; then   # non-prefixed: module name from filename, no -o
    eval "$S2 $COMPFLAGS $extra -c $src" >/dev/null 2>&1
  else
    eval "$S2 $COMPFLAGS $extra -o $out -c $src" >/dev/null 2>&1
  fi
}
for b in $MODS std_exit; do
  g=$(gname "$b")
  # .cmi (only if a .mli exists)
  if [ -f "$b.mli" ]; then
    if try_build "$b" "$g" cmi "$b.mli" && [ -f "$g.cmi" ]; then
      if cmp -s "$g.cmi" "$REFOUT/$g.cmi"; then cis=$((cis+1)); else cid=$((cid+1)); cidl="$cidl $g.cmi"; fi
    else cid=$((cid+1)); cidl="$cidl $g.cmi(S2-fail)"; fi
    cp -p "$REFOUT/$g.cmi" "$g.cmi" 2>/dev/null   # restore official for deps
  fi
  # .cmo
  if try_build "$b" "$g" cmo "$b.ml" && [ -f "$g.cmo" ]; then
    if cmp -s "$g.cmo" "$REFOUT/$g.cmo"; then cos=$((cos+1)); else cod=$((cod+1)); codl="$codl $g.cmo"; fi
  else cod=$((cod+1)); codl="$codl $g.cmo(S2-fail)"; fi
  cp -p "$REFOUT/$g.cmo" "$g.cmo" 2>/dev/null
  cp -p "$REFOUT/$g.cmi" "$g.cmi" 2>/dev/null   # some modules emit .cmi from .ml
done
cd "$ROOT"

echo "=============================================================="
echo "STDLIB DDC RESULT:  cmi same=$cis diff=$cid  |  cmo same=$cos diff=$cod"
[ -n "$cidl" ]  && echo "  cmi DIFF:$cidl"
[ -n "$codl" ]  && echo "  cmo DIFF:$codl"
echo "  REF=$REFOUT  S2=$S2DIR"
echo "=============================================================="
if [ "$cid" -eq 0 ] && [ "$cod" -eq 0 ] && [ $((cis+cos)) -gt 0 ]; then
  echo "PASS: diverse S2 reproduces $cis .cmi + $cos .cmo of the stdlib bit-identically"
  echo "      to the official bytecode compiler (every artifact, none skipped)."
  exit 0
else
  echo "FAIL: stdlib divergence (cmi diff=$cid cmo diff=$cod)."
  exit 1
fi
