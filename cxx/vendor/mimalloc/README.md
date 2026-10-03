# mimalloc (vendored)

[mimalloc](https://github.com/microsoft/mimalloc) 3.4.5, the allocator
c++ocamlc and c++ocamlopt statically link (`cxx/Makefile`: its static
override object, `src/static.c` compiled with the C compiler; `MIMALLOC=no`
builds without it).  MIT licensed (`LICENSE`).

These are the release's `include/` and `src/` directories, unmodified, from
https://github.com/microsoft/mimalloc/archive/refs/tags/v3.4.5.tar.gz
(sha256 19a43af0645c57d348e729d5b31e23e912582911bb1047f795790834d3416221).  To update: replace both directories and `LICENSE` with
another release's, and rebuild.
