module F (X : Set.OrderedType) = Set.Make (X)
module N = F (struct type t = int let compare = compare end)
module N2 = F (Int)
