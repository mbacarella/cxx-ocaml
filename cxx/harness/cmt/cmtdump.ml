(* Structural dump of a .cmt / .cmti (the cmt_parity.sh oracle): every
   cmt_infos field, the typed tree through Printtyped, the uid_to_decl
   table in its own bucket order.  Built against the tree's compiler-libs. *)
open Cmt_format

let pr fmt = Printf.printf fmt

let decl_kind = function
  | Typedtree.Value _ -> "Value"
  | Value_binding _ -> "Value_binding" | Type _ -> "Type"
  | Constructor _ -> "Constructor"
  | Extension_constructor _ -> "Extension_constructor" | Label _ -> "Label"
  | Module _ -> "Module" | Module_substitution _ -> "Module_substitution"
  | Module_binding _ -> "Module_binding" | Module_type _ -> "Module_type"
  | Class _ -> "Class" | Class_type _ -> "Class_type"

let uid u = Format.asprintf "%a" Shape.Uid.print u
let loc l = Format.asprintf "%a" Location.print_loc l
let digest = function None -> "None" | Some d -> Digest.to_hex d

let annots = function
  | Implementation s -> pr "Implementation\n%s" (Format.asprintf "%a" Printtyped.implementation s)
  | Interface s -> pr "Interface\n%s" (Format.asprintf "%a" Printtyped.interface s)
  | Packed (sg, files) ->
      pr "Packed [%s]\n%s\n" (String.concat "; " files)
        (Format.asprintf "%a" Printtyp.signature sg)
  | Partial_implementation parts | Partial_interface parts ->
      pr "Partial (%d parts)\n" (Array.length parts);
      Array.iter (function
        | Partial_structure s -> pr "- structure\n%s" (Format.asprintf "%a" Printtyped.implementation s)
        | Partial_structure_item s ->
            pr "- structure_item\n%s" (Format.asprintf "%a" Printtyped.implementation
              { Typedtree.str_items = [s]; str_type = []; str_final_env = Env.empty })
        | Partial_expression _ -> pr "- expression\n"
        | Partial_pattern _ -> pr "- pattern\n"
        | Partial_class_expr _ -> pr "- class_expr\n"
        | Partial_signature s -> pr "- signature\n%s" (Format.asprintf "%a" Printtyped.interface s)
        | Partial_signature_item _ -> pr "- signature_item\n"
        | Partial_module_type _ -> pr "- module_type\n") parts

let () =
  let f = Sys.argv.(1) in
  let cmi, cmt = Cmt_format.read f in
  (match cmi with
   | None -> pr "cmi: none\n"
   | Some c -> pr "cmi: %s (%d crcs)\n" c.Cmi_format.cmi_name (List.length c.cmi_crcs));
  match cmt with
  | None -> pr "no cmt\n"
  | Some c ->
      pr "modname: %s\n" c.cmt_modname;
      pr "args: [%s]\n" (String.concat "; " (Array.to_list c.cmt_args));
      pr "sourcefile: %s\n" (Option.value ~default:"None" c.cmt_sourcefile);
      pr "builddir: %s\n" c.cmt_builddir;
      pr "loadpath: visible [%s] hidden [%s]\n"
        (String.concat "; " c.cmt_loadpath.visible) (String.concat "; " c.cmt_loadpath.hidden);
      pr "source_digest: %s\n" (digest c.cmt_source_digest);
      pr "interface_digest: %s\n" (digest c.cmt_interface_digest);
      pr "use_summaries: %b\n" c.cmt_use_summaries;
      pr "imports:\n";
      List.iter (fun (n, d) -> pr "  %s %s\n" n (digest d)) c.cmt_imports;
      pr "comments:\n";
      List.iter (fun (s, l) -> pr "  %S %s\n" s (loc l)) c.cmt_comments;
      pr "declaration_dependencies:\n";
      List.iter (fun (k, a, b) ->
          pr "  %s %s %s\n"
            (match k with Shape.Uid.Deps.Definition_to_declaration -> "Def->Decl"
                        | Declaration_to_declaration -> "Decl->Decl")
            (uid a) (uid b)) c.cmt_declaration_dependencies;
      pr "uid_to_decl (%d):\n" (Shape.Uid.Tbl.length c.cmt_uid_to_decl);
      Shape.Uid.Tbl.iter (fun u d -> pr "  %s %s\n" (uid u) (decl_kind d)) c.cmt_uid_to_decl;
      pr "impl_shape: %s\n"
        (match c.cmt_impl_shape with None -> "None" | Some s -> Format.asprintf "%a" Shape.print s);
      pr "ident_occurrences (%d):\n" (List.length c.cmt_ident_occurrences);
      List.iter (fun (lid, r) ->
          pr "  %s %s %s\n" (Format.asprintf "%a" Pprintast.longident lid.Location.txt) (loc lid.loc)
            (Format.asprintf "%a" Shape_reduce.print_result r)) c.cmt_ident_occurrences;
      pr "initial_env summary: %s\n"
        (match Env.summary c.cmt_initial_env with Env.Env_empty -> "empty" | _ -> "non-empty");
      annots c.cmt_annots
