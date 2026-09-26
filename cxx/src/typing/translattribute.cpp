// Port of lambda/translattribute.ml (TYPECHECKER.md stage 10).
//
// The warnings it reports (Duplicated_attribute, Attribute_payload,
// Inlining_impossible) are not emitted: warning emission is stage 9.  The
// marking of used attributes (Builtin_attributes.mark_used) is not ported.

#include "cppcaml/typing/translattribute.hpp"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "cppcaml/typing/builtin_attributes.hpp"
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

// Builtin_attributes.select_attributes
std::vector<const pt::Attribute*> select_attributes(const Actions& actions, const pt::Attributes& attrs) {
  std::vector<const pt::Attribute*> r;
  for (const pt::Attribute* a : attrs) {
    bool keep = false;
    for (auto& [nm, action] : actions)
      if (attr_equals_builtin(a->attr_name.txt, nm) && action == Action::Return) {
        keep = true;
        break;
      }
    if (keep) r.push_back(a);
  }
  return r;
}

const pt::Attribute* find_attribute(const Actions& p, const pt::Attributes& attributes) {
  std::vector<const pt::Attribute*> l = select_attributes(p, attributes);
  if (l.empty()) return nullptr;
  // (two or more: Warnings.Duplicated_attribute on the second, not emitted)
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
R parse_id_payload(R default_, R empty, const std::vector<std::pair<std::string_view, R>>& cases,
                   const pt::Payload& payload) {
  // warn (): Warnings.Attribute_payload, not emitted
  Res<std::string_view> r = get_optional_payload<std::string_view>(get_id_from_exp, payload);
  if (!r.ok) return default_;
  if (!r.v) return empty;
  for (auto& [id, v] : cases)
    if (id == *r.v) return v;
  return default_;
}

InlineAttribute inl(InlineAttribute::Kind k) { return InlineAttribute{k, 0}; }

InlineAttribute parse_inline_attribute(const pt::Attribute* attr) {
  using IK = InlineAttribute::Kind;
  if (!attr) return inl(IK::Default_inline);
  if (attr_equals_builtin(attr->attr_name.txt, "unrolled")) {
    const pt::Expression* e = get_payload(attr->attr_payload);
    std::optional<long> n = e ? get_int_from_exp(e) : std::nullopt;
    if (n) return InlineAttribute{IK::Unroll, *n};
    return inl(IK::Default_inline);
  }
  return parse_id_payload<InlineAttribute>(
      inl(IK::Default_inline), inl(IK::Always_inline),
      {{"never", inl(IK::Never_inline)}, {"always", inl(IK::Always_inline)}, {"hint", inl(IK::Hint_inline)}},
      attr->attr_payload);
}

SpecialiseAttribute parse_specialise_attribute(const pt::Attribute* attr) {
  using S = SpecialiseAttribute;
  if (!attr) return S::Default_specialise;
  return parse_id_payload<S>(S::Default_specialise, S::Always_specialise,
                             {{"never", S::Never_specialise}, {"always", S::Always_specialise}},
                             attr->attr_payload);
}

LocalAttribute parse_local_attribute(const pt::Attribute* attr) {
  using L = LocalAttribute;
  if (!attr) return L::Default_local;
  return parse_id_payload<L>(L::Default_local, L::Always_local,
                             {{"never", L::Never_local}, {"always", L::Always_local}, {"maybe", L::Default_local}},
                             attr->attr_payload);
}

PollAttribute parse_poll_attribute(const pt::Attribute* attr) {
  using P = PollAttribute;
  if (!attr) return P::Default_poll;
  return parse_id_payload<P>(P::Default_poll, P::Default_poll, {{"error", P::Error_poll}}, attr->attr_payload);
}

PollAttribute get_poll_attribute(const pt::Attributes& l) {
  return parse_poll_attribute(find_attribute(is_poll_attribute, l));
}

// (check_local_inline / check_poll_inline / check_poll_local only warn)

lam_t lfunction_with_attr(const FunctionAttribute& attr, const LFunction* f) {
  return lfunction(f->kind, f->params, f->return_, f->body, attr, f->loc);
}

lam_t add_tmc_attribute(lam_t expr, const Location&, const pt::Attributes& attributes) {
  auto* lf = as<Lfunction>(expr);
  if (!lf) return expr;
  const pt::Attribute* attr = find_attribute(is_tmc_attribute, attributes);
  if (!attr) return expr;
  // (tmc_candidate already set: Duplicated_attribute, not emitted)
  FunctionAttribute a = lf->f->attr;
  a.tmc_candidate = true;
  return lfunction_with_attr(a, lf->f);
}

lam_t add_poll_attribute(lam_t expr, const Location&, const pt::Attributes& attributes) {
  auto* lf = as<Lfunction>(expr);
  if (!lf || lf->f->attr.stub) return expr;
  PollAttribute poll = get_poll_attribute(attributes);
  if (poll == PollAttribute::Default_poll) return expr;
  FunctionAttribute a = lf->f->attr;
  a.poll = poll;
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

lam_t add_inline_attribute(lam_t expr, const Location&, const pt::Attributes& attributes) {
  auto* lf = as<Lfunction>(expr);
  if (!lf || lf->f->attr.stub) return expr;
  InlineAttribute inline_ = get_inline_attribute(attributes);
  if (inline_.kind == InlineAttribute::Kind::Default_inline) return expr;
  FunctionAttribute a = lf->f->attr;
  a.inline_ = inline_;
  return lfunction_with_attr(a, lf->f);
}

lam_t add_specialise_attribute(lam_t expr, const Location&, const pt::Attributes& attributes) {
  auto* lf = as<Lfunction>(expr);
  if (!lf || lf->f->attr.stub) return expr;
  SpecialiseAttribute specialise = get_specialise_attribute(attributes);
  if (specialise == SpecialiseAttribute::Default_specialise) return expr;
  FunctionAttribute a = lf->f->attr;
  a.specialise = specialise;
  return lfunction_with_attr(a, lf->f);
}

lam_t add_local_attribute(lam_t expr, const Location&, const pt::Attributes& attributes) {
  auto* lf = as<Lfunction>(expr);
  if (!lf || lf->f->attr.stub) return expr;
  LocalAttribute local = get_local_attribute(attributes);
  if (local == LocalAttribute::Default_local) return expr;
  FunctionAttribute a = lf->f->attr;
  a.local = local;
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
  if (!r.ok) return TailcallAttribute::Default_tailcall;  // (Attribute_payload, not emitted)
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
