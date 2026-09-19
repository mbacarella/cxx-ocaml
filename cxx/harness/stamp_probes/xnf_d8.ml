module W = struct module F ( X : Set.OrderedType ) = struct module N : sig type
  t end = Set.Make( X ) end end
