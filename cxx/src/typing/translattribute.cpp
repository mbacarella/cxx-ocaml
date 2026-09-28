// Port of lambda/translattribute.ml (TYPECHECKER.md stage 10).
//

#include "cppcaml/typing/translattribute.hpp"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/location.hpp"
#include "typecore_internal.hpp"

namespace cppcaml::typing::translattribute {

namespace pt = parsetree;
namespace tt = typedtree;
using namespace lambda;
using builtin_attributes::attr_equals_builtin;
using lam_t = cppcaml::typing::lambda::lambda;

namespace {

// Config.flambda (ocamlc: false)
constexpr bool config_flambda = false;

// Builtin_attributes.attribute_action = Return | Mark_used_only
enum class Action { Return, Mark_used_only };
using Actions = std::vector<std::pair<std::string_view, Action>>;

const Action return_if_flambda = config_flambda ? Action::Return : Action::Mark_used_only;

const Actions is_inline_attribute = {{"inline", Action::Return}};
const Actions is_inlined_attribute = {{"inlined", Action::Return}, {"unrolled", return_if_flambda}};
const Actions is_specialise_attribute = {{"specialise", return_if_flambda}};
const Actions is_specialised_attribute = {{"specialised", return_if_flambda}};
const Actions is_local_attribute = {{"local", Action::Return}};
const Actions is_tailcall_attribute = {{"tailcall", Action::Return}};
const Actions is_tmc_attribute = {{"tail_mod_cons", Action::Return}};
const Actions is_poll_attribute = {{"poll", Action::Return}};

// Builtin_attributes.select_attributes (the named attributes marked used)
std::vector<const pt::Attribute*> select_attributes(const Actions& actions, const pt::Attributes& attrs) {
  std::vector<std::pair<std::string_view, builtin_attributes::AttrAction>> acts;
  for (auto& [nm, action] : actions)
    acts.emplace_back(nm, action == Action::Return ? builtin_attributes::AttrAction::Return
                                                   : builtin_attributes::AttrAction::Mark_used_only);
  return builtin_attributes::select_attributes(acts, attrs);
}

void warn(const Location& loc, warnings::Warning::K k, std::string s, std::string s2 = {}) {
  warnings::Warning w = warnings::Warning::make(k);
  w.s = std::move(s);
  w.s2 = std::move(s2);
  location::prerr_warning(loc, w);
}

const pt::Attribute* find_attribute(const Actions& p, const pt::Attributes& attributes) {
  if (attributes.empty()) return nullptr;  // nothing to select nor mark used
  std::vector<const pt::Attribute*> l = select_attributes(p, attributes);
  if (l.empty()) return nullptr;
  if (l.size() >= 2)
    warn(l[1]->attr_name.loc, warnings::Warning::K::Duplicated_attribute, std::string(l[1]->attr_name.txt));
  return l[0];
}

// get_payload: PStr [{pstr_desc = Pstr_eval (exp, [])}] -> Some exp
const pt::Expression* get_payload(const pt::Payload& p) {
  if (p.kind != pt::Payload::Kind::PStr || p.str.size() != 1) return nullptr;
  auto* ev = pt::as<pt::Pstr_eval>(p.str[0]->pstr_desc);
  if (!ev || !ev->attrs.empty()) return nullptr;
  return ev->exp;
}

// Result: Error () | Ok None | Ok (Some x)
template <class T>
struct Res {
  bool ok = false;
  std::optional<T> v;
};

template <class T, class F>
Res<T> get_optional_payload(F&& get_from_exp, const pt::Payload& p) {
  if (p.kind == pt::Payload::Kind::PStr && p.str.empty()) return {true, std::nullopt};
  const pt::Expression* e = get_payload(p);
  if (!e) return {};
  std::optional<T> v = get_from_exp(e);
  if (!v) return {};
  return {true, v};
}

std::optional<std::string_view> get_id_from_exp(const pt::Expression* e) {
  auto* id = pt::as<pt::Pexp_ident>(e->pexp_desc);
  if (id && id->lid.txt->kind == Longident::Kind::Lident) return id->lid.txt->s;
  return std::nullopt;
}

std::optional<long> get_int_from_exp(const pt::Expression* e) {
  auto* c = pt::as<pt::Pexp_constant>(e->pexp_desc);
  if (!c || c->c.pconst_desc.kind != pt::ConstantDesc::Kind::Pconst_integer || c->c.pconst_desc.has_suffix)
    return std::nullopt;
  // Misc.Int_literal_converter.int (Failure -> Error ())
  typecore::Error err(location::none(), nullptr, typecore::EK::Literal_overflow);
  std::optional<tt::Constant> k = typecore::constant(c->c, &err);
  if (!k) return std::nullopt;
  return k->i;
}

std::optional<std::string_view> get_construct_from_exp(const pt::Expression* e) {
  auto* c = pt::as<pt::Pexp_construct>(e->pexp_desc);
  if (c && c->lid.txt->kind == Longident::Kind::Lident && !c->arg) return c->lid.txt->s;
  return std::nullopt;
}

std::optional<bool> get_bool_from_exp(const pt::Expression* e) {
  std::optional<std::string_view> c = get_construct_from_exp(e);
  if (!c) return std::nullopt;
  if (*c == "true") return true;
  if (*c == "false") return false;
  return std::nullopt;
}

template <class R>
R parse_id_payload(std::string_view txt, const Location& loc, R default_, R empty,
                   const std::vector<std::pair<std::string_view, R>>& cases, const pt::Payload& payload) {
  auto warn_ = [&] {
    std::string msg;
    for (std::size_t i = 0; i < cases.size(); ++i) {
      if (i > 0) msg += ", ";
      msg += "'" + std::string(cases[i].first) + "'";
    }
    msg = "It must be either " + msg + " or empty";
    warn(loc, warnings::Warning::K::Attribute_payload, std::string(txt), msg);
    return default_;
  };
  Res<std::string_view> r = get_optional_payload<std::string_view>(get_id_from_exp, payload);
  if (!r.ok) return warn_();
  if (!r.v) return empty;
  for (auto& [id, v] : cases)
    if (id == *r.v) return v;
  return warn_();
}

InlineAttribute inl(InlineAttribute::Kind k) { return InlineAttribute{k, 0}; }

InlineAttribute parse_inline_attribute(const pt::Attribute* attr) {
  using IK = InlineAttribute::Kind;
  if (!attr) return inl(IK::Default_inline);
  std::string_view txt = attr->attr_name.txt;
  const Location& loc = attr->attr_name.loc;
  if (attr_equals_builtin(attr->attr_name.txt, "unrolled")) {
    // the 'unrolled' attributes must be used as [@unrolled n].
    const pt::Expression* e = get_payload(attr->attr_payload);
    std::optional<long> n = e ? get_int_from_exp(e) : std::nullopt;
    if (n) return InlineAttribute{IK::Unroll, *n};
    warn(loc, warnings::Warning::K::Attribute_payload, std::string(txt), "It must be an integer literal");
    return inl(IK::Default_inline);
  }
  return parse_id_payload<InlineAttribute>(
      txt, loc, inl(IK::Default_inline), inl(IK::Always_inline),
      {{"never", inl(IK::Never_inline)}, {"always", inl(IK::Always_inline)}, {"hint", inl(IK::Hint_inline)}},
      attr->attr_payload);
}

SpecialiseAttribute parse_specialise_attribute(const pt::Attribute* attr) {
  using S = SpecialiseAttribute;
  if (!attr) return S::Default_specialise;
  return parse_id_payload<S>(attr->attr_name.txt, attr->attr_name.loc, S::Default_specialise, S::Always_specialise,
                             {{"never", S::Never_specialise}, {"always", S::Always_specialise}},
                             attr->attr_payload);
}

LocalAttribute parse_local_attribute(const pt::Attribute* attr) {
  using L = LocalAttribute;
  if (!attr) return L::Default_local;
  return parse_id_payload<L>(attr->attr_name.txt, attr->attr_name.loc, L::Default_local, L::Always_local,
                             {{"never", L::Never_local}, {"always", L::Always_local}, {"maybe", L::Default_local}},
                             attr->attr_payload);
}

PollAttribute parse_poll_attribute(const pt::Attribute* attr) {
  using P = PollAttribute;
  if (!attr) return P::Default_poll;
  return parse_id_payload<P>(attr->attr_name.txt, attr->attr_name.loc, P::Default_poll, P::Default_poll,
                             {{"error", P::Error_poll}}, attr->attr_payload);
}

PollAttribute get_poll_attribute(const pt::Attributes& l) {
  return parse_poll_attribute(find_attribute(is_poll_attribute, l));
}

bool inlines(const InlineAttribute& i) {
  using IK = InlineAttribute::Kind;
  return i.kind == IK::Always_inline || i.kind == IK::Hint_inline || i.kind == IK::Unroll;
}
void check_local_inline(const Location& loc, const FunctionAttribute& attr) {
  if (attr.local == LocalAttribute::Always_local && inlines(attr.inline_))
    warn(loc, warnings::Warning::K::Duplicated_attribute, "local/inline");
}
void check_poll_inline(const Location& loc, const FunctionAttribute& attr) {
  if (attr.poll == PollAttribute::Error_poll && inlines(attr.inline_))
    warn(loc, warnings::Warning::K::Inlining_impossible, "[@poll error] is incompatible with inlining");
}
void check_poll_local(const Location& loc, const FunctionAttribute& attr) {
  if (attr.poll == PollAttribute::Error_poll && attr.local == LocalAttribute::Always_local)
    warn(loc, warnings::Warning::K::Inlining_impossible,
         "[@poll error] is incompatible with local function optimization");
}

lam_t lfunction_with_attr(const FunctionAttribute& attr, const LFunction* f) {
  return lfunction(f->kind, f->params, f->return_, f->body, attr, f->loc);
}

lam_t add_tmc_attribute(lam_t expr, const Location& loc, const pt::Attributes& attributes) {
  auto* lf = as<Lfunction>(expr);
  if (!lf) return expr;
  const pt::Attribute* attr = find_attribute(is_tmc_attribute, attributes);
  if (!attr) return expr;
  if (lf->f->attr.tmc_candidate) warn(loc, warnings::Warning::K::Duplicated_attribute, "tail_mod_cons");
  FunctionAttribute a = lf->f->attr;
  a.tmc_candidate = true;
  return lfunction_with_attr(a, lf->f);
}

lam_t add_poll_attribute(lam_t expr, const Location& loc, const pt::Attributes& attributes) {
  auto* lf = as<Lfunction>(expr);
  if (!lf || lf->f->attr.stub) return expr;
  PollAttribute poll = get_poll_attribute(attributes);
  if (poll == PollAttribute::Default_poll) return expr;
  if (lf->f->attr.poll == PollAttribute::Error_poll)
    warn(loc, warnings::Warning::K::Duplicated_attribute, "poll error");
  FunctionAttribute a = lf->f->attr;
  a.poll = poll;
  check_poll_inline(loc, a);
  check_poll_local(loc, a);
  a.inline_ = inl(InlineAttribute::Kind::Never_inline);
  a.local = LocalAttribute::Never_local;
  return lfunction_with_attr(a, lf->f);
}

}  // namespace

InlineAttribute get_inline_attribute(const pt::Attributes& l) {
  return parse_inline_attribute(find_attribute(is_inline_attribute, l));
}

SpecialiseAttribute get_specialise_attribute(const pt::Attributes& l) {
  return parse_specialise_attribute(find_attribute(is_specialise_attribute, l));
}

LocalAttribute get_local_attribute(const pt::Attributes& l) {
  return parse_local_attribute(find_attribute(is_local_attribute, l));
}

lam_t add_inline_attribute(lam_t expr, const Location& loc, const pt::Attributes& attributes) {
  auto* lf = as<Lfunction>(expr);
  if (!lf || lf->f->attr.stub) return expr;
  InlineAttribute inline_ = get_inline_attribute(attributes);
  if (inline_.kind == InlineAttribute::Kind::Default_inline) return expr;
  if (lf->f->attr.inline_.kind != InlineAttribute::Kind::Default_inline)
    warn(loc, warnings::Warning::K::Duplicated_attribute, "inline");
  FunctionAttribute a = lf->f->attr;
  a.inline_ = inline_;
  check_local_inline(loc, a);
  check_poll_inline(loc, a);
  return lfunction_with_attr(a, lf->f);
}

lam_t add_specialise_attribute(lam_t expr, const Location& loc, const pt::Attributes& attributes) {
  auto* lf = as<Lfunction>(expr);
  if (!lf || lf->f->attr.stub) return expr;
  SpecialiseAttribute specialise = get_specialise_attribute(attributes);
  if (specialise == SpecialiseAttribute::Default_specialise) return expr;
  if (lf->f->attr.specialise != SpecialiseAttribute::Default_specialise)
    warn(loc, warnings::Warning::K::Duplicated_attribute, "specialise");
  FunctionAttribute a = lf->f->attr;
  a.specialise = specialise;
  return lfunction_with_attr(a, lf->f);
}

lam_t add_local_attribute(lam_t expr, const Location& loc, const pt::Attributes& attributes) {
  auto* lf = as<Lfunction>(expr);
  if (!lf || lf->f->attr.stub) return expr;
  LocalAttribute local = get_local_attribute(attributes);
  if (local == LocalAttribute::Default_local) return expr;
  if (lf->f->attr.local != LocalAttribute::Default_local)
    warn(loc, warnings::Warning::K::Duplicated_attribute, "local");
  FunctionAttribute a = lf->f->attr;
  a.local = local;
  check_local_inline(loc, a);
  check_poll_local(loc, a);
  return lfunction_with_attr(a, lf->f);
}

InlineAttribute get_inlined_attribute(const tt::Expression* e) {
  return parse_inline_attribute(find_attribute(is_inlined_attribute, e->exp_attributes));
}

InlineAttribute get_inlined_attribute_on_module(const tt::ModuleExpr* e) {
  InlineAttribute attr = parse_inline_attribute(find_attribute(is_inlined_attribute, e->mod_attributes));
  if (auto* c = tt::as<tt::Tmod_constraint>(e->mod_desc)) {
    InlineAttribute inner_attr = get_inlined_attribute_on_module(c->me);
    if (attr.kind == InlineAttribute::Kind::Default_inline) return inner_attr;
  }
  return attr;
}

SpecialiseAttribute get_specialised_attribute(const tt::Expression* e) {
  return parse_specialise_attribute(find_attribute(is_specialised_attribute, e->exp_attributes));
}

TailcallAttribute get_tailcall_attribute(const tt::Expression* e) {
  const pt::Attribute* attr = find_attribute(is_tailcall_attribute, e->exp_attributes);
  if (!attr) return TailcallAttribute::Default_tailcall;
  Res<bool> r = get_optional_payload<bool>(get_bool_from_exp, attr->attr_payload);
  if (!r.ok) {
    warn(attr->attr_name.loc, warnings::Warning::K::Attribute_payload, std::string(attr->attr_name.txt),
         "Only an optional boolean literal is supported.");
    return TailcallAttribute::Default_tailcall;
  }
  if (!r.v || *r.v) return TailcallAttribute::Tailcall_expectation_true;
  return TailcallAttribute::Tailcall_expectation_false;
}

lam_t add_function_attributes(lam_t lam, const Location& loc, const pt::Attributes& attr) {
  lam = add_inline_attribute(lam, loc, attr);
  lam = add_specialise_attribute(lam, loc, attr);
  lam = add_local_attribute(lam, loc, attr);
  lam = add_tmc_attribute(lam, loc, attr);
  // last because poll overrides inline and local
  lam = add_poll_attribute(lam, loc, attr);
  return lam;
}

}  // namespace cppcaml::typing::translattribute
