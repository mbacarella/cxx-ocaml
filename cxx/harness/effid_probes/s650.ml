(* S428 probe: s648 (ctype's `subtype_row` fold, ctype.ml:6050) with the REAL
   arm bodies -- `Diff {got = t1; expected = t2} :: trace` reads t1 TWICE, so
   the `Some t1` payload's alias let survives Simplif: upstream binds it at
   its level, above the f2 switch (the exit carrying t1 sits under the
   branch's own `if *match*` on the `Some t2` cell). *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

type diff = Diff of { got : int; expected : int }

exception Err of string * int

let sub env trace t1 t2 c =
  if t1 < t2 then c + env + List.length trace else c

let sr env trace pairs c0 =
  List.fold_left
    (fun c (l, f1, f2) ->
      match f1, f2 with
        (Rpresent None | Reither (true, _, _)), Rpresent None -> c
      | Rpresent (Some t1), Rpresent (Some t2) ->
          sub env (Diff { got = t1; expected = t2 } :: trace) t1 t2 c
      | Reither (false, t1 :: _, _), Rpresent (Some t2) ->
          sub env (Diff { got = t1; expected = t2 } :: trace) t1 t2 c
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
      (match sr 1 [Diff { got = 0; expected = 0 }] [("x", a, b)] 0 with
       | n -> print_string (string_of_int n)
       | exception Err (l, c) -> print_string (l ^ string_of_int c)
       | exception Exit -> print_string "E");
      print_char ' ') all;
    print_newline ()) all
