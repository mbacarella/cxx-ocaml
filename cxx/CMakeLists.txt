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
  src/cmi.cpp
  src/typer.cpp
  src/typedtree_print.cpp)
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
