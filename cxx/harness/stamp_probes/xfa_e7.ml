module F (X : Set.OrderedType) = Set.Make (X)
module N : Set.S = F (Int)
