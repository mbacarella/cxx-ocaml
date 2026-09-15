module F (X : Set.OrderedType) = struct type t = Set.Make(X).t let f (x : t) = x end
