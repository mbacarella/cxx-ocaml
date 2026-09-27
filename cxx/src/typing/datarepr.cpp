// Ports of typing/data_types.ml and typing/datarepr.ml.  See datarepr.hpp.
#include "cppcaml/typing/datarepr.hpp"

#include <algorithm>

namespace cppcaml::typing {

using namespace types;
using namespace btype;

namespace data_types {

bool equal_tag(const ConstructorTag& a, const ConstructorTag& b) {
  using K = ConstructorTag::Kind;
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case K::Cstr_constant:
    case K::Cstr_block: return a.n == b.n;
    case K::Cstr_unboxed: return true;
    case K::Cstr_extension: return path::same(a.ext_path, b.ext_path);
  }
  return false;
}

bool equal_constr(const ConstructorDescription* a, const ConstructorDescription* b) {
  return equal_tag(a->cstr_tag, b->cstr_tag);
}

bool may_equal_constr(const ConstructorDescription* a, const ConstructorDescription* b) {
  if (a->cstr_arity != b->cstr_arity) return false;
  if (a->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension &&
      b->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension)
    return true;  // extension constructors may be rebindings of each other
  return equal_tag(a->cstr_tag, b->cstr_tag);
}

Path::t cstr_res_type_path(const ConstructorDescription* c) {
  auto* t = as<Tconstr>(get_desc(c->cstr_res));
  if (!t) throw std::logic_error("Data_types.cstr_res_type_path");
  return t->path;
}
Slice<TypeExpr*> cstr_res_type_params(const ConstructorDescription* c) {
  auto* t = as<Tconstr>(get_desc(c->cstr_res));
  if (!t) throw std::logic_error("Data_types.cstr_res_type_params");
  return t->args;
}
Path::t lbl_res_type_path(const LabelDescription* l) {
  auto* t = as<Tconstr>(get_desc(l->lbl_res));
  if (!t) throw std::logic_error("Data_types.lbl_res_type_path");
  return t->path;
}

}  // namespace data_types

namespace datarepr {

// Simplified version of Ctype.free_vars
std::vector<TypeExpr*> free_vars(TypeExpr* ty, bool param) {
  TypeSet ret;
  with_type_mark([&](TypeMark& mark) {
    std::function<void(TypeExpr*)> loop = [&](TypeExpr* t) {
      if (!try_mark_node(mark, t)) return;
      const TypeDesc* d = get_desc(t);
      if (d->kind == DescKind::Tvar) {
        ret.add(t);
      } else if (auto* v = as<Tvariant>(d)) {
        iter_row(loop, v->row);
        if (!static_row(v->row)) {
          if (get_desc(row_more(v->row))->kind == DescKind::Tvar && param) ret.add(t);
          else loop(row_more(v->row));
        }
      } else {
        iter_type_expr(loop, t);
      }
    };
    loop(ty);
  });
  return ret.elements();
}

static TypeExpr* newgenconstr(Path::t path, Slice<TypeExpr*> tyl) {
  return newgenty(tconstr(path, tyl, make<MemoRef>(mnil())));
}

static TypeExpr* tuple_of(const std::vector<TypeExpr*>& tyl) {
  std::vector<LabeledTy> l;
  for (TypeExpr* t : tyl) l.push_back({OptStr::none(), t});
  return newgenty(ttuple(slice(l)));
}

std::pair<std::vector<TypeExpr*>, std::vector<TypeExpr*>> constructor_existentials(
    const ConstructorArguments& cd_args, TypeExpr* cd_res) {
  std::vector<TypeExpr*> tyl;
  if (cd_args.kind == ConstructorArguments::Kind::Cstr_tuple)
    tyl.assign(cd_args.tuple.begin(), cd_args.tuple.end());
  else
    for (auto* l : cd_args.record) tyl.push_back(l->ld_type);
  std::vector<TypeExpr*> existentials;
  if (cd_res) {
    auto arg_vars = free_vars(tuple_of(tyl));
    auto res_vars = free_vars(cd_res);
    // TypeSet.elements (TypeSet.diff arg_vars res_vars): by id order
    for (TypeExpr* t : arg_vars)
      if (std::none_of(res_vars.begin(), res_vars.end(), [&](TypeExpr* r) { return r == t; }))
        existentials.push_back(t);
  }
  return {std::move(tyl), std::move(existentials)};
}

struct ConstrArgs {
  std::vector<TypeExpr*> existentials;
  Slice<TypeExpr*> args;  // a Cstr_tuple's own list
  const TypeDeclaration* inlined;
};

static ConstrArgs constructor_args(const UnitInfo* current_unit, PrivateFlag priv,
                                   const ConstructorArguments& cd_args, TypeExpr* cd_res,
                                   Path::t path, RecordRepresentation rep) {
  auto [tyl, existentials] = constructor_existentials(cd_args, cd_res);
  if (cd_args.kind == ConstructorArguments::Kind::Cstr_tuple)
    return {existentials, cd_args.tuple, nullptr};
  auto type_params = free_vars(tuple_of(tyl), /*param=*/true);
  long arity = static_cast<long>(type_params.size());
  auto* kind = make<TypeKind>();
  kind->kind = TypeKind::Kind::Type_record;
  kind->labels = cd_args.record;
  kind->record_repr = rep;
  // Separability.default_signature: Config.flat_float_array is true
  std::vector<Separability> sep(static_cast<std::size_t>(arity), Separability::Deepsep);
  auto* tdecl = make<TypeDeclaration>(
      slice(type_params), arity, kind, priv, nullptr,
      slice(variance::unknown_signature(true, arity)), slice(sep), false, lowest_level,
      location::none(), Attributes{}, TypeImmediacy::Unknown, false, uid::mk(current_unit));
  return {existentials, slice(std::vector<TypeExpr*>{newgenconstr(path, slice(type_params))}), tdecl};
}

static std::vector<std::pair<Ident::t, const ConstructorDescription*>> constructor_descrs(
    const UnitInfo* current_unit, Path::t ty_path, const TypeDeclaration* decl,
    Slice<const ConstructorDeclaration*> cstrs, VariantRepresentation rep) {
  TypeExpr* ty_res0 = newgenconstr(ty_path, decl->type_params);
  long num_consts = 0, num_nonconsts = 0;
  auto is_const = [](const ConstructorDeclaration* cd) {
    return cd->cd_args.kind == ConstructorArguments::Kind::Cstr_tuple && cd->cd_args.tuple.empty();
  };
  for (auto* cd : cstrs) (is_const(cd) ? num_consts : num_nonconsts)++;
  // describe_constructors recurses on the REST before building the head's
  // description (`let (tag, descr_rem) = ...` then constructor_args), so the
  // uids/ids of later constructors' inline records are allocated first.
  std::function<std::vector<std::pair<Ident::t, const ConstructorDescription*>>(
      std::size_t, long, long)>
      describe = [&](std::size_t k, long idx_const, long idx_nonconst) {
        std::vector<std::pair<Ident::t, const ConstructorDescription*>> out;
        if (k == cstrs.size()) return out;
        const ConstructorDeclaration* cd = cstrs[k];
        TypeExpr* ty_res = cd->cd_res ? cd->cd_res : ty_res0;
        ConstructorTag tag;
        std::vector<std::pair<Ident::t, const ConstructorDescription*>> descr_rem;
        if (rep == VariantRepresentation::Variant_unboxed) {
          tag.kind = ConstructorTag::Kind::Cstr_unboxed;
        } else if (is_const(cd)) {
          tag.kind = ConstructorTag::Kind::Cstr_constant;
          tag.n = idx_const;
          descr_rem = describe(k + 1, idx_const + 1, idx_nonconst);
        } else {
          tag.kind = ConstructorTag::Kind::Cstr_block;
          tag.n = idx_nonconst;
          descr_rem = describe(k + 1, idx_const, idx_nonconst + 1);
        }
        std::string_view cstr_name = ident::name(cd->cd_id);
        RecordRepresentation representation;
        if (rep == VariantRepresentation::Variant_unboxed) {
          representation.kind = RecordRepresentation::Kind::Record_unboxed;
          representation.unboxed_inlined = true;
        } else {
          representation.kind = RecordRepresentation::Kind::Record_inlined;
          representation.inlined_tag = idx_nonconst;
        }
        ConstrArgs ca = constructor_args(
            current_unit, decl->type_private, cd->cd_args, cd->cd_res,
            Path::pextra_ty(ty_path, Path::Extra::Pcstr_ty, cstr_name), representation);
        auto* cstr = make<ConstructorDescription>(
            cstr_name, ty_res, slice(ca.existentials), ca.args,
            static_cast<long>(ca.args.size()), tag, num_consts, num_nonconsts,
            cd->cd_res != nullptr, decl->type_private, cd->cd_loc, cd->cd_attributes,
            ca.inlined, cd->cd_uid);
        out.emplace_back(cd->cd_id, cstr);
        out.insert(out.end(), descr_rem.begin(), descr_rem.end());
        return out;
      };
  return describe(0, 0, 0);
}

const ConstructorDescription* extension_descr(const UnitInfo* current_unit, Path::t path_ext,
                                              const ExtensionConstructor* ext) {
  TypeExpr* ty_res = ext->ext_ret_type ? ext->ext_ret_type
                                       : newgenconstr(ext->ext_type_path, ext->ext_type_params);
  RecordRepresentation rep;
  rep.kind = RecordRepresentation::Kind::Record_extension;
  rep.extension = path_ext;
  ConstrArgs ca = constructor_args(current_unit, ext->ext_private, ext->ext_args,
                                   ext->ext_ret_type,
                                   Path::pextra_ty(path_ext, Path::Extra::Pext_ty), rep);
  ConstructorTag tag;
  tag.kind = ConstructorTag::Kind::Cstr_extension;
  tag.ext_path = path_ext;
  tag.ext_constant = ca.args.empty();
  return make<ConstructorDescription>(
      zborrow(path::last(path_ext)), ty_res, slice(ca.existentials), ca.args,
      static_cast<long>(ca.args.size()), tag, -1L, -1L, ext->ext_ret_type != nullptr,
      ext->ext_private, ext->ext_loc, ext->ext_attributes, ca.inlined, ext->ext_uid);
}

static std::vector<std::pair<Ident::t, const LabelDescription*>> label_descrs(
    TypeExpr* ty_res, Slice<const LabelDeclaration*> lbls, RecordRepresentation repres,
    PrivateFlag priv) {
  // all_labels: one array shared by every description, filled as they are made
  std::size_t n = lbls.size();
  auto** all = n ? static_cast<const LabelDescription**>(
                       zone().alloc(sizeof(LabelDescription*) * n, alignof(LabelDescription*)))
                 : nullptr;
  Slice<const LabelDescription*> all_slice{all, n};
  std::vector<std::pair<Ident::t, const LabelDescription*>> out;
  for (std::size_t num = 0; num < n; ++num) {
    const LabelDeclaration* l = lbls[num];
    auto* lbl = make<LabelDescription>(ident::name(l->ld_id), ty_res, l->ld_type, l->ld_mutable,
                                       l->ld_atomic, static_cast<long>(num), all_slice, repres,
                                       priv, l->ld_loc, l->ld_attributes, l->ld_uid);
    all[num] = lbl;
    out.emplace_back(l->ld_id, lbl);
  }
  return out;
}

const ConstructorDeclaration* find_constr_by_tag(
    const ConstructorTag& tag, Slice<const ConstructorDeclaration*> cstrlist) {
  long num_const = 0, num_nonconst = 0;
  for (auto* c : cstrlist) {
    bool is_const =
        c->cd_args.kind == ConstructorArguments::Kind::Cstr_tuple && c->cd_args.tuple.empty();
    if (is_const) {
      if (tag.kind == ConstructorTag::Kind::Cstr_constant && tag.n == num_const) return c;
      ++num_const;
    } else {
      if ((tag.kind == ConstructorTag::Kind::Cstr_block && tag.n == num_nonconst) ||
          tag.kind == ConstructorTag::Kind::Cstr_unboxed)
        return c;
      ++num_nonconst;
    }
  }
  throw ConstrNotFound{};
}

std::vector<std::pair<Ident::t, const ConstructorDescription*>> constructors_of_type(
    const UnitInfo* current_unit, Path::t ty_path, const TypeDeclaration* decl) {
  if (decl->type_kind->kind != TypeKind::Kind::Type_variant) return {};
  return constructor_descrs(current_unit, ty_path, decl, decl->type_kind->constructors,
                            decl->type_kind->variant_repr);
}

std::vector<std::pair<Ident::t, const LabelDescription*>> labels_of_type(
    Path::t ty_path, const TypeDeclaration* decl) {
  if (decl->type_kind->kind != TypeKind::Kind::Type_record) return {};
  return label_descrs(newgenconstr(ty_path, decl->type_params), decl->type_kind->labels,
                      decl->type_kind->record_repr, decl->type_private);
}

}  // namespace datarepr

}  // namespace cppcaml::typing
