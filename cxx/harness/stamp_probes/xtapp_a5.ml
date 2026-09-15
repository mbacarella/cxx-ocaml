module F (X : Map.OrderedType) = struct type 'a t = 'a Map.Make(X).t end
