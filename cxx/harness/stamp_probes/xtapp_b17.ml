module F (X : Set.OrderedType) = struct exception E of Set.Make(X).t end
