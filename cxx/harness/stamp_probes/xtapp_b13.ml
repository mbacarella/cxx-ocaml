module F (X : Set.OrderedType) = struct module G (Y : Set.OrderedType) = struct type t = Set.Make(Y).t type u = Set.Make(X).t end end
