module W = struct module F ( X : Set.OrderedType ) = struct module N = Set.Make(
  X ) end module G ( Y : Set.OrderedType ) = struct module N = Set.Make( Y ) end
  end
