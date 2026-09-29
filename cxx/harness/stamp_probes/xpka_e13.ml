type z = int
module F (X : Set.OrderedType) = Set.Make (X)
let f () = let module S = F (Int) in
  (module S : Set.S with type elt = int)
