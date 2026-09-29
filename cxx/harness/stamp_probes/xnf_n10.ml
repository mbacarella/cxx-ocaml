module W = struct module F ( X : Set.OrderedType ) = struct module N : Set.S
  with type elt = X.t = Set.Make( X ) let f (x : N.t) = x end end
