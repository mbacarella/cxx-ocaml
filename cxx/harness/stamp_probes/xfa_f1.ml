module F (X : Set.OrderedType) = Map.Make (X)
module N = F (Int)
