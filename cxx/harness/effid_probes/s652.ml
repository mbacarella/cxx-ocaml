(* S430 probe: ctype's `moregen_row` field loop (ctype.ml:4776-4841) -- the
   LAST divergent hunk in the corpus.  Same two-column `row_field` matrix as
   eqtype_row (s649), but the three `when may_inst` rows sit BETWEEN the
   guardless Reither/Reither row and the mismatch rows, and the arms raise
   rather than compute.  Ours re-tests f2 with `ACC1 ISINT BRANCHIF` right
   before the `Reither _, Rpresent _` arm (ctype dump ~9580) where upstream
   falls straight into it. *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

exception Err of string * int

let eq_ext f1 f2 = f1 == f2
let link _ _ = ()
let mg env t1 t2 = if t1 = t2 then () else raise (Err ("mg", t1 + t2))

let mr env may_inst l f1 f2 =
  if f1 == f2 then () else
  match f1, f2 with
  | Rpresent (Some t1), Rpresent (Some t2) -> begin
      try mg env t1 t2 with Err (_, n) -> raise (Err ("inc" ^ l, n))
    end
  | Rpresent None, Rpresent None -> ()
  | Reither (c1, tl1, _), Reither (c2, tl2, m2) -> begin
      try
        if not (eq_ext f1 f2) then begin
          if c1 && not c2 then raise (Err ("unex", 0));
          link f1 (Reither (c2, [], m2));
          if List.length tl1 = List.length tl2 then
            List.iter2 (mg env) tl1 tl2
          else match tl2 with
            | t2 :: _ -> List.iter (fun t1 -> mg env t1 t2) tl1
            | [] -> if tl1 <> [] then raise (Err ("unex", 1))
        end
      with Err (_, n) -> raise (Err ("inc" ^ l, n))
    end
  | Reither (false, tl1, _), Rpresent (Some t2) when may_inst -> begin
      try link f1 f2; List.iter (fun t1 -> mg env t1 t2) tl1
      with Err (_, n) -> raise (Err ("inc" ^ l, n))
    end
  | Reither (true, [], _), Rpresent None when may_inst -> link f1 f2
  | Reither (_, _, _), Rabsent when may_inst -> link f1 f2
  | Rabsent, Rabsent -> ()
  | Rpresent (Some _), Rpresent None
  | Rpresent None, Rpresent (Some _) -> raise (Err ("inc" ^ l, 0))
  | Reither _, Rpresent _ -> raise (Err ("png1" ^ l, 0))
  | Rpresent _, Reither _ -> raise (Err ("png2" ^ l, 1))
  | Rabsent, (Rpresent _ | Reither _) -> raise (Err ("nt1" ^ l, 2))
  | (Rpresent _ | Reither _), Rabsent -> raise (Err ("nt2" ^ l, 3))

let all =
  [ Rpresent None; Rpresent (Some 5); Rpresent (Some 7); Rabsent;
    Reither (true, [], false); Reither (false, [], true);
    Reither (true, [7], false); Reither (false, [7; 8], true) ]

let () =
  List.iter (fun mi ->
      List.iter (fun a -> List.iter (fun b ->
          (match mr 1 mi "x" a b with
           | () -> print_char '.'
           | exception Err (s, n) -> print_string (s ^ string_of_int n));
          print_char ' ') all;
        print_newline ()) all)
    [ false; true ]
