module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).t constraint u = u end end
