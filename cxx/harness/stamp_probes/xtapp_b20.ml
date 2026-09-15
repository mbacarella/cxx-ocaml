module F (X : Set.OrderedType) = struct module S = Set.Make(X) type t = S.t end
