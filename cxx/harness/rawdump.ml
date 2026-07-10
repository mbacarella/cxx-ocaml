(* Raw ident-stamp dumper: read a .cmi, walk its signature, print every
   type ident / extension type-path ident / value-type Tconstr ident with its
   Ident stamp so we can see stamp collisions Printtyp hides behind `/n`. *)
let stamp id = Ident.unique_name id

let rec path = function
  | Path.Pident id -> "Pident(" ^ stamp id ^ ")"
  | Path.Pdot (p, s) -> path p ^ "." ^ s
  | Path.Papply (a, b) -> path a ^ "(" ^ path b ^ ")"
  | Path.Pextra_ty (p, _) -> path p ^ "#extra"

let rec ty t =
  let t = Types.get_desc t in
  match t with
  | Types.Tconstr (p, args, _) ->
      "[" ^ path p ^ (if args = [] then "" else "<" ^ String.concat "," (List.map ty args) ^ ">") ^ "]"
  | Types.Tarrow (_, a, b, _) -> ty a ^ " -> " ^ ty b
  | Types.Ttuple ts -> String.concat " * " (List.map (fun (_,x) -> ty x) ts)
  | Types.Tvar _ -> "'a"
  | Types.Tpoly (t, _) -> ty t
  | Types.Tlink t | Types.Tsubst (t, _) -> ty t
  | Types.Tnil -> "nil"
  | Types.Tfield _ -> "field"
  | Types.Tobject _ -> "obj"
  | Types.Tvariant _ -> "variant"
  | Types.Tunivar _ -> "univar"
  | Types.Tpackage _ -> "package"

let rec sigi indent s =
  List.iter (fun item ->
    match item with
    | Types.Sig_type (id, td, _, _) ->
        Printf.printf "%stype %s%s\n" indent (stamp id)
          (match td.Types.type_manifest with
           | Some m -> " = " ^ ty m
           | None -> "");
        (match td.Types.type_kind with
         | Types.Type_record (lds, _) ->
             List.iter (fun ld ->
               Printf.printf "%s  { %s : %s }\n" indent
                 (Ident.name ld.Types.ld_id) (ty ld.Types.ld_type)) lds
         | Types.Type_variant (cds, _) ->
             List.iter (fun cd ->
               match cd.Types.cd_args with
               | Types.Cstr_tuple args ->
                   Printf.printf "%s  | %s of %s\n" indent
                     (Ident.name cd.Types.cd_id)
                     (String.concat " * " (List.map ty args))
               | Types.Cstr_record lds ->
                   List.iter (fun ld ->
                     Printf.printf "%s  | %s { %s : %s }\n" indent
                       (Ident.name cd.Types.cd_id)
                       (Ident.name ld.Types.ld_id) (ty ld.Types.ld_type)) lds) cds
         | _ -> ())
    | Types.Sig_typext (id, ec, _, _) ->
        Printf.printf "%stypext %s : extends %s\n" indent (stamp id)
          (path ec.Types.ext_type_path)
    | Types.Sig_value (id, vd, _) ->
        Printf.printf "%sval %s : %s\n" indent (stamp id) (ty vd.Types.val_type)
    | Types.Sig_module (id, _, md, _, _) ->
        Printf.printf "%smodule %s =\n" indent (stamp id);
        (match md.Types.md_type with
         | Types.Mty_signature s -> sigi (indent ^ "  ") s
         | _ -> Printf.printf "%s  <non-sig>\n" indent)
    | _ -> ()) s

let () =
  let cmi = Cmi_format.read_cmi Sys.argv.(1) in
  sigi "" cmi.Cmi_format.cmi_sign
