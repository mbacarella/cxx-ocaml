module F (X : Set.OrderedType) = Set.Make (X)
module N = F (String)
