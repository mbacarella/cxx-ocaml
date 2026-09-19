module W : sig end = struct module F ( X : Set.OrderedType ) = struct type u =
  Set.Make( X ).t end end
