module X = struct type t = int let compare = compare end
module F (A : Set.OrderedType) = struct type t = int end
type t = F(X).t
