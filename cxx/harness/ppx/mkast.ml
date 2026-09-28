(* mkast [-intf] [-map NAME] SOURCE OUT: parse SOURCE with ocamlc's own
   parser (Pparse, as ocamlc does) and write it as a binary AST file
   (Pparse.write_ast), optionally through one of ppx_tests.ml's mappers --
   what a dune ppx driver hands ocamlc as `-impl foo.pp.ml`. *)

let () =
  let intf = ref false and map = ref "" and files = ref [] in
  Arg.parse
    [ "-intf", Arg.Set intf, " an interface";
      "-map", Arg.Set_string map, "NAME apply a mapper of Ppx_tests" ]
    (fun f -> files := f :: !files) "mkast [-intf] [-map NAME] SOURCE OUT";
  match List.rev !files with
  | [src; out] ->
      Location.input_name := src;
      let mapper = if !map = "" then Ast_mapper.default_mapper else Ppx_tests.mapper !map in
      if !intf then begin
        let ast = Pparse.parse_interface ~tool_name:"mkast" src in
        Pparse.write_ast Pparse.Signature out (mapper.Ast_mapper.signature mapper ast)
      end else begin
        let ast = Pparse.parse_implementation ~tool_name:"mkast" src in
        Pparse.write_ast Pparse.Structure out (mapper.Ast_mapper.structure mapper ast)
      end
  | _ -> prerr_endline "usage: mkast [-intf] [-map NAME] SOURCE OUT"; exit 2
