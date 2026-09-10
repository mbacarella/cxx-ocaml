(* S425 probe: the `Rpresent` cell of ctype's eqtype_row (ctype.ml:5170)
   alone.  After the f1 switch the row `Rpresent _, Reither _ -> 8` is a
   division of its own, reached only from the two `case tag 1` cells, and its
   handler is the bare arm exit.  Erasing that alias catch before the arm is
   wired gives arm 8 two sites and sends its catch to the match's root, where
   upstream keeps `(catch .. with (n) 8)` around the `if` that shares it.
   NOARMALIAS=1 reverts. *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

let f f1 f2 =
  match f1, f2 with
  | Rpresent (Some t1), Rpresent (Some t2) -> 1 + t1 + t2
  | Rpresent None, Rpresent None -> 2
  | Rpresent (Some _), Rpresent None
  | Rpresent None, Rpresent (Some _) -> 6
  | Rpresent _, Reither _ -> 8
  | (Rpresent _ | Reither _), Rabsent -> 10
  | _ -> 0

let all =
  [ Rpresent None; Rpresent (Some 5); Rabsent;
    Reither (true, [], false); Reither (false, [], true);
    Reither (true, [7], false); Reither (false, [7; 8], true) ]

let () =
  List.iter (fun a -> List.iter (fun b ->
                          print_string (string_of_int (f a b));
                          print_char ' ') all;
              print_newline ())
    all
