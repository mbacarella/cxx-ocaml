type z = int
let sort (type s) set l =
  let module Set = (val set : Set.S with type elt = s) in Set.cardinal Set.empty
let mk (type s) cmp = let module S = Set.Make(struct type t = s
  let compare = cmp end) in (module S : Set.S with type elt = s)
let both l = let f set = sort set l in [f (mk compare); f (mk compare)]
