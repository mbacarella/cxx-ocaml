module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).t end end module Z : sig module F ( X : Set.OrderedType ) : sig type u =
  Set.Make( X ).t end end = W
