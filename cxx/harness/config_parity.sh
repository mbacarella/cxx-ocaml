#!/usr/bin/env bash
# The configuration cxx/Makefile generates (cxx/src/tools/gen_config.cpp,
# from configure's output) against Config as the tree's ocamlc evaluates it
# (gen_driver_tables.ml's config and linkconfig): Config.print_config's
# variables and the typed values, line for line.
#   config_parity.sh   (O=<build dir>, default cxx/build)
set -u
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
R="$(cd "$(dirname "$SELF")/../.." && pwd -P)"
G="$R/cxx/${O:-build}/generated"
mk=(make -C "$R/cxx" --no-print-directory MODE=debug)
[ -n "${O:-}" ] && mk+=("O=$O")
"${mk[@]}" "${O:-build}/generated/stamp" >/dev/null || exit 2
T=$(mktemp -d)
trap 'rm -rf "${T:?}"' EXIT
cp "$R/cxx/harness/gen_driver_tables.ml" "$T/"
( cd "$T" && "$R/ocamlc.opt" -nostdlib -I "$R/stdlib" -I "$R/compilerlibs" -I "$R/utils" \
    -I "$R/parsing" -I "$R/typing" -I "$R/driver" -I "$R/bytecomp" -I "$R/lambda" -I "$R/file_formats" \
    "$R/compilerlibs/ocamlcommon.cma" "$R/compilerlibs/ocamlbytecomp.cma" gen_driver_tables.ml -o gen ) || exit 2
"$R/runtime/ocamlrun" "$T/gen" config > "$T/table.ocaml"
"$R/runtime/ocamlrun" "$T/gen" linkconfig > "$T/link.ocaml"
tail -n +2 "$G/config_table.inc" > "$T/table.cxx"
# (config_link.inc's file names have no linkconfig counterpart)
tail -n +2 "$G/cppcaml/typing/config_link.inc" | grep -v '^inline const char\* const ' > "$T/link.cxx"
rc=0
for k in table link; do
  if cmp -s "$T/$k.ocaml" "$T/$k.cxx"; then echo "SAME $k ($(($(wc -l < "$T/$k.cxx"))) lines)"
  else echo "DIFF $k"; diff "$T/$k.ocaml" "$T/$k.cxx"; rc=1; fi
done
exit $rc
