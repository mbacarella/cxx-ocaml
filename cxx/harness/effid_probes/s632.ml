(* S420 PIN: a guarded ROOT-OR row that is NOT last, with every later row
   disjoint from it -- Or_matrix appends them all, so the or-handler's
   guard-fail really does raise the match's own default entry. *)
type d =
  | Constr of int * int list
  | Field of int * int * int
  | Nil
let g x = string_of_int x
let hh a b c = string_of_int (a + b + c)
let pat_mode () = Sys.opaque_identity true
let peq a b = a = (b : int)
let f d1 d2 =
  begin match (d1, d2) with
  | (Constr (p1, tl1), Constr (p2, tl2)) when peq p1 p2 ->
      g (List.length tl1 + List.length tl2)
  | (Constr (p1, []), Constr (p2, [])) when pat_mode () && p1 > 0 && p2 > 0 ->
      g (p1 + p2)
  | (Constr (_,_), _) | (_, Constr (_,_)) when pat_mode () -> g 1
  | (Field (fl, kind, rem), Nil) | (Nil, Field (fl, kind, rem)) -> hh fl kind rem
  | (_, _) -> "d"
  end
let () =
  List.iter (fun (a, b) -> print_string (f a b); print_newline ())
    [ (Constr (1, [7]), Constr (1, [8]));
      (Constr (2, []), Constr (3, []));
      (Constr (4, [1]), Nil);
      (Nil, Constr (5, []));
      (Field (1, 2, 3), Nil);
      (Nil, Field (4, 5, 6));
      (Nil, Nil) ]
