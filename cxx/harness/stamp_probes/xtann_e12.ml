module F (X : Set.OrderedType) = struct class c = object val v : Set.Make(X).t option = None end end
