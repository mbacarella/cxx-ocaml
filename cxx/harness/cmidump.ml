(* Canonical, uid-INDEPENDENT but location-AWARE dump of a .cmi, for verifying
   c++ocamlc's written interface against the oracle's "modulo uids".

   Two sections, both decoded with the compiler's own Cmi_format reader:
     SIG  -- Printtyp.signature (the type structure; Printtyp ignores uids AND
             locations, so this is exactly the existing cmi_parity view).
     LOC  -- one line per declaration with its source Location.t (filename
             normalised to a basename, positions as lnum:col).  This is the part
             Printtyp hides; it is what byte-identity additionally needs.

   Uids never appear in either section, so a diff of two cmidumps is forgiving of
   uid numbering (the fragile tail) while still catching locations and every
   structural difference.  Rejected files exit 3. *)

let base f = Filename.basename f

let pos (p : Lexing.position) =
  Printf.sprintf "%s:%d:%d" (base p.Lexing.pos_fname) p.Lexing.pos_lnum
    (p.Lexing.pos_cnum - p.Lexing.pos_bol)

let loc (l : Location.t) =
  if l.Location.loc_ghost then "GHOST"
  else Printf.sprintf "%s-%s" (pos l.Location.loc_start) (pos l.Location.loc_end)

let p path what l = Printf.printf "LOC %s %s %s\n" path what (loc l)

let rec walk prefix (items : Types.signature) =
  List.iter
    (fun (it : Types.signature_item) ->
      match it with
      | Sig_value (id, vd, _) -> p (prefix ^ Ident.name id) "val" vd.val_loc
      | Sig_type (id, td, _, _) ->
          let tp = prefix ^ Ident.name id in
          p tp "type" td.type_loc;
          (match td.type_kind with
           | Type_variant (cds, _) ->
               List.iter
                 (fun (cd : Types.constructor_declaration) ->
                   p (tp ^ "#" ^ Ident.name cd.cd_id) "ctor" cd.cd_loc;
                   match cd.cd_args with
                   | Cstr_record lds ->
                       List.iter
                         (fun (ld : Types.label_declaration) ->
                           p (tp ^ "#" ^ Ident.name cd.cd_id ^ "." ^ Ident.name ld.ld_id)
                             "inlbl" ld.ld_loc)
                         lds
                   | Cstr_tuple _ -> ())
                 cds
           | Type_record (lds, _) ->
               List.iter
                 (fun (ld : Types.label_declaration) ->
                   p (tp ^ "." ^ Ident.name ld.ld_id) "label" ld.ld_loc)
                 lds
           | Type_abstract _ | Type_open -> ())
      | Sig_typext (id, ec, _, _) -> p (prefix ^ Ident.name id) "ext" ec.ext_loc
      | Sig_module (id, _, md, _, _) ->
          let mp = prefix ^ Ident.name id in
          p mp "module" md.md_loc;
          (match md.md_type with
           | Mty_signature s -> walk (mp ^ ".") s
           | _ -> ())
      | Sig_modtype (id, mtd, _) ->
          let mp = prefix ^ Ident.name id in
          p mp "modtype" mtd.mtd_loc;
          (match mtd.mtd_type with
           | Some (Mty_signature s) -> walk (mp ^ ".") s
           | _ -> ())
      | Sig_class (id, _, _, _) -> Printf.printf "LOC %s class -\n" (prefix ^ Ident.name id)
      | Sig_class_type (id, _, _, _) ->
          Printf.printf "LOC %s classtype -\n" (prefix ^ Ident.name id))
    items

let () =
  let file = Sys.argv.(1) in
  match Cmi_format.read_cmi file with
  | cmi ->
      Format.set_margin 160;
      Printf.printf "NAME %s\n" cmi.Cmi_format.cmi_name;
      Format.printf "%a@." Printtyp.signature cmi.Cmi_format.cmi_sign;
      walk "" cmi.Cmi_format.cmi_sign
  | exception e ->
      prerr_endline (Printexc.to_string e);
      exit 3
