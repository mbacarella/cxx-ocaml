module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).elt type v = Set.Make( X ).t end end
