module F (X : Set.OrderedType) = struct let f (x : Set.Make(X).t) = (x :> Set.Make(X).t) end
