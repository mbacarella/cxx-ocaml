// Port of typing/typemod.ml, part 5: the signature of a packed unit
// (package_signatures, package_units) for -pack, and its .cmt (Packed).
#include <algorithm>
#include <filesystem>

#include "cppcaml/typing/cmt_format.hpp"

#include "cppcaml/typing/mtype.hpp"
#include "cppcaml/typing/shape.hpp"
#include "typemod_internal.hpp"

namespace cppcaml::typing::typemod {

namespace fs = std::filesystem;

namespace {

// Unit_info.lax_modname_from_source: the basename up to its first dot,
// capitalized
std::string modname_from_source(const std::string& f) {
  std::string base = fs::path(f).filename().string();
  std::size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  if (!base.empty() && base[0] >= 'a' && base[0] <= 'z') base[0] = static_cast<char>(base[0] - 'a' + 'A');
  return base;
}

// Misc.chop_extensions: the file name without the extensions of its basename
std::string chop_extensions(const std::string& f) {
  std::size_t slash = f.rfind('/');
  std::size_t start = slash == std::string::npos ? 0 : slash + 1;
  std::size_t dot = f.find('.', start);
  return dot == std::string::npos ? f : f.substr(0, dot);
}

// Filename.remove_extension
std::string remove_extension(const std::string& f) {
  std::size_t slash = f.rfind('/');
  std::size_t start = slash == std::string::npos ? 0 : slash + 1;
  std::size_t dot = f.rfind('.');
  if (dot == std::string::npos || dot < start || dot == start) return f;
  return f.substr(0, dot);
}

Location in_file(std::string_view name) {  // Location.in_file
  Position p{zborrow(name), 1, 0, -1};
  return Location{p, p, true};
}

struct PackUnit {
  std::string_view name;
  Signature sg;
};

}  // namespace

// package_signatures units
static Signature package_signatures(const std::vector<PackUnit>& units) {
  struct WithIds {
    Ident::t oldid, newid;
    Signature sg;
  };
  std::vector<WithIds> units_with_ids;
  for (const PackUnit& u : units) {
    Ident::t oldid = Ident::create_persistent(u.name);
    Ident::t newid = Ident::create_local(u.name);
    units_with_ids.push_back({oldid, newid, u.sg});
  }
  subst::t s = subst::identity();
  for (const WithIds& u : units_with_ids) s = subst::add_module(u.oldid, Path::pident(u.newid), s);
  std::vector<const SignatureItem*> out;
  for (const WithIds& u : units_with_ids) {
    // This signature won't be used for anything, it'll just be saved in a cmi
    Signature sg = subst::signature(subst::Scoping::make_local(), s, u.sg);
    Uid md_uid = uid::mk(env::get_current_unit());
    auto* md = make<ModuleDeclaration>(ModuleDeclaration{mty_signature(sg), {}, location::none(), md_uid});
    out.push_back(sig_module(u.newid, ModulePresence::Mp_present, md, RecStatus::Trec_not, Visibility::Exported));
  }
  return slice(out);
}

const tt::ModuleCoercion* package_units(env::t initial_env, const std::vector<std::string>& objfiles,
                                        const std::string& target_modname, const std::string& target_cmi) {
  // Read the signatures of the units
  std::vector<PackUnit> units;
  for (const std::string& f : objfiles) {
    // Unit_info.Artifact.modname: one string, the import set's if first
    std::string_view modname = zstr(modname_from_source(f));
    std::string cmi = chop_extensions(f) + ".cmi";  // Unit_info.companion_cmi
    Signature sg = env::read_signature_named(modname, cmi);
    bool is_cmi = f.size() >= 4 && f.compare(f.size() - 4, 4, ".cmi") == 0;
    if (is_cmi && !mtype::no_code_needed_sig(env::initial(), sg)) {
      Error e(location::none(), env::empty(), Error::Kind::Implementation_is_required);
      e.name = f;
      throw e;
    }
    units.push_back({modname, sg});
  }
  // Compute signature of packaged unit
  ident::reinit();
  Signature sg = package_signatures(units);
  // Compute the shape of the package
  std::string prefix = remove_extension(target_cmi);
  Uid pack_uid = uid::of_compilation_unit_id(ident::name(Ident::create_persistent(zborrow(prefix))));
  shape::ItemMap map = shape::map::empty();
  for (const PackUnit& u : units)
    map = shape::map::add_module(map, Ident::create_persistent(u.name), shape::for_persistent_unit(u.name));
  shape::t shape = shape::str(&pack_uid, map);
  // See if explicit interface is provided
  std::string mli = prefix + ".mli";
  if (fs::exists(mli)) {
    if (!fs::exists(target_cmi)) {
      Error e(in_file(mli), env::empty(), Error::Kind::Interface_not_compiled);
      e.name = mli;
      throw e;
    }
    Signature dclsig = env::read_signature(target_modname, target_cmi);
    auto [cc, shape_] = includemod::compunit(initial_env, true, "(obtained by packing)", sg, mli, dclsig, shape);
    (void)shape_;
    cmt_format::BinaryAnnots annots{cmt_format::BinaryAnnots::Kind::Packed};
    annots.packed_sg = sg;
    annots.packed_files = objfiles;
    cmt_format::save_cmt(prefix + ".cmt", target_modname, std::nullopt, annots, initial_env, nullptr, shape);
    return cc;
  }
  // Determine imports
  std::vector<std::pair<std::string, std::optional<std::string>>> imports;
  for (auto& imp : env::imports())
    if (std::none_of(units.begin(), units.end(), [&](const PackUnit& u) { return u.name == imp.first; }))
      imports.push_back(imp);
  // Write packaged signature
  if (!clflags::dont_write_files) {
    cmi_format::CmiInfos cmi =
        env::save_signature_with_imports(StrMap<std::string_view>{}, sg, target_modname, target_cmi, imports);
    cmt_format::BinaryAnnots annots{cmt_format::BinaryAnnots::Kind::Packed};
    annots.packed_sg = cmi.cmi_sign;
    annots.packed_files = objfiles;
    cmt_format::save_cmt(prefix + ".cmt", target_modname, std::nullopt, annots, initial_env, &cmi, shape);
  }
  return tt::tcoerce_none();
}

}  // namespace cppcaml::typing::typemod
