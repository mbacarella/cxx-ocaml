module F (X : Set.OrderedType) = struct class type ct = object method m : Set.Make(X).t end end
