(* S428 probe: ctype's `eqtype_row` cell (ctype.ml:5170-5214) with the REAL
   try/with arm bodies -- s642 (the same matrix with constant bodies) is 0/0,
   so the bodies are what reproduces the 11k residual: upstream binds the
   `t1 :: tl1` cell's fields AT THEIR LEVEL (above the f2 switch), and t2/tl2
   sink below the guard. *)
type rf = Rpresent of int option | Reither of bool * int list * bool | Rabsent

exception Trace of int list

let eq (a : int) (b : int) = if a = b then () else raise (Trace [a; b])

let eqr l f1 f2 =
  if f1 == f2 then () else
  match f1, f2 with
  | Rpresent (Some t1), Rpresent (Some t2) -> begin
      try eq t1 t2
      with Trace trace -> raise (Trace (l :: trace))
    end
  | Rpresent None, Rpresent None -> ()
  | Reither (c1, [], _), Reither (c2, [], _) when c1 = c2 -> ()
  | Reither (c1, t1 :: tl1, _), Reither (c2, t2 :: tl2, _) when c1 = c2 ->
      begin
        try
          eq t1 t2;
          if List.length tl1 = List.length tl2 then
            List.iter2 eq tl1 tl2
          else begin
            List.iter (eq t1) tl2;
            List.iter (fun t1 -> eq t1 t2) tl1
          end
        with Trace trace -> raise (Trace (l :: trace))
      end
  | Rabsent, Rabsent -> ()
  | Rpresent (Some _), Rpresent None
  | Rpresent None, Rpresent (Some _)
  | Reither _, Reither _ -> raise (Trace [l; 6])
  | Reither _, Rpresent _ -> raise (Trace [l; 7])
  | Rpresent _, Reither _ -> raise (Trace [l; 8])
  | Rabsent, (Rpresent _ | Reither _) -> raise (Trace [l; 9])
  | (Rpresent _ | Reither _), Rabsent -> raise (Trace [l; 10])

let all =
  [ Rpresent None; Rpresent (Some 5); Rabsent;
    Reither (true, [], false); Reither (false, [], true);
    Reither (true, [7], false); Reither (false, [7; 8], true);
    Reither (true, [7; 9], false) ]

let () =
  List.iteri (fun i a -> List.iter (fun b ->
      (match eqr i a b with
       | () -> print_string "ok"
       | exception Trace l ->
           print_string (String.concat "." (List.map string_of_int l)));
      print_char ' ') all;
    print_newline ())
    all
