# Shared freshness guard against the stale-binary trap.
#
# The parity/DDC/bootstrap harnesses run `cxx/build/c++*` binaries.  Those are a
# SEPARATE build from `cxx/build-release/` (the one iterated on by hand), so a
# `make -C cxx <tool>` + `cp` of one binary leaves the OTHERS stale:
# on 2026-07-12/13 two of the `cxx/build/` tools sat 5 days behind source
# while the gates reported green -- a two-day vacuous pass that hid live
# segfaults.  This guard makes that impossible: before a harness uses a
# `cxx/build` binary, it re-runs make (cxx/Makefile tracks the precise source
# dependencies) so the binary always reflects current `cxx/src`/`cxx/include`.  A
# rebuild failure ABORTS the harness rather than let it test a stale binary.
#
# Usage (source, then call with the tool(s) the harness needs):
#     source "$(dirname "${BASH_SOURCE[0]}")/_require_fresh.sh"
#     require_fresh c++ocamlc
#
# Escape hatch: CPPCAML_SKIP_FRESH=1 skips the check (e.g. bisecting a
# hand-built binary).  It prints a loud warning so a skip is never silent.

require_fresh() {
  if [ -n "${CPPCAML_SKIP_FRESH:-}" ]; then
    echo "[require_fresh] WARNING: CPPCAML_SKIP_FRESH set -- NOT verifying $* is built from current source." >&2
    return 0
  fi
  local root build
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  build="$root/cxx/build"
  # cxx/Makefile tracks the real source deps, so this is a no-op when
  # already current and a rebuild when any dependency changed.  Either way
  # the binary below is guaranteed to match cxx/src / cxx/include afterward.
  if ! make -C "$root/cxx" --no-print-directory MODE=debug "$@" >&2; then
    echo "[require_fresh] FATAL: 'make -C cxx MODE=debug $*' FAILED -- refusing to run a harness on a stale/broken binary." >&2
    exit 3
  fi
  local b
  for b in "$@"; do
    [ -x "$build/$b" ] || { echo "[require_fresh] FATAL: $build/$b missing after build." >&2; exit 3; }
  done
}
