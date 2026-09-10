(* S430 probe: ctype's `mcomp` (ctype.ml:2911-2927) -- the LAST divergent line
   in the corpus.  The row `(Tconstr _, Tconstr _)` above takes every value
   with BOTH heads, so inside the `t1 = Tconstr` cell of a later row t2 can no
   longer be a Tconstr: upstream sends the `tl1 <> []` miss of `(Tconstr (_,
   [], _), _) when g2` straight past `(_, Tconstr (_, [], _)) when g1` to the
   `(Tconstr _, _) | (_, Tconstr _)` row, where we re-dispatch t2 first.
   The conjunctive negative fact is GmNeg2's. *)
type d =
  | Dvar of int option
  | Darrow of string * d * d
  | Dtuple of d list
  | Dconstr of int * d list * d
  | Dobject of d
  | Dfield of string * d
  | Dnil
  | Dlink of d
  | Dsubst of d
  | Duniv of int
  | Dpoly of d * d list
  | Dpackage of int * d list

exception Incompatible

let inj a = a > 0
let dnum = function
  | Dvar _ -> 1 | Darrow _ -> 2 | Dtuple _ -> 3 | Dconstr _ -> 4
  | Dobject _ -> 5 | Dfield _ -> 6 | Dnil -> 7 | Dlink _ -> 8
  | Dsubst _ -> 9 | Duniv _ -> 10 | Dpoly _ -> 11 | Dpackage _ -> 12

let mc env t1 t2 =
  match t1, t2 with
  | Dvar _, _ | _, Dvar _ -> 0
  | Darrow (l1, a1, b1), Darrow (l2, a2, b2) when l1 = l2 ->
      dnum a1 + dnum a2 + dnum b1 + dnum b2
  | Dtuple tl1, Dtuple tl2 -> List.length tl1 + List.length tl2
  | Dconstr (p1, tl1, _), Dconstr (p2, tl2, _) ->
      p1 + p2 + List.length tl1 + List.length tl2
  | Dconstr (_, [], _), _ when inj (dnum t2) -> raise Incompatible
  | _, Dconstr (_, [], _) when inj (dnum t1) -> raise Incompatible
  | Dconstr (p, _, _), _ | _, Dconstr (p, _, _) ->
      if p > 3 then raise Incompatible else p
  | Dpoly (u1, _), Dpoly (u2, _) -> dnum u1 + dnum u2
  | _ -> raise Incompatible

let all =
  [ Dvar None; Darrow ("a", Dnil, Dnil); Dtuple [ Dnil ];
    Dconstr (1, [], Dnil); Dconstr (5, [ Dnil ], Dnil); Dobject Dnil;
    Dfield ("f", Dnil); Dnil; Dlink Dnil; Dsubst Dnil; Duniv 1;
    Dpoly (Dnil, []); Dpackage (1, []) ]

let () =
  List.iter (fun a ->
      List.iter (fun b ->
          (match mc 0 a b with
           | n -> print_string (string_of_int n)
           | exception Incompatible -> print_char 'I');
          print_char ' ') all;
      print_newline ()) all
