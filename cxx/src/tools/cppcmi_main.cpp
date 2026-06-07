// Smoke-test driver for the Marshal reader: decode a .cmi's header value
// ((module-name, signature)) and print a shallow summary.  This proves the
// wire-format plumbing before the Types.signature interpreter is layered on top.
#include "cppcaml/cmi.hpp"
#include "cppcaml/marshal.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace m = cppcaml::marshal;
namespace cmi = cppcaml::cmi;

static std::size_t find_marshal_magic(const std::vector<std::uint8_t>& b) {
  // The cmi magic string (Config.cmi_magic_number) precedes the first Marshal
  // value; locate the Marshal small/big/compressed magic that follows it.
  for (std::size_t k = 0; k + 4 <= b.size(); ++k) {
    if (b[k] == 0x84 && b[k + 1] == 0x95 && b[k + 2] == 0xA6 &&
        (b[k + 3] == 0xBE || b[k + 3] == 0xBF || b[k + 3] == 0xBD))
      return k;
  }
  throw m::Error("no Marshal magic found in file");
}

static void print_summary(const m::Arena& a, std::size_t id, int depth) {
  const m::Value& v = a[id];
  std::printf("%*s", depth * 2, "");
  switch (v.kind) {
    case m::Value::Kind::Int:
      std::printf("Int %lld\n", v.i);
      break;
    case m::Value::Kind::String:
      std::printf("String[%zu] \"%.*s\"\n", v.str.size(),
                  static_cast<int>(v.str.size()), v.str.c_str());
      break;
    case m::Value::Kind::Double:
      std::printf("Double %g\n", v.d);
      break;
    case m::Value::Kind::DoubleArray:
      std::printf("DoubleArray[%zu]\n", v.darr.size());
      break;
    case m::Value::Kind::Block:
      std::printf("Block tag=%u size=%zu\n", v.tag, v.fields.size());
      if (depth < 3)
        for (std::size_t f : v.fields) print_summary(a, f, depth + 1);
      break;
  }
}

// Typed mode: load the cmi as a Types.signature and print value bindings.
static int typed_mode(const char* file, const char* sel) {
  try {
    cmi::CmiFile c = cmi::CmiFile::load(file);
    std::printf("module %s\n", c.module_name().c_str());
    std::string s = sel;
    if (s == "--values") {
      for (const auto& v : c.values())
        std::printf("  val %s : %s\n", v.name.c_str(),
                    cmi::print_type(v.type).c_str());
      return 0;
    }
    if (s == "--fields") {
      int i = 0;
      for (const auto& f : c.sig().fields) std::printf("  %d %s\n", i++, f.c_str());
      return 0;
    }
    if (s == "--types") {
      for (const auto& t : c.types())
        std::printf("  %s\n", cmi::print_type_decl(t).c_str());
      return 0;
    }
    if (s == "--modules") {
      for (const auto& md : c.modules())
        std::printf("  module %s : %s\n", md.name.c_str(),
                    md.type ? cmi::print_module_type(*md.type).c_str() : "?");
      return 0;
    }
    if (const cmi::ModuleDecl* md = c.find_module(s)) {
      std::printf("  module %s : %s\n", md->name.c_str(),
                  md->type ? cmi::print_module_type(*md->type).c_str() : "?");
      return 0;
    }
    if (const cmi::SigValue* v = c.find_value(s)) {
      std::printf("  val %s : %s\n", v->name.c_str(),
                  cmi::print_type(v->type).c_str());
      return 0;
    }
    if (const cmi::TypeDecl* t = c.find_type(s)) {
      std::printf("  %s\n", cmi::print_type_decl(*t).c_str());
      return 0;
    }
    std::fprintf(stderr, "'%s' not found as value or type\n", sel);
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s file.cmi              (shallow Marshal dump)\n"
                 "       %s file.cmi --values     (all value bindings)\n"
                 "       %s file.cmi NAME         (one value's type)\n",
                 argv[0], argv[0], argv[0]);
    return 2;
  }
  if (argc >= 3) {
    return typed_mode(argv[1], argv[2]);
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  try {
    std::size_t off = find_marshal_magic(bytes);
    m::Arena arena;
    std::size_t root = m::read_value(bytes.data(), bytes.size(), off, arena);
    std::printf("=== header value ===\n");
    print_summary(arena, root, 0);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
