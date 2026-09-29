module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).t module N = Set.Make( X ) type v = N.t end end
