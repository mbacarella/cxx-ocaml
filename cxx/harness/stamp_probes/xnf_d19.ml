module W = struct module F ( X : Set.OrderedType ) ( Y : Set.OrderedType ) =
  struct module N = Set.Make( X ) end end
