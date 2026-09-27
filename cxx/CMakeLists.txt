cmake_minimum_required(VERSION 3.22)
project(cppcaml LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# Core compiler library (grows one pipeline stage at a time).
add_library(cppcaml_core
  src/token.cpp
  src/lexer.cpp
  src/parser.cpp
  src/ast_print.cpp
  src/marshal.cpp
  src/blake2.cpp
  src/omarshal.cpp
  src/link.cpp
  # The typing/ port (TYPECHECKER.md).
  src/typing/ident.cpp
  src/typing/path.cpp
  src/typing/types.cpp
  src/typing/btype.cpp
  src/typing/datarepr.cpp
  src/typing/predef.cpp
  src/typing/subst.cpp
  src/typing/persistent_env.cpp
  src/typing/longident.cpp
  src/typing/env.cpp
  src/typing/mtype.cpp
  src/typing/ctype.cpp
  src/typing/ctype_instance.cpp
  src/typing/ctype_expand.cpp
  src/typing/ctype_unify.cpp
  src/typing/ctype_filter.cpp
  src/typing/ctype_moregen.cpp
  src/typing/ctype_misc.cpp
  src/typing/parsetree_of_ast.cpp
  src/typing/typedtree.cpp
  src/typing/typetexp.cpp
  src/typing/typecore_util.cpp
  src/typing/typecore_pat.cpp
  src/typing/typecore_app.cpp
  src/typing/typecore_exp.cpp
  src/typing/typecore_fun.cpp
  src/typing/typecore_let.cpp
  src/typing/camlinternal_format.cpp
  src/typing/parmatch.cpp
  src/typing/typeopt.cpp
  src/typing/value_rec_check.cpp
  src/typing/primitive.cpp
  src/typing/typedecl_unboxed.cpp
  src/typing/typedecl_variance.cpp
  src/typing/typedecl_separability.cpp
  src/typing/typedecl.cpp
  src/typing/typedecl_ext.cpp
  src/typing/includecore.cpp
  src/typing/includeclass.cpp
  src/typing/shape.cpp
  src/typing/signature_group.cpp
  src/typing/includemod.cpp
  src/typing/parsetree_ovalue.cpp
  src/typing/typemod_util.cpp
  src/typing/error_report.cpp
  src/typing/warnings.cpp
  src/typing/warnings_message.cpp
  src/typing/builtin_attributes.cpp
  src/typing/lambda.cpp
  src/typing/format.cpp
  src/typing/printlambda.cpp
  src/typing/format_doc.cpp
  src/typing/oprint.cpp
  src/typing/pprintast.cpp
  src/typing/out_type.cpp
  src/typing/printtyp.cpp
  src/typing/printpat.cpp
  src/typing/misc.cpp
  src/typing/location.cpp
  src/typing/errortrace_report.cpp
  src/typing/reporters.cpp
  src/typing/env_report.cpp
  src/typing/typecore_report.cpp
  src/typing/includecore_report.cpp
  src/typing/typedecl_report.cpp
  src/typing/includeclass_report.cpp
  src/typing/signature_matching.cpp
  src/typing/includemod_errorprinter.cpp
  src/typing/typemod_report.cpp
  src/typing/typeclass_report.cpp
  src/typing/translate_report.cpp
  src/typing/simplif.cpp
  src/typing/tmc.cpp
  src/typing/translobj.cpp
  src/typing/translclass.cpp
  src/typing/translmod.cpp
  src/typing/translcore.cpp
  src/typing/translprim.cpp
  src/typing/translattribute.cpp
  src/typing/value_rec_compiler.cpp
  src/typing/matching.cpp
  src/typing/bytegen.cpp
  src/typing/printinstr.cpp
  src/typing/emitcode.cpp
  src/typing/cmt_format.cpp
  src/typing/shape_reduce.cpp
  src/typing/arg.cpp
  src/typing/config.cpp
  src/typing/compenv.cpp
  src/typing/main_args.cpp
  src/typing/bytepackager.cpp
  src/typing/typemod_sig.cpp
  src/typing/typemod_str.cpp
  src/typing/typemod_unit.cpp
  src/typing/typemod_pack.cpp
  src/typing/typecore_class.cpp
  src/typing/typeclass.cpp
  src/typing/cmi_format.cpp)
target_include_directories(cppcaml_core PUBLIC include)
# The typing/ port initializes aggregates partially on purpose (an OCaml
# constructor's unused fields stay defaulted).
file(GLOB CPPCAML_TYPING_SOURCES CONFIGURE_DEPENDS src/typing/*.cpp)
set_source_files_properties(${CPPCAML_TYPING_SOURCES} src/tools/typing_dump_main.cpp
  PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)

# mimalloc: statically link its malloc override into every tool that uses the
# core library.  glibc malloc is the measured bottleneck (the compiler is
# allocation-/data-cache bound); the override is ~9% faster on a full compiler
# build and produces byte-identical output.  We link the prebuilt static-
# override object (mimalloc.o) rather than -lmimalloc so the strong malloc
# symbols are always pulled in and reliably interpose libc's.  From nixpkgs
# (see flake.nix); CMAKE_PREFIX_PATH covers it in the dev shell.  Opt out with
# -DCPPCAML_MIMALLOC=OFF.
option(CPPCAML_MIMALLOC "Statically link the mimalloc allocator override" ON)
if(CPPCAML_MIMALLOC)
  find_file(MIMALLOC_OBJECT mimalloc.o PATH_SUFFIXES lib
            DOC "mimalloc static-override object")
  if(MIMALLOC_OBJECT)
    message(STATUS "mimalloc override: ${MIMALLOC_OBJECT}")
    target_link_libraries(cppcaml_core PUBLIC ${MIMALLOC_OBJECT})
    # The mmap pre-decoded cmi cache (cmi.cpp) uses the mi_* arena/heap API to
    # contain a cmi's decoded graph in one region; it degrades to the normal
    # decode path when the header is absent.
    find_path(MIMALLOC_INCLUDE_DIR mimalloc.h DOC "mimalloc header directory")
    if(MIMALLOC_INCLUDE_DIR)
      message(STATUS "mimalloc headers: ${MIMALLOC_INCLUDE_DIR}")
      target_include_directories(cppcaml_core PUBLIC ${MIMALLOC_INCLUDE_DIR})
      target_compile_definitions(cppcaml_core PUBLIC CPPCAML_HAVE_MIMALLOC=1)
    endif()
  else()
    message(FATAL_ERROR
      "mimalloc.o not found (add pkgs.mimalloc / set CMAKE_PREFIX_PATH, "
      "or pass -DCPPCAML_MIMALLOC=OFF)")
  endif()
endif()

# Stage tools.
add_executable(c++lex src/tools/cpplex_main.cpp)
target_link_libraries(c++lex PRIVATE cppcaml_core)

add_executable(c++parse src/tools/cppparse_main.cpp)
target_link_libraries(c++parse PRIVATE cppcaml_core)

add_executable(c++link src/tools/cpplink_main.cpp)
target_link_libraries(c++link PRIVATE cppcaml_core)

add_executable(c++ocamlc src/tools/cppocamlc_main.cpp)
target_link_libraries(c++ocamlc PRIVATE cppcaml_core)

# Stage-1 oracle for the typing/ port: dump a cmi's decoded Types graph
# (compared against cxx/harness/typing_cmidump.ml).
add_executable(c++typing-dump src/tools/typing_dump_main.cpp)
target_link_libraries(c++typing-dump PRIVATE cppcaml_core)
