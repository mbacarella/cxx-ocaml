module F (X : Set.OrderedType) = Set.Make (X)
module A = struct type t = int let compare = compare end
module N = F (A)
module N2 = F (A)
