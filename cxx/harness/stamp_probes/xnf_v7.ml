module W = struct module F ( X : Set.OrderedType ) ( Y : Set.OrderedType ) =
  struct type u = Set.Make( X ).t type v = Set.Make( Y ).t end end
