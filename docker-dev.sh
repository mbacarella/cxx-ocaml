#!/usr/bin/env bash
#
# Start the c++caml dev container and drop into an interactive shell.
#
# Builds the `cppcaml` image if it's missing, bind-mounts this repo (so the host
# and container never drift) and your ~/.claude (so Claude Code auth + this
# project's memory persist), then enters a bash prompt inside the Nix dev shell
# with clang/cmake/ocaml/menhir + the `claude` CLI on PATH.
#
#   ./docker-dev.sh              # interactive shell in the container
#   ./docker-dev.sh --build      # force a rebuild of the image first
#   ./docker-dev.sh claude       # run a one-off command instead of a shell
#   ./docker-dev.sh bash -c 'JOBS=8 bash ./cxx/harness/lambda_parity.sh'
#
set -euo pipefail

IMAGE=cppcaml
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONTAINER_REPO=/home/mbac/code/c++caml

# Optional --build / -b: (re)build the image before running.
build=0
if [ "${1:-}" = "--build" ] || [ "${1:-}" = "-b" ]; then build=1; shift; fi

if [ "$build" -eq 1 ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo ">> building $IMAGE image (first run takes a while)..." >&2
  docker build -t "$IMAGE" "$REPO_DIR"
fi

# Bind-mount the repo. Also persist Claude Code state from the host: the
# ~/.claude DIRECTORY (credentials, projects, sessions, memory) *and* the
# ~/.claude.json FILE (onboarding, project list, history) -- both are needed or
# Claude Code starts as a brand-new install.
mounts=( -v "$REPO_DIR:$CONTAINER_REPO" )
if [ -d "$HOME/.claude" ]; then
  mounts+=( -v "$HOME/.claude:/home/mbac/.claude" )
fi
if [ -f "$HOME/.claude.json" ]; then
  mounts+=( -v "$HOME/.claude.json:/home/mbac/.claude.json" )
fi

# Pass the host UID/GID so the entrypoint drops to a matching user -- files
# written to the mounts come out owned by you, not root.
ids=( -e "HOST_UID=$(id -u)" -e "HOST_GID=$(id -g)" )

# Allocate a TTY only when stdin/stdout actually are one.
tty_flags=( -i )
if [ -t 0 ] && [ -t 1 ]; then tty_flags=( -i -t ); fi

# No args -> the image's default CMD (an interactive bash inside the dev shell).
exec docker run --rm "${tty_flags[@]}" "${mounts[@]}" "${ids[@]}" "$IMAGE" "$@"
