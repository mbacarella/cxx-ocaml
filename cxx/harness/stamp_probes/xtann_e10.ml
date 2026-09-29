module F (X : Set.OrderedType) = struct let f x = let y : Set.Make(X).t = x in y end
