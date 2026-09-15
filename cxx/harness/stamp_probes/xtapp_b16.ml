module F (X : Set.OrderedType) = struct class c = object method m : Set.Make(X).t option = None end end
