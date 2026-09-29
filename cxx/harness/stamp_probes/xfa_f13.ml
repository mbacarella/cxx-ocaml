module F (X : Set.OrderedType) = Set.Make (X)
module G (X : Set.OrderedType) = Set.Make (X)
module N = F (Int)
module N2 = G (String)
