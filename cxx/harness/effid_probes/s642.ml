(* S423 probe: ctype's `eqtype_row` (ctype.ml:5170) -- the twin of s640/s641,
   but with TWO GUARDED rows ahead of the root-or.  `lead_free` (lambda.cpp
   ~17245) refuses any guarded leading row, so precompile_or's handler is
   withheld here outright. *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

let eqr f1 f2 =
  match f1, f2 with
  | Rpresent (Some t1), Rpresent (Some t2) -> 1 + t1 + t2
  | Rpresent None, Rpresent None -> 2
  | Reither (c1, [], _), Reither (c2, [], _) when c1 = c2 -> 3
  | Reither (c1, t1 :: tl1, _), Reither (c2, t2 :: tl2, _) when c1 = c2 ->
      4 + t1 + t2 + List.length tl1 + List.length tl2
  | Rabsent, Rabsent -> 5
  | Rpresent (Some _), Rpresent None
  | Rpresent None, Rpresent (Some _)
  | Reither _, Reither _ -> 6
  | Reither _, Rpresent _ -> 7
  | Rpresent _, Reither _ -> 8
  | Rabsent, (Rpresent _ | Reither _) -> 9
  | (Rpresent _ | Reither _), Rabsent -> 10

let all =
  [ Rpresent None; Rpresent (Some 5); Rabsent;
    Reither (true, [], false); Reither (false, [], true);
    Reither (true, [7], false); Reither (false, [7; 8], true) ]

let () =
  List.iter (fun a -> List.iter (fun b ->
                          print_string (string_of_int (eqr a b));
                          print_char ' ') all;
              print_newline ())
    all
