#!/usr/bin/env bash
# A reference OCaml configured WITH zstd, and a c++ocamlc configured for it,
# to check c++ocamlc's compressed Marshal (Compression.output_value: .cmi,
# .cmt, the .cmo debug / hint sections, -pack, the linker reading them)
# against ocamlc byte for byte -- real opam switches are built with zstd.
#
#   1. a git worktree of HEAD at $DIR (default /tmp/ocaml-zstd), configured
#      with zstd (the system's libzstd, which configure finds), make world.opt;
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
# Environment: DIR=, CXX= (the C++ compiler, default g++).
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
DIR="${DIR:-/tmp/ocaml-zstd}"
CXX_COMPILER="${CXX:-g++}"
vm_limit 16000000

if [ ! -x "$DIR/ocamlc.opt" ]; then
  git -C "$ROOT" worktree add --detach "$DIR" HEAD
  ( cd "$DIR" &&
    ./configure --disable-ocamldoc --disable-ocamltest > configure.out &&
    grep -q "^#define HAS_ZSTD 1" runtime/caml/s.h &&
    make -j12 world.opt > make.out 2>&1 )
fi
( cd "$DIR" && cxx/harness/gen_driver_tables.sh )
grep -q "compression_supported = true" "$DIR/cxx/include/cppcaml/typing/config_link.inc" ||
  { echo "zstd_reference.sh: $DIR's runtime has no zstd" >&2; exit 2; }
# (libzstd: configure's ZSTD_LIBS, the runtime's)
make -C "$DIR/cxx" -j12 CXX="$CXX_COMPILER" c++ocamlc c++typing-dump
echo "ready: $DIR (ocamlc.opt with zstd; cxx/build-release/c++ocamlc configured for it)"
