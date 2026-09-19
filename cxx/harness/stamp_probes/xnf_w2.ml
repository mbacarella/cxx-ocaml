module G ( X : Set.OrderedType ) = struct type u = Set.Make( X ).t end module W
  = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X ).t
  end end
