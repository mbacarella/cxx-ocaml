(* Structural dumps of the typer's data -- the oracle for the typing/ port
   (TYPECHECKER.md).  Built against compiler-libs; the C++ side
   (c++typing-dump) prints the same formats from typing::, and the two must
   be byte-identical.

   Modes:
     typing_dump ctype STDLIB OPS  run Ctype operations (instance, expand,
                                 unify, moregen, equal, filter_arrow,
                                 subtype, matches, ...) on the types of
                                 named values/types (stage 3)
     typing_dump FILE.cmi        a .cmi's raw Types graph (stage 1)
     typing_dump gen CMI...      Env queries naming every item of the cmis
     typing_dump env QUERIES     run the queries through Env.find_*_by_name
                                 in Env.initial + open Stdlib (stage 2); type
                                 ids and ident stamps are renumbered by first
                                 visit, since their absolute values depend on
                                 process initialization

   It dumps the REPRESENTATION, not a view of it: no [repr] (that would
   path-compress), raw level/scope/id, Tlink/Texpand as stored, every
   mutable cell (commutable, field_kind, row-field ext, object name, abbrev
   memo, unscoped ident) numbered by first visit so sharing is visible.
   Where Types keeps a type abstract (row_desc, field_kind, commutable,
   Ident.t), the value is read with Obj -- its marshaled layout, which is
   exactly what the C++ decoder reads.  Parsetree payloads of attributes are
   dumped generically (block tags, ints, strings). *)

open Types

let b = Buffer.create 65536
let canonical = ref false
let s x = Buffer.add_string b x
let i n = s (string_of_int n)
let q x = s (Printf.sprintf "%S" x)

(* first-visit numbering by physical identity *)
module Phys = Hashtbl.Make (struct
  type t = Obj.t
  let equal = ( == )
  let hash = Hashtbl.hash
end)

let tbls : (string, int Phys.t * int ref) Hashtbl.t = Hashtbl.create 8
(* [visit kind v] = (number, first) *)
let visit kind (v : Obj.t) =
  let t, c =
    match Hashtbl.find_opt tbls kind with
    | Some x -> x
    | None ->
        let x = (Phys.create 64, ref 0) in
        Hashtbl.add tbls kind x;
        x
  in
  match Phys.find_opt t v with
  | Some n -> (n, false)
  | None ->
      let n = !c in
      incr c;
      Phys.add t v n;
      (n, true)

let ids : (int, int) Hashtbl.t = Hashtbl.create 64
let stamps : (int, int) Hashtbl.t = Hashtbl.create 64
let canon tbl n =
  if not !canonical then n
  else match Hashtbl.find_opt tbl n with
    | Some k -> k
    | None -> let k = Hashtbl.length tbl in Hashtbl.add tbl n k; k
let reset_numbering () =
  Hashtbl.reset tbls; Hashtbl.reset ids; Hashtbl.reset stamps

let opt f = function None -> s "None" | Some x -> s "(Some "; f x; s ")"
let list f l =
  s "[";
  List.iteri (fun k x -> if k > 0 then s "; "; f x) l;
  s "]"
let bool x = s (if x then "true" else "false")

let pos (p : Lexing.position) =
  s "{"; q p.pos_fname; s " "; i p.pos_lnum; s " "; i p.pos_bol; s " ";
  i p.pos_cnum; s "}"
let loc (l : Location.t) =
  s "<"; pos l.loc_start; s " "; pos l.loc_end;
  if l.loc_ghost then s " ghost"; s ">"

(* generic dump of an arbitrary marshaled value (attribute payloads) *)
let rec generic (v : Obj.t) =
  if Obj.is_int v then i (Obj.obj v : int)
  else
    let t = Obj.tag v in
    if t = Obj.string_tag then q (Obj.obj v : string)
    else if t = Obj.double_tag then s (Printf.sprintf "%h" (Obj.obj v : float))
    else begin
      let n, first = visit "G" v in
      if not first then (s "@G"; i n)
      else begin
        s "#G"; i n; s "{"; i t;
        for k = 0 to Obj.size v - 1 do s " "; generic (Obj.field v k) done;
        s "}"
      end
    end

let unscoped (u : Ident.Unscoped.t) =
  let n, first = visit "U" (Obj.repr u) in
  if not first then (s "@U"; i n)
  else begin
    (* { mutable state : Udesc of {name; stamp} | Ulink of t } *)
    s "#U"; i n; s "{";
    let st = Obj.field (Obj.repr u) 0 in
    (match Obj.tag st with
     | 0 ->
         let d = Obj.field st 0 in
         s "Udesc "; q (Obj.obj (Obj.field d 0)); s " ";
         i (canon stamps (Obj.obj (Obj.field d 1)))
     | _ -> s "Ulink "; generic (Obj.field st 0));
    s "}"
  end

let ident (id : Ident.t) =
  let v = Obj.repr id in
  let str k = (Obj.obj (Obj.field v k) : string)
  and int k = (Obj.obj (Obj.field v k) : int) in
  match Obj.tag v with
  | 0 -> s "Local("; q (str 0); s " "; i (canon stamps (int 1)); s ")"
  | 1 -> s "Scoped("; q (str 0); s " "; i (canon stamps (int 1)); s " "; i (int 2); s ")"
  | 2 -> s "Global("; q (Obj.obj (Obj.field v 0)); s ")"
  | 3 -> s "Predef("; q (str 0); s " "; i (int 1); s ")"
  | _ -> s "Unscoped("; unscoped (Obj.obj (Obj.field v 0)); s ")"

let rec path (p : Path.t) =
  match p with
  | Pident id -> s "Pident("; ident id; s ")"
  | Pdot (p, n) -> s "Pdot("; path p; s " "; q n; s ")"
  | Papply (p1, p2) -> s "Papply("; path p1; s " "; path p2; s ")"
  | Pextra_ty (p, e) ->
      s "Pextra_ty("; path p; s " ";
      (match e with
       | Pcstr_ty n -> s "Pcstr_ty "; q n
       | Pext_ty -> s "Pext_ty");
      s ")"

let arg_label = function
  | Asttypes.Nolabel -> s "Nolabel"
  | Labelled l -> s "Labelled "; q l
  | Optional l -> s "Optional "; q l

let uid (u : Shape.Uid.t) =
  match u with
  | Compilation_unit c -> s "Uid.Compilation_unit "; q c
  | Item { comp_unit; id; from } ->
      s "Uid.Item("; q comp_unit; s " "; i id; s " ";
      s (match from with Unit_info.Intf -> "Intf" | Impl -> "Impl"); s ")"
  | Local_opaque_item { comp_unit; id } ->
      s "Uid.Local_opaque_item("; q comp_unit; s " "; i id; s ")"
  | Internal -> s "Uid.Internal"
  | Predef p -> s "Uid.Predef "; q p

let attributes (l : Parsetree.attributes) =
  list (fun (a : Parsetree.attribute) ->
      s "@@"; q a.attr_name.txt; s " "; loc a.attr_name.loc; s " ";
      loc a.attr_loc; s " "; generic (Obj.repr a.attr_payload)) l

(* ---- type expressions ---- *)

let rec commu (c : Obj.t) =
  if Obj.is_int c then s (if (Obj.obj c : int) = 0 then "Cok" else "Cunknown")
  else begin
    let n, first = visit "C" c in
    if not first then (s "@C"; i n)
    else (s "#C"; i n; s "{Cvar "; commu (Obj.field c 0); s "}")
  end

and field_kind (k : Obj.t) =
  if Obj.is_int k then
    s (match (Obj.obj k : int) with
       | 0 -> "FKprivate" | 1 -> "FKpublic" | _ -> "FKabsent")
  else begin
    let n, first = visit "K" k in
    if not first then (s "@K"; i n)
    else (s "#K"; i n; s "{FKvar "; field_kind (Obj.field k 0); s "}")
  end

and name_ref (r : (Path.t * type_expr list) option ref) =
  let n, first = visit "N" (Obj.repr r) in
  if not first then (s "@N"; i n)
  else begin
    s "#N"; i n; s "{";
    opt (fun (p, tl) -> path p; s " "; list ty tl) !r;
    s "}"
  end

and memo (m : abbrev_memo) =
  match m with
  | Mnil -> s "Mnil"
  | Mcons { privacy; path = p; abbreviation; expansion; rem } ->
      s "Mcons("; s (if privacy = Private then "Private" else "Public");
      s " "; path p; s " "; ty abbreviation; s " "; ty expansion; s " ";
      memo rem; s ")"
  | Mlink r -> s "Mlink "; memo_ref r

and memo_ref (r : abbrev_memo ref) =
  let n, first = visit "M" (Obj.repr r) in
  if not first then (s "@M"; i n)
  else (s "#M"; i n; s "{"; memo !r; s "}")

and row_field (f : row_field) =
  match_row_field f
    ~present:(fun t -> s "RFpresent "; opt ty t)
    ~absent:(fun () -> s "RFabsent")
    ~either:(fun no_arg tl matched (ext, e) ->
        s "RFeither("; bool no_arg; s " "; list ty tl; s " "; bool matched;
        s " ";
        let n, first = visit "E" (Obj.repr ext) in
        if not first then (s "@E"; i n)
        else begin
          s "#E"; i n; s "{";
          (match e with None -> s "RFnone" | Some f -> row_field f);
          s "}"
        end;
        s ")")

and fixed (f : fixed_explanation) =
  match f with
  | Univar t -> s "Univar "; ty t
  | Fixed_private -> s "Fixed_private"
  | Reified p -> s "Reified "; path p
  | Rigid -> s "Rigid"

and row (r : row_desc) =
  (* { row_fields; row_more; row_closed; row_fixed; row_name } *)
  let v = Obj.repr r in
  let fields : (string * row_field) list = Obj.obj (Obj.field v 0) in
  s "{";
  list (fun (l, f) -> q l; s ":"; row_field f) fields;
  s " "; ty (Obj.obj (Obj.field v 1));
  s " "; bool (Obj.obj (Obj.field v 2));
  s " "; opt fixed (Obj.obj (Obj.field v 3));
  s " "; opt (fun (p, tl) -> path p; s " "; list ty tl) (Obj.obj (Obj.field v 4));
  s "}"

and package (p : package) =
  s "{"; path p.pack_path; s " ";
  list (fun (sl, t) -> list q sl; s " "; ty t) p.pack_constraints; s "}"

and ty (t : type_expr) =
  let r = Transient_expr.coerce t in
  let n, first = visit "T" (Obj.repr r) in
  if not first then (s "@T"; i n)
  else begin
    s "#T"; i n; s "{"; i r.level; s " "; i (Obj.obj (Obj.field (Obj.repr r) 2));
    s " "; i (canon ids r.id); s " ";
    (match r.desc with
     | Tvar nm -> s "Tvar "; opt q nm
     | Tarrow (l, a, b, c) ->
         s "Tarrow("; arg_label l; s " "; ty a; s " "; ty b; s " ";
         commu (Obj.repr c); s ")"
     | Ttuple l -> s "Ttuple "; list (fun (lb, t) -> opt q lb; s ":"; ty t) l
     | Tconstr (p, tl, m) ->
         s "Tconstr("; path p; s " "; list ty tl; s " "; memo_ref m; s ")"
     | Tobject (f, nm) -> s "Tobject("; ty f; s " "; name_ref nm; s ")"
     | Tfield (l, k, a, b) ->
         s "Tfield("; q l; s " "; field_kind (Obj.repr k); s " "; ty a; s " ";
         ty b; s ")"
     | Tnil -> s "Tnil"
     | Tvariant r -> s "Tvariant "; row r
     | Tunivar nm -> s "Tunivar "; opt q nm
     | Tpoly (a, tl) -> s "Tpoly("; ty a; s " "; list ty tl; s ")"
     | Tpackage p -> s "Tpackage "; package p
     | Tfunctor (l, u, p, a) ->
         s "Tfunctor("; arg_label l; s " "; unscoped u; s " "; package p;
         s " "; ty a; s ")"
     | Texpand (a, p, tl) ->
         s "Texpand("; ty a; s " "; path p; s " "; list ty tl; s ")"
     | Tlink a -> s "Tlink "; ty a
     | Tsubst (a, o) -> s "Tsubst("; ty a; s " "; opt ty o; s ")");
    s "}"
  end

(* ---- declarations ---- *)

let variance (v : Variance.t) = i (Obj.obj (Obj.repr v) : int)
let separability (x : Separability.t) =
  s (match x with Ind -> "Ind" | Sep -> "Sep" | Deepsep -> "Deepsep")
let private_flag = function
  | Asttypes.Private -> s "Private" | Public -> s "Public"
let mutable_flag = function
  | Asttypes.Immutable -> s "Immutable" | Mutable -> s "Mutable"
let virtual_flag = function
  | Asttypes.Virtual -> s "Virtual" | Concrete -> s "Concrete"

let native_repr = function
  | Primitive.Same_as_ocaml_repr -> s "Same_as_ocaml_repr"
  | Unboxed_float -> s "Unboxed_float"
  | Unboxed_integer bi ->
      s "Unboxed_integer ";
      s (match bi with Pnativeint -> "Pnativeint" | Pint32 -> "Pint32"
                     | Pint64 -> "Pint64")
  | Untagged_immediate -> s "Untagged_immediate"

let label_decl (l : label_declaration) =
  s "{"; ident l.ld_id; s " "; mutable_flag l.ld_mutable; s " ";
  s (match l.ld_atomic with Nonatomic -> "Nonatomic" | Atomic -> "Atomic");
  s " "; ty l.ld_type; s " "; loc l.ld_loc; s " "; attributes l.ld_attributes;
  s " "; uid l.ld_uid; s "}"

let cstr_args = function
  | Cstr_tuple tl -> s "Cstr_tuple "; list ty tl
  | Cstr_record ll -> s "Cstr_record "; list label_decl ll

let cstr_decl (c : constructor_declaration) =
  s "{"; ident c.cd_id; s " "; cstr_args c.cd_args; s " "; opt ty c.cd_res;
  s " "; loc c.cd_loc; s " "; attributes c.cd_attributes; s " "; uid c.cd_uid;
  s "}"

let type_decl (d : type_declaration) =
  s "{params="; list ty d.type_params; s " arity="; i d.type_arity;
  s " kind=";
  (match d.type_kind with
   | Type_abstract o ->
       s "Type_abstract ";
       (match o with
        | Definition -> s "Definition"
        | Rec_check_regularity -> s "Rec_check_regularity"
        | Approx_recmod -> s "Approx_recmod"
        | Existential n -> s "Existential "; q n
        | Equation (a, c) -> s "Equation("; ty a; s " "; ty c; s ")")
   | Type_record (ll, rep) ->
       s "Type_record("; list label_decl ll; s " ";
       (match rep with
        | Record_regular -> s "Record_regular"
        | Record_float -> s "Record_float"
        | Record_unboxed x -> s "Record_unboxed "; bool x
        | Record_inlined n -> s "Record_inlined "; i n
        | Record_extension p -> s "Record_extension "; path p);
       s ")"
   | Type_variant (cl, rep) ->
       s "Type_variant("; list cstr_decl cl; s " ";
       s (match rep with Variant_regular -> "Variant_regular"
                       | Variant_unboxed -> "Variant_unboxed");
       s ")"
   | Type_open -> s "Type_open"
   | Type_external n -> s "Type_external "; q n);
  s " private="; private_flag d.type_private;
  s " manifest="; opt ty d.type_manifest;
  s " variance="; list variance d.type_variance;
  s " separability="; list separability d.type_separability;
  s " newtype="; bool d.type_is_newtype;
  s " expansion_scope="; i d.type_expansion_scope;
  s " loc="; loc d.type_loc;
  s " attrs="; attributes d.type_attributes;
  s " immediate=";
  s (match d.type_immediate with
     | Unknown -> "Unknown" | Always -> "Always"
     | Always_on_64bits -> "Always_on_64bits");
  s " unboxed_default="; bool d.type_unboxed_default;
  s " uid="; uid d.type_uid; s "}"

let ext_constr (e : extension_constructor) =
  s "{"; path e.ext_type_path; s " "; list ty e.ext_type_params; s " ";
  cstr_args e.ext_args; s " "; opt ty e.ext_ret_type; s " ";
  private_flag e.ext_private; s " "; loc e.ext_loc; s " ";
  attributes e.ext_attributes; s " "; uid e.ext_uid; s "}"

let meths_map f (m : 'a Meths.t) =
  list (fun (k, v) -> q k; s ":"; f v) (Meths.bindings m)

let class_sig (c : class_signature) =
  s "{"; ty c.csig_self; s " "; ty c.csig_self_row; s " ";
  field_kind (Obj.repr c.csig_dummy_method); s " ";
  list (fun (k, (m, v, t)) -> q k; s ":"; mutable_flag m; s " ";
         virtual_flag v; s " "; ty t) (Vars.bindings c.csig_vars);
  s " ";
  meths_map (fun (p, v, t) ->
      (match p with
       | Mpublic -> s "Mpublic"
       | Mprivate k -> s "Mprivate "; field_kind (Obj.repr k));
      s " "; virtual_flag v; s " "; ty t) c.csig_meths;
  s "}"

let rec class_type = function
  | Cty_constr (p, tl, c) ->
      s "Cty_constr("; path p; s " "; list ty tl; s " "; class_type c; s ")"
  | Cty_signature c -> s "Cty_signature "; class_sig c
  | Cty_arrow (l, t, c) ->
      s "Cty_arrow("; arg_label l; s " "; ty t; s " "; class_type c; s ")"

let value_kind = function
  | Val_reg -> s "Val_reg"
  | Val_prim p ->
      s "Val_prim{"; q p.prim_name; s " "; i p.prim_arity; s " ";
      bool p.prim_alloc; s " "; q p.prim_native_name; s " ";
      list native_repr p.prim_native_repr_args; s " ";
      native_repr p.prim_native_repr_res; s "}"
  | Val_ivar (m, n) -> s "Val_ivar("; mutable_flag m; s " "; q n; s ")"
  | Val_self _ -> s "Val_self"
  | Val_anc _ -> s "Val_anc"

let rec_status = function
  | Trec_not -> s "Trec_not" | Trec_first -> s "Trec_first"
  | Trec_next -> s "Trec_next"
let visibility = function Exported -> s "Exported" | Hidden -> s "Hidden"

let rec modtype depth = function
  | Mty_ident p -> s "Mty_ident "; path p
  | Mty_signature sg -> s "Mty_signature"; signature (depth + 1) sg
  | Mty_functor (Unit, m) -> s "Mty_functor(Unit "; modtype depth m; s ")"
  | Mty_functor (Named (id, a), m) ->
      s "Mty_functor(Named("; opt ident id; s " "; modtype depth a; s ") ";
      modtype depth m; s ")"
  | Mty_alias p -> s "Mty_alias "; path p

and signature depth sg =
  s "[";
  List.iter (fun it ->
      s "\n"; s (String.make (2 * depth) ' ');
      item depth it) sg;
  s "]"

and item depth = function
  | Sig_value (id, vd, v) ->
      s "Sig_value "; ident id; s " {"; ty vd.val_type; s " ";
      value_kind vd.val_kind; s " "; loc vd.val_loc; s " ";
      attributes vd.val_attributes; s " "; uid vd.val_uid; s "} ";
      visibility v
  | Sig_type (id, td, r, v) ->
      s "Sig_type "; ident id; s " "; type_decl td; s " "; rec_status r;
      s " "; visibility v
  | Sig_typext (id, e, st, v) ->
      s "Sig_typext "; ident id; s " "; ext_constr e; s " ";
      s (match st with Text_first -> "Text_first" | Text_next -> "Text_next"
                     | Text_exception -> "Text_exception");
      s " "; visibility v
  | Sig_module (id, pr, md, r, v) ->
      s "Sig_module "; ident id; s " ";
      s (match pr with Mp_present -> "Mp_present" | Mp_absent -> "Mp_absent");
      s " {"; modtype depth md.md_type; s " "; attributes md.md_attributes;
      s " "; loc md.md_loc; s " "; uid md.md_uid; s "} "; rec_status r; s " ";
      visibility v
  | Sig_modtype (id, mtd, v) ->
      s "Sig_modtype "; ident id; s " {"; opt (modtype depth) mtd.mtd_type;
      s " "; attributes mtd.mtd_attributes; s " "; loc mtd.mtd_loc; s " ";
      uid mtd.mtd_uid; s "} "; visibility v
  | Sig_class (id, cd, r, v) ->
      s "Sig_class "; ident id; s " {"; list ty cd.cty_params; s " ";
      class_type cd.cty_type; s " "; path cd.cty_path; s " ";
      opt ty cd.cty_new; s " "; list variance cd.cty_variance; s " ";
      loc cd.cty_loc; s " "; attributes cd.cty_attributes; s " ";
      uid cd.cty_uid; s "} "; rec_status r; s " "; visibility v
  | Sig_class_type (id, ct, r, v) ->
      s "Sig_class_type "; ident id; s " {"; list ty ct.clty_params; s " ";
      class_type ct.clty_type; s " "; path ct.clty_path; s " ";
      type_decl ct.clty_hash_type; s " "; list variance ct.clty_variance;
      s " "; loc ct.clty_loc; s " "; attributes ct.clty_attributes; s " ";
      uid ct.clty_uid; s "} "; rec_status r; s " "; visibility v

let dump_cmi file =
  let cmi = Cmi_format.read_cmi file in
  s "name "; q cmi.cmi_name; s "\nsig"; signature 1 cmi.cmi_sign;
  s "\ncrcs ";
  list (fun (n, d) -> q n; s " "; opt (fun d -> s (Digest.to_hex d)) d)
    cmi.cmi_crcs;
  s "\nflags ";
  list (function
      | Cmi_format.Rectypes -> s "Rectypes"
      | Opaque -> s "Opaque"
      | Alerts m ->
          s "Alerts ";
          list (fun (k, v) -> q k; s "="; q v) (Misc.Stdlib.String.Map.bindings m))
    cmi.cmi_flags;
  s "\n"

(* ---- stage 2: Env queries ---- *)

let short_name m =
  let p = "Stdlib__" in
  let lp = String.length p in
  if String.length m > lp && String.sub m 0 lp = p
  then Some (String.sub m lp (String.length m - lp)) else None

let gen files =
  let emit kind name = print_string (kind ^ " " ^ name ^ "\n") in
  let rec items prefixes depth sg =
    List.iter (fun it ->
        let each kind name = List.iter (fun p -> emit kind (p ^ "." ^ name)) prefixes in
        match it with
        | Sig_value (id, _, _) -> each "value" (Ident.name id)
        | Sig_type (id, td, _, _) ->
            each "type" (Ident.name id);
            (match td.type_kind with
             | Type_variant (cds, _) ->
                 List.iter (fun cd -> each "constr" (Ident.name cd.cd_id)) cds
             | Type_record (lds, _) ->
                 List.iter (fun ld -> each "label" (Ident.name ld.ld_id)) lds
             | _ -> ())
        | Sig_typext (id, _, _, _) -> each "constr" (Ident.name id)
        | Sig_module (id, _, md, _, _) ->
            each "module" (Ident.name id);
            (match md.md_type with
             | Mty_signature sg when depth < 2 ->
                 items (List.map (fun p -> p ^ "." ^ Ident.name id) prefixes)
                   (depth + 1) sg
             | _ -> ())
        | Sig_modtype (id, _, _) -> each "modtype" (Ident.name id)
        | Sig_class (id, _, _, _) -> each "class" (Ident.name id)
        | Sig_class_type (id, _, _, _) -> each "cltype" (Ident.name id))
      sg
  in
  List.iter (fun f ->
      let cmi = Cmi_format.read_cmi f in
      let m = cmi.cmi_name in
      let prefixes = match short_name m with Some n -> [m; n] | None -> [m] in
      items prefixes 0 cmi.cmi_sign)
    files

(* "A.B.c", with functor applications "F(X).t" (arguments are dotted paths,
   possibly applications themselves) *)
let lid_of_string str =
  let n = String.length str in
  let pos = ref 0 in
  let ident () =
    let st = !pos in
    while !pos < n && not (List.mem str.[!pos] ['.'; '('; ')']) do incr pos done;
    String.sub str st (!pos - st)
  in
  let rec path () =
    let l = ref (Longident.Lident (ident ())) in
    let continue = ref true in
    while !continue && !pos < n do
      match str.[!pos] with
      | '(' ->
          incr pos;
          let a = path () in
          incr pos;  (* ')' *)
          l := Longident.Lapply (Location.mknoloc !l, Location.mknoloc a)
      | '.' ->
          incr pos;
          l := Longident.Ldot (Location.mknoloc !l, Location.mknoloc (ident ()))
      | _ -> continue := false
    done;
    !l
  in
  path ()

let cstr_tag = function
  | Data_types.Cstr_constant n -> s "Cstr_constant "; i n
  | Cstr_block n -> s "Cstr_block "; i n
  | Cstr_unboxed -> s "Cstr_unboxed"
  | Cstr_extension (p, c) -> s "Cstr_extension("; path p; s " "; bool c; s ")"

let cstr_descr (c : Data_types.constructor_description) =
  s "{"; q c.cstr_name; s " "; ty c.cstr_res; s " "; list ty c.cstr_existentials;
  s " "; list ty c.cstr_args; s " "; i c.cstr_arity; s " "; cstr_tag c.cstr_tag;
  s " "; i c.cstr_consts; s " "; i c.cstr_nonconsts; s " "; bool c.cstr_generalized;
  s " "; private_flag c.cstr_private; s " "; loc c.cstr_loc; s " ";
  attributes c.cstr_attributes; s " "; opt type_decl c.cstr_inlined; s " ";
  uid c.cstr_uid; s "}"

let lbl_descr (l : Data_types.label_description) =
  s "{"; q l.lbl_name; s " "; ty l.lbl_res; s " "; ty l.lbl_arg; s " ";
  mutable_flag l.lbl_mut; s " ";
  s (match l.lbl_atomic with Nonatomic -> "Nonatomic" | Atomic -> "Atomic");
  s " "; i l.lbl_pos; s " "; i (Array.length l.lbl_all); s " ";
  (match l.lbl_repres with
   | Record_regular -> s "Record_regular"
   | Record_float -> s "Record_float"
   | Record_unboxed x -> s "Record_unboxed "; bool x
   | Record_inlined n -> s "Record_inlined "; i n
   | Record_extension p -> s "Record_extension "; path p);
  s " "; private_flag l.lbl_private; s " "; loc l.lbl_loc; s " ";
  attributes l.lbl_attributes; s " "; uid l.lbl_uid; s "}"

let run_queries file env =
  let ic = open_in file in
  (try
     while true do
       let line = input_line ic in
       match String.index_opt line ' ' with
       | None -> ()
       | Some k ->
           let kind = String.sub line 0 k in
           let name = String.sub line (k + 1) (String.length line - k - 1) in
           let lid = lid_of_string name in
           reset_numbering ();
           s line; s " => ";
           (try
              match kind with
              | "value" ->
                  let p, vd = Env.find_value_by_name lid env in
                  path p; s " "; ty vd.val_type; s " "; value_kind vd.val_kind
              | "type" ->
                  let p, td = Env.find_type_by_name lid env in
                  path p; s " "; type_decl td
              | "constr" -> cstr_descr (Env.find_constructor_by_name lid env)
              | "label" -> lbl_descr (Env.find_label_by_name lid env)
              | "module" ->
                  let p, md = Env.find_module_by_name lid env in
                  path p; s " "; modtype 1 md.md_type
              | "modtype" ->
                  let p, mtd = Env.find_modtype_by_name lid env in
                  path p; s " "; opt (modtype 1) mtd.mtd_type
              | "class" ->
                  let p, cd = Env.find_class_by_name lid env in
                  path p; s " "; class_type cd.cty_type
              | "cltype" ->
                  let p, ct = Env.find_cltype_by_name lid env in
                  path p; s " "; class_type ct.clty_type
              | _ -> s "BADKIND"
            with Not_found -> s "NOTFOUND");
           s "\n"
     done
   with End_of_file -> ());
  close_in ic

(* ---- stage 3: Ctype operations ---- *)

let elt_name (e : _ Errortrace.elt) =
  match e with
  | Diff _ -> "Diff" | Variant _ -> "Variant" | Obj _ -> "Obj"
  | Escape _ -> "Escape" | Function_label_mismatch _ -> "Function_label_mismatch"
  | Tuple_label_mismatch _ -> "Tuple_label_mismatch"
  | Incompatible_fields _ -> "Incompatible_fields"
  | First_class_module _ -> "First_class_module" | Univar _ -> "Univar"
  | Rec_occur _ -> "Rec_occur"

let expanded (e : Errortrace.expanded_type) = s "<"; ty e.ty; s " "; ty e.expanded; s ">"

let err_trace (tr : (Errortrace.expanded_type, _) Errortrace.elt list) =
  list (fun (e : (Errortrace.expanded_type, _) Errortrace.elt) ->
      s (elt_name e);
      match e with
      | Diff { got; expected } -> s "("; expanded got; s " "; expanded expected; s ")"
      | Incompatible_fields { name; diff = { got; expected } } ->
          s "("; q name; s " "; ty got; s " "; ty expected; s ")"
      | _ -> ()) tr

let run_ctype file env =
  let value name =
    let _, vd = Env.find_value_by_name (lid_of_string name) env in
    vd.val_type
  in
  let ic = open_in file in
  (try
     while true do
       let line = input_line ic in
       match String.split_on_char ' ' line with
       | op :: args ->
           reset_numbering ();
           s line; s " => ";
           (try
              match op, args with
              | "inst", [v] -> ty (Ctype.instance (value v))
              | "gen", [v] ->
                  let vt = value v in
                  ty (Ctype.with_local_level_generalize (fun () -> Ctype.instance vt))
              | "expand", [t] ->
                  let p, td = Env.find_type_by_name (lid_of_string t) env in
                  let t = Ctype.newconstr p (List.map (fun _ -> Ctype.newvar ()) td.type_params) in
                  let e = Ctype.expand_head env t in
                  ty e; s " | "; ty (Ctype.full_expand ~may_forget_scope:false env t)
              | "unify", [v1; v2] ->
                  let t1 = Ctype.instance (value v1) in
                  let t2 = Ctype.instance (value v2) in
                  (match Ctype.unify env t1 t2 with
                   | () -> s "OK "; ty t1
                   | exception Ctype.Unify { trace } -> s "ERR "; err_trace trace)
              | "moregen", [v1; v2] ->
                  (match Ctype.moregeneral env (value v1) (value v2) with
                   | () -> s "OK"
                   | exception Ctype.Moregen { trace } -> s "ERR "; err_trace trace)
              | "equal", [v1; v2] ->
                  (match Ctype.equal env true [value v1] [value v2] with
                   | () -> s "OK"
                   | exception Ctype.Equality { trace; subst } ->
                       s "ERR "; err_trace trace; s " ";
                       list (fun (a, b) -> ty a; s "="; ty b) subst)
              | "arrow", [v] ->
                  (match Ctype.filter_arrow env ~in_apply:false (Ctype.instance (value v))
                           Nolabel ~param_hole:false with
                   | Ok { ty_param; ty_ret } -> s "OK "; ty ty_param; s " "; ty ty_ret
                   | Error (Unification_error { trace }) -> s "ERR "; err_trace trace
                   | Error (Label_mismatch { got; expected; expected_type }) ->
                       s "Label_mismatch "; arg_label got; s " "; arg_label expected; s " ";
                       ty expected_type
                   | Error Not_a_function -> s "Not_a_function")
              | "subtype", [v1; v2] ->
                  let t1 = Ctype.instance (value v1) in
                  let t2 = Ctype.instance (value v2) in
                  (match (Ctype.subtype env t1 t2) () with
                   | () -> s "OK "; ty t1; s " "; ty t2
                   | exception Ctype.Subtype { trace; unification_trace } ->
                       s "ERR ";
                       list (fun (Errortrace.Subtype.Diff { got; expected }) ->
                           expanded got; s " "; expanded expected) trace;
                       s " "; err_trace unification_trace)
              | "match", [v1; v2] ->
                  let t1 = Ctype.instance (value v1) in
                  let t2 = Ctype.instance (value v2) in
                  (match Ctype.matches ~expand_error_trace:true env t1 t2 with
                   | () -> s "OK"
                   | exception Ctype.Matches_failure (_, { trace }) ->
                       s "ERR "; err_trace trace)
              | "labels", [v] ->
                  let labels, ~is_ret_tvar = Ctype.arrow_labels env (value v) in
                  list arg_label labels; s " "; bool is_ret_tvar
              | "nongen", [v] ->
                  (match Ctype.nongen_vars_in_schema env (Ctype.instance (value v)) with
                   | None -> s "None"
                   | Some set -> s "Some "; list ty (Btype.TypeSet.elements set))
              | "enlarge", [v] ->
                  let t, warn = Ctype.enlarge_type env (Ctype.instance (value v)) in
                  ty t; s " "; bool warn
              | _ -> s "BADOP"
            with Not_found -> s "NOTFOUND");
           s "\n"
       | [] -> ()
     done
   with End_of_file -> ());
  close_in ic

let () =
  match Array.to_list Sys.argv with
  | _ :: "gen" :: files -> gen files
  | _ :: "env" :: stdlib_dir :: queries :: _ ->
      canonical := true;
      Load_path.init ~auto_include:Load_path.no_auto_include
        ~visible:(String.split_on_char ':' stdlib_dir) ~hidden:[];
      let env =
        match Env.open_pers_signature "Stdlib" Env.initial with
        | Ok env -> env
        | Error _ -> failwith "open Stdlib"
      in
      run_queries queries env;
      print_string (Buffer.contents b)
  | _ :: "ctype" :: stdlib_dir :: queries :: _ ->
      canonical := true;
      Load_path.init ~auto_include:Load_path.no_auto_include
        ~visible:(String.split_on_char ':' stdlib_dir) ~hidden:[];
      let env =
        match Env.open_pers_signature "Stdlib" Env.initial with
        | Ok env -> env
        | Error _ -> failwith "open Stdlib"
      in
      run_ctype queries env;
      print_string (Buffer.contents b)
  | _ :: file :: _ -> dump_cmi file; print_string (Buffer.contents b)
  | _ -> prerr_endline "usage: typing_dump FILE.cmi | gen CMI... | env STDLIB QUERIES"
