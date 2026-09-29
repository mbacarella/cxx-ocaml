// The error reports of lambda/ (Translmod, Translprim, Translcore,
// Translclass, Tmc): see reporters.hpp.
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/reporters.hpp"
#include "cppcaml/typing/tmc.hpp"
#include "cppcaml/typing/translclass.hpp"
#include "cppcaml/typing/translcore.hpp"
#include "cppcaml/typing/translmod.hpp"
#include "cppcaml/typing/translprim.hpp"

namespace cppcaml::typing::reporters {

namespace {

namespace fd = format_doc;
using fd::doc_printf;
using fd::Formatter;
using fd::fprintf;
using location::Msg;
using location::Report;
using misc::style::code_str;

// Translmod.collect_components / get_relative_path
void collect_components(Path::t p, std::vector<std::string>& out) {
  switch (p->kind) {
    case Path::Kind::Pident: out.push_back(std::string(ident::name(p->id))); break;
    case Path::Kind::Pdot:
      collect_components(p->p1, out);
      out.push_back(std::string(p->s));
      break;
    case Path::Kind::Papply:
    case Path::Kind::Pextra_ty: collect_components(p->p1, out); break;
  }
}
std::string get_relative_path(const std::string& top_module, Path::t path) {
  std::vector<std::string> comps;
  collect_components(path, comps);
  if (comps.size() >= 2 && comps[0] == top_module) comps.erase(comps.begin());
  std::string r;
  for (std::size_t i = 0; i < comps.size(); ++i) {
    if (i) r += ".";
    r += comps[i];
  }
  return r;
}

Msg explanation_submsg(Ident::t id, const translmod::UnsafeInfo& u) {
  if (u.unnamed) throw std::logic_error("Translmod.explanation_submsg");
  std::string top_module(ident::name(id));
  std::string guilty = get_relative_path(top_module, u.path);
  const char* fmt = "";
  using R = translmod::UnsafeInfo::Reason;
  switch (u.reason) {
    case R::Unsafe_module_binding: fmt = "Module %a defines an unsafe module, %a ."; break;
    case R::Unsafe_functor: fmt = "Module %a defines an unsafe functor, %a ."; break;
    case R::Unsafe_typext: fmt = "Module %a defines an unsafe extension constructor, %a ."; break;
    case R::Unsafe_non_function: fmt = "Module %a defines an unsafe value, %a ."; break;
  }
  return Msg{u.loc, doc_printf(fmt, code_str(top_module), code_str(guilty))};
}

Report translmod_error(const translmod::Error& e) {
  if (e.kind == translmod::Error::Kind::Circular_dependency) {
    std::vector<Msg> sub;
    for (auto& [id, u] : e.cycle) sub.push_back(explanation_submsg(id, u));
    auto cycle = e.cycle;
    auto print_cycle = [cycle](Formatter& ppf) {
      auto pp_sep = [](Formatter& f) { fprintf(f, "@ -> "); };
      fd::pp_print_list(ppf, [](Formatter& f, const std::pair<Ident::t, translmod::UnsafeInfo>& x) {
        fd::pp_print_string(f, ident::name(x.first));
      }, cycle, pp_sep);
      pp_sep(ppf);
      fd::pp_print_string(ppf, ident::name(cycle.front().first));
    };
    return location::errorf_sub(e.loc, std::move(sub),
                                "Cannot safely evaluate the definition of the following cycle@ of "
                                "recursively-defined modules:@ %a.@ There are no safe modules in this cycle@ %a.",
                                print_cycle, [](Formatter& f) { misc::print_see_manual(f, {12, 2}); });
  }
  return location::errorf(location::none(), "@[Conflicting %a attributes@]", code_str("inline"));
}

Report tmc_error(const tmc::Error& e) {
  std::vector<Msg> sub;
  for (const auto& arg : e.arguments)
    for (const tmc::TmcCallInformation& info : arg) {
      if (e.explicit_ && !info.explicit_) continue;
      sub.push_back(location::msg(debuginfo::to_location(info.loc), e.explicit_ ? "This call is explicitly annotated."
                                                                               : "This call could be annotated."));
    }
  if (!e.explicit_)
    return location::errorf_sub(
        e.loc, std::move(sub), "%t", [](Formatter& ppf) {
          fprintf(ppf,
                  "%a:@ this@ constructor@ application@ may@ be@ TMC-transformed@ in@ several@ different@ ways.@ "
                  "Please@ disambiguate@ by@ adding@ an@ explicit@ %a attribute@ to@ the@ call@ that@ should@ be@ "
                  "made@ tail-recursive,@ or@ a@ %a attribute@ on@ calls@ that@ should@ not@ be@ transformed.",
                  code_str("[@tail_mod_cons]"), code_str("[@tailcall]"), code_str("[@tailcall false]"));
        });
  return location::errorf_sub(e.loc, std::move(sub), "%t", [](Formatter& ppf) {
    fprintf(ppf,
            "%a:@ this@ constructor@ application@ may@ be@ TMC-transformed@ in@ several@ different@ ways.@ Only@ "
            "one@ of@ the@ arguments@ may@ become@ a@ TMC@ call,@ but@ several@ arguments@ contain@ calls@ that@ "
            "are@ explicitly@ marked@ as@ tail-recursive.@ Please@ fix@ the@ conflict@ by@ reviewing@ and@ fixing@ "
            "the@ conflicting@ annotations.",
            code_str("[@tail_mod_cons]"));
  });
}

}  // namespace

void register_translate() {
  location::register_error_of_exn([](std::exception_ptr ep) -> std::optional<Report> {
    try {
      std::rethrow_exception(ep);
    } catch (const translmod::Error& e) {
      return translmod_error(e);
    } catch (const translprim::Error& e) {
      bool unknown = e.kind == translprim::Error::Kind::Unknown_builtin_primitive;
      std::string n = e.name;
      return location::error_of_printer(e.loc, [unknown, n](Formatter& ppf) {
        if (unknown)
          fprintf(ppf, "Unknown builtin primitive %a", code_str(n));
        else
          fprintf(ppf, "Wrong arity for builtin primitive %a", code_str(n));
      });
    } catch (const translcore::Error& e) {
      bool super = e.kind == translcore::Error::Kind::Free_super_var;
      return location::error_of_printer(e.loc, [super](Formatter& ppf) {
        if (super)
          fprintf(ppf, "Ancestor names can only be used to select inherited methods");
        else
          fprintf(ppf, "Unreachable expression was reached");
      });
    } catch (const translclass::Error& e) {
      std::string a = e.a, b = e.b;
      return location::error_of_printer(e.loc, [a, b](Formatter& ppf) {
        fprintf(ppf, "Method labels %a and %a are incompatible.@ %s", code_str(a), code_str(b), "Change one of them.");
      });
    } catch (const tmc::Error& e) {
      return tmc_error(e);
    } catch (...) {
    }
    return std::nullopt;
  });
}

}  // namespace cppcaml::typing::reporters
