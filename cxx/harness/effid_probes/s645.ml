(* S425 probe: the `Reither` cell of ctype's eqtype_row (ctype.ml:5170)
   alone.  The division `{Reither _, Reither _ -> 6; Reither _, Rpresent _ ->
   7}` is reached from `case tag 0` cells and from `case tag 1` cells -- the
   arrivals prove DIFFERENT constructors for f2, so no single fact survives
   their intersection, but their union is {Rpresent, Reither} and upstream's
   Context.lub gives the `Rabsent` gap no clause at all.  NOCTXLUB=1
   reverts. *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

let g f1 f2 =
  match f1, f2 with
  | Reither (c1, [], _), Reither (c2, [], _) when c1 = c2 -> 3
  | Reither (c1, t1 :: tl1, _), Reither (c2, t2 :: tl2, _) when c1 = c2 ->
      4 + t1 + t2 + List.length tl1 + List.length tl2
  | Reither _, Reither _ -> 6
  | Reither _, Rpresent _ -> 7
  | (Rpresent _ | Reither _), Rabsent -> 10
  | _ -> 0

let all =
  [ Rpresent None; Rpresent (Some 5); Rabsent;
    Reither (true, [], false); Reither (false, [], true);
    Reither (true, [7], false); Reither (false, [7; 8], true) ]

let () =
  List.iter (fun a -> List.iter (fun b ->
                          print_string (string_of_int (g a b));
                          print_char ' ') all;
              print_newline ())
    all
