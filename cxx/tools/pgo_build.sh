#!/usr/bin/env bash
# Build the release c++ocamlc with profile-guided optimisation + ThinLTO.
#
#   1. an instrumented build (CPPCAML_PGO_GENERATE) in cxx/build-pgo-gen;
#   2. training: cxx/harness/bench.sh's corpora (startup, small, compiler,
#      stdlib) compiled once by it -- real compiles of real code;
#   3. llvm-profdata merge;
#   4. the target build (default cxx/build-release) reconfigured with
#      CPPCAML_LTO and CPPCAML_PGO_PROFILE, and rebuilt.
# The profile belongs to the sources it was collected on: rerun after
# changing them (a stale profile still builds, only less well).
#
# Usage: cxx/tools/pgo_build.sh [build-dir]
# Environment:
#   LLVM_PROFDATA=   llvm-profdata of the SAME major version as the C++
#                    compiler (raw profiles are version-specific); default:
#                    the first llvm-profdata on PATH whose version matches,
#                    e.g. from `nix build nixpkgs#llvmPackages_20.llvm`
#   CMAKE_PREFIX_PATH  as for the normal build (mimalloc; compiler-rt's
#                    profile runtime for step 1, found there too)
#   EXTRA_CMAKE=     more -D options for both builds (e.g. -DCMAKE_AR=...)
set -euo pipefail
SELF="$(readlink -f "$0")"
CXXDIR="$(cd "$(dirname "$SELF")/.." && pwd)"
ROOT="$(cd "$CXXDIR/.." && pwd)"
TARGET="$(readlink -m "${1:-$CXXDIR/build-release}")"
GEN="$CXXDIR/build-pgo-gen"
PROF="$CXXDIR/build-pgo-profile"
CXX_COMPILER="${CXX:-clang++}"
if [ -f "$TARGET/CMakeCache.txt" ]; then
  CXX_COMPILER=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$TARGET/CMakeCache.txt")
fi
major=$("$CXX_COMPILER" --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)
[ -n "$major" ] || { echo "pgo_build.sh: $CXX_COMPILER is not clang" >&2; exit 2; }
if [ -z "${LLVM_PROFDATA:-}" ]; then
  for c in $(type -ap llvm-profdata "llvm-profdata-$major" 2>/dev/null); do
    if "$c" --version 2>/dev/null | grep -q "LLVM version $major\."; then LLVM_PROFDATA=$c; break; fi
  done
fi
[ -n "${LLVM_PROFDATA:-}" ] || {
  echo "pgo_build.sh: no llvm-profdata $major.x (set LLVM_PROFDATA=; e.g. nix build nixpkgs#llvmPackages_$major.llvm)" >&2
  exit 2
}
extra=(${EXTRA_CMAKE:-})
ulimit -v 32000000

echo "== 1. instrumented build ($GEN)"
cmake -S "$CXXDIR" -B "$GEN" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
  -DCPPCAML_PGO_GENERATE=ON -DCPPCAML_LTO=OFF -DCPPCAML_PGO_PROFILE= "${extra[@]}" >/dev/null
ninja -C "$GEN" c++ocamlc

echo "== 2. training (bench.sh corpora)"
rm -rf "$PROF"; mkdir -p "$PROF"
CPPCAML_NO_FASTEXIT=1 LLVM_PROFILE_FILE="$PROF/c-%m.profraw" CPP="$GEN/c++ocamlc" REPS=1 \
  OUT="$PROF/bench" "$ROOT/cxx/harness/bench.sh" >/dev/null 2>&1 || true
ls "$PROF"/*.profraw >/dev/null 2>&1 || { echo "pgo_build.sh: training wrote no profile" >&2; exit 2; }

echo "== 3. merge"
"$LLVM_PROFDATA" merge -o "$PROF/c++ocamlc.profdata" "$PROF"/*.profraw

echo "== 4. optimised build ($TARGET)"
cmake -S "$CXXDIR" -B "$TARGET" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
  -DCPPCAML_PGO_GENERATE=OFF -DCPPCAML_LTO=ON -DCPPCAML_PGO_PROFILE="$PROF/c++ocamlc.profdata" "${extra[@]}" >/dev/null
ninja -C "$TARGET"
echo "done: $TARGET (profile $PROF/c++ocamlc.profdata)"
