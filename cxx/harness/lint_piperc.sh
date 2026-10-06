#!/usr/bin/env bash
# Lint guard against the pipe-rc antipattern in the bootstrap/DDC harnesses.
#
# The bug (bit us repeatedly, parts 26/27):
#
#     ( ocamlc ... | tail -3 ); echo rc=$?
#
# reports `tail`'s exit status (0), NOT ocamlc's -- so a compile that FAILED is
# read as a PASS.  The fix is to capture the real rc before the pipe, use
# ${PIPESTATUS[0]}, or run the pipeline under `set -o pipefail`.
#
# This guard flags any harness that pipes a command and then reads `$?`/`rc=$?`
# WITHOUT a `set -o pipefail` in the file.  Exit 1 on any finding.
set -u
cd "$(dirname "$0")" || exit 1
. ./portable.sh

rc=0
for f in ./*.sh; do
  [ "$(basename "$f")" = "lint_piperc.sh" ] && continue
  # A file that opts into pipefail is safe by construction.
  grep -qE '^[[:space:]]*set[[:space:]]+-o[[:space:]]+pipefail' "$f" && continue
  # Otherwise flag `... | ... ); echo ... $?` or `; ... rc=$?` right after a pipe.
  hits=$(grep -nE '\|.*\)[[:space:]]*;[[:space:]]*(echo[[:space:]]+)?rc=\$\?|\|.*;[[:space:]]*echo[[:space:]]+rc=\$\?' "$f")
  if [ -n "$hits" ]; then
    echo "PIPE-RC RISK in $f (no 'set -o pipefail'):"
    sed 's/^/    /' <<< "$hits"
    rc=1
  fi
done

if [ "$rc" = 0 ]; then
  echo "lint_piperc: clean (no unguarded pipe-rc patterns)."
fi
exit $rc
