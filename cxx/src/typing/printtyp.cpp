// Port of typing/printtyp.ml (TYPECHECKER.md stage 9).
#include "cppcaml/typing/printtyp.hpp"

#include "cppcaml/typing/ident.hpp"
#include "cppcaml/typing/oprint.hpp"
#include "cppcaml/typing/pprintast.hpp"
#include "cppcaml/typing/printlambda.hpp"

namespace cppcaml::typing::printtyp {

using namespace format_doc;
namespace ot = outcometree;
using out_type::Mode;

void namespaced_ident(out_type::Namespace ns, Formatter& ppf, Ident::t id) {
  pp_print_string(ppf, out_type::ident_name(ns, id)->printed_name);
}

void wrap_printing_env(bool error, env::t env, const std::function<void()>& f) {
  out_type::wrap_printing_env(error, env, f);
}

void ident(Formatter& ppf, Ident::t id) { pp_print_string(ppf, out_type::ident_name(std::nullopt, id)->printed_name); }

void longident(Formatter& ppf, Longident::t lid) { pprintast::longident(ppf, lid); }

void typexp(Mode mode, Formatter& ppf, TypeExpr* ty) { oprint::out_type(ppf, out_type::tree_of_typexp(mode, ty)); }

void type_expansion(Mode k, Formatter& ppf, const out_type::ExpansionPair& e) {
  out_type::pp_type_expansion(ppf, out_type::trees_of_type_expansion(k, e));
}

void type_declaration(Ident::t id, Formatter& ppf, const TypeDeclaration* decl) {
  oprint::out_sig_item(ppf, out_type::tree_of_type_declaration(id, decl, RecStatus::Trec_first));
}

void type_expr(Formatter& ppf, TypeExpr* ty) {
  // [type_expr] is used directly by error message printers,
  // we mark eventual loops ourself to avoid any misuse and stack overflow
  out_type::prepare_for_printing({ty});
  out_type::prepared_type_expr(ppf, ty);
}

void shared_type_scheme(Formatter& ppf, TypeExpr* ty) {
  out_type::add_type_to_preparation(ty);
  printtyp::typexp(Mode::Type_scheme, ppf, ty);
}

void type_scheme(Formatter& ppf, TypeExpr* ty) {
  out_type::prepare_for_printing({ty});
  out_type::prepared_type_scheme(ppf, ty);
}

void path(Formatter& ppf, Path::t p) { oprint::out_ident(ppf, out_type::tree_of_path(p, false)); }

void type_path(Formatter& ppf, Path::t p) { oprint::out_ident(ppf, out_type::tree_of_type_path(p)); }

void value_description(Ident::t id, Formatter& ppf, const ValueDescription* decl) {
  oprint::out_sig_item(ppf, out_type::tree_of_value_description(id, decl));
}

void class_type(Formatter& ppf, const ClassType* cty) {
  out_type::reset();
  out_type::prepare_class_type(cty);
  oprint::out_class_type(ppf, out_type::tree_of_class_type(Mode::Type, cty));
}

void class_declaration(Ident::t id, Formatter& ppf, const ClassDeclaration* cl) {
  oprint::out_sig_item(ppf, out_type::tree_of_class_declaration(id, cl, RecStatus::Trec_first));
}

void cltype_declaration(Ident::t id, Formatter& ppf, const ClassTypeDeclaration* cl) {
  oprint::out_sig_item(ppf, out_type::tree_of_cltype_declaration(id, cl, RecStatus::Trec_first));
}

void modtype(Formatter& ppf, const ModuleType* mty) { oprint::out_module_type(ppf, out_type::tree_of_modtype(mty)); }

void modtype_declaration(Ident::t id, Formatter& ppf, const ModtypeDeclaration* decl) {
  oprint::out_sig_item(ppf, out_type::tree_of_modtype_declaration(id, decl));
}

void constructor(Formatter& ppf, const ConstructorDeclaration* c) {
  out_type::reset_except_conflicts();
  out_type::add_constructor_to_preparation(c);
  out_type::prepared_constructor(ppf, c);
}

void constructor_arguments(Formatter& ppf, const ConstructorArguments& a) {
  auto tys = out_type::tree_of_constructor_arguments(a);
  auto* t = ot::otyp(ot::OutType::K::Otyp_tuple);
  for (const ot::OutType* x : tys) t->tuple.emplace_back(std::nullopt, x);
  oprint::out_type(ppf, t);
}

void label(Formatter& ppf, const LabelDeclaration* l) {
  out_type::prepare_for_printing({l->ld_type});
  oprint::out_label(ppf, out_type::tree_of_label(l));
}

void extension_constructor(Ident::t id, Formatter& ppf, const ExtensionConstructor* ext) {
  oprint::out_sig_item(ppf, out_type::tree_of_extension_constructor(id, ext, ExtStatus::Text_first));
}

void extension_only_constructor(Ident::t id, Formatter& ppf, const ExtensionConstructor* ext) {
  out_type::reset_except_conflicts();
  out_type::prepare_type_constructor_arguments(ext->ext_args);
  if (ext->ext_ret_type) out_type::add_type_to_preparation(ext->ext_ret_type);
  std::string name(ident::name(id));
  auto [args, ret] = out_type::extension_constructor_args_and_ret_type_subtree(ext->ext_args, ext->ext_ret_type);
  fprintf(ppf, "@[<hv>%a@]", pr(oprint::out_constr, ot::OutConstructor{name, args, ret}));
}

namespace {
void print_signature(Formatter& ppf, const std::vector<const ot::OutSigItem*>& tree) {
  fprintf(ppf, "@[<v>%a@]", pr(oprint::out_signature, tree));
}
}  // namespace

void signature(Formatter& ppf, Signature sg) { fprintf(ppf, "%a", pr(print_signature, out_type::tree_of_signature(sg))); }

std::string string_of_path(Path::t p) { return format_doc::asprintf("%a", pr(path, p)); }

std::vector<std::string> strings_of_paths(out_type::Namespace ns, const std::vector<Path::t>& ps) {
  std::vector<const ot::OutIdent*> trees;
  for (Path::t p : ps) trees.push_back(out_type::namespaced_tree_of_path(ns, p));
  std::vector<std::string> r;
  for (auto* t : trees) r.push_back(format_doc::asprintf("%a", pr(oprint::out_ident, t)));
  return r;
}

void printed_signature(const std::string& sourcefile, Formatter& ppf, Signature sg) {
  // we are tracking any collision event for warning 63
  out_type::ident_conflicts::reset();
  auto t = out_type::tree_of_signature(sg);
  // (Warning 63 Erroneous_printed_signature: reported with the warnings, stage 9b)
  (void)sourcefile;
  (void)out_type::ident_conflicts::err_msg();
  print_signature(ppf, t);
}

namespace {
// Env.print_path / Printlambda's record_rep printer: Printtyp.path laid out
void format_path(format::Formatter& ppf, Path::t p) {
  format_doc::Formatter f;
  path(f, p);
  format_doc::format(ppf, f.doc);
}
}  // namespace

void install_hooks() { printlambda::print_path = format_path; }

}  // namespace cppcaml::typing::printtyp
