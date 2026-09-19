module W = struct module F ( _ : Set.OrderedType ) ( Y : Set.OrderedType ) =
  struct type u = Set.Make( Y ).t end end
