// Port of driver/pparse.ml's binary-AST half (see pparse.hpp), with
// parsing/ast_mapper.ml's ppx context (PpxContext.make, add_ppx_context_*,
// drop_ppx_context_* ~restore:false).
#include "cppcaml/typing/pparse.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

#include "cppcaml/marshal.hpp"
#include "cppcaml/typing/arg.hpp"
#include "cppcaml/typing/ast_invariants.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/cmi_format.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/parsetree_ovalue.hpp"
#include "cppcaml/typing/persistent_env.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::pparse {

using parsetree::Signature;
using parsetree::Structure;

namespace {

namespace pt = parsetree;

// Filename.quote (Unix)
std::string quote(const std::string& s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  return r + "'";
}

// Filename.temp_file prefix "": a fresh file in $TMPDIR (else /tmp)
std::string temp_file(const std::string& prefix) {
  const char* t = std::getenv("TMPDIR");
  std::string dir = t && *t ? t : "/tmp";
  static std::mt19937 rng{std::random_device{}()};
  for (int counter = 0;; ++counter) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "%06x", static_cast<unsigned>(rng() & 0xFFFFFF));
    std::string name = dir + "/" + prefix + hex;
    int fd = ::open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      ::close(fd);
      return name;
    }
    if (counter >= 1000) throw arg::SysError(name + ": " + std::strerror(errno));
  }
}

// Ccomp.command: -verbose echoes the command; 127 (not run) is Sys_error
int command(const std::string& cmdline) {
  if (clflags::verbose) {
    std::cout.flush();
    std::cerr << "+ " << cmdline << '\n';
    std::cerr.flush();
  }
  int st = std::system(cmdline.c_str());
  int res = st == -1 ? 127 : WIFEXITED(st) ? WEXITSTATUS(st) : 255;
  if (res == 127) throw arg::SysError(cmdline);
  return res;
}

bool file_exists(const std::string& fn) {
  struct stat sb;
  return ::stat(fn.c_str(), &sb) == 0;
}

std::string read_file(const std::string& fn) {
  std::ifstream in(fn, std::ios::binary);
  if (!in) throw arg::SysError(fn + ": " + std::strerror(errno));
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// set_input_lexbuf: the whole file named [name], for error excerpts; a
// file that cannot be read leaves the previous lexbuf
void set_input_lexbuf(const std::string& name) {
  std::ifstream in(name, std::ios::binary);
  if (!in) return;
  std::ostringstream ss;
  ss << in.rdbuf();
  location::input_source = ss.str();
}

// the values of a binary AST's contents after its magic: input_name, AST
std::pair<std::string, const OValue*> input_ast(const std::string& contents, AstKind kind) {
  std::size_t off = magic_of_kind(kind).size();
  auto* d = reinterpret_cast<const std::uint8_t*>(contents.data());
  const OValue* name = cmi_format::input_ovalue(d, contents.size(), off);
  if (name->kind != OValue::Kind::String) throw cppcaml::marshal::Error("input_value: not a string");
  const OValue* ast = cmi_format::input_ovalue(d, contents.size(), off);
  return {std::string(name->s), ast};
}

// ---- Ast_mapper.PpxContext.make, with Ast_helper's nodes (their
// locations: !default_loc, Location.none) ----
// ast_mapper.ml's string literals: one object per content (ocamlopt, which
// built the reference ocamlc.opt, merges a unit's equal string constants)
std::string_view lit(std::string_view s) { return ocaml_literal("parsing/ast_mapper.ml", s); }
struct Ctx {
  Location none = location::none();
  pt::LidLoc lid(std::string_view name) const { return pt::LidLoc{Longident::lident(lit(name)), none}; }
  const pt::Expression* exp(const pt::ExpressionDesc* d) const {
    return make<pt::Expression>(pt::Expression{d, none, {}, {}});
  }
  const pt::Expression* string(std::string_view s) const {
    pt::Constant c{};
    c.pconst_desc.kind = pt::ConstantDesc::Kind::Pconst_string;
    c.pconst_desc.s = zstr(s);
    c.pconst_desc.str_loc = none;
    c.pconst_loc = none;
    return exp(make<pt::Pexp_constant>(pt::Pexp_constant{{pt::ExpressionDesc::Kind::Pexp_constant}, c}));
  }
  const pt::Expression* construct(std::string_view name, const pt::Expression* arg) const {
    return exp(make<pt::Pexp_construct>(pt::Pexp_construct{{pt::ExpressionDesc::Kind::Pexp_construct}, lid(name), arg}));
  }
  const pt::Expression* tuple(const pt::Expression* a, const pt::Expression* b) const {
    return exp(make<pt::Pexp_tuple>(pt::Pexp_tuple{
        {pt::ExpressionDesc::Kind::Pexp_tuple},
        slice(std::vector<pt::LabeledExpression>{{OptStr::none(), a}, {OptStr::none(), b}})}));
  }
  const pt::Expression* boolean(bool x) const { return construct(x ? "true" : "false", nullptr); }
  template <class T, class F>
  const pt::Expression* list(const std::vector<T>& l, F&& f) const {
    // make_list: x :: rest, from the head (the recursion's evaluation order)
    std::vector<const pt::Expression*> items;
    for (const T& x : l) items.push_back(f(x));
    const pt::Expression* r = construct("[]", nullptr);
    for (std::size_t k = items.size(); k-- > 0;) r = construct("::", tuple(items[k], r));
    return r;
  }
  const pt::Expression* strings(const std::vector<std::string_view>& l) const {
    return list(l, [&](std::string_view s) { return string_obj(s); });
  }
  // a Pconst_string of the string object [s] itself
  const pt::Expression* string_obj(std::string_view s) const {
    pt::Constant c{};
    c.pconst_desc.kind = pt::ConstantDesc::Kind::Pconst_string;
    c.pconst_desc.s = s;
    c.pconst_desc.str_loc = none;
    c.pconst_loc = none;
    return exp(make<pt::Pexp_constant>(pt::Pexp_constant{{pt::ExpressionDesc::Kind::Pexp_constant}, c}));
  }
};

// Clflags' directory lists as string objects, and Load_path's: Compmisc's
// init_path keeps an -I / -H argument's string (Misc.expand_directory
// returns a directory without `+` as it is), so a load path entry equal to
// one of them is that object
struct Dirs {
  std::vector<std::string_view> include, hidden;
  Dirs() {
    for (const std::string& d : clflags::include_dirs) include.push_back(zstr(d));
    for (const std::string& d : clflags::hidden_include_dirs) hidden.push_back(zstr(d));
  }
  static std::vector<std::string_view> of(const std::vector<std::string>& l,
                                          const std::vector<std::string_view>& args) {
    std::vector<std::string_view> r;
    for (const std::string& d : l) {
      std::string_view v;
      for (std::string_view a : args)
        if (a == d) v = a;
      r.push_back(v.data() ? v : zstr(d));
    }
    return r;
  }
};

const pt::Attribute* ppx_context(std::string_view tool_name) {
  Ctx c;
  auto [visible, hidden] = load_path::get_paths();
  Dirs dirs;
  std::vector<std::pair<pt::LidLoc, const pt::Expression*>> fields;
  auto field = [&](std::string_view name, const pt::Expression* e) { fields.push_back({c.lid(name), e}); };
  field("tool_name", c.string(tool_name));
  field("include_dirs", c.strings(dirs.include));
  field("hidden_include_dirs", c.strings(dirs.hidden));
  field("load_path", c.tuple(c.strings(Dirs::of(visible, dirs.include)), c.strings(Dirs::of(hidden, dirs.hidden))));
  std::vector<std::string_view> opens;
  for (const std::string& m : clflags::open_modules) opens.push_back(zstr(m));
  field("open_modules", c.strings(opens));
  field("for_package",
        clflags::for_package ? c.construct("Some", c.string(*clflags::for_package)) : c.construct("None", nullptr));
  field("debug", c.boolean(clflags::debug));
  field("use_threads", c.boolean(clflags::use_threads));
  field("use_vmthreads", c.boolean(false));
  field("recursive_types", c.boolean(clflags::recursive_types));
  field("principal", c.boolean(clflags::principal));
  field("no_alias_deps", c.boolean(clflags::no_alias_deps));
  field("unboxed_types", c.boolean(clflags::unboxed_types));
  field("unsafe_string", c.boolean(false));
  field("cookies", c.construct("[]", nullptr));  // String.Map.bindings !cookies: none
  const pt::Expression* rec = c.exp(make<pt::Pexp_record>(
      pt::Pexp_record{{pt::ExpressionDesc::Kind::Pexp_record}, slice(fields), nullptr}));
  const pt::StructureItem* item = make<pt::StructureItem>(pt::StructureItem{
      make<pt::Pstr_eval>(pt::Pstr_eval{{pt::StructureItemDesc::Kind::Pstr_eval}, rec, {}}), c.none});
  pt::Payload p{};
  p.kind = pt::Payload::Kind::PStr;
  p.str = slice(std::vector<const pt::StructureItem*>{item});
  return make<pt::Attribute>(pt::Attribute{pt::StrLoc{lit("ocaml.ppx.context"), c.none}, p, c.none});
}

// write_ast: magic, output_value !Location.input_name, output_value ast
void write_ast(AstKind kind, const std::string& fn, const OValue* ast) {
  std::FILE* oc = std::fopen(fn.c_str(), "wb");
  if (!oc) throw arg::SysError(fn + ": " + std::strerror(errno));
  std::string magic = magic_of_kind(kind);
  std::fwrite(magic.data(), 1, magic.size(), oc);
  auto* name = make<OValue>();
  name->kind = OValue::Kind::String;
  name->s = zstr(location::input_name);
  std::vector<std::uint8_t> a = cmi_format::output_ovalue(name);
  std::vector<std::uint8_t> b = cmi_format::output_ovalue(ast);
  std::fwrite(a.data(), 1, a.size(), oc);
  std::fwrite(b.data(), 1, b.size(), oc);
  std::fclose(oc);
}

std::string apply_rewriter(AstKind kind, const std::string& fn_in, const std::string& ppx) {
  std::string magic = magic_of_kind(kind);
  std::string fn_out = temp_file("camlppx");
  std::string comm = ppx + " " + quote(fn_in) + " " + quote(fn_out);
  bool ok = command(comm) == 0;
  std::remove(fn_in.c_str());
  if (!ok) {
    std::remove(fn_out.c_str());
    throw Error(Error::Kind::CannotRun, comm);
  }
  if (!file_exists(fn_out)) throw Error(Error::Kind::WrongMagic, comm);
  // check magic before passing to the next ppx
  std::string buffer;
  {
    std::ifstream ic(fn_out, std::ios::binary);
    buffer.resize(magic.size());
    ic.read(buffer.data(), static_cast<std::streamsize>(magic.size()));
    if (ic.gcount() != static_cast<std::streamsize>(magic.size())) buffer.clear();
  }
  if (buffer != magic) {
    std::remove(fn_out.c_str());
    throw Error(Error::Kind::WrongMagic, comm);
  }
  return fn_out;
}

// read_ast: Location.input_name := the recorded name; the AST value
const OValue* read_ast(AstKind kind, const std::string& fn) {
  struct Remove {
    const std::string& f;
    ~Remove() { std::remove(f.c_str()); }
  } remove{fn};
  std::string contents = read_file(fn);
  auto [name, ast] = input_ast(contents, kind);
  location::input_name = name;
  return ast;
}

const OValue* rewrite(AstKind kind, const OValue* ast) {
  std::string fn = temp_file("camlppx");
  write_ast(kind, fn, ast);
  // List.fold_left (apply_rewriter kind) fn (List.rev ppxs)
  const std::vector<std::string>& ppxs = clflags::all_ppx;
  for (auto it = ppxs.rbegin(); it != ppxs.rend(); ++it) fn = apply_rewriter(kind, fn, *it);
  return read_ast(kind, fn);
}

bool is_ppx_context(const pt::Attribute* a) { return a->attr_name.txt == "ocaml.ppx.context"; }

}  // namespace

std::string magic_of_kind(AstKind k) {
  return *config::config_var(k == AstKind::Structure ? "ast_impl_magic_number" : "ast_intf_magic_number");
}

bool is_ast_file(const std::string& contents, AstKind kind) {
  std::string magic = magic_of_kind(kind);
  if (contents.size() < magic.size()) return false;  // really_input_string's End_of_file
  if (contents.compare(0, magic.size(), magic) == 0) return true;
  if (contents.compare(0, 9, magic, 0, 9) == 0)
    misc::fatal_error("OCaml and preprocessor have incompatible versions");
  return false;
}

namespace {
template <class R, class F>
R read_ast_file(const std::string& contents, AstKind kind, F&& decode) {
  auto [name, ast] = input_ast(contents, kind);
  location::input_name = name;
  set_input_lexbuf(name);
  if (clflags::unsafe)
    location::prerr_warning(location::in_file(location::input_name),
                            warnings::Warning::make(warnings::Warning::K::Unsafe_array_syntax_without_parsing));
  return decode(ast);
}
}  // namespace

Structure read_ast_structure(const std::string& contents) {
  Structure s = read_ast_file<Structure>(contents, AstKind::Structure,
                                         [](const OValue* v) { return pt::structure_of_ovalue(v); });
  if (clflags::all_ppx.empty()) ast_invariants::structure(s);
  return s;
}
Signature read_ast_signature(const std::string& contents) {
  Signature s = read_ast_file<Signature>(contents, AstKind::Signature,
                                         [](const OValue* v) { return pt::signature_of_ovalue(v); });
  if (clflags::all_ppx.empty()) ast_invariants::signature(s);
  return s;
}

Structure apply_rewriters_str(Structure ast, std::string_view tool_name) {
  if (clflags::all_ppx.empty()) return ast;
  // Ast_mapper.add_ppx_context_str: Str.attribute (ppx_context ()) :: ast
  const pt::Attribute* ctx = ppx_context(tool_name);
  std::vector<const pt::StructureItem*> items{make<pt::StructureItem>(pt::StructureItem{
      make<pt::Pstr_attribute>(pt::Pstr_attribute{{pt::StructureItemDesc::Kind::Pstr_attribute}, ctx}),
      location::none()})};
  items.insert(items.end(), ast.begin(), ast.end());
  Structure r = pt::structure_of_ovalue(rewrite(AstKind::Structure, pt::ovalue_of_ast_structure(slice(items))));
  // drop_ppx_context_str ~restore:false
  if (!r.empty())
    if (auto* a = pt::as<pt::Pstr_attribute>(r[0]->pstr_desc); a && is_ppx_context(a->attr))
      r = Structure{r.p + 1, r.n - 1};
  ast_invariants::structure(r);
  return r;
}

Signature apply_rewriters_sig(Signature ast, std::string_view tool_name) {
  if (clflags::all_ppx.empty()) return ast;
  const pt::Attribute* ctx = ppx_context(tool_name);
  std::vector<const pt::SignatureItem*> items{make<pt::SignatureItem>(pt::SignatureItem{
      make<pt::Psig_attribute>(pt::Psig_attribute{{pt::SignatureItemDesc::Kind::Psig_attribute}, ctx}),
      location::none()})};
  items.insert(items.end(), ast.begin(), ast.end());
  Signature r = pt::signature_of_ovalue(rewrite(AstKind::Signature, pt::ovalue_of_ast_signature(slice(items))));
  if (!r.empty())
    if (auto* a = pt::as<pt::Psig_attribute>(r[0]->psig_desc); a && is_ppx_context(a->attr))
      r = Signature{r.p + 1, r.n - 1};
  ast_invariants::signature(r);
  return r;
}

}  // namespace cppcaml::typing::pparse
