// Port of utils/ccomp.ml; see ccomp.hpp.
#include "cppcaml/typing/ccomp.hpp"

#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include "cppcaml/typing/arg.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compenv.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/filename.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/persistent_env.hpp"

namespace cppcaml::typing::ccomp {

namespace cf = clflags;

namespace {
// Sys.command: the shell's exit status, 255 when it did not exit normally
int sys_command(const std::string& cmd) {
  std::cout.flush();
  std::cerr.flush();
  int status = std::system(cmd.c_str());
  if (status == -1) throw arg::SysError(cmd);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 255;
}

// build_response_file lst (the file is removed at exit)
std::string build_response_file(const std::vector<std::string>& lst) {
  std::string responsefile = filename::temp_file("camlresp", "");
  {
    std::ofstream oc(responsefile, std::ios::binary);
    for (const std::string& f : lst) oc << f << "\n";
  }
  static std::vector<std::string> to_remove;
  static bool registered = false;
  to_remove.push_back(responsefile);
  if (!registered) {
    registered = true;
    std::atexit([] {
      for (const std::string& f : to_remove) misc::remove_file(f);
    });
  }
  return "@" + responsefile;
}

std::string concat_space(const std::vector<std::string>& l) {
  std::string s;
  for (std::size_t i = 0; i < l.size(); ++i) s += (i ? " " : "") + l[i];
  return s;
}

std::string quote_prefixed(bool response_files, const std::string& pr, const std::vector<std::string>& lst) {
  std::vector<std::string> l;
  for (const std::string& f : lst)
    if (!f.empty()) l.push_back(pr + f);
  return quote_files(response_files, l);
}

// String.concat " " (List.rev !Clflags.all_ccopts)
std::string ccopts() {
  std::vector<std::string> l(cf::all_ccopts.rbegin(), cf::all_ccopts.rend());
  return concat_space(l);
}

// remove_Wl cclibs: -Wl,-foo,bar -> -foo bar
std::vector<std::string> remove_Wl(const std::vector<std::string>& cclibs) {
  std::vector<std::string> r;
  for (const std::string& c : cclibs) {
    if (c.size() >= 4 && c.compare(0, 4, "-Wl,") == 0) {
      std::string s = c.substr(4);
      for (char& ch : s)
        if (ch == ',') ch = ' ';
      r.push_back(s);
    } else {
      r.push_back(c);
    }
  }
  return r;
}

// expand_libname cclibs
std::vector<std::string> expand_libname(const std::vector<std::string>& cclibs) {
  std::vector<std::string> r;
  for (const std::string& c : cclibs) {
    if (c.rfind("-l", 0) == 0) {
      std::string libname = "lib" + c.substr(2) + config::ext_lib;
      try {
        r.push_back(load_path::find(libname));
      } catch (const load_path::NotFound&) {
        r.push_back(libname);
      }
    } else {
      r.push_back(c);
    }
  }
  return r;
}
}  // namespace

int command(const std::string& cmdline) {
  if (cf::verbose) {
    std::cout.flush();
    std::cerr << "+ " << cmdline << std::endl;
  }
  int res = sys_command(cmdline);
  if (res == 127) throw arg::SysError(cmdline);
  return res;
}

void run_command(const std::string& cmdline) { (void)command(cmdline); }

std::string quote_files(bool response_files, const std::vector<std::string>& lst) {
  std::vector<std::string> quoted;
  for (const std::string& f : lst)
    if (!f.empty()) quoted.push_back(filename::quote(f));
  std::string s = concat_space(quoted);
  if (response_files && (s.size() >= 65536 || (s.size() >= 4096 && config::target_win32)))
    return build_response_file(quoted);
  return s;
}

std::string quote_optfile(const std::optional<std::string>& f) { return f ? filename::quote(*f) : ""; }

int compile_file(const std::string& name, const std::optional<std::string>& output, const std::string& opt,
                 const std::optional<std::string>& stable_name) {
  // (the "msvc" output filter: not for the Unix toolchains)
  std::string debug_prefix_map;
  if (stable_name && config::c_has_debug_prefix_map && config::system.rfind("mingw", 0) != 0)
    debug_prefix_map = " -fdebug-prefix-map=" + name + "=" + *stable_name;
  std::string cc;
  if (cf::c_compiler) {
    cc = *cf::c_compiler;
  } else {
    const std::string& cflags = cf::native_code ? config::native_cflags : config::bytecode_cflags;
    const std::string& cppflags = cf::native_code ? config::native_cppflags : config::bytecode_cppflags;
    cc = config::c_compiler + " " + cflags + " " + cppflags;
  }
  std::vector<std::string> dirs;  // List.rev (hidden_include_dirs @ include_dirs)
  std::vector<std::string> all = cf::hidden_include_dirs;
  all.insert(all.end(), cf::include_dirs.begin(), cf::include_dirs.end());
  for (auto it = all.rbegin(); it != all.rend(); ++it)
    dirs.push_back(compenv::expand_directory(config::standard_library, *it));
  std::string cmd = cc + debug_prefix_map + " " + (output ? config::c_output_obj + *output : std::string()) + " " +
                    opt + " -c " + (cf::debug && config::ccomp_type != "msvc" ? "-g" : "") + " " + ccopts() + " " +
                    quote_prefixed(true, "-I", dirs) + " " + cf::std_include_flag("-I") + " " + filename::quote(name);
  return command(cmd);
}

int create_archive(const std::string& archive, const std::vector<std::string>& file_list) {
  misc::remove_file(archive);
  std::string quoted_archive = filename::quote(archive);
  if (file_list.empty()) return 0;  // Don't call the archiver: #6550/#1094/#9011
  if (config::ccomp_type == "msvc")
    return command("link /lib /nologo /out:" + quoted_archive + " " + quote_files(true, file_list));
  return command(config::ar + " rc " + quoted_archive + " " +
                 quote_files(config::ar_supports_response_files, file_list));
}

int call_linker(LinkMode mode, const std::string& output_name, const std::vector<std::string>& files,
                const std::string& extra) {
  std::string cmd;
  if (mode == LinkMode::Partial) {
    std::string l_prefix = "-L";
    std::vector<std::string> fs = files;
    if (config::ccomp_type == "msvc") {
      l_prefix = "/libpath:";
      fs = expand_libname(files);
    }
    cmd = config::native_pack_linker + filename::quote(output_name) + " " +
          quote_prefixed(true, l_prefix, load_path::get_path_list()) + " " + quote_files(true, remove_Wl(fs)) +
          " " + extra;
  } else {
    std::string linker;
    if (cf::c_compiler) linker = *cf::c_compiler;
    else if (mode == LinkMode::Exe) linker = config::mkexe;
    else if (mode == LinkMode::Dll) linker = config::mkdll;
    else linker = config::mkmaindll;
    cmd = linker + " -o " + filename::quote(output_name) + " " + "" /*(Clflags.std_include_flag "-I")*/ + " " +
          quote_prefixed(true, "-L", load_path::get_path_list()) + " " + ccopts() + " " + quote_files(true, files) +
          " " + extra;
  }
  return command(cmd);
}

}  // namespace cppcaml::typing::ccomp
