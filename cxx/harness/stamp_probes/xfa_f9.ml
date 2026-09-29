module F (X : Set.OrderedType) (Y : Set.OrderedType) = Set.Make (X)
module N = F (Int)
