(* a functor RESULT's members, cited from this unit's inferred values *)
module S = Set.Make (Int)
let e : S.t = S.empty
let n = S.cardinal e
let x : S.elt = 1
