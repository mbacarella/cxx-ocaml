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

(* ---- stage 4a: Parsetree ---- *)

(* Locations the C++ parser does not record yet are printed through [gloc],
   masked as "?" (TYPECHECKER.md lists them). *)
let mask_gaps = ref true
let parse_file = ref ""

module P = struct
  open Parsetree
  (* compact: <lnum,bol,cnum-lnum,bol,cnum[ g]>, the file name when it is
     not the unit's own *)
  let pos (p : Lexing.position) =
    if p.pos_fname <> !parse_file then (q p.pos_fname; s ":");
    i p.pos_lnum; s ","; i p.pos_bol; s ","; i p.pos_cnum
  let loc (l : Location.t) =
    s "<"; pos l.loc_start; s "-"; pos l.loc_end; if l.loc_ghost then s " g"; s ">"
  let gloc l = if !mask_gaps then s "?" else loc l
  let str_loc (x : string Asttypes.loc) = q x.txt; s " "; loc x.loc
  let str_gloc (x : string Asttypes.loc) = q x.txt; s " "; gloc x.loc
  let rec lid (l : Longident.t) =
    match l with
    | Lident n -> s "Lident "; q n
    | Ldot (a, b) -> s "Ldot("; lid a.txt; s " "; loc a.loc; s " "; q b.txt; s " "; loc b.loc; s ")"
    | Lapply (a, b) -> s "Lapply("; lid a.txt; s " "; loc a.loc; s " "; lid b.txt; s " "; loc b.loc; s ")"
  let lid_loc (x : Longident.t Asttypes.loc) = s "{"; lid x.txt; s " "; loc x.loc; s "}"
  let optstr = opt q
  let flag_closed = function Asttypes.Closed -> s "Closed" | Open -> s "Open"
  let flag_rec = function Asttypes.Nonrecursive -> s "Nonrec" | Recursive -> s "Rec"
  let flag_mut = function Asttypes.Immutable -> s "Immutable" | Mutable -> s "Mutable"
  let flag_priv = function Asttypes.Private -> s "Private" | Public -> s "Public"
  let flag_virt = function Asttypes.Virtual -> s "Virtual" | Concrete -> s "Concrete"
  let flag_ovr = function Asttypes.Override -> s "Override" | Fresh -> s "Fresh"
  let char_opt = function None -> s "None" | Some c -> s "Some "; i (Char.code c)

  let constant (c : constant) =
    s "{";
    (match c.pconst_desc with
     | Pconst_integer (t, suf) -> s "Pconst_integer "; q t; s " "; char_opt suf
     | Pconst_char c -> s "Pconst_char "; i (Char.code c)
     | Pconst_string (t, l, d) -> s "Pconst_string "; q t; s " "; loc l; s " "; optstr d
     | Pconst_float (t, suf) -> s "Pconst_float "; q t; s " "; char_opt suf);
    s " "; loc c.pconst_loc; s "}"

  let rec attribute ?(gap_name = false) (a : attribute) =
    s "{attr "; ignore gap_name; str_loc a.attr_name;
    s " "; payload a.attr_payload; s " "; loc a.attr_loc; s "}"
  and attrs l = list (fun a -> attribute a) l
  and payload = function
    | PStr st -> s "PStr "; structure st
    | PSig sg -> s "PSig "; signature sg
    | PTyp t -> s "PTyp "; core_type t
    | PPat (p, g) -> s "PPat "; pattern p; s " "; opt expression g
  and extension ((n, p) : extension) = s "{ext "; str_loc n; s " "; payload p; s "}"

  and package_type ~gap (p : package_type) =
    s "{pack "; lid_loc p.ppt_path; s " ";
    list (fun (l, t) -> lid_loc l; s "="; core_type t) p.ppt_constraints;
    s " "; if gap then gloc p.ppt_loc else loc p.ppt_loc; s " "; attrs p.ppt_attrs; s "}"

  and core_type (t : core_type) =
    s "(T "; loc t.ptyp_loc; s " "; attrs t.ptyp_attributes; s " ";
    (match t.ptyp_desc with
     | Ptyp_any -> s "Ptyp_any"
     | Ptyp_var v -> s "Ptyp_var "; q v
     | Ptyp_arrow (l, a, b) -> s "Ptyp_arrow "; arg_label l; s " "; core_type a; s " "; core_type b
     | Ptyp_tuple tl -> s "Ptyp_tuple "; list (fun (l, t) -> optstr l; s ":"; core_type t) tl
     | Ptyp_constr (l, tl) -> s "Ptyp_constr "; lid_loc l; s " "; list core_type tl
     | Ptyp_object (fl, c) ->
         s "Ptyp_object ";
         list (fun (f : object_field) ->
             s "{"; (match f.pof_desc with
              | Otag (l, t) -> s "Otag "; str_loc l; s " "; core_type t
              | Oinherit t -> s "Oinherit "; core_type t);
             s " "; loc f.pof_loc; s " "; attrs f.pof_attributes; s "}") fl;
         s " "; flag_closed c
     | Ptyp_class (l, tl) -> s "Ptyp_class "; lid_loc l; s " "; list core_type tl
     | Ptyp_alias (t, n) -> s "Ptyp_alias "; core_type t; s " "; str_loc n
     | Ptyp_variant (fl, c, ls) ->
         s "Ptyp_variant ";
         list (fun (f : row_field) ->
             s "{"; (match f.prf_desc with
              | Rtag (l, b, tl) -> s "Rtag "; str_loc l; s " "; bool b; s " "; list core_type tl
              | Rinherit t -> s "Rinherit "; core_type t);
             s " "; loc f.prf_loc; s " "; attrs f.prf_attributes; s "}") fl;
         s " "; flag_closed c; s " "; opt (list q) ls
     | Ptyp_poly (vs, t) -> s "Ptyp_poly "; list str_gloc vs; s " "; core_type t
     | Ptyp_package p -> s "Ptyp_package "; package_type ~gap:false p
     | Ptyp_open (l, t) -> s "Ptyp_open "; lid_loc l; s " "; core_type t
     | Ptyp_extension e -> s "Ptyp_extension "; extension e
     | Ptyp_functor (l, n, p, t) ->
         s "Ptyp_functor "; arg_label l; s " "; str_loc n; s " "; package_type ~gap:true p;
         s " "; core_type t);
    s ")"

  and pattern (p : pattern) =
    s "(P "; loc p.ppat_loc; s " "; attrs p.ppat_attributes; s " ";
    (match p.ppat_desc with
     | Ppat_any -> s "Ppat_any"
     | Ppat_var n -> s "Ppat_var "; str_loc n
     | Ppat_alias (p, n) -> s "Ppat_alias "; pattern p; s " "; str_loc n
     | Ppat_constant c -> s "Ppat_constant "; constant c
     | Ppat_interval (a, b) -> s "Ppat_interval "; constant a; s " "; constant b
     | Ppat_tuple (pl, c) ->
         s "Ppat_tuple "; list (fun (l, p) -> optstr l; s ":"; pattern p) pl; s " "; flag_closed c
     | Ppat_construct (l, a) ->
         s "Ppat_construct "; lid_loc l; s " ";
         opt (fun (vs, p) -> list str_loc vs; s " "; pattern p) a
     | Ppat_variant (l, a) -> s "Ppat_variant "; q l; s " "; opt pattern a
     | Ppat_record (fl, c) ->
         s "Ppat_record "; list (fun (l, p) -> lid_loc l; s "="; pattern p) fl; s " "; flag_closed c
     | Ppat_array pl -> s "Ppat_array "; list pattern pl
     | Ppat_or (a, b) -> s "Ppat_or "; pattern a; s " "; pattern b
     | Ppat_constraint (p, t) -> s "Ppat_constraint "; pattern p; s " "; core_type t
     | Ppat_type l -> s "Ppat_type "; lid_loc l
     | Ppat_lazy p -> s "Ppat_lazy "; pattern p
     | Ppat_unpack (n, pk) ->
         s "Ppat_unpack "; opt q n.txt; s " "; loc n.loc; s " "; opt (package_type ~gap:true) pk
     | Ppat_exception p -> s "Ppat_exception "; pattern p
     | Ppat_effect (a, b) -> s "Ppat_effect "; pattern a; s " "; pattern b
     | Ppat_extension e -> s "Ppat_extension "; extension e
     | Ppat_open (l, p) -> s "Ppat_open "; lid_loc l; s " "; pattern p);
    s ")"

  and case (c : case) =
    s "{case "; pattern c.pc_lhs; s " "; opt expression c.pc_guard; s " "; expression c.pc_rhs; s "}"
  and binding_op (b : binding_op) =
    s "{bop "; str_loc b.pbop_op; s " "; pattern b.pbop_pat; s " "; expression b.pbop_exp;
    s " "; loc b.pbop_loc; s "}"
  and args l = list (fun (lb, e) -> arg_label lb; s ":"; expression e) l

  and expression (e : expression) =
    s "(E "; loc e.pexp_loc; s " "; attrs e.pexp_attributes; s " ";
    (match e.pexp_desc with
     | Pexp_ident l -> s "Pexp_ident "; lid_loc l
     | Pexp_constant c -> s "Pexp_constant "; constant c
     | Pexp_let (r, vbs, b) ->
         s "Pexp_let "; flag_rec r; s " "; list value_binding vbs; s " "; expression b
     | Pexp_function (ps, tc, body) ->
         s "Pexp_function ";
         list (fun (p : function_param) ->
             s "{"; loc p.pparam_loc; s " ";
             (match p.pparam_desc with
              | Pparam_val (l, d, p) ->
                  s "Pparam_val "; arg_label l; s " "; opt expression d; s " "; pattern p
              | Pparam_newtype n -> s "Pparam_newtype "; str_loc n);
             s "}") ps;
         s " ";
         opt (function
             | Pconstraint t -> s "Pconstraint "; core_type t
             | Pcoerce (f, t) -> s "Pcoerce "; opt core_type f; s " "; core_type t) tc;
         s " ";
         (match body with
          | Pfunction_body e -> s "Pfunction_body "; expression e
          | Pfunction_cases (cs, l, a) ->
              s "Pfunction_cases "; list case cs; s " "; loc l; s " "; attrs a)
     | Pexp_apply (f, a) -> s "Pexp_apply "; expression f; s " "; args a
     | Pexp_match (e, cs) -> s "Pexp_match "; expression e; s " "; list case cs
     | Pexp_try (e, cs) -> s "Pexp_try "; expression e; s " "; list case cs
     | Pexp_tuple el -> s "Pexp_tuple "; list (fun (l, e) -> optstr l; s ":"; expression e) el
     | Pexp_construct (l, a) -> s "Pexp_construct "; lid_loc l; s " "; opt expression a
     | Pexp_variant (l, a) -> s "Pexp_variant "; q l; s " "; opt expression a
     | Pexp_record (fl, b) ->
         s "Pexp_record "; list (fun (l, e) -> lid_loc l; s "="; expression e) fl;
         s " "; opt expression b
     | Pexp_field (e, l) -> s "Pexp_field "; expression e; s " "; lid_loc l
     | Pexp_setfield (a, l, b) ->
         s "Pexp_setfield "; expression a; s " "; lid_loc l; s " "; expression b
     | Pexp_array el -> s "Pexp_array "; list expression el
     | Pexp_ifthenelse (a, b, c) ->
         s "Pexp_ifthenelse "; expression a; s " "; expression b; s " "; opt expression c
     | Pexp_sequence (a, b) -> s "Pexp_sequence "; expression a; s " "; expression b
     | Pexp_while (a, b) -> s "Pexp_while "; expression a; s " "; expression b
     | Pexp_for (p, a, b, d, c) ->
         s "Pexp_for "; pattern p; s " "; expression a; s " "; expression b; s " ";
         s (match d with Upto -> "Upto" | Downto -> "Downto"); s " "; expression c
     | Pexp_constraint (e, t) -> s "Pexp_constraint "; expression e; s " "; core_type t
     | Pexp_coerce (e, f, t) ->
         s "Pexp_coerce "; expression e; s " "; opt core_type f; s " "; core_type t
     | Pexp_send (e, m) -> s "Pexp_send "; expression e; s " "; str_loc m
     | Pexp_new l -> s "Pexp_new "; lid_loc l
     | Pexp_setinstvar (n, e) -> s "Pexp_setinstvar "; str_loc n; s " "; expression e
     | Pexp_override fl ->
         s "Pexp_override "; list (fun (n, e) -> str_loc n; s "="; expression e) fl
     | Pexp_struct_item (it, e) -> s "Pexp_struct_item "; structure_item it; s " "; expression e
     | Pexp_assert e -> s "Pexp_assert "; expression e
     | Pexp_lazy e -> s "Pexp_lazy "; expression e
     | Pexp_poly (e, t) -> s "Pexp_poly "; expression e; s " "; opt core_type t
     | Pexp_object cs -> s "Pexp_object "; class_structure cs
     | Pexp_newtype (n, e) -> s "Pexp_newtype "; str_loc n; s " "; expression e
     | Pexp_pack (m, p) -> s "Pexp_pack "; module_expr m; s " "; opt (package_type ~gap:true) p
     | Pexp_letop { let_; ands; body } ->
         s "Pexp_letop "; binding_op let_; s " "; list binding_op ands; s " "; expression body
     | Pexp_extension e -> s "Pexp_extension "; extension e
     | Pexp_unreachable -> s "Pexp_unreachable");
    (match e.pexp_desc with
     | Pexp_assert _ ->
         (* all Typecore reads of pexp_loc_stack: its innermost location *)
         let rec innermost = function [] -> e.pexp_loc | [l] -> l | _ :: l -> innermost l in
         s " innermost "; loc (innermost e.pexp_loc_stack)
     | _ -> ());
    s ")"

  and value_constraint = function
    | Pvc_constraint { locally_abstract_univars; typ } ->
        s "Pvc_constraint "; list str_loc locally_abstract_univars; s " "; core_type typ
    | Pvc_coercion { ground; coercion } ->
        s "Pvc_coercion "; opt core_type ground; s " "; core_type coercion
  and value_binding (vb : value_binding) =
    s "{vb "; pattern vb.pvb_pat; s " "; expression vb.pvb_expr; s " ";
    opt value_constraint vb.pvb_constraint; s " "; attrs vb.pvb_attributes; s " ";
    loc vb.pvb_loc; s "}"

  and value_description (v : value_description) =
    s "{val "; str_loc v.pval_name; s " "; core_type v.pval_type; s " ";
    attrs v.pval_attributes; s " "; loc v.pval_loc; s "}"
  and primitive (p : primitive_description) =
    s "{prim "; str_loc p.pprim_name; s " ";
    (match p.pprim_kind with
     | Pprim_decl (t, l) -> s "Pprim_decl "; core_type t; s " "; list q l
     | Pprim_alias (t, l) -> s "Pprim_alias "; opt core_type t; s " "; lid_loc l);
    s " "; attrs p.pprim_attributes; s " "; loc p.pprim_loc; s "}"

  and type_param ~gap ((t, (v, inj)) : core_type * (Asttypes.variance * Asttypes.injectivity)) =
    core_type t; s " ";
    if gap && !mask_gaps then s "?" else begin
      s (match v with Covariant -> "+" | Contravariant -> "-" | NoVariance -> "."
                    | Bivariant -> "+-");
      s (match inj with Injective -> "!" | NoInjectivity -> "")
    end
  and label_decl (l : label_declaration) =
    s "{ld "; str_loc l.pld_name; s " "; flag_mut l.pld_mutable; s " "; core_type l.pld_type;
    s " "; loc l.pld_loc; s " "; attrs l.pld_attributes; s "}"
  and ctor_args = function
    | Pcstr_tuple tl -> s "Pcstr_tuple "; list core_type tl
    | Pcstr_record ll -> s "Pcstr_record "; list label_decl ll
  and type_declaration (d : type_declaration) =
    s "{td "; str_loc d.ptype_name; s " "; list (type_param ~gap:false) d.ptype_params; s " ";
    list (fun (a, b, l) -> core_type a; s "="; core_type b; s " "; loc l) d.ptype_constraints;
    s " ";
    (match d.ptype_kind with
     | Ptype_abstract -> s "Ptype_abstract"
     | Ptype_variant cds ->
         s "Ptype_variant ";
         list (fun (c : constructor_declaration) ->
             s "{cd "; str_loc c.pcd_name; s " "; list str_gloc c.pcd_vars; s " ";
             ctor_args c.pcd_args; s " "; opt core_type c.pcd_res; s " "; loc c.pcd_loc;
             s " "; attrs c.pcd_attributes; s "}") cds
     | Ptype_record lds -> s "Ptype_record "; list label_decl lds
     | Ptype_open -> s "Ptype_open"
     | Ptype_external n -> s "Ptype_external "; q n);
    s " "; flag_priv d.ptype_private; s " "; opt core_type d.ptype_manifest; s " ";
    attrs d.ptype_attributes; s " "; loc d.ptype_loc; s "}"
  and extension_constructor (c : extension_constructor) =
    s "{ext "; str_loc c.pext_name; s " ";
    (match c.pext_kind with
     | Pext_decl (vs, a, r) ->
         s "Pext_decl "; list str_gloc vs; s " "; ctor_args a; s " "; opt core_type r
     | Pext_rebind l -> s "Pext_rebind "; lid_loc l);
    s " "; loc c.pext_loc; s " "; attrs c.pext_attributes; s "}"
  and type_extension (x : type_extension) =
    s "{tyext "; lid_loc x.ptyext_path; s " "; list (type_param ~gap:true) x.ptyext_params; s " ";
    list extension_constructor x.ptyext_constructors; s " "; flag_priv x.ptyext_private; s " ";
    loc x.ptyext_loc; s " "; attrs x.ptyext_attributes; s "}"
  and type_exception (x : type_exception) =
    s "{tyexn "; extension_constructor x.ptyexn_constructor; s " "; loc x.ptyexn_loc; s " ";
    attrs x.ptyexn_attributes; s "}"

  and open_description (o : open_description) =
    s "{open "; lid_loc o.popen_expr; s " "; flag_ovr o.popen_override; s " "; loc o.popen_loc;
    s " "; attrs o.popen_attributes; s "}"
  and class_type (x : class_type) =
    s "(CT "; loc x.pcty_loc; s " "; attrs x.pcty_attributes; s " ";
    (match x.pcty_desc with
     | Pcty_constr (l, tl) -> s "Pcty_constr "; lid_loc l; s " "; list core_type tl
     | Pcty_signature cs -> s "Pcty_signature "; class_signature cs
     | Pcty_arrow (l, t, c) -> s "Pcty_arrow "; arg_label l; s " "; core_type t; s " "; class_type c
     | Pcty_extension e -> s "Pcty_extension "; extension e
     | Pcty_open (o, c) -> s "Pcty_open "; open_description o; s " "; class_type c);
    s ")"
  and class_signature (cs : class_signature) =
    s "{csig "; core_type cs.pcsig_self; s " ";
    list (fun (f : class_type_field) ->
        s "(CTF "; loc f.pctf_loc; s " "; attrs f.pctf_attributes; s " ";
        (match f.pctf_desc with
         | Pctf_inherit c -> s "Pctf_inherit "; class_type c
         | Pctf_val (n, m, v, t) ->
             s "Pctf_val "; str_loc n; s " "; flag_mut m; s " "; flag_virt v; s " "; core_type t
         | Pctf_method (n, p, v, t) ->
             s "Pctf_method "; str_loc n; s " "; flag_priv p; s " "; flag_virt v; s " "; core_type t
         | Pctf_constraint (a, b) -> s "Pctf_constraint "; core_type a; s " "; core_type b
         | Pctf_attribute a -> s "Pctf_attribute "; attribute ~gap_name:true a
         | Pctf_extension e -> s "Pctf_extension "; extension e);
        s ")") cs.pcsig_fields;
    s "}"
  and class_infos : 'a. ('a -> unit) -> 'a class_infos -> unit = fun f x ->
    s "{ci "; flag_virt x.pci_virt; s " "; list (type_param ~gap:true) x.pci_params; s " ";
    str_loc x.pci_name; s " "; f x.pci_expr; s " "; loc x.pci_loc; s " ";
    attrs x.pci_attributes; s "}"
  and class_expr (x : class_expr) =
    s "(CE "; loc x.pcl_loc; s " "; attrs x.pcl_attributes; s " ";
    (match x.pcl_desc with
     | Pcl_constr (l, tl) -> s "Pcl_constr "; lid_loc l; s " "; list core_type tl
     | Pcl_structure cs -> s "Pcl_structure "; class_structure cs
     | Pcl_fun (l, d, p, c) ->
         s "Pcl_fun "; arg_label l; s " "; opt expression d; s " "; pattern p; s " "; class_expr c
     | Pcl_apply (c, a) -> s "Pcl_apply "; class_expr c; s " "; args a
     | Pcl_let (r, vbs, c) ->
         s "Pcl_let "; flag_rec r; s " "; list value_binding vbs; s " "; class_expr c
     | Pcl_constraint (c, t) -> s "Pcl_constraint "; class_expr c; s " "; class_type t
     | Pcl_extension e -> s "Pcl_extension "; extension e
     | Pcl_open (o, c) -> s "Pcl_open "; open_description o; s " "; class_expr c);
    s ")"
  and class_field_kind = function
    | Cfk_virtual t -> s "Cfk_virtual "; core_type t
    | Cfk_concrete (o, e) -> s "Cfk_concrete "; flag_ovr o; s " "; expression e
  and class_structure (cs : class_structure) =
    s "{cstr "; pattern cs.pcstr_self; s " ";
    list (fun (f : class_field) ->
        s "(CF "; loc f.pcf_loc; s " "; attrs f.pcf_attributes; s " ";
        (match f.pcf_desc with
         | Pcf_inherit (o, c, a) ->
             s "Pcf_inherit "; flag_ovr o; s " "; class_expr c; s " "; opt str_loc a
         | Pcf_val (n, m, k) ->
             s "Pcf_val "; str_loc n; s " "; flag_mut m; s " "; class_field_kind k
         | Pcf_method (n, p, k) ->
             s "Pcf_method "; str_loc n; s " "; flag_priv p; s " "; class_field_kind k
         | Pcf_constraint (a, b) -> s "Pcf_constraint "; core_type a; s " "; core_type b
         | Pcf_initializer e -> s "Pcf_initializer "; expression e
         | Pcf_attribute a -> s "Pcf_attribute "; attribute ~gap_name:true a
         | Pcf_extension e -> s "Pcf_extension "; extension e);
        s ")") cs.pcstr_fields;
    s "}"

  and functor_param = function
    | Unit -> s "Unit"
    | Named (n, mt) -> s "Named "; opt q n.txt; s " "; loc n.loc; s " "; module_type mt
  and module_type (m : module_type) =
    s "(MT "; loc m.pmty_loc; s " "; attrs m.pmty_attributes; s " ";
    (match m.pmty_desc with
     | Pmty_ident l -> s "Pmty_ident "; lid_loc l
     | Pmty_signature sg -> s "Pmty_signature "; signature sg
     | Pmty_functor (p, b) -> s "Pmty_functor "; functor_param p; s " "; module_type b
     | Pmty_with (mt, cs) ->
         s "Pmty_with "; module_type mt; s " ";
         list (function
             | Pwith_type (l, d) -> s "Pwith_type "; lid_loc l; s " "; type_declaration d
             | Pwith_module (a, b) -> s "Pwith_module "; lid_loc a; s " "; lid_loc b
             | Pwith_modtype (l, m) -> s "Pwith_modtype "; lid_loc l; s " "; module_type m
             | Pwith_modtypesubst (l, m) -> s "Pwith_modtypesubst "; lid_loc l; s " "; module_type m
             | Pwith_typesubst (l, d) -> s "Pwith_typesubst "; lid_loc l; s " "; type_declaration d
             | Pwith_modsubst (a, b) -> s "Pwith_modsubst "; lid_loc a; s " "; lid_loc b) cs
     | Pmty_typeof me -> s "Pmty_typeof "; module_expr me
     | Pmty_extension e -> s "Pmty_extension "; extension e
     | Pmty_alias l -> s "Pmty_alias "; lid_loc l);
    s ")"
  and module_declaration (md : module_declaration) =
    s "{md "; opt q md.pmd_name.txt; s " "; loc md.pmd_name.loc; s " "; module_type md.pmd_type;
    s " "; attrs md.pmd_attributes; s " "; loc md.pmd_loc; s "}"
  and modtype_declaration (m : module_type_declaration) =
    s "{mtd "; str_loc m.pmtd_name; s " "; opt module_type m.pmtd_type; s " ";
    attrs m.pmtd_attributes; s " "; loc m.pmtd_loc; s "}"
  and signature_item (it : signature_item) =
    s "(SI "; loc it.psig_loc; s " ";
    (match it.psig_desc with
     | Psig_value v -> s "Psig_value "; value_description v
     | Psig_primitive p -> s "Psig_primitive "; primitive p
     | Psig_type (r, l) -> s "Psig_type "; flag_rec r; s " "; list type_declaration l
     | Psig_typesubst l -> s "Psig_typesubst "; list type_declaration l
     | Psig_typext x -> s "Psig_typext "; type_extension x
     | Psig_exception x -> s "Psig_exception "; type_exception x
     | Psig_module md -> s "Psig_module "; module_declaration md
     | Psig_modsubst ms ->
         s "Psig_modsubst "; str_loc ms.pms_name; s " "; lid_loc ms.pms_manifest; s " ";
         attrs ms.pms_attributes; s " "; loc ms.pms_loc
     | Psig_recmodule l -> s "Psig_recmodule "; list module_declaration l
     | Psig_modtype m -> s "Psig_modtype "; modtype_declaration m
     | Psig_modtypesubst m -> s "Psig_modtypesubst "; modtype_declaration m
     | Psig_open o -> s "Psig_open "; open_description o
     | Psig_include x ->
         s "Psig_include "; module_type x.pincl_mod; s " "; loc x.pincl_loc; s " ";
         attrs x.pincl_attributes
     | Psig_class l -> s "Psig_class "; list (class_infos class_type) l
     | Psig_class_type l -> s "Psig_class_type "; list (class_infos class_type) l
     | Psig_attribute a -> s "Psig_attribute "; attribute ~gap_name:true a
     | Psig_extension (e, a) -> s "Psig_extension "; extension e; s " "; attrs a);
    s ")"
  and signature sg = list signature_item sg

  and module_expr (m : module_expr) =
    s "(ME "; loc m.pmod_loc; s " "; attrs m.pmod_attributes; s " ";
    (match m.pmod_desc with
     | Pmod_ident l -> s "Pmod_ident "; lid_loc l
     | Pmod_structure st -> s "Pmod_structure "; structure st
     | Pmod_functor (p, b) -> s "Pmod_functor "; functor_param p; s " "; module_expr b
     | Pmod_apply (a, b) -> s "Pmod_apply "; module_expr a; s " "; module_expr b
     | Pmod_apply_unit a -> s "Pmod_apply_unit "; module_expr a
     | Pmod_constraint (a, t) -> s "Pmod_constraint "; module_expr a; s " "; module_type t
     | Pmod_unpack e -> s "Pmod_unpack "; expression e
     | Pmod_extension e -> s "Pmod_extension "; extension e);
    s ")"
  and module_binding (mb : module_binding) =
    s "{mb "; opt q mb.pmb_name.txt; s " "; loc mb.pmb_name.loc; s " "; module_expr mb.pmb_expr;
    s " "; attrs mb.pmb_attributes; s " "; loc mb.pmb_loc; s "}"
  and structure_item (it : structure_item) =
    s "(SI "; loc it.pstr_loc; s " ";
    (match it.pstr_desc with
     | Pstr_eval (e, a) -> s "Pstr_eval "; expression e; s " "; attrs a
     | Pstr_value (r, vbs) -> s "Pstr_value "; flag_rec r; s " "; list value_binding vbs
     | Pstr_val v -> s "Pstr_val "; value_description v
     | Pstr_primitive p -> s "Pstr_primitive "; primitive p
     | Pstr_type (r, l) -> s "Pstr_type "; flag_rec r; s " "; list type_declaration l
     | Pstr_typext x -> s "Pstr_typext "; type_extension x
     | Pstr_exception x -> s "Pstr_exception "; type_exception x
     | Pstr_module mb -> s "Pstr_module "; module_binding mb
     | Pstr_recmodule l -> s "Pstr_recmodule "; list module_binding l
     | Pstr_modtype m -> s "Pstr_modtype "; modtype_declaration m
     | Pstr_open o ->
         s "Pstr_open "; module_expr o.popen_expr; s " "; flag_ovr o.popen_override; s " ";
         loc o.popen_loc; s " "; attrs o.popen_attributes
     | Pstr_class l -> s "Pstr_class "; list (class_infos class_expr) l
     | Pstr_class_type l -> s "Pstr_class_type "; list (class_infos class_type) l
     | Pstr_include x ->
         s "Pstr_include "; module_expr x.pincl_mod; s " "; loc x.pincl_loc; s " ";
         attrs x.pincl_attributes
     | Pstr_attribute a -> s "Pstr_attribute "; attribute ~gap_name:true a
     | Pstr_extension (e, a) -> s "Pstr_extension "; extension e; s " "; attrs a);
    s ")\n"
  and structure st = list structure_item st
end

let dump_parse file =
  parse_file := file;
  let st = Pparse.parse_implementation ~tool_name:"ocamlc" file in
  P.structure st; s "\n"

(* ---- stage 4b: Typetexp ---- *)

let texp_error_name : Typetexp.error -> string = function
  | Unbound_type_variable _ -> "Unbound_type_variable"
  | No_type_wildcards -> "No_type_wildcards"
  | Undefined_type_constructor _ -> "Undefined_type_constructor"
  | Type_arity_mismatch _ -> "Type_arity_mismatch"
  | Bound_type_variable _ -> "Bound_type_variable"
  | Recursive_type -> "Recursive_type"
  | Type_mismatch _ -> "Type_mismatch"
  | Alias_type_mismatch _ -> "Alias_type_mismatch"
  | Present_has_conjunction _ -> "Present_has_conjunction"
  | Present_has_no_type _ -> "Present_has_no_type"
  | Constructor_mismatch _ -> "Constructor_mismatch"
  | Not_a_variant _ -> "Not_a_variant"
  | Variant_tags _ -> "Variant_tags"
  | Invalid_variable_name _ -> "Invalid_variable_name"
  | Cannot_quantify _ -> "Cannot_quantify"
  | Multiple_constraints_on_type _ -> "Multiple_constraints_on_type"
  | Method_mismatch _ -> "Method_mismatch"
  | Opened_object _ -> "Opened_object"
  | Not_an_object _ -> "Not_an_object"
  | Repeated_tuple_label _ -> "Repeated_tuple_label"
  | Polymorphic_optional_param _ -> "Polymorphic_optional_param"
  | Functor_optional_param _ -> "Functor_optional_param"

let lookup_error_name : Env.lookup_error -> string = function
  | Unbound_value _ -> "Unbound_value" | Unbound_type _ -> "Unbound_type"
  | Unbound_constructor _ -> "Unbound_constructor" | Unbound_label _ -> "Unbound_label"
  | Unbound_module _ -> "Unbound_module" | Unbound_class _ -> "Unbound_class"
  | Unbound_modtype _ -> "Unbound_modtype" | Unbound_cltype _ -> "Unbound_cltype"
  | Unbound_instance_variable _ -> "Unbound_instance_variable"
  | Not_an_instance_variable _ -> "Not_an_instance_variable"
  | Masked_instance_variable _ -> "Masked_instance_variable"
  | Masked_self_variable _ -> "Masked_self_variable"
  | Masked_ancestor_variable _ -> "Masked_ancestor_variable"
  | Structure_used_as_functor _ -> "Structure_used_as_functor"
  | Abstract_used_as_functor _ -> "Abstract_used_as_functor"
  | Functor_used_as_structure _ -> "Functor_used_as_structure"
  | Abstract_used_as_structure _ -> "Abstract_used_as_structure"
  | Generative_used_as_applicative _ -> "Generative_used_as_applicative"
  | Illegal_reference_to_recursive_module _ -> "Illegal_reference_to_recursive_module"
  | Illegal_reference_to_recursive_class_type _ ->
      "Illegal_reference_to_recursive_class_type"
  | Cannot_scrape_alias _ -> "Cannot_scrape_alias"

let rec ctyp (c : Typedtree.core_type) =
  s "(CT "; P.loc c.ctyp_loc; s " ";
  (match c.ctyp_desc with
   | Ttyp_any -> s "Ttyp_any"
   | Ttyp_var n -> s "Ttyp_var "; q n
   | Ttyp_arrow (l, a, b) -> s "Ttyp_arrow "; arg_label l; s " "; ctyp a; s " "; ctyp b
   | Ttyp_tuple l -> s "Ttyp_tuple "; list (fun (l, c) -> opt q l; s ":"; ctyp c) l
   | Ttyp_constr (p, l, a) -> s "Ttyp_constr "; path p; s " "; P.lid_loc l; s " "; list ctyp a
   | Ttyp_object (fl, c) ->
       s "Ttyp_object ";
       list (fun (f : Typedtree.object_field) ->
           (match f.of_desc with
            | OTtag (n, c) -> s "OTtag "; P.str_loc n; s " "; ctyp c
            | OTinherit c -> s "OTinherit "; ctyp c)) fl;
       s " "; P.flag_closed c
   | Ttyp_class (p, l, a) -> s "Ttyp_class "; path p; s " "; P.lid_loc l; s " "; list ctyp a
   | Ttyp_alias (c, n) -> s "Ttyp_alias "; ctyp c; s " "; q n.txt
   | Ttyp_variant (fl, c, ls) ->
       s "Ttyp_variant ";
       list (fun (f : Typedtree.row_field) ->
           (match f.rf_desc with
            | Ttag (n, b, cl) -> s "Ttag "; q n.txt; s " "; bool b; s " "; list ctyp cl
            | Tinherit c -> s "Tinherit "; ctyp c)) fl;
       s " "; P.flag_closed c; s " "; opt (list q) ls
   | Ttyp_poly (vs, c) -> s "Ttyp_poly "; list q vs; s " "; ctyp c
   | Ttyp_package p ->
       s "Ttyp_package "; path p.tpt_path; s " ";
       list (fun (l, c) -> P.lid_loc l; s "="; ctyp c) p.tpt_constraints
   | Ttyp_open (p, l, c) -> s "Ttyp_open "; path p; s " "; P.lid_loc l; s " "; ctyp c
   | Ttyp_functor (lb, id, p, c) ->
       s "Ttyp_functor "; arg_label lb; s " "; ident id.txt; s " "; path p.tpt_path; s " "; ctyp c);
  s " : "; ty c.ctyp_type; s ")"

let run_typexp dirs modname file =
  parse_file := file;
  Load_path.init ~auto_include:Load_path.no_auto_include
    ~visible:(String.split_on_char ':' dirs) ~hidden:[];
  let env =
    match Env.open_pers_signature "Stdlib" Env.initial with
    | Ok env -> env
    | Error _ -> failwith "open Stdlib"
  in
  let env =
    if modname = "-" then env
    else match Env.open_pers_signature modname env with
      | Ok env -> env
      | Error _ -> env
      | exception _ -> env
  in
  let sg = Pparse.parse_interface ~tool_name:"ocamlc" file in
  List.iter (fun (it : Parsetree.signature_item) ->
      let one name (sty : Parsetree.core_type) =
        reset_numbering ();
        s name; s " => ";
        (try ctyp (Typetexp.transl_type_scheme env sty) with
         | Typetexp.Error.In_context (l, _, e) ->
             s "ERR "; s (texp_error_name e); s " "; P.loc l
         | Env.Error.In_context (Lookup_error (l, _, e)) ->
             s "ERR Env."; s (lookup_error_name e); s " "; P.loc l
         | Env.Error.In_context _ -> s "ERR Env.other");
        s "\n"
      in
      match it.psig_desc with
      | Psig_value vd -> one vd.pval_name.txt vd.pval_type
      | Psig_primitive pd ->
          (match pd.pprim_kind with
           | Pprim_decl (t, _) | Pprim_alias (Some t, _) -> one pd.pprim_name.txt t
           | Pprim_alias (None, _) -> ())
      | _ -> ())
    sg

(* ---- stage 4c: Typecore ---- *)

let tc_error_name : Typecore.error -> string = function
  | Constructor_arity_mismatch _ -> "Constructor_arity_mismatch"
  | Label_mismatch _ -> "Label_mismatch"
  | Pattern_type_clash _ -> "Pattern_type_clash"
  | Or_pattern_type_clash _ -> "Or_pattern_type_clash"
  | Multiply_bound_variable _ -> "Multiply_bound_variable"
  | Orpat_vars _ -> "Orpat_vars"
  | Expr_type_clash _ -> "Expr_type_clash"
  | Function_arity_type_clash _ -> "Function_arity_type_clash"
  | Apply_non_function _ -> "Apply_non_function"
  | Apply_wrong_label _ -> "Apply_wrong_label"
  | Label_multiply_defined _ -> "Label_multiply_defined"
  | Label_missing _ -> "Label_missing"
  | Label_not_mutable _ -> "Label_not_mutable"
  | Wrong_name _ -> "Wrong_name"
  | Name_type_mismatch _ -> "Name_type_mismatch"
  | Invalid_format _ -> "Invalid_format"
  | Not_an_object _ -> "Not_an_object"
  | Undefined_method _ -> "Undefined_method"
  | Undefined_self_method _ -> "Undefined_self_method"
  | Virtual_class _ -> "Virtual_class"
  | Private_type _ -> "Private_type"
  | Private_label _ -> "Private_label"
  | Private_constructor _ -> "Private_constructor"
  | Unbound_instance_variable _ -> "Unbound_instance_variable"
  | Instance_variable_not_mutable _ -> "Instance_variable_not_mutable"
  | Not_subtype _ -> "Not_subtype"
  | Outside_class -> "Outside_class"
  | Value_multiply_overridden _ -> "Value_multiply_overridden"
  | Coercion_failure _ -> "Coercion_failure"
  | Not_a_function _ -> "Not_a_function"
  | Too_many_arguments _ -> "Too_many_arguments"
  | Abstract_wrong_label _ -> "Abstract_wrong_label"
  | Not_a_polymorphic_variant_type _ -> "Not_a_polymorphic_variant_type"
  | Incoherent_label_order -> "Incoherent_label_order"
  | Less_general _ -> "Less_general"
  | Modules_not_allowed -> "Modules_not_allowed"
  | Cannot_infer_signature -> "Cannot_infer_signature"
  | Not_a_packed_module _ -> "Not_a_packed_module"
  | Unexpected_existential _ -> "Unexpected_existential"
  | Invalid_interval -> "Invalid_interval"
  | Invalid_for_loop_index -> "Invalid_for_loop_index"
  | No_value_clauses -> "No_value_clauses"
  | Exception_pattern_disallowed -> "Exception_pattern_disallowed"
  | Mixed_value_and_exception_patterns_under_guard -> "Mixed_value_and_exception_patterns_under_guard"
  | Effect_pattern_below_toplevel -> "Effect_pattern_below_toplevel"
  | Invalid_continuation_pattern -> "Invalid_continuation_pattern"
  | Inlined_record_escape -> "Inlined_record_escape"
  | Inlined_record_expected -> "Inlined_record_expected"
  | Unrefuted_pattern _ -> "Unrefuted_pattern"
  | Invalid_extension_constructor_payload -> "Invalid_extension_constructor_payload"
  | Not_an_extension_constructor -> "Not_an_extension_constructor"
  | Invalid_atomic_loc_payload -> "Invalid_atomic_loc_payload"
  | Label_not_atomic _ -> "Label_not_atomic"
  | Atomic_in_pattern _ -> "Atomic_in_pattern"
  | Literal_overflow _ -> "Literal_overflow"
  | Unknown_literal _ -> "Unknown_literal"
  | Illegal_letrec_pat -> "Illegal_letrec_pat"
  | Illegal_letrec_expr -> "Illegal_letrec_expr"
  | Illegal_class_expr -> "Illegal_class_expr"
  | Letop_type_clash _ -> "Letop_type_clash"
  | Andop_type_clash _ -> "Andop_type_clash"
  | Bindings_type_clash _ -> "Bindings_type_clash"
  | Unbound_existential _ -> "Unbound_existential"
  | Bind_existential _ -> "Bind_existential"
  | Missing_type_constraint -> "Missing_type_constraint"
  | Wrong_expected_kind _ -> "Wrong_expected_kind"
  | Expr_not_a_record_type _ -> "Expr_not_a_record_type"
  | Constructor_labeled_arg -> "Constructor_labeled_arg"
  | Partial_tuple_pattern_bad_type -> "Partial_tuple_pattern_bad_type"
  | Extra_tuple_label _ -> "Extra_tuple_label"
  | Missing_tuple_label _ -> "Missing_tuple_label"
  | Repeated_tuple_exp_label _ -> "Repeated_tuple_exp_label"
  | Repeated_tuple_pat_label _ -> "Repeated_tuple_pat_label"
  | Optional_poly_param _ -> "Optional_poly_param"
  | Cannot_unify_tfunctor_to_tarrow _ -> "Cannot_unify_tfunctor_to_tarrow"
  | Cannot_omit_tfunctor_argument _ -> "Cannot_omit_tfunctor_argument"

let td_error_name : Typedecl.error -> string = function
  | Repeated_parameter -> "Repeated_parameter"
  | Duplicate_constructor _ -> "Duplicate_constructor"
  | Too_many_constructors -> "Too_many_constructors"
  | Duplicate_label _ -> "Duplicate_label"
  | Recursive_abbrev _ -> "Recursive_abbrev"
  | Cycle_in_def _ -> "Cycle_in_def"
  | Definition_mismatch _ -> "Definition_mismatch"
  | Constraint_failed _ -> "Constraint_failed"
  | Inconsistent_constraint _ -> "Inconsistent_constraint"
  | Type_clash _ -> "Type_clash"
  | Non_regular _ -> "Non_regular"
  | Null_arity_external -> "Null_arity_external"
  | Missing_native_external -> "Missing_native_external"
  | Unbound_type_var _ -> "Unbound_type_var"
  | Cannot_extend_private_type _ -> "Cannot_extend_private_type"
  | Not_extensible_type _ -> "Not_extensible_type"
  | Extension_mismatch _ -> "Extension_mismatch"
  | Rebind_wrong_type _ -> "Rebind_wrong_type"
  | Rebind_mismatch _ -> "Rebind_mismatch"
  | Rebind_private _ -> "Rebind_private"
  | Variance _ -> "Variance"
  | Unavailable_type_constructor _ -> "Unavailable_type_constructor"
  | Multiple_native_repr_attributes -> "Multiple_native_repr_attributes"
  | Cannot_unbox_or_untag_type _ -> "Cannot_unbox_or_untag_type"
  | Deep_unbox_or_untag_attribute _ -> "Deep_unbox_or_untag_attribute"
  | Type_cannot_be_external _ -> "Type_cannot_be_external"
  | Immediacy _ -> "Immediacy"
  | Separability _ -> "Separability"
  | Bad_unboxed_attribute _ -> "Bad_unboxed_attribute"
  | Boxed_and_unboxed -> "Boxed_and_unboxed"
  | Nonrec_gadt -> "Nonrec_gadt"
  | Invalid_private_row_declaration _ -> "Invalid_private_row_declaration"
  | Atomic_field_must_be_mutable _ -> "Atomic_field_must_be_mutable"
  | External_with_non_syntactic_arity -> "External_with_non_syntactic_arity"
  | Primitive_alias_does_not_refer_to_primitive _ -> "Primitive_alias_does_not_refer_to_primitive"
  | Primitive_type_mismatch _ -> "Primitive_type_mismatch"

let tm_error_name : Typemod.error -> string = function
  | Cannot_apply _ -> "Cannot_apply"
  | Not_included _ -> "Not_included"
  | Cannot_eliminate_dependency _ -> "Cannot_eliminate_dependency"
  | Signature_expected -> "Signature_expected"
  | Structure_expected _ -> "Structure_expected"
  | With_no_component _ -> "With_no_component"
  | With_mismatch _ -> "With_mismatch"
  | With_makes_applicative_functor_ill_typed _ -> "With_makes_applicative_functor_ill_typed"
  | With_changes_module_alias _ -> "With_changes_module_alias"
  | With_creates_invalid_aliases _ -> "With_creates_invalid_aliases"
  | With_cannot_remove_constrained_type -> "With_cannot_remove_constrained_type"
  | With_package_manifest _ -> "With_package_manifest"
  | Repeated_name _ -> "Repeated_name"
  | Non_generalizable _ -> "Non_generalizable"
  | Non_generalizable_module _ -> "Non_generalizable_module"
  | Implementation_is_required _ -> "Implementation_is_required"
  | Interface_not_compiled _ -> "Interface_not_compiled"
  | Not_allowed_in_functor_body -> "Not_allowed_in_functor_body"
  | Not_a_packed_module _ -> "Not_a_packed_module"
  | Incomplete_packed_module _ -> "Incomplete_packed_module"
  | Scoping_pack _ -> "Scoping_pack"
  | Recursive_module_require_explicit_type -> "Recursive_module_require_explicit_type"
  | Apply_generative -> "Apply_generative"
  | Cannot_scrape_alias _ -> "Cannot_scrape_alias"
  | Cannot_scrape_package_type _ -> "Cannot_scrape_package_type"
  | Badly_formed_signature _ -> "Badly_formed_signature"
  | Cannot_hide_id _ -> "Cannot_hide_id"
  | Invalid_type_subst_rhs -> "Invalid_type_subst_rhs"
  | Non_packable_local_modtype_subst _ -> "Non_packable_local_modtype_subst"
  | With_cannot_remove_packed_modtype _ -> "With_cannot_remove_packed_modtype"
  | Cannot_alias _ -> "Cannot_alias"
  | Val_in_structure -> "Val_in_structure"

let tcl_error_name : Typeclass.error -> string = function
  | Unconsistent_constraint _ -> "Unconsistent_constraint"
  | Field_type_mismatch _ -> "Field_type_mismatch"
  | Unexpected_field _ -> "Unexpected_field"
  | Structure_expected _ -> "Structure_expected"
  | Cannot_apply _ -> "Cannot_apply"
  | Apply_wrong_label _ -> "Apply_wrong_label"
  | Pattern_type_clash _ -> "Pattern_type_clash"
  | Repeated_parameter -> "Repeated_parameter"
  | Unbound_class_2 _ -> "Unbound_class_2"
  | Unbound_class_type_2 _ -> "Unbound_class_type_2"
  | Abbrev_type_clash _ -> "Abbrev_type_clash"
  | Constructor_type_mismatch _ -> "Constructor_type_mismatch"
  | Virtual_class _ -> "Virtual_class"
  | Undeclared_methods _ -> "Undeclared_methods"
  | Parameter_arity_mismatch _ -> "Parameter_arity_mismatch"
  | Parameter_mismatch _ -> "Parameter_mismatch"
  | Bad_parameters _ -> "Bad_parameters"
  | Bad_class_type_parameters _ -> "Bad_class_type_parameters"
  | Class_match_failure _ -> "Class_match_failure"
  | Unbound_val _ -> "Unbound_val"
  | Unbound_type_var _ -> "Unbound_type_var"
  | Non_generalizable_class _ -> "Non_generalizable_class"
  | Cannot_coerce_self _ -> "Cannot_coerce_self"
  | Non_collapsable_conjunction _ -> "Non_collapsable_conjunction"
  | Self_clash _ -> "Self_clash"
  | Mutability_mismatch _ -> "Mutability_mismatch"
  | No_overriding _ -> "No_overriding"
  | Duplicate _ -> "Duplicate"
  | Closing_self_type _ -> "Closing_self_type"
  | Polymorphic_class_parameter -> "Polymorphic_class_parameter"

let report (e : exn) =
  s "ERR ";
  (match e with
   | Typecore.Error.In_context (l, _, err) ->
       s "Typecore."; s (tc_error_name err); s " "; P.loc l
   | Typetexp.Error.In_context (l, _, err) ->
       s "Typetexp."; s (texp_error_name err); s " "; P.loc l
   | Env.Error.In_context (Lookup_error (l, _, err)) ->
       s "Env."; s (lookup_error_name err); s " "; P.loc l
   | Env.Error.In_context (Missing_module (l, _, _)) ->
       s "Env.Missing_module "; P.loc l
   | Env.Error.In_context (Illegal_value_name (l, _)) ->
       s "Env.Illegal_value_name "; P.loc l
   | Typedecl.Error.In_context (l, err) ->
       s "Typedecl."; s (td_error_name err); s " "; P.loc l
   | Attr_helper.Error (l, _) -> s "Attr_helper "; P.loc l
   | e when Printexc.exn_slot_name e = "Typemod.Error.In_context" ->
       (* not exported by typemod.mli: In_context of loc * env * error *)
       let r = Obj.repr e in
       s "Typemod."; s (tm_error_name (Obj.obj (Obj.field r 3))); s " ";
       P.loc (Obj.obj (Obj.field r 1) : Location.t)
   | Includemod.Apply_error { loc = l; _ } -> s "Includemod.Apply_error "; P.loc l
   | Includemod.Error _ -> s "Includemod.Error"
   | Typeclass.Error.In_context (l, _, err) ->
       s "Typeclass."; s (tcl_error_name err); s " "; P.loc l
   | Typemod.Error_forward err | Typeclass.Error_forward err ->
       s "Error_forward "; P.loc err.Location.main.loc
   | Primitive.Error (l, _) -> s "Primitive "; P.loc l
   | Typecore.Error_forward err ->
       s "Error_forward "; P.loc err.Location.main.loc
   | e when Printexc.exn_slot_name e = "Typetexp.Error_forward" ->
       (* not exported: Error_forward of Location.error *)
       let err : Location.error = Obj.obj (Obj.field (Obj.repr e) 1) in
       s "Error_forward "; P.loc err.Location.main.loc
   | Syntaxerr.Error _ -> s "Syntaxerr"
   | e -> raise e);
  s "\n"

(* Type the structure items a Typemod-free port can: `let` bindings
   (Typecore.type_binding) and evaluated expressions (type_expression), in
   Env.initial + open Stdlib; stop at the first other item. *)
let run_core dirs file =
  parse_file := file;
  Load_path.init ~auto_include:Load_path.no_auto_include
    ~visible:(String.split_on_char ':' dirs) ~hidden:[];
  let env =
    match Env.open_pers_signature "Stdlib" Env.initial with
    | Ok env -> env
    | Error _ -> failwith "open Stdlib"
  in
  let st = Pparse.parse_implementation ~tool_name:"ocamlc" file in
  Typecore.reset_delayed_checks ();
  let rec go env = function
    | [] ->
        (match Typecore.force_delayed_checks () with
         | () -> s "END\n"
         | exception e -> report e)
    | (it : Parsetree.structure_item) :: rest ->
        match it.pstr_desc with
        | Pstr_value (rf, vbs) ->
            (match Typecore.type_binding env rf vbs with
             | exception e -> report e
             | (defs, newenv) ->
                 List.iter (fun id ->
                     reset_numbering ();
                     s "val "; s (Ident.name id); s " : ";
                     ty (Env.find_value (Path.Pident id) newenv).val_type; s "\n")
                   (Typedtree.let_bound_idents defs);
                 go newenv rest)
        | Pstr_eval (e, _) ->
            (match Typecore.type_expression env e with
             | exception e -> report e
             | exp ->
                 reset_numbering (); s "eval : "; ty exp.exp_type; s "\n";
                 go env rest)
        | Pstr_type (rf, sdecls) ->
            (match Typedecl.transl_type_decl env rf sdecls with
             | exception e -> report e
             | (decls, newenv, _) ->
                 List.iter (fun (d : Typedtree.type_declaration) ->
                     reset_numbering ();
                     s "type "; s (Ident.name d.typ_id); s " : "; type_decl d.typ_type; s "\n")
                   decls;
                 go newenv rest)
        | Pstr_typext te ->
            (match Typedecl.transl_type_extension true env it.pstr_loc te with
             | exception e -> report e
             | (tyext, newenv, _) ->
                 List.iter (fun (c : Typedtree.extension_constructor) ->
                     reset_numbering ();
                     s "ext "; s (Ident.name c.ext_id); s " : "; ext_constr c.ext_type; s "\n")
                   tyext.tyext_constructors;
                 go newenv rest)
        | Pstr_exception te ->
            (match Typedecl.transl_type_exception env te with
             | exception e -> report e
             | (texn, newenv, _) ->
                 reset_numbering ();
                 s "exn "; s (Ident.name texn.tyexn_constructor.ext_id); s " : ";
                 ext_constr texn.tyexn_constructor.ext_type; s "\n";
                 go newenv rest)
        | Pstr_primitive pd ->
            (match Typedecl.transl_prim_desc env it.pstr_loc pd with
             | exception e -> report e
             | (desc, newenv) ->
                 reset_numbering ();
                 s "val "; s (Ident.name desc.prim_id); s " : ";
                 ty desc.prim_val.val_type; s " "; value_kind desc.prim_val.val_kind; s "\n";
                 go newenv rest)
        | _ -> s "STOP\n"
  in
  go env st

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

(* ---- stage 5: Typemod.type_structure on a whole implementation ---- *)
let run_struct dirs file =
  parse_file := file;
  Load_path.init ~auto_include:Load_path.no_auto_include
    ~visible:(String.split_on_char ':' dirs) ~hidden:[];
  let env =
    match Env.open_pers_signature "Stdlib" Env.initial with
    | Ok env -> env
    | Error _ -> failwith "open Stdlib"
  in
  let st = Pparse.parse_implementation ~tool_name:"ocamlc" file in
  Typecore.reset_delayed_checks ();
  Env.reset_required_globals ();
  match
    let (_str, sg, names, _shape, finalenv) = Typemod.type_structure env st in
    let simple_sg = Typemod.Signature_names.simplify finalenv names sg in
    Typemod.check_nongen_signature finalenv simple_sg;
    Typecore.force_delayed_checks ();
    simple_sg
  with
  | sg -> reset_numbering (); s "sig"; signature 1 sg; s "\nEND\n"
  | exception e -> report e

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
  | _ :: "typexp" :: dirs :: modname :: file :: _ ->
      canonical := true;
      run_typexp dirs modname file; print_string (Buffer.contents b)
  | _ :: "struct" :: dirs :: file :: _ ->
      canonical := true;
      run_struct dirs file; print_string (Buffer.contents b)
  | _ :: "core" :: dirs :: file :: _ ->
      canonical := true;
      run_core dirs file; print_string (Buffer.contents b)
  | _ :: "parse" :: file :: _ ->
      dump_parse file; print_string (Buffer.contents b)
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
