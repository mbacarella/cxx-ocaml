module F (X : Set.OrderedType) = Set.Make (X)
module N = F (Int)
module type T = module type of N
