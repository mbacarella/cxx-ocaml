module W = struct module F ( X : Set.OrderedType ) = struct module N = Set.Make(
  X ) end module G ( X : Set.OrderedType ) = struct module N = Set.Make( X ) end
  end
