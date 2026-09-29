(* ppx_tests NAME infile outfile: Ppx_tests' mapper NAME as a -ppx
   rewriter, or a misbehaving rewriter:
     fail      exits 1 (Pparse: Error while running external preprocessor)
     notfound  exits 127, as sh does for a missing command (Sys_error)
     nooutput  writes no output file
     junk      writes a file without the AST magic *)
let () =
  let n = Array.length Sys.argv in
  let name = if n >= 4 then Sys.argv.(n - 3) else "" in
  match name with
  | "fail" -> exit 1
  | "notfound" -> exit 127
  | "nooutput" -> exit 0
  | "junk" ->
      let oc = open_out_bin Sys.argv.(n - 1) in
      output_string oc "not an AST\n";
      close_out oc
  | _ ->
      Ast_mapper.run_main (function
        | [ name ] -> Ppx_tests.mapper name
        | _ -> failwith "ppx_tests: expected one mapper name")
