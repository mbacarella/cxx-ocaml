// Ports of utils/load_path.ml, utils/consistbl.ml and
// typing/persistent_env.ml (TYPECHECKER.md).
#pragma once

#include <cstdio>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/cmi_format.hpp"
#include "cppcaml/typing/lazy_backtrack.hpp"

namespace cppcaml::typing {

// ---- Load_path -----------------------------------------------------------
namespace load_path {

enum class Visibility { Visible, Hidden };

struct NotFound {};

// Misc.normalized_unit_filename (Utf8_lexeme.uncapitalize); ASCII names only
// -- a non-ASCII first character is kept as is.
std::optional<std::string> normalized_unit_filename(const std::string& fn);

void reset();
// Load_path.init ~visible ~hidden (no auto-include of otherlibs)
void init(const std::vector<std::string>& visible, const std::vector<std::string>& hidden);
void add_dir(bool hidden, const std::string& dir);
std::vector<std::string> get_path_list();
// Load_path.get_paths (): { visible; hidden }
std::pair<std::vector<std::string>, std::vector<std::string>> get_paths();
// Load_path.get_visible () |> List.map Dir.files: each visible directory's
// file names ("" read as the current directory), in the path's order
std::vector<std::vector<std::string>> visible_dir_files();
// raise NotFound
std::string find(const std::string& fn);
std::pair<std::string, Visibility> find_normalized_with_visibility(const std::string& fn);
std::string find_normalized(const std::string& fn);

}  // namespace load_path

// ---- Consistbl (Module_name = String) --------------------------------------
class Consistbl {
 public:
  struct Inconsistency {
    std::string unit_name, inconsistent_source, original_source;
  };
  struct NotAvailable {
    std::string name;
  };
  void clear() { tbl_.clear(); }
  // raises Inconsistency
  void check(const std::string& name, const std::string& crc, const std::string& source);
  void check_noadd(const std::string& name, const std::string& crc, const std::string& source);
  std::string source(const std::string& name) const;
  // extract l tbl: sorted-unique names, consed -> reverse sorted order
  std::vector<std::pair<std::string, std::optional<std::string>>> extract(
      const std::vector<std::string>& l) const;

 private:
  std::unordered_map<std::string, std::pair<std::string, std::string>> tbl_;
};

// ---- Persistent_env ----------------------------------------------------------
namespace persistent_env {

struct Error : std::runtime_error {
  enum class Kind { Illegal_renaming, Inconsistent_import, Need_recursive_types };
  Kind kind;
  std::string a, b, c;  // Illegal_renaming(modname, ps_name, filename) /
                        // Inconsistent_import(name, source1, source2) /
                        // Need_recursive_types(modname)
  Error(Kind k, std::string a_, std::string b_ = {}, std::string c_ = {});
};

struct PersistentSignature {
  std::string filename;
  cmi_format::CmiInfos cmi;
  load_path::Visibility visibility;
};

// Persistent_signature.load: find `unit_name.cmi` on the load path and read it.
// (Replaceable, like the OCaml ref.)
extern std::function<std::optional<PersistentSignature>(bool allow_hidden,
                                                        const std::string& unit_name)>
    load;

struct PersStruct {
  std::string ps_name;
  std::vector<std::pair<std::string, std::optional<std::string>>> ps_crcs;
  std::string ps_filename;
  std::vector<cmi_format::PersFlag> ps_flags;
  load_path::Visibility ps_visibility;
};


template <class PM>
class PersistentEnv {
 public:
  struct Info {  // Missing | Found of pers_struct * 'a
    bool found;
    PersStruct ps;
    PM pm{};
  };

  void clear() {
    persistent_structures_.clear();
    imported_units_.clear();
    imported_opaque_units_.clear();
    crc_units_.clear();
    can_load_cmis_ = true;
    log_ = nullptr;
  }

  void clear_missing() {
    for (auto it = persistent_structures_.begin(); it != persistent_structures_.end();)
      it = it->second.found ? std::next(it) : persistent_structures_.erase(it);
  }

  // String.Set.add keeps the set's own string when the name is already in
  // it: the first string added for a name is the one Env.imports returns
  // (Emitcode's cu_imports shares it -- e.g. with the name of the persistent
  // ident a required global is), so keep its identity.
  void add_import(std::string_view s) {
    if (!imported_units_.count(s)) imported_units_.emplace(std::string(s), zborrow(s));
  }
  // the string Env.imports carries for [name] (added by add_import)
  std::string_view import_name(std::string_view name) const {
    auto it = imported_units_.find(name);
    return it == imported_units_.end() ? std::string_view{} : it->second;
  }
  void register_import_as_opaque(const std::string& s) { imported_opaque_units_.insert(s); }

  const PM* find_in_cache(const std::string& s) const {
    auto it = persistent_structures_.find(s);
    if (it == persistent_structures_.end() || !it->second.found) return nullptr;
    return &it->second.pm;
  }

  void import_crcs(const std::string& source,
                   const std::vector<std::pair<std::string, std::optional<std::string>>>& crcs) {
    for (auto& [name, crco] : crcs) {
      if (!crco) continue;
      add_import(name);
      crc_units_.check(name, *crco, source);
    }
  }

  void check_consistency(const PersStruct& ps) {
    try {
      import_crcs(ps.ps_filename, ps.ps_crcs);
    } catch (const Consistbl::Inconsistency& e) {
      throw Error(Error::Kind::Inconsistent_import, e.unit_name, e.original_source,
                  e.inconsistent_source);
    }
  }

  bool can_load_cmis() const { return can_load_cmis_; }

  // without_cmis f: run f with cmi loading disabled, then undo the lazy
  // forcings it logged (Lazy_backtrack.backtrack).
  template <class F>
  auto without_cmis(F&& f) -> decltype(f()) {
    LazyLog log;
    bool saved = can_load_cmis_;
    LazyLog* saved_log = log_;
    can_load_cmis_ = false;
    log_ = &log;
    struct Restore {
      PersistentEnv& e;
      bool saved;
      LazyLog* saved_log;
      ~Restore() {
        e.can_load_cmis_ = saved;
        e.log_ = saved_log;
      }
    };
    decltype(f()) res = [&] {
      Restore r{*this, saved, saved_log};
      return f();
    }();
    backtrack(log);
    return res;
  }
  // the Cannot_load_cmis log (null when cmis can be loaded)
  LazyLog* cannot_load_log() const { return log_; }

  template <class F>
  void fold(F&& f) const {
    for (auto& [name, info] : persistent_structures_)
      if (info.found) f(name, info.pm);
  }

  // find_pers_struct ~allow_hidden penv val_of_pers_sig check name
  // throws load_path::NotFound when there is no such unit
  const Info& find_pers_struct(bool allow_hidden,
                               const std::function<PM(const PersistentSignature&)>& val_of_pers_sig,
                               bool check, std::string_view name) {
    if (name == "*predef*") throw load_path::NotFound{};
    auto it = persistent_structures_.find(name);
    if (it != persistent_structures_.end()) {
      if (!it->second.found) throw load_path::NotFound{};
      if (allow_hidden || it->second.ps.ps_visibility == load_path::Visibility::Visible)
        return it->second;
      throw load_path::NotFound{};
    }
    if (!can_load_cmis_) throw load_path::NotFound{};
    std::optional<PersistentSignature> psig = load(allow_hidden, std::string(name));
    if (!psig) {
      if (allow_hidden) persistent_structures_[std::string(name)] = Info{false, {}, {}};
      throw load_path::NotFound{};
    }
    add_import(name);
    PM pm = val_of_pers_sig(*psig);
    return acknowledge_pers_struct(check, std::string(name), *psig, pm);
  }

  // read penv f cmi: read_pers_struct ~check:true
  PM read(const std::function<PM(const PersistentSignature&)>& f, const std::string& modname,
          const std::string& filename) {
    add_import(modname);
    PersistentSignature pers_sig{filename, cmi_format::read_cmi(filename), load_path::Visibility::Visible};
    PM pm = f(pers_sig);
    return acknowledge_pers_struct(true, modname, pers_sig, pm).pm;
  }

  PM find(bool allow_hidden, const std::function<PM(const PersistentSignature&)>& f,
          std::string_view name) {
    return find_pers_struct(allow_hidden, f, true, name).pm;
  }

  // `check`: record the weak dependency (the No_cmi_file warning check is
  // not ported yet).
  void check(std::string_view name) {
    if (!persistent_structures_.count(name)) add_import(name);
  }

  std::string crc_of_unit(const std::function<PM(const PersistentSignature&)>& f,
                          const std::string& name) {
    const Info& info = find_pers_struct(true, f, true, name);
    for (auto& [n, crc] : info.ps.ps_crcs)
      if (n == name) {
        if (!crc) throw std::logic_error("Persistent_env.crc_of_unit");
        return *crc;
      }
    throw std::logic_error("Persistent_env.crc_of_unit");
  }

  std::vector<std::pair<std::string, std::optional<std::string>>> imports() const {
    std::vector<std::string> l;
    for (auto& [name, _] : imported_units_) l.push_back(name);
    return crc_units_.extract(l);
  }
  bool looked_up(const std::string& modname) const {
    return persistent_structures_.count(modname) != 0;
  }
  bool is_imported(std::string_view s) const { return imported_units_.count(s) != 0; }
  bool is_imported_opaque(const std::string& s) const {
    return imported_opaque_units_.count(s) != 0;
  }

  // make_cmi penv modname sign alerts
  cmi_format::CmiInfos make_cmi(const std::string& modname, Signature sign, StrMap<std::string_view> alerts) const {
    cmi_format::CmiInfos c;
    c.cmi_name = zborrow(modname);
    c.cmi_sign = sign;
    c.cmi_crcs = imports();
    if (clflags::recursive_types) c.cmi_flags.push_back({cmi_format::PersFlag::Kind::Rectypes, {}});
    if (clflags::opaque) c.cmi_flags.push_back({cmi_format::PersFlag::Kind::Opaque, {}});
    c.cmi_flags.push_back({cmi_format::PersFlag::Kind::Alerts, alerts});
    return c;
  }

  // save_cmi penv psig pm: write the .cmi, then enter it in the persistent
  // table so that imports() also returns its crc (save_pers_struct).
  // Returns the crc.
  std::string save_cmi(const PersistentSignature& psig, const PM& pm) {
    const cmi_format::CmiInfos& cmi = psig.cmi;
    std::string crc;
    try {
      crc = cmi_format::output_cmi(psig.filename, cmi);
    } catch (...) {
      std::remove(psig.filename.c_str());
      throw;
    }
    std::string modname(cmi.cmi_name);
    PersStruct ps{modname, cmi.cmi_crcs, psig.filename, cmi.cmi_flags, psig.visibility};
    ps.ps_crcs.insert(ps.ps_crcs.begin(), {modname, crc});
    persistent_structures_.insert_or_assign(modname, Info{true, ps, pm});
    for (auto& f : ps.ps_flags)
      if (f.kind == cmi_format::PersFlag::Kind::Opaque) register_import_as_opaque(modname);
    crc_units_.check(modname, crc, psig.filename);
    add_import(modname);
    return crc;
  }

 private:
  const Info& acknowledge_pers_struct(bool check, const std::string& modname,
                                      const PersistentSignature& pers_sig, const PM& pm) {
    PersStruct ps{std::string(pers_sig.cmi.cmi_name), pers_sig.cmi.cmi_crcs, pers_sig.filename,
                  pers_sig.cmi.cmi_flags, pers_sig.visibility};
    if (ps.ps_name != modname)
      throw Error(Error::Kind::Illegal_renaming, modname, ps.ps_name, pers_sig.filename);
    for (auto& f : ps.ps_flags) {
      if (f.kind == cmi_format::PersFlag::Kind::Rectypes && !clflags::recursive_types)
        throw Error(Error::Kind::Need_recursive_types, ps.ps_name);
      if (f.kind == cmi_format::PersFlag::Kind::Opaque) register_import_as_opaque(modname);
    }
    if (check) check_consistency(ps);
    auto [it, _] = persistent_structures_.insert_or_assign(modname, Info{true, std::move(ps), pm});
    return it->second;
  }

  std::map<std::string, Info, std::less<>> persistent_structures_;
  std::map<std::string, std::string_view, std::less<>> imported_units_;
  std::set<std::string> imported_opaque_units_;
  Consistbl crc_units_;
  bool can_load_cmis_ = true;
  LazyLog* log_ = nullptr;
};

}  // namespace persistent_env

}  // namespace cppcaml::typing
