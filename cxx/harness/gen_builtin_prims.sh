#!/usr/bin/env bash
# Regenerate cxx/include/cppcaml/builtin_prims.hpp from runtime/primitives --
# the C++ analogue of `make lambda/runtimedef.ml`.  The linker bakes the table
# in (as upstream bakes in Runtimedef) rather than reading the file, so it must
# be regenerated whenever the runtime's primitive list changes.
#
# Usage: bash cxx/harness/gen_builtin_prims.sh > cxx/include/cppcaml/builtin_prims.hpp
set -u
R="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
sed '/kBuiltinPrimitives\[\] = {/q' "$R/cxx/include/cppcaml/builtin_prims.hpp"
sed 's/.*/    "&",/' "$R/runtime/primitives"
printf '};\n\n}  // namespace cppcaml::link\n'
