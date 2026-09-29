module F (X : Set.OrderedType) = Set.Make (X)
module N = F (Int)
module N2 = F (struct type t = int let compare = compare end)
module N3 = F (String)
