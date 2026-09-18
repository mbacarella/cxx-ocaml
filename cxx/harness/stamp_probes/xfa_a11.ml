module F (X : Set.OrderedType) = Set.Make (X)
module N = F (Int)
type u = N.t
