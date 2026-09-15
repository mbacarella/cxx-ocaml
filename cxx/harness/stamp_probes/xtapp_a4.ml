module F (X : Set.OrderedType) = struct type t = Set.Make(X).t type u = Set.Make(X).t end
