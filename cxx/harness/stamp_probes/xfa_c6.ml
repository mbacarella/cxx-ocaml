module F (X : Set.OrderedType) = Set.Make (X)
module N2 = Set.Make (struct type t = int let compare = compare end)
module N = F (Int)
