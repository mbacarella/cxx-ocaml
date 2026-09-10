(* S427 probe: ctype's `subtype_row` fold (ctype.ml:6050) -- a two-column
   `row_field` match inside a List.fold_left closure whose FIRST row is a
   column-0 or-row with a real rest column (`Rpresent None`), and whose only
   other row compatible with it is the match's own wildcard catch-all.  With
   the catch-all peeled into the default, do_split's NO list is EMPTY, which
   attempt_split treated as a bail: the or-row was exploded in place and its
   rest column re-tested in every alternative's cell.  Upstream hands
   precompile_or the incoming default environment and gives the or-row a
   handler of its own (rest test -> action, miss -> the catch-all).
   Pins: 0/0; 22/32 under NOORSNEMPTY=1. *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

exception Err of string * int

let sub env trace t1 t2 c = if t1 < t2 then c + env + trace else c

let sr env trace pairs c0 =
  List.fold_left
    (fun c (l, f1, f2) ->
      match f1, f2 with
        (Rpresent None | Reither (true, _, _)), Rpresent None -> c
      | Rpresent (Some t1), Rpresent (Some t2) ->
          sub env (trace + 1) t1 t2 c
      | Reither (false, t1 :: _, _), Rpresent (Some t2) ->
          sub env (trace + 1) t1 t2 c
      | Rabsent, _ -> c
      | Rpresent None, Rpresent (Some _)
      | Rpresent (Some _), Rpresent None -> raise (Err (l, c))
      | _ -> raise Exit)
    c0 pairs

let all =
  [ Rpresent None; Rpresent (Some 5); Rabsent;
    Reither (true, [], false); Reither (false, [], true);
    Reither (true, [7], false); Reither (false, [7; 8], true);
    Reither (true, [7], true); Reither (false, [], false) ]

let () =
  List.iter (fun a -> List.iter (fun b ->
      (match sr 1 2 [("x", a, b)] 0 with
       | n -> print_string (string_of_int n)
       | exception Err (l, c) -> print_string (l ^ string_of_int c)
       | exception Exit -> print_string "E");
      print_char ' ') all;
    print_newline ()) all
