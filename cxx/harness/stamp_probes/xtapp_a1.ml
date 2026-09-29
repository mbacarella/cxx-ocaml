module F (X : Set.OrderedType) = struct type t = Set.Make(X).t end
