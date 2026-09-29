

(* Checking that the optimisation for avoiding capture
   of simple aliases in non-syntactic recursive function definitions
   works properly *)

(* Original example from issue number 14876 *)
type expr = Lit of int | Many of expr list

let rec depth : expr -> int =
    let __0 = depth in
    function
    | Lit _ -> 0
    | Many l ->
      List.fold_left (fun acc b -> Int.max acc (__0 b)) 0 l
