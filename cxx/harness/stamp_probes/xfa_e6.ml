module F (X : Set.OrderedType) = Set.Make (X)
include F (Int)
