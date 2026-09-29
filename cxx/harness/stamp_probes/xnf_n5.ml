module F ( X : Set.OrderedType ) = struct module N : Set.S = Set.Make( X ) type
  u = N.t end
