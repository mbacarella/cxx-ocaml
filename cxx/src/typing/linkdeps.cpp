// Port of utils/linkdeps.ml; see linkdeps.hpp.
#include "cppcaml/typing/linkdeps.hpp"

#include "cppcaml/typing/hashtbl.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::linkdeps {

namespace {
struct HashString {
  long operator()(const std::string& s) const { return hashtbl::hash_string(s); }
};
struct HashCompunitAndSource {
  long operator()(const CompunitAndSource& x) const {
    return hashtbl::hash_string_record({&x.compunit, &x.filename});
  }
};
}  // namespace

struct T::Tables {
  hashtbl::Hashtbl<std::string, Refs, HashString> missing_compunits{17};
  hashtbl::Hashtbl<std::string, std::vector<std::string>, HashString> provided_compunits{17};
  hashtbl::Hashtbl<CompunitAndSource, Refs, HashCompunitAndSource> badly_ordered_deps{17};
};

T::T(bool complete) : complete_(complete), t_(std::make_unique<Tables>()) {}
T::~T() = default;

bool T::required(const std::string& compunit) const { return t_->missing_compunits.mem(compunit); }

void T::add(const std::string& filename, const std::string& compunit, const std::vector<std::string>& provides,
            const std::vector<std::string>& requires_) {
  CompunitAndSource by{compunit, filename};
  // add_required t by name
  auto add_required = [&](const std::string& name) {
    // update t k f = Hashtbl.replace t k (f (Hashtbl.find_opt t k)), f adding
    // [by] to the set: an existing binding's set grows in place (a copy per
    // add is quadratic, where OCaml's Set.add shares)
    auto update = [&](auto& tbl, const auto& k) {
      if (Refs* r = tbl.find_mut(k)) r->insert(by);
      else tbl.replace(k, Refs{by});
    };
    if (const std::vector<std::string>* files = t_->provided_compunits.find_opt(name))
      update(t_->badly_ordered_deps, CompunitAndSource{name, files->front()});
    update(t_->missing_compunits, name);
  };
  for (const std::string& r : requires_) add_required(r);
  for (const std::string& p : provides) {
    t_->missing_compunits.remove(p);
    const std::vector<std::string>* l0 = t_->provided_compunits.find_opt(p);
    std::vector<std::string> l;
    l.push_back(filename);  // filename :: l
    if (l0) l.insert(l.end(), l0->begin(), l0->end());
    t_->provided_compunits.replace(p, l);
  }
}

std::optional<Error> T::check() const {
  Error e{};
  for (auto& [k, v] : t_->missing_compunits.to_seq())
    e.missing.emplace_back(k, std::vector<CompunitAndSource>(v.begin(), v.end()));
  for (auto& [k, v] : t_->badly_ordered_deps.to_seq())
    e.wrong_order.emplace_back(k, std::vector<CompunitAndSource>(v.begin(), v.end()));
  for (auto& [k, files] : t_->provided_compunits.to_seq())
    if (files.size() > 1) e.multiple.emplace_back(k, files);
  if (e.multiple.empty() && e.wrong_order.empty() && e.missing.empty()) return std::nullopt;
  if (e.multiple.empty() && e.wrong_order.empty()) {
    if (!complete_) return std::nullopt;
    e.kind = Error::Kind::Missing_implementations;
    return e;
  }
  if (e.multiple.empty()) {
    e.kind = Error::Kind::Wrong_link_order;
    return e;
  }
  e.kind = Error::Kind::Multiple_definitions;
  return e;
}

// ---- report_error_doc ~print_filename:Location.Doc.filename -------------------

namespace {
namespace fd = format_doc;
using fd::Formatter;
using fd::fprintf;

void print_filename(Formatter& ppf, const std::string& f) { location::doc::filename(ppf, f); }

// print_reference print_fname ppf {compunit; filename}
void print_reference(Formatter& ppf, const CompunitAndSource& r) {
  fprintf(ppf, "%a (%a)", misc::style::code_str(r.compunit), [&](Formatter& f) { print_filename(f, r.filename); });
}

template <class T, class P>
void pp_list_comma(Formatter& ppf, P elt, const std::vector<T>& l) {
  fd::pp_print_list(ppf, elt, l, fd::comma);
}
}  // namespace

void report_error_doc(Formatter& ppf, const Error& e) {
  switch (e.kind) {
    case Error::Kind::Missing_implementations: {
      auto print_modules = [&](Formatter& f) {
        for (auto& [md, rq] : e.missing)
          fprintf(f, "@ @[<hov 2>%a referenced from %a@]", misc::style::code_str(md),
                  [&](Formatter& g) { pp_list_comma(g, print_reference, rq); });
      };
      fprintf(ppf, "@[<v 2>No implementation provided for the following modules:%t@]", print_modules);
      break;
    }
    case Error::Kind::Wrong_link_order: {
      auto depends_on = [](Formatter& f, const std::pair<CompunitAndSource, std::vector<CompunitAndSource>>& x) {
        fprintf(f, "@ @[<hov 2>%a depends on %a@]", [&](Formatter& g) { pp_list_comma(g, print_reference, x.second); },
                [&](Formatter& g) { print_reference(g, x.first); });
      };
      fprintf(ppf, "@[<hov 2>Wrong link order:%a@]", [&](Formatter& f) { pp_list_comma(f, depends_on, e.wrong_order); });
      break;
    }
    case Error::Kind::Multiple_definitions: {
      auto print = [](Formatter& f, const std::pair<std::string, std::vector<std::string>>& x) {
        fprintf(f, "@ @[<hov>Multiple definitions of module %a in files %a@]", misc::style::code_str(x.first),
                [&](Formatter& g) {
                  pp_list_comma(
                      g, [](Formatter& h, const std::string& file) { misc::style::as_inline_code(print_filename, h, file); },
                      x.second);
                });
      };
      fprintf(ppf, "@[<hov 2> Duplicated implementations:%a@]", [&](Formatter& f) { pp_list_comma(f, print, e.multiple); });
      break;
    }
  }
}

}  // namespace cppcaml::typing::linkdeps
