module F (X : Set.OrderedType) = Set.Make (X)
module N : Set.S = F (struct type t = int let compare = compare end)
