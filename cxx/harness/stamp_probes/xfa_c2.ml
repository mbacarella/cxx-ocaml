module F (X : Set.OrderedType) = Set.Make (X)
module N2 = Set.Make (Int)
module N = F (Int)
