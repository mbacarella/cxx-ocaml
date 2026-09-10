(* S423 probe: ctype's `unify_row_field` (ctype.ml:3782) -- the same two-column
   `row_field` match, with the root-or arriving LAST (two alternatives, each
   with a nested or in both columns) behind ten simple rows and two
   single-column ors.  Again no guard, so `lead_free` is not the blocker. *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

let urf f1 f2 =
  match f1, f2 with
    Rpresent (Some t1), Rpresent (Some t2) -> 1 + t1 + t2
  | Rpresent None, Rpresent None -> 2
  | Reither (c1, tl1, m1), Reither (c2, tl2, m2) ->
      3 + (if c1 || c2 then 1 else 0) + (if m1 || m2 then 2 else 0)
      + List.length tl1 + List.length tl2
  | Reither (_, _, false), Rabsent -> 4
  | Rabsent, Reither (_, _, false) -> 5
  | Rabsent, Rabsent -> 6
  | Reither (false, tl, _), Rpresent (Some t2) -> 7 + t2 + List.length tl
  | Rpresent (Some t1), Reither (false, tl, _) -> 8 + t1 + List.length tl
  | Reither (true, [], _), Rpresent None -> 9
  | Rpresent None, Reither (true, [], _) -> 10
  | Rabsent, (Rpresent _ | Reither (_, _, true)) -> 11
  | (Rpresent _ | Reither (_, _, true)), Rabsent -> 12
  | (Rpresent (Some _) | Reither (false, _, _)),
    (Rpresent None | Reither (true, _, _))
  | (Rpresent None | Reither (true, _, _)),
    (Rpresent (Some _) | Reither (false, _, _)) -> 13
  | Reither (true, _ :: _, _), Rpresent _
  | Rpresent _, Reither (true, _ :: _, _) -> 14

let all =
  [ Rpresent None; Rpresent (Some 5); Rabsent;
    Reither (true, [], false); Reither (false, [], true);
    Reither (true, [7], false); Reither (false, [7; 8], true);
    Reither (true, [7], true); Reither (false, [], false) ]

let () =
  List.iter (fun a -> List.iter (fun b ->
                          print_string (string_of_int (urf a b));
                          print_char ' ') all;
              print_newline ())
    all
