(* S424 probe: the Reither cell of ctype's `unify_row_field` (ctype.ml:3782) on
   its own.  After the f1 switch the sub-matrix is four columns wide -- the
   Reither payload (c, tl, m) plus f2 -- and split_no_or divides it three ways:
   {row 1, row 2} | {row 3, row 4} | the or-row's alternative.  The middle
   division is used from ONE switch cell upstream, so simplify_exits inlines it
   there; we route the `Rabsent` cell's miss into it as well, and the catch
   survives. *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

let urf f1 f2 =
  match f1, f2 with
  | Reither (c1, tl1, m1), Reither (c2, tl2, m2) ->
      3 + (if c1 || c2 then 1 else 0) + (if m1 || m2 then 2 else 0)
      + List.length tl1 + List.length tl2
  | Reither (_, _, false), Rabsent -> 4
  | Reither (false, tl, _), Rpresent (Some t2) -> 7 + t2 + List.length tl
  | Reither (true, [], _), Rpresent None -> 9
  | (Rpresent _ | Reither (_, _, true)), Rabsent -> 12
  | _ -> 0

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
