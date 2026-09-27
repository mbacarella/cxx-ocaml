#!/usr/bin/env bash
# Driver-options oracle (TYPECHECKER.md, "Driver options"): ocamlc.opt and
# c++ocamlc on the same command lines -- stdout, stderr, the exit code and
# the files each run leaves behind (names, and the bytes of .cmi / .cmo /
# .cma / .dump files) -- byte for byte.  Both are invoked as `ocamlc` (a
# symlink on PATH), so the program name Arg prints is the same, one after
# the other in the same scratch directory, with OCAMLLIB set to the tree's
# stdlib (Config.standard_library).
#
# Usage: cli_parity.sh [case-name-substring]   (V=1: show the diffs)
#
# KNOWN differences (counted apart):
#   config    -config's standard_library_default: c++ocamlc is not
#             installed; its default is the stdlib next to it
#   bin-annot -bin-annot's .cmt (Cmt_format is not ported yet)
set -u
SELF="$(readlink -f "$0")"
ROOT="${ROOT:-$(cd "$(dirname "$SELF")/../.." && pwd)}"
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
T=$(mktemp -d)
trap 'rm -rf "${T:?}"' EXIT
mkdir -p "$T/bin/o" "$T/bin/c" "$T/out"
ln -s "$ROOT/ocamlc.opt" "$T/bin/o/ocamlc"
ln -s "$CPP" "$T/bin/c/ocamlc"
export OCAMLLIB="$ROOT/stdlib"
unset OCAMLPARAM OCAML_COLOR OCAML_ERROR_STYLE NO_COLOR CAMLLIB
FILTER="${1:-}"

# the fixture every case starts from
fixture() {
  cat > a.ml <<'EOF'
let x = 1
let f y = y + x
EOF
  cat > b.ml <<'EOF'
let () = print_int (A.f 41); print_newline ()
EOF
  cat > c.mli <<'EOF'
val g : int -> int
EOF
  cat > c.ml <<'EOF'
let g x = x * 2
EOF
  cat > bad.ml <<'EOF'
let x : int = "no"
EOF
  cat > warn.ml <<'EOF'
let f x = match x with 0 -> 1
EOF
  printf -- '-c\na.ml\n' > args.txt
  printf -- '-c\0a.ml\0' > args0.txt
  printf -- '-w\n+a\n-c\nwarn.ml\n' > wargs.txt
  cat > pp.sh <<'EOF'
#!/bin/sh
sed 's/ZZZ/1/' "$1"
EOF
  chmod +x pp.sh
  cat > z.ml <<'EOF'
let z = ZZZ
EOF
  echo 'int cppcaml_x(int n) { return n + 1; }' > x.c
  echo 'module X = Stdlib' > opened.ml
  echo 'let y = List.length []' > useopen.ml
  cat > sfx.mlx <<'EOF'
val g : int -> int
EOF
  cp c.ml sfx.ml
}

KNOWN_CASES=" config bin-annot "
N=0; SAME=0; DIFF=0; KNOWN=0
declare -a DIFFS=()
# run NAME ARGS...  (ENV=... before the call sets the environment)
run() {
  local name="$1"; shift
  if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then return; fi
  N=$((N + 1))
  local d="$T/w"
  local side
  for side in o c; do
    rm -rf "${d:?}"; mkdir -p "$d"
    ( cd "$d" && fixture && PATH="$T/bin/$side:$PATH" \
        env ${RUN_ENV:-} bash -c 'ocamlc "$@"' ocamlc "$@" >"$T/out/$side.stdout" 2>"$T/out/$side.stderr"
      echo "rc=$?" > "$T/out/$side.rc" )
    ( cd "$d" && for f in $(find . -type f | sort); do
        case "$f" in
          *.cmi|*.cmo|*.cma|*.dump) printf '%s %s\n' "$f" "$(cksum < "$f")" ;;
          *) printf '%s\n' "$f" ;;
        esac
      done ) > "$T/out/$side.files"
  done
  # the external preprocessor's temporary file names are random
  sed -i 's|/tmp/ocamlpp[A-Za-z0-9]*|/tmp/ocamlppXXXXXX|g' "$T/out/o.stderr" "$T/out/c.stderr"
  local ok=1 k
  for k in stdout stderr rc files; do
    cmp -s "$T/out/o.$k" "$T/out/c.$k" || ok=0
  done
  if [ $ok = 1 ]; then SAME=$((SAME + 1))
  elif [[ "$KNOWN_CASES" == *" $name "* ]]; then KNOWN=$((KNOWN + 1)); else
    DIFF=$((DIFF + 1)); DIFFS+=("$name")
    if [ -n "${V:-}" ]; then
      echo "=== DIFF $name: ocamlc $*"
      for k in stdout stderr rc files; do
        cmp -s "$T/out/o.$k" "$T/out/c.$k" || { echo "--- $k"; diff "$T/out/o.$k" "$T/out/c.$k" | head -20; }
      done
    fi
  fi
}

# refuse NAME ARGS...: an option c++ocamlc does not implement is refused
# where its effect would take place -- "option -X is not supported yet",
# exit 2, nothing written (ocamlc is not consulted)
NREF=0; REFOK=0
declare -a REFBAD=()
refuse() {
  local name="$1"; shift
  if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then return; fi
  NREF=$((NREF + 1))
  local d="$T/w"
  rm -rf "${d:?}"; mkdir -p "$d"
  ( cd "$d" && fixture && PATH="$T/bin/c:$PATH" bash -c 'ocamlc "$@"' ocamlc "$@" >"$T/out/r.stdout" 2>"$T/out/r.stderr"
    echo "$?" > "$T/out/r.rc" )
  if [ "$(cat "$T/out/r.rc")" = 2 ] && grep -q "is not supported yet" "$T/out/r.stderr"; then REFOK=$((REFOK + 1))
  else REFBAD+=("$name"); [ -n "${V:-}" ] && { echo "=== REFUSE? $name"; cat "$T/out/r.stderr"; }; fi
}

# ---- the battery ----
# Arg's errors and the usage
run empty
run unknown -foo
run unknown-after-file a.ml -foo
run unknown-eq -foo=bar
run missing-arg -I
run missing-arg-o -o
run missing-w -w
run noarg-eq -c=1
run eq-arg -I=. -c a.ml
run bad-symbol -color bogus
run bad-symbol-stop -stop-after x
run bad-int -match-context-rows x
run good-int -match-context-rows 0x10 -c a.ml
run help -help
run help2 --help
run help-eq -help=1
run bad-w -w foo
run bad-warn-error -warn-error foo
run bad-alert -alert '++'
run unsafe-string -unsafe-string
run vmthread -vmthread
run plugin -plugin x
# information
run version -version
run version2 --version
run vnum -vnum
run where -where
run v -v
run config -config
run config-var -config-var version
run config-var-bool -config-var flambda
run config-var-none -config-var nosuch
run warn-help -warn-help
# files
run dontknow nosuch.xyz
run dontknow-after-c -c a.ml nosuch.xyz
run dash-file - a.ml -c
run dash-missing -
run cmi-without-pack a.cmi
run no-input -c
run no-input-a -a
run a-without-o -a a.ml
# compile / -o / prefixes
run c a.ml -c
run c-mli-ml c.mli c.ml -c
run c-o -c -o foo.cmo a.ml
run c-o-cmi -c -o foo.cmi c.mli
run c-o-dir -c -o sub a.ml
run c-o-multi -c -o foo.cmo a.ml c.ml
run o-link a.ml b.ml -o prog
run default-exe a.ml b.ml
run stop-parsing -stop-after parsing -c a.ml
run stop-typing -stop-after typing -c a.ml
run stop-typing-mli -stop-after typing -c c.mli
run stop-lambda -stop-after lambda a.ml b.ml -o prog
run stop-twice -stop-after typing -stop-after parsing -c a.ml
run stop-same -stop-after typing -stop-after typing -c a.ml
run i -i a.ml
run i-c -i -c a.ml
run i-mli -i c.mli
run impl -c -impl a.ml
run intf -c -intf c.mli
run intf-suffix -c -intf-suffix .mlx sfx.mlx sfx.ml
run cmi-file -c c.mli -o other.cmi -cmi-file other.cmi c.ml
run open -c -open Stdlib a.ml
run open-list -c -open Stdlib -open List useopen.ml
run pp -c -pp ./pp.sh z.ml
run pp-verbose -c -verbose -pp ./pp.sh z.ml
run pp-fails -c -pp false z.ml
run bad-program bad.ml -c
run warnings warn.ml -c
run warn-error-on -warn-error +a -w +a warn.ml -c
run args -args args.txt
run args0 -args0 args0.txt
run args-warn -args wargs.txt
run args-missing -args nosuch.txt
run rest-dashdash -c -- a.ml
# exclusive options
run c-and-a -c -a a.ml -o x.cma
run a-and-pack -a -pack a.cmo -o x.cmo
run i-and-a -i -a a.ml -o x.cma
run output-obj-ext -output-obj a.ml -o x.byte
# -a / -pack
run archive a.ml -a -o lib.cma
run archive-linkall -linkall a.ml -a -o lib.cma
run archive-g -g a.ml -a -o lib.cma
run for-pack -for-pack P -c a.ml
run pack-files -pack -o p.cmo a.ml c.mli c.ml
# dumps
run dump-into-file -c -dump-into-file -dlambda a.ml
run dlambda -c -dlambda a.ml
run drawlambda -c -drawlambda a.ml
run dinstr -c -dinstr a.ml
# OCAMLPARAM / environment
RUN_ENV="OCAMLPARAM=_,w=+a" run ocamlparam-w -c warn.ml
RUN_ENV="OCAMLPARAM=w=+a,_" run ocamlparam-before -c warn.ml
RUN_ENV="OCAMLPARAM=bogus" run ocamlparam-syntax -c a.ml
RUN_ENV="OCAMLPARAM=_,_" run ocamlparam-twice -c a.ml
RUN_ENV="OCAMLPARAM=_,frob=1" run ocamlparam-discard -c a.ml
RUN_ENV="OCAMLPARAM=_,g=2" run ocamlparam-badbool -c a.ml
RUN_ENV="OCAMLPARAM=:g=1:_" run ocamlparam-sep -c a.ml
RUN_ENV="OCAMLPARAM=_,stop-after=zz" run ocamlparam-badpass -c a.ml
RUN_ENV="OCAMLPARAM=_,color=bad" run ocamlparam-badcolor -c a.ml
RUN_ENV="OCAML_COLOR=bad" run env-color -c a.ml
RUN_ENV="OCAML_ERROR_STYLE=bad" run env-error-style -c a.ml
RUN_ENV="OCAML_ERROR_STYLE=short" run env-error-style-short -c bad.ml
run error-style-short -error-style short -c bad.ml
run error-style-contextual -error-style contextual -c bad.ml
run color-never -color never -c bad.ml
run color-always -color always -c bad.ml
run color-always-warn -color always -w +a -c warn.ml
run color-auto -color auto -c bad.ml
RUN_ENV="OCAML_COLOR=always" run env-color-always -c bad.ml
RUN_ENV="NO_COLOR=1" run no-color -c bad.ml
RUN_ENV="NO_COLOR=1" run no-color-explicit -color always -c bad.ml
RUN_ENV="OCAMLPARAM=_,color=always" run ocamlparam-color -c bad.ml
# typing flags (accepted and effective)
run flags-dune -w @1..3@5..28@30..39@43@46..47@49..57@61..62@67@69@40-41-42-44-45-48-58-59-60-66 \
  -strict-sequence -strict-formats -short-paths -keep-locs -g -c a.ml
run bin-annot -bin-annot -c a.ml
run no-alias-deps -no-alias-deps -c a.ml
run safe-string -safe-string -c a.ml
run principal -principal -rectypes -nolabels -c a.ml
run nostdlib -nostdlib -nopervasives -c a.ml
run nocwd -nocwd -c a.ml
# more flags with an effect
run keywords -keywords 5.2 -c a.ml
run keywords-extra -keywords 5.2+foo -c a.ml
run keywords-bad -keywords bad -c a.ml
run hidden -H . -c b.ml
run thread -thread -c a.ml
run linkall-link -linkall a.ml b.ml -o prog
run noautolink -noautolink a.ml b.ml -o prog
run without-runtime -without-runtime a.ml b.ml -o prog
run use-prims -use-prims nosuch -c a.ml
run no-check-prims -no-check-prims a.ml b.ml -o prog
run cc -cc gcc -c a.ml
run custom-compile -custom -c a.ml
run dllib-compile -dllib -lfoo -c a.ml
run set-runtime-default-bad -set-runtime-default foo
run set-runtime-default-unknown -set-runtime-default foo=bar
run launch-method-bad -launch-method bad
# refused where the effect would take place
refuse annot -annot -c a.ml
refuse dtypes -dtypes -c a.ml
refuse ppx -ppx ./pp.sh -c a.ml
refuse dsource -dsource -c a.ml
refuse dtypedtree -dtypedtree -c a.ml
refuse dshape -dshape -c a.ml
refuse dmatchcomp -dmatchcomp -c a.ml
refuse dcanonical-ids -dcanonical-ids -c a.ml
refuse compat-32 -compat-32 -c a.ml
refuse dtimings -dtimings -c a.ml
refuse depend -depend a.ml
# the linker's (Bytelink / Bytelibrarian / Dll / Ccomp ports)
run dllib-link -dllib -lfoo a.ml b.ml -o prog
run dllpath -dllpath /x a.ml b.ml -o prog
run cclib-link -cclib -lfoo a.ml b.ml -o prog
run bytecode-hints -bytecode-hints a.ml b.ml -o prog
run runtime-search -runtime-search enable a.ml b.ml -o prog
run launch-method -launch-method sh a.ml b.ml -o prog
run launch-method-exe -launch-method exe a.ml b.ml -o prog
run set-runtime-default -set-runtime-default standard_library_default=/x a.ml b.ml -o prog
run cclib-archive -cclib -lfoo a.ml -a -o lib.cma
run custom-archive -custom a.ml -a -o lib.cma
run ccopt-dllib-archive -ccopt -O2 -dllib -lbar a.ml -a -o lib.cma
run c-file -c x.c
run c-file-o -c x.c -o y.o
# the C-toolchain links need an installed stdlib (caml/*.h, libcamlrun.a)
INST="${INST:-/tmp/cxxsw/usr/local/lib/ocaml}"
if [ -f "$INST/caml/mlvalues.h" ]; then
  RUN_ENV="OCAMLLIB=$INST" run custom -custom a.ml b.ml -o prog
  RUN_ENV="OCAMLLIB=$INST" run output-obj -output-obj a.ml b.ml -o prog.o
  RUN_ENV="OCAMLLIB=$INST" run output-obj-c -output-obj a.ml b.ml -o prog.c
  RUN_ENV="OCAMLLIB=$INST" run output-complete-exe -output-complete-exe a.ml b.ml -o prog
  RUN_ENV="OCAMLLIB=$INST" run make-runtime -make-runtime a.ml -o rt
else
  echo "(the C-toolchain cases skipped: no installed stdlib at INST=$INST)"
fi

echo "cases $N: SAME $SAME  DIFF $DIFF  KNOWN $KNOWN"
echo "refused $NREF: as expected $REFOK"
for n in "${REFBAD[@]:-}"; do [ -n "$n" ] && echo "REFUSE-FAILED $n"; done
for n in "${DIFFS[@]:-}"; do [ -n "$n" ] && echo "DIFF $n"; done
