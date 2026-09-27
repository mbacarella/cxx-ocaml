// Port of stdlib/arg.ml (see arg.hpp).
#include "cppcaml/typing/arg.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>

namespace cppcaml::typing::arg {

namespace {

struct Error {  // Unknown | Wrong | Missing | Message
  enum class K { Unknown, Wrong, Missing, Message } k;
  std::string s, arg, expected;
};
struct Stop {  // exception Stop of error
  Error e;
};

const Spec* assoc3(const std::string& x, const std::vector<Option>& l) {
  for (const Option& o : l)
    if (o.key == x) return &o.spec;
  return nullptr;
}

std::string make_symlist(const std::string& prefix, const std::string& sep, const std::string& suffix,
                         const std::vector<std::string>& l) {
  if (l.empty()) return "<none>";
  std::string r = prefix + l[0];
  for (std::size_t i = 1; i < l.size(); ++i) r += sep + l[i];
  return r + suffix;
}

void print_spec(std::string& buf, const Option& o) {
  if (o.doc.empty()) return;
  if (o.spec.k == Spec::K::Symbol)
    buf += "  " + o.key + " " + make_symlist("{", "|", "}", o.spec.symbols) + o.doc + "\n";
  else
    buf += "  " + o.key + " " + o.doc + "\n";
}

// the -help / --help actions (help_action: raise (Stop (Unknown "-help")))
std::vector<Option> add_help(const std::vector<Option>& speclist) {
  std::vector<Option> r = speclist;
  auto help = [](const char* key) {
    Option o;
    o.key = key;
    o.spec.k = Spec::K::Unit;
    o.spec.unit = [] { throw Stop{{Error::K::Unknown, "-help", "", ""}}; };
    o.doc = " Display this list of options";
    return o;
  };
  if (!assoc3("-help", speclist)) r.push_back(help("-help"));
  if (!assoc3("--help", speclist)) r.push_back(help("--help"));
  return r;
}

void usage_b(std::string& buf, const std::vector<Option>& speclist, const std::string& errmsg) {
  buf += errmsg + "\n";
  for (const Option& o : add_help(speclist)) print_spec(buf, o);
}

bool bool_of_string_opt(const std::string& s, bool& out) {
  if (s == "true") return out = true, true;
  if (s == "false") return out = false, true;
  return false;
}

// float_of_string_opt: strtod over the whole string, `_` allowed as in OCaml
bool float_of_string_opt(const std::string& s0, double& out) {
  std::string s;
  for (char c : s0)
    if (c != '_') s += c;
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  out = std::strtod(s.c_str(), &end);
  return end && *end == '\0';
}

}  // namespace

// int_of_string_opt (runtime/ints.c parse_intnat, 63-bit ints): an optional
// sign, a 0x / 0o / 0b / 0u prefix, digits with `_` after the first one; a
// decimal literal must fit in [min_int, max_int], the others may use the
// full unsigned range and wrap
bool int_of_string_opt(const std::string& s, long& out) {
  std::size_t i = 0, n = s.size();
  bool neg = false;
  if (i < n && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
  int base = 10;
  bool is_signed = true;
  if (i + 1 < n && s[i] == '0') {
    switch (s[i + 1]) {
      case 'x': case 'X': base = 16; is_signed = false; i += 2; break;
      case 'o': case 'O': base = 8; is_signed = false; i += 2; break;
      case 'b': case 'B': base = 2; is_signed = false; i += 2; break;
      case 'u': case 'U': base = 10; is_signed = false; i += 2; break;
      default: break;
    }
  }
  auto digit = [&](char c) -> int {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return -1;
    return d < base ? d : -1;
  };
  if (i >= n || digit(s[i]) < 0) return false;
  constexpr unsigned long long max_int = (1ULL << 62) - 1;       // 63-bit OCaml int
  constexpr unsigned long long umax = (1ULL << 63) - 1;          // 2 * max_int + 1
  unsigned long long res = 0;
  for (; i < n; ++i) {
    if (s[i] == '_') continue;
    int d = digit(s[i]);
    if (d < 0) return false;
    if (res > (umax - static_cast<unsigned>(d)) / static_cast<unsigned>(base)) return false;
    res = res * base + d;
  }
  if (is_signed) {
    if (neg ? res > max_int + 1 : res > max_int) return false;
  } else if (res > umax) {
    return false;
  }
  // wrap into 63-bit two's complement
  unsigned long long v = neg ? (0ULL - res) : res;
  v &= umax;
  long r = (v & (1ULL << 62)) ? static_cast<long>(v | ~umax) : static_cast<long>(v);
  out = r;
  return true;
}

std::string usage_string(const std::vector<Option>& speclist, const std::string& errmsg) {
  std::string b;
  usage_b(b, speclist, errmsg);
  return b;
}

void usage(const std::vector<Option>& speclist, const std::string& errmsg) {
  std::cerr << usage_string(speclist, errmsg);
  std::cerr.flush();
}

void parse_and_expand_argv_dynamic(long& current, std::vector<std::string>& argv, std::vector<Option>& speclist,
                                   const AnonFun& anonfun, const std::string& errmsg) {
  const long initpos = current;
  auto convert_error = [&](const Error& error) -> std::string {
    // (the message: the program name, the error, then the usage)
    std::string b;
    std::string progname = initpos < static_cast<long>(argv.size()) ? argv[initpos] : "(?)";
    switch (error.k) {
      case Error::K::Unknown:
        if (error.s != "-help" && error.s != "--help") b += progname + ": unknown option '" + error.s + "'.\n";
        break;
      case Error::K::Missing: b += progname + ": option '" + error.s + "' needs an argument.\n"; break;
      case Error::K::Wrong:
        b += progname + ": wrong argument '" + error.arg + "'; option '" + error.s + "' expects " + error.expected +
             ".\n";
        break;
      case Error::K::Message: b += progname + ": " + error.s + ".\n"; break;
    }
    usage_b(b, speclist, errmsg);
    return b;
  };
  auto raise_converted = [&](const Error& e) {
    std::string msg = convert_error(e);
    if (e.k == Error::K::Unknown && (e.s == "-help" || e.s == "--help")) throw Help(msg);
    throw Bad(msg);
  };
  ++current;
  while (current < static_cast<long>(argv.size())) {
    try {
      const std::string s = argv[current];
      if (s.rfind("-", 0) == 0) {
        // (-help / --help are not in speclist: an Unknown "-help" becomes Help)
        const Spec* action = assoc3(s, speclist);
        std::optional<std::string> follow;
        if (!action) {
          std::size_t eq = s.find('=');
          if (eq != std::string::npos) {
            action = assoc3(s.substr(0, eq), speclist);
            if (action) follow = s.substr(eq + 1);
          }
          if (!action) throw Stop{{Error::K::Unknown, s, "", ""}};
        }
        auto no_arg = [&] {
          if (follow) throw Stop{{Error::K::Wrong, s, *follow, "no argument"}};
        };
        auto get_arg = [&]() -> std::string {
          if (!follow) {
            if (current + 1 < static_cast<long>(argv.size())) return argv[current + 1];
            throw Stop{{Error::K::Missing, s, "", ""}};
          }
          return *follow;
        };
        auto consume_arg = [&] {
          if (!follow) ++current;
        };
        std::function<void(const Spec&)> treat_action = [&](const Spec& a) {
          switch (a.k) {
            case Spec::K::Unit: no_arg(); a.unit(); break;
            case Spec::K::Bool: {
              std::string arg = get_arg();
              bool b;
              if (!bool_of_string_opt(arg, b)) throw Stop{{Error::K::Wrong, s, arg, "a boolean"}};
              a.bool_(b);
              consume_arg();
              break;
            }
            case Spec::K::Set: no_arg(); *a.ref = true; break;
            case Spec::K::Clear: no_arg(); *a.ref = false; break;
            case Spec::K::String: {
              std::string arg = get_arg();
              a.string(arg);
              consume_arg();
              break;
            }
            case Spec::K::Symbol: {
              std::string arg = get_arg();
              bool mem = false;
              for (const std::string& sy : a.symbols) mem = mem || sy == arg;
              if (mem) {
                a.string(arg);
                consume_arg();
              } else {
                throw Stop{{Error::K::Wrong, s, arg, "one of: " + make_symlist("", " ", "", a.symbols)}};
              }
              break;
            }
            case Spec::K::Set_string: *a.sref = get_arg(); consume_arg(); break;
            case Spec::K::Int: {
              std::string arg = get_arg();
              long x;
              if (!int_of_string_opt(arg, x)) throw Stop{{Error::K::Wrong, s, arg, "an integer"}};
              a.int_(x);
              consume_arg();
              break;
            }
            case Spec::K::Set_int: {
              std::string arg = get_arg();
              long x;
              if (!int_of_string_opt(arg, x)) throw Stop{{Error::K::Wrong, s, arg, "an integer"}};
              *a.iref = x;
              consume_arg();
              break;
            }
            case Spec::K::Float: {
              std::string arg = get_arg();
              double x;
              if (!float_of_string_opt(arg, x)) throw Stop{{Error::K::Wrong, s, arg, "a float"}};
              a.float_(x);
              consume_arg();
              break;
            }
            case Spec::K::Set_float: {
              std::string arg = get_arg();
              double x;
              if (!float_of_string_opt(arg, x)) throw Stop{{Error::K::Wrong, s, arg, "a float"}};
              *a.fref = x;
              consume_arg();
              break;
            }
            case Spec::K::Tuple:
              no_arg();
              for (const Spec& sp : a.tuple) treat_action(sp);
              break;
            case Spec::K::Rest:
              no_arg();
              while (current < static_cast<long>(argv.size()) - 1) {
                a.string(argv[current + 1]);
                consume_arg();
              }
              break;
            case Spec::K::Rest_all: {
              no_arg();
              std::vector<std::string> acc;
              while (current < static_cast<long>(argv.size()) - 1) {
                acc.push_back(argv[current + 1]);
                consume_arg();
              }
              a.rest_all(acc);
              break;
            }
            case Spec::K::Expand: {
              std::string arg = get_arg();
              std::vector<std::string> newarg = a.expand(arg);
              consume_arg();
              std::vector<std::string> r(argv.begin(), argv.begin() + current + 1);
              r.insert(r.end(), newarg.begin(), newarg.end());
              r.insert(r.end(), argv.begin() + current + 1, argv.end());
              argv = std::move(r);
              break;
            }
          }
        };
        const Spec spec = *action;  // (the action may extend speclist)
        treat_action(spec);
      } else {
        anonfun(s);
      }
    } catch (const Bad& m) {
      raise_converted({Error::K::Message, m.msg, "", ""});
    } catch (const Stop& e) {
      raise_converted(e.e);
    }
    ++current;
  }
}

namespace {
std::vector<std::string> read_aux(bool trim, char sep, const std::string& file) {
  std::FILE* ic = std::fopen(file.c_str(), "rb");
  if (!ic) throw SysError(file + ": " + std::strerror(errno));
  std::vector<std::string> words;
  std::string buf;
  auto stash = [&] {
    std::string word = buf;
    if (trim && !word.empty() && word.back() == '\r') word.pop_back();
    words.push_back(word);
    buf.clear();
  };
  int c;
  while ((c = std::fgetc(ic)) != EOF) {
    if (static_cast<char>(c) == sep) stash();
    else buf += static_cast<char>(c);
  }
  if (!buf.empty()) stash();
  std::fclose(ic);
  return words;
}
}  // namespace

std::vector<std::string> read_arg(const std::string& file) { return read_aux(true, '\n', file); }
std::vector<std::string> read_arg0(const std::string& file) { return read_aux(false, '\0', file); }

}  // namespace cppcaml::typing::arg
