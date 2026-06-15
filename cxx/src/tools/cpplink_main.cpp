// c++link — link .cmo/.cma objects into a runnable bytecode executable.
//
// Like `ocamlc` used as a linker, this auto-includes the standard library:
// stdlib.cma is prepended and std_exit.cmo appended (the latter's initializer
// runs `do_at_exit ()`, which flushes the standard channels on normal exit --
// without it a program's buffered stdout is lost).  Pass -nostdlib to suppress
// this and link only the given inputs.
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "cppcaml/link.hpp"

namespace fs = std::filesystem;

// Locate the stdlib directory (containing stdlib.cma / std_exit.cmo): an
// explicit -I, else $OCAMLLIB/$CAMLLIB, else "stdlib" relative to the CWD, else
// relative to this executable (<repo>/cxx/build/c++link -> <repo>/stdlib).
static std::string discover_stdlib(const std::string& flag) {
  if (!flag.empty()) return flag;
  if (const char* e = std::getenv("OCAMLLIB"); e && *e) return e;
  if (const char* e = std::getenv("CAMLLIB"); e && *e) return e;
  if (fs::exists("stdlib/stdlib.cma")) return "stdlib";
  std::error_code ec;
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    fs::path cand = exe.parent_path().parent_path().parent_path() / "stdlib";
    if (fs::exists(cand / "stdlib.cma")) return cand.string();
  }
  return "stdlib";
}

int main(int argc, char** argv) {
  std::vector<std::string> inputs;
  std::string out = "a.out", runtime, stdlib_flag;
  bool nostdlib = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) out = argv[++i];
    else if (a == "-runtime" && i + 1 < argc) runtime = argv[++i];
    else if (a == "-I" && i + 1 < argc) stdlib_flag = argv[++i];
    else if (a == "-nostdlib" || a == "-nopervasives") nostdlib = true;
    else inputs.push_back(a);
  }
  if (inputs.empty()) {
    std::cerr << "usage: c++link [-runtime <ocamlrun>] [-nostdlib] [-I <stdlib>] "
                 "<obj.cmo|lib.cma>... -o <exe>\n";
    return 2;
  }

  // Match ocamlc's linker defaults: prepend stdlib.cma and append std_exit.cmo,
  // unless suppressed or the caller already passed them explicitly.
  if (!nostdlib) {
    auto named = [&](const char* base) {
      for (auto& in : inputs) if (fs::path(in).filename() == base) return true;
      return false;
    };
    std::string sd = discover_stdlib(stdlib_flag);
    if (!named("std_exit.cmo")) {
      fs::path se = fs::path(sd) / "std_exit.cmo";
      if (fs::exists(se)) inputs.push_back(se.string());
    }
    if (!named("stdlib.cma")) {
      fs::path cma = fs::path(sd) / "stdlib.cma";
      if (fs::exists(cma)) inputs.insert(inputs.begin(), cma.string());
    }
  }

  try {
    cppcaml::link::link_executable(inputs, out, runtime);
  } catch (const std::exception& e) {
    std::cerr << "c++link: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
