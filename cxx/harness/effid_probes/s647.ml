(* S426 probe, CARRIED (37/75): a tuple-level or-row followed by a wildcard
   row.  Upstream's column-level split_no_or (matching.ml:1616) ends the first
   division at row 4's omega head (`_, Rabsent` cannot group with a
   constructor discriminator, and every later row is compatible with it), so
   the or-row's exploded alternatives form a division of their own behind the
   `_, Rabsent` handler, then the wildcard row a third: the first switch's
   misses `(exit 6)` into `switch* f1 {tag 0: (if (field 0 f1) (exit 4) (exit
   5)); ..}` with the column-1 or-handlers (4)/(5) inside.  We fold the
   alternatives' tests into the first switch's cells.  Not the s641 tail's
   shape (that one is ctx-only and closed by S426); recorded for NEXT. *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

let urf f1 f2 =
  match f1, f2 with
    Rpresent (Some _), Rpresent (Some _) -> 1
  | Rpresent None, Rpresent None -> 2
  | Reither _, Reither _ -> 3
  | Rabsent, _ -> 6
  | _, Rabsent -> 12
  | (Rpresent (Some _) | Reither (false, _, _)),
    (Rpresent None | Reither (true, _, _))
  | (Rpresent None | Reither (true, _, _)),
    (Rpresent (Some _) | Reither (false, _, _)) -> 13
  | _ -> 14

let all =
  [ Rpresent None; Rpresent (Some 5); Rabsent;
    Reither (true, [], false); Reither (false, [], true);
    Reither (true, [7], false); Reither (false, [7; 8], true) ]

let () =
  List.iter (fun a -> List.iter (fun b ->
                          print_string (string_of_int (urf a b));
                          print_char ' ') all;
              print_newline ())
    all
