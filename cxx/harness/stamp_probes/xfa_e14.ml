module F (X : Set.OrderedType) = Set.Make (X)
module F2 = F
module N = F2 (Int)
