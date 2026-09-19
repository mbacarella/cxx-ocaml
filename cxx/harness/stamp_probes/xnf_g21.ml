module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).t list type v = Set.Make( X ).t end end
