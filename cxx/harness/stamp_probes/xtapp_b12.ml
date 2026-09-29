module F (X : Set.OrderedType) = struct type t = Set.Make(X).t end
module Y = struct type t = int let compare = compare end
module Z = F(Y)
