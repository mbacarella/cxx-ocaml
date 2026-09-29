

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
;;

(* A few more complicated cases, with capture of renamed variables.
   Only [b] should end up as part of [foo]'s function context. *)

let rec foo =
  let x = foo in
  let y = x in
  let z = bar in
  let a = z in
  let b () = y (); a () in
  fun () -> x (); y (); z (); a (); b ()
and bar () = foo ()
