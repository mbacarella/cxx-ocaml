module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).t module G ( Y : Set.OrderedType ) = struct type u = Set.Make( X ).t end end
  end
