module F (X : Set.OrderedType) = struct type t = Set.Make(X).t let f (x : Set.Make(X).t) = x end
