module F ( X : Set.OrderedType ) = struct module N = Set.Make( X ) end module Z
  = struct module G ( Y : Set.OrderedType ) = struct module N = Set.Make( Y )
  end end
