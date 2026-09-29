// Port of bytecomp/dll.ml, the batch linker's half: the DLL search path and
// opening shared libraries For_checking (their dynamic symbols read with
// Binutils) to check that the primitives a program uses exist.
// (For_execution -- dlopen, synchronize_primitive -- is the toplevel's and
// Dynlink's, not ocamlc's.)
#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cppcaml::typing::dll {

// Failure msg (a library that cannot be read); Sys_error (it cannot be
// opened) escapes as arg::SysError
struct Failure : std::runtime_error {
  explicit Failure(const std::string& m) : std::runtime_error(m) {}
};

// extract_dll_name (~suffixed, file)
std::string extract_dll_name(const std::pair<bool, std::string>& dllib);
// Dll.init_compile nostdlib
void init_compile(bool nostdlib);
// add_path dirs
void add_path(const std::vector<std::string>& dirs);
// open_dlls For_checking names: raises Failure (std::runtime_error with msg)
void open_dlls_for_checking(const std::vector<std::string>& names);
void close_all_dlls();
// find_primitive prim_name: Some Prim_exists (true) | None
bool find_primitive(const std::string& prim_name);
void reset();
std::vector<std::string> search_path();

}  // namespace cppcaml::typing::dll
