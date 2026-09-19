module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).t type v = Set.Make( X ).elt end end
