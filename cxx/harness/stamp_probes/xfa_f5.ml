module F (X : Set.OrderedType) : Set.S with type elt = X.t =
  Set.Make (X)
module N = F (Int)
