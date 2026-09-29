module F (X : Set.OrderedType) = struct include Set.Make(X) type u = Set.Make(X).t end
