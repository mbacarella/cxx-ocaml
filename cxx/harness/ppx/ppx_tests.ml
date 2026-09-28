(* Test rewriters for ppx_parity.sh, on compiler-libs' Ast_mapper:
     id      the identity (the default mapper: every node rebuilt)
     ext     [%answer] -> 42, [%double e] -> e + e, [%twice e] -> (e, e)
             with one [e] node twice (a shared subtree), [%%item] -> let
             generated = 1
     derive  [@@deriving name] on a type -> let name_of_<t> = "<t>"
     attr    [@ocaml.warning "-26-27"] on every let binding, and an
             unknown attribute on every structure item
     ghost   every location made ghost
     copy    every location and position a fresh record of equal value
             (as ppxlib's AST migration copies them)
     none    every location Location.none
     bad     an ill-formed AST (a one-element tuple) for Ast_invariants *)

open Parsetree
open Ast_helper

let loc_mapper f =
  { Ast_mapper.default_mapper with location = (fun _ l -> f l) }

let ext_mapper =
  let super = Ast_mapper.default_mapper in
  let expr self e =
    match e.pexp_desc with
    | Pexp_extension ({ txt = "answer"; loc }, PStr []) ->
        Exp.constant ~loc (Const.int 42)
    | Pexp_extension ({ txt = "double"; loc }, PStr [ { pstr_desc = Pstr_eval (x, _); _ } ]) ->
        let x = self.Ast_mapper.expr self x in
        Exp.apply ~loc (Exp.ident ~loc { txt = Longident.Lident "+"; loc }) [ (Nolabel, x); (Nolabel, x) ]
    | Pexp_extension ({ txt = "twice"; loc }, PStr [ { pstr_desc = Pstr_eval (x, _); _ } ]) ->
        let x = self.Ast_mapper.expr self x in
        Exp.tuple ~loc [ (None, x); (None, x) ]
    | _ -> super.expr self e
  in
  let structure_item self si =
    match si.pstr_desc with
    | Pstr_extension (({ txt = "item"; loc }, PStr []), _) ->
        Str.value ~loc Asttypes.Nonrecursive
          [ Vb.mk ~loc (Pat.var ~loc { txt = "generated"; loc }) (Exp.constant ~loc (Const.int 1)) ]
    | _ -> super.structure_item self si
  in
  { super with expr; structure_item }

let derive_mapper =
  let super = Ast_mapper.default_mapper in
  let structure self items =
    List.concat_map
      (fun si ->
        let si = self.Ast_mapper.structure_item self si in
        match si.pstr_desc with
        | Pstr_type (_, decls) ->
            let extra =
              List.filter_map
                (fun td ->
                  if List.exists (fun a -> a.attr_name.txt = "deriving") td.ptype_attributes then
                    let loc = { td.ptype_loc with loc_ghost = true } in
                    let name = td.ptype_name.txt in
                    Some
                      (Str.value ~loc Asttypes.Nonrecursive
                         [ Vb.mk ~loc (Pat.var ~loc { txt = "name_of_" ^ name; loc }) (Exp.constant ~loc (Const.string name)) ])
                  else None)
                decls
            in
            si :: extra
        | _ -> [ si ])
      items
  in
  { super with structure }

let attr_mapper =
  let super = Ast_mapper.default_mapper in
  let warn loc =
    Attr.mk ~loc { txt = "ocaml.warning"; loc } (PStr [ Str.eval ~loc (Exp.constant ~loc (Const.string "-26-27")) ])
  in
  let value_binding self vb =
    let vb = super.value_binding self vb in
    { vb with pvb_attributes = warn vb.pvb_loc :: vb.pvb_attributes }
  in
  let structure self items =
    List.concat_map
      (fun si ->
        let si = self.Ast_mapper.structure_item self si in
        [ si; Str.attribute ~loc:si.pstr_loc (Attr.mk ~loc:si.pstr_loc { txt = "ppx_tests.mark"; loc = si.pstr_loc } (PStr [])) ])
      items
  in
  { super with value_binding; structure }

let bad_mapper =
  let super = Ast_mapper.default_mapper in
  let expr self e =
    match e.pexp_desc with
    | Pexp_extension ({ txt = "bad"; loc }, _) -> Exp.tuple ~loc [ (None, Exp.constant ~loc (Const.int 1)) ]
    | _ -> super.expr self e
  in
  { super with expr }

let mapper = function
  | "id" -> Ast_mapper.default_mapper
  | "ext" -> ext_mapper
  | "derive" -> derive_mapper
  | "attr" -> attr_mapper
  | "ghost" -> loc_mapper (fun l -> { l with loc_ghost = true })
  | "copy" ->
      let pos (p : Lexing.position) = { p with pos_lnum = p.pos_lnum } in
      loc_mapper (fun l -> { loc_start = pos l.loc_start; loc_end = pos l.loc_end; loc_ghost = l.loc_ghost })
  | "none" -> loc_mapper (fun _ -> Location.none)
  | "bad" -> bad_mapper
  | s -> failwith ("ppx_tests: unknown mapper " ^ s)
