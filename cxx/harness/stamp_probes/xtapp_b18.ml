module F (X : Set.OrderedType) = struct type t = Stdlib.Set.Make(X).t end
