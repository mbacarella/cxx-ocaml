module F ( X : Set.OrderedType ) = struct module V = struct type u = Set.Make( X
  ).t end end
