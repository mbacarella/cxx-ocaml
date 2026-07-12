{
  description = "c++caml — a C++ reimplementation of the OCaml compiler, validated against the OCaml trunk oracle";

  inputs = {
    # 25.05 gives gcc-14 libstdc++ (std::generator, std::print) + clang 20.
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        llvm = pkgs.llvmPackages_20;   # clang 20: full C++23 language surface
        mimalloc = pkgs.mimalloc;      # allocator override, static-linked (see CMakeLists.txt)
        # One toolchain, clang, for everything: it builds the C++ rewrite *and*
        # the OCaml trunk oracle (clang is a Tier-1, CI-tested C compiler for
        # OCaml on Linux, and the only one on macOS). Using clang as the shell's
        # stdenv compiler means `cc`/`CC` resolve to clang with no gcc wrapper
        # shadowing it on PATH.
      in {
        devShells.default = (pkgs.mkShell.override { stdenv = llvm.stdenv; }) {
          name = "cppcaml-dev";

          packages = [
            # --- C++ / build tooling (clang comes from the stdenv) ---
            llvm.lld
            pkgs.clang-tools    # clangd, clang-format, clang-tidy
            pkgs.cmake
            pkgs.ninja
            pkgs.gnumake        # for the OCaml trunk build
            pkgs.binutils
            pkgs.pkg-config
            mimalloc            # statically linked into the tools: ~9% faster builds

            # --- OCaml host tools: write oracle token/AST dumpers,
            #     regenerate .mll/.mly when we need to read the tables ---
            pkgs.ocaml
            pkgs.ocamlPackages.menhir

            # --- misc dev ---
            pkgs.gdb
            pkgs.diffutils
            pkgs.git
          ];

          # Banner goes to stderr so `nix develop --command <tool>` keeps a clean
          # stdout for piping/diffing oracle output.
          shellHook = ''
            export CC=clang
            export CXX=clang++
            # Let CMake's find_file locate mimalloc.o (nix's cmake hook does not
            # populate CMAKE_PREFIX_PATH in an interactive shell).
            export CMAKE_PREFIX_PATH="${mimalloc}''${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
            {
              echo "c++caml dev shell (clang-only)"
              echo "  C/C++ : $(clang --version | head -1)"
              echo "  ocaml host: $(ocaml -version)"
              echo "Build the oracle:  ./configure CC=clang && make -j\$(nproc)"
              echo "Build the rewrite: cmake -G Ninja -B cxx/build cxx && ninja -C cxx/build"
            } 1>&2
          '';
        };
      });
}
