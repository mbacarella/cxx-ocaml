module F (X : Set.OrderedType) = Set.Make (X)
let x = let module N = F (struct type t = int let compare = compare end) in
  N.cardinal N.empty
