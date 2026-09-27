// See builtin_attributes.hpp: parsing/builtin_attributes.ml.
#include "cppcaml/typing/builtin_attributes.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/location.hpp"

namespace cppcaml::typing::builtin_attributes {

namespace {

// Attribute_table: string loc, compared structurally
using Key = std::tuple<std::string, std::string, long, long, long, std::string, long, long, long, bool>;
Key key(std::string_view txt, const Location& l) {
  return Key{std::string(txt),
             std::string(l.loc_start.pos_fname),
             l.loc_start.pos_lnum,
             l.loc_start.pos_bol,
             l.loc_start.pos_cnum,
             std::string(l.loc_end.pos_fname),
             l.loc_end.pos_lnum,
             l.loc_end.pos_bol,
             l.loc_end.pos_cnum,
             l.loc_ghost};
}
struct Entry {
  std::string txt;
  Location loc;
  long seq;  // insertion order (the table's order for attr_order ties)
};
std::map<Key, Entry>& unused_attrs() {
  static std::map<Key, Entry> t;
  return t;
}
long seq_counter = 0;

const std::set<std::string_view>& builtin_attrs() {
  static const std::set<std::string_view> s = {
      "alert",    "atomic",   "boxed",           "deprecated", "deprecated_mutable", "explicit_arity",
      "immediate", "immediate64", "inline",      "inlined",    "noalloc",            "poll",
      "ppwarning", "remove_aliases", "specialise", "specialised", "tailcall",         "tail_mod_cons",
      "unboxed",  "untagged", "unrolled",        "warnerror",  "warning",            "warn_on_literal_pattern"};
  return s;
}

std::string_view drop_ocaml_attr_prefix(std::string_view s) {
  if (s.size() > 6 && s.substr(0, 6) == "ocaml.") return s.substr(6);
  return s;
}

// ---- payloads of Types attributes (the parsed source's, or a .cmi's value) ----
using OV = OValue;
const OV* field(const OV* v, std::size_t i) {
  return v && v->kind == OV::Kind::Block && i < v->fields.size() ? v->fields[i] : nullptr;
}
bool is_block(const OV* v, unsigned tag) { return v && v->kind == OV::Kind::Block && v->tag == tag; }
// PStr [ {pstr_desc = Pstr_eval (e, _)} ]: e
const OV* single_eval(const OV* payload) {
  if (!is_block(payload, 0)) return nullptr;  // PStr
  const OV* l = field(payload, 0);
  if (!is_block(l, 0) || !(field(l, 1) && field(l, 1)->kind == OV::Kind::Int && field(l, 1)->i == 0)) return nullptr;
  const OV* item = field(l, 0);
  const OV* desc = field(item, 0);
  if (!is_block(desc, 0)) return nullptr;  // Pstr_eval
  return field(desc, 0);
}
// {pexp_desc = Pexp_constant {pconst_desc = Pconst_string (s, _, _)}}
std::optional<std::string_view> string_const(const OV* e) {
  const OV* d = field(e, 0);
  if (!is_block(d, 1)) return std::nullopt;  // Pexp_constant
  const OV* c = field(field(d, 0), 0);        // pconst_desc
  if (!is_block(c, 2)) return std::nullopt;  // Pconst_string
  const OV* s = field(c, 0);
  if (!s || s->kind != OV::Kind::String) return std::nullopt;
  return s->s;
}
// {pexp_desc = Pexp_ident {txt = Lident id}}
std::optional<std::string_view> lident(const OV* e) {
  const OV* d = field(e, 0);
  if (!is_block(d, 0)) return std::nullopt;  // Pexp_ident
  const OV* lid = field(field(d, 0), 0);      // txt
  if (!is_block(lid, 0)) return std::nullopt;  // Lident
  const OV* s = field(lid, 0);
  if (!s || s->kind != OV::Kind::String) return std::nullopt;
  return s->s;
}
std::optional<std::string_view> string_of_payload_ov(const OV* p) { return string_const(single_eval(p)); }
std::optional<std::pair<std::string, std::string>> kind_and_message_ov(const OV* p) {
  const OV* e = single_eval(p);
  if (!e) return std::nullopt;
  const OV* d = field(e, 0);
  if (is_block(d, 4)) {  // Pexp_apply (f, [Nolabel, arg])
    std::optional<std::string_view> id = lident(field(d, 0));
    const OV* args = field(d, 1);
    if (!id || !is_block(args, 0)) return std::nullopt;
    const OV* rest = field(args, 1);
    if (!rest || rest->kind != OV::Kind::Int || rest->i != 0) return std::nullopt;
    const OV* a = field(args, 0);
    const OV* lbl = field(a, 0);
    if (!lbl || lbl->kind != OV::Kind::Int || lbl->i != 0) return std::nullopt;
    std::optional<std::string_view> s = string_const(field(a, 1));
    if (!s) return std::nullopt;
    return std::make_pair(std::string(*id), std::string(*s));
  }
  if (std::optional<std::string_view> id = lident(e)) return std::make_pair(std::string(*id), std::string());
  return std::nullopt;
}

std::string string_of_opt_payload_attr(const Attribute* a) {
  if (a->ast) return string_of_opt_payload(a->ast->attr_payload);
  std::optional<std::string_view> s = string_of_payload_ov(a->attr_payload);
  return s ? std::string(*s) : std::string();
}
std::optional<std::pair<std::string, std::string>> kind_and_message_attr(const Attribute* a) {
  if (a->ast) return kind_and_message(a->ast->attr_payload);
  return kind_and_message_ov(a->attr_payload);
}

// cat s1 s2
std::string cat(std::string_view s1, std::string_view s2) {
  if (s2.empty()) return std::string(s1);
  return std::string(s1) + "\n" + std::string(s2);
}

// alerts_of_attrs's fold over (kind, message) pairs
StrMap<std::string_view> fold_alerts(const std::vector<std::pair<std::string, std::string>>& l) {
  StrMap<std::string_view> acc;
  for (const auto& [kind, message] : l) {
    const std::string_view* old = acc.find_opt(kind);
    std::string upd = (!old || old->empty()) ? message : cat(*old, message);
    acc = acc.add(zborrow(kind), zborrow(upd));
  }
  return acc;
}

}  // namespace

bool is_builtin_attr(std::string_view s) { return builtin_attrs().count(drop_ocaml_attr_prefix(s)) != 0; }

void register_attr(std::string_view txt, const Location& loc) {
  if (!is_builtin_attr(txt)) return;
  Key k = key(txt, loc);
  auto& t = unused_attrs();
  auto it = t.find(k);
  if (it == t.end()) t.emplace(std::move(k), Entry{std::string(txt), loc, seq_counter++});
}

void mark_used(std::string_view txt, const Location& loc) {
  auto& t = unused_attrs();
  if (t.empty()) return;
  t.erase(key(txt, loc));
}

void warn_unused() {
  std::vector<Entry> keys;
  for (auto& [k, e] : unused_attrs()) keys.push_back(e);
  unused_attrs().clear();
  // compiler_stops_before_attributes_consumed ()
  bool stops_before_lambda = clflags::stop_after && *clflags::stop_after < clflags::Pass::Lambda;
  if (stops_before_lambda || clflags::print_types) return;
  std::sort(keys.begin(), keys.end(), [](const Entry& a, const Entry& b) { return a.seq < b.seq; });
  std::stable_sort(keys.begin(), keys.end(), [](const Entry& a, const Entry& b) {
    int c = a.loc.loc_start.pos_fname.compare(b.loc.loc_start.pos_fname);
    if (c != 0) return c < 0;
    return a.loc.loc_start.pos_cnum < b.loc.loc_start.pos_cnum;
  });
  for (const Entry& e : keys)
    location::prerr_warning(e.loc, warnings::Warning::with_s(warnings::Warning::K::Misplaced_attribute, e.txt));
}

bool has_attribute(std::string_view nm, const parsetree::Attributes& attrs) {
  for (const parsetree::Attribute* a : attrs)
    if (attr_equals_builtin(a->attr_name.txt, nm)) {
      mark_used(a->attr_name);
      return true;
    }
  return false;
}
bool has_attribute(std::string_view nm, const Attributes& attrs) {
  for (const Attribute* a : attrs)
    if (attr_equals_builtin(a->attr_name, nm)) {
      mark_used(a->attr_name, a->attr_name_loc);
      return true;
    }
  return false;
}

std::vector<const parsetree::Attribute*> select_attributes(
    const std::vector<std::pair<std::string_view, AttrAction>>& actions, const parsetree::Attributes& attrs) {
  std::vector<const parsetree::Attribute*> out;
  for (const parsetree::Attribute* a : attrs) {
    bool keep = false;
    for (const auto& [nm, action] : actions)
      if (attr_equals_builtin(a->attr_name.txt, nm)) {
        mark_used(a->attr_name);
        keep = action == AttrAction::Return;
        break;
      }
    if (keep) out.push_back(a);
  }
  return out;
}

std::optional<std::string_view> string_of_payload(const parsetree::Payload& p) {
  using namespace parsetree;
  if (p.kind != Payload::Kind::PStr || p.str.size() != 1) return std::nullopt;
  auto* ev = as<Pstr_eval>(p.str[0]->pstr_desc);
  if (!ev) return std::nullopt;
  auto* c = as<Pexp_constant>(ev->exp->pexp_desc);
  if (!c || c->c.pconst_desc.kind != ConstantDesc::Kind::Pconst_string) return std::nullopt;
  return c->c.pconst_desc.s;
}

std::optional<std::pair<std::string, std::string>> kind_and_message(const parsetree::Payload& p) {
  using namespace parsetree;
  if (p.kind != Payload::Kind::PStr || p.str.size() != 1) return std::nullopt;
  auto* ev = as<Pstr_eval>(p.str[0]->pstr_desc);
  if (!ev) return std::nullopt;
  auto lid = [](const Expression* e) -> std::optional<std::string_view> {
    auto* id = as<Pexp_ident>(e->pexp_desc);
    if (!id || id->lid.txt->kind != Longident::Kind::Lident) return std::nullopt;
    return id->lid.txt->s;
  };
  if (auto* ap = as<Pexp_apply>(ev->exp->pexp_desc)) {
    std::optional<std::string_view> id = lid(ap->fn);
    if (!id || ap->args.size() != 1 || ap->args[0].label.kind != ArgLabel::Kind::Nolabel) return std::nullopt;
    auto* c = as<Pexp_constant>(ap->args[0].exp->pexp_desc);
    if (!c || c->c.pconst_desc.kind != ConstantDesc::Kind::Pconst_string) return std::nullopt;
    return std::make_pair(std::string(*id), std::string(c->c.pconst_desc.s));
  }
  if (std::optional<std::string_view> id = lid(ev->exp)) return std::make_pair(std::string(*id), std::string());
  return std::nullopt;
}

// ---- marking ----
void mark_alerts_used(const parsetree::Attributes& l) {
  for (const parsetree::Attribute* a : l)
    if (attr_equals_builtin(a->attr_name.txt, "deprecated") || attr_equals_builtin(a->attr_name.txt, "alert"))
      mark_used(a->attr_name);
}
void mark_alerts_used(const Attributes& l) {
  for (const Attribute* a : l)
    if (attr_equals_builtin(a->attr_name, "deprecated") || attr_equals_builtin(a->attr_name, "alert"))
      mark_used(a->attr_name, a->attr_name_loc);
}
void mark_warn_on_literal_pattern_used(const Attributes& l) {
  for (const Attribute* a : l)
    if (attr_equals_builtin(a->attr_name, "warn_on_literal_pattern")) mark_used(a->attr_name, a->attr_name_loc);
}
void mark_deprecated_mutable_used(const Attributes& l) {
  for (const Attribute* a : l)
    if (attr_equals_builtin(a->attr_name, "deprecated_mutable")) mark_used(a->attr_name, a->attr_name_loc);
}
bool warn_on_literal_pattern(const Attributes& attrs) { return has_attribute("warn_on_literal_pattern", attrs); }

// ---- alerts ----
StrMap<std::string_view> alerts_of_attrs(const std::vector<const parsetree::Attribute*>& l) {
  std::vector<std::pair<std::string, std::string>> alerts;
  for (const parsetree::Attribute* a : l) {
    if (attr_equals_builtin(a->attr_name.txt, "deprecated"))
      alerts.emplace_back("deprecated", string_of_opt_payload(a->attr_payload));
    else if (attr_equals_builtin(a->attr_name.txt, "alert")) {
      if (auto km = kind_and_message(a->attr_payload)) alerts.push_back(*km);
    }
  }
  return fold_alerts(alerts);
}
StrMap<std::string_view> alerts_of_attrs(const Attributes& l) {
  std::vector<std::pair<std::string, std::string>> alerts;
  for (const Attribute* a : l) {
    if (attr_equals_builtin(a->attr_name, "deprecated"))
      alerts.emplace_back("deprecated", string_of_opt_payload_attr(a));
    else if (attr_equals_builtin(a->attr_name, "alert")) {
      if (auto km = kind_and_message_attr(a)) alerts.push_back(*km);
    }
  }
  return fold_alerts(alerts);
}

StrMap<std::string_view> alerts_of_sig(bool mark, const parsetree::Signature& sg) {
  std::vector<const parsetree::Attribute*> a;
  for (const parsetree::SignatureItem* it : sg) {
    auto* at = parsetree::as<parsetree::Psig_attribute>(it->psig_desc);
    if (!at) break;
    a.push_back(at->attr);
  }
  if (mark)
    for (const parsetree::Attribute* x : a)
      if (attr_equals_builtin(x->attr_name.txt, "deprecated") || attr_equals_builtin(x->attr_name.txt, "alert"))
        mark_used(x->attr_name);
  return alerts_of_attrs(a);
}
StrMap<std::string_view> alerts_of_str(bool mark, const parsetree::Structure& str) {
  std::vector<const parsetree::Attribute*> a;
  for (const parsetree::StructureItem* it : str) {
    auto* at = parsetree::as<parsetree::Pstr_attribute>(it->pstr_desc);
    if (!at) break;
    a.push_back(at->attr);
  }
  if (mark)
    for (const parsetree::Attribute* x : a)
      if (attr_equals_builtin(x->attr_name.txt, "deprecated") || attr_equals_builtin(x->attr_name.txt, "alert"))
        mark_used(x->attr_name);
  return alerts_of_attrs(a);
}

void check_alerts(const Location& loc, const Attributes& attrs, std::string_view s) {
  alerts_of_attrs(attrs).iter([&](std::string_view kind, std::string_view message) {
    location::alert(loc, std::string(kind), cat(s, message));
  });
}

void check_alerts_inclusion(const Location& def, const Location& use, const Location& loc, const Attributes& attrs1,
                            const Attributes& attrs2, std::string_view s) {
  StrMap<std::string_view> m2 = alerts_of_attrs(attrs2);
  alerts_of_attrs(attrs1).iter([&](std::string_view kind, std::string_view msg) {
    if (!m2.mem(kind)) location::alert(loc, std::string(kind), cat(s, msg), def, use);
  });
}

namespace {
std::optional<std::string> deprecated_mutable_of_attrs(const Attributes& attrs) {
  for (const Attribute* a : attrs)
    if (attr_equals_builtin(a->attr_name, "deprecated_mutable")) return string_of_opt_payload_attr(a);
  return std::nullopt;
}
}  // namespace

void check_deprecated_mutable(const Location& loc, const Attributes& attrs, std::string_view s) {
  if (std::optional<std::string> txt = deprecated_mutable_of_attrs(attrs))
    location::deprecated(loc, "mutating field " + cat(s, *txt));
}

void check_deprecated_mutable_inclusion(const Location& def, const Location& use, const Location& loc,
                                        const Attributes& attrs1, const Attributes& attrs2, std::string_view s) {
  std::optional<std::string> t1 = deprecated_mutable_of_attrs(attrs1);
  std::optional<std::string> t2 = deprecated_mutable_of_attrs(attrs2);
  if (t1 && !t2) location::deprecated(loc, "mutating field " + cat(s, *t1), def, use);
}

parsetree::Attributes ast_attributes(const Attributes& l) {
  std::vector<const parsetree::Attribute*> out;
  for (const Attribute* a : l)
    if (a->ast) out.push_back(a->ast);
  return slice(out);
}

// ---- warning attributes ----
namespace {
void warn_payload(const Location& loc, std::string_view txt, std::string msg) {
  warnings::Warning w = warnings::Warning::make(warnings::Warning::K::Attribute_payload);
  w.s = std::string(txt);
  w.s2 = std::move(msg);
  location::prerr_warning(loc, w);
}
}  // namespace

void warning_attribute(const parsetree::Attribute* a, bool ppwarning) {
  using parsetree::as;
  const Location& loc = a->attr_loc;
  const parsetree::StrLoc& name = a->attr_name;
  auto process = [&](bool errflag) {
    mark_used(name);
    if (std::optional<std::string_view> s = string_of_payload(a->attr_payload)) {
      try {
        if (std::optional<warnings::Alert> a = warnings::parse_options(errflag, *s)) location::prerr_alert(loc, *a);
      } catch (const warnings::Bad& e) {
        warn_payload(loc, name.txt, e.what());
      }
    } else {
      warn_payload(loc, name.txt, "A single string literal is expected");
    }
  };
  if (attr_equals_builtin(name.txt, "warning")) {
    process(false);
  } else if (attr_equals_builtin(name.txt, "warnerror")) {
    process(true);
  } else if (attr_equals_builtin(name.txt, "alert")) {
    if (std::optional<std::string_view> s = string_of_payload(a->attr_payload)) {
      mark_used(name);
      try {
        warnings::parse_alert_option(*s);
      } catch (const warnings::Bad& e) {
        warn_payload(loc, name.txt, e.what());
      }
    } else {
      std::optional<std::pair<std::string, std::string>> km = kind_and_message(a->attr_payload);
      if (km && km->first == "all") {
        warn_payload(loc, name.txt, "The alert name 'all' is reserved");
      } else if (km) {
        if (!warnings::is_active(53)) mark_used(name);
      } else {
        mark_used(name);
        warn_payload(loc, name.txt, "Invalid payload");
      }
    }
  } else if (ppwarning && attr_equals_builtin(name.txt, "ppwarning")) {
    const parsetree::Payload& p = a->attr_payload;
    const parsetree::Pstr_eval* ev = nullptr;
    if (p.kind == parsetree::Payload::Kind::PStr && p.str.size() == 1) ev = as<parsetree::Pstr_eval>(p.str[0]->pstr_desc);
    const parsetree::Pexp_constant* c = ev ? as<parsetree::Pexp_constant>(ev->exp->pexp_desc) : nullptr;
    if (c && c->c.pconst_desc.kind == parsetree::ConstantDesc::Kind::Pconst_string) {
      mark_used(name);
      location::prerr_warning(p.str[0]->pstr_loc,
                              warnings::Warning::with_s(warnings::Warning::K::Preprocessor,
                                                        std::string(c->c.pconst_desc.s)));
    } else {
      mark_used(name);
      warn_payload(loc, name.txt, "A single string literal is expected");
    }
  }
}

}  // namespace cppcaml::typing::builtin_attributes
