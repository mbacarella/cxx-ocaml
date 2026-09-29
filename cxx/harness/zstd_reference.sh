#!/usr/bin/env bash
# A reference OCaml configured WITH zstd, and a c++ocamlc configured for it,
# to check c++ocamlc's compressed Marshal (Compression.output_value: .cmi,
# .cmt, the .cmo debug / hint sections, -pack, the linker reading them)
# against ocamlc byte for byte -- real opam switches are built with zstd.
#
#   1. a git worktree of HEAD at $DIR (default /tmp/ocaml-zstd), configured
#      like this tree plus zstd (PKG_CONFIG_PATH -> libzstd), make world.opt;
#   2. its driver tables regenerated there (gen_driver_tables.sh answers
#      compression_supported from that tree's own runtime) and its
#      c++ocamlc built against the SAME libzstd.
# Then run that worktree's harnesses (their ROOT is the worktree), with the
# shared /tmp caches kept apart, e.g.:
#   cd $DIR && JOBS=12 DUMP=cmo cxx/harness/lambda_port_parity.sh
#   cd $DIR && REF=/tmp/effid_ref_z cxx/harness/effid.sh
#   cd $DIR && REF=/tmp/effid_ref_z cxx/harness/cmi_port_parity.sh
#   cd $DIR && TOOLS=/tmp/cmt_parity_tools_z cxx/harness/cmt_parity.sh --stdlib
#   cd $DIR && G=-g cxx/harness/pack_parity.sh
#   cd $DIR && make install DESTDIR=/tmp/cxxsw_z &&
#     INST=/tmp/cxxsw_z/usr/local/lib/ocaml cxx/harness/link_parity.sh
#   cd $DIR && TOOLS=/tmp/cmt_parity_tools_z STAGE=/tmp/dune_parity_stage_z \
#     OUT=/tmp/dune_parity_z cxx/harness/dune_parity.sh
#   cd $DIR && CPP=$DIR/cxx/build-release/c++ocamlc CACHE=/tmp/exec_oracle_cache_z \
#     cxx/harness/exec_parity.sh
# Environment: DIR=, ZSTD=<zstd store path>, ZSTD_DEV=<its -dev output>,
#   MIMALLOC=, MIMALLOC_DEV= (default: found in /nix/store), CXX= (the C++
#   compiler, default the one cxx/build-release was configured with).
set -euo pipefail
SELF="$(readlink -f "$0")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
DIR="${DIR:-/tmp/ocaml-zstd}"
pick() { ls -d /nix/store/*-"$1" 2>/dev/null | head -1; }
ZSTD="${ZSTD:-$(pick 'zstd-1.5.7')}"
ZSTD_DEV="${ZSTD_DEV:-$(pick 'zstd-1.5.7-dev')}"
MIMALLOC="${MIMALLOC:-$(pick 'mimalloc-3.4.5')}"
MIMALLOC_DEV="${MIMALLOC_DEV:-$(pick 'mimalloc-3.4.5-dev')}"
for v in ZSTD ZSTD_DEV MIMALLOC MIMALLOC_DEV; do
  [ -d "${!v}" ] || { echo "zstd_reference.sh: $v not found (set $v=, e.g. nix build nixpkgs#zstd.dev)" >&2; exit 2; }
done
CXX_COMPILER="${CXX:-$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$ROOT/cxx/build-release/CMakeCache.txt")}"
ulimit -v 16000000

if [ ! -x "$DIR/ocamlc.opt" ]; then
  git -C "$ROOT" worktree add --detach "$DIR" HEAD
  ( cd "$DIR" &&
    PKG_CONFIG_PATH="$ZSTD_DEV/lib/pkgconfig" ./configure CC=clang --disable-ocamldoc --disable-ocamltest \
      LDFLAGS="-Wl,-rpath,$ZSTD/lib" > configure.out &&
    grep -q "^#define HAS_ZSTD 1" runtime/caml/s.h &&
    make -j12 world.opt > make.out 2>&1 )
fi
( cd "$DIR" && cxx/harness/gen_driver_tables.sh )
grep -q "compression_supported = true" "$DIR/cxx/include/cppcaml/typing/config_link.inc" ||
  { echo "zstd_reference.sh: $DIR's runtime has no zstd" >&2; exit 2; }
cmake -S "$DIR/cxx" -B "$DIR/cxx/build-release" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
  "-DCMAKE_PREFIX_PATH=$MIMALLOC;$MIMALLOC_DEV;$ZSTD;$ZSTD_DEV" > /dev/null
ninja -C "$DIR/cxx/build-release" c++ocamlc c++typing-dump
echo "ready: $DIR (ocamlc.opt with zstd; cxx/build-release/c++ocamlc configured for it)"
