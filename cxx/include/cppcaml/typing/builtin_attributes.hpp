// Port of parsing/builtin_attributes.ml, as far as the typer has needed it.
//
// Deviation (TYPECHECKER.md): warnings are not ported yet, so
// `warning_scope` does not process [@warning] / [@ocaml.warning]
// attributes -- it only runs its body.  This matters for diagnostics only.
#pragma once

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "cppcaml/typing/parsetree.hpp"

namespace cppcaml::typing::builtin_attributes {

// attr_equals_builtin: `s` or `ocaml.s`
inline bool attr_equals_builtin(std::string_view txt, std::string_view s) {
  return txt == s || (txt.size() == 6 + s.size() && txt.substr(0, 6) == "ocaml." &&
                      txt.substr(6) == s);
}
inline bool has_attribute(std::string_view nm, const parsetree::Attributes& attrs) {
  for (const parsetree::Attribute* a : attrs)
    if (attr_equals_builtin(a->attr_name.txt, nm)) return true;
  return false;
}
// the same, over Types attributes (type_attributes, val_attributes, ...)
inline bool has_attribute(std::string_view nm, const Attributes& attrs) {
  for (const Attribute* a : attrs)
    if (attr_equals_builtin(a->attr_name, nm)) return true;
  return false;
}
inline bool explicit_arity(const parsetree::Attributes& attrs) {
  return has_attribute("explicit_arity", attrs);
}

// string_of_payload / string_of_opt_payload: a lone string constant
inline std::optional<std::string_view> string_of_payload(const parsetree::Payload& p) {
  using namespace parsetree;
  if (p.kind != Payload::Kind::PStr || p.str.size() != 1) return std::nullopt;
  auto* ev = as<Pstr_eval>(p.str[0]->pstr_desc);
  if (!ev) return std::nullopt;
  auto* c = as<Pexp_constant>(ev->exp->pexp_desc);
  if (!c || c->c.pconst_desc.kind != ConstantDesc::Kind::Pconst_string) return std::nullopt;
  return c->c.pconst_desc.s;
}
inline std::string string_of_opt_payload(const parsetree::Payload& p) {
  std::optional<std::string_view> s = string_of_payload(p);
  return s ? std::string(*s) : std::string();
}

// kind_and_message: `[@alert kind "message"]` / `[@alert kind]`
inline std::optional<std::pair<std::string, std::string>> kind_and_message(const parsetree::Payload& p) {
  using namespace parsetree;
  if (p.kind != Payload::Kind::PStr || p.str.size() != 1) return std::nullopt;
  auto* ev = as<Pstr_eval>(p.str[0]->pstr_desc);
  if (!ev) return std::nullopt;
  auto lident = [](const Expression* e) -> std::optional<std::string_view> {
    auto* id = as<Pexp_ident>(e->pexp_desc);
    if (!id || id->lid.txt->kind != Longident::Kind::Lident) return std::nullopt;
    return id->lid.txt->s;
  };
  if (auto* ap = as<Pexp_apply>(ev->exp->pexp_desc)) {
    std::optional<std::string_view> id = lident(ap->fn);
    if (!id || ap->args.size() != 1 || ap->args[0].label.kind != ArgLabel::Kind::Nolabel) return std::nullopt;
    auto* c = as<Pexp_constant>(ap->args[0].exp->pexp_desc);
    if (!c || c->c.pconst_desc.kind != ConstantDesc::Kind::Pconst_string) return std::nullopt;
    return std::make_pair(std::string(*id), std::string(c->c.pconst_desc.s));
  }
  if (std::optional<std::string_view> id = lident(ev->exp)) return std::make_pair(std::string(*id), std::string());
  return std::nullopt;
}

// alerts_of_attrs: kind -> message, the messages of one kind joined by "\n"
// (alert_attr: [@deprecated] is the "deprecated" alert)
inline StrMap<std::string_view> alerts_of_attrs(const std::vector<const parsetree::Attribute*>& l) {
  StrMap<std::string_view> acc;
  for (const parsetree::Attribute* a : l) {
    std::string kind, message;
    if (attr_equals_builtin(a->attr_name.txt, "deprecated")) {
      kind = "deprecated";
      message = string_of_opt_payload(a->attr_payload);
    } else if (attr_equals_builtin(a->attr_name.txt, "alert")) {
      auto km = kind_and_message(a->attr_payload);
      if (!km) continue;  // bad payloads are warning_attribute's to report
      std::tie(kind, message) = *km;
    } else {
      continue;
    }
    const std::string_view* old = acc.find_opt(kind);
    std::string upd = (!old || old->empty()) ? message : (message.empty() ? std::string(*old)
                                                                         : std::string(*old) + "\n" + message);
    acc = acc.add(zborrow(kind), zborrow(upd));
  }
  return acc;
}
// alerts_of_sig / alerts_of_str: the leading floating attributes (the
// marking of used attributes belongs to warnings, not ported)
inline StrMap<std::string_view> alerts_of_sig(const parsetree::Signature& sg) {
  std::vector<const parsetree::Attribute*> a;
  for (const parsetree::SignatureItem* it : sg) {
    auto* at = parsetree::as<parsetree::Psig_attribute>(it->psig_desc);
    if (!at) break;
    a.push_back(at->attr);
  }
  return alerts_of_attrs(a);
}
inline StrMap<std::string_view> alerts_of_str(const parsetree::Structure& str) {
  std::vector<const parsetree::Attribute*> a;
  for (const parsetree::StructureItem* it : str) {
    auto* at = parsetree::as<parsetree::Pstr_attribute>(it->pstr_desc);
    if (!at) break;
    a.push_back(at->attr);
  }
  return alerts_of_attrs(a);
}

template <class F>
auto warning_scope(const parsetree::Attributes&, F&& f) -> decltype(f()) {
  return f();
}

}  // namespace cppcaml::typing::builtin_attributes
