// Port of typing/typemod.ml, part 4: module type of a module expression,
// packing modules (type_package), the forward references into Typecore /
// Typetexp / Typeclass / Ctype / Env, compilation units
// (type_implementation, type_interface) and the initial environment.  The
// cmt / annot output and the printing of inferred signatures (-i) are not
// ported; writing the cmi is the driver's (env::save_signature is not
// ported yet).
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>

#include "cppcaml/typing/parmatch.hpp"
#include "cppcaml/typing/printtyp.hpp"
#include "cppcaml/typing/oprint.hpp"
#include "cppcaml/typing/warnings.hpp"
#include "cppcaml/typing/persistent_env.hpp"
#include "cppcaml/typing/shape.hpp"
#include "cppcaml/typing/cmt_format.hpp"
#include "cppcaml/typing/shape_reduce.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "typecore_internal.hpp"
#include "typemod_internal.hpp"

namespace cppcaml::typing::typemod {

using namespace types;
using pt::as;
using SK = SignatureItem::Kind;
using MK = tt::ModuleExprDesc::Kind;

// defined in typemod_str.cpp
std::pair<const tt::ModuleExpr*, shape::t> type_module_alias_with_shape(env::t env, const pt::ModuleExpr* smod);
std::pair<const tt::StructureItem*, env::t> type_str_item_fwd(env::t env, const pt::StructureItem* item);
const tt::ModuleExpr* wrap_constraint_package_(env::t env, bool mark, const tt::ModuleExpr* arg, const ModuleType* mty,
                                               const tt::ModuleType* explicit_);
void check_package_closed_(const Location& loc, env::t env, TypeExpr* typ,
                           const std::vector<std::pair<std::vector<std::string_view>, TypeExpr*>>& fl);
ctype::PackageSubtypeResult package_subtype_(env::t env, const Package* p1, const Package* p2);

// ---- normalize types in a signature ----------------------------------------------------------
static void normalize_modtype(const ModuleType* mty);
static void normalize_signature(Signature sg) {
  for (auto* it : sg) {
    if (it->kind == SK::Sig_value) ctype::normalize_type(it->value->val_type);
    else if (it->kind == SK::Sig_module) normalize_modtype(it->md->md_type);
  }
}
static void normalize_modtype(const ModuleType* mty) {
  if (mty->kind == ModuleType::Kind::Mty_signature) normalize_signature(mty->sign);
  else if (mty->kind == ModuleType::Kind::Mty_functor) normalize_modtype(mty->res);
}

// Extract the module type of a module expression
static std::pair<const tt::ModuleExpr*, const ModuleType*> type_module_type_of(env::t env, const pt::ModuleExpr* smod) {
  bool remove_aliases = builtin_attributes::has_attribute("remove_aliases", smod->pmod_attributes);
  const tt::ModuleExpr* tmty;
  if (auto* i = as<pt::Pmod_ident>(smod->pmod_desc)) {
    // turn off strengthening in this case
    auto [path, md] = env::lookup_module(true, smod->pmod_loc, i->lid.txt, env);
    tmty = make<tt::ModuleExpr>(make<tt::Tmod_ident>(tt::Tmod_ident{{MK::Tmod_ident}, path, i->lid}), smod->pmod_loc,
                                md->md_type, env, smod->pmod_attributes);
  } else {
    tmty = type_module(env, smod);
  }
  const ModuleType* mty = mtype::scrape_for_type_of(remove_aliases, env, tmty->mod_type);
  // PR#5036: must not contain non-generalized type variables
  check_nongen_modtype(env, smod->pmod_loc, mty);
  return {tmty, mty};
}

// ---- for Typecore ----------------------------------------------------------------------------
// Graft a longident onto a path
static Path::t extend_path(Path::t path, Longident::t lid) {
  switch (lid->kind) {
    case Longident::Kind::Lident: return Path::pdot(path, lid->s);
    case Longident::Kind::Ldot: return Path::pdot(extend_path(path, lid->l1), lid->s);
    default: throw std::logic_error("extend_path");
  }
}

// Lookup a type's longident within a signature (raises env::NotFound)
static std::function<Path::t(Longident::t)> lookup_type_in_sig(Signature sg) {
  auto types = std::make_shared<std::map<std::string_view, Ident::t>>();
  auto modules = std::make_shared<std::map<std::string_view, Ident::t>>();
  for (auto* it : sg) {
    if (it->kind == SK::Sig_type) (*types)[ident::name(it->id)] = it->id;
    else if (it->kind == SK::Sig_module) (*modules)[ident::name(it->id)] = it->id;
  }
  auto find = [](const std::map<std::string_view, Ident::t>& m, std::string_view n) {
    auto f = m.find(n);
    if (f == m.end()) throw env::NotFound{};
    return f->second;
  };
  return [types, modules, find](Longident::t lid) -> Path::t {
    std::function<Path::t(Longident::t)> mp = [&](Longident::t l) -> Path::t {
      switch (l->kind) {
        case Longident::Kind::Lident: return Path::pident(find(*modules, l->s));
        case Longident::Kind::Ldot: return Path::pdot(mp(l->l1), l->s);
        default: throw std::logic_error("lookup_type_in_sig");
      }
    };
    switch (lid->kind) {
      case Longident::Kind::Lident: return Path::pident(find(*types, lid->s));
      case Longident::Kind::Ldot: return Path::pdot(mp(lid->l1), lid->s);
      default: throw std::logic_error("lookup_type_in_sig");
    }
  };
}

static Longident::t unflatten(const std::vector<std::string_view>& l) {
  if (l.empty()) throw std::logic_error("Option.get");
  Longident::t r = Longident::lident(l[0]);
  for (std::size_t k = 1; k < l.size(); ++k) r = Longident::ldot(r, location::none(), l[k], location::none());
  return r;
}

static std::pair<const tt::ModuleExpr*, const Package*> type_package(env::t env, const pt::ModuleExpr* m,
                                                                     const Package* pack) {
  long outer_scope = ctype::get_current_level();
  // type the module and create a scope in a raised level
  auto [modl, scope] = typetexp::ty_var_env::with_local_scope([&] {
    return ctype::with_local_level([&] {
      const tt::ModuleExpr* me = type_module(env, m);
      long sc = ctype::create_scope();
      return std::make_pair(me, sc);
    });
  });
  mtype::lower_nongen(outer_scope, modl->mod_type);
  std::vector<PackConstraint> fl2;
  if (!pack->pack_constraints.empty()) {
    std::function<Path::t(Longident::t)> type_path;
    const tt::ModuleExprDesc* d = modl->mod_desc;
    Path::t mp = nullptr;
    if (auto* i = as<tt::Tmod_ident>(d)) mp = i->path;
    else if (auto* c = as<tt::Tmod_constraint>(d); c && !c->explicit_mty)
      if (auto* i2 = as<tt::Tmod_ident>(c->me->mod_desc)) mp = i2->path;
    if (mp) {
      // (PR#6982: special cased because of strengthening and packages)
      type_path = [mp](Longident::t lid) { return extend_path(mp, lid); };
    } else {
      Signature sg = extract_sig_open(env, modl->mod_loc, modl->mod_type);
      auto [sg2, env2] = env::enter_signature(static_cast<int>(scope), sg, env);
      type_path = lookup_type_in_sig(sg2);
      env = env2;
    }
    // List.fold_right: from the last constraint
    for (std::size_t k = pack->pack_constraints.size(); k-- > 0;) {
      const PackConstraint& c = pack->pack_constraints[k];
      Path::t path;
      try {
        path = type_path(unflatten(std::vector<std::string_view>(c.path.begin(), c.path.end())));
      } catch (const env::NotFound&) {
        continue;
      }
      const TypeDeclaration* decl;
      try {
        decl = env::find_type(path, env);
      } catch (const env::NotFound&) {
        continue;
      }
      if (decl->type_arity > 0) continue;
      TypeExpr* t = btype::newgenty(tconstr(path, {}, make<MemoRef>(mnil())));
      fl2.insert(fl2.begin(), PackConstraint{c.path, t});
    }
  }
  const ModuleType* mty;
  if (pack->pack_constraints.empty()) {
    mty = mty_ident(pack->pack_path);
  } else {
    auto* p = make<Package>(pack->pack_path, slice(fl2));
    mty = modtype_of_package(env, modl->mod_loc, p);
  }
  for (auto& c : fl2) {
    try {
      ctype::unify(env, c.ty, ctype::newvar());
    } catch (const ctype::Unify&) {
      Error e = err(modl->mod_loc, env, EK::Scoping_pack);
      e.lid = unflatten(std::vector<std::string_view>(c.path.begin(), c.path.end()));
      e.ty = c.ty;
      raise_error(e);
    }
  }
  const tt::ModuleExpr* modl2 = wrap_constraint_package_(env, true, modl, mty, nullptr);
  return {modl2, make<Package>(pack->pack_path, slice(fl2))};
}

// ---- fill in the forward declarations ---------------------------------------------------------
void install_forward_refs() {
  // The idents ocamlc creates while its modules initialize, before the first
  // Ident.reinit records the stamp level, in link order (shape, parmatch,
  // typeclass): every later stamp, the saved .cmi's included, counts them.
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    (void)shape::for_unnamed_functor_param();
    (void)env::initial();  // Env.initial (its type ids: module-init ids)
    parmatch::module_init();
    typeclass::module_init();
  }
  typecore::type_module = [](env::t env, const pt::ModuleExpr* smod) {
    auto [me, s] = type_module_alias_with_shape(env, smod);
    return std::make_pair(me, static_cast<const void*>(s));
  };
  typecore::type_str_item = type_str_item_fwd;
  typetexp::transl_modtype_longident = [](const Location& loc, env::t env, Longident::t lid) {
    return env::lookup_modtype_path(true, loc, lid, env);
  };
  typetexp::transl_modtype = transl_modtype;
  typecore::type_open = [](std::shared_ptr<bool> used_slot, OverrideFlag ovf, env::t env, const Location& loc, const pt::LidLoc& lid) {
    return type_open_(used_slot, false, ovf, env, loc, lid);
  };
  typetexp::type_open = typecore::type_open;
  typecore::type_open_decl = [](std::shared_ptr<bool> used_slot, env::t env, const pt::OpenDeclaration* od) {
    TypeOpenDeclResult r = type_open_decl_(used_slot, false, false, create_signature_names(), env, od);
    return typecore::TypeOpenDeclResult{r.od, r.sg, r.env};
  };
  typecore::type_package = type_package;
  typecore::check_package_closed = check_package_closed_;
  typeclass::type_open_descr = [](std::shared_ptr<bool> used_slot, env::t env, const pt::OpenDescription* od) {
    return type_open_descr(used_slot, false, env, od);
  };
  type_module_type_of_fwd = type_module_type_of;
  typetexp::check_package_with_type_constraints = merge::check_package_with_type_constraints;
  ctype::package_subtype = package_subtype_;
  ctype::set_modtype_of_package(modtype_of_package);
  env::check_well_formed_module = check_well_formed_module;
  env::add_delayed_check_forward = typecore::add_delayed_check;
  typeclass::install_forward_refs();
  includemod::install_forward_refs();
}

// ---- typecheck an implementation file -------------------------------------------------------
// Filename.remove_extension
static std::string remove_extension(const std::string& name) {
  std::size_t slash = name.rfind('/');
  std::size_t dot = name.rfind('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return name;
  std::size_t base = slash == std::string::npos ? 0 : slash + 1;
  // a basename made of leading dots only has no extension
  bool only_dots = true;
  for (std::size_t k = base; k < dot; ++k) only_dots = only_dots && name[k] == '.';
  if (only_dots) return name;
  return name.substr(0, dot);
}
// Unit_info.modname_from_source: the basename up to its first '.', capitalized
static std::string modname_of_filename(const std::string& file) {
  std::size_t slash = file.rfind('/');
  std::string base = slash == std::string::npos ? file : file.substr(slash + 1);
  std::size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  if (!base.empty() && base[0] >= 'a' && base[0] <= 'z') base[0] = static_cast<char>(base[0] - 'a' + 'A');
  return base;
}
static tt::Implementation type_implementation_(const UnitInfo& target, env::t initial_env, pt::Structure ast);

// Typemod.type_implementation's save_cmt: the unit's .cmt (Unit_info.cmt)
static void save_cmt(const UnitInfo& target, const cmt_format::BinaryAnnots& annots, env::t initial_env,
                     const cmi_format::CmiInfos* cmi, shape::t shape) {
  cmt_format::save_cmt(target.prefix + ".cmt", target.modname, target.source_file, annots, initial_env, cmi, shape);
  cmt_format::gen_annot(annots);
}

tt::Implementation type_implementation(const UnitInfo& target, env::t initial_env, pt::Structure ast) {
  cmt_format::clear();
  shape_reduce::reset();
  try {
    return type_implementation_(target, initial_env, ast);
  } catch (...) {
    // ~exceptionally: the saved parts as a partial .cmt
    cmt_format::BinaryAnnots annots{cmt_format::BinaryAnnots::Kind::Partial_implementation};
    annots.parts = cmt_format::get_saved_types();
    try {
      save_cmt(target, annots, initial_env, nullptr, nullptr);
    } catch (...) {
    }
    throw;
  }
}

static tt::Implementation type_implementation_(const UnitInfo& target, env::t initial_env, pt::Structure ast) {
  // (the typing recovery is not ported)
  typecore::reset_delayed_checks();
  env::reset_required_globals();
  if (clflags::print_types)  // #7656
    warnings::parse_options(false, "-32-34-37-38-60");
  TypeStructureResult r = type_structure(initial_env, ast);
  // Ident.create_persistent modname: the unit's one name string
  Ident::t unit_id = Ident::create_persistent(uid::unit_name_string(target.modname));
  shape::t shape0 = shape::set_uid_if_none(r.shape, uid::of_compilation_unit_id(ident::name(unit_id)));
  Signature simple_sg = simplify(r.env, r.names, r.sg);
  if (clflags::print_types) {
    typecore::force_delayed_checks();
    // Format.fprintf std_formatter "%a@." (Printtyp.printed_signature sourcefile) simple_sg
    printtyp::wrap_printing_env(false, initial_env, [&] {
      format_doc::Formatter d;
      printtyp::printed_signature(target.human(), d, simple_sg);
      format::Formatter out;
      format_doc::format(out, d.doc);
      out.print_newline();
      std::fwrite(out.contents().data(), 1, out.contents().size(), stdout);
      std::fflush(stdout);
    });
    // (the result is ignored by Compile.implementation)
    return {r.str, tt::tcoerce_none(), simple_sg};
  }
  // Unit_info.mli_from_source: the (human) source's prefix and
  // Config.interface_suffix
  std::string source_intf = remove_extension(target.human()) + config::interface_suffix;
  struct stat sb;
  if (clflags::cmi_file || ::stat(source_intf.c_str(), &sb) == 0) {
    std::string compiled_intf_file, intf_modname = target.modname;
    if (clflags::cmi_file) {
      // Unit_info.Artifact.from_filename: the unit named after the file
      compiled_intf_file = *clflags::cmi_file;
      intf_modname = modname_of_filename(compiled_intf_file);
    } else {
      // Unit_info.find_normalized_cmi: the unit's .cmi in the load path
      try {
        compiled_intf_file = load_path::find_normalized(target.modname + ".cmi");
      } catch (const load_path::NotFound&) {
        Error e(location::in_file(target.human()), env::empty(), EK::Interface_not_compiled);
        e.name = source_intf;
        typing_recovery::log_and_raise(e);
      }
    }
    Signature dclsig = env::read_signature(intf_modname, compiled_intf_file);
    auto [coercion, shape] =
        includemod::compunit(initial_env, true, target.human(), r.sg, source_intf, dclsig, shape0);
    typecore::force_delayed_checks();
    // It is important to run these checks after the inclusion test above,
    // so that value declarations which are not used internally but
    // exported are not reported as being unused.
    shape::t reduced = shape_reduce::local_reduce_empty(shape);
    cmt_format::BinaryAnnots annots{cmt_format::BinaryAnnots::Kind::Implementation};
    annots.structure = r.str;
    save_cmt(target, annots, initial_env, nullptr, reduced);
    return {r.str, coercion, dclsig};
  }
  location::prerr_warning(location::in_file(target.human()),
                          warnings::Warning::make(warnings::Warning::K::Missing_mli));
  auto [coercion, shape] = includemod::compunit(initial_env, true, target.human(), r.sg, "(inferred signature)",
                                                simple_sg, shape0);
  check_nongen_signature(r.env, simple_sg);
  normalize_signature(simple_sg);
  typecore::force_delayed_checks();
  shape::t reduced = shape_reduce::local_reduce_empty(shape);
  StrMap<std::string_view> alerts = builtin_attributes::alerts_of_str(true, ast);
  if (!clflags::dont_write_files) {
    cmi_format::CmiInfos cmi = env::save_signature(alerts, simple_sg, target.modname, target.prefix + ".cmi");
    cmt_format::BinaryAnnots annots{cmt_format::BinaryAnnots::Kind::Implementation};
    annots.structure = r.str;
    save_cmt(target, annots, initial_env, &cmi, reduced);
  }
  return {r.str, coercion, simple_sg};
}

const tt::Signature* type_interface(const UnitInfo&, env::t env, pt::Signature ast) {
  return transl_signature(env, ast);
}

// Env.unit_name_of_filename: a .cmi's unit (Unit_info.strict_modname_from_
// source: the basename up to its first '.', capitalized), if a valid name
static std::optional<std::string> unit_name_of_filename(const std::string& fn) {
  std::size_t dot = fn.rfind('.');
  if (dot == std::string::npos || fn.substr(dot) != ".cmi") return std::nullopt;  // Filename.extension
  std::string stem = fn.substr(0, fn.find('.'));
  if (!stem.empty() && stem[0] >= 'a' && stem[0] <= 'z') stem[0] = static_cast<char>(stem[0] - 'a' + 'A');
  if (!oprint::is_valid_identifier(stem)) return std::nullopt;  // Unit_info.is_unit_name
  return stem;
}

// Env.persistent_structures_of_dir: the units of a load-path directory's
// file list (Load_path.Dir.files)
static std::set<std::string> persistent_structures_of_dir(const std::vector<std::string>& files) {
  std::set<std::string> out;
  for (const std::string& f : files)
    if (std::optional<std::string> u = unit_name_of_filename(f)) out.insert(*u);
  return out;
}

env::t initial_env(const Location& loc, const std::optional<std::string>& initially_opened_module,
                   const std::vector<std::string>& open_implicit_modules) {
  env::t env = env::initial();
  auto open_module = [&](env::t e, const std::string& m) {
    // (Parse.simple_module_path of the -open argument: a dotted path)
    Longident::t lid = nullptr;
    std::size_t start = 0;
    for (;;) {
      std::size_t dot = m.find('.', start);
      std::string_view part = zborrow(m.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
      lid = lid ? Longident::ldot(lid, location::none(), part, location::none()) : Longident::lident(part);
      if (dot == std::string::npos) break;
      start = dot + 1;
    }
    return type_open_(nullptr, false, OverrideFlag::Override, e, loc, pt::LidLoc{lid, loc}).second;
  };
  auto add_units = [](env::t e, const std::set<std::string>& units) {
    for (auto& name : units) e = env::add_persistent_structure(Ident::create_persistent(zborrow(name)), e);
    return e;
  };
  std::vector<std::set<std::string>> units;
  // List.map Env.persistent_structures_of_dir (Load_path.get_visible ())
  for (auto& files : load_path::visible_dir_files()) units.push_back(persistent_structures_of_dir(files));
  if (initially_opened_module) {
    // Locate the directory that contains [m], add the units it contains to
    // the environment and open [m] in the resulting environment.
    std::size_t k = 0;
    for (; k < units.size(); ++k)
      if (units[k].count(*initially_opened_module)) break;
    if (k < units.size()) {
      env = add_units(env, units[k]);
      // List.rev_append before after: the directories before k, reversed,
      // then those after
      std::vector<std::set<std::string>> other;
      for (std::size_t j = k; j-- > 0;) other.push_back(units[j]);
      for (std::size_t j = k + 1; j < units.size(); ++j) other.push_back(units[j]);
      units = other;
    }
    env = open_module(env, *initially_opened_module);
  }
  for (auto& u : units) env = add_units(env, u);
  for (auto& m : open_implicit_modules) env = open_module(env, m);
  return env;
}

}  // namespace cppcaml::typing::typemod
