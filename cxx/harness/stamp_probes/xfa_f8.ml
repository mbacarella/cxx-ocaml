module F (X : Set.OrderedType) (Y : Set.OrderedType) = Set.Make (Y)
module N = F (Int) (String)
