(* S423 probe: ctype's `mcomp_row` (ctype.ml:3024) -- a two-column match over
   `row_field` whose FIRST row is a four-alternative root-or with nested ors
   inside the alternatives' columns.  No guard anywhere, so `lead_free` is not
   what withholds precompile_or's handler here. *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

let mc f1 f2 =
  match f1, f2 with
  | Rpresent None, (Rpresent (Some _) | Reither (_, _::_, _) | Rabsent)
  | Rpresent (Some _), (Rpresent None | Reither (true, _, _) | Rabsent)
  | (Reither (_, _::_, _) | Rabsent), Rpresent None
  | (Reither (true, _, _) | Rabsent), Rpresent (Some _) -> 1
  | Rpresent (Some t1), Rpresent (Some t2) -> 2 + t1 + t2
  | Rpresent (Some t1), Reither (false, tl2, _) -> 3 + t1 + List.length tl2
  | Reither (false, tl1, _), Rpresent (Some t2) -> 4 + t2 + List.length tl1
  | _ -> 0

let all =
  [ Rpresent None; Rpresent (Some 5); Rabsent;
    Reither (true, [], false); Reither (false, [], true);
    Reither (true, [7], false); Reither (false, [7; 8], true) ]

let () =
  List.iter (fun a -> List.iter (fun b -> print_string (string_of_int (mc a b)))
                        all;
              print_newline ())
    all
