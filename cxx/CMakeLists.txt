cmake_minimum_required(VERSION 3.28)
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
  src/cmi.cpp
  src/typer.cpp
  src/typedtree_print.cpp
  src/infer.cpp
  src/infer_check.cpp
  src/lambda.cpp
  src/bytecode.cpp
  src/omarshal.cpp
  src/cmo.cpp
  src/link.cpp)
target_include_directories(cppcaml_core PUBLIC include)

# Stage tools.
add_executable(c++lex src/tools/cpplex_main.cpp)
target_link_libraries(c++lex PRIVATE cppcaml_core)

add_executable(c++parse src/tools/cppparse_main.cpp)
target_link_libraries(c++parse PRIVATE cppcaml_core)

add_executable(c++cmi src/tools/cppcmi_main.cpp)
target_link_libraries(c++cmi PRIVATE cppcaml_core)

add_executable(c++type src/tools/cpptype_main.cpp)
target_link_libraries(c++type PRIVATE cppcaml_core)

add_executable(c++infer-test src/tools/cppinfer_test.cpp)
target_link_libraries(c++infer-test PRIVATE cppcaml_core)

# Header-only: modsig.hpp needs no library, so no static-lib link (avoids the
# llvm-ar gotcha).  Just the include dir.
add_executable(c++modsig-test src/tools/cppmodsig_test.cpp)
target_include_directories(c++modsig-test PRIVATE include)

add_executable(c++lambda src/tools/cpplambda_main.cpp)
target_link_libraries(c++lambda PRIVATE cppcaml_core)

add_executable(c++instr src/tools/cppinstr_main.cpp)
target_link_libraries(c++instr PRIVATE cppcaml_core)

add_executable(c++cmo src/tools/cppcmo_main.cpp)
target_link_libraries(c++cmo PRIVATE cppcaml_core)

add_executable(c++link src/tools/cpplink_main.cpp)
target_link_libraries(c++link PRIVATE cppcaml_core)

add_executable(c++ocamlc src/tools/cppocamlc_main.cpp)
target_link_libraries(c++ocamlc PRIVATE cppcaml_core)
