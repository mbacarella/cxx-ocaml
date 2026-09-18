module F (X : Set.OrderedType) = Set.Make (X)
module N = F (Stdlib__Int)
