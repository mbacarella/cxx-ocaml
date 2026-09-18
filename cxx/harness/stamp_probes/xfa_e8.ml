module F (X : Set.OrderedType) = Set.Make (X)
module N : Set.S with type elt = int = F (Int)
