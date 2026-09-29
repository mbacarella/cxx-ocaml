module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).t type v = Map.Make( X ).key type w = int Map.Make( X ).t end end
