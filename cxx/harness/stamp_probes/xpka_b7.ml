type z = int
module F (X : Set.OrderedType) = Set.Make (X)
module S = F (Int)
let f () = (module S : Set.S with type elt = int)
