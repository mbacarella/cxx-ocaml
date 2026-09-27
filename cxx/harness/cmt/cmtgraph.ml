(* The value graph of a .cmt / .cmti with Marshal's sharing: #n marks a
   block's first occurrence, @n a later one (a back-reference).  Prints the
   cmi part (when present) and the cmt_infos value field by field. *)
let seen : (int, (Obj.t * int) list) Hashtbl.t = Hashtbl.create 1024
let counter = ref 0
let rec dump b depth (o : Obj.t) =
  if Obj.is_int o then Buffer.add_string b (string_of_int (Obj.obj o : int))
  else
    let h = Hashtbl.hash o in
    let bucket = try Hashtbl.find seen h with Not_found -> [] in
    match List.assq_opt o bucket with
    | Some n -> Printf.bprintf b "@%d" n
    | None ->
      incr counter; let n = !counter in
      Hashtbl.replace seen h ((o, n) :: bucket);
      let t = Obj.tag o in
      if t = Obj.string_tag then Printf.bprintf b "#%d%S" n (Obj.obj o : string)
      else if t = Obj.double_tag then Printf.bprintf b "#%d%h" n (Obj.obj o : float)
      else if t = Obj.custom_tag then Printf.bprintf b "#%d<custom>" n
      else begin
        Printf.bprintf b "#%d[%d:" n t;
        for i = 0 to Obj.size o - 1 do
          if i > 0 then Buffer.add_char b ' ';
          if depth > 1_000_000 then Buffer.add_string b "..." else dump b (depth + 1) (Obj.field o i)
        done;
        Buffer.add_char b ']'
      end
let () =
  let ic = open_in_bin Sys.argv.(1) in
  let magic = really_input_string ic 12 in
  let b = Buffer.create 65536 in
  if magic = Config.cmi_magic_number then begin
    for _ = 1 to 3 do dump b 0 (input_value ic); Buffer.add_char b '\n' done;
    ignore (really_input_string ic 12)
  end;
  let v : Obj.t = input_value ic in
  for i = 0 to Obj.size v - 1 do
    Printf.bprintf b "field %d: " i; dump b 0 (Obj.field v i); Buffer.add_char b '\n'
  done;
  print_string (Buffer.contents b)
