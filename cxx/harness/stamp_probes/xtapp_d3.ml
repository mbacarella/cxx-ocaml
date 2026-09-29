module F (X : Set.OrderedType) (Y : Set.OrderedType) = struct type t = Set.Make(X).t * Set.Make(Y).t end
