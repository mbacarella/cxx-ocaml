// Port of typing/includemod_errorprinter.ml (TYPECHECKER.md stage 9): the
// linearized report of a module inclusion failure (err_msgs), of a functor
// application failure, and the first-class module coercion messages.
//
// OCaml's `Fmt.dprintf` closures become std::function printers; their
// arguments are evaluated when the closure is built (right to left, as an
// OCaml application), only the printing is delayed -- the trees of module
// types are computed at the same point as in ocamlc.
#include "cppcaml/typing/includemod_errorprinter.hpp"

#include <algorithm>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/cmi_format.hpp"
#include "cppcaml/typing/includeclass.hpp"
#include "cppcaml/typing/includecore.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/oprint.hpp"
#include "cppcaml/typing/pprintast.hpp"
#include "cppcaml/typing/printtyp.hpp"
#include "cppcaml/typing/signature_matching.hpp"

namespace cppcaml::typing::includemod_errorprinter {

namespace {

namespace fd = format_doc;
namespace E = includemod::error;
namespace tt = typedtree;
using fd::doc_printf;
using fd::Formatter;
using fd::fprintf;
using location::Msg;
using location::Report;
using misc::style::code_str;
using Printer = std::function<void(Formatter&)>;
using out_type::Mode;
using MK = ModuleType::Kind;

Printer ignore_p() {
  return [](Formatter&) {};
}
template <class... Args>
Printer dprintf(std::string_view fmt, Args... args) {
  std::string f(fmt);
  return [f, args...](Formatter& ppf) { fprintf(ppf, f, args...); };
}

// ---- Context ----

struct Pos {  // Module of Ident.t | Modtype of Ident.t | Arg of functor_parameter | Body of ..
  enum class K { Module, Modtype, Arg, Body } k;
  Ident::t id = nullptr;
  FunctorParameter fp{};
};
using Ctx = std::vector<Pos>;  // an OCaml list, head first

Path::t path_of_context(const Ctx& c) {
  Path::t p = Path::pident(c[0].id);
  for (std::size_t i = 1; i < c.size(); ++i) p = Path::pdot(p, ident::name(c[i].id));
  return p;
}

std::string argname(const FunctorParameter& x) {
  if (x.is_unit) return "";
  if (!x.id) return "_";
  return std::string(ident::name(x.id));
}

void context(Formatter& ppf, const Ctx& c, std::size_t i);
void context_mty(Formatter& ppf, const Ctx& c, std::size_t i) {
  if (i < c.size() && (c[i].k == Pos::K::Module || c[i].k == Pos::K::Modtype))
    fprintf(ppf, "@[<2>sig@ %a@;<1 -2>end@]", [&](Formatter& f) { context(f, c, i); });
  else
    context(ppf, c, i);
}
void args(Formatter& ppf, const Ctx& c, std::size_t i) {
  if (i < c.size() && c[i].k == Pos::K::Body)
    fprintf(ppf, "(%s)%a", argname(c[i].fp), [&](Formatter& f) { args(f, c, i + 1); });
  else if (i < c.size() && c[i].k == Pos::K::Arg)
    fprintf(ppf, "(%s :@ %a) : ...", argname(c[i].fp), [&](Formatter& f) { context_mty(f, c, i + 1); });
  else
    fprintf(ppf, " :@ %a", [&](Formatter& f) { context_mty(f, c, i); });
}
void context(Formatter& ppf, const Ctx& c, std::size_t i) {
  if (i >= c.size()) {
    fprintf(ppf, "<here>");
    return;
  }
  const Pos& p = c[i];
  switch (p.k) {
    case Pos::K::Module:
      fprintf(ppf, "@[<2>module %a%a@]", fd::pr(printtyp::ident, p.id), [&](Formatter& f) { args(f, c, i + 1); });
      break;
    case Pos::K::Modtype:
      fprintf(ppf, "@[<2>module type %a =@ %a@]", fd::pr(printtyp::ident, p.id),
              [&](Formatter& f) { context_mty(f, c, i + 1); });
      break;
    case Pos::K::Body:
      fprintf(ppf, "(%s) ->@ %a", argname(p.fp), [&](Formatter& f) { context_mty(f, c, i + 1); });
      break;
    case Pos::K::Arg:
      fprintf(ppf, "(%s : %a) -> ...", argname(p.fp), [&](Formatter& f) { context_mty(f, c, i + 1); });
      break;
  }
}
bool all_modules(const Ctx& c) {
  return std::all_of(c.begin(), c.end(), [](const Pos& p) { return p.k == Pos::K::Module; });
}
void alt_pp(Formatter& ppf, const Ctx& cxt) {
  if (cxt.empty()) return;
  if (all_modules(cxt))
    fprintf(ppf, ",@ in module %a", misc::style::code(printtyp::path, path_of_context(cxt)));
  else
    fprintf(ppf, ",@ @[<hv 2>at position@ %a@]",
            misc::style::code([](Formatter& f, Ctx c) { context(f, c, 0); }, cxt));
}
void ctx_pp(Formatter& ppf, const Ctx& cxt) {
  if (cxt.empty()) return;
  if (all_modules(cxt))
    fprintf(ppf, "In module %a:@ ", misc::style::code(printtyp::path, path_of_context(cxt)));
  else
    fprintf(ppf, "@[<hv 2>At position@ %a@]@ ", misc::style::code([](Formatter& f, Ctx c) { context(f, c, 0); }, cxt));
}
Ctx rev(const Ctx& c) { return Ctx(c.rbegin(), c.rend()); }
Ctx cons(Pos p, const Ctx& c) {
  Ctx r{p};
  r.insert(r.end(), c.begin(), c.end());
  return r;
}

// ---- Runtime_coercion ----

struct CoercePos {  // Item of int | InArg | InBody
  enum class K { Item, InArg, InBody } k;
  long n = 0;
};
struct RtChange {  // Transposition | Primitive_coercion | Alias_coercion
  enum class K { Transposition, Primitive_coercion, Alias_coercion } k;
  long a = 0, b = 0;
  std::string prim;
  Path::t path = nullptr;
};
using CPath = std::vector<CoercePos>;  // head first
using FirstChange = std::optional<std::pair<CPath, RtChange>>;

FirstChange first_change_under(const CPath& path, const tt::ModuleCoercion* coerc);
FirstChange first_item_transposition(const CPath& path, long pos, Slice<tt::PosCoercion> l, std::size_t k) {
  for (; k < l.size(); ++k, ++pos) {
    long n = l[k].pos;
    if (!(n < 0 || n == pos)) return std::make_pair(CPath(path.rbegin(), path.rend()), RtChange{RtChange::K::Transposition, pos, n});
  }
  return std::nullopt;
}
FirstChange first_non_id(const CPath& path, long pos, Slice<tt::PosCoercion> l, std::size_t k) {
  using CK = tt::ModuleCoercion::Kind;
  for (; k < l.size(); ++k, ++pos) {
    const tt::ModuleCoercion* c = l[k].cc;
    if (c->kind == CK::Tcoerce_none) continue;
    if (c->kind == CK::Tcoerce_alias) {
      RtChange r{RtChange::K::Alias_coercion};
      r.path = c->alias_path;
      return std::make_pair(CPath(path.rbegin(), path.rend()), r);
    }
    if (c->kind == CK::Tcoerce_primitive) {
      RtChange r{RtChange::K::Primitive_coercion};
      r.prim = std::string(c->prim->pc_desc->prim_name);
      return std::make_pair(CPath(path.rbegin(), path.rend()), r);
    }
    CPath p2 = path;
    p2.insert(p2.begin(), CoercePos{CoercePos::K::Item, pos});
    if (FirstChange fc = first_change_under(p2, c)) return fc;
  }
  return std::nullopt;
}
FirstChange first_change_under(const CPath& path, const tt::ModuleCoercion* coerc) {
  using CK = tt::ModuleCoercion::Kind;
  switch (coerc->kind) {
    case CK::Tcoerce_structure:
      if (FirstChange fc = first_item_transposition(path, 0, coerc->pos_cc, 0)) return fc;
      return first_non_id(path, 0, coerc->pos_cc, 0);
    case CK::Tcoerce_functor: {
      CPath pa = path;
      pa.insert(pa.begin(), CoercePos{CoercePos::K::InArg});
      if (FirstChange fc = first_change_under(pa, coerc->arg)) return fc;
      CPath pb = path;
      pb.insert(pb.begin(), CoercePos{CoercePos::K::InBody});
      return first_change_under(pb, coerc->res);
    }
    default: return std::nullopt;
  }
}

struct NotFound {};

const SignatureItem* runtime_item(long k, Signature s) {
  for (const SignatureItem* it : s) {
    if (!includemod::is_runtime_component(it)) continue;
    if (k == 0) return it;
    --k;
  }
  throw NotFound{};
}

std::pair<Ctx, Signature> find(env::t env, Ctx ctx, const CPath& path, std::size_t i, const ModuleType* mt) {
  for (;;) {
    if (mt->kind == MK::Mty_ident || mt->kind == MK::Mty_alias) {
      const ModtypeDeclaration* d;
      try {
        d = env::find_modtype(mt->path, env);
      } catch (const env::NotFound&) {
        throw NotFound{};
      }
      if (!d->mtd_type) throw NotFound{};
      mt = d->mtd_type;
      continue;
    }
    if (mt->kind == MK::Mty_signature && i >= path.size()) return {rev(ctx), mt->sign};
    if (i >= path.size()) throw NotFound{};
    const CoercePos& cp = path[i];
    if (mt->kind == MK::Mty_signature && cp.k == CoercePos::K::Item) {
      const SignatureItem* it = runtime_item(cp.n, mt->sign);
      if (it->kind != SignatureItem::Kind::Sig_module) throw NotFound{};
      ctx = cons(Pos{Pos::K::Module, it->id}, ctx);
      mt = it->md->md_type;
      ++i;
      continue;
    }
    if (mt->kind == MK::Mty_functor && !mt->param.is_unit && cp.k == CoercePos::K::InArg) {
      Pos p{Pos::K::Arg};
      p.fp = mt->param;
      ctx = cons(p, ctx);
      mt = mt->param.mty;
      ++i;
      continue;
    }
    if (mt->kind == MK::Mty_functor && cp.k == CoercePos::K::InBody) {
      Pos p{Pos::K::Body};
      p.fp = mt->param;
      ctx = cons(p, ctx);
      mt = mt->res;
      ++i;
      continue;
    }
    throw NotFound{};
  }
}

void pp_item(Formatter& ppf, const includemod::ItemIdentName& x) {
  fprintf(ppf, "%s %a", includemod::kind_of_field_desc(x.desc), code_str(std::string(ident::name(x.id))));
}

using CtxPrinter = std::function<void(Formatter&, const Ctx&)>;

void illegal_permutation(const CtxPrinter& ctx_printer, env::t env, Formatter& ppf, const ModuleType* mty,
                         const tt::ModuleCoercion* c) {
  FirstChange fc = first_change_under({}, c);
  if (!fc || fc->second.k != RtChange::K::Transposition) throw std::logic_error("Includemod_errorprinter.illegal_permutation");
  try {
    auto [ctx, mt] = find(env, {}, fc->first, 0, mty);
    includemod::ItemIdentName ik = includemod::item_ident_name(runtime_item(fc->second.a, mt));
    includemod::ItemIdentName il = includemod::item_ident_name(runtime_item(fc->second.b, mt));
    fprintf(ppf,
            "@[<hv 2>Illegal permutation of runtime components in a module type.@ @[For example%a,@]@ @[the %a@ and "
            "the %a are not in the same order@ in the expected and actual module types.@]@]",
            [&](Formatter& f) { ctx_printer(f, ctx); }, fd::pr(pp_item, ik), fd::pr(pp_item, il));
  } catch (const NotFound&) {
    fprintf(ppf, "Illegal permutation of runtime components in a module type.");
  }
}

void in_package_subtype(const CtxPrinter& ctx_printer, env::t env, const ModuleType* mty, const tt::ModuleCoercion* c,
                        Formatter& ppf) {
  FirstChange fc = first_change_under({}, c);
  if (!fc) {
    fprintf(ppf, "The two first-class module types differ by their runtime size.");
    return;
  }
  try {
    auto [ctx, mt] = find(env, {}, fc->first, 0, mty);
    const RtChange& ch = fc->second;
    switch (ch.k) {
      case RtChange::K::Primitive_coercion:
        fprintf(ppf, "@[The two first-class module types differ by a coercion of@ the primitive %a@ to a value%a.@]",
                code_str(ch.prim), [&](Formatter& f) { ctx_printer(f, ctx); });
        break;
      case RtChange::K::Alias_coercion:
        fprintf(ppf,
                "@[The two first-class module types differ by a coercion of@ a module alias %a@ to a module%a.@]",
                misc::style::code(printtyp::path, ch.path), [&](Formatter& f) { ctx_printer(f, ctx); });
        break;
      case RtChange::K::Transposition: {
        includemod::ItemIdentName ik = includemod::item_ident_name(runtime_item(ch.a, mt));
        includemod::ItemIdentName il = includemod::item_ident_name(runtime_item(ch.b, mt));
        fprintf(ppf,
                "@[@[The two first-class module types do not share@ the same positions for runtime components.@]@ "
                "@[For example,%a@ the %a@ occurs at the expected position of@ the %a.@]@]",
                [&](Formatter& f) { ctx_printer(f, ctx); }, fd::pr(pp_item, ik), fd::pr(pp_item, il));
        break;
      }
    }
  } catch (const NotFound&) {
    fprintf(ppf, "@[The two packages types do not share@ the@ same@ positions@ for@ runtime@ components.@]");
  }
}

// ---- helpers ----

bool is_big_mty(const ModuleType* a, const ModuleType* b) {
  long size = clflags::error_size;
  return size > 0 && static_cast<long>(cmi_format::marshaled_size(a, b)) > size;
}
bool is_big_mtd(const ModtypeDeclaration* a, const ModtypeDeclaration* b) {
  long size = clflags::error_size;
  return size > 0 && static_cast<long>(cmi_format::marshaled_size(a, b)) > size;
}

void show_loc(std::string_view msg, Formatter& ppf, const Location& loc) {
  std::string_view f = loc.loc_start.pos_fname;
  if (f.empty() || f == "_none_" || f == "//toplevel//") return;
  std::string m(msg);
  fprintf(ppf, "@\n@[<2>%a:@ %s@]", [&](Formatter& ff) { location::doc::loc(ff, loc); }, m);
}
void show_locs(Formatter& ppf, const Location& loc1, const Location& loc2) {
  show_loc("Expected declaration", ppf, loc2);
  show_loc("Actual declaration", ppf, loc1);
}

Printer dmodtype(const ModuleType* mty) {
  const outcometree::OutModuleType* t = out_type::tree_of_modtype(mty);
  return [t](Formatter& ppf) { oprint::out_module_type(ppf, t); };
}

void space(Formatter& ppf) { fprintf(ppf, "@ "); }

// ---- With_shorthand ----

template <class A>
struct Named {  // 'a named
  A item;
  std::string name;
};
struct Short {  // Original of module_type | Synthetic of module_type named
  bool synthetic;
  const ModuleType* mty;
  std::string name;
};
struct ShortParam {  // Unit | Named of (Ident.t option * module_type t)
  bool unit;
  Ident::t id = nullptr;
  Short short_{};
};

enum class Side { Got, Expected, Unneeded };
enum class Variant { App, Inclusion };

std::string make_name(Side side, long pos) {
  switch (side) {
    case Side::Got: return "$S" + std::to_string(pos);
    case Side::Expected: return "$T" + std::to_string(pos);
    case Side::Unneeded: return "...";
  }
  return "";
}

Short modtype_short(const ModuleType* mty, const std::string& name) {
  if (mty->kind == MK::Mty_ident || mty->kind == MK::Mty_alias || (mty->kind == MK::Mty_signature && mty->sign.empty()))
    return Short{false, mty, ""};
  return Short{true, mty, name};
}
ShortParam functor_param(const Named<FunctorParameter>& ua) {
  if (ua.item.is_unit) return ShortParam{true};
  return ShortParam{false, ua.item.id, modtype_short(ua.item.mty, ua.name)};
}
Printer pp_short(const Short& s) {
  if (!s.synthetic) return dmodtype(s.mty);
  return dprintf("%s", s.name);
}
Printer pp_orig(const Short& s) { return dmodtype(s.mty); }

Printer definition(const Named<FunctorParameter>& x) {
  ShortParam sp = functor_param(x);
  if (sp.unit) return dprintf("()");
  if (!sp.short_.synthetic) return dmodtype(sp.short_.mty);
  Printer m = dmodtype(sp.short_.mty);
  std::string name = sp.short_.name;
  return [name, m](Formatter& ppf) { fprintf(ppf, "%s@ =@ %t", name, m); };
}
Printer param(const Named<FunctorParameter>& x) {
  ShortParam sp = functor_param(x);
  if (sp.unit) return dprintf("()");
  return pp_short(sp.short_);
}
Printer qualified_param(const Named<FunctorParameter>& x) {
  ShortParam sp = functor_param(x);
  if (sp.unit) return dprintf("()");
  if (!sp.id && !sp.short_.synthetic && sp.short_.mty->kind == MK::Mty_signature && sp.short_.mty->sign.empty())
    return dprintf("(sig end)");
  if (!sp.id) return pp_short(sp.short_);
  Printer p = pp_short(sp.short_);
  std::string n(ident::name(sp.id));
  return [n, p](Formatter& ppf) { fprintf(ppf, "(%s : %t)", n, p); };
}
using AK = E::FunctorArgDescr::Kind;
Printer definition_of_argument(const Named<includemod::AppArg>& ua) {
  const auto& [arg, mty] = ua.item;
  switch (arg.kind) {
    case AK::Unit: return dprintf("()");
    case AK::Empty_struct: return dprintf("(struct end)");
    case AK::Named: {
      Short m = modtype_short(mty, ua.name);
      Printer p = pp_orig(m);
      Path::t path = arg.path;
      return [path, p](Formatter& ppf) { fprintf(ppf, "%a@ :@ %t", fd::pr(printtyp::path, path), p); };
    }
    case AK::Anonymous: {
      Short s = modtype_short(mty, ua.name);
      if (!s.synthetic) return dmodtype(s.mty);
      Printer m = dmodtype(s.mty);
      std::string name = s.name;
      return [name, m](Formatter& ppf) { fprintf(ppf, "%s@ :@ %t", name, m); };
    }
  }
  return ignore_p();
}
Printer arg_p(const Named<includemod::AppArg>& ua) {
  const auto& [arg, mty] = ua.item;
  switch (arg.kind) {
    case AK::Unit: return dprintf("()");
    case AK::Empty_struct: return dprintf("(struct end)");
    case AK::Named: {
      Path::t p = arg.path;
      return [p](Formatter& ppf) { printtyp::path(ppf, p); };
    }
    case AK::Anonymous: return pp_short(modtype_short(mty, ua.name));
  }
  return ignore_p();
}

// patch ctx p: the shorthands
template <class L>
using NChange = diffing::Change<Named<L>, Named<FunctorParameter>, const tt::ModuleCoercion*, E::FunctorParamSymptom>;
template <class L>
using NPatch = std::vector<std::pair<long, NChange<L>>>;

template <class L>
NPatch<L> with_shorthands(Variant ctx,
                          const std::vector<diffing::Change<L, FunctorParameter, const tt::ModuleCoercion*,
                                                            E::FunctorParamSymptom>>& p) {
  auto elide_if_app = [ctx](Side s) { return ctx == Variant::App ? Side::Unneeded : s; };
  NPatch<L> r;
  long i = 0;
  for (const auto& d : p) {
    long pos = ++i;
    NChange<L> n{static_cast<typename NChange<L>::K>(d.k)};
    using K = typename NChange<L>::K;
    switch (n.k) {
      case K::Insert: n.right = {d.right, make_name(Side::Expected, pos)}; break;
      case K::Delete: n.left = {d.left, make_name(elide_if_app(Side::Got), pos)}; break;
      case K::Change:
        n.left = {d.left, make_name(Side::Got, pos)};
        n.right = {d.right, make_name(Side::Expected, pos)};
        n.diff = d.diff;
        break;
      case K::Keep:
        n.left = {d.left, make_name(Side::Got, pos)};
        n.right = {d.right, make_name(elide_if_app(Side::Expected), pos)};
        n.eq = d.eq;
        break;
    }
    r.push_back({pos, n});
  }
  return r;
}

template <class L>
NPatch<L> prepare_patch(bool drop, Variant ctx,
                        std::vector<diffing::Change<L, FunctorParameter, const tt::ModuleCoercion*,
                                                    E::FunctorParamSymptom>> patch) {
  if (drop) {
    while (!patch.empty() && patch.back().k == diffing::Change<L, FunctorParameter, const tt::ModuleCoercion*,
                                                                E::FunctorParamSymptom>::K::Insert)
      patch.pop_back();
  }
  return with_shorthands<L>(ctx, patch);
}

diffing::ChangeKind classify_k(int k) {
  switch (k) {
    case 0: return diffing::ChangeKind::Deletion;
    case 1: return diffing::ChangeKind::Insertion;
    case 2: return diffing::ChangeKind::Preservation;
    default: return diffing::ChangeKind::Modification;
  }
}

// ---- Functor_suberror ----

Ident::t param_id(const Named<FunctorParameter>& x) { return x.item.is_unit ? nullptr : x.item.id; }

// Diffing.classify
template <class Kind>
diffing::ChangeKind classify(Kind k) {
  switch (k) {
    case Kind::Delete: return diffing::ChangeKind::Deletion;
    case Kind::Insert: return diffing::ChangeKind::Insertion;
    case Kind::Change: return diffing::ChangeKind::Modification;
    case Kind::Keep: return diffing::ChangeKind::Preservation;
  }
  return diffing::ChangeKind::Preservation;
}

// pretty_params sep proj printer patch
template <class T>
Printer pretty_params(const std::vector<std::tuple<Ident::t, T, diffing::ChangeKind>>& params,
                      const std::function<Printer(const T&)>& printer, std::size_t i = 0) {
  if (i >= params.size()) return ignore_p();
  // the style of Diffing.classify x
  auto pp_param = [&](const T& p, diffing::ChangeKind k) -> Printer {
    Printer pr = printer(p);
    return [pr, k](Formatter& ppf) {
      fd::pp_open_stag(ppf, diffing::style_tag(k));
      pr(ppf);
      fd::pp_close_stag(ppf);
    };
  };
  if (i + 1 == params.size()) return pp_param(std::get<1>(params[i]), std::get<2>(params[i]));
  // dprintf "%t%a%t" (pp_param param) sep () (hide_id id q): right to left
  Ident::t id = std::get<0>(params[i]);
  Printer rest;
  if (!id)
    rest = pretty_params(params, printer, i + 1);
  else
    out_type::ident_names::with_fuzzy(id, [&] { rest = pretty_params(params, printer, i + 1); });
  Printer head = pp_param(std::get<1>(params[i]), std::get<2>(params[i]));
  return [head, rest](Formatter& ppf) {
    head(ppf);
    space(ppf);
    rest(ppf);
  };
}

template <class L>
Printer expected_p(const NPatch<L>& d) {
  using K = typename NChange<L>::K;
  std::vector<std::tuple<Ident::t, Named<FunctorParameter>, diffing::ChangeKind>> ps;
  for (const auto& [_, x] : d)
    if (x.k != K::Delete) ps.push_back({param_id(x.right), x.right, classify(x.k)});
  return pretty_params<Named<FunctorParameter>>(ps, qualified_param);
}

Printer inclusion_got(const NPatch<FunctorParameter>& d) {
  using K = NChange<FunctorParameter>::K;
  std::vector<std::tuple<Ident::t, Named<FunctorParameter>, diffing::ChangeKind>> ps;
  for (const auto& [_, x] : d)
    if (x.k != K::Insert) ps.push_back({param_id(x.left), x.left, classify(x.k)});
  return pretty_params<Named<FunctorParameter>>(ps, qualified_param);
}
Printer inclusion_insert(const Named<FunctorParameter>& mty) {
  Printer d = definition(mty);
  return [d](Formatter& ppf) { fprintf(ppf, "An argument appears to be missing with module type@;<1 2>@[%t@]", d); };
}
Printer inclusion_delete(const Named<FunctorParameter>& mty) {
  Printer d = definition(mty);
  return [d](Formatter& ppf) { fprintf(ppf, "An extra argument is provided of module type@;<1 2>@[%t@]", d); };
}
Printer inclusion_ok(const Named<FunctorParameter>& x, const Named<FunctorParameter>& y) {
  Printer py = param(y);
  Printer px = param(x);
  return [px, py](Formatter& ppf) { fprintf(ppf, "Module types %t and %t match", px, py); };
}
Printer inclusion_diff(const Named<FunctorParameter>& g0, const Named<FunctorParameter>& e0,
                       const std::function<Printer()>& more) {
  Printer g = definition(g0);
  Printer e = definition(e0);
  Printer m = more();
  return [g, e, m](Formatter& ppf) {
    fprintf(ppf, "Module types do not match:@ @[%t@]@;<1 -2>does not include@ @[%t@]%t", g, e, m);
  };
}
Printer inclusion_incompatible(const FunctorParameter& p) {
  if (p.is_unit) return dprintf("The functor was expected to be applicative at this position");
  return dprintf("The functor was expected to be generative at this position");
}

Printer app_got(const NPatch<includemod::AppArg>& d) {
  using K = NChange<includemod::AppArg>::K;
  std::vector<std::tuple<Ident::t, Named<includemod::AppArg>, diffing::ChangeKind>> ps;
  for (const auto& [_, x] : d)
    if (x.k != K::Insert) ps.push_back({nullptr, x.left, classify(x.k)});
  return pretty_params<Named<includemod::AppArg>>(ps, arg_p);
}
Printer app_delete(const Named<includemod::AppArg>& mty) {
  Printer d = definition_of_argument(mty);
  return [d](Formatter& ppf) { fprintf(ppf, "The following extra argument is provided@;<1 2>@[%t@]", d); };
}
Printer app_ok(const Named<includemod::AppArg>& x, const Named<FunctorParameter>& y) {
  ShortParam sp = functor_param(y);
  Printer pp_orig_name = ignore_p();
  if (!sp.unit && !sp.short_.synthetic) {
    Printer m = dmodtype(sp.short_.mty);
    pp_orig_name = [m](Formatter& ppf) { fprintf(ppf, " %t", m); };
  }
  Printer a = arg_p(x);
  return [a, pp_orig_name](Formatter& ppf) {
    fprintf(ppf, "Module %t matches the expected module type%t", a, pp_orig_name);
  };
}
Printer app_diff(const Named<includemod::AppArg>& g0, const Named<FunctorParameter>& e0,
                 const std::function<Printer()>& more) {
  Printer g = definition_of_argument(g0);
  Printer e = definition(e0);
  Printer m = more();
  return [g, e, m](Formatter& ppf) {
    fprintf(ppf, "Modules do not match:@ @[%t@]@;<1 -2>is not included in@ @[%t@]%t", g, e, m);
  };
}
Printer app_single_diff(const Named<includemod::AppArg>& g, const Named<FunctorParameter>& e0,
                        const std::function<Printer()>& more) {
  const ModuleType* mty = g.item.second;
  Printer e = e0.item.is_unit ? dprintf("()") : dmodtype(e0.item.mty);
  Printer m = more();
  Printer dm = dmodtype(mty);
  return [dm, e, m](Formatter& ppf) {
    fprintf(ppf, "Modules do not match:@ @[%t@]@;<1 -2>is not included in@ @[%t@]%t", dm, e, m);
  };
}
Printer app_incompatible(const E::FunctorArgDescr& a) {
  if (a.kind == AK::Unit) return dprintf("The functor was expected to be applicative at this position");
  if (a.kind == AK::Empty_struct) throw std::logic_error("Includemod_errorprinter.App.incompatible");
  return dprintf("The functor was expected to be generative at this position");
}

template <class L>
using SubFn = std::function<Printer(bool, const includemod::InclusionEnv&, const NChange<L>&)>;

template <class L>
Msg subcase(const SubFn<L>& sub, bool expansion_token, const includemod::InclusionEnv& env,
            const std::pair<long, NChange<L>>& pd) {
  Printer p;
  printtyp::wrap_printing_env(true, env.i_env, [&] { p = sub(expansion_token, env, pd.second); });
  long pos = pd.first;
  diffing::ChangeKind k = classify_k(static_cast<int>(pd.second.k));
  return location::msg_noloc("%a%a%a%a@[<hv 2>%t@]%a", [](Formatter& f) { fd::pp_print_tab(f); },
                             [](Formatter& f) { fd::pp_open_tbox(f); },
                             [pos, k](Formatter& f) { diffing::prefix(f, pos, k); },
                             [](Formatter& f) { fd::pp_set_tab(f); }, p, [](Formatter& f) { fd::pp_close_tbox(f); });
}
template <class L>
Msg onlycase(const SubFn<L>& sub, bool expansion_token, const includemod::InclusionEnv& env,
             const std::pair<long, NChange<L>>& pd) {
  Printer p;
  printtyp::wrap_printing_env(true, env.i_env, [&] { p = sub(expansion_token, env, pd.second); });
  return location::msg_noloc("%a@[<hv 2>%t@]", [](Formatter& f) { fd::pp_print_tab(f); }, p);
}
// params: the subcases, a list head first
template <class L>
std::vector<Msg> params_msgs(const SubFn<L>& sub, bool expansion_token, const includemod::InclusionEnv& env,
                             const NPatch<L>& l) {
  if (l.size() == 1) return {onlycase<L>(sub, expansion_token, env, l[0])};
  std::vector<Msg> subcases;  // head first
  std::size_t i = 0;
  while (i < l.size()) {
    if (l[i].second.k == NChange<L>::K::Keep) {
      subcases.insert(subcases.begin(), subcase<L>(sub, expansion_token, env, l[i]));
      ++i;
      continue;
    }
    subcases.insert(subcases.begin(), subcase<L>(sub, expansion_token, env, l[i]));
    for (std::size_t j = i + 1; j < l.size(); ++j)
      subcases.insert(subcases.begin(), subcase<L>(sub, false, env, l[j]));
    break;
  }
  return subcases;
}

// ---- the linearized error ----

using Msgs = std::vector<Msg>;  // an OCaml list, head first
Msgs cons_m(Msg m, Msgs l) {
  l.insert(l.begin(), std::move(m));
  return l;
}

Msg with_context(const Ctx& ctx, const Printer& p) {
  Ctx r = rev(ctx);
  return location::msg_noloc("%a%a", [r](Formatter& f) { ctx_pp(f, r); }, p);
}
Msg dwith_context(const Ctx& ctx, const Printer& p) {
  Ctx r = rev(ctx);
  return location::msg_noloc("%a%t", [r](Formatter& f) { ctx_pp(f, r); }, p);
}

Printer coalesce(const Msgs& msgs) {
  if (msgs.empty()) return ignore_p();
  Msgs before(msgs.rbegin(), msgs.rend());
  return [before](Formatter& ppf) {
    fd::pp_print_list(ppf, [](Formatter& f, const Msg& m) { fd::pp_doc(f, m.txt); }, before, space);
  };
}
Printer subcase_list(const Msgs& l) {
  if (l.empty()) return ignore_p();
  Msgs r(l.rbegin(), l.rend());
  return [r](Formatter& ppf) {
    fprintf(ppf, "@;<1 -2>@[%a@]", [&](Formatter& f) {
      fd::pp_print_list(f, [](Formatter& ff, const Msg& m) { fd::pp_doc(ff, m.txt); }, r, space);
    });
  };
}

// core env id x
Printer core(env::t env, Ident::t id, const E::CoreSigitemSymptom& x) {
  using K = E::CoreSigitemSymptom::Kind;
  switch (x.kind) {
    case K::Value_descriptions: {
      const outcometree::OutSigItem* te = out_type::tree_of_value_description(id, x.vd2);
      const outcometree::OutSigItem* tg = out_type::tree_of_value_description(id, x.vd1);
      includecore::ValueMismatch sy = *x.value;
      Location l1 = x.vd1->val_loc, l2 = x.vd2->val_loc;
      return [=](Formatter& ppf) {
        fprintf(ppf, "@[<v>@[<hv>%s:@;<1 2>%a@ %s@;<1 2>%a@]%a%a@]", "Values do not match",
                fd::pr(oprint::out_sig_item, tg), "is not included in", fd::pr(oprint::out_sig_item, te),
                [&](Formatter& f) { includecore::report_value_mismatch("the first", "the second", env, f, sy); },
                [&](Formatter& f) { show_locs(f, l1, l2); });
      };
    }
    case K::Type_declarations: {
      const outcometree::OutSigItem* te = out_type::tree_of_type_declaration(id, x.td2, RecStatus::Trec_first);
      const outcometree::OutSigItem* tg = out_type::tree_of_type_declaration(id, x.td1, RecStatus::Trec_first);
      includecore::TypeMismatch sy = *x.type;
      Location l1 = x.td1->type_loc, l2 = x.td2->type_loc;
      return [=](Formatter& ppf) {
        fprintf(ppf, "@[<v>@[<hv>%s:@;<1 2>%a@ %s@;<1 2>%a@]@,%a%a@]", "Type declarations do not match",
                fd::pr(oprint::out_sig_item, tg), "is not included in", fd::pr(oprint::out_sig_item, te),
                [&](Formatter& f) {
                  includecore::report_type_mismatch("the first", "the second", "declaration", env, f, sy);
                },
                [&](Formatter& f) { show_locs(f, l1, l2); });
      };
    }
    case K::Extension_constructors: {
      const outcometree::OutSigItem* te = out_type::tree_of_extension_constructor(id, x.ext2, ExtStatus::Text_first);
      const outcometree::OutSigItem* tg = out_type::tree_of_extension_constructor(id, x.ext1, ExtStatus::Text_first);
      includecore::ExtensionConstructorMismatch sy = *x.ext;
      Location l1 = x.ext1->ext_loc, l2 = x.ext2->ext_loc;
      return [=](Formatter& ppf) {
        fprintf(ppf, "@[<v>@[<hv>%s:@;<1 2>%a@ %s@;<1 2>%a@]@ %a%a@]", "Extension declarations do not match",
                fd::pr(oprint::out_sig_item, tg), "is not included in", fd::pr(oprint::out_sig_item, te),
                [&](Formatter& f) {
                  includecore::report_extension_constructor_mismatch("the first", "the second", "declaration", env,
                                                                     f, sy);
                },
                [&](Formatter& f) { show_locs(f, l1, l2); });
      };
    }
    case K::Class_type_declarations: {
      const outcometree::OutSigItem* te = out_type::tree_of_cltype_declaration(id, x.clty2, RecStatus::Trec_first);
      const outcometree::OutSigItem* tg = out_type::tree_of_cltype_declaration(id, x.clty1, RecStatus::Trec_first);
      std::vector<ctype::ClassMatchFailure> sy = x.classes;
      return [=](Formatter& ppf) {
        fprintf(ppf, "@[<hv 2>Class type declarations do not match:@ %a@;<1 -2>does not match@ %a@]@ %a",
                fd::pr(oprint::out_sig_item, tg), fd::pr(oprint::out_sig_item, te),
                [&](Formatter& f) { includeclass::report_error_doc(Mode::Type_scheme, f, sy); });
      };
    }
    case K::Class_declarations: {
      const outcometree::OutSigItem* t1 = out_type::tree_of_class_declaration(id, x.cl1, RecStatus::Trec_first);
      const outcometree::OutSigItem* t2 = out_type::tree_of_class_declaration(id, x.cl2, RecStatus::Trec_first);
      std::vector<ctype::ClassMatchFailure> sy = x.classes;
      return [=](Formatter& ppf) {
        fprintf(ppf, "@[<hv 2>Class declarations do not match:@ %a@;<1 -2>does not match@ %a@]@ %a",
                fd::pr(oprint::out_sig_item, t1), fd::pr(oprint::out_sig_item, t2),
                [&](Formatter& f) { includeclass::report_error_doc(Mode::Type_scheme, f, sy); });
      };
    }
  }
  return ignore_p();
}

void missing_field(Formatter& ppf, const SignatureItem* item) {
  includemod::ItemIdentName x = includemod::item_ident_name(item);
  fprintf(ppf, "The %s %a is required but not provided%a", includemod::kind_of_field_desc(x.desc),
          misc::style::code(printtyp::ident, x.id), [&](Formatter& f) { show_loc("Expected declaration", f, x.loc); });
}

void suggest_renaming_field(Formatter& ppf, Ident::t left_id, const Location& left_loc, const SignatureItem* item) {
  includemod::ItemIdentName x = includemod::item_ident_name(item);
  fd::Doc main = doc_printf("The %s@{<ralign> @}%a is required but not provided.",
                            includemod::kind_of_field_desc(x.desc), code_str(std::string(ident::name(x.id))));
  fd::Doc hint = doc_printf("@{<hint>Hint@}: @{<ralign>@}%a is a close match.%a%a",
                            code_str(std::string(ident::name(left_id))),
                            [&](Formatter& f) { show_loc("Expected declaration", f, x.loc); },
                            [&](Formatter& f) { show_loc("Possible match", f, left_loc); });
  auto [m, h] = misc::align_hint("", main, hint);
  fd::pp_doc(ppf, m);
  fd::pp_print_cut(ppf);
  fd::pp_doc(ppf, h);
}

Printer module_types(const E::ModuleTypeDiff& d) {
  const outcometree::OutModuleType* t2 = out_type::tree_of_modtype(d.expected);
  const outcometree::OutModuleType* t1 = out_type::tree_of_modtype(d.got);
  return [t1, t2](Formatter& ppf) {
    fprintf(ppf, "@[<hv 2>Modules do not match:@ %a@;<1 -2>is not included in@ %a@]",
            fd::pr(oprint::out_module_type, t1), fd::pr(oprint::out_module_type, t2));
  };
}
Printer eq_module_types(const E::ModuleTypeDiff& d) {
  const outcometree::OutModuleType* t2 = out_type::tree_of_modtype(d.expected);
  const outcometree::OutModuleType* t1 = out_type::tree_of_modtype(d.got);
  return [t1, t2](Formatter& ppf) {
    fprintf(ppf, "@[<hv 2>Module types do not match:@ %a@;<1 -2>is not equal to@ %a@]",
            fd::pr(oprint::out_module_type, t1), fd::pr(oprint::out_module_type, t2));
  };
}
Printer module_type_declarations(Ident::t id, const ModtypeDeclaration* d1, const ModtypeDeclaration* d2) {
  const outcometree::OutSigItem* t2 = out_type::tree_of_modtype_declaration(id, d2);
  const outcometree::OutSigItem* t1 = out_type::tree_of_modtype_declaration(id, d1);
  return [t1, t2](Formatter& ppf) {
    fprintf(ppf, "@[<hv 2>Module type declarations do not match:@ %a@;<1 -2>does not match@ %a@]",
            fd::pr(oprint::out_sig_item, t1), fd::pr(oprint::out_sig_item, t2));
  };
}

std::optional<Printer> core_module_type_symptom(const E::CoreModuleTypeSymptom& x) {
  if (x.kind != E::CoreModuleTypeSymptom::Kind::Unbound_module_path) return std::nullopt;
  Path::t p = x.path;
  return [p](Formatter& ppf) { fprintf(ppf, "Unbound module %a", misc::style::code(printtyp::path, p)); };
}

Msgs functor_expected(const Msgs& before, const Ctx& ctx) {
  Printer main = dprintf("@[This module should not be@ a@ structure,@ a@ functor@ was expected.@]");
  return cons_m(dwith_context(ctx, main), before);
}

Msgs unexpected_functor(env::t env, const Msgs& before, const Ctx& ctx, const E::FunctorParamsInfo& got,
                        const E::FunctorParamsInfo& expected) {
  const ModuleType* rmty = got.res;
  Printer intro =
      expected.res->kind == MK::Mty_ident
          ? dprintf("@[This module should not be a functor,@ a@ module with an@ abstract@ module@ type@ was expected.@]")
          : dprintf("@[This module should not be a functor,@ a@ structure was expected.@]");
  Printer main;
  try {
    includemod::modtypes_consistency(location::none(), env, rmty, expected.res);
    main = [intro](Formatter& ppf) { fprintf(ppf, "%t@ @{<hint>Hint@}: Did you forget to apply the functor?", intro); };
  } catch (...) {
    main = [intro](Formatter& ppf) {
      fprintf(ppf,
              "%t@ @[Moreover,@ the type of the functor@ body@ is@ incompatible@ with@ the@ expected@ module "
              "type.@]",
              intro);
    };
  }
  return cons_m(dwith_context(ctx, main), before);
}

Msgs module_type(bool expansion_token, bool eqmode, const includemod::InclusionEnv& env, Msgs before, const Ctx& ctx,
                 const E::ModuleTypeDiff& diff);
Msgs module_type_symptom(bool eqmode, bool expansion_token, const includemod::InclusionEnv& env, Msgs before,
                         const Ctx& ctx, const E::ModuleTypeSymptom& s);
Msgs signature(bool expansion_token, Msgs before, const Ctx& ctx, const E::SignatureSymptom& sgs);
Printer functor_arg_diff(bool expansion_token, const includemod::InclusionEnv& env,
                         const NChange<FunctorParameter>& patch);

Msgs compare_functor_params(bool expansion_token, const includemod::InclusionEnv& env, const Msgs& before,
                            const Ctx& ctx, const E::FunctorParamsInfo& got, const E::FunctorParamsInfo& expected) {
  NPatch<FunctorParameter> d = prepare_patch<FunctorParameter>(
      false, Variant::Inclusion, includemod::functor_inclusion_diff(env, got.params, got.res, expected.params));
  Printer actual = inclusion_got(d);
  Printer exp = expected_p<FunctorParameter>(d);
  Printer main = [actual, exp](Formatter& ppf) {
    fprintf(ppf, "@[<hv 2>Modules do not match:@ @[%t@ -> ...@]@;<1 -2>is not included in@ @[%t@ -> ...@]@]", actual,
            exp);
  };
  Msgs msgs = cons_m(dwith_context(ctx, main), before);
  Msgs functor_suberrors;
  if (expansion_token) functor_suberrors = params_msgs<FunctorParameter>(functor_arg_diff, expansion_token, env, d);
  functor_suberrors.insert(functor_suberrors.end(), msgs.begin(), msgs.end());
  return functor_suberrors;
}

Msgs functor_params(bool expansion_token, const includemod::InclusionEnv& env, const Msgs& before, const Ctx& ctx,
                    const E::FunctorParamsInfo& got, const E::FunctorParamsInfo& expected) {
  if (got.params.empty()) return functor_expected(before, ctx);
  if (expected.params.empty()) return unexpected_functor(env.i_env, before, ctx, got, expected);
  return compare_functor_params(expansion_token, env, before, ctx, got, expected);
}

Msgs module_type(bool expansion_token, bool eqmode, const includemod::InclusionEnv& env, Msgs before, const Ctx& ctx,
                 const E::ModuleTypeDiff& diff) {
  using SK = E::ModuleTypeSymptom::Kind;
  const E::ModuleTypeSymptom& sy = *diff.symptom;
  if (sy.kind == SK::After_alias_expansion) return module_type_symptom(eqmode, expansion_token, env, before, ctx, sy);
  if (sy.kind == SK::Functor_params) return functor_params(expansion_token, env, before, ctx, sy.got, sy.expected);
  auto inner = eqmode ? eq_module_types : module_types;
  Msg next = sy.kind == SK::Mt_core ? dwith_context(ctx, inner(diff))
             : is_big_mty(diff.got, diff.expected) ? location::msg_noloc("...")
                                                   : dwith_context(ctx, inner(diff));
  before = cons_m(next, before);
  return module_type_symptom(eqmode, expansion_token, env, before, ctx, sy);
}

Msgs module_type_symptom(bool eqmode, bool expansion_token, const includemod::InclusionEnv& env, Msgs before,
                         const Ctx& ctx, const E::ModuleTypeSymptom& s) {
  using SK = E::ModuleTypeSymptom::Kind;
  switch (s.kind) {
    case SK::Mt_core: {
      std::optional<Printer> m = core_module_type_symptom(s.core);
      if (!m) return before;
      return cons_m(location::msg_noloc("%t", *m), before);
    }
    case SK::Signature: return signature(expansion_token, before, ctx, *s.sig);
    case SK::Functor_params: return functor_params(expansion_token, env, before, ctx, s.got, s.expected);
    case SK::Functor_result: return module_type(expansion_token, false, env, before, ctx, *s.diff);
    case SK::After_alias_expansion: return module_type(expansion_token, eqmode, env, before, ctx, *s.diff);
  }
  return before;
}

Msgs module_type_decl(bool expansion_token, const includemod::InclusionEnv& env, Msgs before, const Ctx& ctx,
                      Ident::t id, const ModtypeDeclaration* got, const ModtypeDeclaration* expected,
                      const E::ModuleTypeDeclarationSymptom& sy) {
  Msg next = is_big_mtd(got, expected) ? location::msg_noloc("...")
                                       : dwith_context(ctx, module_type_declarations(id, got, expected));
  before = cons_m(next, before);
  using K = E::ModuleTypeDeclarationSymptom::Kind;
  Ctx c2 = cons(Pos{Pos::K::Modtype, id}, ctx);
  switch (sy.kind) {
    case K::Not_less_than:
      before = cons_m(location::msg_noloc("The first module type is not included in the second"), before);
      return module_type(expansion_token, true, env, before, c2, *sy.diff);
    case K::Not_greater_than:
      before = cons_m(location::msg_noloc("The second module type is not included in the first"), before);
      return module_type(expansion_token, true, env, before, c2, *sy.diff);
    case K::Incomparable: return module_type(expansion_token, true, env, before, c2, *sy.less_than);
    case K::Illegal_permutation: {
      if (!got->mtd_type) throw std::logic_error("Includemod_errorprinter.module_type_decl");
      const ModuleType* mty = got->mtd_type;
      const tt::ModuleCoercion* c = sy.coercion;
      env::t e = env.i_env;
      Printer p = [mty, c, e](Formatter& ppf) { illegal_permutation(alt_pp, e, ppf, mty, c); };
      return cons_m(with_context(c2, p), before);
    }
  }
  return before;
}

Msgs sigitem(bool expansion_token, const includemod::InclusionEnv& env, const Msgs& before, const Ctx& ctx,
             Ident::t name, const E::SigitemSymptom& s) {
  using K = E::SigitemSymptom::Kind;
  switch (s.kind) {
    case K::Core: return cons_m(dwith_context(ctx, core(env.i_env, name, s.core)), before);
    case K::Module_type:
      return module_type(expansion_token, false, env, before, cons(Pos{Pos::K::Module, name}, ctx), *s.diff);
    case K::Module_type_declaration:
      return module_type_decl(expansion_token, env, before, ctx, name, s.mtd1, s.mtd2, *s.mtd_symptom);
  }
  return before;
}

Msgs signature(bool expansion_token, Msgs before, const Ctx& ctx, const E::SignatureSymptom& sgs) {
  Msgs result;
  printtyp::wrap_printing_env(true, sgs.env, [&] {
    signature_matching::Report r = signature_matching::suggest(sgs);
    auto suggestion_text = [](Formatter& ppf, const signature_matching::Suggestion<signature_matching::Alteration>& s) {
      if (s.alteration.missing)
        missing_field(ppf, s.subject);
      else
        suggest_renaming_field(ppf, s.alteration.id, s.alteration.loc, s.subject);
    };
    if (!r.alterations.empty()) {
      if (!expansion_token) {
        result = before;
        return;
      }
      auto last = r.alterations.back();
      // List.map (msg "%a" suggestion_text) init @ (with_context .. last :: before): right to left
      Msgs tail = cons_m(with_context(ctx, fd::pr(suggestion_text, last)), before);
      Msgs head;
      for (std::size_t i = 0; i + 1 < r.alterations.size(); ++i)
        head.push_back(location::msg_noloc("%a", fd::pr(suggestion_text, r.alterations[i])));
      head.insert(head.end(), tail.begin(), tail.end());
      result = head;
      return;
    }
    if (r.incompatibles.empty()) throw std::logic_error("Includemod_errorprinter.signature");
    const auto& a = r.incompatibles[0];
    includemod::InclusionEnv env{sgs.env, sgs.subst};
    result = sigitem(expansion_token, env, before, ctx, types::signature_item_id(a.subject), a.alteration);
  });
  return result;
}

Printer functor_arg_diff(bool expansion_token, const includemod::InclusionEnv& env,
                         const NChange<FunctorParameter>& patch) {
  using K = NChange<FunctorParameter>::K;
  switch (patch.k) {
    case K::Insert: return inclusion_insert(patch.right);
    case K::Delete: return inclusion_delete(patch.left);
    case K::Keep: return inclusion_ok(patch.left, patch.right);
    case K::Change:
      if (patch.diff.kind == E::FunctorParamSymptom::Kind::Incompatible_params)
        return inclusion_incompatible(patch.diff.arg);
      return inclusion_diff(patch.left, patch.right, [&] {
        return subcase_list(module_type_symptom(false, expansion_token, env, {}, {}, *patch.diff.mismatch->symptom));
      });
  }
  return ignore_p();
}

Printer functor_app_diff_p(bool expansion_token, const includemod::InclusionEnv& env,
                           const NChange<includemod::AppArg>& patch) {
  using K = NChange<includemod::AppArg>::K;
  switch (patch.k) {
    case K::Insert: return inclusion_insert(patch.right);
    case K::Delete: return app_delete(patch.left);
    case K::Keep: return app_ok(patch.left, patch.right);
    case K::Change:
      if (patch.diff.kind == E::FunctorParamSymptom::Kind::Incompatible_params)
        return app_incompatible(patch.diff.arg_descr);
      return app_diff(patch.left, patch.right, [&] {
        return subcase_list(module_type_symptom(false, expansion_token, env, {}, {}, *patch.diff.mismatch->symptom));
      });
  }
  return ignore_p();
}

Msgs module_type_subst(const includemod::InclusionEnv& env, Ident::t id, const ModuleType* got,
                       const E::ModuleTypeDeclarationSymptom& sy) {
  using K = E::ModuleTypeDeclarationSymptom::Kind;
  Ctx c{Pos{Pos::K::Modtype, id}};
  switch (sy.kind) {
    case K::Not_less_than:
    case K::Not_greater_than: return module_type(true, true, env, {}, c, *sy.diff);
    case K::Incomparable: return module_type(true, true, env, {}, c, *sy.less_than);
    case K::Illegal_permutation: {
      const tt::ModuleCoercion* cc = sy.coercion;
      env::t e = env.i_env;
      Printer p = [got, cc, e](Formatter& ppf) { illegal_permutation(alt_pp, e, ppf, got, cc); };
      return {with_context(c, p)};
    }
  }
  return {};
}

Msgs all(const includemod::InclusionEnv& env, const E::All& a) {
  using K = E::All::Kind;
  switch (a.kind) {
    case K::In_Compilation_unit: {
      std::string got = a.got_name, expected = a.expected_name;
      Msg first = location::msg_noloc("%a", [&](Formatter& ppf) {
        fprintf(ppf, "The implementation %a@ does not match the interface %a:@ ", code_str(got), code_str(expected));
      });
      return signature(true, {first}, {}, *a.sig);
    }
    case K::In_Type_declaration: return {location::msg_noloc("%t", core(env.i_env, a.id, *a.core))};
    case K::In_Module_type: return module_type(true, false, env, {}, {}, *a.diff);
    case K::In_Module_type_substitution: return module_type_subst(env, a.id, a.mty1, *a.mtd_symptom);
    case K::In_Signature: return signature(true, {}, {}, *a.sig);
    case K::In_Expansion: {
      std::optional<Printer> m = core_module_type_symptom(a.expansion);
      if (!m) throw std::logic_error("Includemod_errorprinter.all");
      return {location::msg_noloc("%t", *m)};
    }
  }
  return {};
}

}  // namespace

void err_msgs(Formatter& ppf, const includemod::Explanation& e) {
  printtyp::wrap_printing_env(true, e.env, [&] {
    Printer p = coalesce(all(includemod::InclusionEnv{e.env, subst::identity()}, e.all));
    p(ppf);
  });
}

fd::Doc coercion_in_package_subtype(env::t env, const ModuleType* mty, const tt::ModuleCoercion* c) {
  return doc_printf("%t", [&](Formatter& ppf) { in_package_subtype(alt_pp, env, mty, c, ppf); });
}

Report report_error_doc(const includemod::Explanation& err) {
  fd::Doc txt = doc_printf("%a", [&](Formatter& f) { err_msgs(f, err); });
  return location::mkerror(location::in_file(location::input_name), {}, out_type::ident_conflicts::err_msg(), txt);
}

Report report_apply_error_doc(const Location& loc, env::t env, const includemod::ApplicationName& app_name,
                              const ModuleType* mty_f, const std::vector<includemod::AppArg>& args) {
  NPatch<includemod::AppArg> d =
      prepare_patch<includemod::AppArg>(true, Variant::App, includemod::functor_app_diff(env, mty_f, args));
  using K = NChange<includemod::AppArg>::K;
  auto footnote = [] { return out_type::ident_conflicts::err_msg(); };
  if (d.size() == 1 && d[0].second.k == K::Change) {
    const NChange<includemod::AppArg>& c = d[0].second;
    if (c.diff.kind == E::FunctorParamSymptom::Kind::Incompatible_params) {
      fd::Doc txt = doc_printf("%t", app_incompatible(c.diff.arg_descr));
      return location::mkerror(loc, {}, footnote(), txt);
    }
    includemod::InclusionEnv ie{env, subst::identity()};
    Printer p = app_single_diff(c.left, c.right, [&] {
      return subcase_list(module_type_symptom(false, true, ie, {}, {}, *c.diff.mismatch->symptom));
    });
    fd::Doc txt = doc_printf("%t", p);
    return location::mkerror(loc, {}, footnote(), txt);
  }
  bool not_functor = std::all_of(d.begin(), d.end(), [](const auto& x) { return x.second.k == K::Delete; });
  if (not_functor) {
    if (app_name.kind == includemod::ApplicationNameKind::Named_leftmost_functor)
      return location::errorf(loc, "@[The module %a is not a functor, it cannot be applied.@]",
                              misc::style::code(printtyp::longident, app_name.lid));
    return location::errorf(loc, "@[This module is not a functor, it cannot be applied.@]");
  }
  Printer intro = [app_name](Formatter& ppf) {
    switch (app_name.kind) {
      case includemod::ApplicationNameKind::Anonymous_functor:
        fprintf(ppf, "This functor application is ill-typed.");
        break;
      case includemod::ApplicationNameKind::Full_application_path:
        fprintf(ppf, "The functor application %a is ill-typed.", misc::style::code(printtyp::longident, app_name.lid));
        break;
      case includemod::ApplicationNameKind::Named_leftmost_functor:
        fprintf(ppf, "This application of the functor %a is ill-typed.",
                misc::style::code(printtyp::longident, app_name.lid));
        break;
    }
  };
  Printer actual = app_got(d);
  Printer expected = expected_p<includemod::AppArg>(d);
  Msgs sub = params_msgs<includemod::AppArg>(functor_app_diff_p, true, includemod::InclusionEnv{env, subst::identity()}, d);
  std::reverse(sub.begin(), sub.end());
  fd::Doc txt = doc_printf(
      "@[<hv>%t@ These arguments:@;<1 2>@[%t@]@ do not match these parameters:@;<1 2>@[%t@ -> ...@]@]", intro, actual,
      expected);
  return location::mkerror(loc, sub, footnote(), txt);
}

}  // namespace cppcaml::typing::includemod_errorprinter
