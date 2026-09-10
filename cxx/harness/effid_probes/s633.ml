(* S420 CONTROL (morematch's flatguard): a guarded root-or row followed by a
   COMPATIBLE or-row.  Or_matrix cannot append it, so it becomes a following
   sub-matrix and the guard-fail raises THAT entry, not the match default.
   Sending the fall-out to the default answers 3 for (2,3) -- a miscompile. *)
let flatguard c =
  let x, y = c in
  match x, y with
  | (1,2)|(2,3) when y = 2 -> 1
  | (1,_)|(_,3) -> 2
  | _ -> 3
let () =
  List.iter (fun p -> print_int (flatguard p); print_newline ())
    [ (1,2); (1,3); (2,3); (2,4); (3,3); (7,9) ]
