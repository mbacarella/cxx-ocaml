type z = int
let sort (type s) set l =
  let module Set = (val set : Set.S with type elt = s) in Set.cardinal Set.empty
let mk (type s) cmp = let module S = Set.Make(struct type t = s
  let compare = cmp end) in (module S : Set.S with type elt = s)
let both (type s) l (x : (module Set.S with type elt = s)) =
  List.map (fun set -> sort set l) [ x ]
