module F (X : Set.OrderedType) = Set.Make (X)
module G (Y : Set.OrderedType) = Set.Make (Y)
module N = F (Int)
module N2 = G (Int)
