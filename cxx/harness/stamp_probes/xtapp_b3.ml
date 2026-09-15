module F (X : Set.OrderedType) = struct type v = C of Set.Make(X).t end
