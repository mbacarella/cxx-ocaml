(* Emit c++ocamlc's driver tables from ocamlc's own compiler-libs values:
   Main_args.Make_bytecomp_options's option list (keys, Arg kinds, Symbol
   choices, docs, in ocamlc's order), Config.print_config's variables, and
   Warnings.descriptions (for -warn-help).  See gen_driver_tables.sh. *)
module O = Main_args.Make_bytecomp_options (Main_args.Default.Main)

let esc s =
  let b = Buffer.create (String.length s + 8) in
  String.iter (fun c -> match c with
    | '"' -> Buffer.add_string b "\\\""
    | '\\' -> Buffer.add_string b "\\\\"
    | '\n' -> Buffer.add_string b "\\n"
    | '\t' -> Buffer.add_string b "\\t"
    | c when Char.code c < 32 || Char.code c >= 127 ->
        Printf.bprintf b "\\%03o" (Char.code c)
    | c -> Buffer.add_char b c) s;
  Buffer.contents b

let kind = function
  | Arg.Unit _ -> "Unit" | Arg.Bool _ -> "Bool" | Arg.Set _ -> "Set"
  | Arg.Clear _ -> "Clear" | Arg.String _ -> "String"
  | Arg.Set_string _ -> "Set_string" | Arg.Int _ -> "Int"
  | Arg.Set_int _ -> "Set_int" | Arg.Float _ -> "Float"
  | Arg.Set_float _ -> "Set_float" | Arg.Tuple _ -> "Tuple"
  | Arg.Symbol _ -> "Symbol" | Arg.Rest _ -> "Rest"
  | Arg.Rest_all _ -> "Rest_all" | Arg.Expand _ -> "Expand"

let () =
  match Sys.argv with
  | [| _; "options" |] ->
      List.iter (fun (k, spec, doc) ->
        let syms = match spec with Arg.Symbol (l, _) -> l | _ -> [] in
        Printf.printf "    {\"%s\", K::%s, {%s}, \"%s\"},\n" (esc k) (kind spec)
          (String.concat ", "
             (List.map (fun s -> "\"" ^ esc s ^ "\"") syms))
          (esc doc))
        O.list
  | [| _; "config" |] ->
      let file = Filename.temp_file "config" "" in
      let oc = open_out_bin file in
      Config.print_config oc;
      close_out oc;
      let ic = open_in_bin file in
      (try
         while true do
           let line = input_line ic in
           let i = String.index line ':' in
           let name = String.sub line 0 i in
           let v = String.sub line (i + 2) (String.length line - i - 2) in
           Printf.printf "    {\"%s\", \"%s\"},\n" (esc name) (esc v)
         done
       with End_of_file -> ());
      close_in ic;
      Sys.remove file
  | [| _; "warnings" |] ->
      List.iter (fun { Warnings.number; names; description; since } ->
        Printf.printf "    {%d, {%s}, \"%s\", %s},\n" number
          (String.concat ", " (List.map (fun s -> "\"" ^ esc s ^ "\"") names))
          (esc description)
          (match since with
           | None -> "{}"
           | Some r -> Printf.sprintf "{{%d, %d}}" r.Sys.major r.Sys.minor))
        Warnings.descriptions
  | _ -> prerr_endline "usage: gen_driver_tables (options|config|warnings)"; exit 2
