module F (X : Set.OrderedType) = Map.Make (X)
module N = F (struct type t = int let compare = compare end)
