module W = struct module F ( X : Set.OrderedType ) = struct module N : Set.S
  with type elt = X.t = Set.Make( X ) type u = N.t end end
