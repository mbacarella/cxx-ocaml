module W = struct module F ( X : Set.OrderedType ) = struct module G ( Y :
  Set.OrderedType ) = struct include Set.Make( Y ) end end end
