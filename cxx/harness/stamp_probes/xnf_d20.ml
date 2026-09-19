module W : sig module F ( X : Set.OrderedType ) : sig module N : Set.S with type
  elt = X.t end end = struct module F ( X : Set.OrderedType ) = struct module N
  = Set.Make( X ) end end
