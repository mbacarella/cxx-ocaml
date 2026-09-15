module type T = sig module X : Set.OrderedType type t = Set.Make(X).t end
