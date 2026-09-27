# Shared freshness guard against the stale-binary trap.
#
# The parity/DDC/bootstrap harnesses run `cxx/build/c++*` binaries.  Those are a
# SEPARATE build from `cxx/build-release/` (the one iterated on by hand), so a
# `ninja -C cxx/build-release ...` + `cp` of one binary leaves the OTHERS stale:
# on 2026-07-12/13 two of the `cxx/build/` tools sat 5 days behind source
# while the gates reported green -- a two-day vacuous pass that hid live
# segfaults.  This guard makes that impossible: before a harness uses a
# `cxx/build` binary, it re-runs ninja (which does precise source-dependency
# tracking) so the binary always reflects current `cxx/src`/`cxx/include`.  A
# rebuild failure ABORTS the harness rather than let it test a stale binary.
#
# Usage (source, then call with the ninja target(s) the harness needs):
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
  if [ ! -f "$build/build.ninja" ]; then
    echo "[require_fresh] FATAL: $build is not a ninja build dir; cannot verify freshness of $*." >&2
    exit 3
  fi
  if ! command -v ninja >/dev/null 2>&1; then
    echo "[require_fresh] FATAL: ninja not found; cannot verify $* is current." >&2
    exit 3
  fi
  # ninja tracks the real source deps, so this is a no-op (~0.5s) when already
  # current and a full rebuild when any dependency changed.  Either way the
  # binary below is guaranteed to match cxx/src / cxx/include afterward.
  if ! ninja -C "$build" "$@" >&2; then
    echo "[require_fresh] FATAL: 'ninja -C cxx/build $*' FAILED -- refusing to run a harness on a stale/broken binary." >&2
    exit 3
  fi
  local b
  for b in "$@"; do
    [ -x "$build/$b" ] || { echo "[require_fresh] FATAL: $build/$b missing after build." >&2; exit 3; }
  done
}
