# syntax=docker/dockerfile:1
#
# A reusable dev environment for the c++caml project (an OCaml compiler
# reimplemented in C++, validated against a stock-OCaml oracle), plus the Claude
# Code CLI.
#
# The repo is NOT baked into the image -- mount your working tree at run time so
# the host and container can never drift:
#
#   docker build -t cppcaml .
#   docker run --rm -it -v "$PWD:/home/mbac/code/c++caml" cppcaml
#
# Base is glibc Debian so the prebuilt Claude Code native binary runs as-is
# (and keeps working across its self-updates). Nix is installed on top to supply
# the exact toolchain from the repo's own flake.nix (pinned by flake.lock):
# clang 20, cmake, ninja, make, the OCaml 5.3 host, menhir, gdb, git -- realised
# into the image's Nix store so `nix develop` is instant.
#
# The oracle (ocamlc.opt + stdlib + runtime/ocamlrun) and the C++ rewrite live
# in the mounted tree. If the host already built them they're visible
# immediately; otherwise build inside the container:
#   ./configure CC=clang && make -j"$(nproc)" world.opt && \
#     cmake -G Ninja -B cxx/build cxx && ninja -C cxx/build
#
# More examples:
#   # run a parity harness against the mounted tree:
#   docker run --rm -v "$PWD:/home/mbac/code/c++caml" cppcaml \
#     bash -c 'JOBS=8 bash ./cxx/harness/lambda_parity.sh'
#   # Claude Code, with auth + project memory persisted from the host:
#   docker run --rm -it \
#     -v "$PWD:/home/mbac/code/c++caml" -v "$HOME/.claude:/home/mbac/.claude" \
#     cppcaml claude
#
FROM debian:bookworm-slim

# Minimal host deps: the Nix installer + TLS roots, git, and procps (glibc and
# libstdc++ -- which the native Claude Code binary needs -- ship with Debian).
RUN apt-get update && apt-get install -y --no-install-recommends \
      curl xz-utils ca-certificates git procps && \
    rm -rf /var/lib/apt/lists/*

# Match the original host layout: HOME=/home/mbac and the tree at ~/code/c++caml,
# so a mounted ~/.claude resolves to the same Claude Code project key and
# repo-relative paths line up.
ENV HOME=/home/mbac
ENV REPO=/home/mbac/code/c++caml
ENV NPM_CONFIG_PREFIX=/home/mbac/.npm-global
RUN mkdir -p "$HOME" "$NPM_CONFIG_PREFIX" "$REPO"

# Install Nix (Determinate installer: non-interactive and container-friendly,
# no systemd) and enable flakes + the unified `nix` command.
RUN curl --proto '=https' --tlsv1.2 -sSf -L https://install.determinate.systems/nix \
      | sh -s -- install linux --init none --no-confirm && \
    mkdir -p /etc/nix && \
    printf 'experimental-features = nix-command flakes\nmax-jobs = auto\n' \
      >> /etc/nix/nix.conf
ENV PATH=/home/mbac/.npm-global/bin:/home/mbac/.nix-profile/bin:/nix/var/nix/profiles/default/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# Pre-realise the dev-shell toolchain into the image's Nix store, keyed on the
# flake alone (copied to a throwaway path so the mount point stays empty and the
# heavy source tree -- incl. .git -- is never copied into the store). The store
# paths persist in the image layer; at run time `nix develop path:/opt/flake-warm`
# reuses them.
COPY flake.nix flake.lock /opt/flake-warm/
RUN nix develop "path:/opt/flake-warm" --command true

# Node.js (from the same nixpkgs channel as the flake) + the Claude Code CLI
# (a prebuilt native binary -- runs because the base is glibc Debian).
RUN nix profile install github:NixOS/nixpkgs/nixos-25.05#nodejs_20 && \
    npm install -g @anthropic-ai/claude-code

# Land in the mounted repo, inside the dev shell. The dev shell is built from the
# warm flake (identical to the repo's, but avoids copying the mounted tree into
# the Nix store on every invocation).
WORKDIR ${REPO}
# `--command` runs the given argv inside the dev shell. Use a *non-login* bash:
# a login shell re-sources /etc/profile, which resets PATH and would drop the
# toolchain that `nix develop` put on PATH.
ENTRYPOINT ["nix", "develop", "path:/opt/flake-warm", "--command"]
CMD ["bash"]
