module W = struct module F ( X : Set.OrderedType ) = struct module G ( Y :
  Set.OrderedType ) = struct type u = Set.Make( Y ).t end end end
