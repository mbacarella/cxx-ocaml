module F (X : Set.OrderedType) = struct module S = Set.Make(X) let x = S.empty end
