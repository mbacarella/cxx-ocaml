// The few operating-system services whose interface differs between Linux
// and macOS.
#pragma once

#include <sys/stat.h>
#include <time.h>

#include <cstdint>
#include <string>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace cppcaml::os {

// The runtime's caml_executable_name (runtime/unix.c): /proc/self/exe's
// target on Linux, _NSGetExecutablePath (not resolved) on macOS; "" when
// unknown.
inline std::string self_exe() {
#if defined(__APPLE__)
  std::uint32_t n = 256;
  std::string buf(n, '\0');
  if (_NSGetExecutablePath(buf.data(), &n) != 0) {
    buf.assign(n, '\0');
    if (_NSGetExecutablePath(buf.data(), &n) != 0) return "";
  }
  buf.resize(std::char_traits<char>::length(buf.c_str()));
  return buf;
#else
  std::string buf(256, '\0');
  for (;;) {
    ssize_t r = ::readlink("/proc/self/exe", buf.data(), buf.size());
    if (r < 0) return "";
    if (static_cast<std::size_t>(r) < buf.size()) {
      buf.resize(static_cast<std::size_t>(r));
      return buf;
    }
    if (buf.size() >= (1 << 20)) return "";
    buf.resize(buf.size() * 2);
  }
#endif
}

// A stat's modification and status-change times.
inline const struct timespec& mtime(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_mtimespec;
#else
  return st.st_mtim;
#endif
}
inline const struct timespec& ctime(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_ctimespec;
#else
  return st.st_ctim;
#endif
}

}  // namespace cppcaml::os
