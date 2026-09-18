module F (X : Set.OrderedType) = Set.Make (X)
module G (Y : Set.OrderedType) = F (Y)
module M = G (Int)
