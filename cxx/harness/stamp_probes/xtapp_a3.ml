module F (X : Set.OrderedType) = struct module XS = Set.Make(X) type t = Set.Make(X).t end
