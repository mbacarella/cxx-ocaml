(* asttree FILE: a binary AST file's values as a tree (sharing unfolded) *)
let rec dump b (o : Obj.t) =
  if Obj.is_int o then Buffer.add_string b (string_of_int (Obj.obj o : int))
  else
    let t = Obj.tag o in
    if t = Obj.string_tag then Printf.bprintf b "%S" (Obj.obj o : string)
    else if t = Obj.double_tag then Printf.bprintf b "%h" (Obj.obj o : float)
    else begin
      Printf.bprintf b "[%d:" t;
      for i = 0 to Obj.size o - 1 do
        Buffer.add_char b ' ';
        dump b (Obj.field o i)
      done;
      Buffer.add_char b ']'
    end
let () =
  let ic = open_in_bin Sys.argv.(1) in
  let _magic = really_input_string ic 12 in
  let name : Obj.t = input_value ic in
  let ast : Obj.t = input_value ic in
  let b = Buffer.create 4096 in
  dump b name; Buffer.add_char b '\n';
  dump b ast;
  print_string (Buffer.contents b); print_newline ()
