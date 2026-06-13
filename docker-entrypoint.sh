#!/usr/bin/env bash
#
# Container entrypoint for the cppcaml dev image. Runs the given command inside
# the Nix dev shell. If HOST_UID is set (and non-zero), it drops to a user with
# that UID/GID so files written to the bind-mounted repo and ~/.claude are owned
# by you on the host rather than root -- while the root nix-daemon still owns the
# multi-user store. With HOST_UID unset/0 it just runs as root (no remap).
#
set -e

uid="${HOST_UID:-0}"
gid="${HOST_GID:-0}"

run_devshell() { exec nix develop "path:/opt/flake-warm" --command "$@"; }

if [ "$uid" = "0" ]; then
  run_devshell "$@"
fi

# Multi-user store: an unprivileged user must talk to the root daemon, so make
# sure it's running.
if ! pgrep -x nix-daemon >/dev/null 2>&1; then
  rm -f /nix/var/nix/daemon-socket/socket 2>/dev/null || true
  nix-daemon >/tmp/nix-daemon.log 2>&1 &
  for _ in $(seq 1 100); do
    [ -S /nix/var/nix/daemon-socket/socket ] && break
    sleep 0.05
  done
fi

# A user matching the host UID/GID, homed at /home/mbac (so a mounted ~/.claude
# lines up). -o allows reusing an id that already exists in the base image.
groupadd -o -g "$gid" dev 2>/dev/null || true
id -u dev >/dev/null 2>&1 || \
  useradd -o -u "$uid" -g "$gid" -d /home/mbac -M -s /bin/bash dev 2>/dev/null || true

# Let that user own its home and create fresh nix/XDG state (the build populated
# these as root; clear them so the dev user isn't blocked on their lock files).
chown "$uid:$gid" /home/mbac 2>/dev/null || true
rm -rf /home/mbac/.cache /home/mbac/.local 2>/dev/null || true

exec setpriv --reuid "$uid" --regid "$gid" --init-groups \
  nix develop "path:/opt/flake-warm" --command "$@"
