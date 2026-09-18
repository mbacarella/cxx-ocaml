module F (X : Set.OrderedType) = Set.Make (X)
module A = Int
module N = F (A)
